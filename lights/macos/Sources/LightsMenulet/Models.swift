import Foundation
import LightsCore

public enum AccessRole: String, CaseIterable, Identifiable, Sendable {
    case guest, owner
    public var id: Self { self }
    public var title: String { self == .owner ? "Owner" : "Guest" }
}

public enum ConnectionPreference: String, CaseIterable, Identifiable, Sendable {
    case automatic, internet, bluetooth
    public var id: Self { self }
    public var title: String {
        switch self { case .automatic: "Automatic"; case .internet: "Internet"; case .bluetooth: "Bluetooth" }
    }
}

public struct AccessRequest: Sendable {
    public let endpoint: String
    public let name: String
    public let role: AccessRole
    public let credential: String
    public let preference: ConnectionPreference
    public let rememberAccess: Bool
    public let nearbyPeripheralID: UUID?
    public init(endpoint: String, name: String, role: AccessRole, credential: String, preference: ConnectionPreference,
                rememberAccess: Bool = false, nearbyPeripheralID: UUID? = nil) {
        self.endpoint = endpoint; self.name = name; self.role = role; self.credential = credential; self.preference = preference
        self.rememberAccess = rememberAccess
        self.nearbyPeripheralID = nearbyPeripheralID
    }
}

public enum ConnectionPresentation: Equatable, Sendable {
    case disconnected, connecting, internet, bluetooth, reconnecting, offline, preview
    public var title: String {
        switch self {
        case .disconnected: "Not connected"
        case .connecting: "Connecting…"
        case .internet: "Internet"
        case .bluetooth: "Nearby · Bluetooth"
        case .reconnecting: "Reconnecting…"
        case .offline: "Offline · last known state"
        case .preview: "Preview · no real lights"
        }
    }
    public var usable: Bool { self == .internet || self == .bluetooth || self == .preview }
}

public enum LightObservationTrust: String, Sendable { case unavailable, unverifiedLAN, verifiedHomeKey }
public struct ReportedLightState: Equatable, Sendable {
    public var power: Bool?
    public var brightness: Int?
    public var kelvin: Int?
    public var rgb: WiZRGB?
    public var sceneID: Int?
    public var effectSpeed: Int?
    public var dualZoneRatio: Int?
    public var observedAt: Date?
    public var trust: LightObservationTrust
    public init(power: Bool? = nil, brightness: Int? = nil, kelvin: Int? = nil, rgb: WiZRGB? = nil,
                sceneID: Int? = nil, effectSpeed: Int? = nil, dualZoneRatio: Int? = nil, observedAt: Date? = nil,
                trust: LightObservationTrust = .unavailable) {
        self.power = power; self.brightness = brightness; self.kelvin = kelvin; self.rgb = rgb
        self.sceneID = sceneID; self.effectSpeed = effectSpeed; self.dualZoneRatio = dualZoneRatio; self.observedAt = observedAt
        self.trust = trust
    }
}

public struct Light: Identifiable, Equatable, Sendable {
    public let id: String
    public var name: String
    public var groupID: String
    public var reachable: Bool
    public var capabilities: WiZCapabilities
    public var reported: ReportedLightState
    public var revision: UInt64
    public var reportPending: Bool
    public var controlUnavailableReason: String?
    public init(id: String, name: String, groupID: String, reachable: Bool, capabilities: WiZCapabilities,
                reported: ReportedLightState, revision: UInt64, reportPending: Bool = false, controlUnavailableReason: String? = nil) {
        self.id = id; self.name = name; self.groupID = groupID; self.reachable = reachable
        self.capabilities = capabilities; self.reported = reported; self.revision = revision
        self.reportPending = reportPending
        self.controlUnavailableReason = controlUnavailableReason
    }
}

public struct LightGroup: Identifiable, Equatable, Sendable {
    public let id: String
    public var name: String
    public init(id: String, name: String) { self.id = id; self.name = name }
}

public struct SceneOption: Identifiable, Equatable, Sendable {
    public let id: Int
    public let name: String
    public let symbol: String
    public init(id: Int, name: String, symbol: String) { self.id = id; self.name = name; self.symbol = symbol }
}

public struct GuestSession: Identifiable, Equatable, Sendable {
    public let id: String
    public let name: String
    public let device: String?
    public var lastActive: Date?
    public var expiresAt: Date?
    public var revoked: Bool
    public var expired: Bool
    public init(id: String, name: String, device: String? = nil, lastActive: Date? = nil, expiresAt: Date? = nil,
                revoked: Bool = false, expired: Bool = false) {
        self.id = id; self.name = name; self.device = device; self.lastActive = lastActive; self.expiresAt = expiresAt
        self.revoked = revoked; self.expired = expired
    }
}

public struct AccessPolicy: Equatable, Sendable {
    public var guestFreezeUntil: Date?
    public var ownerHoldUntil: Date?
    public var guestPasswordRevision: UInt64
    public var guestFreezeActive: Bool
    public var ownerHoldActive: Bool
    public var frozenLightIDs: Set<String>
    public var ownerHeldLightIDs: Set<String>
    public init(guestFreezeUntil: Date? = nil, ownerHoldUntil: Date? = nil, guestPasswordRevision: UInt64 = 0,
                guestFreezeActive: Bool = false, ownerHoldActive: Bool = false,
                frozenLightIDs: Set<String> = [], ownerHeldLightIDs: Set<String> = []) {
        self.guestFreezeUntil = guestFreezeUntil; self.ownerHoldUntil = ownerHoldUntil; self.guestPasswordRevision = guestPasswordRevision
        self.guestFreezeActive = guestFreezeActive; self.ownerHoldActive = ownerHoldActive
        self.frozenLightIDs = frozenLightIDs; self.ownerHeldLightIDs = ownerHeldLightIDs
    }
    public func isGuestFrozen(at now: Date) -> Bool { guestFreezeActive || guestFreezeUntil.map { $0 > now } == true }
    public func guestRestriction(at now: Date, selectedIDs: Set<String>? = nil) -> String? {
        let freezeApplies = selectedIDs == nil || frozenLightIDs.isEmpty || !frozenLightIDs.isDisjoint(with: selectedIDs!)
        let holdApplies = selectedIDs == nil || ownerHeldLightIDs.isEmpty || !ownerHeldLightIDs.isDisjoint(with: selectedIDs!)
        if isGuestFrozen(at: now), freezeApplies {
            if let until = guestFreezeUntil, until > now { return "Guest controls are paused until \(until.formatted(date: .omitted, time: .shortened))." }
            return "The owner has paused guest controls for this selection."
        }
        if (ownerHoldActive || ownerHoldUntil.map { $0 > now } == true), holdApplies {
            if let until = ownerHoldUntil, until > now { return "The owner is keeping these settings until \(until.formatted(date: .omitted, time: .shortened))." }
            return "The owner is keeping the current settings for this selection."
        }
        return nil
    }
}

public enum OwnerCapability: String, Hashable, Sendable { case freeze, revoke, rotatePassword, remove, holdSettings, importHomeKey }

public struct RoomSnapshot: Equatable, Sendable {
    public var id: String
    public var name: String
    public var revision: UInt64
    public var role: AccessRole
    public var connection: ConnectionPresentation
    public var lights: [Light]
    public var groups: [LightGroup]
    public var scenes: [SceneOption]
    public var policy: AccessPolicy
    public var sessions: [GuestSession]
    public var observedAt: Date
    public var ownerCapabilities: Set<OwnerCapability>
    public var controlUnavailableReason: String?
    public init(id: String, name: String, revision: UInt64, role: AccessRole, connection: ConnectionPresentation,
                lights: [Light], groups: [LightGroup], scenes: [SceneOption], policy: AccessPolicy,
                sessions: [GuestSession], observedAt: Date, ownerCapabilities: Set<OwnerCapability> = [], controlUnavailableReason: String? = nil) {
        self.id = id; self.name = name; self.revision = revision; self.role = role; self.connection = connection
        self.lights = lights; self.groups = groups; self.scenes = scenes; self.policy = policy
        self.sessions = sessions; self.observedAt = observedAt
        self.ownerCapabilities = ownerCapabilities
        self.controlUnavailableReason = controlUnavailableReason
    }
}

public enum LightSelection: Hashable, Identifiable, Sendable {
    case room, group(String), light(String)
    public var id: String {
        switch self { case .room: "room"; case .group(let id): "group:\(id)"; case .light(let id): "light:\(id)" }
    }
}

public enum OwnerAction: Sendable {
    case freezeGuests(until: Date?)
    case holdSettings(until: Date?)
    case revokeSession(String)
    case revokeAllGuests
    case rotateGuestPassword(String)
    case removeLight(String)
    case importHomeKey(Data)
}

public enum GatewayAction: Sendable {
    case lighting(ids: [String], patch: WiZLightingPatch)
    case owner(OwnerAction)
}

public struct GatewayCommand: Sendable {
    public let operationID: UUID
    public let expectedRevision: UInt64
    public let action: GatewayAction
    public init(expectedRevision: UInt64, action: GatewayAction, operationID: UUID = UUID()) {
        self.expectedRevision = expectedRevision
        self.action = action
        self.operationID = operationID
    }
}

public struct GatewayOutcome: Sendable {
    public let snapshot: RoomSnapshot
    public let confirmedIDs: Set<String>
    public let failures: [String: String]
    public init(snapshot: RoomSnapshot, confirmedIDs: Set<String> = [], failures: [String: String] = [:]) {
        self.snapshot = snapshot
        self.confirmedIDs = confirmedIDs
        self.failures = failures
    }
}

public enum MenuletError: LocalizedError, Equatable {
    case unavailable, disconnected, ownerRequired, restricted(String), staleRevision, invalidCredential, invalidSelection
    case accessRevoked, accessExpired
    public var errorDescription: String? {
        switch self {
        case .unavailable: "Room lighting is not ready yet. The owner needs to complete gateway setup before you can connect."
        case .disconnected: "Connect to the room before changing its lights."
        case .ownerRequired: "Only the owner can change room access or bulb setup."
        case .restricted(let message): message
        case .staleRevision: "The room changed while this action was pending. Review the latest settings and try again."
        case .invalidCredential: "Enter the room address and an access credential."
        case .invalidSelection: "Choose a light that is still part of this room."
        case .accessRevoked: "Your room access was revoked. Controls have stopped. Rejoining requires an explicit sign-in with the current guest password."
        case .accessExpired: "Your room access expired. Controls have stopped. Sign in explicitly to rejoin."
        }
    }
}

/// The gateway adapter owns authentication and authorization. A UI refresh or
/// transport receipt alone must never be promoted into a confirmed bulb state.
@MainActor public protocol MenuletTransport: AnyObject {
    var canConnect: Bool { get }
    var isPreview: Bool { get }
    var unavailableReason: String? { get }
    func connect(_ request: AccessRequest) async throws -> RoomSnapshot
    func explicitlyRejoinGuest(_ request: AccessRequest) async throws -> RoomSnapshot
    func refresh() async throws -> RoomSnapshot
    func execute(_ command: GatewayCommand) async throws -> GatewayOutcome
    func disconnect() async
    func signOut() async throws
}

public extension MenuletTransport {
    /// Adapters with remembered credentials override this and report deletion
    /// failures. A transport-only close is a separate automatic-failover action.
    func signOut() async throws { await disconnect() }
    func explicitlyRejoinGuest(_ request: AccessRequest) async throws -> RoomSnapshot {
        guard request.role == .guest else { throw MenuletError.ownerRequired }
        return try await connect(request)
    }
}

@MainActor public final class UnconfiguredTransport: MenuletTransport {
    public let canConnect = false
    public let isPreview = false
    public let unavailableReason: String? = MenuletError.unavailable.localizedDescription
    public init() {}
    public func connect(_ request: AccessRequest) async throws -> RoomSnapshot { throw MenuletError.unavailable }
    public func refresh() async throws -> RoomSnapshot { throw MenuletError.unavailable }
    public func execute(_ command: GatewayCommand) async throws -> GatewayOutcome { throw MenuletError.unavailable }
    public func disconnect() async {}
}
