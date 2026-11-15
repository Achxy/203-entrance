import Foundation
import LightsCore
import LightsMenulet
import LightsSecurity

enum CheckFailure: Error { case failed(String) }

@MainActor private final class RevokedFixture: MenuletTransport {
    let canConnect = true, isPreview = true
    let unavailableReason: String? = nil
    private(set) var connectCount = 0
    func connect(_ request: AccessRequest) async throws -> RoomSnapshot { connectCount += 1; throw MenuletError.accessRevoked }
    func refresh() async throws -> RoomSnapshot { throw MenuletError.accessRevoked }
    func execute(_ command: GatewayCommand) async throws -> GatewayOutcome { throw MenuletError.accessRevoked }
    func disconnect() async {}
}

@MainActor private final class CleanupFixture: MenuletTransport {
    let canConnect = true, isPreview = true
    let unavailableReason: String? = nil
    var failCleanup = true
    func connect(_ request: AccessRequest) async throws -> RoomSnapshot { throw MenuletError.unavailable }
    func refresh() async throws -> RoomSnapshot { throw MenuletError.unavailable }
    func execute(_ command: GatewayCommand) async throws -> GatewayOutcome { throw MenuletError.unavailable }
    func disconnect() async {}
    func signOut() async throws { if failCleanup { throw CredentialStoreError.status(-1) } }
}

private actor ReadinessFixture: GatewayReadinessChecking {
    let online: Bool
    private(set) var calls = 0
    init(online: Bool) { self.online = online }
    func isOnline(endpoint: URL, trustedIdentity: TrustedGatewayIdentity) async throws -> Bool { calls += 1; return online }
}

@MainActor private final class CommissioningFixture: GatewayCommissioningTransport {
    let canConnect = true, isPreview = false
    let unavailableReason: String? = nil
    let snapshot = PreviewTransport().snapshot
    var searching = false
    private(set) var starts = 0, cancellations = 0, adoptions = 0
    private(set) var legacyWasAllowed = false
    private(set) var lastAccess: AccessRequest?
    func connect(_ request: AccessRequest) async throws -> RoomSnapshot { lastAccess = request; return snapshot }
    func refresh() async throws -> RoomSnapshot { snapshot }
    func execute(_ command: GatewayCommand) async throws -> GatewayOutcome { throw MenuletError.unavailable }
    func disconnect() async {}
    func startDiscovery() async throws { starts += 1 }
    func cancelDiscovery() async throws { cancellations += 1; searching = false }
    func candidate(at index: Int) async throws -> GatewayDiscoveryPage {
        if searching { try await Task.sleep(for: .milliseconds(50)) }
        return GatewayDiscoveryPage(discovering: searching, total: 1,
            candidate: index == 0 ? GatewayCandidate(id: 0, mac: "aa:bb:cc:dd:ee:ff", identified: true, module: "Fixture light", firmware: "fixture") : nil)
    }
    func adoptCandidate(_ index: Int, name: String, allowLegacyControl: Bool) async throws -> RoomSnapshot {
        adoptions += 1; legacyWasAllowed = allowLegacyControl
        return snapshot
    }
}

@main
struct MenuletChecks {
    @MainActor static func main() async throws {
        var checks = 0
        func expect(_ condition: @autoclosure () -> Bool, _ message: String) throws {
            guard condition() else { throw CheckFailure.failed(message) }
            checks += 1
        }
        func rejects(_ message: String, _ body: () throws -> Void) throws {
            do { try body() } catch { checks += 1; return }
            throw CheckFailure.failed(message)
        }
        func settled(_ model: MenuletModel) async throws {
            let deadline = Date().addingTimeInterval(3)
            repeat { try await Task.sleep(for: .milliseconds(30)) } while (!model.pendingIDs.isEmpty || model.busy) && Date() < deadline
            try expect(model.pendingIDs.isEmpty && !model.busy, "Commands must settle within the bounded preview interval")
        }
        let ordinary = MenuletModel.configured(arguments: ["203Lights"])
        try expect(ordinary.snapshot == nil && !ordinary.isPreview && ordinary.canConnect, "Normal launch offers real readiness-gated onboarding without inventing a connection")
        let defaultIdentity = try Room203Defaults.trustedIdentity()
        try expect(defaultIdentity.gatewayID == "entrance-c7f088" && ordinary.endpoint == Room203Defaults.relayEndpoint.absoluteString,
                   "Room 203 includes only its public trusted identity and default relay address")
        ordinary.setPower(true)
        try expect(ordinary.snapshot == nil && ordinary.problem != nil, "Disconnected writes must fail visibly")
        let accidental = MenuletModel.configured(arguments: ["203Lights", "--preview-state", "ready"])
        try expect(accidental.snapshot == nil, "Preview fixtures require the explicit preview switch")

        let transport = PreviewTransport()
        let model = MenuletModel(transport: transport, initialSnapshot: transport.snapshot)
        try expect(model.isOwner && model.hasBrightness && !model.supportsColor, "Mixed capabilities must not fabricate RGB support")
        try expect(model.temperatureRange == 2700.0...6500.0 && model.supportedScenes.isEmpty, "Group controls use capability intersection")
        model.selection = .light("preview-desk")
        try expect(model.supportsColor && model.supportedScenes.count == 4, "Individual capabilities remain available")
        for value in 10...90 { model.setBrightness(Double(value)) }
        try await settled(model)
        try expect(transport.executedCommandCount == 1, "A synchronous slider burst must coalesce to its latest value")
        try expect(model.selectedLights.first?.reported.brightness == 90, "Final desired value must be confirmed")
        model.setTemperature(3000)
        try await settled(model)
        try expect(model.selectedLights.first?.reported.kelvin == 3000, "Temperature change must preserve observed state")
        try expect(model.message?.contains("Preview") == true, "Preview changes must never claim a real bulb changed")

        let partial = PreviewTransport(scenario: "partial")
        let mixed = MenuletModel(transport: partial, initialSnapshot: partial.snapshot)
        mixed.setPower(true)
        try await settled(mixed)
        try expect(mixed.partialFailures.count == 1 && mixed.partialFailures["preview-ceiling"] != nil, "Partial group failure must identify failed targets")
        try expect(mixed.snapshot?.lights.first { $0.id == "preview-ceiling" }?.reported.power == false, "Unreachable bulb state must remain unconfirmed")

        let frozen = PreviewTransport(role: .guest, scenario: "frozen")
        let guest = MenuletModel(transport: frozen, initialSnapshot: frozen.snapshot)
        guest.setPower(false)
        try expect(!guest.canControl && frozen.executedCommandCount == 0, "Frozen guest controls must not enqueue writes")
        await guest.ownerAction(.revokeAllGuests)
        try expect(frozen.snapshot.sessions.count == 2, "Guest cannot invoke owner actions")
        let revokedTransport = RevokedFixture()
        let revoked = MenuletModel(transport: revokedTransport, initialSnapshot: PreviewTransport(role: .guest).snapshot)
        await revoked.refresh()
        try expect(revoked.snapshot == nil && !revoked.canControl && revoked.requiresExplicitRejoin && revoked.problem?.contains("revoked") == true && revokedTransport.connectCount == 0,
                   "Revocation must stop controls and never automatically reconnect or reenroll")
        let frozenPolicy = AccessPolicy(guestFreezeActive: true, frozenLightIDs: ["preview-desk"])
        try expect(frozenPolicy.guestRestriction(at: Date(), selectedIDs: ["preview-bed"]) == nil,
                   "Partial owner freezes must not disable unrelated lights")
        let restriction = frozenPolicy.guestRestriction(at: Date(), selectedIDs: ["preview-desk"])
        try expect(restriction != nil && restriction?.contains("until") != true, "Unknown freeze expiry must not be fabricated")
        let cleanup = CleanupFixture()
        let cleanupModel = MenuletModel(transport: cleanup, initialSnapshot: PreviewTransport().snapshot)
        await cleanupModel.signOut()
        try expect(cleanupModel.snapshot == nil && cleanupModel.needsCredentialCleanup, "Sign-out must disclose failed Keychain cleanup")
        cleanup.failCleanup = false
        await cleanupModel.signOut()
        try expect(!cleanupModel.needsCredentialCleanup && cleanupModel.problem == nil, "Credential cleanup can be explicitly retried")
        let commissioning = CommissioningFixture()
        let savedAccess = MenuletModel(transport: commissioning)
        await savedAccess.connect(credential: "")
        try expect(commissioning.lastAccess == nil, "Empty access must not read saved credentials without an explicit selection")
        savedAccess.rememberAccess = true
        await savedAccess.connect(credential: "", explicitlyRejoin: true)
        try expect(commissioning.lastAccess == nil, "Explicit rejoin must require the entered current password")
        await savedAccess.connect(credential: "")
        try expect(commissioning.lastAccess?.rememberAccess == true && commissioning.lastAccess?.credential.isEmpty == true && savedAccess.snapshot != nil,
                   "Use Saved Access must reach the adapter only through the explicit remembered-access path")
        let setup = MenuletModel(transport: commissioning, initialSnapshot: commissioning.snapshot)
        await setup.discoverLights()
        try expect(commissioning.starts == 1 && setup.discoveredLights.count == 1 && !setup.discoveringLights,
                   "Owner discovery must consume real transport results and finish")
        await setup.adoptLight(candidateID: 0, name: "Desk")
        try expect(commissioning.adoptions == 1 && !commissioning.legacyWasAllowed && setup.discoveredLights.isEmpty,
                   "Adoption must always require verified control and remove the consumed candidate")
        let cancelFixture = CommissioningFixture(); cancelFixture.searching = true
        let cancelSetup = MenuletModel(transport: cancelFixture, initialSnapshot: cancelFixture.snapshot)
        let search = Task { await cancelSetup.discoverLights() }
        try await Task.sleep(for: .milliseconds(10))
        cancelSetup.stopDiscoveringLights()
        await search.value
        try expect(cancelFixture.cancellations == 1 && !cancelSetup.busy, "Discovery cancellation must reach the gateway after the current response")
        let forbiddenSetup = MenuletModel(transport: commissioning, initialSnapshot: PreviewTransport(role: .guest).snapshot)
        await forbiddenSetup.discoverLights()
        try expect(commissioning.starts == 1 && !forbiddenSetup.canCommission, "Guests must not begin commissioning")
        let offline = MenuletModel.configured(arguments: ["203Lights", "--preview", "--preview-state", "offline"])
        try expect(!offline.canControl && offline.connection == .offline, "Offline snapshots are read-only")
        let stale = MenuletModel.configured(arguments: ["203Lights", "--preview", "--preview-state", "stale"])
        try expect(!stale.canControl && stale.restriction?.contains("out of date") == true, "Stale room state must be clearly read-only")
        await stale.refresh()
        try expect(stale.canControl, "A fresh adapter snapshot restores stale controls")
        let refreshTransport = PreviewTransport()
        let refreshModel = MenuletModel(transport: refreshTransport, initialSnapshot: refreshTransport.snapshot)
        await refreshModel.refresh()
        await refreshModel.refresh()
        try expect(refreshTransport.refreshCount == 1, "Repeated refreshes must stay within the five-second client budget")
        let foregroundTransport = PreviewTransport()
        let foregroundModel = MenuletModel(transport: foregroundTransport, initialSnapshot: foregroundTransport.snapshot)
        foregroundModel.setPanelActive(true)
        foregroundModel.setPanelActive(false)
        try await Task.sleep(for: .milliseconds(50))
        try expect(foregroundTransport.refreshCount == 0, "A panel hidden before the refresh task runs must perform no reads")
        foregroundModel.setPanelActive(true)
        try await Task.sleep(for: .milliseconds(150))
        foregroundModel.setPanelActive(false)
        try expect(foregroundTransport.refreshCount == 1, "An active panel may refresh once; hiding it stops subsequent scheduled reads")
        let beforeInvalid = transport.executedCommandCount
        model.setBrightness(.nan); model.setTemperature(.infinity); model.setEffectSpeed(-1)
        try expect(transport.executedCommandCount == beforeInvalid && model.pendingIDs.isEmpty, "Invalid slider inputs must not trap or enqueue")

        await model.ownerAction(.freezeGuests(until: Date().addingTimeInterval(900)))
        try expect(model.snapshot?.policy.guestFreezeUntil != nil, "Owner policy returned by adapter must be reflected")
        await model.ownerAction(.revokeSession("preview-guest-a"))
        try expect(model.snapshot?.sessions.count == 1, "Owner can revoke the intended guest session")
        let revision = model.snapshot!.policy.guestPasswordRevision
        await model.ownerAction(.rotateGuestPassword("a long example password"))
        try expect(model.snapshot!.policy.guestPasswordRevision == revision + 1 && model.snapshot!.sessions.isEmpty, "Password rotation always revokes existing guest grants")

        let duplicate = GatewayCommand(expectedRevision: transport.snapshot.revision, action: .lighting(ids: ["preview-desk"], patch: .init(power: true)))
        let first = try await transport.execute(duplicate)
        let second = try await transport.execute(duplicate)
        try expect(first.snapshot.revision == second.snapshot.revision, "Duplicate operation identity must not repeat a preview mutation")

        let race = PreviewTransport()
        @MainActor func attempt(_ transport: PreviewTransport, _ value: Bool) async -> Bool {
            do {
                _ = try await transport.execute(.init(expectedRevision: 1, action: .lighting(ids: ["preview-desk"], patch: .init(power: value))))
                return true
            } catch { return false }
        }
        async let raceA = attempt(race, true)
        async let raceB = attempt(race, false)
        let concurrentResults = await [raceA, raceB]
        try expect(concurrentResults.filter { $0 }.count == 1 && race.snapshot.revision == 2, "Concurrent stale revisions must not both mutate the room")

        let ownerRace = PreviewTransport()
        let queuedOwner = MenuletModel(transport: ownerRace, initialSnapshot: ownerRace.snapshot)
        queuedOwner.setBrightness(75)
        await queuedOwner.ownerAction(.revokeAllGuests)
        try expect(ownerRace.snapshot.sessions.count == 2, "Owner policy must not bypass a queued lighting transaction")
        try await settled(queuedOwner)

        model.setBrightness(70)
        await model.signOut()
        try await Task.sleep(for: .milliseconds(400))
        try expect(model.snapshot == nil && model.pendingIDs.isEmpty && !model.busy, "Sign-out cancels queued changes and cannot resurrect the session")

        let inFlight = PreviewTransport()
        let leaving = MenuletModel(transport: inFlight, initialSnapshot: inFlight.snapshot)
        leaving.setPower(false)
        try await Task.sleep(for: .milliseconds(550))
        await leaving.signOut()
        try await Task.sleep(for: .milliseconds(200))
        try expect(leaving.snapshot == nil && leaving.problem == nil, "Canceled in-flight writes must not restore a signed-out UI or stale error")

        let connection = String(repeating: "a", count: 32), boot = String(repeating: "b", count: 16)
        func encoded(_ value: [String: Any]) throws -> Data { try JSONSerialization.data(withJSONObject: value) }
        let identity = try RelayFrameCodec.decodeOnline(encoded(["v": 1, "type": "light_online", "online": true,
                                                                 "connection": connection, "boot": boot, "protocol": 1]))
        try expect(identity.connection == connection && identity.boot == boot, "Relay online identity must retain the session and boot binding")
        let request = try RelayFrameCodec.encodeRequest(id: 1, operation: .data, payload: Data(repeating: 0x42, count: 1024), roleHint: .guest)
        try expect(request.count <= RelayFrameCodec.maximumFrameBytes, "Maximum payload must fit the relay envelope")
        try rejects("Oversized outgoing messages must fail before sending") {
            _ = try RelayFrameCodec.encodeRequest(id: 1, operation: .data, payload: Data(count: 1025))
        }
        try rejects("Empty encrypted data must fail") { _ = try RelayFrameCodec.encodeRequest(id: 1, operation: .data, payload: Data()) }
        try rejects("Request identity zero must fail") { _ = try RelayFrameCodec.encodeRequest(id: 0, operation: .session, payload: Data([1])) }
        let closeFrame = try RelayFrameCodec.encodeRequest(id: 1, operation: .close, payload: Data())
        try expect(!closeFrame.isEmpty, "Close is the only operation that permits an empty payload")
        let baseReply: [String: Any] = ["v": 1, "type": "light_reply", "connection": connection, "boot": boot, "id": 1, "status": 200, "payload": "Zg==", "role": "guest"]
        let reply = try RelayFrameCodec.decodeReply(encoded(baseReply), expectedID: 1, identity: identity)
        try expect(reply == Data([102]), "Reply bytes must decode without treating relay role hints as authorization")
        for (field, value): (String, Any) in [("v", 2), ("id", 2), ("boot", String(repeating: "c", count: 16)),
                                             ("connection", String(repeating: "c", count: 32)), ("type", "light_offline"),
                                             ("role", "administrator"), ("role", "none"), ("payload", ""), ("payload", "Zh=="), ("payload", "Zg==\n"), ("unexpected", true)] {
            var invalid = baseReply; invalid[field] = value
            try rejects("Mismatched or noncanonical relay field \(field) must fail") {
                _ = try RelayFrameCodec.decodeReply(encoded(invalid), expectedID: 1, identity: identity)
            }
        }
        let duplicateKey = Data("{\"v\":1,\"v\":1,\"type\":\"light_reply\",\"connection\":\"\(connection)\",\"boot\":\"\(boot)\",\"id\":1,\"status\":200,\"payload\":\"Zg==\"}".utf8)
        try rejects("Duplicate envelope keys must not be discarded by JSONDecoder") {
            _ = try RelayFrameCodec.decodeReply(duplicateKey, expectedID: 1, identity: identity)
        }
        let fractionalID = Data("{\"v\":1,\"type\":\"light_reply\",\"connection\":\"\(connection)\",\"boot\":\"\(boot)\",\"id\":1.0,\"status\":200,\"payload\":\"Zg==\"}".utf8)
        try rejects("Fractional integer spelling must be rejected") {
            _ = try RelayFrameCodec.decodeReply(fractionalID, expectedID: 1, identity: identity)
        }
        var rejected = baseReply; rejected["status"] = 429; rejected["error"] = "rate"; rejected["payload"] = ""
        try rejects("Relay refusal must never be accepted as encrypted application success") {
            _ = try RelayFrameCodec.decodeReply(encoded(rejected), expectedID: 1, identity: identity)
        }
        try rejects("Frame size limit must be enforced before JSON parsing") {
            _ = try RelayFrameCodec.decodeOnline(Data(count: 2049))
        }
        for address in ["ws://example.invalid/v1/lights", "wss://example.invalid/wrong", "wss://name:secret@example.invalid/v1/lights", "wss://example.invalid/v1/lights?token=secret"] {
            try rejects("Unsafe relay endpoint must be rejected without connecting") { _ = try RelayWebSocketChannel(endpoint: URL(string: address)!) }
        }
        _ = try RelayWebSocketChannel(endpoint: URL(string: "wss://example.invalid/v1/lights")!)
        try rejects("Credential records must require a trusted identity") { _ = try SecretRecordID(trustedGatewayIdentity: Data(), kind: .ownerKey) }
        _ = try SecretRecordID(trustedGatewayIdentity: Data(repeating: 1, count: 32), kind: .ownerKey)
        let discovery = NearbyGatewayDiscovery()
        try expect(!discovery.isScanning && discovery.candidates.isEmpty, "Constructing the BLE seam must not begin discovery")
        try rejects("Discovery duration must be bounded before requesting Bluetooth") { try discovery.start(serviceUUID: UUID(), duration: 0) }
        let export = Data(#"{"version":"1.0","udp_signing_key":"00112233445566778899aabbccddeeff","devices":[{"url":"https://example.invalid/never-fetch","capabilities":{"rgb":true}}]}"#.utf8)
        let imported = try WiZHomeImport.key(from: export)
        try expect(imported.count == 16 && imported.first == 0 && imported.last == 255,
                   "Official JSON import reads only the key and never follows exported metadata")
        try rejects("Oversized WiZ exports must be rejected") { _ = try WiZHomeImport.key(from: Data(count: WiZHomeImport.maximumBytes + 1)) }
        try rejects("Unsupported export versions must be rejected") {
            _ = try WiZHomeImport.key(from: Data(#"{"version":"2.0","udp_signing_key":"00112233445566778899aabbccddeeff"}"#.utf8))
        }
        try rejects("Malformed signing keys must be rejected") {
            _ = try WiZHomeImport.key(from: Data(#"{"version":"1.0","udp_signing_key":"not-a-key"}"#.utf8))
        }
        try rejects("Duplicate export keys must be rejected") {
            _ = try WiZHomeImport.key(from: Data(#"{"version":"1.0","version":"1.0","udp_signing_key":"00112233445566778899aabbccddeeff"}"#.utf8))
        }
        try rejects("JSON import never accepts remote URLs") { _ = try WiZHomeImport.key(fromLocalFile: URL(string: "https://example.invalid/export.json")!) }
        let syntheticFile = FileManager.default.temporaryDirectory.appendingPathComponent("203-lights-fixture-\(UUID().uuidString).json")
        try export.write(to: syntheticFile)
        defer { try? FileManager.default.removeItem(at: syntheticFile) }
        let importedFile = try WiZHomeImport.key(fromLocalFile: syntheticFile)
        try expect(importedFile == imported, "Bounded regular-file import must match in-memory parsing")
        let nearbyDescriptor: [String: Any] = ["v": 1, "protocol": 1, "connection": connection, "boot": boot,
                                               "gateway_id": "room-203", "security": 2, "patch": 1]
        let nearbyIdentity = try BluetoothGatewayIdentityCodec.decode(encoded(nearbyDescriptor), expectedGatewayID: "room-203")
        try expect(nearbyIdentity == identity, "BLE must use actual server boot and route, never fabricated metadata")
        for (field, value): (String, Any) in [("gateway_id", "some-other-room"), ("security", 1), ("patch", 0), ("protocol", 2), ("extra", 1)] {
            var invalid = nearbyDescriptor; invalid[field] = value
            try rejects("Wrong BLE identity descriptor field \(field) must close before SRP exchange") {
                _ = try BluetoothGatewayIdentityCodec.decode(encoded(invalid), expectedGatewayID: "room-203")
            }
        }

        var routes = TransportSelectionPolicy()
        routes.observe(.internet, succeeded: true, roundTripMilliseconds: 300, at: 0)
        let initialRoute = routes.choose(at: 0)
        try expect(initialRoute.route == .internet && initialRoute.requiresFreshAuthentication, "First route requires authentication")
        for tick in 1...3 { routes.observe(.bluetooth, succeeded: true, roundTripMilliseconds: 30, at: Double(tick)) }
        let dwell = routes.choose(at: 3)
        try expect(dwell.route == .internet && !dwell.requiresFreshAuthentication, "Small timing changes must not bypass route dwell")
        routes.observe(.internet, succeeded: true, roundTripMilliseconds: 300, at: 30)
        routes.observe(.bluetooth, succeeded: true, roundTripMilliseconds: 30, at: 30)
        let improved = routes.choose(at: 30)
        try expect(improved.route == .bluetooth && improved.requiresFreshAuthentication, "Sustained faster route requires a fresh bound session")
        routes.observe(.bluetooth, succeeded: false, roundTripMilliseconds: nil, at: 31)
        try expect(routes.choose(at: 31).route == .bluetooth, "One failure must not flap routes")
        routes.observe(.bluetooth, succeeded: false, roundTripMilliseconds: nil, at: 32)
        let failedOver = routes.choose(at: 32)
        try expect(failedOver.route == .internet && failedOver.requiresFreshAuthentication, "Two failures may bypass dwell to a healthy fallback")
        try expect(routes.choose(at: 33).requiresFreshAuthentication == false, "Same route does not request repeated authentication")
        try expect(routes.choose(at: 70).route == nil, "Stale observations must not advertise an available path")
        routes.observe(.internet, succeeded: true, roundTripMilliseconds: 100, at: 71)
        try expect(routes.choose(preference: .bluetooth, at: 71).route == nil, "Forced Bluetooth does not secretly choose Internet")
        routes.reset()
        routes.observe(.internet, succeeded: true, roundTripMilliseconds: .nan, at: 72)
        try expect(routes.choose(at: 72).route == nil, "Nonfinite latency must not create a healthy route")

        let readiness = ReadinessFixture(online: true)
        let coordinator = GatewayRouteCoordinator(trustedIdentity: defaultIdentity, readiness: readiness)
        let routeRequest = AccessRequest(endpoint: Room203Defaults.relayEndpoint.absoluteString, name: "Preview guest", role: .guest,
                                         credential: "fixture only", preference: .automatic)
        let candidate = try await coordinator.makeRoute(routeRequest, excluding: [])
        let cachedCandidate = try await coordinator.makeRoute(routeRequest, excluding: [])
        let readinessCalls = await readiness.calls
        try expect(candidate.route == .internet && cachedCandidate.route == .internet && readinessCalls == 1,
                   "Route construction checks and briefly caches readiness without opening a socket")
        let offlineReadiness = ReadinessFixture(online: false)
        let nearbyCoordinator = GatewayRouteCoordinator(trustedIdentity: defaultIdentity, nearbyPeripheralID: UUID(), readiness: offlineReadiness)
        let nearby = try await nearbyCoordinator.makeRoute(routeRequest, excluding: [])
        try expect(nearby.route == .bluetooth, "An offline relay can select an explicitly chosen nearby peer without opening it")
        do {
            _ = try await nearbyCoordinator.makeRoute(routeRequest, excluding: [.internet, .bluetooth])
            throw CheckFailure.failed("Excluded failed routes must not be recycled")
        } catch is CheckFailure { throw CheckFailure.failed("Excluded failed routes must not be recycled") }
        catch { checks += 1 }

        print("LightsMenuletChecks: \(checks) assertions passed. UI state, capabilities, pacing, policy, races, sign-out, relay framing and route hysteresis checked. No sockets opened, BLE scans started, or Keychain entries written.")
        if CommandLine.arguments.contains("--gateway-wire-checks") { try await GatewayAdapterChecks.run() }
    }
}
