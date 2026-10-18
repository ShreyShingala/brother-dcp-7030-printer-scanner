/*
 * Internal interfaces shared by the brscan core.
 */
#ifndef BRSCAN_INTERNAL_H
#define BRSCAN_INTERNAL_H

#include "brscan.h"

#define BR_VENDOR_ID   0x04f9
#define BR_PRODUCT_ID  0x01ea   /* DCP-7030 */

extern int br_verbose;
void br_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void br_hexdump(const char *label, const uint8_t *data, size_t len);

/* ---- USB transport (usb_iokit.c) ---- */

typedef struct br_usb br_usb;

brscan_status br_usb_open(br_usb **out);
void br_usb_close(br_usb *usb);

/* Vendor control-IN request to the device (bmRequestType 0xC0). */
brscan_status br_usb_control_in(br_usb *usb, uint8_t request, uint16_t value,
                                uint16_t index, uint8_t *buf, uint16_t len,
                                uint16_t *got);

brscan_status br_usb_write(br_usb *usb, const uint8_t *buf, size_t len);

/*
 * Read whatever the scanner sends next, up to len bytes.
 * Waits up to timeout_ms for data to start. *got = 0 on timeout (returns BRSCAN_OK).
 */
brscan_status br_usb_read(br_usb *usb, uint8_t *buf, size_t len, size_t *got,
                          unsigned timeout_ms);

/* ---- Data decoding (decode.c) ---- */

/* Brother RLENGTH (PackBits-like) decode. Returns bytes consumed from src. */
size_t br_rlength_decode(const uint8_t *src, size_t src_len,
                         uint8_t **dst, size_t *dst_len, size_t *dst_cap);

/* Decode a complete recorded scan stream (as saved by --dump). For tests/replay. */
brscan_status br_decode_stream(const uint8_t *stream, size_t len, const brscan_options *o,
                               int width, brscan_image *out);

/* JPEG bytes -> 4-channel RGBX image (image_out.c). */
brscan_status br_decode_jpeg(const uint8_t *data, size_t len, int dpi, brscan_image *out);

#endif
