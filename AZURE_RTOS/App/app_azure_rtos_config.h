
/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    app_azure_rtos_config.h
  * @author  MCD Application Team
  * @brief   app_azure_rtos config header file
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
#ifndef APP_AZURE_RTOS_CONFIG_H
#define APP_AZURE_RTOS_CONFIG_H
#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* Exported types ------------------------------------------------------------*/
/* USER CODE BEGIN ET */

/* USER CODE END ET */

/* Exported constants --------------------------------------------------------*/
/* Using static memory allocation via threadX Byte memory pools */

#define USE_STATIC_ALLOCATION                    1

#define TX_APP_MEM_POOL_SIZE                     1024

/* Everything MX_NetXDuo_Init() sub-allocates (via tx_byte_allocate) comes
 * out of this one fixed pool, in order: the packet pool
 * (NX_PACKET_POOL_SIZE, app_netxduo.h -- (1544 + sizeof(NX_PACKET)=56) * 32
 * = 51200 bytes on its own since the packet count was bumped 10->32 for
 * TLS), the IP instance (2*1024), ARP (1024), the main thread stack
 * (2*1024), the HTTP thread stack (2*1024) -- 58368 bytes minimum. The
 * original 51200 covered the old 10-packet pool (16000 bytes) plus those
 * four with room to spare; it does not cover the new one at all (leaves
 * literally 0 for everything allocated after the packet pool). Sized here
 * with ~23KB of headroom above the 58368 baseline. */
#define NX_APP_MEM_POOL_SIZE                     81920

/* USER CODE BEGIN EC */

/* USER CODE END EC */

/* Exported macro ------------------------------------------------------------*/
/* USER CODE BEGIN EM */

/* USER CODE END EM */

/* Exported functions prototypes ---------------------------------------------*/
/* USER CODE BEGIN EFP */

/* USER CODE END EFP */

/* Private defines -----------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

#ifdef __cplusplus
}
#endif

#endif /* APP_AZURE_RTOS_CONFIG_H */

