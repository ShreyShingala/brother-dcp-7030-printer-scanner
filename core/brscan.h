/*
 * brscan - native macOS scanner driver for the Brother DCP-7030
 * (Brother "family 3" USB protocol).
 */
#ifndef BRSCAN_H
#define BRSCAN_H

#include <stddef.h>
#include <stdint.h>
#include <CoreGraphics/CoreGraphics.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BRSCAN_OK = 0,
    BRSCAN_ERR_NOT_FOUND,     /* scanner not connected */
    BRSCAN_ERR_ACCESS,        /* USB interface in use by another program */
    BRSCAN_ERR_USB,           /* low level USB failure */
    BRSCAN_ERR_BUSY,          /* scanner busy (e.g. printing, or menu open on panel) */
    BRSCAN_ERR_COVER_OPEN,
    BRSCAN_ERR_JAMMED,
    BRSCAN_ERR_NO_DOCS,
    BRSCAN_ERR_DEVICE,        /* service error / device stuck: power-cycle the printer */
    BRSCAN_ERR_PROTOCOL,      /* unexpected data from scanner */
    BRSCAN_ERR_TIMEOUT,
    BRSCAN_ERR_CANCELLED,
    BRSCAN_ERR_NOMEM,
    BRSCAN_ERR_DECODE,
    BRSCAN_ERR_WRITE,
    BRSCAN_ERR_INVALID,
} brscan_status;

typedef enum {
    BRSCAN_MODE_COLOR = 0,
    BRSCAN_MODE_GRAY,
    BRSCAN_MODE_BW,           /* gray scan, thresholded on the Mac */
} brscan_mode;

typedef enum {
    BRSCAN_FORMAT_PDF = 0,
    BRSCAN_FORMAT_PNG,
    BRSCAN_FORMAT_JPEG,
} brscan_format;

typedef struct {
    brscan_mode mode;
    int dpi;                  /* 100, 150, 200, 300, 600 */
    double width_mm;          /* <= 0 means full glass width (210mm) */
    double height_mm;         /* <= 0 means full glass height (295mm) */
    int brightness;           /* -50..50, 0 = default */
    int contrast;             /* -50..50, 0 = default */
} brscan_options;

/* Decoded image. channels is 1 (8-bit gray) or 4 (RGBX, X unused). */
typedef struct {
    int width;
    int height;
    int channels;
    int dpi;
    size_t bytes_per_row;
    uint8_t *pixels;
} brscan_image;

/* fraction in [0,1], or < 0 when unknown. Called from the scanning thread. */
typedef void (*brscan_progress_fn)(double fraction, void *ctx);

void brscan_default_options(brscan_options *opts);

/* 1 if a DCP-7030 is on USB, else 0. */
int brscan_device_present(void);

/* Open a session, run the device query, close. Writes a short report to `report`. */
brscan_status brscan_probe(char *report, size_t report_len);

/*
 * Scan one page from the flatbed. Blocks until done.
 * Set *cancel to non-zero from another thread to stop (may be NULL).
 */
brscan_status brscan_scan(const brscan_options *opts, brscan_image *out,
                          brscan_progress_fn progress, void *progress_ctx,
                          volatile int *cancel);

void brscan_image_free(brscan_image *img);

/* Rotate in place by 180 degrees (the DCP-7030 scans from the far end of the glass). */
void brscan_image_rotate180(brscan_image *img);

/* Caller releases the result (Swift does this automatically). */
CGImageRef brscan_image_create_cgimage(const brscan_image *img) CF_RETURNS_RETAINED;

brscan_status brscan_write_file(const brscan_image *img, const char *path, brscan_format fmt);

const char *brscan_strerror(brscan_status st);

/* Debugging: verbose log to stderr; dump raw USB scan stream to a file. */
void brscan_set_verbose(int verbose);
void brscan_set_dump_path(const char *path);

#ifdef __cplusplus
}
#endif

#endif
