/* sensors.h -- onboard B-U585I-IOT02A sensor suite (I2C2), read out as JSON.
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
 * Sensors_ReadJSON() below simply omits whatever didn't come up. */
void Sensors_Init(void);

/* Reads every sensor that initialized successfully and formats the result
 * into *buf as one compact JSON object, e.g.:
 *   {"env":{"hts221":{"temp_c":23.41,"hum_rh":41.20},
 *           "lps22hh":{"temp_c":23.60,"press_hpa":1013.25}},
 *    "motion":{"accel_mg":{"x":-12,"y":34,"z":998},
 *              "gyro_mdps":{"x":10,"y":-5,"z":2},
 *              "mag_mgauss":{"x":210,"y":-88,"z":410}},
 *    "light":{"als_raw":812,"white_raw":640},
 *    "ranging_mm":[120,-1,4300, ... ]}   (16 zones, 4x4 profile; -1 = no
 *                                         valid target in that zone)
 * A sensor that failed at Sensors_Init(), or errors on this particular
 * read, is simply left out of its object (or the whole key, if it's the
 * only member) rather than aborting the whole payload -- always as much
 * data as is actually available, never all-or-nothing.
 *
 * Values are hand-formatted as fixed-point (2 decimal places) rather than
 * with printf's "%f": this project shows no existing use of float-format
 * printf/snprintf anywhere, and STM32CubeIDE's default nano.specs newlib
 * build does not support it unless explicitly re-linked with
 * "-u _printf_float" -- easy to get silently-wrong output from, so this
 * sidesteps it entirely instead of assuming that flag is set.
 *
 * Returns the number of bytes written to *buf, NOT including the
 * terminating NUL (i.e. the value to pass as an HTTP Content-Length),
 * or 0 if buf/buf_size are unusable. The buffer is always left
 * NUL-terminated on success. Recommended buf_size: >= 512 bytes (a full
 * reading with every sensor present is a few hundred bytes; 512 leaves
 * headroom without probing every sensor's exact worst case). */
uint32_t Sensors_ReadJSON(char *buf, uint32_t buf_size);

#ifdef __cplusplus
}
#endif

#endif /* SENSORS_H */
