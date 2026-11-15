import AppKit
import Foundation
import LightsCore
import SwiftUI

@MainActor public final class MenuletModel: ObservableObject {
    @Published public private(set) var snapshot: RoomSnapshot?
    @Published public var selection: LightSelection = .room
    @Published public var connectionPreference: ConnectionPreference = .automatic { didSet { preferences?.connection = connectionPreference } }
    @Published public private(set) var busy = false
    @Published public private(set) var pendingIDs = Set<String>()
    @Published public private(set) var message: String?
    @Published public private(set) var problem: String?
    @Published public private(set) var partialFailures: [String: String] = [:]
    @Published public private(set) var connection: ConnectionPresentation = .disconnected
    @Published public private(set) var needsCredentialCleanup = false
    @Published public private(set) var requiresExplicitRejoin = false
    @Published public private(set) var discoveredLights: [GatewayCandidate] = []
    @Published public private(set) var discoveringLights = false
    @Published public private(set) var commissioningMessage: String?
    @Published public var endpoint = Room203Defaults.relayEndpoint.absoluteString
    @Published public var displayName = ""
    @Published public var accessRole: AccessRole = .guest
    @Published public var rememberAccess = false
    @Published public var nearbyPeripheralID: UUID?
    @Published public var appearance = "system" { didSet { preferences?.appearance = appearance } }
    public let isPreview: Bool
    public let canConnect: Bool
    public let unavailableReason: String?
    private let transport: any MenuletTransport
    private var queued: [String: (ids: [String], patch: WiZLightingPatch)] = [:]
    private var work: Task<Void, Never>?
    private var lifecycle: UInt64 = 0
    private var exclusiveOperation = false
    private var lastRefreshAttempt = -Double.infinity
    private var preferences: MenuletPreferences?
    private var cancelLightDiscovery = false
    private var panelActive = false
    private var foregroundRefresh: Task<Void, Never>?

    public init(transport: any MenuletTransport, initialSnapshot: RoomSnapshot? = nil) {
        self.transport = transport
        isPreview = transport.isPreview
        canConnect = transport.canConnect
        unavailableReason = transport.unavailableReason
        snapshot = initialSnapshot
        connection = initialSnapshot?.connection ?? .disconnected
    }

    public static func configured(arguments: [String] = CommandLine.arguments, persistPreferences: Bool = false) -> MenuletModel {
        guard arguments.contains("--preview") else {
            let model: MenuletModel
            do {
                let trusted = try Room203Defaults.trustedIdentity()
                let routes = GatewayRouteCoordinator(trustedIdentity: trusted)
                let adapter = GatewayAdapter(trustedIdentity: trusted, enabled: true,
                    onRouteResult: { route, succeeded, milliseconds in routes.recordResult(route, succeeded: succeeded, roundTripMilliseconds: milliseconds) },
                    channelFactory: { request, excluded in try await routes.makeRoute(request, excluding: excluded) })
                model = MenuletModel(transport: adapter)
            } catch { model = MenuletModel(transport: UnconfiguredTransport()) }
            if persistPreferences {
                let preferences = MenuletPreferences()
                model.appearance = preferences.appearance
                model.connectionPreference = preferences.connection
                model.preferences = preferences
            }
            return model
        }
        let role: AccessRole = arguments.contains("--preview-guest") ? .guest : .owner
        let scenario = arguments.firstIndex(of: "--preview-state").flatMap { index in
            arguments.indices.contains(index + 1) ? arguments[index + 1] : nil
        } ?? "ready"
        let demo = PreviewTransport(role: role, scenario: scenario)
        return MenuletModel(transport: demo, initialSnapshot: demo.snapshot)
    }

    public var selectedLights: [Light] {
        guard let snapshot else { return [] }
        switch selection {
        case .room: return snapshot.lights
        case .group(let id): return snapshot.lights.filter { $0.groupID == id }
        case .light(let id): return snapshot.lights.filter { $0.id == id }
        }
    }
    public var selectionTitle: String {
        guard let snapshot else { return "Room" }
        switch selection {
        case .room: return "All Lights"
        case .group(let id): return snapshot.groups.first { $0.id == id }?.name ?? "Group"
        case .light(let id): return snapshot.lights.first { $0.id == id }?.name ?? "Light"
        }
    }
    public var restriction: String? {
        guard snapshot != nil else { return "Connect to your room to control its lights." }
        guard connection.usable else { return "These are last known values. Reconnect before changing lights." }
        if let reason = snapshot?.controlUnavailableReason { return reason }
        if let observedAt = snapshot?.observedAt, Date().timeIntervalSince(observedAt) > 60 {
            return "The room state is out of date. Refresh before changing lights."
        }
        if snapshot?.role == .guest, let reason = snapshot?.policy.guestRestriction(at: Date(), selectedIDs: Set(selectedLights.map(\.id))) { return reason }
        if selectedLights.isEmpty { return "No lights are enrolled in this selection." }
        if let blocked = selectedLights.first(where: { $0.controlUnavailableReason != nil }) {
            return selectedLights.count == 1 ? blocked.controlUnavailableReason : "This selection includes a light that is not ready for signed local commands. Select a ready light."
        }
        return nil
    }
    public var canControl: Bool { restriction == nil && !exclusiveOperation }
    public var isOwner: Bool { snapshot?.role == .owner }
    public func supportsOwner(_ capability: OwnerCapability) -> Bool { isOwner && snapshot?.ownerCapabilities.contains(capability) == true }
    public var canCommission: Bool { isOwner && !isPreview && transport is any GatewayCommissioningTransport && connection.usable }
    public var hasBrightness: Bool { !selectedLights.isEmpty && selectedLights.allSatisfy(\.capabilities.brightness) }
    public var minimumBrightness: Double { Double(selectedLights.map(\.capabilities.minimumDimming).max() ?? 10) }
    public var commonBrightness: Int? {
        let values = Set(selectedLights.compactMap(\.reported.brightness))
        return selectedLights.allSatisfy { $0.reported.brightness != nil } && values.count == 1 ? values.first : nil
    }
    public var supportsColor: Bool { !selectedLights.isEmpty && selectedLights.allSatisfy(\.capabilities.rgb) }
    public var temperatureRange: ClosedRange<Double>? {
        let ranges = selectedLights.compactMap(\.capabilities.kelvinRange)
        guard !ranges.isEmpty, ranges.count == selectedLights.count,
              let low = ranges.map(\.lowerBound).max(), let high = ranges.map(\.upperBound).min(), low <= high else { return nil }
        return Double(low)...Double(high)
    }
    public var supportedScenes: [SceneOption] {
        guard let first = selectedLights.first else { return [] }
        let intersection = selectedLights.dropFirst().reduce(first.capabilities.supportedSceneIDs) { $0.intersection($1.capabilities.supportedSceneIDs) }
        return snapshot?.scenes.filter { intersection.contains($0.id) } ?? []
    }
    public var effectSpeedRange: ClosedRange<Double>? {
        guard selectedLights.allSatisfy({ ($0.reported.sceneID ?? 0) > 0 }) else { return nil }
        let ranges = selectedLights.compactMap(\.capabilities.effectSpeedRange)
        guard ranges.count == selectedLights.count, let low = ranges.map(\.lowerBound).max(),
              let high = ranges.map(\.upperBound).min(), low <= high else { return nil }
        return Double(low)...Double(high)
    }
    public var commonTemperature: Int? {
        let values = Set(selectedLights.compactMap(\.reported.kelvin))
        return selectedLights.allSatisfy { $0.reported.kelvin != nil } && values.count == 1 ? values.first : nil
    }
    public var commonEffectSpeed: Int? {
        let values = Set(selectedLights.compactMap(\.reported.effectSpeed))
        return selectedLights.allSatisfy { $0.reported.effectSpeed != nil } && values.count == 1 ? values.first : nil
    }
    public var color: Color {
        guard let rgb = selectedLights.first?.reported.rgb else { return .white }
        return Color(.sRGB, red: Double(rgb.red) / 255, green: Double(rgb.green) / 255, blue: Double(rgb.blue) / 255)
    }

    public func connect(credential: String, explicitlyRejoin: Bool = false) async {
        guard canConnect else { problem = unavailableReason; return }
        guard connection != .connecting, !busy, work == nil else { return }
        let usingSavedAccess = credential.isEmpty && rememberAccess && !explicitlyRejoin
        guard !endpoint.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty, !credential.isEmpty || usingSavedAccess else { problem = MenuletError.invalidCredential.localizedDescription; return }
        let generation = lifecycle
        busy = true; exclusiveOperation = true
        defer { if generation == lifecycle { busy = false; exclusiveOperation = false } }
        connection = .connecting; problem = nil
        do {
            let request = AccessRequest(endpoint: endpoint, name: displayName, role: accessRole, credential: credential,
                                        preference: connectionPreference, rememberAccess: rememberAccess, nearbyPeripheralID: nearbyPeripheralID)
            let result: RoomSnapshot
            if explicitlyRejoin { result = try await transport.explicitlyRejoinGuest(request) }
            else { result = try await transport.connect(request) }
            guard generation == lifecycle else { return }
            accept(result); requiresExplicitRejoin = false
        } catch {
            if generation == lifecycle, !handleSessionEnd(error) {
                if (error as? GatewayAdapterError) == .rejoinRequired { requiresExplicitRejoin = true }
                connection = .disconnected; problem = error.localizedDescription
            }
        }
    }
    public func refresh() async {
        guard snapshot != nil, !busy, work == nil else { return }
        let uptime = ProcessInfo.processInfo.systemUptime
        guard uptime - lastRefreshAttempt >= 5 else { message = "The room was checked just now."; return }
        lastRefreshAttempt = uptime
        let generation = lifecycle
        busy = true; exclusiveOperation = true
        defer { if generation == lifecycle { busy = false; exclusiveOperation = false } }
        do {
            let result = try await transport.refresh()
            guard generation == lifecycle else { return }
            accept(result); problem = nil
        } catch { if generation == lifecycle, !handleSessionEnd(error) { connection = .offline; problem = error.localizedDescription } }
    }
    public func setPanelActive(_ active: Bool) {
        guard panelActive != active else { return }
        panelActive = active
        if active { startForegroundRefresh() }
    }
    private func startForegroundRefresh() {
        guard panelActive, snapshot != nil, foregroundRefresh == nil else { return }
        foregroundRefresh = Task { @MainActor [weak self] in
            guard let self else { return }
            defer { self.foregroundRefresh = nil }
            var failures = 0
            while self.panelActive, self.snapshot != nil, !Task.isCancelled {
                await self.refresh()
                guard self.panelActive, self.snapshot != nil else { return }
                if self.connection == .offline { failures += 1 } else { failures = 0 }
                guard failures < 3 else { return }
                let delay = min(30, 5.5 * pow(2, Double(failures))) + Double.random(in: 0...1)
                do { try await Task.sleep(for: .seconds(delay)) } catch { return }
            }
        }
        // Hiding the panel only changes the loop condition. An encrypted read
        // already in flight can finish; it is never canceled halfway through.
    }
    public func signOut() async {
        lifecycle += 1; work?.cancel(); work = nil; queued.removeAll(); pendingIDs.removeAll(); busy = false; exclusiveOperation = false
        snapshot = nil; connection = .disconnected; partialFailures = [:]; message = nil; problem = nil; selection = .room
        cancelLightDiscovery = true; discoveringLights = false; discoveredLights = []; commissioningMessage = nil
        let generation = lifecycle
        do {
            try await transport.signOut()
            if generation == lifecycle { needsCredentialCleanup = false; requiresExplicitRejoin = false }
        } catch {
            if generation == lifecycle {
                needsCredentialCleanup = true
                problem = "Disconnected, but saved access could not be removed from Keychain. Unlock your Mac and retry."
            }
        }
    }
    public func clearFeedback() { problem = nil; message = nil; partialFailures = [:] }

    public func discoverLights() async {
        guard canCommission, let setup = transport as? any GatewayCommissioningTransport else {
            commissioningMessage = isPreview ? "Preview mode does not discover or pair real lights." : "Connect with owner access before discovering lights."
            return
        }
        guard !busy, work == nil else { commissioningMessage = "Wait for the current room change to finish."; return }
        let generation = lifecycle
        busy = true; exclusiveOperation = true; discoveringLights = true; cancelLightDiscovery = false
        discoveredLights = []; commissioningMessage = "Looking for lights on the gateway's private network…"
        defer { if generation == lifecycle { busy = false; exclusiveOperation = false; discoveringLights = false } }
        do {
            try await setup.startDiscovery()
            let deadline = ProcessInfo.processInfo.systemUptime + 15
            var page = try await setup.candidate(at: 0)
            while page.discovering, !cancelLightDiscovery, ProcessInfo.processInfo.systemUptime < deadline {
                try await Task.sleep(for: .seconds(1))
                guard generation == lifecycle else { return }
                page = try await setup.candidate(at: 0)
            }
            guard generation == lifecycle else { return }
            if cancelLightDiscovery || page.discovering {
                try await setup.cancelDiscovery()
                commissioningMessage = cancelLightDiscovery ? "Discovery stopped." : "Discovery timed out. Check the light's setup and try again."
                return
            }
            guard (0...8).contains(page.total) else {
                commissioningMessage = "The gateway returned an invalid discovery result. Reconnect and try again."
                return
            }
            var candidates: [GatewayCandidate] = []
            if let candidate = page.candidate { candidates.append(candidate) }
            for index in 1..<max(1, page.total) {
                guard generation == lifecycle else { return }
                if cancelLightDiscovery { try await setup.cancelDiscovery(); commissioningMessage = "Discovery stopped."; return }
                if let candidate = try await setup.candidate(at: index).candidate { candidates.append(candidate) }
            }
            guard generation == lifecycle else { return }
            discoveredLights = candidates
            commissioningMessage = candidates.isEmpty ? "No lights found. Check that the intended bulb has joined the gateway's private Wi-Fi." : "Choose the light you intended to add. Discovery alone does not verify control."
        } catch {
            if generation == lifecycle, !handleSessionEnd(error) { commissioningMessage = error.localizedDescription }
        }
    }

    public func stopDiscoveringLights() {
        cancelLightDiscovery = true
        if discoveringLights { commissioningMessage = "Stopping after the current gateway response…" }
    }

    public func adoptLight(candidateID: Int, name: String) async {
        guard canCommission, let setup = transport as? any GatewayCommissioningTransport,
              discoveredLights.contains(where: { $0.id == candidateID && $0.identified }) else {
            commissioningMessage = "Discover and select the intended light first."
            return
        }
        guard !busy, work == nil else { commissioningMessage = "Wait for the current room change to finish."; return }
        let generation = lifecycle
        busy = true; exclusiveOperation = true
        defer { if generation == lifecycle { busy = false; exclusiveOperation = false } }
        do {
            let updated = try await setup.adoptCandidate(candidateID, name: name.trimmingCharacters(in: .whitespacesAndNewlines), allowLegacyControl: false)
            guard generation == lifecycle else { return }
            accept(updated)
            discoveredLights.removeAll { $0.id == candidateID }
            commissioningMessage = "Added with signed gateway commands required. Check the actual bulb response using the light controls."
        } catch {
            if generation == lifecycle, !handleSessionEnd(error) { commissioningMessage = error.localizedDescription }
        }
    }

    public func setPower(_ on: Bool) { enqueue("power", patch: .init(power: on)) }
    public func setBrightness(_ value: Double) {
        guard value.isFinite, (0...100).contains(value) else { problem = "Choose a brightness between 0 and 100 percent."; return }
        enqueue("brightness", patch: .init(brightnessPercent: Int(value.rounded())))
    }
    public func setTemperature(_ value: Double) {
        guard value.isFinite, (1000...10000).contains(value) else { problem = "Choose a supported color temperature."; return }
        enqueue("style", patch: .init(colorTemperature: Int(value.rounded())))
    }
    public func setScene(_ id: Int) { enqueue("style", patch: .init(sceneID: id)) }
    public func setEffectSpeed(_ value: Double) {
        guard value.isFinite, (10...200).contains(value) else { problem = "Choose a supported effect speed."; return }
        enqueue("speed", patch: .init(effectSpeed: Int(value.rounded())))
    }
    public func setColor(_ value: Color) {
        guard let color = NSColor(value).usingColorSpace(.sRGB),
              let rgb = try? WiZRGB(red: Int((color.redComponent * 255).rounded()),
                                    green: Int((color.greenComponent * 255).rounded()),
                                    blue: Int((color.blueComponent * 255).rounded())) else { return }
        enqueue("style", patch: .init(rgb: rgb))
    }
    private func enqueue(_ field: String, patch: WiZLightingPatch) {
        guard canControl else { problem = restriction ?? "Wait for the current room change to finish."; return }
        let ids = selectedLights.map(\.id)
        let key = ids.sorted().joined(separator: ":") + "/" + field
        if queued.count >= 16 && queued[key] == nil { problem = "Finish the current changes before selecting more lights."; return }
        queued[key] = (ids, patch)
        pendingIDs.formUnion(ids)
        if work == nil {
            let generation = lifecycle
            work = Task { [weak self] in
                try? await Task.sleep(for: .milliseconds(500))
                await self?.drain(generation: generation)
            }
        }
    }
    private func drain(generation: UInt64) async {
        while !Task.isCancelled, generation == lifecycle, let key = queued.keys.sorted().first, let next = queued.removeValue(forKey: key) {
            guard let snapshot, connection.usable else { break }
            busy = true
            do {
                let result = try await transport.execute(.init(expectedRevision: snapshot.revision, action: .lighting(ids: next.ids, patch: next.patch)))
                guard !Task.isCancelled, generation == lifecycle else { return }
                accept(result.snapshot); partialFailures = result.failures
                problem = nil
                message = result.failures.isEmpty ? (isPreview ? "Preview updated. No real light was changed." : "Light state confirmed.") : "\(result.confirmedIDs.count) changed; \(result.failures.count) did not respond."
            } catch {
                guard !Task.isCancelled, generation == lifecycle else { return }
                if handleSessionEnd(error) { return }
                problem = error.localizedDescription
                queued.removeAll()
            }
            pendingIDs = Set(queued.values.flatMap(\.ids)); busy = false
            if !queued.isEmpty { try? await Task.sleep(for: .milliseconds(500)) }
        }
        if generation == lifecycle { queued.removeAll(); pendingIDs.removeAll(); busy = false; work = nil }
    }

    public func ownerAction(_ action: OwnerAction) async {
        guard isOwner else { problem = MenuletError.ownerRequired.localizedDescription; return }
        let capability: OwnerCapability
        switch action {
        case .freezeGuests: capability = .freeze
        case .holdSettings: capability = .holdSettings
        case .revokeSession, .revokeAllGuests: capability = .revoke
        case .rotateGuestPassword: capability = .rotatePassword
        case .removeLight: capability = .remove
        case .importHomeKey: capability = .importHomeKey
        }
        guard supportsOwner(capability) else { problem = "This gateway does not support that action yet."; return }
        guard connection.usable, let snapshot else { problem = MenuletError.disconnected.localizedDescription; return }
        guard !busy, work == nil else { problem = "Wait for the current change to finish."; return }
        let generation = lifecycle
        busy = true; exclusiveOperation = true
        defer { if generation == lifecycle { busy = false; exclusiveOperation = false } }
        do {
            let result = try await transport.execute(.init(expectedRevision: snapshot.revision, action: .owner(action)))
            guard generation == lifecycle else { return }
            accept(result.snapshot); problem = nil
            message = isPreview ? "Preview policy updated. No real access was changed." : "Room policy confirmed."
        } catch { if generation == lifecycle, !handleSessionEnd(error) { problem = error.localizedDescription } }
    }

    private func handleSessionEnd(_ error: any Error) -> Bool {
        guard let reason = error as? MenuletError else { return false }
        switch reason {
        case .accessRevoked, .accessExpired:
            lifecycle += 1; work?.cancel(); work = nil; queued.removeAll(); pendingIDs.removeAll()
            snapshot = nil; connection = .disconnected; busy = false; exclusiveOperation = false
            partialFailures = [:]; message = nil; problem = reason.localizedDescription; selection = .room
            requiresExplicitRejoin = true
            cancelLightDiscovery = true; discoveringLights = false; discoveredLights = []
            return true
        default: return false
        }
    }

    private func accept(_ value: RoomSnapshot) {
        guard snapshot == nil || snapshot?.id != value.id || value.revision >= (snapshot?.revision ?? 0) else { return }
        snapshot = value; connection = value.connection
        if selectedLights.isEmpty { selection = .room }
        if value.role == .guest && value.policy.guestRestriction(at: Date()) != nil { queued.removeAll() }
        startForegroundRefresh()
    }
}
