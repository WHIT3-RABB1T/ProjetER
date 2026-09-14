/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    app_netxduo.c
  * @author  MCD Application Team
  * @brief   NetXDuo applicative file
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

/* Includes ------------------------------------------------------------------*/
#include "app_netxduo.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "app_azure_rtos.h"
#include "sensors.h"
#include <string.h>   /* memcmp -- Discover_ServerIP() below, comparing a received UDP reply against DISCOVERY_REPLY */
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */
static VOID App_Main_Thread_Entry(ULONG thread_input);
static VOID App_HTTP_Thread_Entry(ULONG thread_input);
static VOID ip_address_change_notify_callback(NX_IP *ip_instance, VOID *ptr);
static UINT tls_setup_callback(NX_WEB_HTTP_CLIENT *client_ptr, NX_SECURE_TLS_SESSION *tls_session);
static VOID print_pool_status(const CHAR *label);
static const CHAR *tls_client_state_name(UINT state);
static VOID diag_heartbeat_entry(ULONG id);
static VOID diag_stack_error_notify(TX_THREAD *thread_ptr);
static VOID watchdog_timer_entry(ULONG id);
static VOID FormatIPAddress(ULONG address, CHAR *out);
static UINT Discover_ServerIP(VOID);
static UINT ConnectionIsIdle(VOID);
/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN PV */
TX_THREAD AppMainThread;
TX_THREAD AppHTTPThread;

TX_SEMAPHORE Semaphore;

NX_PACKET_POOL AppPool;

/* Diagnostic heartbeat: nx_web_http_client_post_secure_start() is one long
 * blocking call, and the previous capture showed ~20+ silent seconds
 * inside it with zero application-level output -- we had no idea whether
 * that time was CPU-bound RSA math, a deadlock, or something draining the
 * packet pool, just a gap. A ThreadX timer runs in its own system-timer
 * thread context, independent of AppHTTPThread being blocked, so armed
 * right before the call and disarmed right after, it gives us a live
 * trace *during* that gap instead of a silent stretch bookended by two
 * printfs. */
static TX_TIMER DiagHeartbeatTimer;
static volatile ULONG DiagHeartbeatT0;
static volatile ULONG DiagHeartbeatTicks;

/* Watchdog safety net: real-hardware testing surfaced the NetX/TLS stack
 * occasionally wedging *indefinitely* mid-request -- observed stuck at
 * tls_client_state SERVERHELLO_DONE (mid-RSA, no forward progress), and
 * separately with TCP fully established but the TLS handshake never
 * progressing at all -- in both cases the blocking NetX call
 * (nx_web_http_client_post_secure_start()) simply never returned control
 * to AppHTTPThread, no matter how short its own wait_option was set.
 *
 * That second case traces to a real bug in ST's vendored NetX Secure:
 * _nx_secure_tls_handshake_process() (nx_secure_tls_handshake_process.c)
 * loops calling _nx_secure_tls_session_receive_records() with the FULL
 * wait_option on *every* iteration --
 *     while (state != HANDSHAKE_FINISHED)
 *         status = _nx_secure_tls_session_receive_records(session, &pkt, wait_option);
 * -- so wait_option bounds each individual receive, never the handshake
 * as a whole; and deeper still, _nx_secure_tls_session_start() takes a
 * process-wide mutex (_nx_secure_tls_protection) with a hardcoded
 * TX_WAIT_FOREVER. If that mutex is ever left held (an unpaired
 * get/put on some internal error path we don't have visibility into
 * without a compiler/debugger on real hardware), every future handshake
 * blocks on it forever, completely bypassing whatever wait_option this
 * file passes in -- which is exactly why shortening it to 8s upstream
 * had no effect on this particular failure mode. Not something fixable
 * from application code short of patching vendored middleware blind.
 *
 * A sibling of that same bug class, found later by reading a serial log
 * where the (now full-round, see the comment above App_HTTP_Thread_Entry)
 * heartbeat kept firing for ~10-12s straight with tcp_state=5
 * (NX_TCP_ESTABLISHED) and tcp_outstanding_bytes=0 the entire time --
 * i.e. genuinely nothing left to send or wait on at the TCP level, yet
 * whichever call was in progress (request_packet_allocate/put_packet,
 * both already given a short HTTP_SEND_TIMEOUT_TICKS -- see
 * app_netxduo.h) never returned anyway. Traced to
 * _nx_tcp_socket_send_internal() (nx_tcp_socket_send_internal.c):
 * several tx_mutex_get(&(ip_ptr -> nx_ip_protection), TX_WAIT_FOREVER)
 * calls, guarding the IP instance's shared state, all hardcoded the same
 * way -- so if that mutex is held elsewhere for a while (the Wi-Fi
 * driver's own SPI-level command processing has a documented ~10s
 * internal timeout, see APP_THREAD_PRIORITY's comment above on SPI
 * thread starvation, and separately mx_wifi_conf.h's MX_WIFI_CMD_TIMEOUT),
 * put_packet blocks for that whole stretch no matter what wait_option it
 * was given, same as the TLS case above. Checked every occurrence in one
 * real serial log (13 of them): all 13 ended in an IWDG reset, none
 * recovered on their own even given the full ~10-12s before the reset
 * hit -- so unlike the TLS case, there's no evidence this one would ever
 * resolve if simply given more time to wait; the reset isn't cutting off
 * a recovery that was about to happen.
 *
 * IWDG is the backstop that actually works regardless: WatchdogTimer
 * polls, every WATCHDOG_CHECK_TICKS, whether App_HTTP_Thread_Entry's poll
 * loop has made progress (WatchdogLastProgressTick, stamped at the top of
 * that loop -- see below) within the last WATCHDOG_STALE_TICKS, and only
 * refreshes the hardware watchdog while that's true. A genuine wedge
 * stops progress being stamped, refreshes stop, and the IWDG hardware
 * timeout resets the board on its own -- independent of ThreadX's
 * scheduler, immune to AppHTTPThread being stuck in a blocking library
 * call with no yield point (or, as here, stuck on a mutex that ignores
 * every wait_option passed to it). Armed only once Wi-Fi/DHCP/sensor
 * probing have already finished (right before App_HTTP_Thread_Entry's
 * main loop starts, see there) specifically so a slow-but-progressing
 * boot sequence can never itself trip a spurious reset.
 *
 * Retuned after reading a real serial.log covering 50 of these resets
 * back to back: 44 (88%) were this exact indefinite-hang bug, each one
 * costing ~10-12s of a visibly frozen board before the old, looser timing
 * (5s stale + 8s IWDG reload) finally reset it. Neither of those knobs
 * was actually load-bearing against a false trip -- every real round
 * observed completes in ~0.5-0.65s, nowhere near even a 3s budget -- so
 * they're tightened below to cut that outage roughly in half to two
 * thirds without adding any real risk of a spurious reset.
 *
 * The other 6 (12%) turned out to be a second, genuinely fixable bug of
 * this file's own: the consecutive-failure branch below calls
 * Discover_ServerIP() inline, which legitimately blocks up to
 * DISCOVERY_MAX_ATTEMPTS * DISCOVERY_RETRY_INTERVAL_SEC (~30s) waiting
 * for a reply -- and nothing stamped WatchdogLastProgressTick anywhere
 * inside it, so a slow-but-succeeding rediscovery (the log shows one
 * taking 7 attempts, ~14s, to find the server again after it changed IP)
 * raced the watchdog and could get killed by a full board reset instead
 * of the software-only recovery it was about to complete on its own.
 * Fixed by stamping progress once per attempt in that loop too -- see
 * Discover_ServerIP() below. */
static IWDG_HandleTypeDef   hiwdg;
static TX_TIMER              WatchdogTimer;
static volatile ULONG        WatchdogLastProgressTick;
/* LSI_VALUE (stm32u5xx_hal_conf.h) / IWDG_PRESCALER_256 = 32000/256 = 125 Hz
 * counter clock; 375 counts / 125 Hz = 3.0s hardware timeout. Worst case
 * (a stall starting right after a refresh, so the last real refresh was
 * almost a full WATCHDOG_CHECK_TICKS ago, plus the full IWDG reload on
 * top) that's ~1s + ~3s = ~4s from the moment something actually wedges
 * to the board resetting -- down from ~10-12s -- while every real round
 * (~0.5-0.65s) and every real Discover_ServerIP() attempt (2s, now fed to
 * the watchdog directly, see above) still clear it with room to spare. */
#define WATCHDOG_IWDG_RELOAD    375U
#define WATCHDOG_CHECK_TICKS    (1 * TX_TIMER_TICKS_PER_SECOND)
#define WATCHDOG_STALE_TICKS    (3 * TX_TIMER_TICKS_PER_SECOND)

ULONG IpAddress;
ULONG NetMask;
NX_IP IpInstance;

NX_DHCP DHCPClient;
NX_WEB_HTTP_CLIENT HttpClient;
CHAR *pointer;

/* TLS session setup (tls_setup_callback, below). The ciphersuite table
 * (nx_crypto_tls_ciphers, no ECC/AEAD -- see NetXDuo/App/nx_secure_user.h)
 * offers exactly TLS_RSA_WITH_AES_{128,256}_CBC_SHA256, TLS 1.2, static-RSA
 * key exchange: the server encrypts nothing with its private key except to
 * *decrypt* the client's RSA-encrypted pre-master secret -- about as
 * textbook a TLS handshake as it gets. */
extern const NX_SECURE_TLS_CRYPTO nx_crypto_tls_ciphers;

/* Sized with nx_secure_tls_session_metadata_size_calculate(&nx_crypto_tls_ciphers, ...)
 * against this exact ciphersuite table -- see ST's own Nx_MQTT_Client
 * reference project, which uses the identical table on the identical
 * target. Working scratch space for the crypto operations themselves
 * (RSA/AES/SHA), not certificate or handshake-message storage. */
static CHAR crypto_metadata_client[11600];

/* TLS record reassembly buffer -- big enough for our ~800-byte self-signed
 * leaf cert plus handshake overhead with headroom to spare. */
static UCHAR tls_packet_buffer[4000];

/* The trusted root: our self-signed cert (NetXDuo/App/https_ca_cert.h,
 * generated by tools/gen_https_cert.sh), added to the TLS session's
 * trusted store every connection in tls_setup_callback. Since it's
 * self-signed, this trusted copy and the leaf the server presents are
 * byte-for-byte the same certificate -- there's no separate CA above it. */
static NX_SECURE_X509_CERT trusted_certificate;

/* Scratch space the TLS session parses the server's presented certificate
 * chain into during the handshake (leaf + issuer -- even though our chain
 * is only one cert deep, nx_secure_tls_remote_certificate_allocate still
 * expects both slots set up). */
static NX_SECURE_X509_CERT remote_certificate;
static NX_SECURE_X509_CERT remote_issuer;
static UCHAR remote_cert_buffer[2000];
static UCHAR remote_issuer_buffer[2000];

/* Where the periodic POST actually goes -- set from HTTP_SERVER_ADDRESS/
 * HTTP_SERVER_HOST at first, then overwritten by Discover_ServerIP() as
 * soon as it hears back from a real server (see app_netxduo.h's
 * DISCOVERY_* comment block for why this replaces dialing the compile-
 * time constants directly). ServerHost needs 16 bytes for the longest
 * possible "255.255.255.255\0". */
static ULONG ServerAddress = HTTP_SERVER_ADDRESS;
static CHAR  ServerHost[16] = HTTP_SERVER_HOST;
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/**
  * @brief  Application NetXDuo Initialization.
  * @param memory_ptr: memory pointer
  * @retval int
  */
UINT MX_NetXDuo_Init(VOID *memory_ptr)
{
  UINT ret = NX_SUCCESS;
  TX_BYTE_POOL *byte_pool = (TX_BYTE_POOL*)memory_ptr;

   /* USER CODE BEGIN App_NetXDuo_MEM_POOL */

  /* USER CODE END App_NetXDuo_MEM_POOL */
  /* USER CODE BEGIN 0 */

  /* USER CODE END 0 */

  /* USER CODE BEGIN MX_NetXDuo_Init */
#if (USE_STATIC_ALLOCATION == 1)
  /* Unmistakable marker printed on every boot, first thing, so a serial
   * capture can be checked in one grep for "BUILD_MARKER" against
   * whatever this string currently is -- bump the tag every time this
   * file's instrumentation changes, so "is this actually the build I just
   * flashed" is never a judgment call again. */
  printf("=== BUILD_MARKER: diag-v9-tight-watchdog+discovery-progress ===\r\n");
  printf("Nx_UDP_Echo_Client_App started..\n");

  /* See tx_user.h (TX_ENABLE_STACK_CHECKING) and diag_stack_error_notify
   * above -- registered as early as possible, well before AppHTTPThread
   * ever starts (it's TX_DONT_START until App_Main_Thread_Entry resumes
   * it after DHCP), so any overflow in either app thread gets caught. */
  tx_thread_stack_error_notify(diag_stack_error_notify);

  /* Initialize the NetX system. */
  nx_system_initialize();
  
  /* Allocate the memory for packet_pool.  */
  if (tx_byte_allocate(byte_pool, (VOID **) &pointer,  NX_PACKET_POOL_SIZE, TX_NO_WAIT) != TX_SUCCESS)
  {
    return TX_POOL_ERROR;
  }  
  
  /* Create the Packet pool to be used for packet allocation */
  ret = nx_packet_pool_create(&AppPool, "Main Packet Pool", PAYLOAD_SIZE, pointer, NX_PACKET_POOL_SIZE);

  if (ret != NX_SUCCESS)
  {
    printf("nx_packet_pool_create failed: 0x%02X\r\n", ret);
    return NX_NOT_ENABLED;
  }
  printf("Packet pool created (%lu bytes, %u-byte packets)\r\n",
         (unsigned long)NX_PACKET_POOL_SIZE, (unsigned)PAYLOAD_SIZE);
  print_pool_status("at create");

  /* Allocate the memory for Ip_Instance */
  if (tx_byte_allocate(byte_pool, (VOID **) &pointer,   2 * DEFAULT_MEMORY_SIZE, TX_NO_WAIT) != TX_SUCCESS)
  {
    return TX_POOL_ERROR;
  }

  /* Create the main NX_IP instance -- this synchronously invokes the
   * mx_wifi driver's INITIALIZE command below (module reset/init, MAC
   * address read) and, right after, its ENABLE command (Wi-Fi join). With
   * MX_WIFI_*_DEBUG on (Core/Inc/mx_wifi_conf.h) and
   * NX_DEBUG_DRIVER_SOURCE_LOG routed to printf (nx_driver_emw3080.c /
   * mx_wifi_azure_rtos.c), this call is where "Joining ... <SSID>" and all
   * the module's own IPC/HCI/SLIP/IO chatter should start appearing. */
  printf("Creating IP instance (this drives Wi-Fi module init + join)...\r\n");
  ret = nx_ip_create(&IpInstance, "Main Ip instance", NULL_ADDRESS, NULL_ADDRESS, &AppPool, nx_driver_emw3080_entry,
                     pointer, 2 * DEFAULT_MEMORY_SIZE, DEFAULT_PRIORITY);

  if (ret != NX_SUCCESS)
  {
    printf("nx_ip_create failed: 0x%02X\r\n", ret);
    return NX_NOT_ENABLED;
  }
  printf("IP instance created\r\n");

  /* Allocate the memory for ARP */
  if (tx_byte_allocate(byte_pool, (VOID **) &pointer, DEFAULT_MEMORY_SIZE, TX_NO_WAIT) != TX_SUCCESS)
  {
    return TX_POOL_ERROR;
  }

  /*  Enable the ARP protocol and provide the ARP cache size for the IP instance */
  ret = nx_arp_enable(&IpInstance, (VOID *)pointer, DEFAULT_MEMORY_SIZE);

  if (ret != NX_SUCCESS)
  {
    printf("nx_arp_enable failed: 0x%02X\r\n", ret);
    return NX_NOT_ENABLED;
  }

  /* Enable the ICMP */
  ret = nx_icmp_enable(&IpInstance);

  if (ret != NX_SUCCESS)
  {
    printf("nx_icmp_enable failed: 0x%02X\r\n", ret);
    return NX_NOT_ENABLED;
  }

  /* Enable the UDP protocol required for  DHCP communication */
  ret = nx_udp_enable(&IpInstance);
  if (ret != NX_SUCCESS)
  {
    printf("nx_udp_enable failed: 0x%02X\r\n", ret);
    return NX_NOT_ENABLED;
  }

  /* Enable the TCP protocol, required by the HTTP client */
  ret = nx_tcp_enable(&IpInstance);

  if (ret != NX_SUCCESS)
  {
    printf("nx_tcp_enable failed: 0x%02X\r\n", ret);
    return NX_NOT_ENABLED;
  }
  printf("ARP/ICMP/UDP/TCP enabled\r\n");

  /* Allocate the memory for main thread   */
  if (tx_byte_allocate(byte_pool, (VOID **) &pointer,2 *  DEFAULT_MEMORY_SIZE, TX_NO_WAIT) != TX_SUCCESS)
  {
    return TX_POOL_ERROR;
  }  
  
  /* Create the main thread */
  ret = tx_thread_create(&AppMainThread, "App Main thread", App_Main_Thread_Entry, 0, pointer, 2 * DEFAULT_MEMORY_SIZE,
                         APP_THREAD_PRIORITY, APP_THREAD_PRIORITY, TX_NO_TIME_SLICE, TX_AUTO_START);

  if (ret != TX_SUCCESS)
  {
    printf("AppMainThread create failed: 0x%02X\r\n", ret);
    return NX_NOT_ENABLED;
  }

  /* Allocate the memory for HTTP client thread. Bumped 2 -> 8 *
   * DEFAULT_MEMORY_SIZE (2048 -> 8192 bytes): this thread runs the TLS
   * handshake through nx_web_http_client -> nx_secure_tls ->
   * nx_crypto_rsa, a deep call chain with sizable bignum locals for the
   * RSA modular exponentiation -- 2KB was sized for this thread's
   * original plain UDP/HTTP work, never revisited once TLS moved onto it.
   * TX_ENABLE_STACK_CHECKING (tx_user.h) + the notify handler below will
   * confirm whether 2KB was ever actually overflowing; 8KB is still
   * comfortably inside NX_APP_MEM_POOL_SIZE's budget alongside everything
   * else drawn from the same byte pool. */
  if (tx_byte_allocate(byte_pool, (VOID **) &pointer, 8 * DEFAULT_MEMORY_SIZE, TX_NO_WAIT) != TX_SUCCESS)
  {
    return TX_POOL_ERROR;
  }

  /* create the HTTP client thread */
  ret = tx_thread_create(&AppHTTPThread, "App HTTP Thread", App_HTTP_Thread_Entry, 0, pointer, 8 * DEFAULT_MEMORY_SIZE,
                         APP_THREAD_PRIORITY, APP_THREAD_PRIORITY, TX_NO_TIME_SLICE, TX_DONT_START);

  if (ret != TX_SUCCESS)
  {
    printf("AppHTTPThread create failed: 0x%02X\r\n", ret);
    return NX_NOT_ENABLED;
  }

  /* Diagnostic heartbeat timer -- created once here, TX_NO_ACTIVATE (idle
   * until App_HTTP_Thread_Entry arms it with tx_timer_change +
   * tx_timer_activate right before post_secure_start, then disarms it with
   * tx_timer_deactivate on whichever of that round's several exit paths
   * actually gets hit -- see the big comment right before that
   * tx_timer_activate call for why it now stays armed for the whole round,
   * not just this one call). initial/reschedule ticks here are
   * placeholders, always overwritten by tx_timer_change before use. */
  ret = tx_timer_create(&DiagHeartbeatTimer, "Diag Heartbeat", diag_heartbeat_entry, 0,
                        2 * TX_TIMER_TICKS_PER_SECOND, 2 * TX_TIMER_TICKS_PER_SECOND, TX_NO_ACTIVATE);
  if (ret != TX_SUCCESS)
  {
    printf("DiagHeartbeatTimer create failed: 0x%02X\r\n", ret);
    return NX_NOT_ENABLED;
  }

  /* create the DHCP client */
  ret = nx_dhcp_create(&DHCPClient, &IpInstance, "DHCP Client");

  if (ret != NX_SUCCESS)
  {
    printf("nx_dhcp_create failed: 0x%02X\r\n", ret);
    return NX_NOT_ENABLED;
  }

  /* set DHCP notification callback  */
  tx_semaphore_create(&Semaphore, "App Semaphore", 0);

  printf("MX_NetXDuo_Init done -- AppMainThread will now start DHCP\r\n");
#endif  
  /* USER CODE END MX_NetXDuo_Init */

  return ret;
}

/* USER CODE BEGIN 1 */
/**
* @brief  ip address change callback.
* @param ip_instance: NX_IP instance
* @param ptr: user data
* @retval none
*/
static VOID ip_address_change_notify_callback(NX_IP *ip_instance, VOID *ptr)
{
  /* release the semaphore as soon as an IP address is available */
  tx_semaphore_put(&Semaphore);
}

/* Diagnostic: dump AppPool's packet accounting. Added to directly test
 * whether the TLS handshake stall (32.7s to a plain NX_NO_PACKET) is real
 * packet-pool exhaustion or something else -- the code comment on
 * NX_PACKET_POOL_SIZE already documents this exact failure once before
 * ("observed: ... returning NX_NO_PACKET, 0x01, mid-handshake") and claims
 * bumping to 32 packets fixed it, but we're seeing 0x01 again, so either
 * 32 isn't actually enough for this handshake's real packet usage, or
 * something is holding packets that should have been released. free==0 (or
 * very low) at the failure point confirms it's the former; free still
 * comfortably above 0 would mean NX_NO_PACKET came from somewhere other
 * than true pool exhaustion (e.g. an internal wait-budget artifact). */

/* Fires from ThreadX's own scheduler when TX_ENABLE_STACK_CHECKING
 * (tx_user.h) detects a thread has overrun its stack -- registered via
 * tx_thread_stack_error_notify() in MX_NetXDuo_Init. If this ever prints,
 * it's conclusive: whatever's happening is a real stack overflow, not a
 * timeout or a protocol stall, and no amount of tuning wait_options fixes
 * that -- the stack (or whatever's allocating from it) needs to shrink,
 * or the thread's stack needs to grow further still. */
static VOID diag_stack_error_notify(TX_THREAD *thread_ptr)
{
    printf("\r\n!!! STACK OVERFLOW in thread \"%s\" !!!\r\n\r\n",
           thread_ptr->tx_thread_name ? thread_ptr->tx_thread_name : "?");
}

static VOID print_pool_status(const CHAR *label)
{
    ULONG total = 0, free = 0, empty_requests = 0, empty_suspensions = 0, invalid_releases = 0;

    if (nx_packet_pool_info_get(&AppPool, &total, &free, &empty_requests,
                                &empty_suspensions, &invalid_releases) == NX_SUCCESS)
    {
        printf("[pool %s] free=%lu/%lu, empty_requests=%lu, empty_suspensions=%lu, invalid_releases=%lu\r\n",
               label, free, total, empty_requests, empty_suspensions, invalid_releases);
    }
}

/* Human-readable form of NX_SECURE_TLS_SESSION's nx_secure_tls_client_state
 * (nx_secure_tls.h) -- this is the precise diagnostic we're missing so far:
 * TCP state alone can't tell SERVER_CERTIFICATE (received the cert, still
 * waiting on more from the server -- a protocol-level stall, our move
 * isn't next) apart from SERVERHELLO_DONE (server's done talking, we're
 * the one who's supposed to compute + send ClientKeyExchange next -- a
 * genuinely different failure mode, e.g. slow/stuck RSA). */
static const CHAR *tls_client_state_name(UINT state)
{
    switch (state)
    {
        case NX_SECURE_TLS_CLIENT_STATE_IDLE:                return "IDLE";
        case NX_SECURE_TLS_CLIENT_STATE_ERROR:                return "ERROR";
        case NX_SECURE_TLS_CLIENT_STATE_ALERT_SENT:           return "ALERT_SENT";
        case NX_SECURE_TLS_CLIENT_STATE_HELLO_REQUEST:        return "HELLO_REQUEST";
        case NX_SECURE_TLS_CLIENT_STATE_HELLO_VERIFY:         return "HELLO_VERIFY";
        case NX_SECURE_TLS_CLIENT_STATE_SERVERHELLO:          return "SERVERHELLO";
        case NX_SECURE_TLS_CLIENT_STATE_SERVER_CERTIFICATE:   return "SERVER_CERTIFICATE (waiting for more from server)";
        case NX_SECURE_TLS_CLIENT_STATE_SERVER_KEY_EXCHANGE:  return "SERVER_KEY_EXCHANGE";
        case NX_SECURE_TLS_CLIENT_STATE_CERTIFICATE_REQUEST:  return "CERTIFICATE_REQUEST";
        case NX_SECURE_TLS_CLIENT_STATE_SERVERHELLO_DONE:     return "SERVERHELLO_DONE (our turn: compute+send ClientKeyExchange)";
        case NX_SECURE_TLS_CLIENT_STATE_HANDSHAKE_FINISHED:   return "HANDSHAKE_FINISHED";
        case NX_SECURE_TLS_CLIENT_STATE_RENEGOTIATING:        return "RENEGOTIATING";
        case NX_SECURE_TLS_CLIENT_STATE_ENCRYPTED_EXTENSIONS: return "ENCRYPTED_EXTENSIONS";
        case NX_SECURE_TLS_CLIENT_STATE_HELLO_RETRY:          return "HELLO_RETRY";
        default:                                              return "UNKNOWN";
    }
}

/* True once the TLS session backing HttpClient has fallen back to
 * NX_SECURE_TLS_CLIENT_STATE_IDLE -- the state a session starts in, and
 * the state NetX Secure resets it to whenever the session ends (peer
 * closed the connection, a protocol alert, an internal error). Seeing
 * this *after* post_secure_start already got the handshake all the way
 * to HANDSHAKE_FINISHED means the connection this round was using has
 * died out from under us -- checked in the response-read loop below
 * between short polling slices, instead of only ever finding out the
 * hard way once a whole RESPONSE_TIMEOUT_TICKS wait_option times out. */
static UINT ConnectionIsIdle(VOID)
{
    return (HttpClient.nx_web_http_client_tls_session.nx_secure_tls_client_state
            == NX_SECURE_TLS_CLIENT_STATE_IDLE) ? NX_TRUE : NX_FALSE;
}

/* Fires every 2s (see tx_timer_change() calls around post_secure_start in
 * App_HTTP_Thread_Entry) for as long as that call is blocked, so the
 * previously-silent stretch inside it -- TCP connect, TLS handshake, the
 * RSA-heavy steps -- now prints a live trace instead of a gap. Runs in
 * ThreadX's own system-timer thread context, not AppHTTPThread, which is
 * exactly why it can report *while* that thread is stuck inside one
 * blocking NetX call: elapsed time so far, the packet pool's free count
 * (falling free would mean something's actively consuming/holding
 * packets during this stretch; flat would argue against a mid-handshake
 * leak), and the underlying TCP socket's state + unacked-byte count
 * (nonzero outstanding bytes would mean the transport is still waiting on
 * something over the air; zero means TCP itself is idle and whatever is
 * happening is purely on this end, e.g. RSA math). */
static VOID diag_heartbeat_entry(ULONG id)
{
    ULONG total = 0, free = 0, empty_requests = 0, empty_suspensions = 0, invalid_releases = 0;
    ULONG elapsed_ms;

    TX_PARAMETER_NOT_USED(id);

    DiagHeartbeatTicks++;
    elapsed_ms = (tx_time_get() - DiagHeartbeatT0) * 1000UL / TX_TIMER_TICKS_PER_SECOND;

    nx_packet_pool_info_get(&AppPool, &total, &free, &empty_requests, &empty_suspensions, &invalid_releases);

    printf("[heartbeat #%lu, t+%lu ms] pool free=%lu/%lu (empty_req=%lu, empty_susp=%lu) "
           "tcp_state=%u tcp_outstanding_bytes=%lu tls_client_state=%s\r\n",
           DiagHeartbeatTicks, elapsed_ms, free, total, empty_requests, empty_suspensions,
           (unsigned)HttpClient.nx_web_http_client_socket.nx_tcp_socket_state,
           HttpClient.nx_web_http_client_socket.nx_tcp_socket_tx_outstanding_bytes,
           tls_client_state_name(HttpClient.nx_web_http_client_tls_session.nx_secure_tls_client_state));
}

/* Fires every WATCHDOG_CHECK_TICKS, for the entire lifetime of the app
 * once armed -- see the watchdog comment above WatchdogLastProgressTick's
 * declaration. Runs in ThreadX's own system-timer thread context, so it
 * keeps polling even while AppHTTPThread itself is completely wedged
 * inside a blocking NetX call -- that's the whole point: it can only
 * refresh the hardware watchdog while genuine forward progress is still
 * being made. */
static VOID watchdog_timer_entry(ULONG id)
{
    TX_PARAMETER_NOT_USED(id);

    if ((tx_time_get() - WatchdogLastProgressTick) <= WATCHDOG_STALE_TICKS)
    {
        HAL_IWDG_Refresh(&hiwdg);
    }
    /* else: no progress in over WATCHDOG_STALE_TICKS -- stop refreshing
     * and let the ~3s IWDG hardware timeout reset the board. */
}

/**
* @brief  Main thread entry.
* @param thread_input: ULONG user argument used by the thread entry
* @retval none
*/
static VOID App_Main_Thread_Entry(ULONG thread_input)
{
  UINT ret;
  ULONG waited_sec = 0;

  /* register the IP address change callback */
  ret = nx_ip_address_change_notify(&IpInstance, ip_address_change_notify_callback, NULL);
  if (ret != NX_SUCCESS)
  {
    printf("nx_ip_address_change_notify failed: 0x%02X\r\n", ret);
    Error_Handler();
  }

  /* start the DHCP client */
  printf("Starting DHCP client...\r\n");
  ret = nx_dhcp_start(&DHCPClient);
  if (ret != NX_SUCCESS)
  {
    printf("nx_dhcp_start failed: 0x%02X\r\n", ret);
    Error_Handler();
  }

  /* Wait until an IP address is ready, polling every 5s instead of
   * blocking forever on one TX_WAIT_FOREVER -- previously, if Wi-Fi
   * join/DHCP ever stalled, this thread produced zero further output
   * ("app prints only its startup banner and nothing else", a symptom
   * seen before on the old project). This makes that same stall visible:
   * one line every 5s for as long as we're still waiting, rather than
   * silence indistinguishable from a hang. */
  while (tx_semaphore_get(&Semaphore, 5 * TX_TIMER_TICKS_PER_SECOND) != TX_SUCCESS)
  {
    waited_sec += 5;
    printf("Still waiting for DHCP lease (%lus so far) -- Wi-Fi join not done yet, "
           "or module not answering. Check the MX_WIFI_*_DEBUG / driver join logs above.\r\n",
           waited_sec);
  }
  printf("DHCP lease acquired after %lus\r\n", waited_sec);

  /* get IP address */
  ret = nx_ip_address_get(&IpInstance, &IpAddress, &NetMask);

  /* print the IP address and the net mask */
  PRINT_IP_ADDRESS(IpAddress);

  if (ret != TX_SUCCESS)
  {
    printf("nx_ip_address_get failed: 0x%02X\r\n", ret);
    Error_Handler();
  }
  /* the network is correctly initialized, start the HTTP thread */
  printf("Network up -- starting HTTPS POST client thread\r\n");
  tx_thread_resume(&AppHTTPThread);

  /* this thread is not needed any more, we relinquish it */
  tx_thread_relinquish();

  return;
}

/* Sets up one TLS session for the HTTP client -- passed as the tls_setup
 * callback to nx_web_http_client_post_secure_start(), which invokes it
 * itself right before the handshake starts. Called fresh on every
 * connection (App_HTTP_Thread_Entry creates/deletes the HTTP client each
 * poll), matching the lifecycle both ST's demo_netxduo_https.c and the
 * Nx_MQTT_Client reference project use.
 *
 * NOTE on scope: this validates the certificate *chain* the server
 * presents (its signature must trace back to trusted_certificate below --
 * here, be byte-identical to it, since it's self-signed) but does not
 * check the server's hostname/IP against the certificate's CN/SAN --
 * nx_web_http_client has no hook for that. So "validating the server"
 * here means "cryptographically verifying we got the exact cert we
 * trust," not "matches the URL we asked for" -- a real, common
 * simplification in embedded TLS clients, not a corner cut silently. */
static UINT tls_setup_callback(NX_WEB_HTTP_CLIENT *client_ptr, NX_SECURE_TLS_SESSION *tls_session)
{
    UINT ret;

    NX_PARAMETER_NOT_USED(client_ptr);

    ret = nx_secure_tls_session_create(tls_session, &nx_crypto_tls_ciphers,
                                       crypto_metadata_client, sizeof(crypto_metadata_client));
    if (ret != NX_SUCCESS)
    {
        printf("TLS session create failed: 0x%02X\r\n", ret);
        return(ret);
    }

    ret = nx_secure_tls_session_packet_buffer_set(tls_session, tls_packet_buffer,
                                                  sizeof(tls_packet_buffer));
    if (ret != NX_SUCCESS)
    {
        printf("TLS packet buffer set failed: 0x%02X\r\n", ret);
        return(ret);
    }

    /* Trust anchor: our self-signed cert, embedded at build time
     * (https_ca_cert.h, generated by tools/gen_https_cert.sh). No private
     * key here -- we're only ever verifying a signature with it, never
     * signing anything, so NX_SECURE_X509_KEY_TYPE_NONE. */
    ret = nx_secure_x509_certificate_initialize(&trusted_certificate,
                                                (UCHAR *)https_ca_cert_der, https_ca_cert_der_len,
                                                NX_NULL, 0, NULL, 0, NX_SECURE_X509_KEY_TYPE_NONE);
    if (ret != NX_SUCCESS)
    {
        printf("Trusted certificate init failed: 0x%02X\r\n", ret);
        return(ret);
    }

    ret = nx_secure_tls_trusted_certificate_add(tls_session, &trusted_certificate);
    if (ret != NX_SUCCESS)
    {
        printf("Trusted certificate add failed: 0x%02X\r\n", ret);
        return(ret);
    }

    /* Scratch space for the certificate(s) the server actually sends
     * during the handshake, which TLS parses into these before running
     * the check against trusted_certificate above. */
    ret = nx_secure_tls_remote_certificate_allocate(tls_session, &remote_certificate,
                                                    remote_cert_buffer, sizeof(remote_cert_buffer));
    if (ret != NX_SUCCESS)
    {
        printf("Remote certificate allocate failed: 0x%02X\r\n", ret);
        return(ret);
    }

    ret = nx_secure_tls_remote_certificate_allocate(tls_session, &remote_issuer,
                                                    remote_issuer_buffer, sizeof(remote_issuer_buffer));
    if (ret != NX_SUCCESS)
    {
        printf("Remote issuer certificate allocate failed: 0x%02X\r\n", ret);
        return(ret);
    }

    printf("TLS session set up ok, handshake starting...\r\n");
    print_pool_status("handshake starting");
    return(NX_SUCCESS);
}

/* Renders a big-endian ULONG IPv4 address as "a.b.c.d\0" into out, which
 * must be at least 16 bytes -- the same byte layout PRINT_IP_ADDRESS
 * above already assumes, just captured into ServerHost instead of
 * printed. */
static VOID FormatIPAddress(ULONG address, CHAR *out)
{
    sprintf(out, "%u.%u.%u.%u",
           (unsigned)((address >> 24) & 0xFF),
           (unsigned)((address >> 16) & 0xFF),
           (unsigned)((address >> 8) & 0xFF),
           (unsigned)(address & 0xFF));
}

/* Broadcast-discover the Python server's current IP -- see the DISCOVERY_*
 * comment block in app_netxduo.h for the whole story of why this exists
 * (mobile-hotspot subnets changing out from under a hardcoded address).
 *
 * Opens its own short-lived UDP socket bound to DISCOVERY_PORT, sends
 * DISCOVERY_REQUEST to the local broadcast address, and waits up to
 * DISCOVERY_RETRY_INTERVAL_SEC for a DISCOVERY_REPLY back -- repeating up
 * to DISCOVERY_MAX_ATTEMPTS times (~30s total) before giving up. On a
 * matching reply, ServerAddress/ServerHost are updated from the *packet's
 * own source address* (nx_udp_source_extract), never from anything in its
 * payload -- so this is correct regardless of what the server process
 * itself thinks its address is. Returns NX_SUCCESS if a server answered,
 * or NX_NOT_SUCCESSFUL if every attempt was exhausted with no reply, in
 * which case ServerAddress/ServerHost are left exactly as they were
 * (whatever the caller initialized them to, i.e. the HTTP_SERVER_ADDRESS/
 * HTTP_SERVER_HOST fallback on first boot, or the last good discovered
 * address on a re-discovery triggered mid-session). */
static UINT Discover_ServerIP(VOID)
{
    UINT          ret;
    NX_UDP_SOCKET discovery_socket;
    NX_PACKET    *send_packet;
    NX_PACKET    *receive_packet;
    UINT          attempt;
    ULONG         reply_address;
    UINT          reply_port;
    UCHAR         reply_buffer[64];
    ULONG         bytes;
    UINT          found = NX_FALSE;

    ret = nx_udp_socket_create(&IpInstance, &discovery_socket, "Discovery Socket",
                               NX_IP_NORMAL, NX_FRAGMENT_OKAY, 0x80, 4);
    if (ret != NX_SUCCESS)
    {
        printf("Discovery: socket create failed: 0x%02X -- using fallback address\r\n", ret);
        return ret;
    }

    /* Bound to DISCOVERY_PORT, not NX_ANY_PORT: the server's reply is a
     * unicast send back to whatever source port our broadcast used, so
     * this has to be a fixed, known port for it to reach -- reusing the
     * same number the responder listens on for requests is fine, since
     * UDP ports are independent per direction. */
    ret = nx_udp_socket_bind(&discovery_socket, DISCOVERY_PORT, NX_NO_WAIT);
    if (ret != NX_SUCCESS)
    {
        printf("Discovery: socket bind failed: 0x%02X -- using fallback address\r\n", ret);
        nx_udp_socket_delete(&discovery_socket);
        return ret;
    }

    printf("Discovery: broadcasting '%s' on port %d (up to %d attempts, %ds apart)...\r\n",
           DISCOVERY_REQUEST, DISCOVERY_PORT, DISCOVERY_MAX_ATTEMPTS, DISCOVERY_RETRY_INTERVAL_SEC);

    for (attempt = 0; attempt < DISCOVERY_MAX_ATTEMPTS && found == NX_FALSE; attempt++)
    {
        /* Feed the watchdog once per attempt: this loop can legitimately
         * run for DISCOVERY_MAX_ATTEMPTS * DISCOVERY_RETRY_INTERVAL_SEC
         * (~30s) when called from App_HTTP_Thread_Entry's consecutive-
         * failure branch mid-round, and nothing else in that call path
         * stamps WatchdogLastProgressTick while it's in here -- see the
         * watchdog comment above WatchdogLastProgressTick's declaration
         * for the real serial.log evidence this was actually happening
         * (a slow-but-succeeding rediscovery getting killed by the
         * watchdog instead of the software-only recovery it was about to
         * complete on its own). Harmless when called from thread startup
         * too, before the watchdog is even armed. */
        WatchdogLastProgressTick = tx_time_get();

        ret = nx_packet_allocate(&AppPool, &send_packet, NX_IPv4_UDP_PACKET, NX_NO_WAIT);
        if (ret != NX_SUCCESS)
        {
            printf("Discovery: packet allocate failed: 0x%02X\r\n", ret);
            break;
        }

        ret = nx_packet_data_append(send_packet, DISCOVERY_REQUEST, DISCOVERY_REQUEST_LEN,
                                    &AppPool, NX_NO_WAIT);
        if (ret != NX_SUCCESS)
        {
            printf("Discovery: packet fill failed: 0x%02X\r\n", ret);
            nx_packet_release(send_packet);
            break;
        }

        ret = nx_udp_socket_send(&discovery_socket, send_packet, DISCOVERY_BROADCAST_ADDR, DISCOVERY_PORT);
        if (ret != NX_SUCCESS)
        {
            /* nx_udp_socket_send only releases the packet on success --
             * same contract this file already relies on for
             * nx_web_http_client_put_packet's send_packet further down. */
            printf("Discovery: send failed: 0x%02X (attempt %u/%d)\r\n",
                   ret, attempt + 1, DISCOVERY_MAX_ATTEMPTS);
            nx_packet_release(send_packet);
        }

        ret = nx_udp_socket_receive(&discovery_socket, &receive_packet,
                                    DISCOVERY_RETRY_INTERVAL_SEC * NX_IP_PERIODIC_RATE);
        if (ret == NX_SUCCESS)
        {
            bytes = 0;
            nx_packet_data_extract_offset(receive_packet, 0, reply_buffer,
                                          sizeof(reply_buffer) - 1, &bytes);
            reply_buffer[bytes] = 0;

            if (bytes == DISCOVERY_REPLY_LEN && memcmp(reply_buffer, DISCOVERY_REPLY, DISCOVERY_REPLY_LEN) == 0)
            {
                nx_udp_source_extract(receive_packet, &reply_address, &reply_port);
                ServerAddress = reply_address;
                FormatIPAddress(reply_address, ServerHost);
                printf("Discovery: server found at %s (attempt %u/%d)\r\n",
                       ServerHost, attempt + 1, DISCOVERY_MAX_ATTEMPTS);
                found = NX_TRUE;
            }
            else
            {
                /* Some other broadcast traffic on the same port/subnet --
                 * ignore it and keep waiting out this attempt's window. */
                printf("Discovery: got a %lu-byte reply that wasn't ours -- ignoring\r\n", bytes);
            }
            nx_packet_release(receive_packet);
        }
        /* else: no reply within this attempt's window -- NX_NO_PACKET
         * (0x01) is the expected, normal case here (nobody's answered
         * yet), not worth logging every ~2s; just loop around and
         * broadcast again. */
    }

    nx_udp_socket_unbind(&discovery_socket);
    nx_udp_socket_delete(&discovery_socket);

    if (found == NX_FALSE)
    {
        printf("Discovery: no server found after %d attempts -- falling back to %s\r\n",
               DISCOVERY_MAX_ATTEMPTS, ServerHost);
        return NX_NOT_SUCCESSFUL;
    }
    return NX_SUCCESS;
}

/* Periodic HTTPS POST client: every HTTP_POLL_PERIOD_MS milliseconds,
 * POSTs one combined JSON reading of the whole onboard sensor suite (see
 * sensors.h/.c -- HTS221, LPS22HH, ISM330DHCX, IIS2MDC, VEML3235, all on
 * I2C2) to ServerHost:HTTP_SERVER_HTTPS_PORT/HTTP_RESOURCE (found via
 * Discover_ServerIP() at thread start, or re-found mid-session -- see
 * that function and the DISCOVERY_* block in app_netxduo.h) and prints
 * the (decrypted) response body: post_secure_start (runs the TLS
 * handshake via tls_setup_callback above, then sends headers +
 * Content-Length over the now-encrypted connection) ->
 * request_packet_allocate -> nx_packet_data_append (fills the body) ->
 * put_packet (encrypts and sends it).
 *
 * This used to POST each sensor category to its own resource
 * (/temperature, /accelerometer, etc, one handshake each -- still exactly
 * what Sensors_Endpoints[] in sensors.c does under the hood), but a full
 * TLS handshake on this hardware costs ~500-650ms (RSA-2048, no crypto
 * offload) and doing one per category made a full sweep take 3.5-5s; one
 * combined POST needs only a single handshake per round.
 *
 * It also used to redo that one handshake on *every* round -- a fresh
 * nx_web_http_client_create() + post_secure_start() + ... +
 * nx_web_http_client_delete() cycle each time, deliberately mirroring
 * NetX Duo's own HTTP client POST sample (netx_web_post_basic_test.c).
 * That made every single round pay the full ~500-650ms RSA cost, which
 * is the actual speed ceiling on how often this can send. Now
 * nx_web_http_client_create() only runs once (http_client_ready below),
 * and post_secure_start() is called again on that same, still-open
 * client every round: since tools/http_server.py's responses now claim
 * HTTP/1.1 (Handler.protocol_version there), NetX Duo's own
 * _nx_web_http_client_secure_connect() notices the previous response
 * said to keep the connection alive, checks the TCP socket is still
 * NX_TCP_ESTABLISHED, and -- if so -- skips the TCP connect *and* the
 * TLS handshake entirely, going straight to sending this round's
 * request over the already-encrypted connection. If the connection
 * *isn't* still alive (first round ever, or the peer closed it since),
 * that same call transparently falls back to a full fresh connect +
 * handshake, still using this one client object -- no special-casing
 * needed here for that, it's already how the vendored library behaves
 * (see _nx_web_http_client_connect/_nx_web_http_client_secure_connect in
 * nx_web_http_client.c). The elapsed-time print below will show
 * ~500-650ms only on whichever (now rare) rounds actually pay for a
 * handshake, and a small fraction of that on every reused round.
 *
 * That reuse check only looks at TCP socket state, though -- a
 * connection that's silently died since the last round (Wi-Fi packet
 * loss ate the peer's close, or it just vanished) can still read back as
 * NX_TCP_ESTABLISHED locally, so post_secure_start reuses it anyway, and
 * whichever call actually tries to move data over it next
 * (request_packet_allocate / put_packet / the response-read loop) is
 * what discovers the truth. Observed on real hardware: one of those
 * blocking that long used to blow WATCHDOG_STALE_TICKS and IWDG-reset
 * the *entire board* (full Wi-Fi rejoin + DHCP) just to get back a TLS
 * connection -- see HTTP_SEND_TIMEOUT_TICKS in app_netxduo.h and every
 * nx_web_http_client_delete()/http_client_ready = NX_FALSE pairing
 * below: each of those three steps now fails fast instead of blocking
 * anywhere near that long, and explicitly forces next round to
 * reconnect from scratch rather than trust this same client object
 * again -- a software-only connection reset, several hundred ms at
 * most, instead of a full board reset. */
static VOID App_HTTP_Thread_Entry(ULONG thread_input)
{
    UINT        ret;
    UINT        get_status;
    NXD_ADDRESS server_ip_address;
    NX_PACKET  *send_packet;
    NX_PACKET  *receive_packet;
    UCHAR       receive_buffer[256];
    ULONG       bytes;
    ULONG       round = 0;
    /* static, not a plain local: this thread's stack is already sized
     * tight against the RSA call chain (see the 8x DEFAULT_MEMORY_SIZE
     * comment in MX_NetXDuo_Init below) -- putting 768 more bytes there
     * on top of send/receive buffers would eat back into that margin for
     * no reason, since this buffer only ever needs one writer at a time
     * from this one thread anyway. */
    static CHAR body[768];
    UINT        body_len;
    ULONG       t0;
    ULONG       elapsed_ms;
    ULONG       waited;
    UINT        consecutive_connect_failures = 0;
    UINT        http_client_ready = NX_FALSE;

    TX_PARAMETER_NOT_USED(thread_input);

    /* Probe the sensor suite once, up front: it's independent of Wi-Fi/DHCP
     * (I2C2, not the network), each of the 6 sensors self-reports OK/FAILED
     * over the same serial log, and Sensors_ReadAllJSON() below simply
     * omits whatever didn't come up rather than blocking the HTTP loop
     * on it. */
    Sensors_Init();

    /* Find the server before ever dialing it -- see Discover_ServerIP()
     * and the DISCOVERY_* block in app_netxduo.h. ServerAddress/ServerHost
     * already hold the HTTP_SERVER_ADDRESS/HTTP_SERVER_HOST fallback from
     * their static initializers above, so a failed discovery here (no
     * reply inside ~30s) just means the loop below starts by dialing that
     * fallback instead, exactly as if this call had never been added. */
    Discover_ServerIP();

    server_ip_address.nxd_ip_version = NX_IP_VERSION_V4;
    server_ip_address.nxd_ip_address.v4 = ServerAddress;

    printf("HTTPS POST client ready. Target https://%s:%d%s\r\n",
           ServerHost, HTTP_SERVER_HTTPS_PORT, HTTP_RESOURCE);

    /* Arm the watchdog only now -- Wi-Fi join, DHCP, sensor probing and
     * server discovery are all done, so from here on a stall of
     * WATCHDOG_STALE_TICKS really does mean something's wedged, not just a
     * slow-but-normal boot step still in progress. See the watchdog
     * comment near WatchdogLastProgressTick's declaration above for the
     * full story. */
    WatchdogLastProgressTick = tx_time_get();
#if defined(DBGMCU_APB1FZR1_DBG_IWDG_STOP)
    /* Halt the IWDG countdown while a debugger has the core stopped at a
     * breakpoint -- without this, pausing in the debugger for longer than
     * the ~3-4s timeout resets the board out from under the debug session,
     * which has nothing to do with a real stall. */
    __HAL_DBGMCU_FREEZE_IWDG();
#endif
    hiwdg.Instance       = IWDG;
    hiwdg.Init.Prescaler = IWDG_PRESCALER_256;
    hiwdg.Init.Window    = IWDG_WINDOW_DISABLE;
    hiwdg.Init.Reload    = WATCHDOG_IWDG_RELOAD;
    if (HAL_IWDG_Init(&hiwdg) != HAL_OK)
    {
        printf("Watchdog: HAL_IWDG_Init failed -- continuing without it\r\n");
    }
    else
    {
        tx_timer_create(&WatchdogTimer, "Watchdog Timer", watchdog_timer_entry, 0,
                        WATCHDOG_CHECK_TICKS, WATCHDOG_CHECK_TICKS, TX_AUTO_ACTIVATE);
        printf("Watchdog: armed (~3-4s worst case to reset if a poll round stalls that long)\r\n");
    }

    while (1)
    {
        /* Mark forward progress for the watchdog *before* doing this
         * round's work, not after -- see watchdog_timer_entry() above. If
         * this round's blocking calls below wedge, this is the last
         * timestamp that ever gets set, so staleness (and the eventual
         * reset) is measured from the moment it actually got stuck, not
         * from whenever the previous, successful round happened to
         * finish. */
        WatchdogLastProgressTick = tx_time_get();

        /* Read every sensor that's up into one combined JSON body --
         * see sensors.h. Content-Length has to be known up front for
         * post_secure_start() below, so this has to happen before the
         * TLS handshake even starts, not while streaming the body out. */
        body_len = (UINT)Sensors_ReadAllJSON(body, sizeof(body));

        printf("--- poll round #%lu: connecting to %s:%d ---\r\n",
               round, ServerHost, HTTP_SERVER_HTTPS_PORT);

        /* window_size bumped 1536 -> 8192: found via `ss -i` on the server
         * while a connection sat stuck -- mss:768, and the server had been
         * retransmitting the same segment for 4.5 minutes (retrans:1/12,
         * bytes_acked stuck at exactly 768 = one segment) because our
         * 1536-byte window only ever allowed 2 segments in flight, and
         * once the second one needed a retry, there was no window room
         * left to make progress. ServerHello + our ~800-byte self-signed
         * Certificate + ServerHelloDone need more than 1536 bytes of
         * simultaneous in-flight room at 768 bytes/segment; this is the
         * exact same class of bug as the packet-pool-too-small issue this
         * file already documents for TLS (10 -> 32 packets) -- a value
         * sized for plain HTTP, never revisited when TLS was layered on
         * top. 8192 is comfortably within AppPool's ~48KB capacity.
         *
         * Only actually runs once now (http_client_ready), not every
         * round -- see the big comment above this function for why: the
         * same HttpClient is reused round after round, and
         * post_secure_start() below is what decides on its own whether
         * that means skipping the handshake or redoing it. */
        if (!http_client_ready)
        {
            ret = nx_web_http_client_create(&HttpClient, "HTTP Client", &IpInstance, &AppPool, 8192);
            if (ret == NX_SUCCESS)
            {
                http_client_ready = NX_TRUE;
            }
            else
            {
                printf("HTTP client create failed: 0x%02X\r\n", ret);
            }
        }

        if (http_client_ready)
        {
            /* post_secure_start runs the TLS handshake (via tls_setup_callback
             * above) and then, over the now-encrypted connection, sends the
             * request line + headers (including Content-Length: body_len
             * from the total_bytes argument below); the body itself goes out
             * separately via put_packet.
             *
             * Timeout bumped down from 30s to 8s: real-hardware testing
             * showed occasional TCP connects that never get a response at
             * all (SYN_SENT forever -- real Wi-Fi packet loss, not
             * anything this code can fix) and, separately, failed
             * connects whose internal cleanup took roughly *double* the
             * configured wait_option to actually return an error (30s
             * requested -> ~60-70s observed before the call gave back
             * control) -- so the old 30s ceiling meant one bad connection
             * could stall the whole thread for over a minute. 8s is
             * comfortably above every successful handshake actually
             * observed (~450-650ms), so this shouldn't cut off anything
             * that was going to succeed anyway, just abandon a dead
             * connection faster. This does NOT bound nx_crypto_rsa.c's own
             * modular exponentiation, though (no yield points, done
             * entirely in software -- can't be preempted or timed out by
             * anything short of the IWDG watchdog above), so a genuine
             * stuck-mid-RSA handshake still relies on that as the
             * backstop. The elapsed-time print below shows the real cost
             * of each handshake either way. */
            print_pool_status("before post_secure_start");
            t0 = tx_time_get();
            /* Arm the heartbeat: every 2s from here, diag_heartbeat_entry()
             * prints elapsed time + pool + TCP socket state -- what turns a
             * previously-silent multi-second gap into a live trace. Stays
             * armed for the *entire* round now, not just this call: request_
             * packet_allocate/put_packet/the response-read loop below all
             * used to run with the heartbeat already deactivated, so a stall
             * inside any of them (the real hardware case that motivated
             * HTTP_SEND_TIMEOUT_TICKS -- see its comment in app_netxduo.h)
             * produced total silence right up until either it recovered or
             * the IWDG reset the board, with no evidence of where it was
             * actually stuck. Deactivated on every exit path below instead
             * of unconditionally right after this call returns. */
            DiagHeartbeatT0 = t0;
            DiagHeartbeatTicks = 0;
            tx_timer_change(&DiagHeartbeatTimer, 2 * TX_TIMER_TICKS_PER_SECOND, 2 * TX_TIMER_TICKS_PER_SECOND);
            tx_timer_activate(&DiagHeartbeatTimer);
            ret = nx_web_http_client_post_secure_start(&HttpClient, &server_ip_address, HTTP_SERVER_HTTPS_PORT,
                                                        HTTP_RESOURCE, ServerHost, NX_NULL, NX_NULL,
                                                        body_len, tls_setup_callback, 8 * NX_IP_PERIODIC_RATE);
            elapsed_ms = (tx_time_get() - t0) * 1000UL / TX_TIMER_TICKS_PER_SECOND;
            print_pool_status("after post_secure_start");
            if (ret != NX_SUCCESS)
            {
                tx_timer_deactivate(&DiagHeartbeatTimer);
                printf("POST (TLS) start failed: 0x%02X after %lu ms\r\n", ret, elapsed_ms);
                consecutive_connect_failures++;
                if (consecutive_connect_failures >= DISCOVERY_RETRIGGER_FAILURES)
                {
                    /* Several rounds in a row couldn't even get a connect
                     * going -- most likely the server moved to a new IP
                     * mid-session (same hotspot-restart scenario as at
                     * boot, just now instead of then). Re-broadcast and
                     * pick up wherever it is now; server_ip_address is
                     * rebuilt right after in case ServerAddress changed.
                     *
                     * Also tear down and rebuild HttpClient itself here,
                     * not just the address: with the connection normally
                     * reused round after round now (see the big comment
                     * above this function), reaching this branch at all
                     * means something's stayed broken across several
                     * rounds despite the vendored library's own
                     * transparent reconnect-on-reuse-failure logic already
                     * having every chance to recover it on its own -- a
                     * full delete+recreate is a cheap, unconditional reset
                     * of last resort precisely because that normal path
                     * has already been tried and hasn't been enough. */
                    printf("Discovery: %u consecutive connect failures -- re-discovering server "
                           "and resetting the HTTP client...\r\n", consecutive_connect_failures);
                    Discover_ServerIP();
                    server_ip_address.nxd_ip_address.v4 = ServerAddress;
                    nx_web_http_client_delete(&HttpClient);
                    http_client_ready = NX_FALSE;
                    consecutive_connect_failures = 0;
                }
            }
            else
            {
                consecutive_connect_failures = 0;
                printf("TLS handshake + HTTP headers sent ok after %lu ms, sending body...\r\n", elapsed_ms);
                /* HTTP_SEND_TIMEOUT_TICKS (2s), not the old blind 5s -- see
                 * its comment in app_netxduo.h. This call only ever runs
                 * right after post_secure_start just claimed the
                 * connection is up, and it's a local pool operation with
                 * no network wait really needed, so 2s is already
                 * generous. */
                ret = nx_web_http_client_request_packet_allocate(&HttpClient, &send_packet,
                                                                  HTTP_SEND_TIMEOUT_TICKS);
                if (ret != NX_SUCCESS)
                {
                    tx_timer_deactivate(&DiagHeartbeatTimer);
                    printf("POST packet allocate failed: 0x%02X -- resetting connection for next round\r\n", ret);
                    /* Force a fresh connect+handshake next round rather
                     * than trust this same client object again -- see the
                     * HTTP_SEND_TIMEOUT_TICKS comment in app_netxduo.h for
                     * why a stalled/failed step here is treated as reason
                     * enough to rebuild, not just retry the same reused
                     * connection. */
                    nx_web_http_client_delete(&HttpClient);
                    http_client_ready = NX_FALSE;
                }
                else
                {
                    nx_packet_data_append(send_packet, body, body_len, &AppPool,
                                          HTTP_SEND_TIMEOUT_TICKS);

                    /* Same HTTP_SEND_TIMEOUT_TICKS reasoning as above, and
                     * this is the call that was actually observed on real
                     * hardware blocking long enough (the old blind 5s) to
                     * blow WATCHDOG_STALE_TICKS and IWDG-reset the whole
                     * board -- rejoining Wi-Fi and redoing DHCP -- just to
                     * recover from what a much smaller, software-only
                     * connection reset would have fixed just as well: a
                     * connection that's silently died since the last round
                     * (peer never sent a clean close) still looks locally
                     * NX_TCP_ESTABLISHED to post_secure_start's own reuse
                     * check above, so it reuses it, and this is where that
                     * false confidence actually gets tested against the
                     * network. */
                    ret = nx_web_http_client_put_packet(&HttpClient, send_packet,
                                                        HTTP_SEND_TIMEOUT_TICKS);
                    if (ret != NX_SUCCESS)
                    {
                        tx_timer_deactivate(&DiagHeartbeatTimer);
                        printf("POST send failed: 0x%02X -- resetting connection for next round\r\n", ret);
                        nx_packet_release(send_packet);
                        nx_web_http_client_delete(&HttpClient);
                        http_client_ready = NX_FALSE;
                    }
                    else
                    {
                        /* Drain and print the response body, one packet at a time.
                         * See RESPONSE_POLL_TICKS/RESPONSE_TIMEOUT_TICKS in
                         * app_netxduo.h for why this polls in short slices
                         * (checking ConnectionIsIdle() between them) instead
                         * of handing this call one flat 5-second wait_option
                         * and just accepting however long a dead connection
                         * took to finally time out. */
                        get_status = NX_SUCCESS;
                        waited = 0;
                        while (get_status != NX_WEB_HTTP_GET_DONE)
                        {
                            get_status = nx_web_http_client_response_body_get(&HttpClient, &receive_packet,
                                                                               RESPONSE_POLL_TICKS);

                            if (get_status == NX_NO_PACKET && !ConnectionIsIdle() && waited < RESPONSE_TIMEOUT_TICKS)
                            {
                                /* Nothing yet, but the connection's still
                                 * alive and the old 5s budget isn't used up
                                 * -- keep waiting, same as before. */
                                waited += RESPONSE_POLL_TICKS;
                                continue;
                            }

                            if (get_status != NX_SUCCESS && get_status != NX_WEB_HTTP_GET_DONE)
                            {
                                printf("POST response read failed: 0x%02X%s -- resetting connection for next round\r\n",
                                       get_status,
                                       ConnectionIsIdle() ?
                                       " (connection went idle)" : "");
                                /* Same reasoning as the allocate/put_packet
                                 * failure paths above: don't trust this
                                 * client object again next round, force a
                                 * fresh connect+handshake instead. */
                                nx_web_http_client_delete(&HttpClient);
                                http_client_ready = NX_FALSE;
                                break;
                            }

                            bytes = 0;
                            nx_packet_data_extract_offset(receive_packet, 0, receive_buffer,
                                                          sizeof(receive_buffer) - 1, &bytes);
                            receive_buffer[bytes] = 0;
                            printf("POST %s <- %s -> %s\r\n", HTTP_RESOURCE, body,
                                   (char *)receive_buffer);
                            nx_packet_release(receive_packet);
                        }

                        /* Both ways the loop above can end -- a clean
                         * GET_DONE, or the idle/error break -- fall through
                         * to here, so one deactivate call covers either.
                         * (The allocate/put_packet failure branches further
                         * up have their own, since those return before ever
                         * reaching this point at all.) */
                        tx_timer_deactivate(&DiagHeartbeatTimer);
                        HAL_GPIO_TogglePin(LED_GREEN_GPIO_Port, LED_GREEN_Pin);
                    }
                }
            }
            /* No unconditional nx_web_http_client_delete() here anymore --
             * HttpClient stays alive and open for reuse into next round's
             * post_secure_start() unless the discovery-retrigger branch
             * above (or a future failure path) explicitly decided to tear
             * it down. */
        }

        round++;
        tx_thread_sleep((HTTP_POLL_PERIOD_MS * TX_TIMER_TICKS_PER_SECOND) / 1000);
    }
}
/* USER CODE END 1 */
