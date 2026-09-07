/* sensors.h -- onboard B-U585I-IOT02A sensor suite (I2C2), read out as one
 * combined JSON object per poll round (see Sensors_ReadAllJSON below).
 *
 * Wraps the ST BSP drivers vendored under Drivers/BSP/{B-U585I-IOT02A,
 * Components/*} (copied from STM32Cube_FW_U5_V1.8.0 -- prebuilt ST sensor
 * drivers, not hand-rolled): HTS221 (temp+humidity), LPS22HH
 * (temp+pressure), ISM330DHCX (accel+gyro), IIS2MDC (magnetometer),
 * VEML3235 (ambient light), VL53L5CX (8x8-zone multi-zone ToF ranging --
 * currently never actually brought up, see Sensors_Init()'s comment on
 * why). All six sensors share one bus, I2C2 -- see b_u585i_iot02a_bus.c,
 * BSP_I2C2_Init() -- which each sensor driver's own Init()/Probe() call
 * (and which self-configures its own GPIO/clocks: no CubeMX I2C2 peripheral
 * needed in the .ioc, this is not wired through HAL_I2C_MspInit).
 */
#ifndef SENSORS_H
#define SENSORS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Probes and enables every sensor. Call once, from a plain RTOS thread
 * context after tx_kernel_enter() (BSP_I2C2_Init() and the VL53L5CX's
 * ~84 KB firmware upload both use HAL_GetTick()-based busy-wait delays --
 * fine from a thread, not from an ISR or before the scheduler is running).
 *
 * Each of the 6 sensors is probed independently and a failure on any one
 * (missing part, I2C NAK, bad ID) is logged and does not stop the rest --
 * Sensors_ReadAllJSON() below simply omits whatever didn't come up. */
void Sensors_Init(void);

/* Fills *buf with every sensor's latest reading as one combined JSON
 * object, one key per category -- {"temperature":{"hts221_c":23.41,
 * "lps22hh_c":23.60},"humidity":{"hts221_rh":41.20},"pressure":{...},
 * "accelerometer":{"x":-12,"y":34,"z":998},"gyroscope":{...},
 * "magnetometer":{...},"light":{"als_raw":812,"white_raw":640}} -- and
 * returns the number of bytes written, NOT including the terminating NUL
 * (i.e. the value to pass as an HTTP Content-Length), or 0 if buf/buf_size
 * are unusable. A category whose sensor(s) failed at Sensors_Init(), or
 * errored on this particular read, is simply left out of the object
 * rather than aborting the whole payload -- always as much data as is
 * actually available, never all-or-nothing.
 *
 * One combined object -> one POST per poll round, deliberately: this used
 * to be routed as one small object per category, each posted to its own
 * resource (/temperature, /accelerometer, etc.), but a full TLS handshake
 * on this hardware costs ~500-650ms (RSA-2048 modular exponentiation, no
 * crypto offload) and doing one handshake per category per round made a
 * full sweep take 3.5-5s. One combined POST needs only one handshake, so
 * this is the throughput-favoring tradeoff -- per-resource routing can be
 * resurrected later (see Sensors_Endpoints[] below, still exactly what it
 * was) if separate endpoints matter more than round latency again.
 *
 * Values are hand-formatted as fixed-point (2 decimal places) rather than
 * with printf's "%f": this project shows no existing use of float-format
 * printf/snprintf anywhere, and STM32CubeIDE's default nano.specs newlib
 * build does not support it unless explicitly re-linked with
 * "-u _printf_float" -- easy to get silently-wrong output from, so this
 * sidesteps it entirely instead of assuming that flag is set. Recommended
 * buf_size: >= 512 bytes (a full reading with every category present is a
 * few hundred bytes; 512 leaves headroom without probing the exact worst
 * case). */
uint32_t Sensors_ReadAllJSON(char *buf, uint32_t buf_size);

/* Per-category building blocks Sensors_ReadAllJSON() above is built from
 * -- each read() fills *buf with just that one category's JSON object
 * (e.g. {"x":-12,"y":34,"z":998} for accelerometer) and returns its byte
 * count, or 0 if that category has nothing to report this round. `resource`
 * is this category's former (and readily-resurrectable) standalone HTTP
 * path, e.g. "/accelerometer" -- Sensors_ReadAllJSON() also reuses it
 * directly as the category's key in the combined object, minus the
 * leading '/'. Exposed here mainly so that mapping is table-driven rather
 * than duplicated by hand; not otherwise expected to be called directly. */
typedef uint32_t (*Sensors_ReadFn)(char *buf, uint32_t buf_size);

typedef struct
{
    const char     *resource;
    Sensors_ReadFn  read;
} Sensors_Endpoint_t;

extern const Sensors_Endpoint_t Sensors_Endpoints[];
extern const uint32_t Sensors_EndpointCount;

#ifdef __cplusplus
}
#endif

#endif /* SENSORS_H */
