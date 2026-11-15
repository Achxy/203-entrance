import AppKit
import SwiftUI

/// Creating the owner window only on a user action preserves menulet-only
/// startup on macOS 14, which predates Scene.defaultLaunchBehavior.
@MainActor public final class NativeWindowPresenter {
    public static let shared = NativeWindowPresenter()
    private var ownerWindow: NSWindow?
    private init() {}
    public func showOwner(model: MenuletModel) {
        if ownerWindow == nil {
            let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 740, height: 560),
                                  styleMask: [.titled, .closable, .miniaturizable, .resizable],
                                  backing: .buffered, defer: false)
            window.title = "203 Lights — Manage Room"
            window.minSize = NSSize(width: 660, height: 470)
            window.isReleasedWhenClosed = false
            window.isRestorable = false
            window.contentView = NSHostingView(rootView: OwnerWindow(model: model))
            window.center()
            ownerWindow = window
        }
        NSApplication.shared.activate(ignoringOtherApps: true)
        ownerWindow?.makeKeyAndOrderFront(nil)
    }
}
