import CoreGraphics
import Foundation
import ImageIO

struct ScannedPage: Identifiable {
    let id = UUID()
    var image: CGImage
    var thumbnail: CGImage
    let dpi: Int
}

// MARK: - Image helpers

struct SaveError: LocalizedError {
    let errorDescription: String?
}

func makeThumbnail(_ image: CGImage, maxSide: Int = 240) -> CGImage? {
    let scale = Double(maxSide) / Double(max(image.width, image.height))
    let w = max(1, Int(Double(image.width) * scale))
    let h = max(1, Int(Double(image.height) * scale))
    guard let ctx = CGContext(data: nil, width: w, height: h, bitsPerComponent: 8, bytesPerRow: 0,
                              space: CGColorSpace(name: CGColorSpace.sRGB)!,
                              bitmapInfo: CGImageAlphaInfo.noneSkipLast.rawValue) else { return nil }
    ctx.interpolationQuality = .high
    ctx.draw(image, in: CGRect(x: 0, y: 0, width: w, height: h))
    return ctx.makeImage()
}

func rotated180(_ image: CGImage) -> CGImage? {
    let w = image.width, h = image.height
    let gray = image.colorSpace?.model == .monochrome
    let space = gray ? CGColorSpace(name: CGColorSpace.genericGrayGamma2_2)!
                     : CGColorSpace(name: CGColorSpace.sRGB)!
    let info = gray ? CGImageAlphaInfo.none.rawValue : CGImageAlphaInfo.noneSkipLast.rawValue
    guard let ctx = CGContext(data: nil, width: w, height: h, bitsPerComponent: 8, bytesPerRow: 0,
                              space: space, bitmapInfo: info) else { return nil }
    ctx.translateBy(x: CGFloat(w), y: CGFloat(h))
    ctx.rotate(by: .pi)
    ctx.draw(image, in: CGRect(x: 0, y: 0, width: w, height: h))
    return ctx.makeImage()
}

/// JPEG-encode a page. Used for JPG output and inside PDFs, where it keeps
/// colour scans to a few MB per page instead of tens of MB.
func jpegData(_ image: CGImage, dpi: Int, quality: Double = 0.85) -> Data? {
    let data = NSMutableData()
    guard let dest = CGImageDestinationCreateWithData(data, "public.jpeg" as CFString, 1, nil)
    else { return nil }
    let props: [CFString: Any] = [
        kCGImagePropertyDPIWidth: dpi,
        kCGImagePropertyDPIHeight: dpi,
        kCGImageDestinationLossyCompressionQuality: quality,
    ]
    CGImageDestinationAddImage(dest, image, props as CFDictionary)
    guard CGImageDestinationFinalize(dest) else { return nil }
    return data as Data
}

func writeJPEG(_ page: ScannedPage, to url: URL) throws {
    guard let data = jpegData(page.image, dpi: page.dpi) else {
        throw SaveError(errorDescription: "JPEG encoding failed")
    }
    try data.write(to: url, options: .withoutOverwriting)
}

func writePDF(_ pages: [ScannedPage], to url: URL) throws {
    guard let ctx = CGContext(url as CFURL, mediaBox: nil, nil) else {
        throw SaveError(errorDescription: "Could not create the PDF")
    }
    for page in pages {
        let dpi = Double(page.dpi > 0 ? page.dpi : 300)
        var box = CGRect(x: 0, y: 0, width: Double(page.image.width) * 72 / dpi,
                         height: Double(page.image.height) * 72 / dpi)
        // Drawing a JPEG-backed image lets the PDF embed the JPEG as-is.
        var image = page.image
        if let data = jpegData(page.image, dpi: page.dpi),
           let src = CGImageSourceCreateWithData(data as CFData, nil),
           let jpeg = CGImageSourceCreateImageAtIndex(src, 0, nil) {
            image = jpeg
        }
        ctx.beginPage(mediaBox: &box)
        ctx.draw(image, in: box)
        ctx.endPage()
    }
    ctx.closePDF()
}

/// Grayscale fingerprints used in auto mode to spot an empty glass or a page
/// that wasn't changed. Thresholds were measured on a real DCP-7030:
/// the same page scanned twice differs by ~0.2, a different page by 5+;
/// an empty glass has no dark pixels, a page of text ~2-3%.
struct PageSignature {
    static let width = 128, height = 176
    let pixels: [UInt8]
    let darkFraction: Double

    init?(_ image: CGImage) {
        guard let small = PageSignature.render(image, width: PageSignature.width,
                                               height: PageSignature.height),
              let detail = PageSignature.render(image, width: 512,
                                                height: max(1, 512 * image.height / max(1, image.width)))
        else { return nil }
        pixels = small
        darkFraction = Double(detail.reduce(0) { $0 + ($1 < 160 ? 1 : 0) }) / Double(detail.count)
    }

    private static func render(_ image: CGImage, width: Int, height: Int) -> [UInt8]? {
        var buf = [UInt8](repeating: 255, count: width * height)
        let ok = buf.withUnsafeMutableBytes { raw -> Bool in
            guard let ctx = CGContext(data: raw.baseAddress, width: width, height: height,
                                      bitsPerComponent: 8, bytesPerRow: width,
                                      space: CGColorSpace(name: CGColorSpace.genericGrayGamma2_2)!,
                                      bitmapInfo: CGImageAlphaInfo.none.rawValue) else { return false }
            ctx.interpolationQuality = .medium
            ctx.draw(image, in: CGRect(x: 0, y: 0, width: width, height: height))
            return true
        }
        return ok ? buf : nil
    }

    /// Almost nothing dark: just the lid and a few dust specks.
    var isBlank: Bool { darkFraction < 0.0005 }

    /// Same page left on the glass: only scanner noise differs.
    func isSamePage(as other: PageSignature) -> Bool {
        var total = 0
        for i in 0..<pixels.count {
            total += abs(Int(pixels[i]) - Int(other.pixels[i]))
        }
        return Double(total) / Double(pixels.count) < 1.0
    }
}
