/**
 * nx_rand_hw.c -- hardware TRNG-backed replacement for NX_RAND().
 *
 * NetX/NetX Secure/NetX Crypto default NX_RAND() to plain libc rand()
 * (see Middlewares/ST/netxduo/common/inc/nx_api.h). Nothing in this
 * project ever calls srand(), and newlib-nano's rand() starts from a
 * fixed default seed -- meaning every boot would produce the exact same
 * sequence of "random" numbers. That's fine for nothing in particular and
 * actively wrong for TLS, which uses NX_RAND() (via NX_CRYPTO_RAND(), see
 * crypto_libraries/inc/nx_crypto.h) to seed the DRBG behind the
 * ClientHello random, RSA blinding, etc.
 *
 * This file backs NX_RAND() with the STM32U5's actual hardware TRNG
 * (RNG peripheral, clocked from HSI48 -- see MX_RNG_Init() in main.c)
 * instead. Wired in via NetXDuo/App/nx_user.h: #define NX_RAND nx_rand_hw.
 */
#include "nx_rand_hw.h"
#include "main.h"

extern RNG_HandleTypeDef hrng;

unsigned int nx_rand_hw(void)
{
    uint32_t value = 0;

    /* HAL_RNG_GenerateRandomNumber blocks until the peripheral's internal
     * conditioning/health checks produce a fresh word; on failure (should
     * only happen if the peripheral clock-error-detection trips) fall back
     * to whatever HAL last left in the output register rather than
     * returning a fixed/predictable value. */
    if (HAL_RNG_GenerateRandomNumber(&hrng, &value) != HAL_OK)
    {
        value = hrng.RandomNumber;
    }

    return (unsigned int)value;
}
