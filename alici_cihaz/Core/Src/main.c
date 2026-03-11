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
#include "spi.h"

/* USER CODE BEGIN Includes */
#include "boot_flow.h"
#include "boot_led.h"
#include "boot_rf.h"
#include "boot_storage.h"
#include "neopixel.h"
#include "rf_bootloader.h"
#include "si4432.h"
/* USER CODE END Includes */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN PV */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);

/* USER CODE BEGIN PFP */
/* USER CODE END PFP */

/* USER CODE BEGIN 0 */
/* USER CODE END 0 */

int main(void) {
  /* HAL ve sistem saatini baslat */
  HAL_Init();
  SystemClock_Config(); // HSI x6 PLL = 24 MHz (alici daha dusuk hizda)

  /* Cevresel birim baslatma */
  MX_GPIO_Init();        // GPIO — Si4432 CS/SDN/IRQ, NeoPixel vb.
  MX_SPI2_Init();        // Si4432 SPI baglantisi
  MX_IWDG_Init();        // Watchdog — sonsuz dongu koruması

  /* NeoPixel LED baslatma */
  NeoPixel_Init();

  /* Beyaz kisa flas — MCU ayaga kalktı, diger kontroller baslamadan */
  NeoPixel_SetAll(100, 100, 100); // Beyaz
  NeoPixel_Show();
  HAL_Delay(300);
  NeoPixel_Clear();
  NeoPixel_Show();

  /* ===================================================================
   * BOOT KARARI
   *
   * Oncelik sirasi:
   *   1. Boot flag varsa → dogrudan bootloader (uzaktan tetikleme)
   *   2. Gecerli uygulama varsa → 3 sn RF dinle
   *      - BOOT_REQUEST gelirse → bootloader
   *      - Gelmezse → uygulamaya atla
   *   3. Gecerli uygulama yoksa → bootloader (ilk programlama)
   * =================================================================== */

  /* 1. Boot flag kontrolu */
  if (check_boot_flag()) {
    /* BOOT_FLAG_ADDRESS'de MAGIC + REQUEST var → bootloader moduna gec */
    Bootloader_Main();   // RF uzerinden firmware guncelle
    NVIC_SystemReset();  // Bittikten sonra sistemi yeniden basla
  }

  /* 2. Gecerli uygulama var mi? (MSP'nin 0x2000xxxx olmasi gerekiyor) */
  uint32_t app_msp = *(volatile uint32_t *)APP_ADDRESS;
  if ((app_msp & 0xFFF00000) == 0x20000000) {
    /* Gecerli uygulama var — ama once 3 saniye RF dinle */
    /* Bu sayede gonderici uzaktan bootloader'i tetikleyebilir */
    SI4432_Init();
    HAL_Delay(10);

    uint8_t dev_check = SI4432_ReadReg(0x00); // Device Type reg = 0x08 olmali
    if (dev_check == 0x08) {
      NeoPixel_SetAll(0, 0, 100); // Mavi — RF dinliyor
      NeoPixel_Show();

      uint8_t rx_type, rx_pld[RF_MAX_PAYLOAD];
      uint16_t rx_seq;
      uint8_t rx_pld_len;

      /* 3 saniye BOOT_REQUEST bekle */
      if (RF_WaitForPacket(&rx_type, &rx_seq, rx_pld, &rx_pld_len, 3000)) {
        if (rx_type == RF_CMD_BOOT_REQUEST || rx_type == RF_CMD_AUTH_REQUEST) {
          /* Gonderici bootloader istiyor (BOOT_REQUEST veya AUTH_REQUEST) → bootloader moduna gec */
          NeoPixel_SetAll(255, 128, 0); // Turuncu — bootloader'a geciliyor
          NeoPixel_Show();
          Bootloader_Main();
          NVIC_SystemReset();
        }
      }
      /* Baska tip paket geldi veya timeout → uygulamaya gec */
    }

    /* BOOT_REQUEST gelmedi veya Si4432 yok → uygulamaya atla */
    LED_Off();
    jump_to_application(); // boot_flow.c — MSP + SYSCFG remap + atla
  }

  /* 3. Gecerli uygulama yok → bootloader'da kal (ilk programlama) */
  Bootloader_Main();
  NVIC_SystemReset();

  /* Buraya hic ulasilmamali */
  while (1) {
    HAL_IWDG_Refresh(&hiwdg);
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
