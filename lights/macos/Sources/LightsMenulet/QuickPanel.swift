import AppKit
import LightsCore
import SwiftUI

public struct QuickPanel: View {
    @ObservedObject private var model: MenuletModel
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    public init(model: MenuletModel) { self.model = model }

    public var body: some View {
        TimelineView(.periodic(from: .now, by: 5)) { _ in panel }
    }
    private var panel: some View {
        VStack(spacing: 0) {
            header.padding(16)
            Divider()
            ViewThatFits(in: .vertical) {
                panelContent.fixedSize(horizontal: false, vertical: true)
                ScrollView { panelContent }
            }.frame(maxHeight: 480)
            Divider()
            HStack {
                SettingsLink().labelStyle(.iconOnly).help("Settings")
                    .accessibilityLabel("Open settings")
                Button { NativeWindowPresenter.shared.showOwner(model: model) } label: { Image(systemName: model.isOwner ? "person.badge.key" : "key") }
                    .buttonStyle(.borderless).help(model.isOwner ? "Manage Room" : "Owner Access")
                    .accessibilityLabel(model.isOwner ? "Manage room" : "Owner access")
                Spacer()
                if model.snapshot != nil {
                    Button { Task { await model.refresh() } } label: { Image(systemName: "arrow.clockwise") }
                        .buttonStyle(.borderless).disabled(model.busy).help("Refresh state").accessibilityLabel("Refresh light state")
                }
                Button("Quit") { NSApplication.shared.terminate(nil) }.buttonStyle(.borderless).keyboardShortcut("q")
            }.controlSize(.small).padding(.horizontal, 16).padding(.vertical, 10)
        }
        .frame(width: 370)
        .background(PanelActivityObserver { model.setPanelActive($0) }.frame(width: 0, height: 0).allowsHitTesting(false))
        .preferredColorScheme(model.appearance == "dark" ? .dark : model.appearance == "light" ? .light : nil)
        .animation(reduceMotion ? nil : .easeInOut(duration: 0.16), value: model.selection)
    }

    private var panelContent: some View {
        VStack(alignment: .leading, spacing: 16) {
            if model.snapshot != nil { connectedContent }
            else { AccessForm(model: model, compact: true) }
        }.padding(16)
    }

    private var header: some View {
        HStack(alignment: .center, spacing: 10) {
            Image(systemName: "lightbulb.2.fill").font(.title2).foregroundStyle(.primary)
                .accessibilityHidden(true)
            VStack(alignment: .leading, spacing: 3) {
                Text(model.snapshot?.name ?? "203 Lights").font(.headline).lineLimit(1)
                Label(model.connection.title, systemImage: connectionSymbol).font(.caption).foregroundStyle(.secondary)
            }
            Spacer()
            if model.busy { ProgressView().controlSize(.small).accessibilityLabel("Waiting for confirmed light state") }
            else if let role = model.snapshot?.role { Text(role.title).font(.caption).foregroundStyle(.secondary) }
        }
    }
    private var connectionSymbol: String {
        switch model.connection {
        case .bluetooth: "antenna.radiowaves.left.and.right"
        case .internet: "network"
        case .preview: "eye"
        case .connecting, .reconnecting: "arrow.triangle.2.circlepath"
        default: "circle.dotted"
        }
    }
    @ViewBuilder private var connectedContent: some View {
        if let snapshot = model.snapshot {
            if let restriction = model.restriction {
                Label(restriction, systemImage: snapshot.role == .guest && snapshot.policy.guestRestriction(at: Date()) != nil ? "lock.fill" : "wifi.slash")
                    .font(.callout).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
            }
            Picker("Control", selection: $model.selection) {
                Text("All Lights").tag(LightSelection.room)
                ForEach(snapshot.groups) { Text($0.name).tag(LightSelection.group($0.id)) }
                Divider()
                ForEach(snapshot.lights) { Text($0.name).tag(LightSelection.light($0.id)) }
            }.pickerStyle(.menu)
            LightControls(model: model).id(model.selection)
            if case .light = model.selection {} else {
                Divider()
                VStack(spacing: 2) {
                    ForEach(model.selectedLights) { light in
                        Button { model.selection = .light(light.id) } label: {
                            HStack(spacing: 10) {
                                Image(systemName: light.reported.power == nil ? "questionmark.circle" : light.reported.power == true ? "lightbulb.fill" : "lightbulb")
                                    .foregroundStyle(light.reachable ? .primary : .secondary)
                                VStack(alignment: .leading, spacing: 2) {
                                    Text(light.name).lineLimit(1)
                                    Text(light.reachable ? lightSummary(light) : "Not responding")
                                        .font(.caption).foregroundStyle(.secondary)
                                }
                                Spacer()
                                if model.pendingIDs.contains(light.id) || light.reportPending { ProgressView().controlSize(.mini) }
                                Image(systemName: "chevron.right").font(.caption2).foregroundStyle(.tertiary)
                            }.padding(.vertical, 7).contentShape(Rectangle())
                        }.buttonStyle(.plain).accessibilityLabel("\(light.name), \(light.reachable ? lightSummary(light) : "not responding"), details")
                    }
                }
            }
            FeedbackView(model: model)
            if model.isOwner {
                Divider()
                Button(snapshot.policy.isGuestFrozen(at: Date()) ? "Resume Guest Controls" : "Pause Guests for 15 Minutes",
                       systemImage: snapshot.policy.isGuestFrozen(at: Date()) ? "lock.open" : "pause.circle") {
                    Task { await model.ownerAction(.freezeGuests(until: snapshot.policy.isGuestFrozen(at: Date()) ? nil : Date().addingTimeInterval(900))) }
                }.buttonStyle(.borderless).disabled(model.busy || !model.connection.usable || !model.supportsOwner(.freeze))
            }
        }
    }
    private func lightSummary(_ light: Light) -> String {
        guard let power = light.reported.power else { return "State not observed" }
        guard power else { return "Off" }
        let brightness = light.reported.brightness.map { "\($0)%" } ?? "On"
        if let kelvin = light.reported.kelvin { return "\(brightness) · \(kelvin) K" }
        if let scene = light.reported.sceneID { return "\(brightness) · \(model.snapshot?.scenes.first { $0.id == scene }?.name ?? "Scene \(scene)")" }
        return brightness + (light.reported.rgb == nil ? "" : " · Color")
    }
}

struct PreviewNotice: View {
    var body: some View {
        Label {
            VStack(alignment: .leading, spacing: 2) {
                Text("Preview Mode").fontWeight(.semibold)
                Text("Sample lights only. Nothing is sent to your room.").font(.caption)
            }
        } icon: { Image(systemName: "eye") }
        .foregroundStyle(.secondary).frame(maxWidth: .infinity, alignment: .leading)
        .padding(10).background(.quaternary, in: RoundedRectangle(cornerRadius: 8))
    }
}

struct LightControls: View {
    @ObservedObject var model: MenuletModel
    @State private var brightness = 50.0
    @State private var kelvin = 4000.0
    @State private var speed = 100.0
    @State private var color = Color.white
    @State private var editingBrightness = false
    @State private var editingTemperature = false
    @State private var editingSpeed = false
    @State private var tab = "white"
    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            HStack {
                Text(powerSummary).font(.callout).foregroundStyle(.secondary)
                Spacer()
                Button("On") { model.setPower(true) }.keyboardShortcut("1", modifiers: [.command])
                Button("Off") { model.setPower(false) }.keyboardShortcut("0", modifiers: [.command])
            }.disabled(!model.canControl)
            if model.hasBrightness {
                VStack(spacing: 5) {
                    HStack {
                        Text("Brightness")
                        Spacer()
                        Text(editingBrightness ? "\(Int(brightness))%" : model.commonBrightness.map { "\($0)%" } ?? "Mixed")
                            .monospacedDigit().foregroundStyle(.secondary)
                    }.font(.callout)
                    Slider(value: $brightness, in: model.minimumBrightness...100, step: 1) { Text("Brightness") }
                    minimumValueLabel: { Image(systemName: "sun.min") }
                    maximumValueLabel: { Image(systemName: "sun.max.fill") }
                    onEditingChanged: { editing in editingBrightness = editing; if !editing { model.setBrightness(brightness) } }
                    .labelsHidden()
                    .onChange(of: brightness) { _, value in if editingBrightness { model.setBrightness(value) } }
                    .accessibilityValue("\(Int(brightness)) percent").disabled(!model.canControl)
                }
            }
            if model.temperatureRange != nil || model.supportsColor || !model.supportedScenes.isEmpty {
                if [model.temperatureRange != nil, model.supportsColor, !model.supportedScenes.isEmpty].filter({ $0 }).count > 1 {
                    Picker("Light mode", selection: $tab) {
                        if model.temperatureRange != nil { Text("White").tag("white") }
                        if model.supportsColor { Text("Color").tag("color") }
                        if !model.supportedScenes.isEmpty { Text("Scenes").tag("scenes") }
                    }.pickerStyle(.segmented)
                }
                if tab == "white", let range = model.temperatureRange {
                    VStack(spacing: 5) {
                        HStack { Text("Warmth"); Spacer(); Text("\(Int(kelvin)) K").monospacedDigit().foregroundStyle(.secondary) }.font(.callout)
                        Slider(value: $kelvin, in: range, step: 50) { Text("Color temperature") }
                        minimumValueLabel: { Text("Warm").font(.caption) }
                        maximumValueLabel: { Text("Cool").font(.caption) }
                        onEditingChanged: { editing in editingTemperature = editing; if !editing { model.setTemperature(kelvin) } }
                        .labelsHidden()
                        .onChange(of: kelvin) { _, value in if editingTemperature { model.setTemperature(value) } }
                        .accessibilityValue("\(Int(kelvin)) kelvin").disabled(!model.canControl)
                    }
                } else if tab == "color", model.supportsColor {
                    ColorPicker("Light Color", selection: Binding(get: { color }, set: { color = $0; model.setColor($0) }), supportsOpacity: false)
                        .disabled(!model.canControl)
                    Text("Uses the selected light's color channels.").font(.caption).foregroundStyle(.secondary)
                } else if tab == "scenes" {
                    LazyVGrid(columns: [GridItem(.flexible()), GridItem(.flexible())], spacing: 8) {
                        ForEach(model.supportedScenes) { scene in
                            Button { model.setScene(scene.id) } label: {
                                Label(scene.name, systemImage: scene.symbol).frame(maxWidth: .infinity, alignment: .leading)
                            }.disabled(!model.canControl)
                        }
                    }
                    if let range = model.effectSpeedRange {
                        Slider(value: $speed, in: range, step: 1) { Text("Effect speed") } onEditingChanged: { editing in
                            editingSpeed = editing
                            if !editing { model.setEffectSpeed(speed) }
                        }
                        .onChange(of: speed) { _, value in if editingSpeed { model.setEffectSpeed(value) } }.disabled(!model.canControl)
                    }
                }
            }
            if model.selectedLights.count > 1 && !model.supportsColor && model.selectedLights.contains(where: \.capabilities.rgb) {
                Text("Select an individual color light for colors and scenes. This selection includes white-only lights.")
                    .font(.caption).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
            }
        }
        .onAppear { synchronize() }
        .onChange(of: model.snapshot?.revision) { _, _ in synchronize() }
    }
    private var powerSummary: String {
        let count = model.selectedLights.filter { $0.reported.power == true }.count
        let unknown = model.selectedLights.filter { $0.reported.power == nil }.count
        if model.selectedLights.count == 1 { return unknown == 1 ? "State unknown" : count == 1 ? "On" : "Off" }
        return "\(count) of \(model.selectedLights.count) on" + (unknown > 0 ? " · \(unknown) unknown" : "")
    }
    private func synchronize() {
        if !editingBrightness { brightness = Double(model.commonBrightness ?? 50).clamped(to: model.minimumBrightness...100) }
        if !editingTemperature, let range = model.temperatureRange { kelvin = Double(model.commonTemperature ?? 4000).clamped(to: range) }
        if !editingSpeed, let range = model.effectSpeedRange { speed = Double(model.commonEffectSpeed ?? 100).clamped(to: range) }
        let available = [model.temperatureRange != nil ? "white" : nil, model.supportsColor ? "color" : nil, model.supportedScenes.isEmpty ? nil : "scenes"].compactMap { $0 }
        if !available.contains(tab) { tab = available.first ?? "white" }
        color = model.color
    }
}

struct FeedbackView: View {
    @ObservedObject var model: MenuletModel
    var body: some View {
        if let error = model.problem {
            Label(error, systemImage: "exclamationmark.circle").font(.caption).foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true).accessibilityLabel("Action failed: \(error)")
        } else if let message = model.message {
            Text(message).font(.caption).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
        }
        ForEach(model.partialFailures.keys.sorted(), id: \.self) { id in
            Text("\(model.snapshot?.lights.first { $0.id == id }?.name ?? "Light"): \(model.partialFailures[id] ?? "Not responding")")
                .font(.caption).foregroundStyle(.secondary)
        }
    }
}

extension Double { func clamped(to range: ClosedRange<Double>) -> Double { min(range.upperBound, max(range.lowerBound, self)) } }
