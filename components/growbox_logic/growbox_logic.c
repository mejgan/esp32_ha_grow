#include "growbox_logic.h"

#include <math.h>
#include <string.h>

static uint8_t clamp_u8(uint8_t v, uint8_t lo, uint8_t hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

void gb_settings_clamp(gb_settings_t *s)
{
    /* Read the bools as bytes: a corrupted blob may hold values other than 0/1. */
    uint8_t raw;
    memcpy(&raw, &s->enabled, 1);
    s->enabled = raw != 0;
    memcpy(&raw, &s->auto_mode, 1);
    s->auto_mode = raw != 0;
    memcpy(&raw, &s->manual_on, 1);
    s->manual_on = raw != 0;
    s->manual_level = clamp_u8(s->manual_level, 1, 254);
    s->on_hour = clamp_u8(s->on_hour, 0, 23);
    s->on_minute = clamp_u8(s->on_minute, 0, 59);
    s->off_hour = clamp_u8(s->off_hour, 0, 23);
    s->off_minute = clamp_u8(s->off_minute, 0, 59);
    s->auto_brightness_pct = clamp_u8(s->auto_brightness_pct, 1, 100);
    s->max_temp_c = clamp_u8(s->max_temp_c, 20, 50);
}

bool gb_in_light_window(uint16_t now, uint16_t on, uint16_t off)
{
    if (on == off) {
        return false;
    }
    if (on < off) {
        return now >= on && now < off;
    }
    return now >= on || now < off;
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

uint8_t gb_level_to_pct(uint8_t level)
{
    if (level == 0) {
        return 0;
    }
    long pct = lround(level * 100 / 254.0);
    if (pct < 1) {
        pct = 1;
    } else if (pct > 100) {
        pct = 100;
    }
    return (uint8_t)pct;
}

gb_light_output_t gb_light_decide(const gb_settings_t *s, bool time_valid, uint16_t minute_of_day, bool heat_lock)
{
    gb_light_output_t out;
    if (s->auto_mode) {
        out.on = time_valid && gb_in_light_window(minute_of_day, gb_on_minute_of_day(s), gb_off_minute_of_day(s));
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

uint16_t gb_minute_of_day(int64_t secs)
{
    if (secs < 0) {
        secs = 0;
    }
    return (uint16_t)((secs % 86400) / 60);
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

/* Bit cell limits (us). A '0' high lasts ~26 us, a '1' ~70 us; the low before each bit ~50 us. */
#define DHT_LOW_MIN_US   20
#define DHT_LOW_MAX_US   120
#define DHT_HIGH_MIN_US  5
#define DHT_HIGH_MAX_US  120
#define DHT_ONE_THRESHOLD_US 48

bool gb_dht11_frame_from_pulses(const gb_pulse_t *p, size_t n, uint8_t frame[5])
{
    /* Ring of the last 40 high durations that followed a plausible low. */
    uint16_t highs[40];
    size_t count = 0;
    for (size_t i = 1; i < n; i++) {
        const gb_pulse_t *lo = &p[i - 1];
        const gb_pulse_t *hi = &p[i];
        if (lo->level != 0 || hi->level != 1) {
            continue;
        }
        if (lo->us < DHT_LOW_MIN_US || lo->us > DHT_LOW_MAX_US || hi->us < DHT_HIGH_MIN_US ||
            hi->us > DHT_HIGH_MAX_US) {
            continue;
        }
        highs[count % 40] = hi->us;
        count++;
    }
    if (count < 40) {
        return false;
    }
    memset(frame, 0, 5);
    for (size_t b = 0; b < 40; b++) {
        uint16_t us = highs[(count - 40 + b) % 40];
        frame[b / 8] = (uint8_t)(frame[b / 8] << 1);
        if (us > DHT_ONE_THRESHOLD_US) {
            frame[b / 8] |= 1;
        }
    }
    return true;
}

/* ---------- Zigbee OTA file parser ---------- */

enum {
    OTA_S_HEADER,      /* collecting the first 56 header bytes */
    OTA_S_HEADER_SKIP, /* optional header fields */
    OTA_S_SUB_HEADER,  /* 6-byte sub-element tag + length */
    OTA_S_SUB_DATA,
    OTA_S_DONE,
};

static uint16_t rd16(const uint8_t *b)
{
    return (uint16_t)(b[0] | (b[1] << 8));
}

static uint32_t rd32(const uint8_t *b)
{
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

void gb_ota_parser_init(gb_ota_parser_t *p)
{
    memset(p, 0, sizeof(*p));
    p->state = OTA_S_HEADER;
}

bool gb_ota_parser_done(const gb_ota_parser_t *p)
{
    return p->state == OTA_S_DONE;
}

/* Called whenever an element (header or sub-element) ends; decides what comes next. */
static void ota_next_element(gb_ota_parser_t *p)
{
    p->fill = 0;
    p->state = p->pos >= p->total_size ? OTA_S_DONE : OTA_S_SUB_HEADER;
}

gb_ota_err_t gb_ota_parser_feed(gb_ota_parser_t *p, const uint8_t *data, size_t len, gb_ota_sink_t sink, void *ctx)
{
    while (len > 0) {
        switch (p->state) {
        case OTA_S_HEADER: {
            size_t n = GB_OTA_HEADER_MIN - p->fill;
            n = n < len ? n : len;
            memcpy(p->buf + p->fill, data, n);
            p->fill += n;
            p->pos += n;
            data += n;
            len -= n;
            if (p->fill < GB_OTA_HEADER_MIN) {
                break;
            }
            if (rd32(p->buf) != GB_OTA_FILE_ID) {
                return GB_OTA_ERR_MAGIC;
            }
            p->header_len = rd16(p->buf + 6);
            p->manuf_code = rd16(p->buf + 10);
            p->image_type = rd16(p->buf + 12);
            p->file_version = rd32(p->buf + 14);
            p->total_size = rd32(p->buf + 52);
            if (p->header_len < GB_OTA_HEADER_MIN || p->total_size < p->header_len) {
                return GB_OTA_ERR_HEADER;
            }
            p->remaining = p->header_len - GB_OTA_HEADER_MIN;
            if (p->remaining > 0) {
                p->state = OTA_S_HEADER_SKIP;
            } else {
                ota_next_element(p);
            }
        } break;
        case OTA_S_HEADER_SKIP: {
            size_t n = p->remaining < len ? p->remaining : len;
            p->remaining -= n;
            p->pos += n;
            data += n;
            len -= n;
            if (p->remaining == 0) {
                ota_next_element(p);
            }
        } break;
        case OTA_S_SUB_HEADER: {
            size_t n = 6u - p->fill;
            n = n < len ? n : len;
            memcpy(p->buf + p->fill, data, n);
            p->fill += n;
            p->pos += n;
            data += n;
            len -= n;
            if (p->fill < 6) {
                break;
            }
            p->tag = rd16(p->buf);
            p->remaining = rd32(p->buf + 2);
            if (p->pos > p->total_size || p->remaining > p->total_size - p->pos) {
                return GB_OTA_ERR_HEADER;
            }
            if (p->remaining > 0) {
                p->state = OTA_S_SUB_DATA;
            } else {
                ota_next_element(p);
            }
        } break;
        case OTA_S_SUB_DATA: {
            size_t n = p->remaining < len ? p->remaining : len;
            if (p->tag == GB_OTA_TAG_UPGRADE) {
                if (sink != NULL && sink(ctx, data, n) != 0) {
                    return GB_OTA_ERR_SINK;
                }
                p->image_bytes += n;
            }
            p->remaining -= n;
            p->pos += n;
            data += n;
            len -= n;
            if (p->remaining == 0) {
                ota_next_element(p);
            }
        } break;
        default:
            return GB_OTA_ERR_OVERFLOW;
        }
    }
    return GB_OTA_OK;
}
