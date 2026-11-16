import Foundation
import Darwin
import LightsCore
import LightsMenulet
import LightsSecurity

@MainActor enum ProbeChecks {
    static func run() async throws {
        var assertions = 0
        func expect(_ test: Bool) throws {
            guard test else { throw ProbeFailure.inputFormat }
            assertions += 1
        }
        func rejects(_ work: () throws -> Void) throws {
            do { try work() } catch { assertions += 1; return }
            throw ProbeFailure.inputFormat
        }
        guard case .help = try ProbeOptions.parse([]), case .help = try ProbeOptions.parse(["--help"]),
              case .check = try ProbeOptions.parse(["--check"]) else { throw ProbeFailure.arguments }
        assertions += 3
        let input = ["--live", "--bundle", "/private/tmp/test-owner.json", "--evidence", "/private/tmp/test-evidence.json"]
        guard case .live(let valid) = try ProbeOptions.parse(input) else { throw ProbeFailure.arguments }
        try expect(valid.timeoutSeconds == 45)
        for invalid in [Array(input.dropFirst()), input + ["--live"], input + ["--transport", "bluetooth"],
                        input + ["--endpoint", "wss://other.invalid"], input + ["--timeout", "0"],
                        input + ["--timeout", "121"], input + ["--timeout", "NaN"],
                        input + ["--bundle", "/private/tmp/another.json"], ["--live"],
                        ["--live", "--bundle", "relative.json", "--evidence", "/private/tmp/new.json"],
                        ["--live", "--bundle", "/same.json", "--evidence", "/same.json"],
                        ["--check", "--live"]] { try rejects { _ = try ProbeOptions.parse(invalid) } }
        let target = UUID(uuidString: "00112233-4455-6677-8899-aabbccddeeff")!
        let bluetoothInput = input + ["--transport", "bluetooth", "--peripheral", target.uuidString]
        guard case .live(let bluetoothOptions) = try ProbeOptions.parse(bluetoothInput),
              case .authorize(let permissionOptions) = try ProbeOptions.parse(["--authorize-bluetooth", "--evidence", "/private/tmp/permission.json"]),
              case .discover(let discoveryOptions) = try ProbeOptions.parse(["--discover-bluetooth", "--evidence", "/private/tmp/discovery.json"]) else { throw ProbeFailure.arguments }
        try expect(bluetoothOptions.transport == .bluetooth && bluetoothOptions.peripheralID == target)
        try expect(permissionOptions.bundle == nil && permissionOptions.operation == .authorization && permissionOptions.timeoutSeconds == 120)
        try expect(discoveryOptions.bundle == nil && discoveryOptions.operation == .discovery && discoveryOptions.scanSeconds == 10)
        for invalid in [input + ["--peripheral", target.uuidString], input + ["--transport", "automatic"],
                        input + ["--transport", "bluetooth", "--peripheral", "not-a-uuid"],
                        input + ["--scan-seconds", "5"],
                        ["--authorize-bluetooth", "--evidence", "/private/tmp/a.json", "--bundle", "/private/tmp/b.json"],
                        ["--discover-bluetooth", "--evidence", "/private/tmp/a.json", "--bundle", "/private/tmp/b.json"],
                        ["--authorize-bluetooth", "--evidence", "/private/tmp/a.json", "--live"],
                        ["--discover-bluetooth", "--evidence", "/private/tmp/a.json", "--scan-seconds", "16"],
                        ["--discover-bluetooth", "--evidence", "/private/tmp/a.json", "--timeout", "10", "--scan-seconds", "10"],
                        ["--authorize-bluetooth", "--evidence", "/private/tmp/a.json", "--timeout", "181"]] {
            try rejects { _ = try ProbeOptions.parse(invalid) }
        }
        for operation in [ProbeOperation.room, .authorization, .discovery] {
            try expect(try ProbePermissionPolicy.action(operation: operation, authorization: .allowed) == .proceed)
            try rejects { _ = try ProbePermissionPolicy.action(operation: operation, authorization: .denied) }
            try rejects { _ = try ProbePermissionPolicy.action(operation: operation, authorization: .restricted) }
            if operation == .authorization { try expect(try ProbePermissionPolicy.action(operation: operation, authorization: .undetermined) == .requestApproval) }
            else { try rejects { _ = try ProbePermissionPolicy.action(operation: operation, authorization: .undetermined) } }
        }
        try ProbeApplicationIdentity.validate(bundleExtension: "app", bundleID: ProbeApplicationIdentity.expectedIdentifier,
                                               executable: ProbeApplicationIdentity.expectedExecutable, packageType: "APPL", usage: "Fixture permission purpose",
                                               signingIdentifier: ProbeApplicationIdentity.expectedIdentifier, signatureValid: true)
        assertions += 1
        for field in 0..<7 {
            try rejects {
                try ProbeApplicationIdentity.validate(bundleExtension: field == 0 ? nil : "app",
                    bundleID: field == 1 ? "dev.achyuth.203lights" : ProbeApplicationIdentity.expectedIdentifier,
                    executable: field == 2 ? "other" : ProbeApplicationIdentity.expectedExecutable,
                    packageType: field == 3 ? nil : "APPL", usage: field == 4 ? "" : "Fixture permission purpose",
                    signingIdentifier: field == 5 ? "other" : ProbeApplicationIdentity.expectedIdentifier, signatureValid: field != 6)
            }
        }

        let owner = Data(repeating: 0x42, count: 32).base64EncodedString().replacingOccurrences(of: "=", with: "")
        let guest = "fixture-only-guest-password"
        let profile = ["salt": String(repeating: "0a", count: 16), "verifier": String(repeating: "0b", count: 384)]
        let object: [String: Any] = ["v": 1, "gateway_id": Room203Defaults.gatewayID, "owner_key": owner,
                                    "guest_password": guest, "owner": profile, "guest": profile]
        let data = try JSONSerialization.data(withJSONObject: object, options: [.sortedKeys])
        let appIdentity = ProbeApplicationIdentity(bundle_id: ProbeApplicationIdentity.expectedIdentifier,
                                                   signing_identifier: ProbeApplicationIdentity.expectedIdentifier,
                                                   executable_sha256: String(repeating: "a", count: 64), signature_valid: true)
        var credentialReads = 0, identityChecks = 0, permissionChecks = 0
        let fakeIdentity: () throws -> ProbeApplicationIdentity = { identityChecks += 1; return appIdentity }
        let fakeLoader: (String, String) throws -> ProbeOwnerBundle = { _, gateway in
            credentialReads += 1; return try ProbeOwnerBundle.decode(data, expectedGateway: gateway)
        }
        var unsignedEvidence = ProbeEvidence(identity: try Room203Defaults.trustedIdentity(), transport: .bluetooth)
        try rejects {
            _ = try ProbeReadPreparation.load(bluetoothOptions, expectedGateway: Room203Defaults.gatewayID, evidence: &unsignedEvidence,
                                               application: { throw ProbeFailure.diagnosticBundleRequired },
                                               authorization: { permissionChecks += 1; return .allowed }, loader: fakeLoader)
        }
        try expect(credentialReads == 0 && permissionChecks == 0 && unsignedEvidence.diagnostic_app == nil)
        for status in [ProbeBluetoothAuthorization.undetermined, .denied, .restricted] {
            var deniedEvidence = ProbeEvidence(identity: try Room203Defaults.trustedIdentity(), transport: .bluetooth)
            try rejects { _ = try ProbeReadPreparation.load(bluetoothOptions, expectedGateway: Room203Defaults.gatewayID, evidence: &deniedEvidence,
                                                            application: fakeIdentity, authorization: { status }, loader: fakeLoader) }
            try expect(credentialReads == 0 && !deniedEvidence.identity_verified)
        }
        var preparedEvidence = ProbeEvidence(identity: try Room203Defaults.trustedIdentity(), transport: .bluetooth)
        var prepared = try ProbeReadPreparation.load(bluetoothOptions, expectedGateway: Room203Defaults.gatewayID, evidence: &preparedEvidence,
                                                     application: fakeIdentity, authorization: { .allowed }, loader: fakeLoader)
        prepared.clear()
        try expect(credentialReads == 1 && preparedEvidence.bluetooth_authorization_before == "allowed")
        let checksBeforeInternet = identityChecks
        var internetEvidence = ProbeEvidence(identity: try Room203Defaults.trustedIdentity())
        var internetPrepared = try ProbeReadPreparation.load(valid, expectedGateway: Room203Defaults.gatewayID, evidence: &internetEvidence,
                                                             application: fakeIdentity, authorization: { permissionChecks += 1; return .denied }, loader: fakeLoader)
        internetPrepared.clear()
        try expect(credentialReads == 2 && identityChecks == checksBeforeInternet && permissionChecks == 0)
        for otherMode in [permissionOptions, discoveryOptions] {
            try rejects { _ = try ProbeReadPreparation.load(otherMode, expectedGateway: Room203Defaults.gatewayID, evidence: &internetEvidence,
                                                            application: fakeIdentity, authorization: { .allowed }, loader: fakeLoader) }
        }
        try expect(credentialReads == 2)
        let readiness = ProbeForbiddenReadiness()
        let coordinator = GatewayRouteCoordinator(trustedIdentity: try Room203Defaults.trustedIdentity(), nearbyPeripheralID: target, readiness: readiness)
        let forced = try await coordinator.makeRoute(AccessRequest(endpoint: Room203Defaults.relayEndpoint.absoluteString, name: "Fixture", role: .owner,
                                                                   credential: "", preference: .bluetooth, nearbyPeripheralID: target), excluding: [])
        let readinessCalls = await readiness.calls
        try expect(forced.route == .bluetooth && readinessCalls == 0)
        var collection = ProbeDiscoveryCollector()
        collection.observe(id: target, advertisedServices: [UUID()], rssi: -40)
        try expect(collection.sorted.isEmpty)
        collection.observe(id: target, advertisedServices: [BluetoothFrameCodec.serviceUUID], rssi: -40)
        collection.observe(id: target, advertisedServices: [BluetoothFrameCodec.serviceUUID], rssi: -45)
        try expect(collection.sorted.count == 1 && collection.sorted[0].rssi == -45 && !collection.sorted[0].identity_verified)
        for _ in 0..<12 { collection.observe(id: UUID(), advertisedServices: [BluetoothFrameCodec.serviceUUID], rssi: 127) }
        try expect(collection.sorted.count == 8 && collection.sorted.filter { $0.rssi == nil }.count == 7)
        var bundle = try ProbeOwnerBundle.decode(data, expectedGateway: Room203Defaults.gatewayID)
        try expect(bundle.credential == Data(owner.utf8))
        bundle.clear(); try expect(bundle.credential.isEmpty)
        try rejects { _ = try ProbeOwnerBundle.decode(data, expectedGateway: "different-gateway") }
        var duplicate = data; duplicate.removeLast(); duplicate.append(Data(",\"v\":1}".utf8))
        try rejects { _ = try ProbeOwnerBundle.decode(duplicate, expectedGateway: Room203Defaults.gatewayID) }
        let invalidObjects: [[String: Any]] = [object.merging(["v": true]) { _, rhs in rhs },
                                      object.merging(["extra": "unknown"]) { _, rhs in rhs },
                                      object.merging(["owner_key": "short"]) { _, rhs in rhs },
                                      object.merging(["owner": ["salt": "00", "verifier": "00"]]) { _, rhs in rhs }]
        for invalid in invalidObjects {
            try rejects { _ = try ProbeOwnerBundle.decode(JSONSerialization.data(withJSONObject: invalid), expectedGateway: Room203Defaults.gatewayID) }
        }

        let directory = FileManager.default.temporaryDirectory.appendingPathComponent("lights-probe-check-" + UUID().uuidString, isDirectory: true)
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: false, attributes: [.posixPermissions: 0o700])
        defer { try? FileManager.default.removeItem(at: directory) }
        let path = directory.appendingPathComponent("synthetic-owner.json").path
        try data.write(to: URL(fileURLWithPath: path)); guard chmod(path, 0o600) == 0 else { throw ProbeFailure.inputPermission }
        try expect(try PrivateProbeFile.read(path) == data)
        guard chmod(path, 0o644) == 0 else { throw ProbeFailure.inputPermission }
        try rejects { _ = try PrivateProbeFile.read(path) }
        guard chmod(path, 0o600) == 0 else { throw ProbeFailure.inputPermission }
        let symbolic = directory.appendingPathComponent("symbolic.json").path
        try FileManager.default.createSymbolicLink(atPath: symbolic, withDestinationPath: path)
        try rejects { _ = try PrivateProbeFile.read(symbolic) }
        let fifo = directory.appendingPathComponent("named-pipe").path
        guard mkfifo(fifo, 0o600) == 0 else { throw ProbeFailure.inputPermission }
        try rejects { _ = try PrivateProbeFile.read(fifo) }
        let linked = directory.appendingPathComponent("hard-link.json").path
        try FileManager.default.linkItem(atPath: path, toPath: linked)
        try rejects { _ = try PrivateProbeFile.read(path) }
        try FileManager.default.removeItem(atPath: linked)
        let oversized = directory.appendingPathComponent("oversized.json")
        try Data(repeating: 65, count: 8193).write(to: oversized); guard chmod(oversized.path, 0o600) == 0 else { throw ProbeFailure.inputPermission }
        try rejects { _ = try PrivateProbeFile.read(oversized.path) }

        let privateName = "private-test-light-name", privateGuest = "private-test-guest-name", privateMAC = "aabbccddeeff"
        let light = Light(id: privateMAC, name: privateName, groupID: "private-group", reachable: true,
                          capabilities: try WiZCapabilities(), reported: ReportedLightState(power: true, observedAt: Date(), trust: .unverifiedLAN), revision: 7)
        let session = GuestSession(id: "private-grant", name: privateGuest, device: "private-device")
        let room = RoomSnapshot(id: Room203Defaults.gatewayID, name: "private-room-label", revision: 3, role: .owner,
                                connection: .internet, lights: [light], groups: [], scenes: [], policy: AccessPolicy(), sessions: [session], observedAt: Date())
        var evidence = ProbeEvidence(identity: try Room203Defaults.trustedIdentity())
        try evidence.accept(room, boot: "0123456789abcdef")
        let encoded = try JSONEncoder().encode(evidence), text = String(decoding: encoded, as: UTF8.self)
        for forbidden in [owner, guest, profile["salt"]!, profile["verifier"]!, privateName, privateGuest, privateMAC,
                          "private-group", "private-device", "private-grant", "private-room-label", "owner_key", "guest_password"] {
            try expect(!text.contains(forbidden))
        }
        try expect(evidence.identity_verified && evidence.room?.light_count == 1 && evidence.room?.guest_session_count == 1)
        var invalidRoom = room; invalidRoom.role = .guest
        try rejects { try evidence.accept(invalidRoom, boot: "0123456789abcdef") }
        try rejects { try evidence.accept(room, boot: nil) }
        try rejects { try evidence.accept(room, boot: "0123456789ABCDEF") }
        var bluetoothRoom = room; bluetoothRoom.connection = .bluetooth
        try rejects { try evidence.accept(bluetoothRoom, boot: "0123456789abcdef") }
        var bluetoothEvidence = ProbeEvidence(identity: try Room203Defaults.trustedIdentity(), transport: .bluetooth)
        try rejects { try bluetoothEvidence.accept(room, boot: "0123456789abcdef") }
        try bluetoothEvidence.accept(bluetoothRoom, boot: "0123456789abcdef")
        try expect(bluetoothEvidence.identity_verified && bluetoothEvidence.transport == "bluetooth")
        for operation in [ProbeOperation.authorization, .discovery] {
            var nonRoomEvidence = ProbeEvidence(identity: try Room203Defaults.trustedIdentity(), transport: .bluetooth, operation: operation)
            try rejects { try nonRoomEvidence.accept(bluetoothRoom, boot: "0123456789abcdef") }
            try expect(!nonRoomEvidence.identity_verified && nonRoomEvidence.room == nil && nonRoomEvidence.authenticated_role == nil)
        }
        var discoveredEvidence = ProbeEvidence(identity: try Room203Defaults.trustedIdentity(), transport: .bluetooth, operation: .discovery)
        discoveredEvidence.discovered_peripherals = collection.sorted
        let discoveredJSON = String(decoding: try JSONEncoder().encode(discoveredEvidence), as: UTF8.self)
        try expect(!discoveredJSON.contains("advertised_name") && !discoveredJSON.contains("manufacturer") && !discoveredJSON.contains(owner))
        try expect(ProbeEvidence.classify(NSError(domain: owner, code: 123, userInfo: [NSLocalizedDescriptionKey: guest])) == "probe_failed")
        try expect(ProbeEvidence.classify(GatewayAdapterError.busy) == "session_busy")
        let output = directory.appendingPathComponent("evidence.json").path
        let sink = try PrivateProbeEvidence(output); try sink.write(evidence)
        var info = stat(); try expect(stat(output, &info) == 0 && info.st_mode & 0o7777 == 0o600)
        try expect(!(try PrivateProbeFile.read(output)).isEmpty)
        try rejects { _ = try PrivateProbeEvidence(output) }
        try rejects { try sink.write(evidence) }
        print("LightsGatewayProbeChecks: \(assertions) assertions passed. Synthetic bundles only; no live file, Keychain, network or radio access.")
    }
}

private actor ProbeForbiddenReadiness: GatewayReadinessChecking {
    private(set) var calls = 0
    func isOnline(endpoint: URL, trustedIdentity: TrustedGatewayIdentity) async throws -> Bool {
        calls += 1; throw ProbeFailure.arguments
    }
}
