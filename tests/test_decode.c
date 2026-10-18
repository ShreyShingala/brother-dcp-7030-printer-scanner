/*
 * Offline tests for the scan-stream decoder (no scanner needed).
 */
#include "../core/internal.h"

#include <ImageIO/ImageIO.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
            failures++;                                                      \
        }                                                                    \
    } while (0)

typedef struct {
    uint8_t data[1 << 20];
    size_t len;
} stream_t;

static void put(stream_t *s, const void *p, size_t n)
{
    memcpy(s->data + s->len, p, n);
    s->len += n;
}

static void block(stream_t *s, uint8_t type, const uint8_t *payload, size_t n)
{
    uint8_t h[3] = { type, (uint8_t)(n & 0xff), (uint8_t)(n >> 8) };
    put(s, h, 3);
    put(s, payload, n);
}

static void test_rlength(void)
{
    const uint8_t src[] = { 0x02, 'a', 'b', 'c', 0xfe, 'x', 0x81, 'y' };
    uint8_t *dst = NULL;
    size_t len = 0, cap = 0;
    size_t used = br_rlength_decode(src, sizeof(src), &dst, &len, &cap);
    CHECK(used == sizeof(src));
    CHECK(len == 3 + 3 + 128);
    CHECK(memcmp(dst, "abcxxx", 6) == 0);
    CHECK(dst[6] == 'y' && dst[133] == 'y');

    /* Truncated literal run is left unconsumed. */
    len = 0;
    const uint8_t trunc[] = { 0x03, 'a', 'b' };
    CHECK(br_rlength_decode(trunc, sizeof(trunc), &dst, &len, &cap) == 0);
    free(dst);
}

static void test_gray_rlength_stream(void)
{
    static stream_t s;
    s.len = 0;
    /* line 1: sixteen 0x10 bytes as one repeat run (0xf1 -> 16 copies) */
    const uint8_t l1[] = { 0xf1, 0x10 };
    block(&s, 0x42, l1, sizeof(l1));
    /* line 2: literal 0..15 */
    uint8_t l2[17];
    l2[0] = 15;
    for (int i = 0; i < 16; i++)
        l2[1 + i] = (uint8_t)(i * 16);
    block(&s, 0x42, l2, sizeof(l2));
    put(&s, "\x80", 1);

    brscan_options o;
    brscan_default_options(&o);
    o.mode = BRSCAN_MODE_GRAY;
    brscan_image img;
    CHECK(br_decode_stream(s.data, s.len, &o, 16, &img) == BRSCAN_OK);
    CHECK(img.width == 16 && img.height == 2 && img.channels == 1);
    CHECK(img.pixels[0] == 0x10 && img.pixels[15] == 0x10);
    CHECK(img.pixels[16] == 0 && img.pixels[31] == 240);
    brscan_image_free(&img);

    /* BW thresholds the same data. */
    o.mode = BRSCAN_MODE_BW;
    CHECK(br_decode_stream(s.data, s.len, &o, 16, &img) == BRSCAN_OK);
    CHECK(img.pixels[0] == 0 && img.pixels[31] == 255);
    brscan_image_free(&img);
}

static void test_raw_colour_stream(void)
{
    static stream_t s;
    s.len = 0;
    uint8_t r[16], g[16], b[16];
    for (int line = 0; line < 3; line++) {
        for (int i = 0; i < 16; i++) {
            r[i] = (uint8_t)(10 + line);
            g[i] = (uint8_t)(20 + line);
            b[i] = (uint8_t)(30 + i);
        }
        block(&s, 0x44, r, 16);
        block(&s, 0x48, g, 16);
        block(&s, 0x4c, b, 16);
    }
    put(&s, "\x82", 1);

    brscan_options o;
    brscan_default_options(&o);
    brscan_image img;
    CHECK(br_decode_stream(s.data, s.len, &o, 16, &img) == BRSCAN_OK);
    CHECK(img.width == 16 && img.height == 3 && img.channels == 4);
    const uint8_t *px = img.pixels + 2 * img.bytes_per_row + 5 * 4;   /* line 2, x 5 */
    CHECK(px[0] == 12 && px[1] == 22 && px[2] == 35);
    brscan_image_free(&img);
}

static void test_error_codes(void)
{
    brscan_options o;
    brscan_default_options(&o);
    brscan_image img;
    const uint8_t cover[] = { 0xc4 };
    CHECK(br_decode_stream(cover, 1, &o, 16, &img) == BRSCAN_ERR_COVER_OPEN);
    const uint8_t cancel[] = { 0x86 };
    CHECK(br_decode_stream(cancel, 1, &o, 16, &img) == BRSCAN_ERR_CANCELLED);
    const uint8_t junk[] = { 0x05 };
    CHECK(br_decode_stream(junk, 1, &o, 16, &img) == BRSCAN_ERR_PROTOCOL);
    const uint8_t unterminated[] = { 0x40, 0x02, 0x00, 1, 2 };
    CHECK(br_decode_stream(unterminated, sizeof(unterminated), &o, 2, &img)
          == BRSCAN_ERR_PROTOCOL);
}

static void test_jpeg_stream(void)
{
    /* Make a 64x32 JPEG with ImageIO, then send it split across 0x64 blocks. */
    brscan_image src = { .width = 64, .height = 32, .channels = 4, .dpi = 100,
                         .bytes_per_row = 64 * 4 };
    src.pixels = calloc(32, src.bytes_per_row);
    for (int i = 0; i < 64 * 32; i++) {
        src.pixels[i * 4 + 0] = 200;
        src.pixels[i * 4 + 3] = 255;
    }
    CGImageRef cg = brscan_image_create_cgimage(&src);
    CFMutableDataRef jpg = CFDataCreateMutable(NULL, 0);
    CGImageDestinationRef dst = CGImageDestinationCreateWithData(jpg, CFSTR("public.jpeg"), 1, NULL);
    CGImageDestinationAddImage(dst, cg, NULL);
    CHECK(CGImageDestinationFinalize(dst));
    CFRelease(dst);
    CGImageRelease(cg);
    free(src.pixels);

    static stream_t s;
    s.len = 0;
    const uint8_t *p = CFDataGetBytePtr(jpg);
    size_t n = (size_t)CFDataGetLength(jpg);
    for (size_t off = 0; off < n; off += 100) {
        size_t chunk = n - off < 100 ? n - off : 100;
        block(&s, 0x64, p + off, chunk);
    }
    put(&s, "\x80", 1);
    CFRelease(jpg);

    brscan_options o;
    brscan_default_options(&o);
    brscan_image img;
    CHECK(br_decode_stream(s.data, s.len, &o, 64, &img) == BRSCAN_OK);
    CHECK(img.width == 64 && img.height == 32 && img.channels == 4);
    CHECK(img.pixels[0] > 180 && img.pixels[1] < 40);   /* still red-ish */
    brscan_image_free(&img);
}

static void test_write_files(void)
{
    brscan_image img = { .width = 32, .height = 16, .channels = 1, .dpi = 100,
                         .bytes_per_row = 32 };
    img.pixels = calloc(16, 32);
    const char *dir = getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp";
    char path[1024];
    const char *exts[] = { "pdf", "png", "jpg" };
    brscan_format fmts[] = { BRSCAN_FORMAT_PDF, BRSCAN_FORMAT_PNG, BRSCAN_FORMAT_JPEG };
    for (int i = 0; i < 3; i++) {
        snprintf(path, sizeof(path), "%s/brscan-test.%s", dir, exts[i]);
        CHECK(brscan_write_file(&img, path, fmts[i]) == BRSCAN_OK);
        FILE *f = fopen(path, "rb");
        CHECK(f != NULL);
        if (f) {
            fseek(f, 0, SEEK_END);
            CHECK(ftell(f) > 0);
            fclose(f);
        }
        remove(path);
    }
    free(img.pixels);
}

static void test_rotate180(void)
{
    /* 3x3 gray (odd, so the middle row and pixel are exercised) and stride padding. */
    brscan_image img = { .width = 3, .height = 3, .channels = 1, .dpi = 100,
                         .bytes_per_row = 4 };
    uint8_t px[12] = { 1, 2, 3, 99, 4, 5, 6, 99, 7, 8, 9, 99 };
    img.pixels = px;
    brscan_image_rotate180(&img);
    const uint8_t want[12] = { 9, 8, 7, 99, 6, 5, 4, 99, 3, 2, 1, 99 };
    CHECK(memcmp(px, want, 12) == 0);

    /* 2x1 RGBX keeps channel order within each pixel. */
    brscan_image rgb = { .width = 2, .height = 1, .channels = 4, .dpi = 100,
                         .bytes_per_row = 8 };
    uint8_t c[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    rgb.pixels = c;
    brscan_image_rotate180(&rgb);
    const uint8_t wantc[8] = { 5, 6, 7, 8, 1, 2, 3, 4 };
    CHECK(memcmp(c, wantc, 8) == 0);
}

int main(void)
{
    test_rotate180();
    test_rlength();
    test_gray_rlength_stream();
    test_raw_colour_stream();
    test_error_codes();
    test_jpeg_stream();
    test_write_files();
    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("all decoder tests passed\n");
    return 0;
}
