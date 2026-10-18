import AppKit
import Foundation
import ImageIO

/// Runs scans through the C driver core, collects pages into a document,
/// and publishes state for the UI.
final class Scanner: ObservableObject {
    static let shared = Scanner()

    enum Mode: String, CaseIterable, Identifiable {
        case colour = "Colour"
        case gray = "Gray"
        var id: Self { self }
    }

    enum Format: String, CaseIterable, Identifiable {
        case pdf = "PDF"
        case jpg = "JPG"
        var id: Self { self }
    }

    static let resolutions = [100, 150, 200, 300, 600]
    static let autoDelays = [5, 10, 15, 20, 30]

    // Settings (remembered between launches).
    @Published var mode: Mode { didSet { defaults.set(mode.rawValue, forKey: "mode") } }
    @Published var dpi: Int { didSet { defaults.set(dpi, forKey: "dpi") } }
    @Published var format: Format { didSet { defaults.set(format.rawValue, forKey: "format") } }
    @Published var rotate180: Bool { didSet { defaults.set(rotate180, forKey: "rotate180") } }
    @Published var autoDelay: Int { didSet { defaults.set(autoDelay, forKey: "autoDelay") } }
    @Published var folder: URL { didSet { defaults.set(folder.path, forKey: "folder") } }
    @Published var autoMode = false

    // Document being built.
    @Published private(set) var pages: [ScannedPage] = []
    @Published var selectedPage: UUID?

    // Status.
    @Published private(set) var connected = false
    @Published private(set) var isScanning = false
    @Published private(set) var progress: Double?
    @Published private(set) var countdown: Int?
    @Published private(set) var status = "Ready"
    @Published private(set) var isError = false
    @Published private(set) var lastSaved: [URL] = []

    private let defaults = UserDefaults.standard
    private let cancelFlag = UnsafeMutablePointer<Int32>.allocate(capacity: 1)
    private var connectionTimer: Timer?
    private var countdownTimer: Timer?

    private init() {
        let d = UserDefaults.standard
        mode = Mode(rawValue: d.string(forKey: "mode") ?? "") ?? .colour
        dpi = Scanner.resolutions.contains(d.integer(forKey: "dpi")) ? d.integer(forKey: "dpi") : 300
        format = Format(rawValue: d.string(forKey: "format") ?? "") ?? .pdf
        rotate180 = d.object(forKey: "rotate180") as? Bool ?? true
        autoDelay = Scanner.autoDelays.contains(d.integer(forKey: "autoDelay"))
            ? d.integer(forKey: "autoDelay") : 10
        let desktop = FileManager.default.urls(for: .desktopDirectory, in: .userDomainMask)[0]
        if let path = d.string(forKey: "folder"), FileManager.default.fileExists(atPath: path) {
            folder = URL(fileURLWithPath: path, isDirectory: true)
        } else {
            folder = desktop
        }

        cancelFlag.pointee = 0
        refreshConnection()
        connectionTimer = Timer.scheduledTimer(withTimeInterval: 2, repeats: true) { [weak self] _ in
            self?.refreshConnection()
        }
    }

    deinit {
        connectionTimer?.invalidate()
        countdownTimer?.invalidate()
        cancelFlag.deallocate()
    }

    var selected: ScannedPage? {
        pages.first { $0.id == selectedPage } ?? pages.last
    }

    var isBusy: Bool { isScanning || countdown != nil }

    private func refreshConnection() {
        let present = brscan_device_present() != 0
        if present != connected {
            connected = present
            if !isBusy {
                setStatus(present ? "Ready" : "Scanner not connected", error: !present)
            }
        }
    }

    private func setStatus(_ text: String, error: Bool = false) {
        status = text
        isError = error
    }

    // MARK: Scanning

    /// Scan one page and add it to the document. In auto mode, keeps going.
    func scan() {
        guard !isScanning else { return }
        stopCountdown()
        isScanning = true
        progress = 0
        cancelFlag.pointee = 0
        setStatus("Scanning page \(pages.count + 1)…")

        var opts = brscan_options()
        brscan_default_options(&opts)
        opts.mode = mode == .colour ? BRSCAN_MODE_COLOR : BRSCAN_MODE_GRAY
        opts.dpi = Int32(dpi)
        let rotate = rotate180
        let flag = cancelFlag
        let ctx = Unmanaged.passUnretained(self).toOpaque()

        DispatchQueue.global(qos: .userInitiated).async {
            var img = brscan_image()
            let st = brscan_scan(&opts, &img, { fraction, ctx in
                guard let ctx = ctx else { return }
                let me = Unmanaged<Scanner>.fromOpaque(ctx).takeUnretainedValue()
                DispatchQueue.main.async { me.progress = fraction >= 0 ? fraction : nil }
            }, ctx, flag)

            var page: ScannedPage?
            var signature: PageSignature?
            if st == BRSCAN_OK {
                if rotate {
                    brscan_image_rotate180(&img)
                }
                if let cg = brscan_image_create_cgimage(&img) {
                    page = ScannedPage(image: cg, thumbnail: makeThumbnail(cg) ?? cg,
                                       dpi: Int(img.dpi))
                    signature = PageSignature(cg)
                }
            }
            brscan_image_free(&img)
            let message = String(cString: brscan_strerror(st))

            DispatchQueue.main.async {
                self.isScanning = false
                self.progress = nil
                guard st == BRSCAN_OK, let page = page else {
                    self.autoMode = false
                    let error = st != BRSCAN_ERR_CANCELLED
                    self.setStatus(st == BRSCAN_OK ? "Could not process the scan." : message,
                                   error: error || st == BRSCAN_OK)
                    return
                }
                self.handleScanned(page, signature: signature)
            }
        }
    }

    private var lastSignature: PageSignature?

    private func handleScanned(_ page: ScannedPage, signature: PageSignature?) {
        // In auto mode, an empty glass or an unchanged page means "I'm done".
        if autoMode, let sig = signature {
            if sig.isBlank {
                autoMode = false
                setStatus(pages.isEmpty ? "The glass is empty. Put a page on it and scan again."
                                        : "Glass empty, so auto scan stopped. Click Save.")
                return
            }
            if let last = lastSignature, !pages.isEmpty, sig.isSamePage(as: last) {
                autoMode = false
                setStatus("Same page as before, so auto scan stopped. Click Save.")
                return
            }
        }

        pages.append(page)
        selectedPage = page.id
        lastSignature = signature
        lastSaved = []

        if autoMode {
            startCountdown()
        } else {
            setStatus(pages.count == 1 ? "1 page. Scan another page or click Save."
                                       : "\(pages.count) pages. Scan another page or click Save.")
        }
    }

    func cancel() {
        if countdown != nil {
            stopCountdown()
            autoMode = false
            setStatus("Auto scan paused. \(pages.count) page\(pages.count == 1 ? "" : "s") so far.")
            return
        }
        autoMode = false
        cancelFlag.pointee = 1
        setStatus("Cancelling…")
    }

    // MARK: Auto mode countdown

    private func startCountdown() {
        countdown = autoDelay
        setStatus("Page \(pages.count) done. Put the next page on the glass.")
        countdownTimer = Timer.scheduledTimer(withTimeInterval: 1, repeats: true) { [weak self] _ in
            guard let self = self, let left = self.countdown else { return }
            if left <= 1 {
                self.stopCountdown()
                self.scan()
            } else {
                self.countdown = left - 1
            }
        }
    }

    private func stopCountdown() {
        countdownTimer?.invalidate()
        countdownTimer = nil
        countdown = nil
    }

    func scanNow() {
        stopCountdown()
        scan()
    }

    // MARK: Editing pages

    func rotatePage(_ id: UUID) {
        guard let i = pages.firstIndex(where: { $0.id == id }),
              let rotated = rotated180(pages[i].image) else { return }
        pages[i].image = rotated
        pages[i].thumbnail = makeThumbnail(rotated) ?? rotated
        if i == pages.count - 1 {
            lastSignature = PageSignature(rotated)
        }
    }

    func deletePage(_ id: UUID) {
        pages.removeAll { $0.id == id }
        if selectedPage == id {
            selectedPage = pages.last?.id
        }
        lastSignature = pages.last.flatMap { PageSignature($0.image) }
        setStatus(pages.isEmpty ? "Ready" : "\(pages.count) page\(pages.count == 1 ? "" : "s").")
    }

    func movePage(_ id: UUID, by offset: Int) {
        guard let i = pages.firstIndex(where: { $0.id == id }) else { return }
        let j = i + offset
        guard j >= 0, j < pages.count else { return }
        pages.swapAt(i, j)
    }

    func discardAll() {
        stopCountdown()
        autoMode = false
        pages.removeAll()
        selectedPage = nil
        lastSignature = nil
        setStatus("Ready")
    }

    // MARK: Unsaved pages

    /// Returns true if it's OK to quit/close: nothing unsaved, or the user chose
    /// to save (successfully) or discard.
    func confirmDiscardUnsaved(action: String) -> Bool {
        guard !pages.isEmpty || isScanning else { return true }
        let n = pages.count
        let alert = NSAlert()
        alert.alertStyle = .warning
        if n == 0 {
            alert.messageText = "A scan is in progress."
            alert.informativeText = "If you \(action) now, the page being scanned is lost."
            alert.addButton(withTitle: "Cancel")
            alert.addButton(withTitle: action == "quit" ? "Quit Anyway" : "Close Anyway")
            if alert.runModal() == .alertFirstButtonReturn { return false }
            cancel()
            return true
        }
        alert.messageText = "Save the \(n) scanned page\(n == 1 ? "" : "s") before you \(action)?"
        alert.informativeText = isScanning
            ? "A scan is also in progress; that page will be lost."
            : "If you don't save, the scanned pages are lost."
        alert.addButton(withTitle: "Save")
        alert.addButton(withTitle: "Cancel")
        let dontSave = alert.addButton(withTitle: "Don't Save")
        dontSave.hasDestructiveAction = true
        switch alert.runModal() {
        case .alertFirstButtonReturn:
            if isScanning { cancel() }
            save(force: true)
            return pages.isEmpty   // false if saving failed; the error is shown
        case .alertThirdButtonReturn:
            if isScanning { cancel() }
            discardAll()
            return true
        default:
            return false
        }
    }

    // MARK: Saving

    func save(force: Bool = false) {
        guard !pages.isEmpty, force || !isScanning else { return }
        stopCountdown()
        autoMode = false
        let base = nextBaseName()
        do {
            var urls: [URL] = []
            switch format {
            case .pdf:
                let url = folder.appendingPathComponent("\(base).pdf")
                try writePDF(pages, to: url)
                urls = [url]
            case .jpg:
                for (i, page) in pages.enumerated() {
                    let name = pages.count == 1 ? base : "\(base) - \(i + 1)"
                    let url = folder.appendingPathComponent("\(name).jpg")
                    try writeJPEG(page, to: url)
                    urls.append(url)
                }
            }
            let n = pages.count
            pages.removeAll()
            selectedPage = nil
            lastSignature = nil
            lastSaved = urls
            setStatus("Saved \(urls[0].lastPathComponent)\(n > 1 ? " (\(n) pages)" : "")")
        } catch {
            setStatus("Could not save: \(error.localizedDescription)", error: true)
        }
    }

    /// "Scan 2026-10-18 at 14.32.05", never clashing with an existing file.
    private func nextBaseName() -> String {
        let f = DateFormatter()
        f.dateFormat = "yyyy-MM-dd 'at' HH.mm.ss"
        let stem = "Scan \(f.string(from: Date()))"
        var base = stem
        var n = 2
        let fm = FileManager.default
        while fm.fileExists(atPath: folder.appendingPathComponent("\(base).pdf").path)
                || fm.fileExists(atPath: folder.appendingPathComponent("\(base).jpg").path)
                || fm.fileExists(atPath: folder.appendingPathComponent("\(base) - 1.jpg").path) {
            base = "\(stem) \(n)"
            n += 1
        }
        return base
    }

    func chooseFolder() {
        let panel = NSOpenPanel()
        panel.canChooseDirectories = true
        panel.canChooseFiles = false
        panel.allowsMultipleSelection = false
        panel.directoryURL = folder
        panel.prompt = "Choose"
        if panel.runModal() == .OK, let url = panel.url {
            folder = url
        }
    }

    func revealLastSaved() {
        guard !lastSaved.isEmpty else { return }
        NSWorkspace.shared.activateFileViewerSelecting(lastSaved)
    }

    func openLastSaved() {
        guard let url = lastSaved.first else { return }
        NSWorkspace.shared.open(url)
    }
}
