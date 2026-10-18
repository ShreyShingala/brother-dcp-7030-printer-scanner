/*
 * Image conversion and file output via CoreGraphics / ImageIO.
 */
#include "internal.h"

#include <ImageIO/ImageIO.h>
#include <CoreFoundation/CoreFoundation.h>
#include <stdlib.h>
#include <string.h>

CGImageRef brscan_image_create_cgimage(const brscan_image *img)
{
    if (!img || !img->pixels || img->width <= 0 || img->height <= 0)
        return NULL;
    size_t size = img->bytes_per_row * (size_t)img->height;
    CFDataRef data = CFDataCreate(NULL, img->pixels, (CFIndex)size);
    if (!data)
        return NULL;
    CGDataProviderRef provider = CGDataProviderCreateWithCFData(data);
    CFRelease(data);

    CGColorSpaceRef cs;
    CGBitmapInfo info;
    size_t bpp;
    if (img->channels == 1) {
        cs = CGColorSpaceCreateWithName(kCGColorSpaceGenericGrayGamma2_2);
        info = (CGBitmapInfo)kCGImageAlphaNone;
        bpp = 8;
    } else {
        cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        info = (CGBitmapInfo)kCGImageAlphaNoneSkipLast | kCGBitmapByteOrderDefault;
        bpp = 32;
    }
    CGImageRef cg = CGImageCreate((size_t)img->width, (size_t)img->height, 8, bpp,
                                  img->bytes_per_row, cs, info, provider, NULL, true,
                                  kCGRenderingIntentDefault);
    CGColorSpaceRelease(cs);
    CGDataProviderRelease(provider);
    return cg;
}

brscan_status br_decode_jpeg(const uint8_t *bytes, size_t len, int dpi, brscan_image *out)
{
    memset(out, 0, sizeof(*out));
    CFDataRef data = CFDataCreate(NULL, bytes, (CFIndex)len);
    if (!data)
        return BRSCAN_ERR_NOMEM;
    CGImageSourceRef src = CGImageSourceCreateWithData(data, NULL);
    CFRelease(data);
    if (!src)
        return BRSCAN_ERR_DECODE;
    CGImageRef cg = CGImageSourceCreateImageAtIndex(src, 0, NULL);
    CFRelease(src);
    if (!cg)
        return BRSCAN_ERR_DECODE;

    size_t w = CGImageGetWidth(cg), h = CGImageGetHeight(cg);
    out->width = (int)w;
    out->height = (int)h;
    out->channels = 4;
    out->dpi = dpi;
    out->bytes_per_row = w * 4;
    out->pixels = calloc(h, out->bytes_per_row);
    if (!out->pixels) {
        CGImageRelease(cg);
        return BRSCAN_ERR_NOMEM;
    }
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef ctx = CGBitmapContextCreate(out->pixels, w, h, 8, out->bytes_per_row, cs,
                                             (CGBitmapInfo)kCGImageAlphaNoneSkipLast);
    CGColorSpaceRelease(cs);
    if (!ctx) {
        CGImageRelease(cg);
        brscan_image_free(out);
        return BRSCAN_ERR_DECODE;
    }
    CGContextDrawImage(ctx, CGRectMake(0, 0, (CGFloat)w, (CGFloat)h), cg);
    CGContextRelease(ctx);
    CGImageRelease(cg);
    return BRSCAN_OK;
}

static CFURLRef url_for_path(const char *path)
{
    return CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path,
                                                   (CFIndex)strlen(path), false);
}

static brscan_status write_pdf(CGImageRef cg, const brscan_image *img, CFURLRef url)
{
    double dpi = img->dpi > 0 ? img->dpi : 300;
    CGRect page = CGRectMake(0, 0, img->width * 72.0 / dpi, img->height * 72.0 / dpi);
    CGContextRef pdf = CGPDFContextCreateWithURL(url, &page, NULL);
    if (!pdf)
        return BRSCAN_ERR_WRITE;
    CGContextBeginPage(pdf, &page);
    CGContextDrawImage(pdf, page, cg);
    CGContextEndPage(pdf);
    CGPDFContextClose(pdf);
    CGContextRelease(pdf);
    return BRSCAN_OK;
}

static brscan_status write_bitmap(CGImageRef cg, const brscan_image *img, CFURLRef url,
                                  CFStringRef type)
{
    CGImageDestinationRef dst = CGImageDestinationCreateWithURL(url, type, 1, NULL);
    if (!dst)
        return BRSCAN_ERR_WRITE;
    double dpi = img->dpi > 0 ? img->dpi : 300;
    double quality = 0.9;
    CFNumberRef d = CFNumberCreate(NULL, kCFNumberDoubleType, &dpi);
    CFNumberRef q = CFNumberCreate(NULL, kCFNumberDoubleType, &quality);
    const void *keys[] = { kCGImagePropertyDPIWidth, kCGImagePropertyDPIHeight,
                           kCGImageDestinationLossyCompressionQuality };
    const void *vals[] = { d, d, q };
    CFDictionaryRef props = CFDictionaryCreate(NULL, keys, vals, 3,
                                               &kCFTypeDictionaryKeyCallBacks,
                                               &kCFTypeDictionaryValueCallBacks);
    CGImageDestinationAddImage(dst, cg, props);
    bool ok = CGImageDestinationFinalize(dst);
    CFRelease(props);
    CFRelease(d);
    CFRelease(q);
    CFRelease(dst);
    return ok ? BRSCAN_OK : BRSCAN_ERR_WRITE;
}

brscan_status brscan_write_file(const brscan_image *img, const char *path, brscan_format fmt)
{
    CGImageRef cg = brscan_image_create_cgimage(img);
    if (!cg)
        return BRSCAN_ERR_INVALID;
    CFURLRef url = url_for_path(path);
    if (!url) {
        CGImageRelease(cg);
        return BRSCAN_ERR_WRITE;
    }
    brscan_status st;
    switch (fmt) {
    case BRSCAN_FORMAT_PDF:
        st = write_pdf(cg, img, url);
        break;
    case BRSCAN_FORMAT_PNG:
        st = write_bitmap(cg, img, url, CFSTR("public.png"));
        break;
    case BRSCAN_FORMAT_JPEG:
        st = write_bitmap(cg, img, url, CFSTR("public.jpeg"));
        break;
    default:
        st = BRSCAN_ERR_INVALID;
        break;
    }
    CFRelease(url);
    CGImageRelease(cg);
    return st;
}

void brscan_image_rotate180(brscan_image *img)
{
    if (!img || !img->pixels || img->width <= 0 || img->height <= 0)
        return;
    size_t bpp = (size_t)img->channels;
    size_t w = (size_t)img->width;
    /* Swap pixel (x, y) with (w-1-x, h-1-y), walking from both ends. */
    for (size_t y = 0; y < (size_t)(img->height + 1) / 2; y++) {
        uint8_t *top = img->pixels + y * img->bytes_per_row;
        uint8_t *bot = img->pixels + ((size_t)img->height - 1 - y) * img->bytes_per_row;
        size_t xmax = top == bot ? w / 2 : w;
        for (size_t x = 0; x < xmax; x++) {
            uint8_t *a = top + x * bpp;
            uint8_t *b = bot + (w - 1 - x) * bpp;
            for (size_t c = 0; c < bpp; c++) {
                uint8_t t = a[c];
                a[c] = b[c];
                b[c] = t;
            }
        }
    }
}
