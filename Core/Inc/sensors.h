/* sensors.h -- onboard B-U585I-IOT02A sensor suite (I2C2), read out as one
 * small JSON object per sensor category, each posted to its own HTTP
 * resource (see Sensors_Endpoints[] below and App_HTTP_Thread_Entry in
 * NetXDuo/App/app_netxduo.c, which posts each entry in turn every poll
 * period).
 *
 * Wraps the ST BSP drivers vendored under Drivers/BSP/{B-U585I-IOT02A,
 * Components/*} (copied from STM32Cube_FW_U5_V1.8.0 -- prebuilt ST sensor
 * drivers, not hand-rolled): HTS221 (temp+humidity), LPS22HH
 * (temp+pressure), ISM330DHCX (accel+gyro), IIS2MDC (magnetometer),
 * VEML3235 (ambient light), VL53L5CX (8x8-zone multi-zone ToF ranging).
 * All six sensors share one bus, I2C2 -- see b_u585i_iot02a_bus.c,
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
 * every Sensors_Endpoints[].read below simply reports "nothing to send"
 * (returns 0) for whatever didn't come up. */
void Sensors_Init(void);

/* Fills *buf with one sensor category's reading as a compact JSON object
 * (e.g. {"x":-12,"y":34,"z":998} for accelerometer, or
 * {"hts221_c":23.41,"lps22hh_c":23.60} for temperature) and returns the
 * number of bytes written, NOT including the terminating NUL (i.e. the
 * value to pass as an HTTP Content-Length). Returns 0 -- buf left
 * untouched -- if every sensor behind this category failed at
 * Sensors_Init() or errored on this particular read; the caller
 * (App_HTTP_Thread_Entry) is expected to just skip posting that resource
 * for this round rather than send an empty/placeholder body.
 *
 * Values are hand-formatted as fixed-point (2 decimal places) rather than
 * with printf's "%f": this project shows no existing use of float-format
 * printf/snprintf anywhere, and STM32CubeIDE's default nano.specs newlib
 * build does not support it unless explicitly re-linked with
 * "-u _printf_float" -- easy to get silently-wrong output from, so this
 * sidesteps it entirely instead of assuming that flag is set. */
typedef uint32_t (*Sensors_ReadFn)(char *buf, uint32_t buf_size);

typedef struct
{
    const char     *resource; /* HTTP resource path this category posts to, e.g. "/temperature" */
    Sensors_ReadFn  read;     /* see Sensors_ReadFn above */
} Sensors_Endpoint_t;

/* One entry per sensor category -- temperature, humidity, pressure,
 * accelerometer, gyroscope, magnetometer, light, ranging.
 * App_HTTP_Thread_Entry walks this whole table once per poll period and
 * POSTs each non-empty reading to its own resource, so e.g. temperature
 * data always lands on /temperature and accelerometer data always lands
 * on /accelerometer, as separate requests. Recommended body buffer size
 * for any single entry: >= 512 bytes (the largest single category, ranging
 * -- {"zones":[...]} at the 4x4 profile -- is a few hundred bytes; 512
 * leaves headroom without probing the exact worst case). */
extern const Sensors_Endpoint_t Sensors_Endpoints[];
extern const uint32_t Sensors_EndpointCount;

#ifdef __cplusplus
}
#endif

#endif /* SENSORS_H */
