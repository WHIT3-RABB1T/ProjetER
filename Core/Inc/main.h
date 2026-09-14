/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.h
  * @brief          : Header for main.c file.
  *                   This file contains the common defines of the application.
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2021 STMicroelectronics.
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
#include "stm32u5xx_hal.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <stdio.h>
#include "stm32u5xx_hal_def.h"
/* USER CODE END Includes */

/* Exported types ------------------------------------------------------------*/
/* USER CODE BEGIN ET */
#define MXCHIP_SPI      hspi2
void mxchip_WIFI_ISR(uint16_t pin);
/* USER CODE END ET */

/* Exported constants --------------------------------------------------------*/
/* USER CODE BEGIN EC */
/* USER CODE END EC */

/* Exported macro ------------------------------------------------------------*/
/* USER CODE BEGIN EM */
extern void mxchip_WIFI_ISR(uint16_t pin);
extern void nx_driver_emw3080_interrupt(void);
/* USER CODE END EM */

/* Exported functions prototypes ---------------------------------------------*/
void Error_Handler(void);

/* USER CODE BEGIN EFP */
void Success_Handler(void);
/* USER CODE END EFP */

/* Private defines -----------------------------------------------------------*/
#define MXCHIP_FLOW_Pin GPIO_PIN_15
#define MXCHIP_FLOW_GPIO_Port GPIOG
#define MXCHIP_FLOW_EXTI_IRQn EXTI15_IRQn
#define LED_RED_Pin GPIO_PIN_6
#define LED_RED_GPIO_Port GPIOH
#define LED_GREEN_Pin GPIO_PIN_7
#define LED_GREEN_GPIO_Port GPIOH
#define MXCHIP_NOTIFY_Pin GPIO_PIN_14
#define MXCHIP_NOTIFY_GPIO_Port GPIOD
#define MXCHIP_NOTIFY_EXTI_IRQn EXTI14_IRQn
#define MXCHIP_NSS_Pin GPIO_PIN_12
#define MXCHIP_NSS_GPIO_Port GPIOB
#define MXCHIP_RESET_Pin GPIO_PIN_15
#define MXCHIP_RESET_GPIO_Port GPIOF

/* USER CODE BEGIN Private defines */
/* Blue "USER" push button, PC13 -- not part of the original .ioc pin
 * layout (GPIOC's clock isn't even enabled by the CubeMX-generated block
 * above), added by hand in MX_GPIO_Init() below alongside it. Confirmed
 * against ST's real BSP for this exact board
 * (github.com/STMicroelectronics/b-u585i-iot02a-bsp, b_u585i_iot02a.h/.c
 * -- BUTTON_USER_PIN/BUTTON_USER_GPIO_PORT, and BSP_PB_Init()'s
 * GPIO_PULLDOWN + BSP_PB_GetState()'s plain HAL_GPIO_ReadPin()): pulled
 * low at rest, driven high while held down. That BSP module itself isn't
 * vendored into this project (only the sensor/bus BSP files under
 * Drivers/BSP/B-U585I-IOT02A are -- see sensors.h), so this reads the pin
 * directly via HAL instead of through BSP_PB_GetState(). */
#define USER_BUTTON_Pin GPIO_PIN_13
#define USER_BUTTON_GPIO_Port GPIOC
/* USER CODE END Private defines */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */
