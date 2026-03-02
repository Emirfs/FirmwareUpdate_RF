/* USER CODE BEGIN Header */
/**
 ******************************************************************************
 * @file           : main.c
 * @brief          : UART→RF Gönderici — Firmware Update Bridge + Test Modu
 ******************************************************************************
 * STM32F030C8 (64KB Flash, 8KB RAM)
 *
 * İki çalışma modu:
 *   1. Normal Mod: UART'tan metin al → RF'den gönder → Echo bekle (test)
 *   2. Firmware Update Modu: PC'den 'W' gelince aktif
 *      PC ←UART→ Gönderici ←RF→ Alıcı Bootloader
 *
 * Firmware Update Akışı:
 *   PC→'W' → RF BOOT_REQUEST → Alıcıdan BOOT_ACK bekle → PC'ye ACK
 *   PC→metadata(12) → RF CMD_METADATA → ACK → PC'ye ACK
 *   Alıcı flash siler → RF FLASH_ERASE_DONE → PC'ye ACK
 *   PC→paket(148) → 3×RF DATA_CHUNK → ACK×3 → PC'ye ACK
 *   ... tekrar ...
 *   Alıcı final → RF UPDATE_COMPLETE/FAILED → PC'ye ACK/NACK
 ******************************************************************************
 */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "dma.h"
#include "gpio.h"
#include "iwdg.h"
#include "spi.h"
#include "tim.h"
#include "usart.h"

/* USER CODE BEGIN Includes */
#include "rf_protocol.h"
#include "si4432.h"
#include <string.h>
/* USER CODE END Includes */

/* USER CODE BEGIN PV */
extern UART_HandleTypeDef huart1;
extern IWDG_HandleTypeDef hiwdg;

// Normal mod tamponu
#define MAX_MSG 32
static uint8_t uart_buf[MAX_MSG];
static uint8_t uart_idx = 0;

// RF alma tamponu
static uint8_t rf_rx_buf[64];

// Firmware update tamponları
static uint8_t fw_packet_buf[FW_FULL_PACKET_SIZE]; // 148 byte

// Global RF sequence counter
static uint16_t rf_seq_counter = 0;
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);

/* USER CODE BEGIN 0 */

// =========================================================================
// UART Helper Fonksiyonları
// =========================================================================
static uint8_t fw_debug_en = 1;

static void Print(const char *s) {
  if (!fw_debug_en)
    return;
  HAL_UART_Transmit(&huart1, (uint8_t *)s, (uint16_t)strlen(s), 500);
}

static void PrintHex(uint8_t v) {
  if (!fw_debug_en)
    return;
  const char h[] = "0123456789ABCDEF";
  char o[3] = {h[(v >> 4) & 0xF], h[v & 0xF], ' '};
  HAL_UART_Transmit(&huart1, (uint8_t *)o, 3, 100);
}

static void PrintBuf(const uint8_t *b, uint16_t n) {
  if (!fw_debug_en)
    return;
  HAL_UART_Transmit(&huart1, (uint8_t *)b, n, 500);
}

// =========================================================================
// RF PAKET GÖNDERME/ALMA
// =========================================================================
static void RF_SendPacket(uint8_t type, uint16_t seq, const uint8_t *payload,
                          uint8_t payload_len) {
  uint8_t pkt[RF_MAX_PACKET];
  uint8_t total_len = RF_HEADER_LEN;

  pkt[0] = type;
  pkt[1] = (uint8_t)(seq >> 8);
  pkt[2] = (uint8_t)(seq & 0xFF);

  if (payload && payload_len > 0) {
    if (payload_len > RF_MAX_DATA)
      payload_len = RF_MAX_DATA;
    memcpy(&pkt[3], payload, payload_len);
    total_len += payload_len;
  }

  SI4432_SendPacket(pkt, total_len);
}

/**
 * RF paket bekle.
 * @return 1 = paket alındı, 0 = timeout
 */
static uint8_t RF_WaitForPacket(uint8_t *type, uint16_t *seq, uint8_t *payload,
                                uint8_t *payload_len, uint32_t timeout_ms) {
  uint32_t start = HAL_GetTick();

  SI4432_StartRx();

  while ((HAL_GetTick() - start) < timeout_ms) {
    HAL_IWDG_Refresh(&hiwdg);

    uint8_t len = SI4432_CheckRx(rf_rx_buf);
    if (len >= RF_HEADER_LEN) {
      *type = rf_rx_buf[0];
      *seq = ((uint16_t)rf_rx_buf[1] << 8) | rf_rx_buf[2];
      *payload_len = len - RF_HEADER_LEN;
      if (*payload_len > 0 && payload) {
        memcpy(payload, &rf_rx_buf[3], *payload_len);
      }
      return 1;
    }
  }

  return 0;
}

/**
 * Güvenilir tek RF chunk gönderimi — ACK dönene kadar retry
 * @return 1 = ACK alındı, 0 = başarısız
 */
static uint8_t RF_SendChunkReliable(uint8_t type, uint16_t seq,
                                    const uint8_t *payload,
                                    uint8_t payload_len) {
  uint8_t rx_type, rx_pld[RF_MAX_DATA];
  uint16_t rx_seq;
  uint8_t rx_pld_len;

  for (uint8_t attempt = 0; attempt < RF_MAX_RETRIES; attempt++) {
    RF_SendPacket(type, seq, payload, payload_len);

    if (RF_WaitForPacket(&rx_type, &rx_seq, rx_pld, &rx_pld_len,
                         RF_ACK_TIMEOUT_MS)) {
      if (rx_type == RF_CMD_ACK && rx_seq == seq) {
        return 1;
      }
      // NACK veya yanlış paket → tekrar dene
      if (rx_type == RF_CMD_NACK) {
        HAL_Delay(50); // Kısa bekleme
      }
    }
    HAL_IWDG_Refresh(&hiwdg);
  }

  return 0;
}

// =========================================================================
// FİRMWARE UPDATE MODU — UART↔RF Köprüsü
// =========================================================================
static void FirmwareUpdate_Mode(void) {
  uint8_t ack = UART_ACK;
  uint8_t nack = UART_NACK;
  uint8_t rx_type, rx_pld[RF_MAX_DATA];
  uint16_t rx_seq;
  uint8_t rx_pld_len;

  // UART loglarını kapat (binary iletişimi bozmamak için)
  fw_debug_en = 0;

  // LED0 yak — firmware update modunda
  HAL_GPIO_WritePin(LED0_GPIO_Port, LED0_Pin, GPIO_PIN_SET);

  // ─────────────────────────────────────────────
  // 1. BOOT REQUEST GÖNDER — Alıcıyı bootloader'a geçir
  // ─────────────────────────────────────────────
  Print("[FW] Aliciya BOOT_REQUEST gonderiliyor...\r\n");

  uint8_t boot_ack_received = 0;
  uint32_t boot_start = HAL_GetTick();

  while (!boot_ack_received && (HAL_GetTick() - boot_start) < 30000) {
    HAL_IWDG_Refresh(&hiwdg);

    // BOOT_REQUEST gönder
    RF_SendPacket(RF_CMD_BOOT_REQUEST, rf_seq_counter, NULL, 0);

    // BOOT_ACK bekle
    if (RF_WaitForPacket(&rx_type, &rx_seq, rx_pld, &rx_pld_len, 2000)) {
      if (rx_type == RF_CMD_BOOT_ACK) {
        boot_ack_received = 1;
        Print("[FW] Alici bootloader'a gecti!\r\n");
      }
    }

    // LED toggle
    HAL_GPIO_TogglePin(LED0_GPIO_Port, LED0_Pin);
  }

  if (!boot_ack_received) {
    Print("[FW] HATA: Alici bootloader'a gecilemedi!\r\n");
    HAL_UART_Transmit(&huart1, &nack, 1, 100);
    HAL_GPIO_WritePin(LED0_GPIO_Port, LED0_Pin, GPIO_PIN_RESET);
    return;
  }

  // PC'ye ACK — "alıcı hazır, metadata gönderebilirsin"
  HAL_UART_Transmit(&huart1, &ack, 1, 100);

  // ─────────────────────────────────────────────
  // 2. METADATA AL (UART'tan) ve RF ile gönder
  // ─────────────────────────────────────────────
  uint8_t meta_buf[12];
  if (HAL_UART_Receive(&huart1, meta_buf, 12, 10000) != HAL_OK) {
    Print("[FW] HATA: Metadata alinamadi!\r\n");
    HAL_UART_Transmit(&huart1, &nack, 1, 100);
    return;
  }

  Print("[FW] Metadata alindi, RF ile gonderiliyor...\r\n");

  // Metadata'yı RF ile gönder — güvenilir
  uint8_t meta_sent = 0;
  for (uint8_t retry = 0; retry < RF_MAX_RETRIES && !meta_sent; retry++) {
    RF_SendPacket(RF_CMD_METADATA, rf_seq_counter, meta_buf, 12);

    if (RF_WaitForPacket(&rx_type, &rx_seq, rx_pld, &rx_pld_len, 3000)) {
      if (rx_type == RF_CMD_ACK) {
        meta_sent = 1;
      }
    }
    HAL_IWDG_Refresh(&hiwdg);
  }
  rf_seq_counter++;

  if (!meta_sent) {
    Print("[FW] HATA: Metadata gonderilemedi!\r\n");
    HAL_UART_Transmit(&huart1, &nack, 1, 100);
    return;
  }

  // PC'ye ACK — "metadata kabul edildi"
  HAL_UART_Transmit(&huart1, &ack, 1, 100);

  // ─────────────────────────────────────────────
  // 3. FLASH SİLME BİLDİRİMİ BEKLE (RF'den)
  // ─────────────────────────────────────────────
  Print("[FW] Flash silme bekleniyor...\r\n");

  uint8_t erase_done = 0;
  uint32_t erase_start = HAL_GetTick();

  while (!erase_done && (HAL_GetTick() - erase_start) < 60000) {
    HAL_IWDG_Refresh(&hiwdg);

    if (RF_WaitForPacket(&rx_type, &rx_seq, rx_pld, &rx_pld_len, 5000)) {
      if (rx_type == RF_CMD_FLASH_ERASE_DONE) {
        erase_done = 1;
        // Alıcıya ACK gönder
        RF_SendPacket(RF_CMD_ACK, rx_seq, NULL, 0);
      }
    }
  }

  if (!erase_done) {
    Print("[FW] HATA: Flash silme zaman asimi!\r\n");
    HAL_UART_Transmit(&huart1, &nack, 1, 100);
    return;
  }

  Print("[FW] Flash silindi!\r\n");
  // PC'ye ACK — "flash silindi, paket gönderebilirsin"
  HAL_UART_Transmit(&huart1, &ack, 1, 100);

  // ─────────────────────────────────────────────
  // 4. FİRMWARE PAKET TRANSFERİ
  // ─────────────────────────────────────────────
  Print("[FW] Paket transferi basliyor...\r\n");

  uint32_t packets_sent = 0;

  while (1) {
    HAL_IWDG_Refresh(&hiwdg);

    // UART'tan 148 byte firmware paketi al
    // NOT: Timeout 8s — 4 RF chunk transfer süresi (her biri ~2s worst case)
    //       Son paketten sonra da 8s bekleyip RF dinlemeye geçer
    HAL_StatusTypeDef uart_status =
        HAL_UART_Receive(&huart1, fw_packet_buf, FW_FULL_PACKET_SIZE, 8000);

    if (uart_status == HAL_TIMEOUT) {
      // PC artık paket göndermedi — transfer bitti
      Print("[FW] Paket bekleme timeout — transfer tamamlandi?\r\n");
      break;
    }

    if (uart_status != HAL_OK) {
      Print("[FW] HATA: UART alma hatasi!\r\n");
      HAL_UART_Transmit(&huart1, &nack, 1, 100);
      return;
    }

    // 148 byte'ı 4 RF DATA_CHUNK'a böl
    //   Parça 0: fw_packet_buf[0..47]    → 48 byte data
    //   Parça 1: fw_packet_buf[48..95]   → 48 byte data
    //   Parça 2: fw_packet_buf[96..143]  → 48 byte data
    //   Parça 3: fw_packet_buf[144..147] →  4 byte data
    // Her chunk payload: [CHUNK_IDX:1][CHUNK_CNT:1][DATA:max 48]

    uint8_t all_chunks_ok = 1;

    for (uint8_t chunk = 0; chunk < RF_CHUNKS_PER_PACKET; chunk++) {
      uint8_t chunk_payload[RF_MAX_DATA];
      uint8_t data_offset = chunk * RF_CHUNK_DATA_SIZE;
      uint8_t data_len;

      if (chunk < RF_CHUNKS_PER_PACKET - 1) {
        data_len = RF_CHUNK_DATA_SIZE; // 48 byte
      } else {
        // Son chunk: kalan veri
        data_len = FW_FULL_PACKET_SIZE - data_offset; // 148 - 96 = 52
        if (data_len > RF_CHUNK_DATA_SIZE)
          data_len = RF_CHUNK_DATA_SIZE; // Güvenlik sınırı
      }

      chunk_payload[0] = chunk;                // CHUNK_IDX
      chunk_payload[1] = RF_CHUNKS_PER_PACKET; // CHUNK_CNT
      memcpy(&chunk_payload[2], &fw_packet_buf[data_offset], data_len);

      uint16_t chunk_seq = rf_seq_counter++;

      if (!RF_SendChunkReliable(RF_CMD_DATA_CHUNK, chunk_seq, chunk_payload,
                                data_len + 2)) {
        Print("[FW] HATA: Chunk gonderilemedi!\r\n");
        all_chunks_ok = 0;
        break;
      }
    }

    if (!all_chunks_ok) {
      HAL_UART_Transmit(&huart1, &nack, 1, 100);
      return;
    }

    // Tüm 4 chunk başarıyla gönderildi → PC'ye ACK
    packets_sent++;
    HAL_UART_Transmit(&huart1, &ack, 1, 100);

    // LED toggle — aktif transfer
    HAL_GPIO_TogglePin(LED1_GPIO_Port, LED1_Pin);

    // Her 10 pakette bir log
    if (packets_sent % 10 == 0) {
      Print("[FW] Paket: ");
      PrintHex((uint8_t)(packets_sent >> 8));
      PrintHex((uint8_t)(packets_sent & 0xFF));
      Print("\r\n");
    }
  }

  // ─────────────────────────────────────────────
  // 5. FİNAL SONUCU BEKLE (RF'den)
  // ─────────────────────────────────────────────
  Print("[FW] Final sonucu bekleniyor...\r\n");

  uint8_t final_received = 0;
  uint32_t final_start = HAL_GetTick();

  while (!final_received && (HAL_GetTick() - final_start) < 45000) {
    HAL_IWDG_Refresh(&hiwdg);

    if (RF_WaitForPacket(&rx_type, &rx_seq, rx_pld, &rx_pld_len, 5000)) {
      if (rx_type == RF_CMD_UPDATE_COMPLETE) {
        final_received = 1;
        Print("[FW] *** GUNCELLEME BASARILI! ***\r\n");
        // Alıcıya ACK — retry döngüsünü durdur
        RF_SendPacket(RF_CMD_ACK, rx_seq, NULL, 0);
        // PC'ye final ACK
        HAL_UART_Transmit(&huart1, &ack, 1, 100);

        // LED başarı göstergesi
        for (int i = 0; i < 5; i++) {
          HAL_GPIO_WritePin(LED0_GPIO_Port, LED0_Pin, GPIO_PIN_SET);
          HAL_GPIO_WritePin(LED1_GPIO_Port, LED1_Pin, GPIO_PIN_SET);
          HAL_Delay(200);
          HAL_GPIO_WritePin(LED0_GPIO_Port, LED0_Pin, GPIO_PIN_RESET);
          HAL_GPIO_WritePin(LED1_GPIO_Port, LED1_Pin, GPIO_PIN_RESET);
          HAL_Delay(200);
          HAL_IWDG_Refresh(&hiwdg);
        }
      } else if (rx_type == RF_CMD_UPDATE_FAILED) {
        final_received = 1;
        Print("[FW] HATA: Guncelleme basarisiz! Hata kodu: ");
        if (rx_pld_len > 0)
          PrintHex(rx_pld[0]);
        Print("\r\n");
        // Alıcıya ACK — retry döngüsünü durdur
        RF_SendPacket(RF_CMD_ACK, rx_seq, NULL, 0);
        // PC'ye NACK
        HAL_UART_Transmit(&huart1, &nack, 1, 100);

        // LED hata göstergesi
        for (int i = 0; i < 10; i++) {
          HAL_GPIO_WritePin(LED0_GPIO_Port, LED0_Pin, GPIO_PIN_SET);
          HAL_Delay(100);
          HAL_GPIO_WritePin(LED0_GPIO_Port, LED0_Pin, GPIO_PIN_RESET);
          HAL_Delay(100);
          HAL_IWDG_Refresh(&hiwdg);
        }
      }
    }
  }

  if (!final_received) {
    Print("[FW] Uyari: Final sonucu alinamadi (timeout)\r\n");
    // Timeout durumunda da NACK gönder ki PC takılmasın
    HAL_UART_Transmit(&huart1, &nack, 1, 100);
  }

  HAL_GPIO_WritePin(LED0_GPIO_Port, LED0_Pin, GPIO_PIN_RESET);
  HAL_GPIO_WritePin(LED1_GPIO_Port, LED1_Pin, GPIO_PIN_RESET);

  // Debug'ı tekrar aç
  fw_debug_en = 1;

  Print("[FW] Firmware update modu sona erdi\r\n");
}

/* USER CODE END 0 */

int main(void) {
  HAL_Init();
  SystemClock_Config();

  MX_GPIO_Init();
  MX_DMA_Init();
  MX_USART1_UART_Init();
  MX_SPI2_Init();
  MX_IWDG_Init();
  MX_TIM17_Init();
  MX_TIM16_Init();
  /* USER CODE BEGIN 2 */

  SI4432_Init();
  HAL_Delay(10);

  uint8_t dev = SI4432_ReadReg(0x00);
  if (dev != 0x08) {
    Print("\r\n[HATA] Si4432 bulunamadi!\r\n");
    while (1) {
      HAL_GPIO_TogglePin(LED0_GPIO_Port, LED0_Pin);
      HAL_Delay(100);
      HAL_IWDG_Refresh(&hiwdg);
    }
  }

  HAL_GPIO_WritePin(LED0_GPIO_Port, LED0_Pin, GPIO_PIN_SET);
  HAL_GPIO_WritePin(LED1_GPIO_Port, LED1_Pin, GPIO_PIN_SET);
  HAL_Delay(1000);
  HAL_GPIO_WritePin(LED0_GPIO_Port, LED0_Pin, GPIO_PIN_RESET);
  HAL_GPIO_WritePin(LED1_GPIO_Port, LED1_Pin, GPIO_PIN_RESET);

  Print("\r\n========================================\r\n");
  Print(" Si4432 RF Gonderici + FW Update Bridge\r\n");
  Print("========================================\r\n");
  Print("DEV_TYPE: ");
  PrintHex(dev);
  Print("\r\nMesaj yaz + Enter (normal mod)\r\n");
  Print("'W' gondererek FW update modu baslatilir\r\n\r\n");

  /* USER CODE END 2 */

  while (1) {
    /* USER CODE BEGIN 3 */
    HAL_IWDG_Refresh(&hiwdg);

    uint8_t ch;
    if (HAL_UART_Receive(&huart1, &ch, 1, 50) == HAL_OK) {

      // ─── Firmware Update Modu Tetikleme ───
      if (ch == 'W' || ch == 'w') {
        FirmwareUpdate_Mode();
        uart_idx = 0;
        memset(uart_buf, 0, MAX_MSG);
        continue;
      }

      // ─── Normal Mod: UART → RF Echo Test ───
      if (ch == '\r' || ch == '\n') {
        if (uart_idx > 0) {
          HAL_GPIO_WritePin(LED0_GPIO_Port, LED0_Pin, GPIO_PIN_SET);

          Print("\r\n[TX] Veri: ");
          PrintBuf(uart_buf, uart_idx);
          Print("\r\n[TX] Hex : ");
          for (uint8_t i = 0; i < uart_idx; i++)
            PrintHex(uart_buf[i]);
          Print("\r\n[TX] Gonderiliyor...\r\n");

          SI4432_SendPacket(uart_buf, uart_idx);

          Print("[TX] Gonderildi! Echo bekleniyor...\r\n");
          HAL_GPIO_WritePin(LED0_GPIO_Port, LED0_Pin, GPIO_PIN_RESET);

          uint8_t sent_len = uart_idx;
          uint8_t sent[MAX_MSG];
          memcpy(sent, uart_buf, uart_idx);
          uart_idx = 0;
          memset(uart_buf, 0, MAX_MSG);

          // Echo bekle
          SI4432_StartRx();
          HAL_GPIO_WritePin(LED1_GPIO_Port, LED1_Pin, GPIO_PIN_SET);

          uint8_t got_echo = 0;
          uint32_t t0 = HAL_GetTick();

          while ((HAL_GetTick() - t0) < 10000) {
            HAL_IWDG_Refresh(&hiwdg);

            uint8_t rx[64];
            uint8_t rx_len = SI4432_CheckRx(rx);

            if (rx_len > 0) {
              got_echo = 1;

              for (int b = 0; b < 3; b++) {
                HAL_GPIO_WritePin(LED0_GPIO_Port, LED0_Pin, GPIO_PIN_SET);
                HAL_GPIO_WritePin(LED1_GPIO_Port, LED1_Pin, GPIO_PIN_SET);
                HAL_Delay(100);
                HAL_GPIO_WritePin(LED0_GPIO_Port, LED0_Pin, GPIO_PIN_RESET);
                HAL_GPIO_WritePin(LED1_GPIO_Port, LED1_Pin, GPIO_PIN_RESET);
                HAL_Delay(100);
              }

              Print("\r\n[RX] Veri: ");
              PrintBuf(rx, rx_len);
              Print("\r\n[RX] Hex : ");
              for (uint8_t i = 0; i < rx_len; i++)
                PrintHex(rx[i]);
              Print("\r\n");

              if (rx_len == sent_len && memcmp(sent, rx, sent_len) == 0)
                Print("[SONUC] BASARILI!\r\n");
              else
                Print("[SONUC] FARKLI!\r\n");
              Print("---\r\n\r\n");
              break;
            }
          }

          HAL_GPIO_WritePin(LED1_GPIO_Port, LED1_Pin, GPIO_PIN_RESET);
          if (!got_echo) {
            for (int b = 0; b < 5; b++) {
              HAL_GPIO_WritePin(LED0_GPIO_Port, LED0_Pin, GPIO_PIN_SET);
              HAL_Delay(50);
              HAL_GPIO_WritePin(LED0_GPIO_Port, LED0_Pin, GPIO_PIN_RESET);
              HAL_Delay(50);
            }
            Print("[RX] TIMEOUT!\r\n---\r\n\r\n");
          }
        }
      } else {
        if (uart_idx < MAX_MSG) {
          uart_buf[uart_idx++] = ch;
          HAL_UART_Transmit(&huart1, &ch, 1, 10);
        }
      }
    }
    /* USER CODE END 3 */
  }
}

void SystemClock_Config(void) {
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  RCC_OscInitStruct.OscillatorType =
      RCC_OSCILLATORTYPE_HSI | RCC_OSCILLATORTYPE_LSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.LSIState = RCC_LSI_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI;
  RCC_OscInitStruct.PLL.PLLMUL = RCC_PLL_MUL12;
  RCC_OscInitStruct.PLL.PREDIV = RCC_PREDIV_DIV1;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
    Error_Handler();

  RCC_ClkInitStruct.ClockType =
      RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_PCLK1;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;
  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_1) != HAL_OK)
    Error_Handler();
}

void Error_Handler(void) {
  __disable_irq();
  while (1) {
  }
}

#ifdef USE_FULL_ASSERT
void assert_failed(uint8_t *file, uint32_t line) {}
#endif
