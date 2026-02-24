/* USER CODE BEGIN Header */
/**
 ******************************************************************************
 * @file           : main.c
 * @brief          : UART→RF Gönderici — Si4432 Resmi Örnek Koddan
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
#include "si4432.h"
#include <string.h>
/* USER CODE END Includes */

/* USER CODE BEGIN PV */
extern UART_HandleTypeDef huart1;
extern IWDG_HandleTypeDef hiwdg;

#define MAX_MSG 32
static uint8_t uart_buf[MAX_MSG];
static uint8_t uart_idx = 0;
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);

/* USER CODE BEGIN 0 */
static void Print(const char *s) {
  HAL_UART_Transmit(&huart1, (uint8_t *)s, (uint16_t)strlen(s), 500);
}
static void PrintBuf(const uint8_t *b, uint16_t n) {
  HAL_UART_Transmit(&huart1, (uint8_t *)b, n, 500);
}
static void PrintHex(uint8_t v) {
  const char h[] = "0123456789ABCDEF";
  char o[3] = {h[(v >> 4) & 0xF], h[v & 0xF], ' '};
  HAL_UART_Transmit(&huart1, (uint8_t *)o, 3, 100);
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

  Print("\r\n===========================\r\n");
  Print(" Si4432 RF Gonderici\r\n");
  Print(" (Resmi SiLabs kodundan)\r\n");
  Print("===========================\r\n");
  Print("DEV_TYPE: ");
  PrintHex(dev);
  Print("\r\nSync: ");
  PrintHex(SI4432_ReadReg(0x36));
  PrintHex(SI4432_ReadReg(0x37));
  Print("\r\nMesaj yaz + Enter\r\n\r\n");

  /* USER CODE END 2 */

  while (1) {
    /* USER CODE BEGIN 3 */
    HAL_IWDG_Refresh(&hiwdg);

    uint8_t ch;
    if (HAL_UART_Receive(&huart1, &ch, 1, 50) == HAL_OK) {
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

              // LED0+LED1 birlikte 3x blink — echo geldi!
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
            // LED0 hızlı 5x blink — timeout
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