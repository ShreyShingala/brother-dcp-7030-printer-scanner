import SwiftUI

struct ContentView: View {
    @ObservedObject private var scanner = Scanner.shared
    @State private var confirmDiscard = false

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            settings
            Divider()
            preview
            if !scanner.pages.isEmpty {
                pageStrip
            }
            footer
        }
        .padding(20)
        .frame(minWidth: 520, idealWidth: 560, minHeight: 720, idealHeight: 780)
        .confirmationDialog("Discard \(scanner.pages.count) scanned page(s)?",
                            isPresented: $confirmDiscard) {
            Button("Discard", role: .destructive) { scanner.discardAll() }
        }
    }

    // MARK: Settings

    private var settings: some View {
        Form {
            Picker("Mode:", selection: $scanner.mode) {
                ForEach(Scanner.Mode.allCases) { Text($0.rawValue).tag($0) }
            }
            .pickerStyle(.radioGroup)
            .horizontalRadioGroupLayout()

            Picker("Resolution:", selection: $scanner.dpi) {
                ForEach(Scanner.resolutions, id: \.self) { Text("\($0) dpi").tag($0) }
            }
            .frame(maxWidth: 240)

            Picker("Save as:", selection: $scanner.format) {
                ForEach(Scanner.Format.allCases) { Text($0.rawValue).tag($0) }
            }
            .pickerStyle(.segmented)
            .frame(maxWidth: 240)

            HStack {
                Text("Save to:")
                Text(scanner.folder.lastPathComponent)
                    .foregroundColor(.secondary)
                    .lineLimit(1)
                    .truncationMode(.middle)
                Button("Change…") { scanner.chooseFolder() }
            }

            Toggle("Rotate pages 180°", isOn: $scanner.rotate180)
                .help("Turn this on if your scans come out upside down.")

            HStack {
                Toggle("Auto scan: next page after", isOn: $scanner.autoMode)
                Picker("", selection: $scanner.autoDelay) {
                    ForEach(Scanner.autoDelays, id: \.self) { Text("\($0) s").tag($0) }
                }
                .labelsHidden()
                .frame(width: 80)
                .disabled(!scanner.autoMode)
            }
            .help("After each page, wait so you can swap the page, then scan the next one. "
                  + "Stops when the glass is empty or the page wasn't changed.")
        }
        .disabled(scanner.isBusy)
    }

    // MARK: Preview

    private var preview: some View {
        ZStack {
            // Light gray "desk" so the white page's edges stand out.
            RoundedRectangle(cornerRadius: 8)
                .fill(Color(nsColor: .previewBackdrop))
            RoundedRectangle(cornerRadius: 8)
                .strokeBorder(Color.secondary.opacity(0.3))
            if let page = scanner.selected {
                Image(decorative: page.image, scale: 1)
                    .resizable()
                    .interpolation(.high)
                    .aspectRatio(contentMode: .fit)
                    .overlay(Rectangle().strokeBorder(Color.black.opacity(0.25), lineWidth: 1))
                    .shadow(color: .black.opacity(0.2), radius: 5, x: 0, y: 2)
                    .padding(16)
            } else if !scanner.lastSaved.isEmpty {
                VStack(spacing: 10) {
                    Text(scanner.status).foregroundColor(.secondary)
                    HStack {
                        Button("Open") { scanner.openLastSaved() }
                        Button("Show in Finder") { scanner.revealLastSaved() }
                    }
                }
            } else {
                Text("Put a page face down on the glass, then click Scan.\n"
                     + "Scan more pages to add them to the same document.")
                    .foregroundColor(.secondary)
                    .multilineTextAlignment(.center)
                    .padding()
            }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }

    // MARK: Pages

    private var pageStrip: some View {
        ScrollViewReader { proxy in
            ScrollView(.horizontal, showsIndicators: true) {
                HStack(spacing: 10) {
                    ForEach(Array(scanner.pages.enumerated()), id: \.element.id) { index, page in
                        thumbnail(page, number: index + 1, isLast: index == scanner.pages.count - 1)
                            .id(page.id)
                    }
                }
                .padding(.vertical, 4)
            }
            .frame(height: 128)
            .onChange(of: scanner.pages.count) { _ in
                if let last = scanner.pages.last {
                    withAnimation { proxy.scrollTo(last.id, anchor: .trailing) }
                }
            }
        }
    }

    private func thumbnail(_ page: ScannedPage, number: Int, isLast: Bool) -> some View {
        let isSelected = scanner.selected?.id == page.id
        return VStack(spacing: 3) {
            Image(decorative: page.thumbnail, scale: 1)
                .resizable()
                .aspectRatio(contentMode: .fit)
                .frame(height: 92)
                .overlay(Rectangle()
                    .strokeBorder(isSelected ? Color.accentColor : Color.black.opacity(0.3),
                                  lineWidth: isSelected ? 3 : 1))
                .shadow(color: .black.opacity(0.25), radius: 2, x: 0, y: 1)
            HStack(spacing: 4) {
                Text("\(number)").font(.caption).foregroundColor(.secondary)
                Button { scanner.rotatePage(page.id) } label: {
                    Image(systemName: "rotate.right")
                }
                .buttonStyle(.borderless)
                .help("Rotate this page 180°")
                Button { scanner.deletePage(page.id) } label: {
                    Image(systemName: "trash")
                }
                .buttonStyle(.borderless)
                .help("Remove this page")
            }
            .disabled(scanner.isScanning)
        }
        .contentShape(Rectangle())
        .onTapGesture { scanner.selectedPage = page.id }
        .contextMenu {
            Button("Rotate 180°") { scanner.rotatePage(page.id) }
            Button("Move Left") { scanner.movePage(page.id, by: -1) }.disabled(number == 1)
            Button("Move Right") { scanner.movePage(page.id, by: 1) }.disabled(isLast)
            Divider()
            Button("Remove Page") { scanner.deletePage(page.id) }
        }
    }

    // MARK: Footer

    private var footer: some View {
        VStack(alignment: .leading, spacing: 10) {
            if scanner.isScanning {
                if let p = scanner.progress {
                    ProgressView(value: p)
                } else {
                    ProgressView().progressViewStyle(.linear)
                }
            } else if let left = scanner.countdown {
                ProgressView("Next scan in \(left) s…",
                             value: Double(scanner.autoDelay - left),
                             total: Double(scanner.autoDelay))
            }

            HStack(spacing: 8) {
                Circle()
                    .fill(scanner.connected ? Color.green : Color.red)
                    .frame(width: 8, height: 8)
                    .help(scanner.connected ? "Scanner connected" : "Scanner not connected")
                Text(scanner.status)
                    .foregroundColor(scanner.isError ? .red : .secondary)
                    .lineLimit(2)
                Spacer()
                buttons
            }
        }
    }

    @ViewBuilder
    private var buttons: some View {
        if scanner.isScanning {
            Button("Cancel") { scanner.cancel() }
        } else if scanner.countdown != nil {
            Button("Pause") { scanner.cancel() }
            Button("Save") { scanner.save() }
            Button("Scan Now") { scanner.scanNow() }
                .keyboardShortcut(.defaultAction)
        } else {
            if !scanner.pages.isEmpty {
                Button("Discard") { confirmDiscard = true }
                Button(scanner.pages.count == 1 ? "Save" : "Save \(scanner.pages.count) Pages") {
                    scanner.save()
                }
                .keyboardShortcut("s", modifiers: .command)
            }
            Button(scanner.pages.isEmpty ? "Scan" : "Scan Page \(scanner.pages.count + 1)") {
                scanner.scan()
            }
            .keyboardShortcut(.defaultAction)
            .disabled(!scanner.connected)
        }
    }
}

private extension NSColor {
    /// Light gray in light mode, dark gray in dark mode.
    static let previewBackdrop = NSColor(name: nil) { appearance in
        appearance.bestMatch(from: [.darkAqua, .aqua]) == .darkAqua
            ? NSColor(white: 0.22, alpha: 1)
            : NSColor(white: 0.91, alpha: 1)
    }
}
