/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.h
  * @brief          : Header for main.c file.
  *                   This file contains the common defines of the application.
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __MAIN_H
#define __MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "stm32f0xx_hal.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* Exported types ------------------------------------------------------------*/
/* USER CODE BEGIN ET */

/* USER CODE END ET */

/* Exported constants --------------------------------------------------------*/
/* USER CODE BEGIN EC */

/* USER CODE END EC */

/* Exported macro ------------------------------------------------------------*/
/* USER CODE BEGIN EM */

/* USER CODE END EM */

/* Exported functions prototypes ---------------------------------------------*/
void Error_Handler(void);

/* USER CODE BEGIN EFP */

/* USER CODE END EFP */

/* Private defines -----------------------------------------------------------*/
#define BUTTON_Pin GPIO_PIN_13
#define BUTTON_GPIO_Port GPIOC
#define NEOPIXEL_BTN_Pin GPIO_PIN_0
#define NEOPIXEL_BTN_GPIO_Port GPIOB
#define SI4432_CCA1_Pin GPIO_PIN_10
#define SI4432_CCA1_GPIO_Port GPIOB
#define SI4432_CCA2_Pin GPIO_PIN_11
#define SI4432_CCA2_GPIO_Port GPIOB
#define SI4432_CE_Pin GPIO_PIN_12
#define SI4432_CE_GPIO_Port GPIOB
#define SI4432_CLK_Pin GPIO_PIN_13
#define SI4432_CLK_GPIO_Port GPIOB
#define SI4432_MISO_Pin GPIO_PIN_14
#define SI4432_MISO_GPIO_Port GPIOB
#define SI4432_MOSI_Pin GPIO_PIN_15
#define SI4432_MOSI_GPIO_Port GPIOB
#define SI4432_IRQ_Pin GPIO_PIN_8
#define SI4432_IRQ_GPIO_Port GPIOA
#define SI4432_IRQ_EXTI_IRQn EXTI4_15_IRQn
#define SI4432_SDN_Pin GPIO_PIN_11
#define SI4432_SDN_GPIO_Port GPIOA
#define DS18B20_Pin GPIO_PIN_15
#define DS18B20_GPIO_Port GPIOA
#define RLY1_Pin GPIO_PIN_3
#define RLY1_GPIO_Port GPIOB

/* USER CODE BEGIN Private defines */

/* USER CODE END Private defines */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */
