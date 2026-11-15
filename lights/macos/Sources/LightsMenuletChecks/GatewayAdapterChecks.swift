import Foundation
import CryptoKit
import Darwin
import LightsCore
import LightsMenulet
import LightsSecurity

/// Real client crypto against a scripted, local-only peer. These checks validate
/// the application contract, not an ESP, network path, or physical bulb.
@MainActor enum GatewayAdapterChecks {
    static func run() async throws {
        var count = 0
        func expect(_ value: Bool, _ message: String) throws {
            guard value else { throw CheckFailure.failed("Gateway adapter: " + message) }
            count += 1
        }
        func rejected(_ body: () async throws -> Void) async throws {
            do { try await body() } catch { count += 1; return }
            throw CheckFailure.failed("Gateway adapter expected rejection")
        }
        var scalar = Data(repeating: 0, count: 29); scalar.append(contentsOf: [0x20, 0x3c, 0xa7])
        let publicKey = try P256.Signing.PrivateKey(rawRepresentation: scalar).publicKey.derRepresentation
        let identity = try TrustedGatewayIdentity(gatewayID: "entrance-test", publicKeySPKI: publicKey)
        let store = ScriptedAccessStore()
        let state = ScriptedRoom()
        let factory: @MainActor @Sendable (AccessRequest, Set<GatewayRoute>) async throws -> GatewayAdapterRoute = { request, excluded in
            let route: GatewayRoute = excluded.contains(.internet) ? .bluetooth : .internet
            guard request.preference == .automatic || route == .internet else { throw RelayChannelError.notOpen }
            return GatewayAdapterRoute(channel: try ScriptedByteChannel(room: state, role: request.role), route: route)
        }
        let disabled = GatewayAdapter(trustedIdentity: identity, channelFactory: factory)
        let guest = AccessRequest(endpoint: "wss://fixture.invalid/v1/lights", name: "Test guest", role: .guest,
                                  credential: "not-a-real-lighting-password", preference: .automatic)
        try await rejected { _ = try await disabled.connect(guest) }
        try expect(await state.opens == 0, "default qualification gate must not open a channel")
        for status in [409, 401] {
            let attempts = ScriptedConnectionAttempts()
            let refusal = GatewayAdapter(trustedIdentity: identity, enabled: true, store: store) { _, _ in
                attempts.count += 1
                return GatewayAdapterRoute(channel: RejectingByteChannel(status: status), route: .bluetooth)
            }
            do {
                _ = try await refusal.connect(guest)
                throw CheckFailure.failed("Expected an explicit handshake refusal")
            } catch let error as GatewayAdapterError {
                try expect(error == (status == 409 ? .busy : .authenticationFailed), "BLE busy is distinct from authentication failure")
            }
            try expect(attempts.count == 1, "explicit busy/auth refusal does not probe another route")
        }

        let clock = ScriptedAdapterClock()
        let adapter = GatewayAdapter(trustedIdentity: identity, enabled: true, store: store, clock: clock, channelFactory: factory)
        var room = try await adapter.connect(guest)
        try expect(room.connection == .internet && room.role == .guest && room.lights.count == 2, "real encrypted guest connection maps actual pages")
        try expect(room.lights[0].reported.power == false && room.lights[0].reported.trust == .unverifiedLAN, "observations remain unverified LAN evidence")
        try expect(store.savedCount == 0, "without Remember Access no Keychain write occurs")
        let command = GatewayCommand(expectedRevision: room.revision, action: .lighting(ids: ["aabbcc000001"], patch: .init(power: true)))
        let outcome = try await adapter.execute(command)
        room = outcome.snapshot
        try expect(outcome.confirmedIDs == ["aabbcc000001"] && room.lights[0].reported.power == true, "receipt and readback confirm only the requested light")
        let sentData = await state.lastSetData
        let sent = try sentData.map { try JSONSerialization.jsonObject(with: $0) as! [String: Any] }
        try expect(sent?["id"] as? String == command.operationID.uuidString.replacingOccurrences(of: "-", with: "").lowercased(), "command UUID uses exact32hex wire form")
        try expect(sent?["sequence"] as? String == "1" && sent?["expires_ms"] as? String == "101500", "sequence and original monotonic expiry are bounded decimal strings")
        try expect((sent?["expected"] as? [Int])?.count == 8 && (sent?["expected"] as? [Int])?[0] == 7, "all eight target revisions are encoded")
        let operations = await state.operations
        if let setAt = operations.lastIndex(of: "set") {
            try expect(setAt > 1 && operations[setAt-1] == "room" && operations[setAt-2] == "light", "affected light precedes final fresh room lease")
        } else { throw CheckFailure.failed("Missing set") }
        let writes = await state.setCount
        _ = try await adapter.execute(command)
        try expect(await state.setCount == writes, "retrying same operation never repeats set")
        try await rejected { _ = try await adapter.execute(GatewayCommand(expectedRevision: 0, action: .lighting(ids: ["aabbcc000001"], patch: .init(power: false)))) }
        try expect(await state.setCount == writes, "stale UI revision cannot write")

        await state.setUnknownObservation(true)
        room = try await adapter.refresh()
        try expect(room.lights[1].reported.power == nil && room.lights[1].reported.observedAt == nil && !room.lights[1].reachable, "unknown state stays unknown, never invented off")
        await state.setUnknownObservation(false)
        room = try await adapter.refresh()
        let openBeforeIdle = await state.opens, resumesBeforeIdle = await state.resumeCount
        clock.uptime += 56
        room = try await adapter.refresh()
        let openAfterIdle = await state.opens
        try expect(room.connection == .internet && openAfterIdle == openBeforeIdle + 1, "expected idle expiry renews the available route")
        try expect(await state.resumeCount == resumesBeforeIdle + 1, "idle renewal resumes the same grant")
        await state.dropNextSetReply()
        let ambiguous = GatewayCommand(expectedRevision: room.revision, action: .lighting(ids: ["aabbcc000001"], patch: .init(power: false)))
        let reconciled = try await adapter.execute(ambiguous)
        try expect(reconciled.snapshot.connection == .bluetooth && reconciled.confirmedIDs == ["aabbcc000001"], "lost reply closes old path, authenticates alternate and queries receipt")
        try expect(await state.setCount == writes + 1, "ambiguous set was sent once across failover")
        let firstEnrollCount = await state.enrollCount, firstResumeCount = await state.resumeCount
        try expect(firstEnrollCount == 1 && firstResumeCount == 2, "idle and failover resume the original grant")
        try expect(await state.maximumOpen == 1, "at most one channel is active during failover")
        let oldResult = try await adapter.execute(command)
        try expect(oldResult.confirmedIDs.isEmpty && oldResult.snapshot.lights[0].reported.power == false,
                   "completed operation reuse returns current state, not an obsolete confirmed snapshot")
        room = oldResult.snapshot
        await state.returnMismatchedReadback()
        let mismatch = try await adapter.execute(GatewayCommand(expectedRevision: room.revision, action: .lighting(ids: ["aabbcc000001"], patch: .init(power: true))))
        try expect(mismatch.confirmedIDs.isEmpty && mismatch.failures["aabbcc000001"] != nil, "receipt alone cannot confirm a superseded readback")
        await state.holdUnverifiedReceipt(true)
        let pendingCommand = GatewayCommand(expectedRevision: mismatch.snapshot.revision, action: .lighting(ids: ["aabbcc000001"], patch: .init(power: true)))
        let pending = try await adapter.execute(pendingCommand)
        try expect(pending.confirmedIDs.isEmpty && pending.snapshot.lights[0].reportPending, "queued or sent is pending, never a confirmed bulb setting")
        let beforePendingResolve = await state.setCount
        await state.holdUnverifiedReceipt(false)
        let settled = try await adapter.execute(pendingCommand)
        let afterPendingResolve = await state.setCount
        try expect(settled.confirmedIDs == ["aabbcc000001"] && afterPendingResolve == beforePendingResolve, "pending operation resolves by original UUID without another write")
        await state.setMaintenance(true)
        room = try await adapter.refresh()
        try expect(room.controlUnavailableReason != nil, "storage maintenance remains readable and blocks room controls")
        try await rejected { _ = try await adapter.execute(GatewayCommand(expectedRevision: room.revision, action: .lighting(ids: ["aabbcc000001"], patch: .init(power: false)))) }
        await state.setMaintenance(false)
        await state.setSigningBlocked(true)
        room = try await adapter.refresh()
        try expect(room.lights[0].controlUnavailableReason != nil, "typed signing setup block disables only affected controls")
        try await rejected { _ = try await adapter.execute(GatewayCommand(expectedRevision: room.revision, action: .lighting(ids: ["aabbcc000001"], patch: .init(power: false)))) }
        await state.setSigningBlocked(false)
        await adapter.disconnect()

        let remembered = AccessRequest(endpoint: guest.endpoint, name: guest.name, role: .guest, credential: guest.credential,
                                       preference: .automatic, rememberAccess: true)
        let saved = GatewayAdapter(trustedIdentity: identity, enabled: true, store: store, channelFactory: factory)
        _ = try await saved.connect(remembered)
        try expect(store.savedKinds.contains(.guestGrant) && store.savedKinds.contains(.guestPassword), "explicit remember stores password and per-install grant")
        await saved.disconnect()
        await state.revokeCurrentGrant()
        let resume = AccessRequest(endpoint: guest.endpoint, name: guest.name, role: .guest, credential: "", preference: .automatic, rememberAccess: true)
        let enrollBefore = await state.enrollCount
        try await rejected { _ = try await saved.connect(resume) }
        try await rejected { _ = try await saved.connect(resume) }
        try expect(await state.enrollCount == enrollBefore, "revoked grant never silently enrolls a replacement")
        _ = try await saved.explicitlyRejoinGuest(resume)
        try expect(await state.enrollCount == enrollBefore + 1, "only explicit rejoin creates a fresh grant")
        store.failRemoval = true
        try await rejected { try await saved.signOut() }
        try expect(store.savedCount > 0, "Keychain cleanup failure is visible and not claimed successful")
        store.failRemoval = false
        try await saved.signOut()
        try expect(store.savedCount == 0, "explicit sign out deletes remembered secrets")

        let owner = GatewayAdapter(trustedIdentity: identity, enabled: true, store: store, channelFactory: factory)
        let ownerRequest = AccessRequest(endpoint: guest.endpoint, name: "Owner", role: .owner, credential: guest.credential, preference: .automatic)
        room = try await owner.connect(ownerRequest)
        try expect(room.sessions.count == 1 && room.sessions[0].device == nil && room.sessions[0].lastActive == nil, "occupied session page maps no invented device/activity")
        try expect(room.ownerCapabilities.contains(.importHomeKey) && !room.ownerCapabilities.contains(.holdSettings), "only implemented owner capabilities are advertised")
        let sessionPages = await state.sessionPages
        try expect(sessionPages.allSatisfy { $0 == 3 }, "owner only fetches occupied guest slots")
        try await rejected { _ = try await owner.execute(GatewayCommand(expectedRevision: room.revision, action: .owner(.holdSettings(until: Date().addingTimeInterval(30))))) }
        let frozen = try await owner.execute(GatewayCommand(expectedRevision: room.revision, action: .owner(.freezeGuests(until: Date().addingTimeInterval(60)))))
        try expect(frozen.snapshot.policy.guestFreezeActive && frozen.snapshot.policy.guestFreezeUntil == nil, "freeze mask is shown without an invented expiry")
        room = frozen.snapshot
        let imported = try await owner.execute(GatewayCommand(expectedRevision: room.revision, action: .owner(.importHomeKey(Data(repeating: 0x52, count: 16)))))
        try expect(await state.homeKeyCount == 1 && store.savedCount == 0, "WiZ key import is encrypted and never retained by this adapter")
        let rotated = try await owner.execute(GatewayCommand(expectedRevision: imported.snapshot.revision, action: .owner(.rotateGuestPassword("another-test-only-password"))))
        try expect(await state.rotationWasVerifierOnly, "password rotation sends only16-byte salt and384-byte verifier")
        try expect(rotated.snapshot.policy.guestPasswordRevision > room.policy.guestPasswordRevision, "durable guest epoch is refreshed after rotation")
        try await owner.startDiscovery()
        let candidate = try await owner.candidate(at: 0)
        try expect(candidate.total == 1 && candidate.candidate?.mac == "aabbcc000003", "discovery reads typed candidate metadata")
        try await owner.cancelDiscovery()
        _ = try await owner.adoptCandidate(0, name: "Third light", allowLegacyControl: true)
        try expect(await state.adoptCount == 1, "explicit candidate adoption uses bounded candidate index")
        await state.useMalformedReply()
        try await rejected { _ = try await owner.refresh() }
        try expect(owner.snapshot?.connection == .offline, "malformed authenticated JSON closes the connection")
        await owner.disconnect()
        print("Gateway adapter wire checks: \(count) assertions passed; scripted local peer only, no hardware or network claims")
    }
}

@MainActor private final class ScriptedAdapterClock: GatewayAdapterClock {
    var wallTime = Date()
    var uptime: TimeInterval = 1
    func sleep(seconds: TimeInterval) async throws { uptime += max(0, seconds) }
}
@MainActor private final class ScriptedConnectionAttempts { var count = 0 }

private actor RejectingByteChannel: GatewayByteChannel {
    let status: Int
    init(status: Int) { self.status = status }
    func open() async throws -> RelayChannelIdentity { throw BluetoothChannelError.rejected(status) }
    func exchange(_ operation: RelayOperation, payload: Data, roleHint: AccessRole?) async throws -> Data { throw BluetoothChannelError.notOpen }
    func close() async {}
}

private final class ScriptedAccessStore: GatewayAccessStore, @unchecked Sendable {
    private let lock = NSLock()
    private var values: [String: Data] = [:]
    var failRemoval = false
    var savedCount: Int { lock.lock(); defer { lock.unlock() }; return values.count }
    var savedKinds: Set<StoredSecretKind> { lock.lock(); defer { lock.unlock() }; return Set(values.keys.compactMap { StoredSecretKind(rawValue: $0) }) }
    func read(identity: Data, kind: StoredSecretKind) throws -> Data? { lock.lock(); defer { lock.unlock() }; return values[kind.rawValue] }
    func save(_ value: Data, identity: Data, kind: StoredSecretKind) throws { lock.lock(); defer { lock.unlock() }; values[kind.rawValue] = value }
    func remove(identity: Data, kind: StoredSecretKind) throws {
        lock.lock(); defer { lock.unlock() }
        if failRemoval { throw CredentialStoreError.invalidValue }
        values[kind.rawValue] = nil
    }
}

private actor ScriptedRoom {
    var opens = 0, active = 0, maximumOpen = 0, setCount = 0, enrollCount = 0, resumeCount = 0, homeKeyCount = 0, adoptCount = 0
    var operations: [String] = [], sessionPages: [Int] = []
    var lastSetData: Data?
    var rotationWasVerifierOnly = false
    private var token = "", revokedTokens = Set<String>(), unknownObservation = false, loseSet = false, malformed = false
    private var sequence: UInt64 = 0, policy: UInt64 = 1, guestEpoch: UInt64 = 1
    private var frozen = 0, power = false, revision = 7
    private var commandID: String?, commandPolls = 0
    private var mismatchReadback = false
    private var unverifiedReceipt = false, maintenance = false, signingBlocked = false
    func opened() { opens += 1; active += 1; maximumOpen = max(maximumOpen, active) }
    func closed() { active -= 1 }
    func setUnknownObservation(_ value: Bool) { unknownObservation = value }
    func dropNextSetReply() { loseSet = true }
    func revokeCurrentGrant() { revokedTokens.insert(token) }
    func useMalformedReply() { malformed = true }
    func returnMismatchedReadback() { mismatchReadback = true }
    func holdUnverifiedReceipt(_ value: Bool) { unverifiedReceipt = value }
    func setMaintenance(_ value: Bool) { maintenance = value }
    func setSigningBlocked(_ value: Bool) { signingBlocked = value }

    func reply(_ data: Data, role: AccessRole) throws -> Data {
        let request = try JSONSerialization.jsonObject(with: data) as! [String: Any]
        let op = request["op"] as! String
        operations.append(op)
        if malformed { malformed = false; return Data("{\"v\":1,\"v\":1,\"ok\":true,\"code\":\"ok\"}".utf8) }
        var response: [String: Any] = ["v": 1, "ok": true, "code": "ok"]
        func reject(_ code: String) { response = ["v": 1, "ok": false, "code": code] }
        switch op {
        case "guest.enroll", "guest.resume":
            let proposed = request["token"] as! String
            if revokedTokens.contains(proposed) { reject("revoked") }
            else if op == "guest.resume", proposed != token { reject("revoked") }
            else {
                if op == "guest.enroll" { enrollCount += 1; token = proposed } else { resumeCount += 1 }
                response["grant"] = "123"; response["guest_epoch"] = String(guestEpoch)
            }
        case "room":
            response.merge(["boot": "0123456789abcdef", "now_ms": "100000", "policy_epoch": String(policy),
                            "guest_epoch": String(guestEpoch), "last_sequence": String(sequence), "enabled": 3,
                            "frozen": frozen, "owner_held": 0, "storage_healthy": !maintenance, "driver_busy": false,
                            "bulb_clock_ready": !signingBlocked, "max_lights": 5]) { _, b in b }
            if role == .owner { response["guest_slots"] = 8 }
        case "light":
            let index = request["index"] as! Int
            response.merge(["index": index, "name": index == 0 ? "Desk" : "Ceiling", "mac": index == 0 ? "aabbcc000001" : "aabbcc000002",
                            "revision": index == 0 ? revision : 7, "observed_valid": !(index == 1 && unknownObservation), "observed_ms": "99900",
                            "pending": index == 0 && unverifiedReceipt, "last_result": "verified", "reported": ["power": index == 0 ? power : false, "brightness": 50],
                            "capabilities": ["features": 63, "evidence": 7, "brightness_min": 10, "kelvin_min": 2200, "kelvin_max": 6500,
                                             "module": "ESP01_SHRGB1C_31", "firmware": "fixture", "scenes": [1, 3, 4]],
                            "reply_trust": "unverified_lan", "legacy_control": true, "error": index == 0 && signingBlocked ? "clock_required" : "ok", "permission_enforcement": "gateway_only"] as [String: Any]) { _, b in b }
        case "sessions":
            let page = request["page"] as! Int; sessionPages.append(page)
            response.merge(["page": page, "present": true, "grant": "123", "name": "A self-asserted guest", "expires_ms": "200000", "revoked": false, "expired": false]) { _, b in b }
        case "set":
            setCount += 1; lastSetData = data
            commandID = request["id"] as? String; commandPolls = 0
            sequence = UInt64(request["sequence"] as! String)!
            let patch = request["patch"] as! [String: Any]
            if let value = patch["power"] as? Bool { power = value }
            if mismatchReadback { power.toggle(); mismatchReadback = false }
            revision += 1
            if loseSet { loseSet = false; throw RelayChannelError.timeout }
            response.merge(receipt(result: "queued", verified: 0)) { _, b in b }
        case "command":
            guard request["id"] as? String == commandID else { reject("not_found"); break }
            commandPolls += 1
            let verified = commandPolls > 1 && !unverifiedReceipt
            response.merge(receipt(result: verified ? "verified" : "sent", verified: verified ? 1 : 0)) { _, b in b }
        case "freeze": frozen = (request["duration_ms"] as! NSNumber).uint64Value == 0 ? 0 : 3; policy += 1; response["policy_epoch"] = String(policy)
        case "revoke", "revoke_all": policy += 1; response["policy_epoch"] = String(policy)
        case "home_key": homeKeyCount += 1
        case "rotate_guest":
            rotationWasVerifierOnly = Set(request.keys) == ["v", "op", "salt", "verifier"] && (request["salt"] as? String)?.count == 32 && (request["verifier"] as? String)?.count == 768
            policy += 1; guestEpoch += 1; response["policy_epoch"] = String(policy)
        case "discover": response["code"] = "discovery_started"
        case "discovery_cancel": break
        case "candidate": response.merge(["discovering": false, "count": 1, "present": true, "mac": "aabbcc000003", "identified": true,
                                            "module": "fixture", "firmware": "fixture", "reply_trust": "unverified_lan", "error": "ok"]) { _, b in b }
        case "adopt": adoptCount += 1; response["index"] = 2
        default: throw CheckFailure.failed("Unexpected fixture operation")
        }
        return try JSONSerialization.data(withJSONObject: response, options: [.sortedKeys])
    }
    private func receipt(result: String, verified: Int) -> [String: Any] {
        ["id": commandID!, "result": result, "error": "ok", "sent": result == "queued" ? 0 : 1, "verified": verified, "failed": 0]
    }
}

private actor ScriptedByteChannel: GatewayByteChannel {
    private let room: ScriptedRoom
    private let role: AccessRole
    private let peer: ScriptedSecurityPeer
    private var openState = false
    init(room: ScriptedRoom, role: AccessRole) throws { self.room = room; self.role = role; peer = try ScriptedSecurityPeer(role: role) }
    func open() async throws -> RelayChannelIdentity {
        openState = true; await room.opened()
        return RelayChannelIdentity(connection: "00000000000000000000000000000001", boot: "0123456789abcdef")
    }
    func exchange(_ operation: RelayOperation, payload: Data, roleHint: AccessRole?) async throws -> Data {
        guard openState else { throw RelayChannelError.notOpen }
        var message = try peer.send(["op": operation.rawValue, "payload": payload.base64EncodedString()])
        if let application = message["application"] as? String, let plaintext = Data(base64Encoded: application) {
            let response = try await room.reply(plaintext, role: role)
            message = try peer.send(["reply": response.base64EncodedString()])
        }
        guard let encoded = message["payload"] as? String, let data = Data(base64Encoded: encoded) else { throw CheckFailure.failed("Scripted security peer failed") }
        return data
    }
    func close() async {
        if openState { openState = false; await room.closed() }
        peer.close()
    }
}

private final class ScriptedSecurityPeer: @unchecked Sendable {
    private let process = Process()
    private let input = Pipe(), output = Pipe()
    private var buffer = Data()
    init(role: AccessRole) throws {
        var root = URL(fileURLWithPath: #filePath)
        for _ in 0..<5 { root.deleteLastPathComponent() }
        let override = ProcessInfo.processInfo.environment["LIGHTS_TEST_PYTHON"]
        let python = override.map { URL(fileURLWithPath: $0) } ?? root.appendingPathComponent("lights/security/.research/venv/bin/python")
        guard FileManager.default.isExecutableFile(atPath: python.path), override == nil || override!.hasPrefix("/") else {
            throw CheckFailure.failed("Run bash lights/security/scripts/bootstrap-test-env.sh or set LIGHTS_TEST_PYTHON to an absolute compatible Python path")
        }
        process.executableURL = python
        process.arguments = ["-u", "-c", Self.server, root.path, role.rawValue]
        process.standardInput = input; process.standardOutput = output
        process.standardError = FileHandle.nullDevice
        try process.run()
    }
    deinit { close() }
    func close() { if process.isRunning { process.terminate() }; try? input.fileHandleForWriting.close(); try? output.fileHandleForReading.close() }
    func send(_ object: [String: Any]) throws -> [String: Any] {
        var bytes = try JSONSerialization.data(withJSONObject: object); bytes.append(10)
        try input.fileHandleForWriting.write(contentsOf: bytes)
        while !buffer.contains(10) {
            var descriptor = pollfd(fd: output.fileHandleForReading.fileDescriptor, events: Int16(POLLIN), revents: 0)
            guard poll(&descriptor, 1, 5000) > 0 else { throw CheckFailure.failed("Scripted peer deadline") }
            var chunk = [UInt8](repeating: 0, count: 4096)
            let received = chunk.withUnsafeMutableBytes { Darwin.read(descriptor.fd, $0.baseAddress!, $0.count) }
            guard received > 0 else { throw CheckFailure.failed("Scripted peer process exit") }
            buffer.append(contentsOf: chunk.prefix(received))
            guard buffer.count < 8192 else { throw CheckFailure.failed("Scripted peer response bound") }
        }
        let newline = buffer.firstIndex(of: 10)!
        let line = buffer.prefix(upTo: newline); buffer.removeSubrange(...newline)
        guard let value = try JSONSerialization.jsonObject(with: line) as? [String: Any] else { throw CheckFailure.failed("Scripted peer malformed response") }
        return value
    }
    private static let server = #"""
import sys, os, json, base64, hashlib, hmac, struct
import cryptography, google.protobuf
assert cryptography.__version__ == '46.0.5' and google.protobuf.__version__ == '6.33.2'
from cryptography.hazmat.primitives.ciphers.aead import AESGCM
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives import hashes
root, role = sys.argv[1:]
idf = root + '/.tools/esp-idf'
os.environ['IDF_PATH'] = idf
sys.path.insert(0, idf + '/examples/protocols/esp_local_ctrl/scripts')
import proto
from security.srp6a import get_ng, NG_3072
N,g = get_ng(NG_3072)
H = lambda b: hashlib.sha512(b).digest()
raw = lambda n: n.to_bytes((n.bit_length()+7)//8,'big') or b'\0'
salt = bytes.fromhex('036ee0c7bcb9eda84c9eac97d93decf4f4aa')
username = ('203-' + role).encode()
password = b'not-a-real-lighting-password'
key = ec.derive_private_key(0x203ca7, ec.SECP256R1())
records = []
counter = 1
def emit(v): print(json.dumps(v,separators=(',',':')),flush=True)
def encrypted(plain):
    global counter
    result = aes.encrypt(prefix + struct.pack('>I',counter),plain,None)
    counter += 1
    return result
while True:
    line=sys.stdin.readline()
    if not line: break
    item=json.loads(line)
    wire=base64.b64decode(item['payload'])
    if item['op']=='session':
        request=proto.session_pb2.SessionData.FromString(wire)
        response=proto.session_pb2.SessionData(sec_ver=2)
        if not records:
            assert request.sec2.sc0.client_username==username
            A=request.sec2.sc0.client_pubkey
            assert len(A)==384 and int.from_bytes(A,'big')%N
            x=int.from_bytes(H(salt+H(username+b':'+password)),'big')
            v=pow(g,x,N); k=int.from_bytes(H(raw(N)+g.to_bytes(384,'big')),'big')
            b=int.from_bytes(os.urandom(32),'big')
            B=raw((k*v+pow(g,b,N))%N)
            u=int.from_bytes(H(A+B.rjust(384,b'\0')),'big')
            S=pow((int.from_bytes(A,'big')*pow(v,u,N))%N,b,N)
            K=H(raw(S)); aes=AESGCM(K[:32]); prefix=os.urandom(8)
            xor=bytes(x^y for x,y in zip(H(raw(N)),H(g.to_bytes(384,'big'))))
            M=H(xor+H(username)+salt+A+B+K); proof=H(A+M+K)
            response.sec2.msg=1; response.sec2.sr0.device_pubkey=B; response.sec2.sr0.device_salt=salt
        else:
            assert hmac.compare_digest(request.sec2.sc1.client_proof,M)
            response.sec2.msg=3; response.sec2.sr1.device_proof=proof
            response.sec2.sr1.device_nonce=prefix+struct.pack('>I',1)
        encoded=response.SerializeToString(); records.extend([wire,encoded])
        emit({'payload':base64.b64encode(encoded).decode()})
    elif item['op']=='data':
        plain=aes.decrypt(prefix+struct.pack('>I',counter),wire,None); counter+=1
        request=json.loads(plain)
        if request['op']=='bind':
            transcript=hashlib.sha256(b''.join(struct.pack('>I',len(r))+r for r in records)).hexdigest()
            challenge=request['challenge']
            context=f'203-lights-channel-v1\nentrance-test\n0123456789abcdef\n{transcript}\n{challenge}\n{role}\n'.encode()
            signature=key.sign(context,ec.ECDSA(hashes.SHA256())).hex()
            reply=json.dumps(dict(v=1,op='bound',gateway_id='entrance-test',boot='0123456789abcdef',transcript=transcript,challenge=challenge,role=role,signature=signature),separators=(',',':')).encode()
        else:
            emit({'application':base64.b64encode(plain).decode()})
            reply=base64.b64decode(json.loads(sys.stdin.readline())['reply'])
        emit({'payload':base64.b64encode(encrypted(reply)).decode()})
"""#
}
