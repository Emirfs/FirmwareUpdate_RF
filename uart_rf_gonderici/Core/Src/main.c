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
#include "sender_fw_update.h"
#include "sender_normal_mode.h"
#include "sender_state.h"
#include "sender_uart_debug.h"
#include "tim.h"
#include "usart.h"

/* USER CODE BEGIN Includes */
#include "rf_protocol.h"
#include "si4432.h"
/* USER CODE END Includes */

/* USER CODE BEGIN PV */
extern UART_HandleTypeDef huart1;
extern IWDG_HandleTypeDef hiwdg;

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);

/* USER CODE BEGIN 0 */
/* USER CODE END 0 */

int main(void) {
  /* HAL ve sistem saatini baslat */
  HAL_Init();
  SystemClock_Config(); // HSI x12 PLL = 48 MHz

  /* Cevresel birim (peripheral) baslatma */
  MX_GPIO_Init();       // LED, Si4432 CS/SDN/IRQ pinleri
  MX_DMA_Init();        // DMA kanallari (UART DMA icin)
  MX_USART1_UART_Init(); // 115200 8N1 — PC ile haberlesme
  MX_SPI2_Init();       // Si4432 RF modulu SPI baglantisi
  MX_IWDG_Init();       // Watchdog — kilitlenmeye karsi koruma
  MX_TIM17_Init();      // Timer (kullanilmiyor — ileride rezerve)
  MX_TIM16_Init();      // Timer (kullanilmiyor — ileride rezerve)

  /* Si4432 RF modul baslatma */
  SI4432_Init();
  HAL_Delay(10);

  /* Device Type Register (0x00) kontrol — 0x08 olmali
   * Bu deger Si4432 icin Silicon Labs'in belirtigi deger.
   * Diger deger: baglanti sorunu veya yanlis cihaz. */
  uint8_t dev = SI4432_ReadReg(0x00);
  if (dev != 0x08) {
    Print("\r\n[HATA] Si4432 bulunamadi!\r\n");
    /* Si4432 bulunamadi — LED hizli yanip sonsun, sonsuz dongu */
    while (1) {
      HAL_GPIO_TogglePin(LED0_GPIO_Port, LED0_Pin);
      HAL_Delay(100);
      HAL_IWDG_Refresh(&hiwdg);
    }
  }

  /* Basarili baslama gostergesi: her iki LED 1 sn yansın */
  HAL_GPIO_WritePin(LED0_GPIO_Port, LED0_Pin, GPIO_PIN_SET);
  HAL_GPIO_WritePin(LED1_GPIO_Port, LED1_Pin, GPIO_PIN_SET);
  HAL_Delay(1000);
  HAL_GPIO_WritePin(LED0_GPIO_Port, LED0_Pin, GPIO_PIN_RESET);
  HAL_GPIO_WritePin(LED1_GPIO_Port, LED1_Pin, GPIO_PIN_RESET);

  /* Terminal karsilama mesaji */
  Print("\r\n========================================\r\n");
  Print(" Si4432 RF Gonderici + FW Update Bridge\r\n");
  Print("========================================\r\n");
  Print("DEV_TYPE: ");
  PrintHex(dev); // 0x08 olmali
  Print("\r\nMesaj yaz + Enter (normal mod)\r\n");
  Print("'W' gondererek FW update modu baslatilir\r\n\r\n");

  /* Ana dongu */
  while (1) {
    HAL_IWDG_Refresh(&hiwdg); // Watchdog'u her donguде sifirla

    /* 50 ms bekleme ile bir UART karakteri al.
     * Timeout ile dongu bloke olmaz — watchdog calismaya devam eder. */
    uint8_t ch;
    if (HAL_UART_Receive(&huart1, &ch, 1, 50) == HAL_OK) {
      HandleNormalModeByte(ch); // sender_normal_mode.c — karakteri isle
      /* NOT: 'W' gelirse HandleNormalModeByte icinden FirmwareUpdate_Mode()
       * cagrılır; o fonksiyon donene kadar bu satir bekler. */
    }
  }
}

/*
 * SystemClock_Config — Sistem saatini yapilandir
 *
 * HSI (8 MHz dahili osilatör) → PLL x12 → SYSCLK = 48 MHz
 * HCLK = SYSCLK = 48 MHz, APB1 = HCLK / 1 = 48 MHz
 * LSI: IWDG icin kullanilir
 */
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
