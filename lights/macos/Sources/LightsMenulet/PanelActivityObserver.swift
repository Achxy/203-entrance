import AppKit
import SwiftUI

/// MenuBarExtra can retain its content between presentations. Follow the real
/// native window instead of treating SwiftUI view lifetime as visibility.
struct PanelActivityObserver: NSViewRepresentable {
    let changed: @MainActor (Bool) -> Void
    func makeNSView(context: Context) -> ActivityView { ActivityView(changed: changed) }
    func updateNSView(_ view: ActivityView, context: Context) {
        view.changed = changed
        view.reportActivity()
    }

    final class ActivityView: NSView {
        var changed: @MainActor (Bool) -> Void
        init(changed: @escaping @MainActor (Bool) -> Void) {
            self.changed = changed
            super.init(frame: .zero)
        }
        required init?(coder: NSCoder) { nil }
        override func viewDidMoveToWindow() {
            super.viewDidMoveToWindow()
            NotificationCenter.default.removeObserver(self)
            if let window {
                for name in [NSWindow.didBecomeKeyNotification, NSWindow.didResignKeyNotification,
                             NSWindow.didMiniaturizeNotification, NSWindow.didDeminiaturizeNotification,
                             NSWindow.willCloseNotification] {
                    NotificationCenter.default.addObserver(self, selector: #selector(windowChanged), name: name, object: window)
                }
            }
            reportActivity()
        }
        @objc private func windowChanged(_ notification: Notification) {
            if notification.name == NSWindow.willCloseNotification { changed(false) }
            else { reportActivity() }
        }
        func reportActivity() {
            changed(window.map { $0.isVisible && $0.isKeyWindow && !$0.isMiniaturized } ?? false)
        }
    }
}
