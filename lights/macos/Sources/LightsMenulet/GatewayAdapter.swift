import Foundation
import CoreFoundation
import CryptoKit
import Security
import LightsCore
import LightsSecurity

public enum GatewayAdapterError: LocalizedError, Equatable {
    case unavailable, busy, authenticationFailed, malformedReply, changedBoot, invalidName, unsupportedSetting
    case revoked, expired, rejoinRequired, staleState, operationUncertain, idReused
    case unsupportedOwnerHold, weakGuestPassword, rejected(String)
    public var errorDescription: String? {
        switch self {
        case .unavailable: "This room is not accepting light connections yet."
        case .busy: "The room is busy with another connection. Try again shortly; a disconnected owner session may take up to a minute to expire."
        case .authenticationFailed: "Could not verify this room or credential. Check the access key and trusted room invitation."
        case .malformedReply: "The room returned an invalid response. Reconnect before making changes."
        case .changedBoot: "The gateway restarted. Reconnect to read its current state."
        case .invalidName: "Choose a shorter display name without control characters."
        case .unsupportedSetting: "One or more selected lights do not support these settings."
        case .revoked: "This guest access was revoked. It will not be recreated automatically."
        case .expired: "This guest access expired. Rejoin explicitly with the current room password."
        case .rejoinRequired: "This saved grant needs an explicit rejoin with the current room password."
        case .staleState: "The light or room policy changed. Refresh and review the current settings."
        case .operationUncertain: "The reply was lost. The operation will not be sent again automatically; refresh its status first."
        case .idReused: "This operation identifier was already used for a different request."
        case .unsupportedOwnerHold: "A hold can accompany a new lighting change. This gateway does not support changing a hold on its own."
        case .weakGuestPassword: "Use a longer guest password or passphrase, no more than 256 bytes."
        case .rejected(let code): Self.rejectionMessage(code)
        }
    }
    private static func rejectionMessage(_ code: String) -> String {
        switch code {
        case "frozen": "The owner has paused guest changes for these lights."
        case "owner_hold": "The owner is keeping the current settings for these lights."
        case "overloaded", "driver_busy", "discovery_busy": "The room is busy. Wait briefly before trying again."
        case "storage_maintenance", "not_ready", "not_provisioned": "Room lighting is unavailable until the owner completes setup or recovery."
        case "discovery_unavailable": "Light discovery is not available on this gateway yet."
        case "discovery_required", "adoption_rejected": "Discover the light again and verify its setup before adding it."
        case "unsupported_setting": "The selected lights do not support these settings."
        default: "The gateway rejected this operation. Refresh the room before trying again."
        }
    }
}

public protocol GatewayAccessStore: Sendable {
    func read(identity: Data, kind: StoredSecretKind) throws -> Data?
    func save(_ value: Data, identity: Data, kind: StoredSecretKind) throws
    func remove(identity: Data, kind: StoredSecretKind) throws
}
extension KeychainSecretStore: GatewayAccessStore {
    public func read(identity: Data, kind: StoredSecretKind) throws -> Data? { try read(SecretRecordID(trustedGatewayIdentity: identity, kind: kind)) }
    public func save(_ value: Data, identity: Data, kind: StoredSecretKind) throws { try save(value, for: SecretRecordID(trustedGatewayIdentity: identity, kind: kind)) }
    public func remove(identity: Data, kind: StoredSecretKind) throws { try remove(SecretRecordID(trustedGatewayIdentity: identity, kind: kind)) }
}

@MainActor public protocol GatewayAdapterClock {
    var wallTime: Date { get }
    var uptime: TimeInterval { get }
    func sleep(seconds: TimeInterval) async throws
}
public struct SystemGatewayAdapterClock: GatewayAdapterClock {
    public init() {}
    public var wallTime: Date { Date() }
    public var uptime: TimeInterval { ProcessInfo.processInfo.systemUptime }
    public func sleep(seconds: TimeInterval) async throws { try await Task.sleep(for: .seconds(max(0, seconds))) }
}

public struct GatewayAdapterRoute: Sendable {
    public let channel: any GatewayByteChannel
    public let route: GatewayRoute
    public init(channel: any GatewayByteChannel, route: GatewayRoute) { self.channel = channel; self.route = route }
}
public struct GatewayCandidate: Identifiable, Sendable, Equatable {
    public let id: Int
    public let mac: String
    public let identified: Bool
    public let module: String
    public let firmware: String
    public init(id: Int, mac: String, identified: Bool, module: String, firmware: String) {
        self.id = id; self.mac = mac; self.identified = identified; self.module = module; self.firmware = firmware
    }
}
public struct GatewayDiscoveryPage: Sendable {
    public let discovering: Bool
    public let total: Int
    public let candidate: GatewayCandidate?
    public init(discovering: Bool, total: Int, candidate: GatewayCandidate?) {
        self.discovering = discovering; self.total = total; self.candidate = candidate
    }
}

/// The concrete application adapter. Its default gate stays closed until an
/// integrator supplies a trusted descriptor, a real channel, and qualification.
/// There is no preview, UDP bypass, credential logging or network discovery here.
@MainActor public final class GatewayAdapter: MenuletTransport {
    public let isPreview = false
    public let canConnect: Bool
    public var unavailableReason: String? { canConnect ? nil : GatewayAdapterError.unavailable.localizedDescription }
    public private(set) var snapshot: RoomSnapshot?
    /// Available only while the SRP and pinned gateway binding remain open.
    public var authenticatedBootIdentifier: String? { boot }
    private let trusted: TrustedGatewayIdentity
    private let factory: @MainActor @Sendable (AccessRequest, Set<GatewayRoute>) async throws -> GatewayAdapterRoute
    private let onRouteResult: @MainActor @Sendable (GatewayRoute, Bool, Double?) -> Void
    private let store: any GatewayAccessStore
    private let clock: any GatewayAdapterClock
    private let roomName: String
    private var pipe: SecureGatewayPipe?
    private var boot: String?
    private var role: AccessRole?
    private var route: GatewayRoute?
    private var busy = false
    private var generation: UInt64 = 0
    private var snapshotRevision: UInt64 = 0
    private var lastRoom: RoomRecord?
    private var slotByID: [String: Int] = [:]
    private var grant: GrantRecord?
    private var remember = false
    private var operations: [UUID: OperationRecord] = [:]
    private var operationOrder: [UUID] = []
    private var ownerHoldMilliseconds: UInt32 = 0
    private var connectionRequest: AccessRequest?
    private var runtimePassword = Data()
    private var failedRoute: GatewayRoute?
    private var lastExchangeUptime: TimeInterval?

    public init(trustedIdentity: TrustedGatewayIdentity, enabled: Bool = false,
                roomName: String = "Room 203", store: any GatewayAccessStore = KeychainSecretStore(),
                clock: any GatewayAdapterClock = SystemGatewayAdapterClock(),
                onRouteResult: @escaping @MainActor @Sendable (GatewayRoute, Bool, Double?) -> Void = { _, _, _ in },
                channelFactory: @escaping @MainActor @Sendable (AccessRequest, Set<GatewayRoute>) async throws -> GatewayAdapterRoute) {
        trusted = trustedIdentity; canConnect = enabled; self.roomName = roomName
        self.store = store; self.clock = clock; factory = channelFactory; self.onRouteResult = onRouteResult
    }

    public func connect(_ request: AccessRequest) async throws -> RoomSnapshot { try await connect(request, newGuest: false) }

    /// An explicit user action, never called in response to revocation/expiry or
    /// transport failure. Knowing a shared password permits this re-enrollment.
    public func explicitlyRejoinGuest(_ request: AccessRequest) async throws -> RoomSnapshot {
        guard request.role == .guest else { throw MenuletError.ownerRequired }
        return try await connect(request, newGuest: true)
    }

    private func connect(_ request: AccessRequest, newGuest: Bool) async throws -> RoomSnapshot {
        guard canConnect else { throw GatewayAdapterError.unavailable }
        return try await serialized(renewIdle: false) {
            await self.closePipe()
            let epoch = self.generation
            self.remember = request.rememberAccess
            let secretKind: StoredSecretKind = request.role == .owner ? .ownerKey : .guestPassword
            var password = Data(request.credential.utf8)
            defer { password.resetBytes(in: password.startIndex..<password.endIndex); password.removeAll() }
            if password.isEmpty, request.rememberAccess { password = try self.store.read(identity: self.trusted.publicKeySPKI, kind: secretKind) ?? Data() }
            guard !password.isEmpty, password.count <= 256 else { throw MenuletError.invalidCredential }
            let displayName = request.name.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty ? "Guest" : request.name
            if request.role == .guest {
                guard Self.validName(displayName) else { throw GatewayAdapterError.invalidName }
                if newGuest { self.grant = nil }
                else if self.grant == nil, request.rememberAccess,
                        let saved = try self.store.read(identity: self.trusted.publicKeySPKI, kind: .guestGrant) {
                    self.grant = try GrantRecord.decode(saved)
                }
                if let grant = self.grant, grant.state == .blocked { throw GatewayAdapterError.rejoinRequired }
            }
            let metadata = AccessRequest(endpoint: request.endpoint, name: displayName, role: request.role,
                                         credential: "", preference: request.preference, rememberAccess: request.rememberAccess,
                                         nearbyPeripheralID: request.nearbyPeripheralID)
            self.connectionRequest = metadata
            self.runtimePassword = password
            do {
                try await self.openConnection(metadata, excluding: [])
                try self.valid(epoch)
                if request.rememberAccess { try self.store.save(password, identity: self.trusted.publicKeySPKI, kind: secretKind) }
                if request.role == .guest { try await self.authorizeGuest(name: displayName) }
                return try await self.readSnapshot()
            } catch {
                if epoch == self.generation {
                    await self.closePipe()
                    self.runtimePassword.resetBytes(in: self.runtimePassword.startIndex..<self.runtimePassword.endIndex)
                    self.runtimePassword.removeAll(); self.connectionRequest = nil
                }
                throw error
            }
        }
    }

    public func refresh() async throws -> RoomSnapshot {
        try await serialized {
            do { return try await self.readSnapshot() }
            catch {
                guard Self.isTransportFailure(error) else { throw error }
                try await self.recover(original: error)
                return try await self.readSnapshot()
            }
        }
    }

    public func execute(_ command: GatewayCommand) async throws -> GatewayOutcome {
        try await serialized {
            guard let current = self.snapshot, self.pipe != nil else { throw MenuletError.disconnected }
            let fingerprint = try self.fingerprint(command.action)
            if let prior = self.operations[command.operationID] {
                guard prior.fingerprint == fingerprint else { throw GatewayAdapterError.idReused }
                if let completed = prior.completed { return try await self.refreshCompleted(prior, completed) }
                guard !prior.targets.isEmpty else { throw GatewayAdapterError.operationUncertain }
                return try await self.resolve(command.operationID, targets: prior.targets)
            }
            guard command.expectedRevision == current.revision else { throw GatewayAdapterError.staleState }
            do {
                switch command.action {
                case .lighting(let ids, let patch):
                    return try await self.set(command, ids: ids, patch: patch, fingerprint: fingerprint)
                case .owner(let action):
                    guard self.role == .owner else { throw MenuletError.ownerRequired }
                    return try await self.ownerAction(command.operationID, action, fingerprint: fingerprint)
                }
            } catch {
                guard Self.isTransportFailure(error) else { throw error }
                try await self.recover(original: error)
                if let submitted = self.operations[command.operationID], !submitted.targets.isEmpty {
                    return try await self.resolve(command.operationID, targets: submitted.targets)
                }
                _ = try await self.readSnapshot()
                throw self.operations[command.operationID] == nil ? GatewayAdapterError.staleState : GatewayAdapterError.operationUncertain
            }
        }
    }

    /// This changes only the hold attached to subsequent owner lighting intents.
    /// It does not resend existing bulb state or manipulate a hold on its own.
    public func keepFutureOwnerChanges(milliseconds: UInt32) throws {
        guard role == .owner else { throw MenuletError.ownerRequired }
        guard milliseconds <= 60_000 else { throw GatewayAdapterError.unsupportedOwnerHold }
        ownerHoldMilliseconds = milliseconds
    }

    public func disconnect() async {
        generation &+= 1
        busy = false
        await closePipe()
        runtimePassword.resetBytes(in: runtimePassword.startIndex..<runtimePassword.endIndex)
        runtimePassword.removeAll(); connectionRequest = nil; failedRoute = nil
    }

    public func forgetRememberedAccess() async throws {
        await disconnect()
        var failure: Error?
        for kind in [StoredSecretKind.ownerKey, .guestPassword, .guestGrant] {
            do { try store.remove(identity: trusted.publicKeySPKI, kind: kind) }
            catch { if failure == nil { failure = error } }
        }
        grant = nil; remember = false
        if let failure { throw failure }
    }
    public func signOut() async throws { try await forgetRememberedAccess() }

    public func startDiscovery() async throws {
        try await serialized { try self.requireOwner(); _ = try await self.success("discover", [:]) }
    }
    public func cancelDiscovery() async throws {
        try await serialized { try self.requireOwner(); _ = try await self.success("discovery_cancel", [:]) }
    }
    public func candidate(at index: Int) async throws -> GatewayDiscoveryPage {
        try await serialized {
            try self.requireOwner()
            guard (0...7).contains(index) else { throw MenuletError.invalidSelection }
            let reply = try await self.success("candidate", ["index": index], required: ["discovering", "count", "present"],
                                               optional: ["mac", "identified", "module", "firmware", "reply_trust", "error"])
            let present = try reply.bool("present"), count = try reply.integer("count", in: 0...8)
            guard present == (index < count) else { throw GatewayAdapterError.malformedReply }
            var candidate: GatewayCandidate?
            if present {
                try reply.keys(required: GatewayObject.base.union(["discovering", "count", "present", "mac", "identified", "module", "firmware", "reply_trust", "error"]), optional: [])
                _ = try reply.string("error", maximum: 48)
                guard try reply.string("reply_trust") == "unverified_lan" else { throw GatewayAdapterError.malformedReply }
                candidate = try GatewayCandidate(id: index, mac: reply.mac("mac"), identified: reply.bool("identified"),
                                                  module: reply.string("module", maximum: 47), firmware: reply.string("firmware", maximum: 23))
            } else {
                try reply.keys(required: GatewayObject.base.union(["discovering", "count", "present"]), optional: [])
            }
            return try GatewayDiscoveryPage(discovering: reply.bool("discovering"), total: count, candidate: candidate)
        }
    }
    public func adoptCandidate(_ index: Int, name: String, allowLegacyControl: Bool) async throws -> RoomSnapshot {
        try await serialized {
            try self.requireOwner()
            guard (0...7).contains(index), Self.validName(name) else { throw GatewayAdapterError.invalidName }
            let reply = try await self.success("adopt", ["candidate": index, "name": name, "legacy": allowLegacyControl], required: ["index"])
            _ = try reply.integer("index", in: 0...7)
            return try await self.readSnapshot()
        }
    }
    public func importWiZHomeKey(_ key: Data) async throws -> RoomSnapshot {
        try await serialized {
            try self.requireOwner()
            guard key.count == 16 else { throw WiZProtocolError.invalidSigningKey }
            _ = try await self.success("home_key", ["key": Self.hex(key)])
            return try await self.readSnapshot()
        }
    }

    private func set(_ command: GatewayCommand, ids: [String], patch: WiZLightingPatch, fingerprint: Data) async throws -> GatewayOutcome {
        guard !ids.isEmpty, ids.count <= 8, Set(ids).count == ids.count, let prior = snapshot else { throw MenuletError.invalidSelection }
        let selected = try ids.map { id -> Light in
            guard let light = prior.lights.first(where: { $0.id == id }), slotByID[id] != nil else { throw MenuletError.invalidSelection }
            return light
        }
        var fresh: [LightRecord] = []
        for light in selected {
            let item = try await readLight(slotByID[light.id]!)
            guard item.mac == light.id, UInt64(item.revision) == light.revision, item.observedValid else { throw GatewayAdapterError.staleState }
            fresh.append(item)
        }
        var effective = patch
        if patch.effectSpeed != nil, patch.sceneID == nil {
            let scenes = Set(fresh.compactMap { $0.reported.sceneID })
            guard scenes.count == 1, fresh.allSatisfy({ $0.reported.sceneID != nil }),
                  let scene = scenes.first, scene > 0, patch.rgb == nil, patch.colorTemperature == nil else {
                throw GatewayAdapterError.unsupportedSetting
            }
            effective = WiZLightingPatch(power: patch.power, brightnessPercent: patch.brightnessPercent, rgb: patch.rgb,
                                          colorTemperature: patch.colorTemperature, sceneID: scene, effectSpeed: patch.effectSpeed,
                                          dualZoneRatio: patch.dualZoneRatio, whiteChannels: patch.whiteChannels)
        }
        for item in fresh { try Self.validate(effective, capabilities: item.capabilities) }
        let room = try await readRoom()
        guard room.storageHealthy else { throw GatewayAdapterError.rejected("storage_maintenance") }
        guard let lastRoom, room.policyEpoch == lastRoom.policyEpoch else { throw GatewayAdapterError.staleState }
        var mask = 0, expected = [UInt32](repeating: 0, count: 8)
        for light in fresh {
            guard light.controlUnavailableReason(signingClockReady: room.signingClockReady) == nil else {
                throw GatewayAdapterError.rejected("not_ready")
            }
            guard room.enabled & (1 << light.index) != 0, light.observedMS <= room.nowMS,
                  room.nowMS - light.observedMS <= 60_000 else { throw GatewayAdapterError.staleState }
            mask |= 1 << light.index; expected[light.index] = light.revision
        }
        guard room.lastSequence < UInt64.max, room.nowMS <= UInt64.max - 1500 else { throw GatewayAdapterError.staleState }
        var fields: [String: Any] = ["id": Self.uuid(command.operationID), "boot": room.boot,
                                    "policy_epoch": String(room.policyEpoch), "sequence": String(room.lastSequence + 1),
                                    "expires_ms": String(room.nowMS + 1500), "targets": mask,
                                    "expected": expected, "patch": Self.patch(effective)]
        if role == .owner, ownerHoldMilliseconds > 0 { fields["hold_ms"] = ownerHoldMilliseconds }
        try record(command.operationID, fingerprint: fingerprint, targets: Set(ids), desired: effective,
                   indices: Dictionary(uniqueKeysWithValues: fresh.map { ($0.mac, $0.index) }))
        let reply = try await exchange("set", fields)
        let receipt: Receipt
        do {
            if try !reply.bool("ok") { try await reject(reply.string("code", maximum: 64), operation: "set") }
            receipt = try parseReceipt(reply, id: command.operationID, requestedMask: mask)
        } catch {
            if try !reply.bool("ok"), let snapshot {
                operations[command.operationID]?.completed = GatewayOutcome(snapshot: snapshot, failures: Dictionary(uniqueKeysWithValues: ids.map { ($0, error.localizedDescription) }))
            }
            throw error
        }
        return try await resolve(command.operationID, targets: Set(ids), first: receipt)
    }

    private func resolve(_ id: UUID, targets: Set<String>, first: Receipt? = nil) async throws -> GatewayOutcome {
        guard let operation = operations[id], let desired = operation.desired else { throw GatewayAdapterError.operationUncertain }
        let mask = targets.reduce(0) { $0 | (operation.indices[$1].map { 1 << $0 } ?? 0) }
        var receipt = first
        for _ in 0..<6 {
            if let receipt, receipt.terminal { break }
            let reply = try await exchange("command", ["id": Self.uuid(id)])
            if try !reply.bool("ok") { try await reject(reply.string("code", maximum: 64), operation: "command") }
            receipt = try parseReceipt(reply, id: id, requestedMask: mask)
        }
        guard let receipt else { throw GatewayAdapterError.operationUncertain }
        let updated = try await readSnapshot()
        var confirmed = Set<String>(), failures: [String: String] = [:]
        for target in targets {
            guard let index = operation.indices[target], slotByID[target] == index,
                  let light = updated.lights.first(where: { $0.id == target }) else {
                failures[target] = "The light is no longer enrolled."; continue
            }
            let bit = 1 << index
            if receipt.verified & bit != 0, light.reported.observedAt != nil, !light.reportPending,
               Self.matches(desired, observed: light.reported) {
                confirmed.insert(target)
            } else if receipt.verified & bit != 0 {
                failures[target] = "The light changed after this operation, or its latest state is not yet confirmed."
            } else if !receipt.terminal {
                failures[target] = "Still awaiting a bulb response; the setting is not confirmed."
            } else if receipt.failed & bit != 0 {
                failures[target] = "The bulb did not confirm this setting."
            } else {
                failures[target] = "The operation ended without a confirmed setting for this light."
            }
        }
        let outcome = GatewayOutcome(snapshot: updated, confirmedIDs: confirmed, failures: failures)
        if receipt.terminal { operations[id]?.completed = outcome }
        return outcome
    }

    private func refreshCompleted(_ operation: OperationRecord, _ completed: GatewayOutcome) async throws -> GatewayOutcome {
        let current = try await readSnapshot()
        var confirmed = Set<String>(), failures = completed.failures
        for id in completed.confirmedIDs {
            if let desired = operation.desired, let light = current.lights.first(where: { $0.id == id }),
               operation.indices[id] == slotByID[id], light.reported.observedAt != nil, !light.reportPending,
               Self.matches(desired, observed: light.reported) { confirmed.insert(id) }
            else { failures[id] = "The light has changed since this operation completed." }
        }
        return GatewayOutcome(snapshot: current, confirmedIDs: confirmed, failures: failures)
    }

    private func ownerAction(_ id: UUID, _ action: OwnerAction, fingerprint: Data) async throws -> GatewayOutcome {
        let room = try await readRoom()
        guard room.storageHealthy else { throw GatewayAdapterError.rejected("storage_maintenance") }
        var op: String, fields: [String: Any] = [:], required = Set<String>()
        switch action {
        case .freezeGuests(let until):
            guard room.enabled != 0 else { throw MenuletError.invalidSelection }
            let seconds = until?.timeIntervalSince(clock.wallTime) ?? 0
            guard seconds.isFinite else { throw GatewayAdapterError.staleState }
            op = "freeze"; fields = ["targets": room.enabled, "duration_ms": UInt64(min(86_400, max(0, seconds)) * 1000)]
            required = ["policy_epoch"]
        case .holdSettings: throw GatewayAdapterError.unsupportedOwnerHold
        case .revokeSession(let grantID):
            _ = try GatewayObject.decimal(grantID)
            op = "revoke"; fields = ["grant": grantID]; required = ["policy_epoch"]
        case .revokeAllGuests: op = "revoke_all"; required = ["policy_epoch"]
        case .rotateGuestPassword(let password):
            guard (12...256).contains(password.utf8.count) else { throw GatewayAdapterError.weakGuestPassword }
            let profile = try await Task.detached(priority: .utility) { try SRPVerifier.generate(username: "203-guest", password: Data(password.utf8)) }.value
            try Task.checkCancellation()
            op = "rotate_guest"; fields = ["salt": Self.hex(profile.salt), "verifier": Self.hex(profile.verifier)]; required = ["policy_epoch"]
        case .importHomeKey(let key):
            guard key.count == 16 else { throw WiZProtocolError.invalidSigningKey }
            op = "home_key"; fields = ["key": Self.hex(key)]
        case .removeLight(let id):
            guard let index = slotByID[id], room.enabled & (1 << index) != 0 else { throw MenuletError.invalidSelection }
            op = "remove"; fields = ["index": index]
        }
        try record(id, fingerprint: fingerprint, targets: [])
        let reply = try await success(op, fields, required: required)
        if required.contains("policy_epoch") { _ = try reply.decimal("policy_epoch") }
        let outcome = GatewayOutcome(snapshot: try await readSnapshot())
        operations[id]?.completed = outcome
        return outcome
    }

    private func openConnection(_ request: AccessRequest, excluding initial: Set<GatewayRoute>) async throws {
        let epoch = generation
        var excluded = initial
        var lastError: Error = GatewayAdapterError.unavailable
        for _ in 0..<2 {
            let selected: GatewayAdapterRoute
            do { selected = try await factory(request, excluded) }
            catch { if excluded.isEmpty { throw error }; throw lastError }
            try valid(epoch)
            guard !excluded.contains(selected.route) else { throw lastError }
            excluded.insert(selected.route)
            let secure = SecureGatewayPipe(channel: selected.channel, trustedIdentity: trusted)
            pipe = secure
            do {
                let context = try await secure.authenticate(username: request.role == .owner ? "203-owner" : "203-guest",
                                                            password: runtimePassword, role: request.role == .owner ? .owner : .guest)
                try valid(epoch)
                boot = context.boot; role = request.role; route = selected.route; failedRoute = nil
                lastExchangeUptime = clock.uptime
                onRouteResult(selected.route, true, await secure.lastRoundTripMilliseconds)
                return
            } catch {
                let reported: Error
                let status: Int?
                if let relay = error as? RelayChannelError, case .rejected(let code) = relay { status = code }
                else if let bluetooth = error as? BluetoothChannelError, case .rejected(let code) = bluetooth { status = code }
                else { status = nil }
                if let status, [409, 429].contains(status) {
                    reported = GatewayAdapterError.busy
                } else if error is Security2Error || status == 401 || status == 403 { reported = GatewayAdapterError.authenticationFailed }
                else { reported = error }
                lastError = reported
                onRouteResult(selected.route, false, nil)
                await secure.close()
                if generation == epoch { pipe = nil }
                try valid(epoch)
                if reported as? GatewayAdapterError == .busy || reported as? GatewayAdapterError == .authenticationFailed { throw reported }
                guard request.preference == .automatic, Self.isTransportFailure(error) else { throw reported }
            }
        }
        throw lastError
    }

    private func authorizeGuest(name: String) async throws {
        if grant == nil { grant = try GrantRecord.new(name: name) }
        guard grant?.state != .blocked else { throw GatewayAdapterError.rejoinRequired }
        try saveGrant()
        let access = grant!
        var fields: [String: Any] = ["token": access.token]
        let op = access.state == .pending ? "guest.enroll" : "guest.resume"
        if access.state == .pending { fields["name"] = access.name }
        let reply = try await success(op, fields, required: ["grant", "guest_epoch"])
        let id = try reply.decimal("grant"), guestEpoch = try reply.decimal("guest_epoch")
        guard id > 0, guestEpoch > 0 else { throw GatewayAdapterError.malformedReply }
        grant?.id = id; grant?.guestEpoch = guestEpoch; grant?.state = .active
        try saveGrant()
    }

    private func recover(original: Error) async throws {
        guard let request = connectionRequest, request.preference == .automatic,
              let failedRoute, !runtimePassword.isEmpty else { throw original }
        await closePipe()
        try await openConnection(request, excluding: [failedRoute])
        if request.role == .guest { try await authorizeGuest(name: request.name) }
    }

    private func renewIdleSessionIfNeeded() async throws {
        guard pipe != nil, let lastExchangeUptime, clock.uptime - lastExchangeUptime >= 55,
              let request = connectionRequest, !runtimePassword.isEmpty else { return }
        let priorBoot = boot, priorRoom = lastRoom
        await closePipe()
        // Expected idle expiry is not a failed route sample. Reopen the available
        // path with fresh SRP instead of requiring an unused alternate transport.
        try await openConnection(request, excluding: [])
        if request.role == .guest { try await authorizeGuest(name: request.name) }
        guard boot == priorBoot else { _ = try await readSnapshot(); throw GatewayAdapterError.staleState }
        lastRoom = priorRoom
    }

    private nonisolated static func isTransportFailure(_ error: Error) -> Bool {
        error is RelayChannelError || error is BluetoothChannelError || error is NearbyDiscoveryError ||
        error is SecureGatewayPipeError || error is URLError
    }

    private func readSnapshot() async throws -> RoomSnapshot {
        for attempt in 0..<2 {
            let initial = try await readRoom()
            var lights = [LightRecord]()
            for index in 0..<8 where initial.enabled & (1 << index) != 0 { lights.append(try await readLight(index)) }
            var sessions = [SessionRecord]()
            if role == .owner {
                for page in 0..<16 where initial.guestSlots & (1 << page) != 0 { sessions.append(try await readSession(page)) }
            }
            let final = try await readRoom()
            if initial.enabled != final.enabled || initial.policyEpoch != final.policyEpoch || initial.guestSlots != final.guestSlots {
                if attempt == 0 { continue }; throw GatewayAdapterError.staleState
            }
            let now = clock.wallTime
            let mapped = try lights.map { try $0.light(now: now, gatewayNow: final.nowMS, signingClockReady: final.signingClockReady) }
            guard Set(mapped.map(\.id)).count == mapped.count else { throw GatewayAdapterError.malformedReply }
            let positions = Dictionary(uniqueKeysWithValues: lights.map { ($0.mac, $0.index) })
            let sceneIDs = mapped.reduce(into: Set<Int>()) { $0.formUnion($1.capabilities.supportedSceneIDs) }
            guard snapshotRevision < UInt64.max else { throw GatewayAdapterError.staleState }
            snapshotRevision += 1
            let snapshot = RoomSnapshot(id: trusted.gatewayID, name: roomName, revision: snapshotRevision, role: role!,
                                        connection: route == .bluetooth ? .bluetooth : .internet, lights: mapped, groups: [],
                                        scenes: sceneIDs.sorted().map { SceneOption(id: $0, name: Self.sceneName($0), symbol: "sparkles") },
                                        policy: AccessPolicy(guestPasswordRevision: final.guestEpoch,
                                                             guestFreezeActive: final.frozen != 0, ownerHoldActive: final.ownerHeld != 0,
                                                             frozenLightIDs: Set(mapped.filter { final.frozen & (1 << positions[$0.id]!) != 0 }.map(\.id)),
                                                             ownerHeldLightIDs: Set(mapped.filter { final.ownerHeld & (1 << positions[$0.id]!) != 0 }.map(\.id))),
                                        sessions: sessions.map { $0.model(now: now, gatewayNow: final.nowMS) }, observedAt: now,
                                        ownerCapabilities: role == .owner ? [.freeze, .revoke, .rotatePassword, .remove, .importHomeKey] : [],
                                        controlUnavailableReason: final.storageHealthy ? nil : "Room settings need recovery before lights can be changed.")
            lastRoom = final; slotByID = positions; self.snapshot = snapshot
            return snapshot
        }
        throw GatewayAdapterError.staleState
    }

    private func readRoom() async throws -> RoomRecord {
        let reply = try await success("room", [:], required: ["boot", "now_ms", "policy_epoch", "guest_epoch", "last_sequence", "enabled", "frozen", "owner_held", "storage_healthy", "driver_busy"], optional: ["guest_slots"], additionalRequired: ["bulb_clock_ready", "max_lights"])
        let room = try RoomRecord(reply, owner: role == .owner)
        guard room.boot == boot else { await closePipe(); throw GatewayAdapterError.changedBoot }
        return room
    }
    private func readLight(_ index: Int) async throws -> LightRecord {
        let reply = try await success("light", ["index": index], required: ["index", "name", "mac", "revision", "observed_valid", "observed_ms", "pending", "last_result", "reported", "capabilities", "reply_trust", "legacy_control", "error", "permission_enforcement"])
        let record = try LightRecord(reply)
        guard record.index == index else { throw GatewayAdapterError.malformedReply }
        return record
    }
    private func readSession(_ page: Int) async throws -> SessionRecord {
        let reply = try await success("sessions", ["page": page], required: ["page", "present"], optional: ["grant", "name", "expires_ms", "revoked", "expired"])
        guard try reply.integer("page", in: 0...15) == page, try reply.bool("present") else { throw GatewayAdapterError.staleState }
        return try SessionRecord(reply)
    }

    private func exchange(_ op: String, _ fields: [String: Any]) async throws -> GatewayObject {
        guard let pipe, boot != nil, role != nil else { throw MenuletError.disconnected }
        let epoch = generation
        var object = fields; object["v"] = 1; object["op"] = op
        let data = try JSONSerialization.data(withJSONObject: object, options: [.sortedKeys, .withoutEscapingSlashes])
        guard data.count <= 1008 else { throw GatewayAdapterError.unsupportedSetting }
        try valid(epoch)
        do {
            let reply = try await pipe.request(data)
            try valid(epoch)
            let parsed = try GatewayObject(reply, envelope: true)
            lastExchangeUptime = clock.uptime
            if let route { onRouteResult(route, true, await pipe.lastRoundTripMilliseconds) }
            return parsed
        } catch {
            if generation == epoch {
                if let route { failedRoute = route; onRouteResult(route, false, nil) }
                await closePipe()
            }
            throw error
        }
    }
    private func success(_ op: String, _ fields: [String: Any], required: Set<String> = [], optional: Set<String> = [], additionalRequired: Set<String> = []) async throws -> GatewayObject {
        let reply = try await exchange(op, fields)
        guard try reply.bool("ok") else {
            try reply.keys(required: GatewayObject.base, optional: [])
            let code = try reply.string("code", maximum: 64)
            try await reject(code, operation: op)
            throw GatewayAdapterError.rejected(code)
        }
        try reply.keys(required: GatewayObject.base.union(required).union(additionalRequired), optional: optional)
        return reply
    }
    private func reject(_ code: String, operation: String) async throws {
        if role == .guest,
           ["revoked", "expired", "unauthorized"].contains(code) {
            grant?.state = .blocked
            await closePipe()
            try saveGrant()
            throw code == "expired" ? MenuletError.accessExpired : MenuletError.accessRevoked
        }
        if code == "wrong_boot" { await closePipe(); throw GatewayAdapterError.changedBoot }
        if ["conflict", "policy_changed", "stale_sequence", "fresh_read_required"].contains(code) { throw GatewayAdapterError.staleState }
        if code == "unauthorized" { throw MenuletError.ownerRequired }
    }
    private func parseReceipt(_ reply: GatewayObject, id: UUID, requestedMask: Int) throws -> Receipt {
        let receiptKeys: Set<String> = ["id", "result", "error", "sent", "verified", "failed"]
        if try !reply.bool("ok") {
            try reply.keys(required: GatewayObject.base, optional: receiptKeys)
            let code = try reply.string("code", maximum: 64)
            if code == "not_found" { throw GatewayAdapterError.operationUncertain }
            if ["conflict", "policy_changed", "stale_sequence", "fresh_read_required"].contains(code) { throw GatewayAdapterError.staleState }
            if code == "id_reuse" { throw GatewayAdapterError.idReused }
            throw GatewayAdapterError.rejected(code)
        }
        try reply.keys(required: GatewayObject.base.union(receiptKeys), optional: [])
        let receipt = try Receipt(reply)
        guard receipt.id == Self.uuid(id), (receipt.sent | receipt.verified | receipt.failed) & ~requestedMask == 0,
              receipt.verified & receipt.failed == 0, receipt.verified & ~receipt.sent == 0 else { throw GatewayAdapterError.malformedReply }
        return receipt
    }
    private func serialized<T>(renewIdle: Bool = true, _ work: @MainActor () async throws -> T) async throws -> T {
        guard !busy else { throw GatewayAdapterError.busy }
        busy = true
        let epoch = generation
        defer { if epoch == generation { busy = false } }
        do {
            if renewIdle { try await renewIdleSessionIfNeeded() }
            let result = try await work(); try valid(epoch); return result
        }
        catch {
            if (error as? GatewayAdapterError == .malformedReply || error is WiZProtocolError || error is CancellationError), epoch == generation { await closePipe() }
            throw error
        }
    }
    private func valid(_ epoch: UInt64) throws { try Task.checkCancellation(); guard generation == epoch else { throw SecureGatewayPipeError.changedSession } }
    private func requireOwner() throws { guard role == .owner, pipe != nil else { throw MenuletError.ownerRequired } }
    private func closePipe() async {
        let old = pipe; pipe = nil; boot = nil; role = nil; route = nil; lastRoom = nil; lastExchangeUptime = nil
        ownerHoldMilliseconds = 0
        if snapshot != nil { snapshot!.connection = .offline }
        await old?.close()
    }
    private func saveGrant() throws {
        if remember, let grant { try store.save(JSONEncoder().encode(grant), identity: trusted.publicKeySPKI, kind: .guestGrant) }
    }
    private func record(_ id: UUID, fingerprint: Data, targets: Set<String>, desired: WiZLightingPatch? = nil, indices: [String: Int] = [:]) throws {
        if operations.count >= 32 {
            guard let removable = operationOrder.first(where: { operations[$0]?.completed != nil }) else { throw GatewayAdapterError.busy }
            operations[removable] = nil; operationOrder.removeAll { $0 == removable }
        }
        operations[id] = OperationRecord(fingerprint: fingerprint, targets: targets, desired: desired, indices: indices, completed: nil)
        operationOrder.append(id)
    }
    private func fingerprint(_ action: GatewayAction) throws -> Data {
        let object: [String: Any]
        switch action {
        case .lighting(let ids, let patch):
            guard patch.dualZoneRatio == nil, patch.whiteChannels == nil else { throw GatewayAdapterError.unsupportedSetting }
            object = ["lights": ids.sorted(), "patch": Self.patch(patch)]
        case .owner(let action):
            switch action {
            case .freezeGuests(let date): object = ["owner": "freeze", "until": date?.timeIntervalSince1970 ?? 0]
            case .holdSettings(let date): object = ["owner": "hold", "until": date?.timeIntervalSince1970 ?? 0]
            case .revokeSession(let id): object = ["owner": "revoke", "grant": id]
            case .revokeAllGuests: object = ["owner": "revoke_all"]
            case .rotateGuestPassword(let password): object = ["owner": "rotate", "credential_digest": Self.hex(Data(SHA256.hash(data: Data(password.utf8))))]
            case .importHomeKey(let key): object = ["owner": "home_key", "key_digest": Self.hex(Data(SHA256.hash(data: key)))]
            case .removeLight(let id): object = ["owner": "remove", "id": id]
            }
        }
        return try Data(SHA256.hash(data: JSONSerialization.data(withJSONObject: object, options: [.sortedKeys])))
    }
    private static func validate(_ patch: WiZLightingPatch, capabilities: WiZCapabilities) throws {
        guard patch.dualZoneRatio == nil, patch.whiteChannels == nil else { throw GatewayAdapterError.unsupportedSetting }
        if let brightness = patch.brightnessPercent, !(capabilities.minimumDimming...100).contains(brightness) { throw GatewayAdapterError.unsupportedSetting }
        do { _ = try WiZRequestEncoder.setPilot(patch, capabilities: capabilities, security: .legacyUnsigned) }
        catch { throw GatewayAdapterError.unsupportedSetting }
    }
    private static func patch(_ value: WiZLightingPatch) -> [String: Any] {
        var result: [String: Any] = [:]
        if let v = value.power { result["power"] = v }
        if let v = value.brightnessPercent { result["brightness"] = v }
        if let v = value.rgb { result["rgb"] = [v.red, v.green, v.blue] }
        if let v = value.colorTemperature { result["kelvin"] = v }
        if let v = value.sceneID { result["scene"] = v }
        if let v = value.effectSpeed { result["speed"] = v }
        return result
    }
    private nonisolated static func uuid(_ id: UUID) -> String { id.uuidString.replacingOccurrences(of: "-", with: "").lowercased() }
    private nonisolated static func hex(_ bytes: Data) -> String { bytes.map { String(format: "%02x", $0) }.joined() }
    private nonisolated static func validName(_ name: String) -> Bool {
        (1...32).contains(name.utf8.count) && !name.unicodeScalars.contains { CharacterSet.controlCharacters.contains($0) }
    }
    private static func sceneName(_ id: Int) -> String {
        // Names from the pinned pywizlight scene catalogue; capabilities decide availability.
        let names = [1: "Ocean", 2: "Romance", 3: "Sunset", 4: "Party", 5: "Fireplace", 6: "Cozy", 7: "Forest", 8: "Pastel colors", 9: "Wake-up", 10: "Bedtime", 11: "Warm white", 12: "Daylight", 13: "Cool white", 14: "Night light", 15: "Focus", 16: "Relax", 17: "True colors", 18: "TV time", 19: "Plantgrowth", 20: "Spring", 21: "Summer", 22: "Fall", 23: "Deep dive", 24: "Jungle", 25: "Mojito", 26: "Club", 27: "Christmas", 28: "Halloween", 29: "Candlelight", 30: "Golden white", 31: "Pulse", 32: "Steampunk", 33: "Diwali", 34: "White", 35: "Alarm", 36: "Snowy sky", 40: "Dim-to-warm", 201: "Cinematic", 202: "Vibrant", 203: "Rhythmic", 204: "Relaxation", 1000: "Rhythm"]
        return names[id] ?? ((256...265).contains(id) ? "Custom Mode \(id - 255)" : "Scene \(id)")
    }
    private static func matches(_ desired: WiZLightingPatch, observed: ReportedLightState) -> Bool {
        if let value = desired.power, observed.power != value { return false }
        if let value = desired.brightnessPercent, observed.brightness != value { return false }
        if let value = desired.rgb, observed.rgb != value || (observed.kelvin ?? 0) != 0 { return false }
        if let value = desired.colorTemperature, observed.kelvin != value { return false }
        if (desired.rgb != nil || desired.colorTemperature != nil), (observed.sceneID ?? 0) != 0 { return false }
        if let value = desired.sceneID, observed.sceneID != value { return false }
        if let value = desired.effectSpeed, observed.effectSpeed != value { return false }
        return true
    }
    private struct OperationRecord {
        let fingerprint: Data
        let targets: Set<String>
        let desired: WiZLightingPatch?
        let indices: [String: Int]
        var completed: GatewayOutcome?
    }
    private struct GrantRecord: Codable {
        enum State: String, Codable { case pending, active, blocked }
        let v: Int
        let token: String
        let name: String
        var id: UInt64?
        var guestEpoch: UInt64?
        var state: State
        static func new(name: String) throws -> Self {
            var bytes = Data(count: 32)
            let status = bytes.withUnsafeMutableBytes { SecRandomCopyBytes(kSecRandomDefault, $0.count, $0.baseAddress!) }
            guard status == errSecSuccess else { throw Security2Error.cryptographicFailure }
            return Self(v: 1, token: GatewayAdapter.hex(bytes), name: name, state: .pending)
        }
        static func decode(_ bytes: Data) throws -> Self {
            try StrictJSONValidator.validate(bytes, maximumBytes: 4096)
            guard let object = try JSONSerialization.jsonObject(with: bytes) as? [String: Any],
                  Set(object.keys).isSubset(of: ["v", "token", "name", "id", "guestEpoch", "state"]) else { throw CredentialStoreError.invalidValue }
            let value = try JSONDecoder().decode(Self.self, from: bytes)
            guard value.v == 1, GatewayObject.lowerHex(value.token, count: 64), GatewayAdapter.validName(value.name),
                  value.state != .active || ((value.id ?? 0) > 0 && (value.guestEpoch ?? 0) > 0) else { throw CredentialStoreError.invalidValue }
            return value
        }
    }
}

private struct GatewayObject {
    static let base: Set<String> = ["v", "ok", "code"]
    let values: [String: Any]
    init(_ bytes: Data, envelope: Bool) throws {
        do {
            try StrictJSONValidator.validate(bytes, maximumBytes: 1008)
            guard let object = try JSONSerialization.jsonObject(with: bytes) as? [String: Any] else { throw GatewayAdapterError.malformedReply }
            values = object
            if envelope {
                guard try integer("v", in: 1...1) == 1 else { throw GatewayAdapterError.malformedReply }
                _ = try bool("ok"); let code = try string("code", maximum: 64)
                guard !code.isEmpty, code.utf8.allSatisfy({ (97...122).contains($0) || $0 == 95 }) else { throw GatewayAdapterError.malformedReply }
            }
        } catch { throw GatewayAdapterError.malformedReply }
    }
    init(_ object: [String: Any]) { values = object }
    func keys(required: Set<String>, optional: Set<String>) throws {
        let keys = Set(values.keys)
        guard required.isSubset(of: keys), keys.isSubset(of: required.union(optional)) else { throw GatewayAdapterError.malformedReply }
    }
    func bool(_ key: String) throws -> Bool {
        guard let value = values[key] as? NSNumber, CFGetTypeID(value) == CFBooleanGetTypeID() else { throw GatewayAdapterError.malformedReply }
        return value.boolValue
    }
    func integer(_ key: String, in range: ClosedRange<Int>) throws -> Int {
        guard let value = values[key] as? NSNumber, CFGetTypeID(value) != CFBooleanGetTypeID(), value.doubleValue.isFinite,
              value.doubleValue.rounded(.towardZero) == value.doubleValue,
              value.doubleValue >= Double(range.lowerBound), value.doubleValue <= Double(range.upperBound) else { throw GatewayAdapterError.malformedReply }
        return value.intValue
    }
    func string(_ key: String, maximum: Int = 128) throws -> String {
        guard let value = values[key] as? String, value.utf8.count <= maximum,
              !value.unicodeScalars.contains(where: { CharacterSet.controlCharacters.contains($0) }) else { throw GatewayAdapterError.malformedReply }
        return value
    }
    func decimal(_ key: String) throws -> UInt64 { try Self.decimal(string(key, maximum: 20)) }
    static func decimal(_ value: String) throws -> UInt64 {
        guard !value.isEmpty, value.utf8.count <= 20, value == "0" || !value.hasPrefix("0"),
              value.utf8.allSatisfy({ (48...57).contains($0) }), let result = UInt64(value) else { throw GatewayAdapterError.malformedReply }
        return result
    }
    func mac(_ key: String) throws -> String {
        let text = try string(key, maximum: 12)
        guard Self.lowerHex(text, count: 12), let mac = try? WiZMAC(text) else { throw GatewayAdapterError.malformedReply }
        return mac.rawValue
    }
    func object(_ key: String) throws -> Self { guard let value = values[key] as? [String: Any] else { throw GatewayAdapterError.malformedReply }; return Self(value) }
    func array(_ key: String, maximum: Int) throws -> [Any] { guard let value = values[key] as? [Any], value.count <= maximum else { throw GatewayAdapterError.malformedReply }; return value }
    static func lowerHex(_ text: String, count: Int) -> Bool { text.utf8.count == count && text.utf8.allSatisfy { (48...57).contains($0) || (97...102).contains($0) } }
}

private struct RoomRecord {
    let boot: String
    let nowMS, policyEpoch, guestEpoch, lastSequence: UInt64
    let enabled, frozen, ownerHeld, guestSlots: Int
    let storageHealthy, signingClockReady: Bool
    init(_ reply: GatewayObject, owner: Bool) throws {
        boot = try reply.string("boot", maximum: 16)
        guard GatewayObject.lowerHex(boot, count: 16) else { throw GatewayAdapterError.malformedReply }
        nowMS = try reply.decimal("now_ms"); policyEpoch = try reply.decimal("policy_epoch"); guestEpoch = try reply.decimal("guest_epoch")
        lastSequence = try reply.decimal("last_sequence")
        enabled = try reply.integer("enabled", in: 0...255); frozen = try reply.integer("frozen", in: 0...255); ownerHeld = try reply.integer("owner_held", in: 0...255)
        guestSlots = owner ? try reply.integer("guest_slots", in: 0...65535) : 0
        guard owner || reply.values["guest_slots"] == nil, policyEpoch > 0, guestEpoch > 0,
              (frozen | ownerHeld) & ~enabled == 0 else { throw GatewayAdapterError.malformedReply }
        storageHealthy = try reply.bool("storage_healthy"); _ = try reply.bool("driver_busy")
        signingClockReady = try reply.bool("bulb_clock_ready"); _ = try reply.integer("max_lights", in: 1...8)
    }
}
private struct LightRecord {
    let index: Int, name: String, mac: String
    let revision: UInt32, observedMS: UInt64, observedValid: Bool, pending: Bool
    let capabilities: WiZCapabilities
    let reported: ReportedLightState
    let legacyControl: Bool, error: String
    let powerSupported: Bool
    init(_ reply: GatewayObject) throws {
        index = try reply.integer("index", in: 0...7); name = try reply.string("name", maximum: 32); mac = try reply.mac("mac")
        guard !name.isEmpty, try reply.string("reply_trust") == "unverified_lan" else { throw GatewayAdapterError.malformedReply }
        revision = UInt32(try reply.integer("revision", in: 0...Int(UInt32.max)))
        observedMS = try reply.decimal("observed_ms"); observedValid = try reply.bool("observed_valid"); pending = try reply.bool("pending")
        legacyControl = try reply.bool("legacy_control"); _ = try reply.string("last_result", maximum: 16)
        error = try reply.string("error", maximum: 48)
        guard ["gateway_only", "requires_hardware_validation"].contains(try reply.string("permission_enforcement")) else { throw GatewayAdapterError.malformedReply }
        let caps = try reply.object("capabilities")
        try caps.keys(required: ["features", "evidence", "brightness_min", "kelvin_min", "kelvin_max", "module", "firmware", "scenes"], optional: [])
        let features = try caps.integer("features", in: 0...63)
        powerSupported = features & 1 != 0
        _ = try caps.integer("evidence", in: 0...31); _ = try caps.string("module", maximum: 47); _ = try caps.string("firmware", maximum: 23)
        let minimum = try caps.integer("brightness_min", in: 0...100)
        let lower = try caps.integer("kelvin_min", in: 0...10000), upper = try caps.integer("kelvin_max", in: 0...10000)
        let scenes = try caps.array("scenes", maximum: 40).map { try GatewayObject(["id": $0]).integer("id", in: 1...65535) }
        guard Set(scenes).count == scenes.count, features & 2 == 0 || minimum > 0,
              features & 8 == 0 || (lower >= 1000 && lower <= upper) else { throw GatewayAdapterError.malformedReply }
        capabilities = try WiZCapabilities(brightness: features & 2 != 0, minimumDimming: minimum > 0 ? minimum : 10,
                                            rgb: features & 4 != 0, kelvinRange: features & 8 != 0 ? lower...upper : nil,
                                            supportedSceneIDs: features & 16 != 0 ? Set(scenes) : [], effectSpeedRange: features & 32 != 0 ? 10...200 : nil)
        let state = try reply.object("reported")
        try state.keys(required: [], optional: ["power", "brightness", "rgb", "kelvin", "scene", "speed"])
        let power = try state.values["power"].map { _ in try state.bool("power") }
        let brightness = try state.values["brightness"].map { _ in try state.integer("brightness", in: 0...100) }
        let kelvin = try state.values["kelvin"].map { _ in try state.integer("kelvin", in: 1000...10000) }
        let scene = try state.values["scene"].map { _ in try state.integer("scene", in: 0...65535) }
        let speed = try state.values["speed"].map { _ in try state.integer("speed", in: 10...200) }
        var rgb: WiZRGB?
        if state.values["rgb"] != nil {
            let values = try state.array("rgb", maximum: 3)
            guard values.count == 3 else { throw GatewayAdapterError.malformedReply }
            let channels = try values.map { try GatewayObject(["v": $0]).integer("v", in: 0...255) }
            rgb = try WiZRGB(red: channels[0], green: channels[1], blue: channels[2])
        }
        reported = ReportedLightState(power: observedValid ? power : nil, brightness: observedValid ? brightness : nil,
                                      kelvin: observedValid ? kelvin : nil, rgb: observedValid ? rgb : nil,
                                      sceneID: observedValid ? scene : nil, effectSpeed: observedValid ? speed : nil,
                                      trust: observedValid ? .unverifiedLAN : .unavailable)
    }
    func light(now: Date, gatewayNow: UInt64, signingClockReady: Bool) throws -> Light {
        guard !observedValid || observedMS <= gatewayNow else { throw GatewayAdapterError.malformedReply }
        var state = reported
        if observedValid { state.observedAt = now.addingTimeInterval(-Double(gatewayNow - observedMS) / 1000) }
        return Light(id: mac, name: name, groupID: "room", reachable: observedValid && gatewayNow - observedMS <= 60_000,
                     capabilities: capabilities, reported: state, revision: UInt64(revision), reportPending: pending,
                     controlUnavailableReason: controlUnavailableReason(signingClockReady: signingClockReady))
    }
    func controlUnavailableReason(signingClockReady: Bool) -> String? {
        if !powerSupported { return "This light has not reported supported controls yet." }
        if error == "key_required" { return "The owner needs to import the WiZ home key before this light can be changed." }
        if ["clock_required", "clock_rollback"].contains(error) || (!legacyControl && !signingClockReady) {
            return "The gateway needs a trusted signing clock before this light can be changed."
        }
        return nil
    }
}
private struct SessionRecord {
    let id: UInt64, name: String, expiresMS: UInt64, revoked: Bool, expired: Bool
    init(_ value: GatewayObject) throws {
        id = try value.decimal("grant"); name = try value.string("name", maximum: 32); expiresMS = try value.decimal("expires_ms")
        revoked = try value.bool("revoked"); expired = try value.bool("expired")
        guard id > 0, !name.isEmpty else { throw GatewayAdapterError.malformedReply }
    }
    func model(now: Date, gatewayNow: UInt64) -> GuestSession {
        let expiry = expired ? nil : now.addingTimeInterval(Double(expiresMS > gatewayNow ? expiresMS - gatewayNow : 0) / 1000)
        return GuestSession(id: String(id), name: name, expiresAt: expiry, revoked: revoked, expired: expired)
    }
}
private struct Receipt {
    let id: String, result: String, sent: Int, verified: Int, failed: Int
    var terminal: Bool { !["queued", "sent"].contains(result) }
    init(_ value: GatewayObject) throws {
        id = try value.string("id", maximum: 32); result = try value.string("result", maximum: 16)
        guard GatewayObject.lowerHex(id, count: 32), ["queued", "sent", "verified", "partial", "unknown", "rejected", "superseded", "timed_out"].contains(result) else { throw GatewayAdapterError.malformedReply }
        _ = try value.string("error", maximum: 64)
        sent = try value.integer("sent", in: 0...255); verified = try value.integer("verified", in: 0...255); failed = try value.integer("failed", in: 0...255)
    }
}
