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

#define MAX_PACKET_COUNT         100
#define DEFAULT_MESSAGE          "NetXDuo On STM32U5-IOT"

#define DEFAULT_TIMEOUT          10 * NX_IP_PERIODIC_RATE

/* Server discovery -- see Discover_ServerIP() in app_netxduo.c.
 *
 * This replaces hardcoding the laptop's LAN IP, which is exactly what
 * kept going stale and breaking the TLS POST: mobile-hotspot Wi-Fi
 * (Android in particular) re-randomizes its whole subnet on each
 * activation, so an address hardcoded during one hotspot session
 * silently stops being the laptop once the hotspot restarts on a new
 * subnet -- the board still joins fine and gets its own new DHCP lease,
 * but the TCP SYN it sends to the old, now-nonexistent address just
 * times out with NX_NOT_CONNECTED (0x38), over and over, with nothing
 * ever reaching the Python server. Instead, the board broadcasts a short
 * UDP request on the local subnet (no unicast address needed at all --
 * 255.255.255.255 reaches every device on the same L2 segment
 * regardless of subnet) and tools/http_server.py's discovery listener
 * (same process, a background thread) replies -- the reply's own source
 * IP, not anything in its payload, is the answer, so this works no
 * matter what that machine's actual address is. Repurposes the port
 * number (7000) this project's much older, since-removed UDP-broadcast
 * experiment already used for the same kind of thing -- see
 * tools/udp_listener.py, now superseded by this. */
#define DISCOVERY_PORT               7000
#define DISCOVERY_BROADCAST_ADDR     IP_ADDRESS(255, 255, 255, 255)
#define DISCOVERY_REQUEST            "PROJETER_DISCOVER_SERVER_V1"
#define DISCOVERY_REQUEST_LEN        (sizeof(DISCOVERY_REQUEST) - 1)
#define DISCOVERY_REPLY              "PROJETER_SERVER_HERE_V1"
#define DISCOVERY_REPLY_LEN          (sizeof(DISCOVERY_REPLY) - 1)
#define DISCOVERY_RETRY_INTERVAL_SEC 2
#define DISCOVERY_MAX_ATTEMPTS       15   /* ~30s total before giving up and falling back to HTTP_SERVER_ADDRESS below */
/* Re-run discovery if this many *consecutive* poll rounds fail to even
 * get a TLS connection started -- covers the server moving to a new IP
 * again while the board is already running and happily connected,
 * not just at boot. A handshake that starts but stalls (the separate,
 * already-diagnosed NetX Secure issue the IWDG watchdog exists for)
 * doesn't count as this kind of failure -- see the wait_option comment
 * on nx_web_http_client_post_secure_start() in App_HTTP_Thread_Entry. */
#define DISCOVERY_RETRIGGER_FAILURES 5

/* Last-known-good fallback, used only if Discover_ServerIP() above
 * exhausts every retry with no reply at all (e.g. tools/http_server.py
 * genuinely isn't running yet, or a firewall is blocking the UDP reply
 * specifically) -- Discover_ServerIP() succeeding always overrides this
 * at runtime, so keeping this pointed at whatever the laptop's IP
 * happened to be during the last flash is harmless, not something that
 * needs to be kept up to date by hand any more. */
#define HTTP_SERVER_ADDRESS      IP_ADDRESS(10, 26, 76, 16)
#define HTTP_SERVER_HOST         "10.26.76.16"   /* string form of HTTP_SERVER_ADDRESS above, for the Host: header --
                                                       nx_web_http_client_get_start() rejects a NULL host with
                                                       NX_WEB_HTTP_ERROR (0x30000) before it even opens a connection */
#define HTTP_SERVER_PORT         8000    /* plaintext, unused now that App_HTTP_Thread_Entry posts over TLS */
#define HTTP_SERVER_HTTPS_PORT   8443    /* tools/http_server.py's HTTPS listener -- rerun tools/gen_https_cert.sh
                                             if this IP/host ever changes, so https_ca_cert.h's embedded trust
                                             anchor still matches the cert the server presents */
/* One combined JSON reading (Sensors_ReadAllJSON(), Core/Src/sensors.c) is
 * POSTed here per poll round -- was briefly split into one small object
 * per sensor category, each posted to its own resource, but that meant
 * one full TLS handshake per category per round (~500-650ms each); one
 * combined POST needs only a single handshake per round instead. */
#define HTTP_RESOURCE            "/sensors"
/* Sleep between poll rounds, in milliseconds (kept in ms, not whole
 * seconds, so it can be tuned finer than 1s increments). This is the gap
 * *after* the round's POST finishes, not a bound on the round itself.
 *
 * Used to be a much bigger deal than it looks: every round paid a full
 * ~500-650ms RSA-2048 TLS handshake regardless of this constant (see the
 * "Periodic HTTPS POST client" comment on App_HTTP_Thread_Entry below),
 * so tuning this only ever adjusted the gap on top of that fixed floor.
 * Now that the same connection is reused round after round (HTTP/1.1
 * keep-alive -- same comment), that floor is gone for every round except
 * the rare one that actually needs a fresh handshake: the real per-round
 * cost is just AES-128-CBC/HMAC-SHA256 over a small JSON body plus
 * however long the Wi-Fi round trip takes, both far under a millisecond
 * to low tens of milliseconds. 1ms (not 0): tx_thread_sleep(0) is a
 * documented no-op in ThreadX (_tx_thread_sleep() returns immediately
 * without suspending at all for a 0-tick request), so this is the
 * smallest value that still puts a real tick-boundary yield between
 * rounds rather than one round's tx_web_http_* calls running back-to-back
 * with literally nothing between them. */
#define HTTP_POLL_PERIOD_MS      1

/* The response-read loop in App_HTTP_Thread_Entry used to hand
 * nx_web_http_client_response_body_get() one flat 5-second wait_option
 * and just accept however long a dead connection took to time out --
 * if the server had already closed the connection (NetX Secure reports
 * this by dropping the TLS session back to NX_SECURE_TLS_CLIENT_STATE_IDLE,
 * see ConnectionIsIdle() in app_netxduo.c), nothing was ever coming, but
 * the call still blocked the full 5s before giving up with NX_NO_PACKET
 * and letting the round's own cleanup (nx_web_http_client_delete, next
 * round's fresh connect) run -- observed as the board "going idle" for a
 * flat 5s before it reconnected and worked again. Polling in
 * RESPONSE_POLL_TICKS slices instead, checking ConnectionIsIdle()
 * between them, means a truly dead connection gets noticed and the round
 * abandoned within one slice instead of always waiting out the old
 * budget blind; a connection that's merely slow, not dead, still gets
 * the same RESPONSE_TIMEOUT_TICKS (5s) total to respond as before. */
#define RESPONSE_POLL_TICKS      (NX_IP_PERIODIC_RATE / 4)
#define RESPONSE_TIMEOUT_TICKS   (5 * NX_IP_PERIODIC_RATE)
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
