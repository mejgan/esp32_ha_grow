/*
 * Round-trip check for tools/make_ota_image.py against the firmware's OTA parser:
 *   ota_check gen <app.bin>                                  writes a fake app image
 *   ota_check verify <app.bin> <file.ota> <manuf> <type> <version>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "growbox_logic.h"

static unsigned char *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        perror(path);
        exit(1);
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *buf = malloc((size_t)n + 1);
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
        perror(path);
        exit(1);
    }
    fclose(f);
    *len = (size_t)n;
    return buf;
}

typedef struct {
    unsigned char *data;
    size_t len;
} sink_t;

static int sink(void *ctx, const uint8_t *data, size_t len)
{
    sink_t *s = ctx;
    s->data = realloc(s->data, s->len + len);
    memcpy(s->data + s->len, data, len);
    s->len += len;
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[1], "gen") == 0) {
        FILE *f = fopen(argv[2], "wb");
        if (f == NULL) {
            perror(argv[2]);
            return 1;
        }
        fputc(0xE9, f); /* ESP image magic */
        for (int i = 1; i < 300001; i++) {
            fputc((i * 131 + 7) & 0xFF, f);
        }
        fclose(f);
        return 0;
    }
    if (argc != 7 || strcmp(argv[1], "verify") != 0) {
        fprintf(stderr, "usage: %s gen <bin> | verify <bin> <ota> <manuf> <type> <version>\n", argv[0]);
        return 2;
    }
    size_t app_len, ota_len;
    unsigned char *app = read_file(argv[2], &app_len);
    unsigned char *ota = read_file(argv[3], &ota_len);

    gb_ota_parser_t p;
    sink_t s = {0};
    gb_ota_parser_init(&p);
    /* Feed in 223-byte blocks, like the Zigbee transfer does. */
    for (size_t off = 0; off < ota_len; off += 223) {
        size_t n = ota_len - off < 223 ? ota_len - off : 223;
        gb_ota_err_t err = gb_ota_parser_feed(&p, ota + off, n, sink, &s);
        if (err != GB_OTA_OK) {
            fprintf(stderr, "parser error %d at offset %zu\n", err, off);
            return 1;
        }
    }
    int ok = gb_ota_parser_done(&p) && p.manuf_code == strtoul(argv[4], NULL, 0) &&
             p.image_type == strtoul(argv[5], NULL, 0) && p.file_version == strtoul(argv[6], NULL, 0) &&
             p.total_size == ota_len && s.len == app_len && memcmp(s.data, app, app_len) == 0;
    if (!ok) {
        fprintf(stderr, "mismatch: done=%d manuf=0x%04x type=0x%04x version=0x%08x size=%u image=%zu/%zu\n",
                gb_ota_parser_done(&p), p.manuf_code, p.image_type, p.file_version, p.total_size, s.len, app_len);
        return 1;
    }
    printf("OTA file round trip OK (%zu bytes)\n", ota_len);
    return 0;
}
