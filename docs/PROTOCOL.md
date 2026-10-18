# DCP-7030 scan protocol notes

Brother calls this "family 3". Everything below was checked against a real
DCP-7030 (USB `04f9:01ea`) on macOS 26.

## USB layout

| Interface | Class | Use |
|---|---|---|
| 0 | 0x07 printer | CUPS printing (left alone) |
| 1 | 0xff vendor | scanning: bulk OUT ep 4 (max packet 16), bulk IN ep 3 (max packet 64) |

When there's nothing to send, the scanner answers a bulk IN read with a
**zero-length packet**, so reads have to poll: retry every ~30 ms until data
arrives or the time runs out.

## Sessions

Vendor control-IN request on the default pipe: `bmRequestType 0xC0`, `wValue 2`,
`wIndex 0`, `wLength 5`.

| bRequest | Meaning | Reply |
|---|---|---|
| 1 | start session | `05 10 01 02 00` ready, `... 80` busy |
| 2 | stop session  | `05 10 02 02 00` |

If start replies busy, send stop and then start again.

## Commands (bulk OUT)

Each command is `ESC <letter> \n ... \x80`.

| Command | Reply |
|---|---|
| `ESC Q \n 0x80` | `c1 00 0e 04 ff 3f 00 00 00 00 00 00 00 01 04 01` (device info, 16 bytes; byte 2 + 2 = length) |
| `ESC S \n FB \n 0x80` | `80` (flatbed selected) |
| `ESC I \n R=100,100 \n M=GRAY64 \n 0x80` | 2-byte length + ASCII `100,100,2,209,816,291,1145` = x dpi, y dpi, ?, width mm, width px, height mm, height px |
| `ESC X \n R=..\n M=..\n C=..\n B=50\n N=50\n A=x0,y0,x1,y1\n D=SIN\n 0x80` | starts the scan; image data follows |
| `ESC R` | cancel |

- `M=`: `CGRAY` (24-bit colour), `GRAY64` (8-bit gray). `ERRDIF` and `TEXT` also exist.
- `C=`: `RLENGTH` for gray. For colour the DCP-7030 **ignores `C=JPEG`** and always
  sends raw planes, so we ask for `C=NONE`.
- `A=` is in pixels. The width must be a multiple of 16. The scanner scans the full
  requested width, so we crop to the "width px" from the `ESC I` reply.
- `B=` and `N=` are brightness and contrast, 0–100 (50 = neutral).

The SANE backend runs `ESC Q` in its own session first, then opens a new session
for `S`, `I` and `X`. We do the same.

## Image data (bulk IN)

A stream of blocks:

| Header | Meaning |
|---|---|
| `0x40 len_lo len_hi` + data | gray, raw |
| `0x42 len_lo len_hi` + data | gray, RLENGTH compressed |
| `0x44` / `0x48` / `0x4c` + len + data | red / green / blue plane of one line (len = width) |
| `0x64` + len + data | JPEG chunk (not used by the DCP-7030) |
| `0x80` | scan finished |
| `0x81`, `0x82` | end of page |
| `0x83`, `0x86` | cancelled (by us / by the Stop button) |
| `0xc2` no document, `0xc3` jam, `0xc4` cover open, `0xc5`/`0xc6` device error |

RLENGTH: control byte `c`. If `c & 0x80`, repeat the next byte `0xff - c + 2` times;
otherwise copy the next `c + 1` bytes.
