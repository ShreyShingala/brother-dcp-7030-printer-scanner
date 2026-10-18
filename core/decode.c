/*
 * Brother RLENGTH decoding (a PackBits variant) used for gray scans.
 *
 * Each mini-block starts with a control byte c:
 *   c & 0x80: repeat the next byte (0xff - c + 2) times
 *   else:     copy the next (c + 1) bytes literally
 */
#include "internal.h"

#include <stdlib.h>
#include <string.h>

static int ensure(uint8_t **dst, size_t *cap, size_t need)
{
    if (need <= *cap)
        return 0;
    size_t c = *cap ? *cap : 64 * 1024;
    while (c < need)
        c *= 2;
    uint8_t *p = realloc(*dst, c);
    if (!p)
        return -1;
    *dst = p;
    *cap = c;
    return 0;
}

size_t br_rlength_decode(const uint8_t *src, size_t src_len,
                         uint8_t **dst, size_t *dst_len, size_t *dst_cap)
{
    size_t pos = 0;
    while (pos < src_len) {
        uint8_t c = src[pos];
        if (c & 0x80) {
            if (src_len - pos < 2)
                break;
            size_t count = 0xff - (size_t)c + 2;
            if (ensure(dst, dst_cap, *dst_len + count))
                return (size_t)-1;
            memset(*dst + *dst_len, src[pos + 1], count);
            *dst_len += count;
            pos += 2;
        } else {
            size_t count = (size_t)c + 1;
            if (src_len - pos < count + 1)
                break;
            if (ensure(dst, dst_cap, *dst_len + count))
                return (size_t)-1;
            memcpy(*dst + *dst_len, src + pos + 1, count);
            *dst_len += count;
            pos += count + 1;
        }
    }
    return pos;
}
