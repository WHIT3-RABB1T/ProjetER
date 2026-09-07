/* sensors.c -- see sensors.h for the overview. */
#include "sensors.h"

#include <stdio.h>
#include <stdarg.h>

#include "b_u585i_iot02a_errno.h"
#include "b_u585i_iot02a_env_sensors.h"
#include "b_u585i_iot02a_motion_sensors.h"
#include "b_u585i_iot02a_light_sensor.h"
#include "b_u585i_iot02a_ranging_sensor.h"

/* Per-sensor init-success flags: set once in Sensors_Init(), read from
 * every Sensors_Read*JSON() below to decide whether there's anything to
 * report. A 0 here means that specific chip is simply left out of every
 * future reading -- one bad sensor never takes the others down with it. */
static uint8_t s_hts221_ok;
static uint8_t s_lps22hh_ok;
static uint8_t s_accel_gyro_ok;
static uint8_t s_mag_ok;
static uint8_t s_light_ok;
static uint8_t s_ranging_ok;

/* Appends printf-style output to buf[0..buf_size), tracking how many bytes
 * are "really" used so far (used), and returns the new used count. Never
 * writes past buf_size (snprintf's own bound), and once truncation starts
 * happening it stops asking snprintf to do any more formatting work (still
 * returns a used count clamped to buf_size so the caller can't overflow),
 * so a too-small buffer degrades to "truncated JSON" instead of undefined
 * behaviour -- every Sensors_Read*JSON()'s caller only ever uses the
 * returned length, never assumes every key made it in.
 *
 * Deliberately never passed a "%f"/"%e"/"%g" format: see the note in
 * sensors.h about this project's default nano.specs newlib not supporting
 * float in printf/vsnprintf. Every float sensor value that reaches this
 * function has already been split into whole/hundredths integers first. */
static uint32_t append(char *buf, uint32_t buf_size, uint32_t used, const char *fmt, ...)
{
    va_list args;
    int n;

    if (used >= buf_size)
    {
        return buf_size;
    }

    va_start(args, fmt);
    n = vsnprintf(&buf[used], buf_size - used, fmt, args);
    va_end(args);

    if (n < 0)
    {
        /* Formatting error: leave used where it was, buffer stays valid. */
        return used;
    }

    used += (uint32_t)n;
    return (used > buf_size) ? buf_size : used;
}

/* val*100 rounded to the nearest integer, split into a whole part and a
 * 2-digit fractional part -- e.g. -3.456 -> whole=-3, frac=46 (rendered
 * "-3.46" by the caller). See the "no %f" note above. */
static void float_to_fixed2(float val, long *whole, long *frac)
{
    long scaled = (long)(val * 100.0f + ((val >= 0.0f) ? 0.5f : -0.5f));
    *whole = scaled / 100;
    *frac = scaled % 100;
    if (*frac < 0)
    {
        *frac = -(*frac);
    }
}

/* NUL-terminates buf at the lesser of `used` and buf_size-1, mirroring
 * append()'s own overflow discipline -- every Sensors_Read*JSON() below
 * calls this right before returning so the buffer is always a valid C
 * string even if the last append() got clamped mid-token. */
static void terminate(char *buf, uint32_t buf_size, uint32_t used)
{
    buf[(used < buf_size) ? used : (buf_size - 1)] = '\0';
}

void Sensors_Init(void)
{
    int32_t ret;

    printf("Sensors: probing onboard I2C2 sensor suite...\r\n");

    /* --- Environmental: HTS221 (instance 0, temp+humidity) --- */
    ret = BSP_ENV_SENSOR_Init(0, ENV_TEMPERATURE | ENV_HUMIDITY);
    s_hts221_ok = (uint8_t)(ret == BSP_ERROR_NONE);
    printf("Sensors: HTS221  (temp+humidity)  init %s (%ld)\r\n", s_hts221_ok ? "OK" : "FAILED", (long)ret);
    if (s_hts221_ok)
    {
        BSP_ENV_SENSOR_Enable(0, ENV_TEMPERATURE);
        BSP_ENV_SENSOR_Enable(0, ENV_HUMIDITY);
    }

    /* --- Environmental: LPS22HH (instance 1, temp+pressure) --- */
    ret = BSP_ENV_SENSOR_Init(1, ENV_TEMPERATURE | ENV_PRESSURE);
    s_lps22hh_ok = (uint8_t)(ret == BSP_ERROR_NONE);
    printf("Sensors: LPS22HH (temp+pressure)  init %s (%ld)\r\n", s_lps22hh_ok ? "OK" : "FAILED", (long)ret);
    if (s_lps22hh_ok)
    {
        BSP_ENV_SENSOR_Enable(1, ENV_TEMPERATURE);
        BSP_ENV_SENSOR_Enable(1, ENV_PRESSURE);
    }

    /* --- Motion: ISM330DHCX (instance 0, accel+gyro) --- */
    ret = BSP_MOTION_SENSOR_Init(0, MOTION_GYRO | MOTION_ACCELERO);
    s_accel_gyro_ok = (uint8_t)(ret == BSP_ERROR_NONE);
    printf("Sensors: ISM330DHCX (accel+gyro)  init %s (%ld)\r\n", s_accel_gyro_ok ? "OK" : "FAILED", (long)ret);
    if (s_accel_gyro_ok)
    {
        BSP_MOTION_SENSOR_Enable(0, MOTION_GYRO);
        BSP_MOTION_SENSOR_Enable(0, MOTION_ACCELERO);
    }

    /* --- Motion: IIS2MDC (instance 1, magnetometer) --- */
    ret = BSP_MOTION_SENSOR_Init(1, MOTION_MAGNETO);
    s_mag_ok = (uint8_t)(ret == BSP_ERROR_NONE);
    printf("Sensors: IIS2MDC (magnetometer)   init %s (%ld)\r\n", s_mag_ok ? "OK" : "FAILED", (long)ret);
    if (s_mag_ok)
    {
        BSP_MOTION_SENSOR_Enable(1, MOTION_MAGNETO);
    }

    /* --- Light: VEML3235 (instance 0) --- */
    ret = BSP_LIGHT_SENSOR_Init(0);
    if (ret == BSP_ERROR_NONE)
    {
        ret = BSP_LIGHT_SENSOR_Start(0, LIGHT_SENSOR_MODE_CONTINUOUS);
    }
    s_light_ok = (uint8_t)(ret == BSP_ERROR_NONE);
    printf("Sensors: VEML3235 (light)         init %s (%ld)\r\n", s_light_ok ? "OK" : "FAILED", (long)ret);

    /* --- Ranging: VL53L5CX (instance = built-in/center) ---
     * By far the slowest and most fragile of the six: Init() alone
     * uploads an ~84 KB firmware image to the sensor's own RAM over I2C
     * (see vl53l5cx_api.c / vl53l5cx_buffers.h), so it goes last and every
     * step is logged individually. 4x4 profile (16 zones, not the full
     * 8x8/64) keeps each JSON reading a manageable size and this a
     * manageable amount of I2C traffic per poll. */
    ret = BSP_RANGING_SENSOR_Init(VL53L5A1_DEV_CENTER);
    printf("Sensors: VL53L5CX ranging Init()       -> %ld\r\n", (long)ret);
    if (ret == BSP_ERROR_NONE)
    {
        RANGING_SENSOR_ProfileConfig_t profile;
        profile.RangingProfile = RS_PROFILE_4x4_CONTINUOUS;
        profile.TimingBudget = 10;   /* ms */
        profile.Frequency = 2;       /* Hz */
        profile.EnableAmbient = 0;
        profile.EnableSignal = 0;
        ret = BSP_RANGING_SENSOR_ConfigProfile(VL53L5A1_DEV_CENTER, &profile);
        printf("Sensors: VL53L5CX ConfigProfile()      -> %ld\r\n", (long)ret);
    }
    if (ret == BSP_ERROR_NONE)
    {
        ret = BSP_RANGING_SENSOR_Start(VL53L5A1_DEV_CENTER, RS_MODE_BLOCKING_CONTINUOUS);
        printf("Sensors: VL53L5CX Start()              -> %ld\r\n", (long)ret);
    }
    s_ranging_ok = (uint8_t)(ret == BSP_ERROR_NONE);
    printf("Sensors: VL53L5CX (ranging)       init %s\r\n", s_ranging_ok ? "OK" : "FAILED");

    printf("Sensors: init done -- hts221=%d lps22hh=%d accel/gyro=%d mag=%d light=%d ranging=%d\r\n",
           s_hts221_ok, s_lps22hh_ok, s_accel_gyro_ok, s_mag_ok, s_light_ok, s_ranging_ok);
}

/* --- Per-category readers -- one per Sensors_Endpoints[] entry below. ---
 * Each is self-contained: reads only the chip(s) its category needs,
 * returns 0 (buf untouched) if none of them are up or all of today's reads
 * on them failed, otherwise writes one flat JSON object and returns its
 * length. */

/* {"hts221_c":F,"lps22hh_c":F} -- both chips independently measure
 * temperature; report whichever succeeded, omit whichever didn't. */
static uint32_t read_temperature(char *buf, uint32_t buf_size)
{
    uint32_t used = 0;
    uint8_t any = 0;
    long whole, frac;

    if (s_hts221_ok)
    {
        float temp_c = 0.0f;
        if (BSP_ENV_SENSOR_GetValue(0, ENV_TEMPERATURE, &temp_c) == BSP_ERROR_NONE)
        {
            float_to_fixed2(temp_c, &whole, &frac);
            used = append(buf, buf_size, used, "%s\"hts221_c\":%ld.%02ld", any ? "," : "{", whole, frac);
            any = 1;
        }
    }
    if (s_lps22hh_ok)
    {
        float temp_c = 0.0f;
        if (BSP_ENV_SENSOR_GetValue(1, ENV_TEMPERATURE, &temp_c) == BSP_ERROR_NONE)
        {
            float_to_fixed2(temp_c, &whole, &frac);
            used = append(buf, buf_size, used, "%s\"lps22hh_c\":%ld.%02ld", any ? "," : "{", whole, frac);
            any = 1;
        }
    }
    if (!any)
    {
        return 0;
    }
    used = append(buf, buf_size, used, "}");
    terminate(buf, buf_size, used);
    return used;
}

/* {"hts221_rh":F} */
static uint32_t read_humidity(char *buf, uint32_t buf_size)
{
    uint32_t used = 0;
    float hum_rh = 0.0f;
    long whole, frac;

    if (!s_hts221_ok || (BSP_ENV_SENSOR_GetValue(0, ENV_HUMIDITY, &hum_rh) != BSP_ERROR_NONE))
    {
        return 0;
    }
    float_to_fixed2(hum_rh, &whole, &frac);
    used = append(buf, buf_size, used, "{\"hts221_rh\":%ld.%02ld}", whole, frac);
    terminate(buf, buf_size, used);
    return used;
}

/* {"lps22hh_hpa":F} */
static uint32_t read_pressure(char *buf, uint32_t buf_size)
{
    uint32_t used = 0;
    float press_hpa = 0.0f;
    long whole, frac;

    if (!s_lps22hh_ok || (BSP_ENV_SENSOR_GetValue(1, ENV_PRESSURE, &press_hpa) != BSP_ERROR_NONE))
    {
        return 0;
    }
    float_to_fixed2(press_hpa, &whole, &frac);
    used = append(buf, buf_size, used, "{\"lps22hh_hpa\":%ld.%02ld}", whole, frac);
    terminate(buf, buf_size, used);
    return used;
}

/* Shared by the three ISM330DHCX/IIS2MDC axis readers below --
 * {"x":N,"y":N,"z":N}, plain int32_t (mg / mdps / mgauss), no float
 * involved at all. */
static uint32_t read_axes(char *buf, uint32_t buf_size, uint8_t sensor_ok,
                           uint32_t instance, uint32_t function)
{
    uint32_t used = 0;
    BSP_MOTION_SENSOR_Axes_t axes;

    if (!sensor_ok || (BSP_MOTION_SENSOR_GetAxes(instance, function, &axes) != BSP_ERROR_NONE))
    {
        return 0;
    }
    used = append(buf, buf_size, used, "{\"x\":%ld,\"y\":%ld,\"z\":%ld}",
                  (long)axes.xval, (long)axes.yval, (long)axes.zval);
    terminate(buf, buf_size, used);
    return used;
}

static uint32_t read_accelerometer(char *buf, uint32_t buf_size)
{
    return read_axes(buf, buf_size, s_accel_gyro_ok, 0, MOTION_ACCELERO);
}

static uint32_t read_gyroscope(char *buf, uint32_t buf_size)
{
    return read_axes(buf, buf_size, s_accel_gyro_ok, 0, MOTION_GYRO);
}

static uint32_t read_magnetometer(char *buf, uint32_t buf_size)
{
    return read_axes(buf, buf_size, s_mag_ok, 1, MOTION_MAGNETO);
}

/* {"als_raw":N,"white_raw":N} -- raw register counts, not calibrated lux
 * (BSP_LIGHT_SENSOR_GetValues() returns the two channels straight from the
 * sensor with no lux conversion applied); still real per-poll ambient-light
 * data either way. */
static uint32_t read_light(char *buf, uint32_t buf_size)
{
    uint32_t used = 0;
    uint32_t light_values[2] = { 0, 0 };

    if (!s_light_ok || (BSP_LIGHT_SENSOR_GetValues(0, light_values) != BSP_ERROR_NONE))
    {
        return 0;
    }
    used = append(buf, buf_size, used, "{\"als_raw\":%lu,\"white_raw\":%lu}",
                  (unsigned long)light_values[0], (unsigned long)light_values[1]);
    terminate(buf, buf_size, used);
    return used;
}

/* {"zones":[N,...]} -- one entry per zone (4x4 profile => 16 zones,
 * row-major), first target's distance in mm, or -1 where the BSP reports
 * that zone's target status as not-OK (BSP's own Status[] convention: 0 =
 * OK, per b_u585i_iot02a_ranging_sensor.h). */
static uint32_t read_ranging(char *buf, uint32_t buf_size)
{
    uint32_t used = 0;
    uint32_t i;
    /* static: RANGING_SENSOR_Result_t's ZoneResult[] array is always sized
     * for the max 64 zones (RANGING_SENSOR_MAX_NB_ZONES,
     * VL53L5CX_RESOLUTION_8X8) regardless of which profile is actually
     * running -- around 1.3-1.5 KB -- so this stays off the caller's
     * stack for the same reason as app_netxduo.c's "body" buffer. Only
     * this one thread ever calls into sensors.c. */
    static RANGING_SENSOR_Result_t result;

    if (!s_ranging_ok || (BSP_RANGING_SENSOR_GetDistance(VL53L5A1_DEV_CENTER, &result) != BSP_ERROR_NONE))
    {
        return 0;
    }

    used = append(buf, buf_size, used, "{\"zones\":[");
    for (i = 0; i < result.NumberOfZones; i++)
    {
        long distance_mm = -1;

        if (result.ZoneResult[i].Status[0] == 0U)
        {
            distance_mm = (long)result.ZoneResult[i].Distance[0];
        }
        used = append(buf, buf_size, used, "%s%ld", (i == 0U) ? "" : ",", distance_mm);
    }
    used = append(buf, buf_size, used, "]}");
    terminate(buf, buf_size, used);
    return used;
}

const Sensors_Endpoint_t Sensors_Endpoints[] =
{
    { "/temperature",   read_temperature   },
    { "/humidity",      read_humidity      },
    { "/pressure",      read_pressure      },
    { "/accelerometer", read_accelerometer },
    { "/gyroscope",     read_gyroscope     },
    { "/magnetometer",  read_magnetometer  },
    { "/light",         read_light         },
    { "/ranging",       read_ranging       },
};

const uint32_t Sensors_EndpointCount = sizeof(Sensors_Endpoints) / sizeof(Sensors_Endpoints[0]);
