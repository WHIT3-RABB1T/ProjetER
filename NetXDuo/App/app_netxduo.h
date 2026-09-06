/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    app_netxduo.h
  * @author  MCD Application Team
  * @brief   NetXDuo applicative header file
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
#ifndef __APP_NETXDUO_H__
#define __APP_NETXDUO_H__

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "nx_api.h"

/* Private includes ----------------------------------------------------------*/

/* USER CODE BEGIN Includes */
#include <stdio.h>
#include "main.h"
#include "nxd_dhcp_client.h"
#include "nx_driver_emw3080.h"
#include "nx_web_http_client.h"
#include "nx_secure_tls_api.h"
#include "nx_secure_x509.h"
#include "https_ca_cert.h"
/* USER CODE END Includes */

/* Exported types ------------------------------------------------------------*/
/* USER CODE BEGIN ET */

/* USER CODE END ET */

/* Exported constants --------------------------------------------------------*/
/* USER CODE BEGIN EC */
#define PAYLOAD_SIZE             1544
/* Packet count bumped 10 -> 32: a TLS 1.2 handshake (ClientHello,
 * ServerHello, Certificate, ServerHelloDone, ClientKeyExchange,
 * ChangeCipherSpec, Finished) needs several packets in flight at once out
 * of this same pool, shared with TCP/IP's own internals and the HTTP
 * client -- 10 was plenty for plain UDP/HTTP but TLS exhausts it
 * (observed: nx_web_http_client_post_secure_start returning NX_NO_PACKET,
 * 0x01, mid-handshake). ~48KB total for the pool at 32 packets, well
 * within budget. */
#define NX_PACKET_POOL_SIZE      (( PAYLOAD_SIZE + sizeof(NX_PACKET)) * 32)
#define QUEUE_MAX_SIZE           512

#define DEFAULT_MEMORY_SIZE      1024
#define DEFAULT_MAIN_PRIORITY    10
#define DEFAULT_PRIORITY         5

/* AppMainThread/AppHTTPThread priority. Must be numerically GREATER than
 * (i.e. less urgent than) every mx_wifi driver thread priority --
 * NetXDuo/Target/mx_wifi_azure_rtos_conf.h maps OSPRIORITYREALTIME/
 * ABOVENORMAL/NORMAL to 8/9/11, and Core/Inc/mx_wifi_conf.h assigns those
 * to the driver's SPI (8) and RX/TX (9) threads -- otherwise this thread
 * can starve them. It used to be DEFAULT_PRIORITY (5, i.e. *more* urgent
 * than the driver), which was harmless while this thread only did quick
 * UDP/plain-HTTP I/O, but App_HTTP_Thread_Entry now runs the TLS handshake
 * (nx_secure) inline, including nx_crypto_rsa's modular exponentiation,
 * which has no tx_thread_sleep/relinquish anywhere in it -- a single long,
 * uninterrupted, CPU-bound stretch. At priority 5 that stretch denies the
 * CPU to the driver's priority-8 SPI thread for its entire duration, so if
 * it runs long enough (observed: "command 0x010e timeout(10000 ms) waiting
 * answer" -- MIPC_API_WIFI_BYPASS_OUT_CMD blowing MX_WIFI_CMD_TIMEOUT
 * because the SPI thread never got scheduled to process the module's
 * answer), the Wi-Fi link stalls out from under an in-progress TLS
 * connection. 15 is comfortably below (less urgent than) the driver's
 * lowest (11), so the driver always preempts this thread when it has I/O
 * to service, however long the crypto in this thread runs. */
#define APP_THREAD_PRIORITY      15

#define NULL_ADDRESS             0

#define DEFAULT_PORT             7000
#define UDP_SERVER_PORT          DEFAULT_PORT
#define UDP_CLIENT_PORT          6001
/* Broadcast: no need to know the listening PC's exact IP, just that it's on
 * the same Wi-Fi network. Match with: python3 tools/udp_listener.py --port 7000 */
#define UDP_SERVER_ADDRESS       IP_ADDRESS(255, 255, 255, 255)

#define MAX_PACKET_COUNT         100
#define DEFAULT_MESSAGE          "NetXDuo On STM32U5-IOT"

#define DEFAULT_TIMEOUT          10 * NX_IP_PERIODIC_RATE

/* Periodic HTTP GET client. Unlike the UDP broadcast above, TCP needs a real
 * unicast address — this must match the laptop's actual current LAN IP
 * (check with `python3 -c "import socket;s=socket.socket(socket.AF_INET,
 * socket.SOCK_DGRAM);s.connect(('8.8.8.8',80));print(s.getsockname()[0])"`)
 * on the same Wi-Fi network as the board. Run: python3 tools/http_server.py */
#define HTTP_SERVER_ADDRESS      IP_ADDRESS(10, 198, 244, 16)
#define HTTP_SERVER_HOST         "10.198.244.16"   /* string form of HTTP_SERVER_ADDRESS above, for the Host: header --
                                                       nx_web_http_client_get_start() rejects a NULL host with
                                                       NX_WEB_HTTP_ERROR (0x30000) before it even opens a connection */
#define HTTP_SERVER_PORT         8000    /* plaintext, unused now that App_HTTP_Thread_Entry posts over TLS */
#define HTTP_SERVER_HTTPS_PORT   8443    /* tools/http_server.py's HTTPS listener -- rerun tools/gen_https_cert.sh
                                             if this IP/host ever changes, so https_ca_cert.h's embedded trust
                                             anchor still matches the cert the server presents */
#define HTTP_RESOURCE            "/counter"
#define HTTP_POLL_PERIOD_SEC     2
/* USER CODE END EC */

/* Exported macro ------------------------------------------------------------*/
/* USER CODE BEGIN EM */
#define PRINT_IP_ADDRESS(addr)         do {                                         \
                                            printf("STM32 %s: %lu.%lu.%lu.%lu \n", #addr, \
                                            (addr >> 24) & 0xff,                    \
                                            (addr >> 16) & 0xff,                    \
                                            (addr >> 8) & 0xff,                     \
                                            (addr & 0xff));                         \
                                       } while(0)

#define PRINT_DATA(addr, port, data)   do {                                           \
                                            printf("[%lu.%lu.%lu.%lu:%u] -> '%s' \n", \
                                            (addr >> 24) & 0xff,                      \
                                            (addr >> 16) & 0xff,                      \
                                            (addr >> 8) & 0xff,                       \
                                            (addr & 0xff), port, data);               \
                                       } while(0)
/* USER CODE END EM */

/* Exported functions prototypes ---------------------------------------------*/
UINT MX_NetXDuo_Init(VOID *memory_ptr);

/* USER CODE BEGIN EFP */

/* USER CODE END EFP */

/* Private defines -----------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* USER CODE BEGIN 1 */

/* USER CODE END 1 */

#ifdef __cplusplus
}
#endif
#endif /* __APP_NETXDUO_H__ */
