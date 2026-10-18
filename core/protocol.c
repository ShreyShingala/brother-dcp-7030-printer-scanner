/*
 * Brother "family 3" scan protocol (DCP-7030 and friends).
 *
 * Protocol knowledge comes from the SANE brother_mfp backend by Ralph Little
 * (sane-backends, branch brother_mfp_backend).
 */
#include "internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Session control requests (vendor, device, IN). */
#define REQ_START_SESSION   1
#define REQ_STOP_SESSION    2

/* Block header codes. */
#define BLK_GRAY_RAW        0x40
#define BLK_GRAY_RLENGTH    0x42
#define BLK_RED_RAW         0x44
#define BLK_GREEN_RAW       0x48
#define BLK_BLUE_RAW        0x4c
#define BLK_JPEG            0x64

#define BLK_SCAN_FINISHED   0x80
#define BLK_END_FRAME_MORE  0x81
#define BLK_END_OF_IMAGE    0x82
#define BLK_TRUNC_EARLY     0x83
#define BLK_CANCEL          0x86

#define BLK_QUERY_RESPONSE  0xc1
#define BLK_NO_DOCS         0xc2
#define BLK_PAPER_JAM       0xc3
#define BLK_COVER_OPEN      0xc4
#define BLK_SERVICE_ERR     0xc5
#define BLK_DEVICE_STUCK    0xc6
#define BLK_PROTOCOL_ERROR  0xd0

#define GLASS_WIDTH_MM      210.0
#define GLASS_HEIGHT_MM     295.0
#define MM_PER_INCH         25.4

#define READ_CHUNK          (64 * 1024)
#define DATA_IDLE_LIMIT_MS  60000   /* lamp warm-up can take a while */

/* ------------------------------------------------------------------ */
/* Logging                                                             */

int br_verbose = 0;
static char *dump_path = NULL;

void brscan_set_verbose(int verbose) { br_verbose = verbose; }

void brscan_set_dump_path(const char *path)
{
    free(dump_path);
    dump_path = path ? strdup(path) : NULL;
}

void br_log(const char *fmt, ...)
{
    if (!br_verbose)
        return;
    va_list ap;
    va_start(ap, fmt);
    fputs("brscan: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

void br_hexdump(const char *label, const uint8_t *data, size_t len)
{
    if (!br_verbose)
        return;
    fprintf(stderr, "brscan: %s (%zu bytes):", label, len);
    size_t n = len < 64 ? len : 64;
    for (size_t i = 0; i < n; i++)
        fprintf(stderr, " %02x", data[i]);
    fprintf(stderr, n < len ? " ...\n" : "\n");
}

const char *brscan_strerror(brscan_status st)
{
    switch (st) {
    case BRSCAN_OK:            return "OK";
    case BRSCAN_ERR_NOT_FOUND: return "Scanner not connected. Plug the DCP-7030 into USB and turn it on.";
    case BRSCAN_ERR_ACCESS:    return "The scanner is in use by another program.";
    case BRSCAN_ERR_USB:       return "USB communication error.";
    case BRSCAN_ERR_BUSY:      return "The scanner is busy. Wait for printing to finish or exit the menu on the printer.";
    case BRSCAN_ERR_COVER_OPEN:return "The scanner lid or printer cover is open.";
    case BRSCAN_ERR_JAMMED:    return "Paper jam.";
    case BRSCAN_ERR_NO_DOCS:   return "No document detected.";
    case BRSCAN_ERR_DEVICE:    return "The scanner reported an error. Turn the printer off and on again.";
    case BRSCAN_ERR_PROTOCOL:  return "Unexpected response from the scanner.";
    case BRSCAN_ERR_TIMEOUT:   return "The scanner stopped responding.";
    case BRSCAN_ERR_CANCELLED: return "Scan cancelled.";
    case BRSCAN_ERR_NOMEM:     return "Out of memory.";
    case BRSCAN_ERR_DECODE:    return "Could not decode the scanned image.";
    case BRSCAN_ERR_WRITE:     return "Could not save the file.";
    case BRSCAN_ERR_INVALID:   return "Invalid scan settings.";
    }
    return "Unknown error.";
}

static brscan_status status_from_code(uint8_t code)
{
    switch (code) {
    case BLK_NO_DOCS:        return BRSCAN_ERR_NO_DOCS;
    case BLK_PAPER_JAM:      return BRSCAN_ERR_JAMMED;
    case BLK_COVER_OPEN:     return BRSCAN_ERR_COVER_OPEN;
    case BLK_SERVICE_ERR:
    case BLK_DEVICE_STUCK:   return BRSCAN_ERR_DEVICE;
    default:                 return BRSCAN_ERR_PROTOCOL;
    }
}

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

/* ------------------------------------------------------------------ */
/* Session handling                                                    */

/* Returns OK if ready, BUSY if the scanner said not ready. */
static brscan_status session_request(br_usb *usb, uint8_t req)
{
    uint8_t resp[5] = {0};
    uint16_t got = 0;
    brscan_status st = br_usb_control_in(usb, req, 2, 0, resp, sizeof(resp), &got);
    if (st != BRSCAN_OK)
        return st;
    br_hexdump(req == REQ_START_SESSION ? "start session reply" : "stop session reply",
               resp, got);
    if (got != 5 || resp[0] != 0x05 || resp[1] != 0x10 || resp[3] != 0x02
        || (resp[2] != 0x01 && resp[2] != 0x02))
        return BRSCAN_ERR_PROTOCOL;
    if (resp[4] == 0x00)
        return BRSCAN_OK;
    if (resp[4] == 0x80)
        return BRSCAN_ERR_BUSY;
    return BRSCAN_ERR_PROTOCOL;
}

/* Discard anything the scanner still has queued. */
static void flush_input(br_usb *usb, unsigned max_ms)
{
    uint8_t buf[1024];
    double end = now_ms() + max_ms;
    while (now_ms() < end) {
        size_t got = 0;
        if (br_usb_read(usb, buf, sizeof(buf), &got, 100) != BRSCAN_OK || got == 0)
            break;
        br_hexdump("flushed", buf, got);
    }
}

static brscan_status session_start(br_usb *usb)
{
    brscan_status st = session_request(usb, REQ_START_SESSION);
    if (st == BRSCAN_ERR_BUSY) {
        /* A previous session may have been left open; close and retry. */
        br_log("scanner busy, resetting session");
        (void)session_request(usb, REQ_STOP_SESSION);
        st = session_request(usb, REQ_START_SESSION);
    }
    if (st != BRSCAN_OK)
        return st;
    flush_input(usb, 1000);
    return BRSCAN_OK;
}

static void session_stop(br_usb *usb)
{
    flush_input(usb, 2000);
    (void)session_request(usb, REQ_STOP_SESSION);
}

/* Send a command and wait for a single reply chunk. */
static brscan_status transact(br_usb *usb, const char *cmd, size_t cmd_len,
                              uint8_t *resp, size_t resp_cap, size_t *resp_len)
{
    brscan_status st = br_usb_write(usb, (const uint8_t *)cmd, cmd_len);
    if (st != BRSCAN_OK)
        return st;
    st = br_usb_read(usb, resp, resp_cap, resp_len, 8000);
    if (st != BRSCAN_OK)
        return st;
    if (*resp_len == 0)
        return BRSCAN_ERR_TIMEOUT;
    br_hexdump("reply", resp, *resp_len);
    return BRSCAN_OK;
}

static brscan_status run_query(br_usb *usb, char *report, size_t report_len)
{
    static const char q[] = "\x1b" "Q" "\n" "\x80";
    uint8_t resp[1024];
    size_t n = 0;
    brscan_status st = transact(usb, q, sizeof(q) - 1, resp, sizeof(resp), &n);
    if (st != BRSCAN_OK)
        return st;
    if (n < 3 || resp[0] != BLK_QUERY_RESPONSE)
        return (n >= 1 && (resp[0] & 0xc0) == 0xc0) ? status_from_code(resp[0])
                                                     : BRSCAN_ERR_PROTOCOL;
    if (report && report_len) {
        size_t off = (size_t)snprintf(report, report_len, "query reply (%zu bytes):", n);
        for (size_t i = 0; i < n && off + 4 < report_len; i++)
            off += (size_t)snprintf(report + off, report_len - off, " %02x", resp[i]);
    }
    return BRSCAN_OK;
}

brscan_status brscan_probe(char *report, size_t report_len)
{
    if (report && report_len)
        report[0] = '\0';
    br_usb *usb = NULL;
    brscan_status st = br_usb_open(&usb);
    if (st != BRSCAN_OK)
        return st;
    st = session_start(usb);
    if (st == BRSCAN_OK) {
        st = run_query(usb, report, report_len);
        session_stop(usb);
    }
    br_usb_close(usb);
    return st;
}

/* ------------------------------------------------------------------ */
/* Scan                                                                */

typedef struct {
    uint8_t *data;
    size_t len, cap;
} buf_t;

static int buf_append(buf_t *b, const uint8_t *src, size_t n)
{
    if (b->len + n > b->cap) {
        size_t cap = b->cap ? b->cap : 64 * 1024;
        while (cap < b->len + n)
            cap *= 2;
        uint8_t *p = realloc(b->data, cap);
        if (!p)
            return -1;
        b->data = p;
        b->cap = cap;
    }
    memcpy(b->data + b->len, src, n);
    b->len += n;
    return 0;
}

typedef struct {
    int width;          /* pixels per line requested */
    int crop_width;     /* usable width reported by the scanner (0 = no crop) */
    buf_t gray;         /* 8-bit gray stream */
    buf_t chan[3];      /* raw colour planes, one line per block */
    buf_t jpeg;         /* concatenated JPEG stream */
    buf_t block;        /* payload of the block being received */
    uint8_t block_type;
    size_t block_left;
    int in_block;
} decoder_t;

static void decoder_free(decoder_t *d)
{
    free(d->gray.data);
    for (int i = 0; i < 3; i++)
        free(d->chan[i].data);
    free(d->jpeg.data);
    free(d->block.data);
}

static size_t decoder_bytes(const decoder_t *d)
{
    return d->gray.len + d->chan[0].len + d->chan[1].len + d->chan[2].len + d->jpeg.len;
}

static brscan_status finish_block(decoder_t *d)
{
    const uint8_t *p = d->block.data;
    size_t n = d->block.len;
    int rc = 0;
    switch (d->block_type) {
    case BLK_GRAY_RAW:
        rc = buf_append(&d->gray, p, n);
        break;
    case BLK_GRAY_RLENGTH: {
        size_t used = br_rlength_decode(p, n, &d->gray.data, &d->gray.len, &d->gray.cap);
        if (used == (size_t)-1)
            return BRSCAN_ERR_NOMEM;
        if (used != n)
            br_log("rlength block: %zu of %zu bytes decoded", used, n);
        break;
    }
    case BLK_RED_RAW:
        rc = buf_append(&d->chan[0], p, n);
        break;
    case BLK_GREEN_RAW:
        rc = buf_append(&d->chan[1], p, n);
        break;
    case BLK_BLUE_RAW:
        rc = buf_append(&d->chan[2], p, n);
        break;
    case BLK_JPEG:
        rc = buf_append(&d->jpeg, p, n);
        break;
    default:
        br_log("ignoring unknown data block 0x%02x (%zu bytes)", d->block_type, n);
        break;
    }
    d->block.len = 0;
    return rc ? BRSCAN_ERR_NOMEM : BRSCAN_OK;
}

/*
 * Feed received bytes. Returns OK and sets *done when a terminator arrives.
 * *consumed tells how much of src was used (the rest needs more data).
 */
static brscan_status decoder_feed(decoder_t *d, const uint8_t *src, size_t len,
                                  size_t *consumed, uint8_t *terminator)
{
    size_t pos = 0;
    *terminator = 0;
    while (pos < len) {
        if (d->in_block) {
            size_t take = len - pos < d->block_left ? len - pos : d->block_left;
            if (buf_append(&d->block, src + pos, take))
                return BRSCAN_ERR_NOMEM;
            pos += take;
            d->block_left -= take;
            if (d->block_left == 0) {
                d->in_block = 0;
                brscan_status st = finish_block(d);
                if (st != BRSCAN_OK)
                    return st;
            }
            continue;
        }

        uint8_t h = src[pos];
        if ((h & 0xc0) == 0xc0) {
            br_log("scanner error code 0x%02x", h);
            *consumed = pos + 1;
            return status_from_code(h);
        }
        if (h & 0x80) {
            *terminator = h;
            *consumed = pos + 1;
            return BRSCAN_OK;
        }
        if (h & 0x40) {
            if (len - pos < 3)
                break;   /* need the rest of the header */
            d->block_type = h;
            d->block_left = (size_t)src[pos + 1] | ((size_t)src[pos + 2] << 8);
            pos += 3;
            if (d->block_left == 0) {
                brscan_status st = finish_block(d);
                if (st != BRSCAN_OK)
                    return st;
            } else {
                d->in_block = 1;
            }
            continue;
        }
        br_log("unexpected byte 0x%02x in data stream", h);
        *consumed = pos;
        return BRSCAN_ERR_PROTOCOL;
    }
    *consumed = pos;
    return BRSCAN_OK;
}

void brscan_default_options(brscan_options *o)
{
    memset(o, 0, sizeof(*o));
    o->mode = BRSCAN_MODE_COLOR;
    o->dpi = 300;
}

void brscan_image_free(brscan_image *img)
{
    if (!img)
        return;
    free(img->pixels);
    memset(img, 0, sizeof(*img));
}

static brscan_status build_image_full(decoder_t *d, const brscan_options *o, brscan_image *out);

static brscan_status build_image(decoder_t *d, const brscan_options *o, brscan_image *out)
{
    brscan_status st = build_image_full(d, o, out);
    /* Crop the 16-pixel alignment padding (rows keep their stride). */
    if (st == BRSCAN_OK && d->crop_width > 0 && d->crop_width < out->width)
        out->width = d->crop_width;
    return st;
}

static brscan_status build_image_full(decoder_t *d, const brscan_options *o, brscan_image *out)
{
    memset(out, 0, sizeof(*out));
    out->dpi = o->dpi;
    size_t w = (size_t)d->width;

    if (d->jpeg.len) {
        br_log("jpeg stream: %zu bytes", d->jpeg.len);
        return br_decode_jpeg(d->jpeg.data, d->jpeg.len, o->dpi, out);
    }

    if (d->chan[0].len || d->chan[1].len || d->chan[2].len) {
        size_t n = d->chan[0].len;
        if (d->chan[1].len < n) n = d->chan[1].len;
        if (d->chan[2].len < n) n = d->chan[2].len;
        size_t lines = n / w;
        br_log("raw colour: R=%zu G=%zu B=%zu bytes -> %zu lines",
               d->chan[0].len, d->chan[1].len, d->chan[2].len, lines);
        if (lines == 0)
            return BRSCAN_ERR_DECODE;
        out->width = (int)w;
        out->height = (int)lines;
        out->channels = 4;
        out->bytes_per_row = w * 4;
        out->pixels = malloc(out->bytes_per_row * lines);
        if (!out->pixels)
            return BRSCAN_ERR_NOMEM;
        for (size_t y = 0; y < lines; y++) {
            uint8_t *row = out->pixels + y * out->bytes_per_row;
            const uint8_t *r = d->chan[0].data + y * w;
            const uint8_t *g = d->chan[1].data + y * w;
            const uint8_t *b = d->chan[2].data + y * w;
            for (size_t x = 0; x < w; x++) {
                row[x * 4 + 0] = r[x];
                row[x * 4 + 1] = g[x];
                row[x * 4 + 2] = b[x];
                row[x * 4 + 3] = 0xff;
            }
        }
        return BRSCAN_OK;
    }

    if (d->gray.len) {
        size_t lines = d->gray.len / w;
        br_log("gray: %zu bytes -> %zu lines (remainder %zu)", d->gray.len, lines,
               d->gray.len % w);
        if (lines == 0)
            return BRSCAN_ERR_DECODE;
        out->width = (int)w;
        out->height = (int)lines;
        out->channels = 1;
        out->bytes_per_row = w;
        out->pixels = malloc(w * lines);
        if (!out->pixels)
            return BRSCAN_ERR_NOMEM;
        memcpy(out->pixels, d->gray.data, w * lines);
        if (o->mode == BRSCAN_MODE_BW) {
            for (size_t i = 0; i < w * lines; i++)
                out->pixels[i] = out->pixels[i] < 128 ? 0 : 255;
        }
        return BRSCAN_OK;
    }

    return BRSCAN_ERR_DECODE;
}

brscan_status br_decode_stream(const uint8_t *stream, size_t len, const brscan_options *o,
                               int width, brscan_image *out)
{
    decoder_t d;
    memset(&d, 0, sizeof(d));
    d.width = width;
    size_t pos = 0;
    brscan_status st = BRSCAN_OK;
    uint8_t term = 0;
    while (pos < len && !term) {
        size_t used = 0;
        st = decoder_feed(&d, stream + pos, len - pos, &used, &term);
        pos += used;
        if (st != BRSCAN_OK || used == 0)
            break;
    }
    if (st == BRSCAN_OK && !term)
        st = BRSCAN_ERR_PROTOCOL;   /* stream ended without a terminator */
    if (st == BRSCAN_OK && (term == BLK_TRUNC_EARLY || term == BLK_CANCEL))
        st = BRSCAN_ERR_CANCELLED;
    if (st == BRSCAN_OK)
        st = build_image(&d, o, out);
    decoder_free(&d);
    return st;
}

static int valid_dpi(int dpi)
{
    return dpi == 100 || dpi == 150 || dpi == 200 || dpi == 300 || dpi == 600;
}

/* Read scan data until a terminator. */
static brscan_status read_scan_data(br_usb *usb, decoder_t *d, size_t expected,
                                    brscan_progress_fn progress, void *ctx,
                                    volatile int *cancel, FILE *dump)
{
    uint8_t *rx = malloc(READ_CHUNK * 2);
    if (!rx)
        return BRSCAN_ERR_NOMEM;
    size_t rx_len = 0;
    brscan_status st = BRSCAN_OK;
    double last_data = now_ms();
    int cancel_sent = 0;

    for (;;) {
        if (cancel && *cancel && !cancel_sent) {
            static const char r[] = "\x1b" "R";
            br_log("cancelling");
            (void)br_usb_write(usb, (const uint8_t *)r, sizeof(r) - 1);
            cancel_sent = 1;
            last_data = now_ms();
        }

        size_t got = 0;
        st = br_usb_read(usb, rx + rx_len, READ_CHUNK, &got, 1000);
        if (st != BRSCAN_OK)
            break;
        if (got == 0) {
            double idle_limit = cancel_sent ? 10000 : DATA_IDLE_LIMIT_MS;
            if (now_ms() - last_data > idle_limit) {
                st = cancel_sent ? BRSCAN_ERR_CANCELLED : BRSCAN_ERR_TIMEOUT;
                break;
            }
            continue;
        }
        last_data = now_ms();
        if (dump)
            fwrite(rx + rx_len, 1, got, dump);
        rx_len += got;

        size_t used = 0;
        uint8_t term = 0;
        st = decoder_feed(d, rx, rx_len, &used, &term);
        memmove(rx, rx + used, rx_len - used);
        rx_len -= used;
        if (st != BRSCAN_OK)
            break;

        if (progress) {
            double f = expected ? (double)decoder_bytes(d) / (double)expected : -1;
            progress(f > 0.99 ? 0.99 : f, ctx);
        }

        if (term) {
            br_log("terminator 0x%02x", term);
            if (term == BLK_TRUNC_EARLY || term == BLK_CANCEL || cancel_sent)
                st = BRSCAN_ERR_CANCELLED;
            break;
        }
    }
    free(rx);
    return st;
}

brscan_status brscan_scan(const brscan_options *opts, brscan_image *out,
                          brscan_progress_fn progress, void *ctx, volatile int *cancel)
{
    memset(out, 0, sizeof(*out));
    brscan_options o;
    if (opts)
        o = *opts;
    else
        brscan_default_options(&o);
    if (!valid_dpi(o.dpi))
        return BRSCAN_ERR_INVALID;
    if (o.width_mm <= 0 || o.width_mm > GLASS_WIDTH_MM)
        o.width_mm = GLASS_WIDTH_MM;
    if (o.height_mm <= 0 || o.height_mm > GLASS_HEIGHT_MM)
        o.height_mm = GLASS_HEIGHT_MM;
    if (o.brightness < -50 || o.brightness > 50 || o.contrast < -50 || o.contrast > 50)
        return BRSCAN_ERR_INVALID;

    /* Pixel geometry. Widths must be a multiple of 16. */
    int width = (int)(o.width_mm / MM_PER_INCH * o.dpi);
    width = (width + 15) & ~15;
    int height = (int)(o.height_mm / MM_PER_INCH * o.dpi);

    int color = o.mode == BRSCAN_MODE_COLOR;
    const char *mode = color ? "CGRAY" : "GRAY64";
    /* The DCP-7030 ignores C=JPEG and always sends raw RGB planes, so ask for that. */
    const char *compression = color ? "C=NONE" : "C=RLENGTH";

    if (progress)
        progress(0, ctx);

    br_usb *usb = NULL;
    brscan_status st = br_usb_open(&usb);
    if (st != BRSCAN_OK)
        return st;

    /* Session 1: device query, like the SANE backend does on open. */
    st = session_start(usb);
    if (st != BRSCAN_OK)
        goto out_close;
    st = run_query(usb, NULL, 0);
    session_stop(usb);
    if (st != BRSCAN_OK)
        goto out_close;

    /* Session 2: the scan. */
    st = session_start(usb);
    if (st != BRSCAN_OK)
        goto out_close;

    uint8_t resp[1024];
    size_t n = 0;
    char cmd[512];
    int len;

    /* Select the flatbed. */
    len = snprintf(cmd, sizeof(cmd), "\x1b" "S\nFB\n" "\x80");
    st = transact(usb, cmd, (size_t)len, resp, sizeof(resp), &n);
    if (st != BRSCAN_OK)
        goto out_session;
    if (resp[0] != BLK_SCAN_FINISHED) {
        if ((resp[0] & 0xc0) == 0xc0 && resp[0] != BLK_PROTOCOL_ERROR) {
            st = status_from_code(resp[0]);
            goto out_session;
        }
        br_log("source select reply 0x%02x, continuing", resp[0]);
    }

    /* Scan info query (reply not needed, but the scanner expects it). */
    len = snprintf(cmd, sizeof(cmd), "\x1b" "I\nR=%d,%d\nM=%s\n" "\x80", o.dpi, o.dpi, mode);
    st = transact(usb, cmd, (size_t)len, resp, sizeof(resp), &n);
    if (st != BRSCAN_OK)
        goto out_session;
    if ((resp[0] & 0xc0) == 0xc0 && resp[0] != BLK_QUERY_RESPONSE) {
        st = status_from_code(resp[0]);
        goto out_session;
    }
    /*
     * Reply: 2-byte length, then "xres,yres,?,width_mm,width_px,height_mm,height_px".
     * The device scans the full requested (16-aligned) width; remember the real
     * usable width so we can crop the padding off.
     */
    int crop_width = 0;
    if (n > 2) {
        char text[128];
        size_t tl = n - 2 < sizeof(text) - 1 ? n - 2 : sizeof(text) - 1;
        memcpy(text, resp + 2, tl);
        text[tl] = '\0';
        int xr, yr, unk, wmm, wpx, hmm, hpx;
        if (sscanf(text, "%d,%d,%d,%d,%d,%d,%d", &xr, &yr, &unk, &wmm, &wpx, &hmm, &hpx) == 7) {
            br_log("scan area: %dx%d px (%dx%d mm) at %dx%d dpi", wpx, hpx, wmm, hmm, xr, yr);
            if (wpx > 0 && wpx < width)
                crop_width = wpx;
        }
    }

    /* Start scanning. */
    len = snprintf(cmd, sizeof(cmd),
                   "\x1b" "X\nR=%d,%d\nM=%s\n%s\nB=%d\nN=%d\nA=%d,%d,%d,%d\nD=SIN\n" "\x80",
                   o.dpi, o.dpi, mode, compression, o.brightness + 50, o.contrast + 50,
                   0, 0, width, height);
    st = br_usb_write(usb, (const uint8_t *)cmd, (size_t)len);
    if (st != BRSCAN_OK)
        goto out_session;

    decoder_t d;
    memset(&d, 0, sizeof(d));
    d.width = width;
    d.crop_width = crop_width;
    size_t expected = (size_t)width * (size_t)height * (color ? 3 : 1);

    FILE *dump = dump_path ? fopen(dump_path, "wb") : NULL;
    st = read_scan_data(usb, &d, expected, progress, ctx, cancel, dump);
    if (dump)
        fclose(dump);

    if (st == BRSCAN_OK) {
        st = build_image(&d, &o, out);
        if (st == BRSCAN_OK && progress)
            progress(1.0, ctx);
    }
    decoder_free(&d);

out_session:
    session_stop(usb);
out_close:
    br_usb_close(usb);
    return st;
}
