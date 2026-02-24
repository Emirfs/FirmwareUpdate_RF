/* USER CODE BEGIN Header */
/**
 ******************************************************************************
 * @file           : main.c
 * @brief          : Alıcı — Si4432 RF Alım + NeoPixel + Echo
 ******************************************************************************
 * Resmi Silicon Labs örnek kodlarından alınan register konfigürasyonu
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
#include "neopixel.h"
#include "si4432.h"
#include <string.h>
/* USER CODE END Includes */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN PV */
static uint8_t rx_buf[64];
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);

/* USER CODE BEGIN 0 */
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
  HAL_Delay(500);
  NeoPixel_Clear();
  NeoPixel_Show();

  // Si4432 başlat
  SI4432_Init();
  HAL_Delay(10);

  // SPI kontrol
  uint8_t dev = SI4432_ReadReg(0x00);
  if (dev != 0x08) {
    while (1) {
      NeoPixel_SetAll(255, 0, 0);
      NeoPixel_Show();
      HAL_Delay(150);
      NeoPixel_Clear();
      NeoPixel_Show();
      HAL_Delay(150);
      HAL_IWDG_Refresh(&hiwdg);
    }
  }

  // YEŞİL 3x — hazır
  for (int i = 0; i < 3; i++) {
    NeoPixel_SetAll(0, 200, 0);
    NeoPixel_Show();
    HAL_Delay(200);
    NeoPixel_Clear();
    NeoPixel_Show();
    HAL_Delay(200);
    HAL_IWDG_Refresh(&hiwdg);
  }

  // Dinlemeye başla
  SI4432_StartRx();

  // MAVİ — dinleme modunda
  NeoPixel_SetAll(0, 0, 80);
  NeoPixel_Show();

  /* USER CODE END 2 */

  while (1) {
    /* USER CODE BEGIN 3 */
    HAL_IWDG_Refresh(&hiwdg);

    uint8_t len = SI4432_CheckRx(rx_buf);

    if (len > 0) {
      // MOR — paket alındı
      NeoPixel_SetAll(128, 0, 128);
      NeoPixel_Show();
      HAL_Delay(200);

      // SARI — echo gönderiyorum
      NeoPixel_SetAll(255, 200, 0);
      NeoPixel_Show();
      SI4432_SendPacket(rx_buf, len);
      HAL_Delay(200);

      // YEŞİL — tamam
      NeoPixel_SetAll(0, 200, 0);
      NeoPixel_Show();
      HAL_Delay(200);

      // MAVİ — tekrar dinleme
      SI4432_StartRx();
      NeoPixel_SetAll(0, 0, 80);
      NeoPixel_Show();
    }
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
