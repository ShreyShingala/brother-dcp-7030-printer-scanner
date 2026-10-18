# Brother Scan

MacOS scanner driver and app for the Brother DCP-7030 printer since Brother no longer ships Mac scanner drivers for it.

Printing keeps working through the normal printer driver so I just added scanning. Nothing else needs to be installed: it talks to the scanner directly over USB using macOS's built-in IOKit, and writes images with CoreGraphics/ImageIO.

- Universal app: Apple Silicon and Intel, macOS 12 or later
- Colour or gray, 100–600 dpi, saved as PDF or JPG (the command line tool also does PNG)
- Multi-page documents, with an optional hands-free auto mode
- Flatbed only (the DCP-7030 has no document feeder)

## Building

Needs only the Xcode Command Line Tools (`xcode-select --install`), not full Xcode.

```sh
make            # build/brscan and build/stage.noindex/Brother Scan.app (universal)
make test       # offline decoder tests, no scanner needed
make install    # copy the app to ~/Applications
```

The app is signed ad hoc. If you copy it to another Mac, macOS will warn the first time, just confirm it.

## Command line

```sh
build/brscan --probe                                 # check the scanner answers
build/brscan -o scan.pdf                             # colour, 300 dpi, full glass
build/brscan -o page.png --mode gray --res 150
build/brscan -o receipt.jpg --width 80 --height 200  # scan only part of the glass (mm)
build/brscan -o scan.pdf --rotate                    # rotate 180°
build/brscan -o x.png -v --dump stream.bin           # log USB traffic, save raw stream
```

Ctrl-C cancels a scan cleanly.

## Layout

```
core/      C driver: USB transport (IOKit), protocol, decoding, image output
cli/       brscan command line tool
app/       SwiftUI app
tests/     offline decoder tests
docs/      protocol notes
```

## Limitations

- Preview and Image Capture don't list the scanner. You must open the app directly to scan.
- Only tested on the DCP-7030. Other Brother "family 3" models (DCP-7040, MFC-7340, …) probably use the same protocol, but they'd need their USB product ID added in `core/internal.h`.

## Credits

Sane-project for documenting the Brother's scanner protocol for Linux. You can find them here http://www.sane-project.org/
