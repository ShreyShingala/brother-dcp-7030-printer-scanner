import AppKit
import SwiftUI

@main
struct BrotherScanApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) var appDelegate

    var body: some Scene {
        WindowGroup("Brother Scan") {
            ContentView()
                .background(WindowCloseGuard())
        }
        .windowStyle(.titleBar)
    }
}

final class AppDelegate: NSObject, NSApplicationDelegate {
    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool {
        true
    }

    func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
        Scanner.shared.confirmDiscardUnsaved(action: "quit") ? .terminateNow : .terminateCancel
    }
}

/// Asks before the window closes with unsaved pages. SwiftUI owns the window's
/// delegate, so we wrap it and forward everything else to the original.
private struct WindowCloseGuard: NSViewRepresentable {
    func makeNSView(context: Context) -> NSView {
        let view = NSView()
        DispatchQueue.main.async {
            guard let window = view.window, !(window.delegate is CloseGuardDelegate) else { return }
            let proxy = CloseGuardDelegate(original: window.delegate)
            context.coordinator.proxy = proxy   // the window holds its delegate weakly
            window.delegate = proxy
        }
        return view
    }

    func updateNSView(_ nsView: NSView, context: Context) {}

    func makeCoordinator() -> Coordinator { Coordinator() }

    final class Coordinator {
        var proxy: CloseGuardDelegate?
    }
}

private final class CloseGuardDelegate: NSObject, NSWindowDelegate {
    weak var original: NSWindowDelegate?

    init(original: NSWindowDelegate?) {
        self.original = original
    }

    func windowShouldClose(_ sender: NSWindow) -> Bool {
        guard Scanner.shared.confirmDiscardUnsaved(action: "close") else { return false }
        return original?.windowShouldClose?(sender) ?? true
    }

    override func responds(to selector: Selector!) -> Bool {
        super.responds(to: selector) || (original?.responds(to: selector) ?? false)
    }

    override func forwardingTarget(for selector: Selector!) -> Any? {
        original?.responds(to: selector) == true ? original : nil
    }
}
