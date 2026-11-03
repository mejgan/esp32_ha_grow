#include <string.h>

#include "check.h"
#include "growbox_logic.h"

static gb_settings_t base_settings(void)
{
    return (gb_settings_t){
        .enabled = true,
        .auto_mode = true,
        .manual_on = false,
        .manual_level = 200,
        .on_hour = 6,
        .on_minute = 30,
        .off_hour = 0,
        .off_minute = 0,
        .auto_brightness_pct = 50,
        .max_temp_c = 35,
    };
}

static void test_light_window(void)
{
    /* Plain window 06:30-18:15. */
    CHECK(!gb_in_light_window(6 * 60 + 29, 390, 1095));
    CHECK(gb_in_light_window(390, 390, 1095));
    CHECK(gb_in_light_window(1094, 390, 1095));
    CHECK(!gb_in_light_window(1095, 390, 1095));
    /* Wrapping past midnight 20:00-04:00. */
    CHECK(gb_in_light_window(23 * 60, 1200, 240));
    CHECK(gb_in_light_window(0, 1200, 240));
    CHECK(gb_in_light_window(239, 1200, 240));
    CHECK(!gb_in_light_window(240, 1200, 240));
    CHECK(!gb_in_light_window(1199, 1200, 240));
    /* Equal on/off: never. */
    CHECK(!gb_in_light_window(600, 600, 600));
    /* Off at midnight (06:30-00:00). */
    CHECK(gb_in_light_window(1439, 390, 0));
    CHECK(!gb_in_light_window(0, 390, 0));
}

static void test_light_decide(void)
{
    gb_settings_t s = base_settings();
    gb_light_output_t o = gb_light_decide(&s, true, 12 * 60, false);
    CHECK(o.on);
    CHECK_EQ(o.level, 127);
    o = gb_light_decide(&s, true, 6 * 60 + 29, false);
    CHECK(!o.on);
    o = gb_light_decide(&s, false, 12 * 60, false); /* no clock in auto mode: off */
    CHECK(!o.on);
    o = gb_light_decide(&s, true, 12 * 60, true); /* heat lock */
    CHECK(!o.on);
    s.enabled = false;
    o = gb_light_decide(&s, true, 12 * 60, false);
    CHECK(!o.on);

    s = base_settings();
    s.auto_mode = false;
    s.manual_on = true;
    o = gb_light_decide(&s, false, 0, false);
    CHECK(o.on);
    CHECK_EQ(o.level, 200);
    o = gb_light_decide(&s, false, 0, true);
    CHECK(!o.on);
}

static void test_settings_clamp(void)
{
    gb_settings_t s;
    memset(&s, 0xFF, sizeof(s));
    gb_settings_clamp(&s);
    uint8_t raw;
    memcpy(&raw, &s.enabled, 1);
    CHECK_EQ(raw, 1);
    CHECK_EQ(s.manual_level, 254);
    CHECK_EQ(s.on_hour, 23);
    CHECK_EQ(s.on_minute, 59);
    CHECK_EQ(s.off_hour, 23);
    CHECK_EQ(s.off_minute, 59);
    CHECK_EQ(s.auto_brightness_pct, 100);
    CHECK_EQ(s.max_temp_c, 50);
    memset(&s, 0, sizeof(s));
    gb_settings_clamp(&s);
    CHECK_EQ(s.manual_level, 1);
    CHECK_EQ(s.auto_brightness_pct, 1);
    CHECK_EQ(s.max_temp_c, 20);
}

static void test_conversions(void)
{
    CHECK_EQ(gb_minute_of_day(0), 0);
    CHECK_EQ(gb_minute_of_day(86399), 1439);
    CHECK_EQ(gb_minute_of_day(86400 * 3 + 6 * 3600 + 30 * 60 + 59), 390);
    CHECK_EQ(gb_minute_of_day(-5), 0);

    CHECK_EQ(gb_pct_to_level(0), 3);
    CHECK_EQ(gb_pct_to_level(100), 254);
    CHECK_EQ(gb_pct_to_level(200), 254);
    for (unsigned pct = 1; pct <= 100; pct++) {
        CHECK_EQ(gb_level_to_pct(gb_pct_to_level((uint8_t)pct)), pct);
    }
    CHECK_EQ(gb_level_to_pct(0), 0);
    CHECK_EQ(gb_level_to_pct(1), 1);
    CHECK_EQ(gb_level_to_pct(254), 100);

    CHECK_EQ(gb_soil_pct_x100(2700, 2700, 1300), 0);
    CHECK_EQ(gb_soil_pct_x100(1300, 2700, 1300), 10000);
    CHECK_EQ(gb_soil_pct_x100(2000, 2700, 1300), 5000);
    CHECK_EQ(gb_soil_pct_x100(3000, 2700, 1300), 0);
    CHECK_EQ(gb_soil_pct_x100(500, 2700, 1300), 10000);
    CHECK_EQ(gb_soil_pct_x100(500, 1000, 1000), 0);

    CHECK_NEAR(gb_temt6000_lux(1000), 200.0, 1e-3);
    CHECK_EQ(gb_lux_to_zcl(0.5f), 0);
    CHECK_EQ(gb_lux_to_zcl(1.0f), 1);
    CHECK_EQ(gb_lux_to_zcl(1000.0f), 30001);
    CHECK_EQ(gb_lux_to_zcl(1e9f), 0xFFFE);
}

static void test_heat_lock(void)
{
    CHECK(!gb_heat_lock_update(false, true, 35.0f, 35.0f));
    CHECK(gb_heat_lock_update(false, true, 35.1f, 35.0f));
    CHECK(gb_heat_lock_update(true, true, 34.0f, 35.0f));  /* inside hysteresis: stays locked */
    CHECK(!gb_heat_lock_update(true, true, 33.0f, 35.0f)); /* max - 2: unlocked */
    CHECK(gb_heat_lock_update(true, false, 0.0f, 35.0f));  /* invalid reading keeps state */
    CHECK(!gb_heat_lock_update(false, false, 99.0f, 35.0f));

    bool valid[3] = {false, true, true};
    float temp[3] = {0, 25.0f, 31.0f};
    float out = 0;
    CHECK(gb_floor_control_temp(valid, temp, 3, 1, &out));
    CHECK_NEAR(out, 25.0, 1e-6);
    CHECK(gb_floor_control_temp(valid, temp, 3, 0, &out)); /* own sensor dead: hottest other floor */
    CHECK_NEAR(out, 31.0, 1e-6);
    bool none[3] = {false, false, false};
    CHECK(!gb_floor_control_temp(none, temp, 3, 0, &out));
}

static void test_bmp180(void)
{
    /* Worked example from the BMP180 datasheet. */
    const gb_bmp180_calib_t c = {
        .ac1 = 408, .ac2 = -72, .ac3 = -14383, .ac4 = 32741, .ac5 = 32757, .ac6 = 23153,
        .b1 = 6190, .b2 = 4, .mb = -32768, .mc = -8711, .md = 2868,
    };
    int32_t t10, pa;
    gb_bmp180_compensate(&c, 27898, 23843, 0, &t10, &pa);
    CHECK_EQ(t10, 150);
    CHECK_EQ(pa, 69964);
}

static void test_aht10_dht11(void)
{
    const uint8_t raw[6] = {0x1C, 0x80, 0x00, 0x08, 0x00, 0x00};
    float t = 0, rh = 0;
    CHECK(gb_aht10_convert(raw, &t, &rh));
    CHECK_NEAR(rh, 50.0, 1e-3);
    CHECK_NEAR(t, 50.0, 1e-3);
    const uint8_t busy[6] = {0x9C, 0x80, 0x00, 0x08, 0x00, 0x00};
    CHECK(!gb_aht10_convert(busy, &t, &rh));

    const uint8_t frame[5] = {55, 0, 24, 5, 84};
    CHECK(gb_dht11_decode(frame, &t, &rh));
    CHECK_NEAR(rh, 55.0, 1e-3);
    CHECK_NEAR(t, 24.5, 1e-3);
    const uint8_t bad[5] = {55, 0, 24, 5, 85};
    CHECK(!gb_dht11_decode(bad, &t, &rh));
    const uint8_t zero[5] = {0};
    CHECK(!gb_dht11_decode(zero, &t, &rh));
    const uint8_t neg[5] = {40, 0, 3, 0x82, (40 + 3 + 0x82) & 0xFF};
    CHECK(gb_dht11_decode(neg, &t, &rh));
    CHECK_NEAR(t, -3.2, 1e-3);
}

/* Builds the line pulses of a DHT11 transfer of `frame`, optionally without the host start pulse/response. */
static size_t dht_pulses(const uint8_t frame[5], bool with_start, gb_pulse_t *p)
{
    size_t n = 0;
    if (with_start) {
        p[n++] = (gb_pulse_t){0, 20000}; /* host start */
        p[n++] = (gb_pulse_t){1, 30};
        p[n++] = (gb_pulse_t){0, 80}; /* sensor response */
        p[n++] = (gb_pulse_t){1, 80};
    }
    for (int i = 0; i < 40; i++) {
        bool one = (frame[i / 8] >> (7 - i % 8)) & 1;
        p[n++] = (gb_pulse_t){0, (uint16_t)(48 + i % 5)};
        p[n++] = (gb_pulse_t){1, (uint16_t)(one ? 68 + i % 6 : 23 + i % 5)};
    }
    p[n++] = (gb_pulse_t){0, 52}; /* end of frame, then the line idles high */
    return n;
}

static void test_dht11_pulses(void)
{
    const uint8_t frame[5] = {0x37, 0x00, 0x18, 0x05, 0x54};
    gb_pulse_t p[100];
    uint8_t out[5];

    size_t n = dht_pulses(frame, true, p);
    CHECK(gb_dht11_frame_from_pulses(p, n, out));
    CHECK(memcmp(out, frame, 5) == 0);

    n = dht_pulses(frame, false, p); /* start and response missed */
    CHECK(gb_dht11_frame_from_pulses(p, n, out));
    CHECK(memcmp(out, frame, 5) == 0);

    /* Trailing long idle high recorded by the capture must not count as a bit. */
    n = dht_pulses(frame, true, p);
    p[n++] = (gb_pulse_t){1, 1000};
    CHECK(gb_dht11_frame_from_pulses(p, n, out));
    CHECK(memcmp(out, frame, 5) == 0);

    /* Zero-length pulses (RMT end markers) are ignored. */
    n = dht_pulses(frame, true, p);
    p[n++] = (gb_pulse_t){1, 0};
    CHECK(gb_dht11_frame_from_pulses(p, n, out));
    CHECK(memcmp(out, frame, 5) == 0);

    /* Truncated transfer. */
    n = dht_pulses(frame, false, p);
    CHECK(!gb_dht11_frame_from_pulses(p, n - 12, out));
    CHECK(!gb_dht11_frame_from_pulses(p, 0, out));
}

/* ---------- OTA parser ---------- */

typedef struct {
    uint8_t data[4096];
    size_t len;
    int fail_after;
} sink_t;

static int sink_cb(void *ctx, const uint8_t *data, size_t len)
{
    sink_t *s = ctx;
    if (s->fail_after >= 0 && s->len + len > (size_t)s->fail_after) {
        return -1;
    }
    memcpy(s->data + s->len, data, len);
    s->len += len;
    return 0;
}

static void wr16(uint8_t *b, uint16_t v)
{
    b[0] = v & 0xFF;
    b[1] = v >> 8;
}

static void wr32(uint8_t *b, uint32_t v)
{
    for (int i = 0; i < 4; i++) {
        b[i] = (v >> (8 * i)) & 0xFF;
    }
}

/* Zigbee OTA file: header (+extra bytes), image sub-element, a 10-byte signature sub-element. */
static size_t make_ota(uint8_t *f, const uint8_t *img, size_t img_len, uint16_t extra_hdr)
{
    uint16_t hdr_len = (uint16_t)(56 + extra_hdr);
    size_t total = hdr_len + 6 + img_len + 6 + 10;
    memset(f, 0, total);
    wr32(f, GB_OTA_FILE_ID);
    wr16(f + 4, 0x0100);
    wr16(f + 6, hdr_len);
    wr16(f + 8, extra_hdr ? 0x0004 : 0);
    wr16(f + 10, 0x131B);
    wr16(f + 12, 0x4742);
    wr32(f + 14, 0x02000000);
    wr16(f + 18, 0x0002);
    memcpy(f + 20, "GrowBox-C6", 10);
    wr32(f + 52, (uint32_t)total);
    size_t pos = hdr_len;
    wr16(f + pos, GB_OTA_TAG_UPGRADE);
    wr32(f + pos + 2, (uint32_t)img_len);
    memcpy(f + pos + 6, img, img_len);
    pos += 6 + img_len;
    wr16(f + pos, 0x0001);
    wr32(f + pos + 2, 10);
    memset(f + pos + 6, 0xEE, 10);
    return total;
}

static void test_ota_parser(void)
{
    uint8_t img[1500];
    for (size_t i = 0; i < sizeof(img); i++) {
        img[i] = (uint8_t)(i * 7 + 3);
    }
    static uint8_t file[2000];
    const size_t chunk_sizes[] = {1, 7, 64, 223, 5000};

    for (int extra = 0; extra <= 4; extra += 4) {
        size_t total = make_ota(file, img, sizeof(img), (uint16_t)extra);
        for (size_t c = 0; c < sizeof(chunk_sizes) / sizeof(chunk_sizes[0]); c++) {
            gb_ota_parser_t p;
            static sink_t s;
            memset(&s, 0, sizeof(s));
            s.fail_after = -1;
            gb_ota_parser_init(&p);
            gb_ota_err_t err = GB_OTA_OK;
            for (size_t off = 0; off < total && err == GB_OTA_OK; off += chunk_sizes[c]) {
                size_t n = total - off < chunk_sizes[c] ? total - off : chunk_sizes[c];
                err = gb_ota_parser_feed(&p, file + off, n, sink_cb, &s);
            }
            CHECK_EQ(err, GB_OTA_OK);
            CHECK(gb_ota_parser_done(&p));
            CHECK_EQ(p.manuf_code, 0x131B);
            CHECK_EQ(p.image_type, 0x4742);
            CHECK_EQ(p.file_version, 0x02000000);
            CHECK_EQ(p.image_bytes, sizeof(img));
            CHECK_EQ(s.len, sizeof(img));
            CHECK(memcmp(s.data, img, sizeof(img)) == 0);
            /* Bytes past the announced size are rejected. */
            uint8_t extra_byte = 0;
            CHECK_EQ(gb_ota_parser_feed(&p, &extra_byte, 1, sink_cb, &s), GB_OTA_ERR_OVERFLOW);
        }
    }

    size_t total = make_ota(file, img, sizeof(img), 0);
    gb_ota_parser_t p;
    static sink_t s;

    /* Not partly done before the end. */
    memset(&s, 0, sizeof(s));
    s.fail_after = -1;
    gb_ota_parser_init(&p);
    CHECK_EQ(gb_ota_parser_feed(&p, file, total - 1, sink_cb, &s), GB_OTA_OK);
    CHECK(!gb_ota_parser_done(&p));

    /* Wrong magic. */
    file[0] ^= 0xFF;
    gb_ota_parser_init(&p);
    CHECK_EQ(gb_ota_parser_feed(&p, file, total, sink_cb, &s), GB_OTA_ERR_MAGIC);
    file[0] ^= 0xFF;

    /* Sub-element longer than the file. */
    wr32(file + 56 + 2, 100000);
    gb_ota_parser_init(&p);
    CHECK_EQ(gb_ota_parser_feed(&p, file, total, sink_cb, &s), GB_OTA_ERR_HEADER);
    wr32(file + 56 + 2, sizeof(img));

    /* Header length below the minimum. */
    wr16(file + 6, 40);
    gb_ota_parser_init(&p);
    CHECK_EQ(gb_ota_parser_feed(&p, file, total, sink_cb, &s), GB_OTA_ERR_HEADER);
    wr16(file + 6, 56);

    /* Sink failure is reported. */
    memset(&s, 0, sizeof(s));
    s.fail_after = 100;
    gb_ota_parser_init(&p);
    CHECK_EQ(gb_ota_parser_feed(&p, file, total, sink_cb, &s), GB_OTA_ERR_SINK);
}

int main(void)
{
    test_light_window();
    test_light_decide();
    test_settings_clamp();
    test_conversions();
    test_heat_lock();
    test_bmp180();
    test_aht10_dht11();
    test_dht11_pulses();
    test_ota_parser();
    TEST_MAIN_END();
}
