/**
 * nx_rand_hw.h -- hardware TRNG-backed replacement for NX_RAND().
 *
 * See nx_rand_hw.c for why this exists (nothing in this project seeds
 * libc's rand(), so it would otherwise return the same "random" sequence
 * every single boot -- unacceptable for anything TLS uses NX_RAND() for).
 * Wired in via NetXDuo/App/nx_user.h (#define NX_RAND nx_rand_hw).
 */
#ifndef NX_RAND_HW_H
#define NX_RAND_HW_H

#ifdef __cplusplus
extern "C" {
#endif

unsigned int nx_rand_hw(void);

#ifdef __cplusplus
}
#endif

#endif /* NX_RAND_HW_H */
