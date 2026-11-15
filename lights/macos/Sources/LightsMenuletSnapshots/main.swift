import AppKit
import Foundation
import LightsMenulet
import SwiftUI

enum SnapshotError: Error { case outputMissing, renderFailed(String) }

@main struct MenuletSnapshots {
    @MainActor static func main() throws {
        guard let index = CommandLine.arguments.firstIndex(of: "--output"),
              CommandLine.arguments.indices.contains(index + 1) else { throw SnapshotError.outputMissing }
        let output = URL(fileURLWithPath: CommandLine.arguments[index + 1], isDirectory: true)
        try FileManager.default.createDirectory(at: output, withIntermediateDirectories: true)
        NSApplication.shared.setActivationPolicy(.prohibited)
        var rendered: [String] = []
        for state in ["ready", "partial", "frozen", "disconnected", "long-names"] {
            for scheme in [ColorScheme.light, .dark] {
                let model: MenuletModel
                if state == "disconnected" { model = .configured(arguments: ["203Lights"]) }
                else {
                    let preview = PreviewTransport(role: state == "frozen" ? .guest : .owner, scenario: state)
                    var snapshot = preview.snapshot
                    if state == "long-names" {
                        snapshot.lights[0].name = "The warm study light beside the north-facing window"
                        snapshot.lights[1].name = "Bedside reading light — shared by two roommates"
                        snapshot.groups[0].name = "The longer study and reading area"
                    }
                    model = MenuletModel(transport: preview, initialSnapshot: snapshot)
                    if state == "ready" { model.selection = .light("preview-desk") }
                }
                model.appearance = scheme == .dark ? "dark" : "light"
                let view = QuickPanel(model: model)
                    .frame(width: 370)
                    .background(Color(nsColor: .windowBackgroundColor))
                    .environment(\.colorScheme, scheme)
                let hosting = NSHostingView(rootView: view)
                hosting.frame = NSRect(x: 0, y: 0, width: 370, height: 660)
                hosting.appearance = NSAppearance(named: scheme == .dark ? .darkAqua : .aqua)
                hosting.layoutSubtreeIfNeeded()
                // Give SwiftUI's size preference a few main-runloop passes;
                // no NSWindow is constructed or displayed.
                for _ in 0..<3 {
                    RunLoop.main.run(until: Date().addingTimeInterval(0.02))
                    hosting.layoutSubtreeIfNeeded()
                }
                let height = min(620, max(250, hosting.fittingSize.height))
                hosting.frame.size.height = height
                hosting.layoutSubtreeIfNeeded()
                guard let bitmap = hosting.bitmapImageRepForCachingDisplay(in: hosting.bounds) else { throw SnapshotError.renderFailed(state) }
                hosting.cacheDisplay(in: hosting.bounds, to: bitmap)
                guard let bytes = bitmap.representation(using: .png, properties: [:]) else {
                    throw SnapshotError.renderFailed(state)
                }
                let file = "\(state)-\(scheme == .dark ? "dark" : "light").png"
                try bytes.write(to: output.appendingPathComponent(file), options: .atomic)
                rendered.append(file)
            }
        }
        for (section, name): (OwnerSection, String) in [(.verification, "owner-verification"), (.setup, "owner-setup")] {
            for scheme in [ColorScheme.light, .dark] {
                let preview = PreviewTransport()
                let model = MenuletModel(transport: preview, initialSnapshot: preview.snapshot)
                let view = OwnerWindow(model: model, section: section)
                    .frame(width: 760, height: 650)
                    .background(Color(nsColor: .windowBackgroundColor))
                    .environment(\.colorScheme, scheme)
                let hosting = NSHostingView(rootView: view)
                hosting.frame = NSRect(x: 0, y: 0, width: 760, height: 650)
                hosting.appearance = NSAppearance(named: scheme == .dark ? .darkAqua : .aqua)
                for _ in 0..<3 {
                    RunLoop.main.run(until: Date().addingTimeInterval(0.02))
                    hosting.layoutSubtreeIfNeeded()
                }
                guard let bitmap = hosting.bitmapImageRepForCachingDisplay(in: hosting.bounds) else { throw SnapshotError.renderFailed(name) }
                hosting.cacheDisplay(in: hosting.bounds, to: bitmap)
                guard let bytes = bitmap.representation(using: .png, properties: [:]) else { throw SnapshotError.renderFailed(name) }
                let file = "\(name)-\(scheme == .dark ? "dark" : "light").png"
                try bytes.write(to: output.appendingPathComponent(file), options: .atomic)
                rendered.append(file)
            }
        }
        let manifest: [String: Any] = [
            "renderer": "NSHostingView bitmap cache; offscreen only; no window created",
            "interactive_validation": false,
            "native_control_completeness": "Requires visual inspection and interactive GUI review; initial ImageRenderer attempt omitted AppKit-backed controls",
            "network_connections": 0, "bluetooth_scans": 0, "keychain_writes": 0,
            "files": rendered
        ]
        try JSONSerialization.data(withJSONObject: manifest, options: [.prettyPrinted, .sortedKeys])
            .write(to: output.appendingPathComponent("manifest.json"), options: .atomic)
        print("Wrote \(rendered.count) offscreen fixtures. No windows opened; interactive native controls still need GUI review.")
    }
}
