import Foundation
import LightsCore

/// Explicit visual fixtures. Never selected by normal app startup, never uses a
/// socket or radio, and every screen carries a persistent Preview label.
@MainActor public final class PreviewTransport: MenuletTransport {
    public let canConnect = true
    public let isPreview = true
    public let unavailableReason: String? = nil
    public private(set) var snapshot: RoomSnapshot
    public private(set) var executedCommandCount = 0
    public private(set) var refreshCount = 0
    private var seen: [UUID: GatewayOutcome] = [:]

    public init(role: AccessRole = .owner, scenario: String = "ready") {
        let now = Date().addingTimeInterval(scenario == "stale" ? -180 : 0)
        let color = try! WiZCapabilities(brightness: true, rgb: true, kelvinRange: 2200...6500,
                                        supportedSceneIDs: [1, 4, 6, 11], effectSpeedRange: 10...200)
        let white = try! WiZCapabilities(brightness: true, kelvinRange: 2700...6500)
        let rgb = try! WiZRGB(red: 255, green: 180, blue: 95)
        let lights = [
            Light(id: "preview-desk", name: "Desk", groupID: "work", reachable: true, capabilities: color,
                  reported: .init(power: true, brightness: 72, kelvin: 4000, rgb: nil, sceneID: nil,
                                  effectSpeed: nil, dualZoneRatio: nil, observedAt: now), revision: 1),
            Light(id: "preview-bed", name: "Bedside", groupID: "rest", reachable: true, capabilities: color,
                  reported: .init(power: true, brightness: 35, kelvin: nil, rgb: rgb, sceneID: nil,
                                  effectSpeed: nil, dualZoneRatio: nil, observedAt: now), revision: 1),
            Light(id: "preview-ceiling", name: "Ceiling", groupID: "work", reachable: scenario != "partial", capabilities: white,
                  reported: .init(power: false, brightness: 60, kelvin: 4000, rgb: nil, sceneID: nil,
                                  effectSpeed: nil, dualZoneRatio: nil, observedAt: now.addingTimeInterval(scenario == "partial" ? -600 : 0)), revision: 1)
        ]
        snapshot = RoomSnapshot(id: "preview-room", name: "Room 203", revision: 1, role: role,
                                connection: scenario == "offline" ? .offline : .preview, lights: lights,
                                groups: [.init(id: "work", name: "Study"), .init(id: "rest", name: "Bedside")],
                                scenes: [.init(id: 1, name: "Ocean", symbol: "water.waves"),
                                         .init(id: 4, name: "Sunset", symbol: "sunset"),
                                         .init(id: 6, name: "Forest", symbol: "leaf"),
                                         .init(id: 11, name: "Warm Glow", symbol: "flame")],
                                policy: .init(guestFreezeUntil: scenario == "frozen" ? now.addingTimeInterval(900) : nil,
                                              ownerHoldUntil: nil, guestPasswordRevision: 1),
                                sessions: [.init(id: "preview-guest-a", name: "A guest", device: "MacBook Air",
                                                 lastActive: now.addingTimeInterval(-40), expiresAt: now.addingTimeInterval(3600)),
                                           .init(id: "preview-guest-b", name: "Study buddy", device: "MacBook Pro",
                                                 lastActive: now.addingTimeInterval(-300), expiresAt: now.addingTimeInterval(7200))],
                                observedAt: now, ownerCapabilities: [.freeze, .revoke, .rotatePassword, .remove, .holdSettings, .importHomeKey])
    }

    public func connect(_ request: AccessRequest) async throws -> RoomSnapshot {
        snapshot.role = request.role
        snapshot.connection = .preview
        return snapshot
    }
    public func refresh() async throws -> RoomSnapshot {
        refreshCount += 1
        guard snapshot.connection.usable else { throw MenuletError.disconnected }
        snapshot.observedAt = Date()
        for index in snapshot.lights.indices where snapshot.lights[index].reachable {
            snapshot.lights[index].reported.observedAt = Date()
        }
        return snapshot
    }
    public func disconnect() async {}

    public func execute(_ command: GatewayCommand) async throws -> GatewayOutcome {
        if let outcome = seen[command.operationID] { return outcome }
        guard snapshot.connection.usable else { throw MenuletError.disconnected }
        guard command.expectedRevision == snapshot.revision else { throw MenuletError.staleRevision }
        executedCommandCount += 1
        try await Task.sleep(for: .milliseconds(150))
        if let outcome = seen[command.operationID] { return outcome }
        guard command.expectedRevision == snapshot.revision else { throw MenuletError.staleRevision }
        var confirmed = Set<String>(), failures: [String: String] = [:]
        switch command.action {
        case .lighting(let ids, let patch):
            if snapshot.role == .guest, let reason = snapshot.policy.guestRestriction(at: Date()) {
                throw MenuletError.restricted(reason)
            }
            for id in ids {
                guard let index = snapshot.lights.firstIndex(where: { $0.id == id }) else { failures[id] = "Light removed"; continue }
                guard snapshot.lights[index].reachable else { failures[id] = "Not responding"; continue }
                let caps = snapshot.lights[index].capabilities
                do {
                    // Codec validation only; no encoded bytes leave this process.
                    let checked = try WiZRequestEncoder.setPilot(patch, capabilities: caps, security: .legacyUnsigned)
                    var state = snapshot.lights[index].reported
                    if let value = patch.power { state.power = value }
                    if let value = checked.effectiveBrightnessPercent {
                        if value == 0 { state.power = false } else { state.brightness = value }
                    }
                    if let rgb = patch.rgb { state.rgb = rgb; state.kelvin = nil; state.sceneID = nil }
                    if let kelvin = patch.colorTemperature { state.kelvin = kelvin; state.rgb = nil; state.sceneID = nil }
                    if let scene = patch.sceneID { state.sceneID = scene; state.rgb = nil; state.kelvin = nil }
                    if let speed = patch.effectSpeed { state.effectSpeed = speed }
                    if let ratio = patch.dualZoneRatio { state.dualZoneRatio = ratio }
                    state.observedAt = Date()
                    snapshot.lights[index].reported = state
                    snapshot.lights[index].revision += 1
                    confirmed.insert(id)
                } catch { failures[id] = "Unsupported setting" }
            }
        case .owner(let action):
            guard snapshot.role == .owner else { throw MenuletError.ownerRequired }
            switch action {
            case .freezeGuests(let until): snapshot.policy.guestFreezeUntil = until
            case .holdSettings(let until): snapshot.policy.ownerHoldUntil = until
            case .revokeSession(let id): snapshot.sessions.removeAll { $0.id == id }
            case .revokeAllGuests: snapshot.sessions = []
            case .rotateGuestPassword(let password):
                guard password.utf8.count >= 12, password.utf8.count <= 128 else { throw MenuletError.invalidCredential }
                snapshot.policy.guestPasswordRevision += 1
                snapshot.sessions = []
            case .removeLight(let id): snapshot.lights.removeAll { $0.id == id }
            case .importHomeKey(let bytes):
                guard bytes.count == 16 else { throw MenuletError.invalidCredential }
                // Validate the form only. Do not store a pretend key or promote
                // any preview bulb to an authenticated physical device.
            }
        }
        snapshot.revision += 1
        snapshot.observedAt = Date()
        let outcome = GatewayOutcome(snapshot: snapshot, confirmedIDs: confirmed, failures: failures)
        if seen.count >= 128 { seen.removeAll(keepingCapacity: true) }
        seen[command.operationID] = outcome
        return outcome
    }
}
