/* USER CODE BEGIN Header */
/**
 ******************************************************************************
 * @file           : main.c
 * @brief          : Alıcı Cihaz — RF Bootloader + Uygulama Yükleyici
 ******************************************************************************
 * STM32F030CC (256KB Flash, 32KB RAM)
 *
 * Flash Düzeni:
 *   0x08000000 – 0x08003FFF : Bootloader (16KB, 8 sayfa)
 *   0x08004000 – 0x0803F7FF : Uygulama   (238KB, 119 sayfa)
 *   0x0803F800 – 0x0803FFFF : Boot Flag  (2KB, 1 sayfa)
 *
 * Boot Kararı:
 *   - Boot flag sayfasında BOOT_FLAG_MAGIC + BOOT_FLAG_REQUEST varsa
 *     → Bootloader moduna gir (RF üzerinden firmware güncelleme)
 *   - Yoksa → Uygulamaya atla
 *   - Geçerli uygulama yoksa (MSP kontrolü) → Bootloader'da kal
 *
 * Güncelleme Akışı:
 *   1. CMD_BOOT_ACK gönder (gönderici hazır olana kadar)
 *   2. CMD_METADATA al → firmware boyutu, versiyon, CRC
 *   3. Flash sil → CMD_FLASH_ERASE_DONE gönder
 *   4. DATA_CHUNK paketlerini al (3 parça × ~50 byte = 148 byte)
 *   5. Her 148 byte → CRC-32 doğrula → AES-256 decrypt → Flash'a yaz
 *   6. Final CRC doğrulama → CMD_UPDATE_COMPLETE veya CMD_UPDATE_FAILED
 ******************************************************************************
 */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "gpio.h"
#include "iwdg.h"
#include "rtc.h"
#include "spi.h"
#include "tim.h"

/* USER CODE BEGIN Includes */
#include "aes.h"
#include "neopixel.h"
#include "rf_bootloader.h"
#include "si4432.h"
#include <string.h>
/* USER CODE END Includes */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN PV */

// Varsayılan AES key (Custom bootloader ile birebir aynı)
static const uint8_t DEFAULT_AES_KEY[32] = {
    0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x30, 0x31,
    0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x30, 0x31, 0x32,
    0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x30, 0x31, 0x32};

// Aktif AES key
static uint8_t AES_KEY[32];

// RF alma tamponu
static uint8_t rf_rx_buf[64];

// Firmware paketi birleştirme tamponu
// 4 parça × 48 byte = 192 byte max (148 byte kullanılacak)
static uint8_t fw_assembly_buf[200];
static uint8_t fw_chunks_received;

// Global sequence sayacı
static uint16_t rf_seq_counter = 0;

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);

/* USER CODE BEGIN PFP */
static void jump_to_application(void);
static uint8_t check_boot_flag(void);
static void set_boot_flag(void);
static void clear_boot_flag(void);
static void Bootloader_Main(void);
static void Flash_Erase_Application(void);
static void Flash_Write_Data(uint32_t addr, const uint8_t *data, uint32_t len);
static uint32_t Calculate_CRC32(const uint8_t *data, uint32_t length);
static uint32_t Calculate_Flash_CRC32(uint32_t start_addr, uint32_t length);
static void RF_SendPacket(uint8_t type, uint16_t seq, const uint8_t *payload,
                          uint8_t payload_len);
static uint8_t RF_WaitForPacket(uint8_t *type, uint16_t *seq, uint8_t *payload,
                                uint8_t *payload_len, uint32_t timeout_ms);
static void LED_Bootloader(void);
static void LED_Error(void);
static void LED_Success(void);
static void LED_Transfer(uint32_t packet_num);
static void LED_Off(void);
/* USER CODE END PFP */

/* USER CODE BEGIN 0 */

// =========================================================================
// CRC-32 HESAPLAMA (Software — zlib uyumlu, Custom bootloader ile aynı)
// =========================================================================
static uint32_t Calculate_CRC32(const uint8_t *data, uint32_t length) {
  uint32_t crc = 0xFFFFFFFF;
  for (uint32_t i = 0; i < length; i++) {
    crc ^= data[i];
    for (int j = 0; j < 8; j++) {
      if (crc & 1)
        crc = (crc >> 1) ^ 0xEDB88320;
      else
        crc >>= 1;
    }
  }
  return crc ^ 0xFFFFFFFF;
}

static uint32_t Calculate_Flash_CRC32(uint32_t start_addr, uint32_t length) {
  uint32_t crc = 0xFFFFFFFF;
  uint8_t *ptr = (uint8_t *)start_addr;
  for (uint32_t i = 0; i < length; i++) {
    crc ^= ptr[i];
    for (int j = 0; j < 8; j++) {
      if (crc & 1)
        crc = (crc >> 1) ^ 0xEDB88320;
      else
        crc >>= 1;
    }
    if (i % 4096 == 0)
      HAL_IWDG_Refresh(&hiwdg);
  }
  return crc ^ 0xFFFFFFFF;
}

// =========================================================================
// NeoPixel LED Durum Göstergeleri
// =========================================================================
static void LED_Bootloader(void) {
  // TURUNCU — bootloader modunda
  NeoPixel_SetAll(255, 80, 0);
  NeoPixel_Show();
}

static void LED_Error(void) {
  // KIRMIZI yanıp sönme
  for (int i = 0; i < 5; i++) {
    NeoPixel_SetAll(255, 0, 0);
    NeoPixel_Show();
    HAL_Delay(150);
    NeoPixel_Clear();
    NeoPixel_Show();
    HAL_Delay(150);
    HAL_IWDG_Refresh(&hiwdg);
  }
}

static void LED_Success(void) {
  // YEŞİL 3x blink
  for (int i = 0; i < 3; i++) {
    NeoPixel_SetAll(0, 255, 0);
    NeoPixel_Show();
    HAL_Delay(200);
    NeoPixel_Clear();
    NeoPixel_Show();
    HAL_Delay(200);
    HAL_IWDG_Refresh(&hiwdg);
  }
}

static void LED_Transfer(uint32_t packet_num) {
  // MAVİ-MOR geçişli — aktif transfer göstergesi
  if (packet_num % 2 == 0) {
    NeoPixel_SetAll(0, 0, 200);
  } else {
    NeoPixel_SetAll(128, 0, 200);
  }
  NeoPixel_Show();
}

static void LED_Off(void) {
  NeoPixel_Clear();
  NeoPixel_Show();
}

// =========================================================================
// BOOT FLAG YÖNETİMİ — Flash'ın son sayfasında
// =========================================================================
static uint8_t check_boot_flag(void) {
  volatile uint32_t *ptr = (volatile uint32_t *)BOOT_FLAG_ADDRESS;
  if (ptr[0] == BOOT_FLAG_MAGIC && ptr[1] == BOOT_FLAG_REQUEST) {
    return 1;
  }
  return 0;
}

static void set_boot_flag(void) {
  // Önce sayfayı sil
  HAL_FLASH_Unlock();

  FLASH_EraseInitTypeDef erase;
  erase.TypeErase = FLASH_TYPEERASE_PAGES;
  erase.PageAddress = BOOT_FLAG_ADDRESS;
  erase.NbPages = 1;
  uint32_t error;
  HAL_FLASHEx_Erase(&erase, &error);

  // Magic + Flag yaz (STM32F0: half-word programlama)
  // BOOT_FLAG_MAGIC = 0xB007B007
  HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD, BOOT_FLAG_ADDRESS,
                    (uint16_t)(BOOT_FLAG_MAGIC & 0xFFFF));
  HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD, BOOT_FLAG_ADDRESS + 2,
                    (uint16_t)((BOOT_FLAG_MAGIC >> 16) & 0xFFFF));

  // BOOT_FLAG_REQUEST = 0x00000001
  HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD, BOOT_FLAG_ADDRESS + 4,
                    (uint16_t)(BOOT_FLAG_REQUEST & 0xFFFF));
  HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD, BOOT_FLAG_ADDRESS + 6,
                    (uint16_t)((BOOT_FLAG_REQUEST >> 16) & 0xFFFF));

  HAL_FLASH_Lock();
}

static void clear_boot_flag(void) {
  // Sayfayı sil → tüm değerler 0xFF olur → flag yok
  HAL_FLASH_Unlock();

  FLASH_EraseInitTypeDef erase;
  erase.TypeErase = FLASH_TYPEERASE_PAGES;
  erase.PageAddress = BOOT_FLAG_ADDRESS;
  erase.NbPages = 1;
  uint32_t error;
  HAL_FLASHEx_Erase(&erase, &error);

  HAL_FLASH_Lock();
}

// =========================================================================
// FLASH İŞLEMLERİ — STM32F030 (Page-based, half-word programlama)
// =========================================================================
static void Flash_Erase_Application(void) {
  HAL_FLASH_Unlock();

  FLASH_EraseInitTypeDef erase;
  erase.TypeErase = FLASH_TYPEERASE_PAGES;
  erase.PageAddress = APP_ADDRESS;
  erase.NbPages = APP_PAGES;

  uint32_t error;

  // Sayfaları tek tek sil (watchdog'u besle)
  for (uint32_t i = 0; i < APP_PAGES; i++) {
    FLASH_EraseInitTypeDef single_erase;
    single_erase.TypeErase = FLASH_TYPEERASE_PAGES;
    single_erase.PageAddress = APP_ADDRESS + (i * FLASH_PAGE_SIZE);
    single_erase.NbPages = 1;

    HAL_FLASHEx_Erase(&single_erase, &error);
    HAL_IWDG_Refresh(&hiwdg);
  }

  HAL_FLASH_Lock();
}

static void Flash_Write_Data(uint32_t addr, const uint8_t *data, uint32_t len) {
  HAL_FLASH_Unlock();

  // STM32F030: half-word (16-bit) programlama
  for (uint32_t i = 0; i < len; i += 2) {
    uint16_t half_word;
    if (i + 1 < len) {
      half_word = (uint16_t)data[i] | ((uint16_t)data[i + 1] << 8);
    } else {
      // Son tek byte varsa, 0xFF ile doldur
      half_word = (uint16_t)data[i] | 0xFF00;
    }

    HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD, addr + i, half_word);
  }

  HAL_FLASH_Lock();
}

static uint8_t Flash_Verify_Data(uint32_t addr, const uint8_t *data,
                                 uint32_t len) {
  uint8_t *flash_ptr = (uint8_t *)addr;
  for (uint32_t i = 0; i < len; i++) {
    if (flash_ptr[i] != data[i])
      return 0; // Doğrulama başarısız
  }
  return 1; // Doğrulama başarılı
}

// =========================================================================
// VERSİYON YÖNETİMİ
// =========================================================================
static uint32_t Flash_Read_Version(void) {
  return *(volatile uint32_t *)VERSION_ADDRESS;
}

static void Flash_Write_Version(uint32_t version) {
  // Boot flag sayfası zaten temizlenmiş olmalı, versiyonu yaz
  HAL_FLASH_Unlock();
  HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD, VERSION_ADDRESS,
                    (uint16_t)(version & 0xFFFF));
  HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD, VERSION_ADDRESS + 2,
                    (uint16_t)((version >> 16) & 0xFFFF));
  HAL_FLASH_Lock();
}

// =========================================================================
// RF PAKET GÖNDERME/ALMA
// =========================================================================
static void RF_SendPacket(uint8_t type, uint16_t seq, const uint8_t *payload,
                          uint8_t payload_len) {
  uint8_t pkt[RF_MAX_PACKET_SIZE];
  uint8_t total_len = RF_HEADER_SIZE;

  pkt[0] = type;
  pkt[1] = (uint8_t)(seq >> 8);   // SEQ high
  pkt[2] = (uint8_t)(seq & 0xFF); // SEQ low

  if (payload && payload_len > 0) {
    if (payload_len > RF_MAX_PAYLOAD)
      payload_len = RF_MAX_PAYLOAD;
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
    if (len >= RF_HEADER_SIZE) {
      *type = rf_rx_buf[0];
      *seq = ((uint16_t)rf_rx_buf[1] << 8) | rf_rx_buf[2];
      *payload_len = len - RF_HEADER_SIZE;
      if (*payload_len > 0 && payload) {
        memcpy(payload, &rf_rx_buf[3], *payload_len);
      }
      return 1;
    }
  }

  return 0; // Timeout
}

/**
 * Güvenilir RF gönderim — ACK bekleme + retry
 * @return 1 = ACK alındı, 0 = başarısız
 */
static uint8_t RF_SendReliable(uint8_t type, uint16_t seq,
                               const uint8_t *payload, uint8_t payload_len) {
  uint8_t rx_type, rx_pld[RF_MAX_PAYLOAD];
  uint16_t rx_seq;
  uint8_t rx_pld_len;

  for (uint8_t attempt = 0; attempt < RF_MAX_RETRIES; attempt++) {
    RF_SendPacket(type, seq, payload, payload_len);

    // ACK bekle
    if (RF_WaitForPacket(&rx_type, &rx_seq, rx_pld, &rx_pld_len,
                         RF_ACK_TIMEOUT_MS)) {
      if (rx_type == RF_CMD_ACK && rx_seq == seq) {
        return 1; // Başarılı
      }
    }

    HAL_IWDG_Refresh(&hiwdg);
  }

  return 0; // Başarısız
}

// =========================================================================
// UYGULAMAYA ATLAMA
// =========================================================================
static void jump_to_application(void) {
  uint32_t app_msp = *(volatile uint32_t *)APP_ADDRESS;

  // MSP geçerlilik kontrolü — RAM aralığında olmalı
  if ((app_msp & 0xFFF00000) != 0x20000000) {
    // Geçerli uygulama yok → bootloader'da kal
    return;
  }

  uint32_t jump_addr = *(volatile uint32_t *)(APP_ADDRESS + 4);
  void (*app_reset_handler)(void) = (void (*)(void))jump_addr;

  // ─── STM32F030 Vector Table Remap ───
  // STM32F030 SCB->VTOR desteklemez!
  // Çözüm: Uygulamanın vector table'ını SRAM başına kopyala
  // ve SYSCFG ile SRAM'i 0x00000000 adresine remap et.
  // Bu sayede CPU interrupt olduğunda doğru handler'ları bulur.

  // 1. Uygulamanın vector table'ını SRAM başına kopyala (48 vektör × 4 byte =
  // 192 byte)
  //    STM32F030CC: 16 system exceptions + 32 peripheral interrupts = 48
  //    vectors
  volatile uint32_t *dst = (volatile uint32_t *)0x20000000;
  volatile uint32_t *src = (volatile uint32_t *)APP_ADDRESS;
  for (uint32_t i = 0; i < 48; i++) {
    dst[i] = src[i];
  }

  // 2. SYSCFG clock'u aç (remap için gerekli)
  __HAL_RCC_SYSCFG_CLK_ENABLE();

  // 3. SRAM'i 0x00000000 adresine remap et
  //    SYSCFG->CFGR1 MEM_MODE = 0b11 → Embedded SRAM
  SYSCFG->CFGR1 = (SYSCFG->CFGR1 & ~SYSCFG_CFGR1_MEM_MODE) |
                  (0x03 << SYSCFG_CFGR1_MEM_MODE_Pos);

  // Tüm interrupt'ları kapat
  __disable_irq();

  // HAL deinit
  HAL_RCC_DeInit();
  HAL_DeInit();

  // SysTick kapat
  SysTick->CTRL = 0;
  SysTick->LOAD = 0;
  SysTick->VAL = 0;

  // Tüm NVIC interrupt'larını temizle
  for (uint8_t i = 0; i < 8; i++) {
    NVIC->ICER[i] = 0xFFFFFFFF;
    NVIC->ICPR[i] = 0xFFFFFFFF;
  }

  // MSP ayarla ve atla
  __set_MSP(app_msp);
  app_reset_handler();

  // Buraya asla ulaşılmamalı
  while (1) {
  }
}

// =========================================================================
// ANA BOOTLOADER FONKSİYONU
// =========================================================================
static void Bootloader_Main(void) {
  struct AES_ctx aes_ctx;
  Firmware_Metadata_t metadata;
  uint32_t current_addr = APP_ADDRESS;
  uint32_t packets_received = 0;
  uint32_t total_packets = 0;

  // AES key'i yükle
  memcpy(AES_KEY, DEFAULT_AES_KEY, 32);

  // LED — bootloader modunda
  LED_Bootloader();

  // Si4432 başlat
  SI4432_Init();
  HAL_Delay(10);

  // SPI kontrol
  uint8_t dev = SI4432_ReadReg(0x00);
  if (dev != 0x08) {
    LED_Error();
    // Si4432 yok — bootloader'da kal ama bir şey yapılamaz
    while (1) {
      HAL_IWDG_Refresh(&hiwdg);
      LED_Error();
    }
  }

  // ─────────────────────────────────────────────
  // 1. BOOT_ACK GÖNDER — Gönderici hazır olana kadar
  // ─────────────────────────────────────────────
  {
    uint8_t rx_type, rx_pld[RF_MAX_PAYLOAD];
    uint16_t rx_seq;
    uint8_t rx_pld_len;
    uint8_t got_metadata = 0;

    while (!got_metadata) {
      HAL_IWDG_Refresh(&hiwdg);

      // BOOT_ACK gönder
      RF_SendPacket(RF_CMD_BOOT_ACK, rf_seq_counter++, NULL, 0);

      // TURUNCU blink — bekliyor
      NeoPixel_SetAll(255, 80, 0);
      NeoPixel_Show();

      // Cevap bekle (METADATA veya BOOT_REQUEST)
      if (RF_WaitForPacket(&rx_type, &rx_seq, rx_pld, &rx_pld_len, 1000)) {
        if (rx_type == RF_CMD_METADATA && rx_pld_len >= 12) {
          // Metadata alındı!
          memcpy(&metadata, rx_pld, sizeof(Firmware_Metadata_t));
          total_packets =
              (metadata.firmware_size + FW_PACKET_SIZE - 1) / FW_PACKET_SIZE;
          got_metadata = 1;

          // ACK gönder
          RF_SendPacket(RF_CMD_ACK, rx_seq, NULL, 0);
        }
        // Diğer paket tipleri yok sayılır, tekrar BOOT_ACK gönderilir
      }

      // LED toggle
      NeoPixel_Clear();
      NeoPixel_Show();
      HAL_Delay(200);
    }
  }

  // ─────────────────────────────────────────────
  // 2. FLASH SİL
  // ─────────────────────────────────────────────
  NeoPixel_SetAll(255, 255, 0); // SARI — flash siliniyor
  NeoPixel_Show();

  Flash_Erase_Application();

  // Flash silindi bildir
  {
    uint8_t sent = 0;
    for (uint8_t retry = 0; retry < 10 && !sent; retry++) {
      RF_SendPacket(RF_CMD_FLASH_ERASE_DONE, rf_seq_counter, NULL, 0);

      // ACK bekle
      uint8_t rx_type, rx_pld[RF_MAX_PAYLOAD];
      uint16_t rx_seq;
      uint8_t rx_pld_len;
      if (RF_WaitForPacket(&rx_type, &rx_seq, rx_pld, &rx_pld_len, 2000)) {
        if (rx_type == RF_CMD_ACK) {
          sent = 1;
        }
      }
      HAL_IWDG_Refresh(&hiwdg);
    }
    rf_seq_counter++;
  }

  // ─────────────────────────────────────────────
  // 3. FİRMWARE VERİ ALMA DÖNGÜSÜ
  // ─────────────────────────────────────────────
  fw_chunks_received = 0;
  memset(fw_assembly_buf, 0, sizeof(fw_assembly_buf));

  while (packets_received < total_packets) {
    HAL_IWDG_Refresh(&hiwdg);
    LED_Transfer(packets_received);

    uint8_t rx_type, rx_pld[RF_MAX_PAYLOAD];
    uint16_t rx_seq;
    uint8_t rx_pld_len;

    // DATA_CHUNK bekle
    if (!RF_WaitForPacket(&rx_type, &rx_seq, rx_pld, &rx_pld_len,
                          RF_UPDATE_TIMEOUT)) {
      // Timeout — güncelleme başarısız
      RF_SendPacket(RF_CMD_UPDATE_FAILED, rf_seq_counter++,
                    (uint8_t[]){RF_ERR_TIMEOUT}, 1);
      LED_Error();
      return;
    }

    if (rx_type != RF_CMD_DATA_CHUNK) {
      // Yanlış paket tipi — yok say
      continue;
    }

    // DATA_CHUNK payload: [CHUNK_IDX:1][CHUNK_CNT:1][DATA:N]
    if (rx_pld_len < 3) {
      // Geçersiz payload
      RF_SendPacket(RF_CMD_NACK, rx_seq, NULL, 0);
      continue;
    }

    uint8_t chunk_idx = rx_pld[0];
    uint8_t chunk_cnt = rx_pld[1];
    uint8_t data_len = rx_pld_len - 2;
    uint8_t *chunk_data = &rx_pld[2];

    // Chunk index doğrula
    if (chunk_idx != fw_chunks_received || chunk_cnt != RF_CHUNKS_PER_PACKET) {
      // Sıra dışı veya hatalı chunk — NACK
      RF_SendPacket(RF_CMD_NACK, rx_seq, NULL, 0);
      continue;
    }

    // Chunk'ı birleştirme tamponuna kopyala
    uint32_t offset = chunk_idx * RF_CHUNK_DATA_SIZE;
    if (offset + data_len > sizeof(fw_assembly_buf)) {
      RF_SendPacket(RF_CMD_NACK, rx_seq, NULL, 0);
      continue;
    }

    memcpy(&fw_assembly_buf[offset], chunk_data, data_len);
    fw_chunks_received++;

    // ACK gönder
    RF_SendPacket(RF_CMD_ACK, rx_seq, NULL, 0);

    // 4 chunk tamamlandı mı?
    if (fw_chunks_received >= RF_CHUNKS_PER_PACKET) {
      fw_chunks_received = 0;

      // ─── 148 byte tamam: IV(16) + Encrypted(128) + CRC32(4) ───
      uint8_t *iv_ptr = &fw_assembly_buf[0];
      uint8_t *encrypted_ptr = &fw_assembly_buf[16];
      uint32_t received_crc;
      memcpy(&received_crc, &fw_assembly_buf[144], 4);

      // CRC-32 doğrulaması (şifreli veri üzerinden)
      uint32_t computed_crc = Calculate_CRC32(encrypted_ptr, 128);

      if (computed_crc != received_crc) {
        // CRC hatası — bu paketler tekrar istenmeli
        RF_SendPacket(RF_CMD_UPDATE_FAILED, rf_seq_counter++,
                      (uint8_t[]){RF_ERR_CRC_FAIL}, 1);
        LED_Error();
        return;
      }

      // AES-256 CBC şifre çözme
      AES_init_ctx_iv(&aes_ctx, AES_KEY, iv_ptr);
      AES_CBC_decrypt_buffer(&aes_ctx, encrypted_ptr, 128);

      // İlk paket kontrolü — MSP doğrulama
      if (current_addr == APP_ADDRESS) {
        uint32_t msp_val = *(uint32_t *)encrypted_ptr;
        if ((msp_val & 0xFFF00000) != 0x20000000) {
          RF_SendPacket(RF_CMD_UPDATE_FAILED, rf_seq_counter++,
                        (uint8_t[]){RF_ERR_INVALID_MSP}, 1);
          LED_Error();
          return;
        }
      }

      // Flash'a yaz
      Flash_Write_Data(current_addr, encrypted_ptr, 128);

      // Read-back doğrulama
      if (!Flash_Verify_Data(current_addr, encrypted_ptr, 128)) {
        RF_SendPacket(RF_CMD_UPDATE_FAILED, rf_seq_counter++,
                      (uint8_t[]){RF_ERR_FLASH_VERIFY}, 1);
        LED_Error();
        return;
      }

      current_addr += 128;
      packets_received++;

      // Birleştirme tamponunu temizle
      memset(fw_assembly_buf, 0, sizeof(fw_assembly_buf));
    }
  }

  // ─────────────────────────────────────────────
  // 4. FİNAL CRC DOĞRULAMASI
  // ─────────────────────────────────────────────
  NeoPixel_SetAll(0, 200, 200); // CYAN — doğrulanıyor
  NeoPixel_Show();

  uint32_t flash_crc =
      Calculate_Flash_CRC32(APP_ADDRESS, metadata.firmware_size);

  if (flash_crc != metadata.firmware_crc32) {
    // CRC uyuşmazlığı — LED ile ayırt et (KIRMIZI+MAVİ)
    for (int i = 0; i < 5; i++) {
      NeoPixel_SetAll(255, 0, 128);
      NeoPixel_Show();
      HAL_Delay(150);
      NeoPixel_Clear();
      NeoPixel_Show();
      HAL_Delay(150);
      HAL_IWDG_Refresh(&hiwdg);
    }

    // Retry ile sonuç gönder — transmitter kaçırmasın
    for (uint8_t i = 0; i < 10; i++) {
      RF_SendPacket(RF_CMD_UPDATE_FAILED, rf_seq_counter,
                    (uint8_t[]){RF_ERR_FW_CRC_MISMATCH}, 1);

      uint8_t rx_type, rx_pld[RF_MAX_PAYLOAD];
      uint16_t rx_seq;
      uint8_t rx_pld_len;
      if (RF_WaitForPacket(&rx_type, &rx_seq, rx_pld, &rx_pld_len, 3000)) {
        if (rx_type == RF_CMD_ACK)
          break;
      }
      HAL_IWDG_Refresh(&hiwdg);
    }
    rf_seq_counter++;
    LED_Error();
    return;
  }

  // Boot flag temizle (versiyon yazılmadan ÖNCE sayfayı sil)
  clear_boot_flag();

  // Versiyon kaydet (temiz sayfaya yaz)
  Flash_Write_Version(metadata.firmware_version);

  // Başarı bildir — retry ile, ACK beklenerek
  for (uint8_t i = 0; i < 10; i++) {
    RF_SendPacket(RF_CMD_UPDATE_COMPLETE, rf_seq_counter, NULL, 0);

    uint8_t rx_type, rx_pld[RF_MAX_PAYLOAD];
    uint16_t rx_seq;
    uint8_t rx_pld_len;
    if (RF_WaitForPacket(&rx_type, &rx_seq, rx_pld, &rx_pld_len, 3000)) {
      if (rx_type == RF_CMD_ACK)
        break;
    }
    HAL_IWDG_Refresh(&hiwdg);
  }
  rf_seq_counter++;

  LED_Success();
  HAL_Delay(1000);

  // Uygulamaya atla
  jump_to_application();
}

/* USER CODE END 0 */

int main(void) {
  HAL_Init();
  SystemClock_Config();

  MX_GPIO_Init();
  MX_SPI2_Init();
  MX_IWDG_Init();
  MX_TIM17_Init();
  MX_TIM6_Init();
  MX_TIM16_Init();
  MX_TIM3_Init();
  MX_RTC_Init();
  /* USER CODE BEGIN 2 */

  NeoPixel_Init();

  // BEYAZ — MCU başladı
  NeoPixel_SetAll(100, 100, 100);
  NeoPixel_Show();
  HAL_Delay(300);
  NeoPixel_Clear();
  NeoPixel_Show();

  // Boot kararı
  if (check_boot_flag()) {
    // Boot flag set → doğrudan bootloader moduna gir
    Bootloader_Main();
    NVIC_SystemReset();
  }

  // Geçerli uygulama var mı kontrol et
  uint32_t app_msp = *(volatile uint32_t *)APP_ADDRESS;
  if ((app_msp & 0xFFF00000) == 0x20000000) {
    // Geçerli uygulama var — ama önce RF BOOT_REQUEST dinle (3 saniye)
    // Bu sayede transmitter uzaktan bootloader'ı tetikleyebilir
    SI4432_Init();
    HAL_Delay(10);

    uint8_t dev_check = SI4432_ReadReg(0x00);
    if (dev_check == 0x08) {
      // Si4432 hazır — kısa süre BOOT_REQUEST dinle
      NeoPixel_SetAll(0, 0, 100); // MAVİ — RF dinliyor
      NeoPixel_Show();

      uint8_t rx_type, rx_pld[RF_MAX_PAYLOAD];
      uint16_t rx_seq;
      uint8_t rx_pld_len;

      if (RF_WaitForPacket(&rx_type, &rx_seq, rx_pld, &rx_pld_len, 3000)) {
        if (rx_type == RF_CMD_BOOT_REQUEST) {
          // Transmitter bootloader istiyor → BOOT_ACK gönder ve bootloader'a
          // gir
          NeoPixel_SetAll(255, 128, 0); // TURUNCU — bootloader'a geçiyor
          NeoPixel_Show();
          Bootloader_Main();
          NVIC_SystemReset();
        }
      }
    }

    // BOOT_REQUEST gelmedi veya Si4432 yok → uygulamaya atla
    LED_Off();
    jump_to_application();
  }

  // Geçerli uygulama yok → bootloader'da kal
  Bootloader_Main();
  NVIC_SystemReset();

  /* USER CODE END 2 */

  while (1) {
    /* USER CODE BEGIN 3 */
    HAL_IWDG_Refresh(&hiwdg);
    /* USER CODE END 3 */
  }
}

void SystemClock_Config(void) {
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};
  RCC_PeriphCLKInitTypeDef PeriphClkInit = {0};

  RCC_OscInitStruct.OscillatorType =
      RCC_OSCILLATORTYPE_HSI | RCC_OSCILLATORTYPE_LSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.LSIState = RCC_LSI_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI;
  RCC_OscInitStruct.PLL.PLLMUL = RCC_PLL_MUL6;
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

  PeriphClkInit.PeriphClockSelection = RCC_PERIPHCLK_RTC;
  PeriphClkInit.RTCClockSelection = RCC_RTCCLKSOURCE_LSI;
  if (HAL_RCCEx_PeriphCLKConfig(&PeriphClkInit) != HAL_OK)
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
