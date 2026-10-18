/*
 * USB transport using IOKit's user-space USB API (IOUSBLib).
 *
 * The DCP-7030 exposes a printer-class interface (used by CUPS) and a
 * vendor-specific interface with a bulk IN/OUT pair for scanning. We only
 * open the vendor interface, so printing is unaffected.
 */
#include "internal.h"

#include <IOKit/IOKitLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/usb/IOUSBLib.h>
#include <CoreFoundation/CoreFoundation.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef IOUSBDeviceInterface320 **DevIf;
typedef IOUSBInterfaceInterface300 **IntfIf;

struct br_usb {
    DevIf dev;
    IntfIf intf;
    UInt8 pipe_in;
    UInt8 pipe_out;
    UInt16 max_packet_in;
};

static io_service_t find_device(void)
{
    const char *classes[] = { "IOUSBHostDevice", kIOUSBDeviceClassName };
    for (size_t i = 0; i < sizeof(classes) / sizeof(classes[0]); i++) {
        CFMutableDictionaryRef match = IOServiceMatching(classes[i]);
        if (!match)
            continue;
        SInt32 vid = BR_VENDOR_ID, pid = BR_PRODUCT_ID;
        CFNumberRef v = CFNumberCreate(NULL, kCFNumberSInt32Type, &vid);
        CFNumberRef p = CFNumberCreate(NULL, kCFNumberSInt32Type, &pid);
        CFDictionarySetValue(match, CFSTR(kUSBVendorID), v);
        CFDictionarySetValue(match, CFSTR(kUSBProductID), p);
        CFRelease(v);
        CFRelease(p);
        /* IOServiceGetMatchingService consumes `match`. */
        io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, match);
        if (svc)
            return svc;
    }
    return 0;
}

int brscan_device_present(void)
{
    io_service_t svc = find_device();
    if (!svc)
        return 0;
    IOObjectRelease(svc);
    return 1;
}

static void *plugin_query(io_service_t svc, CFUUIDRef type, CFUUIDRef iid)
{
    IOCFPlugInInterface **plugin = NULL;
    SInt32 score = 0;
    kern_return_t kr = IOCreatePlugInInterfaceForService(svc, type, kIOCFPlugInInterfaceID,
                                                         &plugin, &score);
    if (kr != KERN_SUCCESS || !plugin) {
        br_log("IOCreatePlugInInterfaceForService failed: 0x%x", kr);
        return NULL;
    }
    void *obj = NULL;
    HRESULT hr = (*plugin)->QueryInterface(plugin, CFUUIDGetUUIDBytes(iid), &obj);
    IODestroyPlugInInterface(plugin);
    if (hr != S_OK)
        return NULL;
    return obj;
}

/* Try to use this interface: must be vendor class with bulk IN + bulk OUT. */
static brscan_status try_interface(br_usb *usb, io_service_t isvc)
{
    IntfIf intf = plugin_query(isvc, kIOUSBInterfaceUserClientTypeID,
                               kIOUSBInterfaceInterfaceID300);
    if (!intf)
        return BRSCAN_ERR_USB;

    UInt8 cls = 0, num = 0, ep_count = 0;
    (*intf)->GetInterfaceClass(intf, &cls);
    (*intf)->GetInterfaceNumber(intf, &num);
    br_log("interface %u class 0x%02x", num, cls);
    if (cls != kUSBVendorSpecificClass) {
        (*intf)->Release(intf);
        return BRSCAN_ERR_NOT_FOUND;
    }

    IOReturn ir = (*intf)->USBInterfaceOpen(intf);
    if (ir != kIOReturnSuccess) {
        br_log("USBInterfaceOpen(interface %u) failed: 0x%x", num, ir);
        (*intf)->Release(intf);
        return ir == kIOReturnExclusiveAccess ? BRSCAN_ERR_ACCESS : BRSCAN_ERR_USB;
    }

    (*intf)->GetNumEndpoints(intf, &ep_count);
    UInt8 pin = 0, pout = 0;
    UInt16 mps_in = 64;
    for (UInt8 pipe = 1; pipe <= ep_count; pipe++) {
        UInt8 dir, number, type, interval;
        UInt16 mps;
        if ((*intf)->GetPipeProperties(intf, pipe, &dir, &number, &type, &mps, &interval)
            != kIOReturnSuccess)
            continue;
        br_log("  pipe %u: ep %u dir %s type %u mps %u", pipe, number,
               dir == kUSBIn ? "in" : "out", type, mps);
        if (type != kUSBBulk)
            continue;
        if (dir == kUSBIn && !pin) {
            pin = pipe;
            mps_in = mps;
        } else if (dir == kUSBOut && !pout) {
            pout = pipe;
        }
    }
    if (!pin || !pout) {
        (*intf)->USBInterfaceClose(intf);
        (*intf)->Release(intf);
        return BRSCAN_ERR_NOT_FOUND;
    }

    usb->intf = intf;
    usb->pipe_in = pin;
    usb->pipe_out = pout;
    usb->max_packet_in = mps_in;
    return BRSCAN_OK;
}

brscan_status br_usb_open(br_usb **out)
{
    *out = NULL;
    io_service_t svc = find_device();
    if (!svc)
        return BRSCAN_ERR_NOT_FOUND;

    br_usb *usb = calloc(1, sizeof(*usb));
    if (!usb) {
        IOObjectRelease(svc);
        return BRSCAN_ERR_NOMEM;
    }

    usb->dev = plugin_query(svc, kIOUSBDeviceUserClientTypeID, kIOUSBDeviceInterfaceID320);
    IOObjectRelease(svc);
    if (!usb->dev) {
        free(usb);
        return BRSCAN_ERR_USB;
    }

    /*
     * The device is already configured (the printer side is in use), so we
     * don't open the device itself; we just look for the scanner interface.
     */
    IOUSBFindInterfaceRequest req = {
        .bInterfaceClass = kIOUSBFindInterfaceDontCare,
        .bInterfaceSubClass = kIOUSBFindInterfaceDontCare,
        .bInterfaceProtocol = kIOUSBFindInterfaceDontCare,
        .bAlternateSetting = kIOUSBFindInterfaceDontCare,
    };
    io_iterator_t it = 0;
    if ((*usb->dev)->CreateInterfaceIterator(usb->dev, &req, &it) != kIOReturnSuccess) {
        br_usb_close(usb);
        return BRSCAN_ERR_USB;
    }

    brscan_status st = BRSCAN_ERR_NOT_FOUND;
    io_service_t isvc;
    while ((isvc = IOIteratorNext(it))) {
        brscan_status r = try_interface(usb, isvc);
        IOObjectRelease(isvc);
        if (r == BRSCAN_OK) {
            st = BRSCAN_OK;
            break;
        }
        if (r != BRSCAN_ERR_NOT_FOUND)
            st = r;   /* remember the most useful error */
    }
    IOObjectRelease(it);

    if (st != BRSCAN_OK) {
        br_usb_close(usb);
        return st;
    }
    *out = usb;
    return BRSCAN_OK;
}

void br_usb_close(br_usb *usb)
{
    if (!usb)
        return;
    if (usb->intf) {
        (*usb->intf)->USBInterfaceClose(usb->intf);
        (*usb->intf)->Release(usb->intf);
    }
    if (usb->dev)
        (*usb->dev)->Release(usb->dev);
    free(usb);
}

brscan_status br_usb_control_in(br_usb *usb, uint8_t request, uint16_t value,
                                uint16_t index, uint8_t *buf, uint16_t len, uint16_t *got)
{
    IOUSBDevRequestTO req = {
        .bmRequestType = USBmakebmRequestType(kUSBIn, kUSBVendor, kUSBDevice),
        .bRequest = request,
        .wValue = value,
        .wIndex = index,
        .wLength = len,
        .pData = buf,
        .noDataTimeout = 2000,
        .completionTimeout = 5000,
    };
    /* Pipe 0 is the default control pipe; works through the open interface. */
    IOReturn ir = (*usb->intf)->ControlRequestTO(usb->intf, 0, &req);
    if (ir != kIOReturnSuccess) {
        br_log("control request 0x%02x failed: 0x%x", request, ir);
        return ir == kIOReturnNoDevice ? BRSCAN_ERR_NOT_FOUND : BRSCAN_ERR_USB;
    }
    if (got)
        *got = (uint16_t)req.wLenDone;
    return BRSCAN_OK;
}

brscan_status br_usb_write(br_usb *usb, const uint8_t *buf, size_t len)
{
    if (br_verbose)
        br_hexdump("send", buf, len);
    IOReturn ir = (*usb->intf)->WritePipeTO(usb->intf, usb->pipe_out, (void *)buf,
                                            (UInt32)len, 2000, 5000);
    if (ir != kIOReturnSuccess) {
        br_log("bulk write failed: 0x%x", ir);
        if (ir == kIOUSBTransactionTimeout || ir == kIOReturnTimeout)
            (*usb->intf)->ClearPipeStallBothEnds(usb->intf, usb->pipe_out);
        return ir == kIOReturnNoDevice ? BRSCAN_ERR_NOT_FOUND : BRSCAN_ERR_USB;
    }
    return BRSCAN_OK;
}

brscan_status br_usb_read(br_usb *usb, uint8_t *buf, size_t len, size_t *got,
                          unsigned timeout_ms)
{
    *got = 0;
    /* Request a whole number of packets so the device can't overrun us. */
    size_t mps = usb->max_packet_in ? usb->max_packet_in : 64;
    len -= len % mps;
    if (len == 0)
        return BRSCAN_ERR_INVALID;

    /*
     * The scanner answers "nothing yet" with a zero-length packet, so poll
     * (like the SANE backend does) until data arrives or the time runs out.
     *
     * Each request only has a no-data timeout: once data starts flowing we
     * never abort a transfer half way, which would lose bytes. A read
     * completes on a short packet, which the scanner sends after each chunk.
     */
    unsigned waited = 0;
    for (;;) {
        UInt32 size = (UInt32)len;
        unsigned left = timeout_ms > waited ? timeout_ms - waited : 1;
        IOReturn ir = (*usb->intf)->ReadPipeTO(usb->intf, usb->pipe_in, buf, &size, left, 0);
        if (ir == kIOUSBTransactionTimeout || ir == kIOReturnTimeout) {
            br_log("read timeout after %u ms", timeout_ms);
            /* Resync the data toggle after an aborted request (see IOUSBLib.h). */
            (*usb->intf)->ClearPipeStallBothEnds(usb->intf, usb->pipe_in);
            return BRSCAN_OK;
        }
        if (ir == kIOUSBPipeStalled) {
            (*usb->intf)->ClearPipeStallBothEnds(usb->intf, usb->pipe_in);
            return BRSCAN_ERR_USB;
        }
        if (ir != kIOReturnSuccess) {
            br_log("bulk read failed: 0x%x", ir);
            return ir == kIOReturnNoDevice ? BRSCAN_ERR_NOT_FOUND : BRSCAN_ERR_USB;
        }
        if (size > 0) {
            *got = size;
            return BRSCAN_OK;
        }
        if (waited >= timeout_ms)
            return BRSCAN_OK;
        usleep(30 * 1000);
        waited += 30;
    }
}
