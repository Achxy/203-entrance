import Foundation
import CoreFoundation
import CryptoKit
import Darwin
import LightsCore
import LightsMenulet
import LightsSecurity

enum ProbeFailure: Error { case arguments, inputPermission, inputSize, inputFormat, wrongGateway, evidenceFile, keychainForbidden, deadline, identityMissing }

struct ProbeOptions: Sendable {
    let bundle: String
    let evidence: String
    let timeoutSeconds: UInt64
    enum Mode { case help, check, live(ProbeOptions) }
    static let usage = """
    LightsGatewayProbe: read-only owner authentication and room inspection.
    No arguments: show help without opening files, network connections or radios.

      LightsGatewayProbe --check
      LightsGatewayProbe --live --bundle /absolute/owner-access.json \
        --evidence /absolute/new-private-evidence.json [--transport internet] [--timeout 45]

    Live mode verifies the compiled Room 203 identity and reads room/light/session
    pages. It cannot change lights, policies, guests or provisioning. Bluetooth
    and endpoint overrides are intentionally unsupported. The evidence path must
    not already exist; it is created with permissions 0600. No Keychain is used.
    """
    static func parse(_ arguments: [String]) throws -> Mode {
        if arguments.isEmpty || arguments == ["--help"] { return .help }
        if arguments == ["--check"] { return .check }
        var live = false, fields: [String: String] = [:], index = 0
        while index < arguments.count {
            let key = arguments[index]; index += 1
            if key == "--live" { guard !live else { throw ProbeFailure.arguments }; live = true; continue }
            guard ["--bundle", "--evidence", "--transport", "--timeout"].contains(key), fields[key] == nil,
                  index < arguments.count, !arguments[index].hasPrefix("--") else { throw ProbeFailure.arguments }
            fields[key] = arguments[index]; index += 1
        }
        guard live, let bundle = fields["--bundle"], let evidence = fields["--evidence"], bundle != evidence,
              absolutePath(bundle), absolutePath(evidence), fields["--transport", default: "internet"] == "internet",
              let timeout = UInt64(fields["--timeout", default: "45"]), (5...120).contains(timeout) else { throw ProbeFailure.arguments }
        return .live(Self(bundle: bundle, evidence: evidence, timeoutSeconds: timeout))
    }
    private static func absolutePath(_ value: String) -> Bool {
        value.hasPrefix("/") && value.utf8.count < 4096 && !value.unicodeScalars.contains { CharacterSet.controlCharacters.contains($0) }
    }
}

struct ProbeOwnerBundle {
    private(set) var credential: Data
    mutating func clear() { credential.resetBytes(in: credential.startIndex..<credential.endIndex); credential.removeAll() }
    static func load(_ path: String, expectedGateway: String) throws -> Self {
        var bytes = try PrivateProbeFile.read(path)
        defer { bytes.resetBytes(in: bytes.startIndex..<bytes.endIndex); bytes.removeAll() }
        return try decode(bytes, expectedGateway: expectedGateway)
    }
    static func decode(_ bytes: Data, expectedGateway: String) throws -> Self {
        do {
            try StrictJSONValidator.validate(bytes, maximumBytes: 8192)
            guard let object = try JSONSerialization.jsonObject(with: bytes) as? [String: Any],
                  Set(object.keys) == ["v", "gateway_id", "owner_key", "guest_password", "owner", "guest"],
                  let version = object["v"] as? NSNumber, CFGetTypeID(version) != CFBooleanGetTypeID(), version == 1,
                  let gateway = object["gateway_id"] as? String,
                  let owner = object["owner_key"] as? String, canonicalOwnerKey(owner),
                  let guest = object["guest_password"] as? String, (1...256).contains(guest.utf8.count),
                  !guest.unicodeScalars.contains(where: { CharacterSet.controlCharacters.contains($0) }),
                  profile(object["owner"]), profile(object["guest"]) else { throw ProbeFailure.inputFormat }
            guard gateway == expectedGateway else { throw ProbeFailure.wrongGateway }
            return Self(credential: Data(owner.utf8))
        } catch let error as ProbeFailure { throw error }
        catch { throw ProbeFailure.inputFormat }
    }
    private static func canonicalOwnerKey(_ text: String) -> Bool {
        guard text.utf8.count == 43, text.utf8.allSatisfy({ (65...90).contains($0) || (97...122).contains($0) || (48...57).contains($0) || $0 == 45 || $0 == 95 }),
              let decoded = Data(base64Encoded: text.replacingOccurrences(of: "-", with: "+").replacingOccurrences(of: "_", with: "/") + "="), decoded.count == 32 else { return false }
        return decoded.base64EncodedString().replacingOccurrences(of: "+", with: "-").replacingOccurrences(of: "/", with: "_").replacingOccurrences(of: "=", with: "") == text
    }
    private static func profile(_ input: Any?) -> Bool {
        guard let value = input as? [String: Any], Set(value.keys) == ["salt", "verifier"],
              let salt = value["salt"] as? String, let verifier = value["verifier"] as? String else { return false }
        return lowerHex(salt, count: 32) && lowerHex(verifier, count: 768)
    }
    private static func lowerHex(_ value: String, count: Int) -> Bool {
        value.utf8.count == count && value.utf8.allSatisfy { (48...57).contains($0) || (97...102).contains($0) }
    }
}

enum PrivateProbeFile {
    static func read(_ path: String) throws -> Data {
        let descriptor = Darwin.open(path, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC)
        guard descriptor >= 0 else { throw ProbeFailure.inputPermission }
        defer { Darwin.close(descriptor) }
        var info = stat()
        guard fstat(descriptor, &info) == 0, (info.st_mode & S_IFMT) == S_IFREG,
              info.st_mode & 0o7777 == 0o600, info.st_uid == getuid(), info.st_nlink == 1 else { throw ProbeFailure.inputPermission }
        guard (1...8192).contains(info.st_size) else { throw ProbeFailure.inputSize }
        var result = Data(), buffer = [UInt8](repeating: 0, count: 4096)
        while true {
            let count = buffer.withUnsafeMutableBytes { Darwin.read(descriptor, $0.baseAddress!, $0.count) }
            if count == 0 { break }
            if count < 0 { if errno == EINTR { continue }; throw ProbeFailure.inputPermission }
            guard result.count + count <= 8192 else { throw ProbeFailure.inputSize }
            result.append(contentsOf: buffer.prefix(count))
        }
        guard result.count == info.st_size else { throw ProbeFailure.inputSize }
        return result
    }
}

final class PrivateProbeEvidence {
    private let descriptor: Int32
    private var written = false
    init(_ path: String) throws {
        let fd = Darwin.open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, mode_t(0o600))
        guard fd >= 0 else { throw ProbeFailure.evidenceFile }
        var info = stat()
        guard fchmod(fd, mode_t(0o600)) == 0, fstat(fd, &info) == 0, (info.st_mode & S_IFMT) == S_IFREG,
              info.st_mode & 0o7777 == 0o600, info.st_uid == getuid(), info.st_nlink == 1 else {
            Darwin.close(fd); throw ProbeFailure.evidenceFile
        }
        descriptor = fd
    }
    deinit { Darwin.close(descriptor) }
    func write(_ evidence: ProbeEvidence) throws {
        guard !written else { throw ProbeFailure.evidenceFile }
        written = true
        let encoder = JSONEncoder(); encoder.outputFormatting = [.prettyPrinted, .sortedKeys, .withoutEscapingSlashes]
        var bytes = try encoder.encode(evidence); bytes.append(10)
        guard bytes.count <= 8192 else { throw ProbeFailure.evidenceFile }
        try bytes.withUnsafeBytes { buffer in
            var offset = 0
            while offset < buffer.count {
                let count = Darwin.write(descriptor, buffer.baseAddress!.advanced(by: offset), buffer.count - offset)
                if count < 0 && errno == EINTR { continue }
                guard count > 0 else { throw ProbeFailure.evidenceFile }
                offset += count
            }
        }
        guard fsync(descriptor) == 0 else { throw ProbeFailure.evidenceFile }
    }
}

struct ProbeEvidence: Encodable, Sendable {
    let v = 1
    let kind = "read_only_gateway_probe"
    let run_id: String
    let started_at: String
    let gateway_id: String
    let public_key_sha256: String
    let transport = "internet"
    var duration_ms: UInt64 = 0
    var outcome = "failed"
    var stage = "input"
    var error_code: String?
    var identity_verified = false
    var authenticated_boot: String?
    var authenticated_role: String?
    var room: Room?
    struct Room: Encodable, Sendable {
        let snapshot_revision: String
        let light_count, observed_light_count, pending_light_count, reachable_light_count, restricted_light_count: Int
        let guest_session_count, revoked_guest_session_count, expired_guest_session_count: Int
        let guest_freeze_active, owner_hold_active, controls_available: Bool
        init(_ snapshot: RoomSnapshot) {
            snapshot_revision = String(snapshot.revision); light_count = snapshot.lights.count
            observed_light_count = snapshot.lights.filter { $0.reported.observedAt != nil }.count
            pending_light_count = snapshot.lights.filter(\.reportPending).count
            reachable_light_count = snapshot.lights.filter(\.reachable).count
            restricted_light_count = snapshot.lights.filter { $0.controlUnavailableReason != nil }.count
            guest_session_count = snapshot.sessions.count
            revoked_guest_session_count = snapshot.sessions.filter(\.revoked).count
            expired_guest_session_count = snapshot.sessions.filter(\.expired).count
            guest_freeze_active = snapshot.policy.guestFreezeActive; owner_hold_active = snapshot.policy.ownerHoldActive
            controls_available = snapshot.controlUnavailableReason == nil
        }
    }
    init(identity: TrustedGatewayIdentity) {
        run_id = UUID().uuidString.lowercased(); started_at = ISO8601DateFormatter().string(from: Date())
        gateway_id = identity.gatewayID
        public_key_sha256 = Data(SHA256.hash(data: identity.publicKeySPKI)).map { String(format: "%02x", $0) }.joined()
    }
    mutating func accept(_ snapshot: RoomSnapshot, boot: String?) throws {
        guard snapshot.id == gateway_id, snapshot.role == .owner, snapshot.connection == .internet,
              let boot, boot.utf8.count == 16, boot.utf8.allSatisfy({ (48...57).contains($0) || (97...102).contains($0) }) else { throw ProbeFailure.identityMissing }
        authenticated_boot = boot; authenticated_role = "owner"; identity_verified = true
        room = Room(snapshot); stage = "complete"; outcome = "success"; error_code = nil
    }
    static func classify(_ error: Error) -> String {
        if let error = error as? ProbeFailure {
            switch error {
            case .inputPermission: return "bundle_permissions"
            case .inputSize: return "bundle_size"
            case .inputFormat: return "bundle_format"
            case .wrongGateway: return "wrong_gateway"
            case .deadline: return "probe_timeout"
            case .keychainForbidden: return "unexpected_keychain_access"
            case .identityMissing: return "identity_not_verified"
            case .arguments: return "invalid_arguments"
            case .evidenceFile: return "evidence_file_error"
            }
        }
        if let error = error as? GatewayAdapterError {
            switch error {
            case .busy: return "session_busy"
            case .unavailable: return "gateway_unavailable"
            case .authenticationFailed: return "authentication_failed"
            case .malformedReply: return "invalid_gateway_reply"
            case .changedBoot: return "gateway_restarted"
            default: return "gateway_rejected"
            }
        }
        if error is Security2Error { return "security_failed" }
        if let error = error as? RelayChannelError {
            switch error {
            case .timeout: return "transport_timeout"
            case .rejected: return "relay_rejected"
            default: return "relay_error"
            }
        }
        if error is URLError { return "network_error" }
        if error is CancellationError { return "cancelled" }
        return "probe_failed"
    }
}

private struct ProbeNoSecretStore: GatewayAccessStore {
    func read(identity: Data, kind: StoredSecretKind) throws -> Data? { throw ProbeFailure.keychainForbidden }
    func save(_ value: Data, identity: Data, kind: StoredSecretKind) throws { throw ProbeFailure.keychainForbidden }
    func remove(identity: Data, kind: StoredSecretKind) throws { throw ProbeFailure.keychainForbidden }
}

@MainActor enum ReadOnlyGatewayProbe {
    static func run(_ options: ProbeOptions) async -> Int32 {
        let began = ProcessInfo.processInfo.systemUptime
        do {
            let identity = try Room203Defaults.trustedIdentity()
            let sink = try PrivateProbeEvidence(options.evidence)
            var evidence = ProbeEvidence(identity: identity)
            do {
                var bundle = try ProbeOwnerBundle.load(options.bundle, expectedGateway: identity.gatewayID)
                defer { bundle.clear() }
                let coordinator = GatewayRouteCoordinator(trustedIdentity: identity)
                let adapter = GatewayAdapter(trustedIdentity: identity, enabled: true, store: ProbeNoSecretStore(),
                                             onRouteResult: { coordinator.recordResult($0, succeeded: $1, roundTripMilliseconds: $2) },
                                             channelFactory: { try await coordinator.makeRoute($0, excluding: $1) })
                evidence.stage = "authentication_and_room_read"
                do {
                    let request = AccessRequest(endpoint: Room203Defaults.relayEndpoint.absoluteString, name: "Owner probe", role: .owner,
                                                credential: String(decoding: bundle.credential, as: UTF8.self), preference: .internet, rememberAccess: false)
                    let snapshot = try await withThrowingTaskGroup(of: RoomSnapshot.self) { group in
                        group.addTask { try await adapter.connect(request) }
                        group.addTask { try await Task.sleep(for: .seconds(options.timeoutSeconds)); throw ProbeFailure.deadline }
                        defer { group.cancelAll() }
                        guard let result = try await group.next() else { throw ProbeFailure.deadline }
                        return result
                    }
                    try evidence.accept(snapshot, boot: adapter.authenticatedBootIdentifier)
                    await adapter.disconnect()
                } catch {
                    await adapter.disconnect()
                    throw error
                }
            } catch { evidence.error_code = ProbeEvidence.classify(error) }
            evidence.duration_ms = UInt64(max(0, (ProcessInfo.processInfo.systemUptime - began) * 1000))
            try sink.write(evidence)
            if evidence.outcome == "success" {
                print("Owner identity verified; room status read. Private sanitized evidence written. No lighting or policy changes sent.")
                return 0
            }
            FileHandle.standardError.write(Data("Read-only probe failed (\(evidence.error_code ?? "probe_failed")). Private sanitized evidence written.\n".utf8))
            return 1
        } catch {
            FileHandle.standardError.write(Data("Probe stopped (\(ProbeEvidence.classify(error))). Evidence could not be written; no secret details are logged.\n".utf8))
            return 2
        }
    }
}
