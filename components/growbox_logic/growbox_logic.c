#include "growbox_logic.h"

#include <math.h>

bool gb_in_light_window(uint8_t hour, uint8_t on_hour, uint8_t off_hour)
{
    if (on_hour == off_hour) {
        return false;
    }
    if (on_hour < off_hour) {
        return hour >= on_hour && hour < off_hour;
    }
    return hour >= on_hour || hour < off_hour;
}

bool gb_heat_lock_update(bool locked, bool temp_valid, float temp_c, float max_temp_c)
{
    if (!temp_valid) {
        return locked;
    }
    if (temp_c > max_temp_c) {
        return true;
    }
    if (temp_c <= max_temp_c - GB_HEAT_HYSTERESIS_C) {
        return false;
    }
    return locked;
}

bool gb_floor_control_temp(const bool *valid, const float *temp_c, uint8_t count, uint8_t floor, float *out)
{
    if (floor < count && valid[floor]) {
        *out = temp_c[floor];
        return true;
    }
    bool found = false;
    for (uint8_t i = 0; i < count; i++) {
        if (valid[i] && (!found || temp_c[i] > *out)) {
            *out = temp_c[i];
            found = true;
        }
    }
    return found;
}

uint8_t gb_pct_to_level(uint8_t pct)
{
    if (pct < 1) {
        pct = 1;
    } else if (pct > 100) {
        pct = 100;
    }
    long level = lround(pct * 254 / 100.0);
    return (uint8_t)(level < 1 ? 1 : level);
}

gb_light_output_t gb_light_decide(const gb_settings_t *s, bool time_valid, uint8_t hour, bool heat_lock)
{
    gb_light_output_t out;
    if (s->auto_mode) {
        out.on = time_valid && gb_in_light_window(hour, s->on_hour, s->off_hour);
        out.level = gb_pct_to_level(s->auto_brightness_pct);
    } else {
        out.on = s->manual_on;
        out.level = s->manual_level ? s->manual_level : 254;
    }
    if (heat_lock || !s->enabled) {
        out.on = false;
    }
    return out;
}

uint16_t gb_soil_pct_x100(int mv, int dry_mv, int wet_mv)
{
    if (dry_mv == wet_mv) {
        return 0;
    }
    long pct = (long)(dry_mv - mv) * 10000 / (dry_mv - wet_mv);
    if (pct < 0) {
        pct = 0;
    } else if (pct > 10000) {
        pct = 10000;
    }
    return (uint16_t)pct;
}

float gb_temt6000_lux(int mv)
{
    return mv * 0.2f;
}

uint16_t gb_lux_to_zcl(float lux)
{
    if (lux < 1.0f) {
        return 0;
    }
    long v = lround(10000.0 * log10(lux) + 1.0);
    return (uint16_t)(v > 0xFFFE ? 0xFFFE : v);
}

void gb_bmp180_compensate(const gb_bmp180_calib_t *c, int32_t ut, int32_t up, uint8_t oss,
                          int32_t *temp_x10, int32_t *pressure_pa)
{
    int32_t x1 = ((ut - (int32_t)c->ac6) * (int32_t)c->ac5) >> 15;
    int32_t x2 = ((int32_t)c->mc << 11) / (x1 + c->md);
    int32_t b5 = x1 + x2;
    *temp_x10 = (b5 + 8) >> 4;

    int32_t b6 = b5 - 4000;
    x1 = (c->b2 * ((b6 * b6) >> 12)) >> 11;
    x2 = (c->ac2 * b6) >> 11;
    int32_t x3 = x1 + x2;
    int32_t b3 = ((((int32_t)c->ac1 * 4 + x3) << oss) + 2) / 4;
    x1 = (c->ac3 * b6) >> 13;
    x2 = (c->b1 * ((b6 * b6) >> 12)) >> 16;
    x3 = ((x1 + x2) + 2) >> 2;
    uint32_t b4 = ((uint32_t)c->ac4 * (uint32_t)(x3 + 32768)) >> 15;
    uint32_t b7 = ((uint32_t)up - (uint32_t)b3) * (uint32_t)(50000 >> oss);
    int32_t p;
    if (b7 < 0x80000000u) {
        p = (int32_t)((b7 * 2) / b4);
    } else {
        p = (int32_t)((b7 / b4) * 2);
    }
    x1 = (p >> 8) * (p >> 8);
    x1 = (x1 * 3038) >> 16;
    x2 = (-7357 * p) >> 16;
    p = p + ((x1 + x2 + 3791) >> 4);
    *pressure_pa = p;
}

bool gb_aht10_convert(const uint8_t raw[6], float *temp_c, float *rh)
{
    if (raw[0] & 0x80) {
        return false;
    }
    uint32_t h = ((uint32_t)raw[1] << 12) | ((uint32_t)raw[2] << 4) | (raw[3] >> 4);
    uint32_t t = ((uint32_t)(raw[3] & 0x0F) << 16) | ((uint32_t)raw[4] << 8) | raw[5];
    *rh = h * 100.0f / 1048576.0f;
    *temp_c = t * 200.0f / 1048576.0f - 50.0f;
    return true;
}

bool gb_dht11_decode(const uint8_t d[5], float *temp_c, float *rh)
{
    if ((d[0] | d[1] | d[2] | d[3] | d[4]) == 0) {
        return false;
    }
    if (((d[0] + d[1] + d[2] + d[3]) & 0xFF) != d[4]) {
        return false;
    }
    *rh = d[0] + d[1] * 0.1f;
    float t = d[2] + (d[3] & 0x7F) * 0.1f;
    *temp_c = (d[3] & 0x80) ? -t : t;
    return true;
}

uint8_t gb_hour_from_zcl_local(uint32_t zcl_local_secs)
{
    return (uint8_t)((zcl_local_secs / 3600u) % 24u);
}
