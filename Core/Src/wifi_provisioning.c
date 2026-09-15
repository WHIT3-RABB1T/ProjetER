/**
 * wifi_provisioning.c -- see wifi_provisioning.h for the full picture.
 *
 * Two small pieces:
 *
 *   1. Flash-backed credential storage (wifi_cred_* below) -- the last
 *      8KB page of this MCU's 2MB flash (0x081FE000, Bank 2 Page 127;
 *      STM32U585 is fixed dual-bank, 128 pages of 8KB per 1MB bank, no
 *      software-configurable single-bank alternative exists in this
 *      HAL, so there's no ambiguity to detect at runtime here) holds one
 *      small struct: a magic number, the SSID/password, and a checksum.
 *      Comfortably clear of any code this firmware will ever grow to.
 *
 *   2. A blocking serial prompt (wifi_serial_read_line, called from
 *      WifiProvisioning_Init below) over USART1 -- the same UART printf
 *      already uses for all its debug output (Core/Src/main.c's
 *      MX_USART1_UART_Init: 115200 8N1, UART_MODE_TX_RX -- full duplex
 *      was already configured, RX just never used for anything until
 *      now). Open any serial terminal to the board's USB-serial port,
 *      type the SSID, press Enter, type the password (or just press
 *      Enter for none), and that's it -- no phone, no second Wi-Fi
 *      network, no HTML form.
 */
#include "wifi_provisioning.h"
#include "main.h"                 /* USER_BUTTON_Pin/Port */
#include "tx_api.h"               /* tx_thread_sleep/TX_TIMER_TICKS_PER_SECOND -- see the polling loop below */
#include <string.h>
#include <stdio.h>
#include <stdint.h>

/* Declared (and already used the same way) in stm32u5xx_it.c for the
 * USART1 IRQ handler -- HAL_UART_Init() already ran (MX_USART1_UART_Init,
 * called from main() well before MX_NetXDuo_Init/WifiProvisioning_Init)
 * by the time anything here runs, so this is just borrowing the same
 * live handle, not creating a second one. */
extern UART_HandleTypeDef huart1;

/* ---- globals declared extern in wifi_provisioning.h ------------------ */
char g_wifi_ssid[WIFI_PROVISIONING_SSID_MAX_LEN + 1];
char g_wifi_password[WIFI_PROVISIONING_PASSWORD_MAX_LEN + 1];

/* ======================================================================
 * Flash-backed credential storage
 * ====================================================================== */

/* 'WIFI' -- distinguishes "a real saved network" from flash's erased
 * (all-0xFF) state or leftover unrelated data, before the checksum below
 * even gets checked. */
#define WIFI_CRED_MAGIC        0x57494649UL

#define WIFI_CRED_FLASH_ADDR   0x081FE000UL
#define WIFI_CRED_FLASH_BANK   FLASH_BANK_2
#define WIFI_CRED_FLASH_PAGE   127U

typedef struct
{
    uint32_t magic;
    char     ssid[WIFI_PROVISIONING_SSID_MAX_LEN + 1];
    char     password[WIFI_PROVISIONING_PASSWORD_MAX_LEN + 1];
    uint32_t checksum;
} WifiCredentials_t;

/* HAL_FLASH_Program on this MCU only writes whole 128-bit (16-byte)
 * quad-words -- round up so the write loop below always hands it a
 * full one, regardless of sizeof(WifiCredentials_t)'s actual padding. */
#define WIFI_CRED_PADDED_SIZE  ((sizeof(WifiCredentials_t) + 15U) / 16U * 16U)

/* Not a security boundary -- ssid/password go into flash in plain text
 * either way, same as the literals this replaces did in source. Just
 * enough to tell "a real saved network" apart from noise/corruption:
 * FNV-1a over ssid then password, with a separator byte between them so
 * ("ab","") and ("a","b") don't collide. */
static uint32_t wifi_cred_checksum(const char *ssid, const char *password)
{
    uint32_t hash = 2166136261UL;
    const char *p;

    for (p = ssid; *p != '\0'; p++)
    {
        hash ^= (uint8_t)*p;
        hash *= 16777619UL;
    }
    hash ^= 0xFFU;
    for (p = password; *p != '\0'; p++)
    {
        hash ^= (uint8_t)*p;
        hash *= 16777619UL;
    }
    return hash;
}

/* Erases the credentials page and writes a new SSID/password. Unlike an
 * earlier SoftAP-based design this project tried (see the file header
 * comment in wifi_provisioning.h), there is no separate "mode" (AP vs
 * station) that boot already committed to before knowing the answer --
 * this can just save and let the caller continue straight into
 * nx_ip_create() the same boot, no reboot required. */
static void wifi_cred_save(const char *ssid, const char *password)
{
    WifiCredentials_t creds;
    uint8_t padded[WIFI_CRED_PADDED_SIZE];
    FLASH_EraseInitTypeDef erase_init;
    uint32_t page_error = 0;
    uint32_t offset;

    memset(&creds, 0, sizeof(creds));
    creds.magic = WIFI_CRED_MAGIC;
    strncpy(creds.ssid, ssid, WIFI_PROVISIONING_SSID_MAX_LEN);
    creds.ssid[WIFI_PROVISIONING_SSID_MAX_LEN] = '\0';
    strncpy(creds.password, password, WIFI_PROVISIONING_PASSWORD_MAX_LEN);
    creds.password[WIFI_PROVISIONING_PASSWORD_MAX_LEN] = '\0';
    creds.checksum = wifi_cred_checksum(creds.ssid, creds.password);

    /* 0xFF (not 0) for the padding beyond the real struct, matching
     * flash's own erased state -- not that it matters functionally
     * (nothing ever reads past sizeof(WifiCredentials_t)), just avoids
     * programming bits that don't need to change from an erase. */
    memset(padded, 0xFF, sizeof(padded));
    memcpy(padded, &creds, sizeof(creds));

    printf("WiFi provisioning: saving \"%s\" to flash...\r\n", ssid);

    HAL_FLASH_Unlock();

    erase_init.TypeErase = FLASH_TYPEERASE_PAGES;
    erase_init.Banks     = WIFI_CRED_FLASH_BANK;
    erase_init.Page      = WIFI_CRED_FLASH_PAGE;
    erase_init.NbPages   = 1;
    if (HAL_FLASHEx_Erase(&erase_init, &page_error) != HAL_OK)
    {
        printf("WiFi provisioning: flash erase failed (page_error=0x%08lX) -- "
               "NOT saved, will ask again next boot\r\n", (unsigned long)page_error);
        HAL_FLASH_Lock();
        return;
    }

    for (offset = 0; offset < sizeof(padded); offset += 16U)
    {
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_QUADWORD,
                               WIFI_CRED_FLASH_ADDR + offset,
                               (uint32_t)(uintptr_t)&padded[offset]) != HAL_OK)
        {
            printf("WiFi provisioning: flash program failed at offset %lu -- "
                   "credentials may be incomplete, will ask again next boot\r\n",
                   (unsigned long)offset);
            HAL_FLASH_Lock();
            return;
        }
    }

    HAL_FLASH_Lock();
}

/* ======================================================================
 * Blocking serial prompt over USART1
 * ====================================================================== */

/* Reads one line from USART1 into out (max out_size - 1 bytes, always
 * NUL-terminated), echoing each character back so it's visible in the
 * terminal and handling Backspace/Delete for basic correction. Blocks
 * forever a byte at a time (HAL_MAX_DELAY) -- there's nothing else this
 * boot can usefully do before it has an SSID to try anyway, and a
 * terminal's Enter key reliably shows up eventually. Accepts a line
 * ending in '\r', '\n', or "\r\n" (terminal-dependent); the second byte
 * of "\r\n", if any, is drained with a short timeout so it doesn't leak
 * into the *next* read_line call as a stray empty line.
 *
 * Deliberately does not mask password characters (no '*' echo): this is
 * a debug UART on a cable physically attached to the board, not a
 * network-exposed channel, and masking correctly (backspace has to
 * un-mask correctly too) is real complexity this didn't need. */
static void wifi_serial_read_line(char *out, uint32_t out_size)
{
    uint32_t len = 0;
    uint8_t ch;

    for (;;)
    {
        if (HAL_UART_Receive(&huart1, &ch, 1, HAL_MAX_DELAY) != HAL_OK)
        {
            continue; /* spurious UART error -- just keep waiting for real input */
        }

        if (ch == '\r' || ch == '\n')
        {
            uint8_t maybe_lf;

            /* Swallow a same-line-ending '\n' that may follow '\r' (or
             * vice versa) from a CRLF terminal, so it doesn't become a
             * stray blank line read next call. Short timeout: if
             * nothing more arrives quickly, this was the only ending
             * byte the terminal sent. */
            if (HAL_UART_Receive(&huart1, &maybe_lf, 1, 20) == HAL_OK &&
                maybe_lf != '\n' && maybe_lf != '\r')
            {
                /* Not part of the line ending after all -- rare (would
                 * mean the very next line's first byte arrived within
                 * the 20ms window), but don't silently drop it: treat
                 * it as the first byte the caller's *next* read_line
                 * call should have seen. There's no ungetc for a UART,
                 * so the pragmatic choice is to accept the rare loss
                 * here rather than add a one-byte lookahead buffer for
                 * an edge case this interactive, human-typed prompt is
                 * exceedingly unlikely to hit. */
            }

            out[len] = '\0';
            printf("\r\n");
            return;
        }
        else if (ch == 0x08 || ch == 0x7F) /* Backspace or Delete */
        {
            if (len > 0)
            {
                len--;
                printf("\x08 \x08"); /* move back, erase, move back again */
            }
        }
        else if (ch >= 0x20 && ch < 0x7F && len < out_size - 1U) /* printable */
        {
            out[len++] = (char)ch;
            HAL_UART_Transmit(&huart1, &ch, 1, HAL_MAX_DELAY); /* echo */
        }
        /* Anything else (other control bytes, or input past out_size)
         * is silently ignored rather than echoed or stored. */
    }
}

/* Called once, from MX_NetXDuo_Init() (app_netxduo.c), before
 * nx_ip_create() -- nx_ip_create() synchronously runs the Wi-Fi driver's
 * initialize+enable sequence (nx_driver_emw3080.c), which reads the
 * WIFI_SSID/WIFI_PASSWORD macros (now g_wifi_ssid/g_wifi_password via
 * mx_wifi_conf.h) right then -- so this has to already be done by the
 * time nx_ip_create() is called. */
void WifiProvisioning_Init(void)
{
    const WifiCredentials_t *stored = (const WifiCredentials_t *)WIFI_CRED_FLASH_ADDR;
    GPIO_PinState button_state;

    /* USER_BUTTON reads GPIO_PIN_SET while physically held -- same
     * polarity sensors.c's own read_button() already relies on
     * (main.h/main.c's GPIO_Init), so that part's confirmed correct.
     *
     * A single instantaneous read right here, though, means the button
     * has to already be held down at the exact moment this line
     * executes -- a window well under a second into boot, before this
     * function has printed anything yet -- which isn't something a
     * person *reacting* to what just scrolled by on the serial console
     * can realistically hit. Give it a short, forgiving polling window
     * instead: if it's not already held, check a few more times over
     * the next ~1.5s (long enough to react to the prompt below; short
     * enough not to meaningfully slow down the far more common case of
     * nothing held at all). */
    button_state = HAL_GPIO_ReadPin(USER_BUTTON_GPIO_Port, USER_BUTTON_Pin);
    if (button_state != GPIO_PIN_SET)
    {
        UINT i;

        printf("WiFi provisioning: hold USER button now to reprovision Wi-Fi...\r\n");
        for (i = 0; i < 15U; i++)
        {
            tx_thread_sleep(TX_TIMER_TICKS_PER_SECOND / 10U); /* ~100ms */
            button_state = HAL_GPIO_ReadPin(USER_BUTTON_GPIO_Port, USER_BUTTON_Pin);
            if (button_state == GPIO_PIN_SET)
            {
                break;
            }
        }
    }

    if (button_state != GPIO_PIN_SET &&
        stored->magic == WIFI_CRED_MAGIC &&
        stored->ssid[0] != '\0' &&
        stored->checksum == wifi_cred_checksum(stored->ssid, stored->password))
    {
        strncpy(g_wifi_ssid, stored->ssid, WIFI_PROVISIONING_SSID_MAX_LEN);
        g_wifi_ssid[WIFI_PROVISIONING_SSID_MAX_LEN] = '\0';
        strncpy(g_wifi_password, stored->password, WIFI_PROVISIONING_PASSWORD_MAX_LEN);
        g_wifi_password[WIFI_PROVISIONING_PASSWORD_MAX_LEN] = '\0';

        printf("WiFi provisioning: using saved network \"%s\" "
               "(hold USER button at boot to change it)\r\n", g_wifi_ssid);
        return;
    }

    printf("\r\n=== WiFi Setup ===\r\n");
    printf("%s\r\n",
           (button_state == GPIO_PIN_SET) ? "USER button held at boot -- reprovisioning."
                                           : "No valid saved network found.");
    /* printf's output here is line-buffered (flushes on '\n') -- these
     * two prompts deliberately have none, so the typed answer lands on
     * the same visual line, but that also means without an explicit
     * flush the prompt text itself would sit in the C library's stdio
     * buffer and never actually reach the UART until *something* forces
     * it out, well after the (invisible-to-the-user) HAL_UART_Receive
     * below has already started blocking for input. */
    printf("Enter WiFi SSID: ");
    fflush(stdout);
    wifi_serial_read_line(g_wifi_ssid, sizeof(g_wifi_ssid));
    printf("Enter WiFi Password (leave blank for an open network): ");
    fflush(stdout);
    wifi_serial_read_line(g_wifi_password, sizeof(g_wifi_password));

    wifi_cred_save(g_wifi_ssid, g_wifi_password);

    printf("WiFi provisioning: continuing boot with \"%s\"\r\n", g_wifi_ssid);
}
