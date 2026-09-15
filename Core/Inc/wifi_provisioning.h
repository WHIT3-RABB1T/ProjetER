/**
 * wifi_provisioning.h -- Wi-Fi credentials with no hardcoded network in
 * source, and no rebuild/reflash needed to change networks.
 *
 * The problem this replaces: Core/Inc/mx_wifi_conf.h used to #define
 * WIFI_SSID/WIFI_PASSWORD as fixed string literals, the only network the
 * board could ever join without editing and reflashing the firmware --
 * and since that file is tracked in git, the real Wi-Fi password sat in
 * source control too.
 *
 * How this works instead: mx_wifi_conf.h now #defines WIFI_SSID/
 * WIFI_PASSWORD to point at g_wifi_ssid/g_wifi_password below --
 * runtime-populated buffers instead of literals -- so every place that
 * already uses those macros (nx_driver_emw3080.c, entirely vendored ST
 * middleware, untouched by this feature) keeps working exactly as
 * before, just now reading a variable instead of a constant.
 *
 * WifiProvisioning_Init() -- called from MX_NetXDuo_Init() (app_netxduo.c),
 * before nx_ip_create() ever runs the Wi-Fi driver's join sequence --
 * decides what those buffers actually contain, each boot:
 *
 *   - A previously-saved network in flash, and the board's physical
 *     USER button is NOT held down at boot: copy the saved SSID/
 *     password into the buffers. Nothing else happens; boot proceeds
 *     straight into nx_ip_create() exactly like before this feature
 *     existed.
 *
 *   - No saved network yet, OR the button IS held down (forces
 *     reprovisioning even with a valid saved network): prompt over the
 *     board's existing USART1 debug UART (the same one printf already
 *     uses -- 115200 8N1, already full-duplex, RX just never used for
 *     anything until now) for an SSID and password, save the answer to
 *     flash, and continue the *same* boot into nx_ip_create() with the
 *     freshly-typed values -- no reboot needed, since (unlike a SoftAP-
 *     based scheme) there's no separate "mode" to have already
 *     committed to before knowing the answer.
 *
 * This deliberately does NOT attempt a SoftAP + captive portal (DHCP
 * server + HTTP server the board hosts itself for a phone/laptop to
 * connect to and fill out a form) -- that was tried first and worked
 * right up until it didn't: a real bug two layers down in ST's own
 * vendored DHCP server addon (a 12-byte fixed option-parsing buffer,
 * shared across every DHCP option in a packet, silently overflowed by a
 * real client's own Parameter-Request-List alone, long before the
 * packet's later Requested-IP/Server-ID options -- the two the server
 * actually needed -- were ever reached) that took a live packet capture
 * plus the addon's own vendor-authored tracing to even find, in code
 * this project doesn't own and had no reason to be maintaining a patched
 * fork of. Worth knowing if this is ever revisited, but not worth the
 * risk a second time for what a serial prompt solves in a few dozen
 * lines with no new middleware at all.
 */
#ifndef WIFI_PROVISIONING_H
#define WIFI_PROVISIONING_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Matches MX_WIFI_MAX_SSID_NAME_SIZE/MX_WIFI_MAX_PSWD_NAME_SIZE (mx_wifi.h)
 * -- same sizes the old literal WIFI_SSID/WIFI_PASSWORD had to fit in via
 * nx_driver_emw3080.c's strncpy(ApSettings.SSID/pswd, WIFI_SSID/PASSWORD,
 * sizeof(...)) calls, so these buffers are exactly as large as whatever
 * those calls could ever have copied out of a literal anyway. */
#define WIFI_PROVISIONING_SSID_MAX_LEN      32
#define WIFI_PROVISIONING_PASSWORD_MAX_LEN  64

extern char g_wifi_ssid[WIFI_PROVISIONING_SSID_MAX_LEN + 1];
extern char g_wifi_password[WIFI_PROVISIONING_PASSWORD_MAX_LEN + 1];

/* Call once, from MX_NetXDuo_Init() (app_netxduo.c), before
 * nx_ip_create() -- see the file header comment above for why the
 * ordering matters. Reads flash, checks the USER button, and either
 * copies the saved network into g_wifi_ssid/g_wifi_password or prompts
 * over USART1 (blocking -- see the file header comment) for a new one
 * and saves it to flash before returning. Always returns with
 * g_wifi_ssid/g_wifi_password populated with something to try. */
void WifiProvisioning_Init(void);

#ifdef __cplusplus
}
#endif

#endif /* WIFI_PROVISIONING_H */
