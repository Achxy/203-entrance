import Foundation
import Darwin
import LightsCore
import LightsMenulet

@MainActor enum ProbeChecks {
    static func run() throws {
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

        let owner = Data(repeating: 0x42, count: 32).base64EncodedString().replacingOccurrences(of: "=", with: "")
        let guest = "fixture-only-guest-password"
        let profile = ["salt": String(repeating: "0a", count: 16), "verifier": String(repeating: "0b", count: 384)]
        let object: [String: Any] = ["v": 1, "gateway_id": Room203Defaults.gatewayID, "owner_key": owner,
                                    "guest_password": guest, "owner": profile, "guest": profile]
        let data = try JSONSerialization.data(withJSONObject: object, options: [.sortedKeys])
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
