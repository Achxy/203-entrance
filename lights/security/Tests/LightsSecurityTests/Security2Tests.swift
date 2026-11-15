import Foundation
import CryptoKit
import LightsSecurity

final class Security2Checks {
    struct VerifierFixture: Decodable { let username, password, salt, verifier: String }
    struct VerifierFixtures: Decodable { let verifier_fixture, leading_zero_verifier_fixture: VerifierFixture }
    struct Vectors: Decodable { let idf_revision: String; let native_verifier_matched: Bool; let vectors: [Vector] }
    struct Vector: Decodable {
        let name, username, password, ephemeral, request0, response0, request1, response1: String
        let transcript, key, nonce, gateway, boot, role, spki, challenge: String
        let binding_context, binding_response, binding_ciphertext: String
        let plaintext_request, plaintext_response, request_ciphertext, response_ciphertext, next_response_ciphertext: String
        func bytes(_ path: KeyPath<Vector, String>) -> Data { Data(hex: self[keyPath: path])! }
        func session() throws -> Security2Session {
            try Security2Session(testingUsername: username, password: Data(password.utf8), ephemeral: bytes(\.ephemeral))
        }
        func identity() throws -> TrustedGatewayIdentity { try TrustedGatewayIdentity(gatewayID: gateway, publicKeySPKI: bytes(\.spki)) }
    }
    func vectors() throws -> [Vector] {
        let url = Bundle.module.url(forResource: "idf61", withExtension: "json", subdirectory: "Fixtures")!
        let fixtures = try JSONDecoder().decode(Vectors.self, from: Data(contentsOf: url))
        checkEqual(fixtures.idf_revision, "fff9895c82d744c7237be8847347bdd1b07c6643")
        checkTrue(fixtures.native_verifier_matched)
        return fixtures.vectors
    }
    func handshake(_ v: Vector) async throws -> Security2Session {
        let session = try v.session()
        let request0 = try await session.startHandshake()
        checkEqual(request0, v.bytes(\.request0), v.name)
        let request1 = try await session.receiveChallenge(v.bytes(\.response0))
        checkEqual(request1, v.bytes(\.request1), v.name)
        try await session.verifyServer(v.bytes(\.response1))
        let transcript = try await session.authenticatedTranscriptSHA256()
        checkEqual(transcript, v.bytes(\.transcript), v.name)
        let state = await session.state
        checkEqual(state, .readyForBinding)
        return session
    }
    func bound(_ v: Vector) async throws -> Security2Session {
        let session = try await handshake(v)
        let request = try await session.makeIdentityBindingRequest(trustedGateway: v.identity(), expectedBoot: v.boot,
                                                                  expectedRole: .guest, challenge: v.bytes(\.challenge))
        let box = try AES.GCM.SealedBox(nonce: AES.GCM.Nonce(data: v.bytes(\.nonce)),
                                        ciphertext: request.dropLast(16), tag: request.suffix(16))
        let plaintext = try AES.GCM.open(box, using: SymmetricKey(data: v.bytes(\.key)))
        let object = try JSONSerialization.jsonObject(with: plaintext) as! [String: Any]
        checkEqual(Set(object.keys), Set(["v", "op", "challenge"]))
        checkEqual(object["v"] as? Int, 1)
        checkEqual(object["op"] as? String, "bind")
        checkEqual(object["challenge"] as? String, v.challenge)
        try await session.verifyIdentityBindingResponse(v.bytes(\.binding_ciphertext))
        let state = await session.state
        let role = await session.authenticatedRole
        checkEqual(state, .readyForRequest)
        checkEqual(role, .guest)
        return session
    }
    func expectClosed(_ session: Security2Session, _ body: () async throws -> Void,
                      file: StaticString = #filePath, line: UInt = #line) async {
        do { try await body(); failCheck("Expected rejection", file: file, line: line) }
        catch { checkTrue(error is Security2Error, file: file, line: line) }
        let state = await session.state
        let role = await session.authenticatedRole
        checkEqual(state, .closed, file: file, line: line)
        checkNil(role, file: file, line: line)
    }

    func testPinnedIDFPythonInteropAndIdentityBinding() async throws {
        for vector in try vectors() {
            let session = try await bound(vector)
            let ciphertext = try await session.sealRequest(vector.bytes(\.plaintext_request))
            checkEqual(ciphertext, vector.bytes(\.request_ciphertext), vector.name)
            let plaintext = try await session.openResponse(vector.bytes(\.response_ciphertext))
            checkEqual(plaintext, vector.bytes(\.plaintext_response), vector.name)
            _ = try await session.sealRequest(vector.bytes(\.plaintext_request))
            let next = try await session.openResponse(vector.bytes(\.next_response_ciphertext))
            checkEqual(next, vector.bytes(\.plaintext_response))
            await session.close()
        }
    }

    func testNoLegacySecurityOrInvalidCredentials() throws {
        for patch in [0, -1, 2] {
            checkThrows(try Security2Session(username: "203-guest", password: Data([1]), securityPatchVersion: patch))
        }
        for name in ["", String(repeating: "a", count: 65), "203\0guest"] {
            checkThrows(try Security2Session(username: name, password: Data([1]), securityPatchVersion: 1))
        }
        for password in [Data(), Data(repeating: 1, count: 257)] {
            checkThrows(try Security2Session(username: "203-owner", password: password, securityPatchVersion: 1))
        }
    }

    func testClientPublicKeyIsFresh() async throws {
        let one = try Security2Session(username: "203-guest", password: Data("test only".utf8), securityPatchVersion: 1)
        let two = try Security2Session(username: "203-guest", password: Data("test only".utf8), securityPatchVersion: 1)
        let first = try await one.startHandshake()
        let second = try await two.startHandshake()
        checkNotEqual(first, second)
        let message = try SessionData(serializedBytes: first)
        checkEqual(message.sec2.sc0.clientPubkey.count, 384)
        await one.close(); await two.close()
    }

    func testHandshakeMalformedPacketsFailClosed() async throws {
        let v = try vectors()[0]
        var malformed = [Data(), Data([0xff]), Data(repeating: 0, count: 1025), Data(v.bytes(\.response0).dropLast())]
        malformed.append(Data([0x10, 0x02]) + v.bytes(\.response0)) // duplicate security version
        malformed.append(v.bytes(\.response0) + Data([0x98, 0x06, 1])) // unknown field
        for edit in 0..<7 {
            var response = try SessionData(serializedBytes: v.bytes(\.response0))
            switch edit {
            case 0: response.secVer = .secScheme0
            case 1: response.sec2.msg = .s2SessionResponse1
            case 2: response.sec2.sr0.status = .UNRECOGNIZED(127)
            case 3: response.sec2.sr0.devicePubkey = Data([0])
            case 4: response.sec2.sr0.devicePubkey = Data(repeating: 0xff, count: 385)
            case 5: response.sec2.sr0.deviceSalt = Data(repeating: 1, count: 15)
            default: response.sec2.sr0.devicePubkey = Data(repeating: 0xff, count: 384)
            }
            malformed.append(try response.serializedData())
        }
        for bytes in malformed {
            let session = try v.session()
            _ = try await session.startHandshake()
            await expectClosed(session) { _ = try await session.receiveChallenge(bytes) }
        }
    }

    func testServerProofNonceAndStatusFailClosed() async throws {
        let v = try vectors()[0]
        for edit in 0..<5 {
            let session = try v.session()
            _ = try await session.startHandshake()
            _ = try await session.receiveChallenge(v.bytes(\.response0))
            var response = try SessionData(serializedBytes: v.bytes(\.response1))
            switch edit {
            case 0: response.sec2.sr1.deviceProof[0] ^= 1
            case 1: response.sec2.sr1.deviceProof.removeLast()
            case 2: response.sec2.sr1.deviceNonce.removeLast()
            case 3: response.sec2.sr1.deviceNonce[11] = 0
            default: response.sec2.sr1.status = .UNRECOGNIZED(127)
            }
            let bytes = try response.serializedData()
            await expectClosed(session) { try await session.verifyServer(bytes) }
        }
    }

    func testWrongPasswordNeverEstablishesSession() async throws {
        let v = try vectors()[0]
        let session = try Security2Session(testingUsername: v.username, password: Data("wrong password".utf8), ephemeral: v.bytes(\.ephemeral))
        _ = try await session.startHandshake()
        let proof = try await session.receiveChallenge(v.bytes(\.response0))
        checkNotEqual(proof, v.bytes(\.request1))
        await expectClosed(session) { try await session.verifyServer(v.bytes(\.response1)) }
    }

    func testCommandsBlockedBeforeGatewayBinding() async throws {
        let v = try vectors()[0]
        let session = try await handshake(v)
        await expectClosed(session) { _ = try await session.sealRequest(v.bytes(\.plaintext_request)) }
    }

    func testReplayTruncationTamperingAndOversizeCloseSession() async throws {
        let v = try vectors()[0]
        var damaged = v.bytes(\.response_ciphertext)
        damaged[0] ^= 1
        for ciphertext in [v.bytes(\.binding_ciphertext), Data(), Data(repeating: 1, count: 15),
                           Data(repeating: 1, count: 1025), damaged] {
            let session = try await bound(v)
            _ = try await session.sealRequest(v.bytes(\.plaintext_request))
            await expectClosed(session) { _ = try await session.openResponse(ciphertext) }
        }
        let replay = try await bound(v)
        _ = try await replay.sealRequest(v.bytes(\.plaintext_request))
        _ = try await replay.openResponse(v.bytes(\.response_ciphertext))
        _ = try await replay.sealRequest(v.bytes(\.plaintext_request))
        await expectClosed(replay) { _ = try await replay.openResponse(v.bytes(\.response_ciphertext)) }
    }

    func testLimitsAndUnexpectedOrdering() async throws {
        let v = try vectors()[0]
        let maximum = try await bound(v)
        let ciphertext = try await maximum.sealRequest(Data(repeating: 9, count: 1008))
        checkEqual(ciphertext.count, 1024)
        await maximum.close()
        let oversized = try await bound(v)
        await expectClosed(oversized) { _ = try await oversized.sealRequest(Data(repeating: 1, count: 1009)) }
        let premature = try v.session()
        await expectClosed(premature) { try await premature.verifyServer(v.bytes(\.response1)) }
        let doubleRequest = try await bound(v)
        _ = try await doubleRequest.sealRequest(Data())
        await expectClosed(doubleRequest) { _ = try await doubleRequest.sealRequest(Data()) }
        let doubleResponse = try await bound(v)
        await expectClosed(doubleResponse) { _ = try await doubleResponse.openResponse(v.bytes(\.response_ciphertext)) }
    }

    func testCloseForTimeoutCancellationOrTransportChangeCannotResume() async throws {
        let v = try vectors()[0]
        let session = try await bound(v)
        _ = try await session.sealRequest(v.bytes(\.plaintext_request))
        await session.close()
        await expectClosed(session) { _ = try await session.openResponse(v.bytes(\.response_ciphertext)) }
        await expectClosed(session) { _ = try await session.startHandshake() }
    }

    func testConcurrentRequestsFailClosed() async throws {
        let v = try vectors()[0]
        let session = try await bound(v)
        let results = await withTaskGroup(of: Bool.self) { group in
            for _ in 0..<2 {
                group.addTask { do { _ = try await session.sealRequest(Data()); return true } catch { return false } }
            }
            var output = [Bool]()
            for await item in group { output.append(item) }
            return output
        }
        checkEqual(results.filter { $0 }.count, 1)
        let state = await session.state
        checkEqual(state, .closed)
    }

    func testCounterCannotWrapOrUseZero() throws {
        checkEqual(try advancedCounter(1), 2)
        checkEqual(try advancedCounter(UInt32.max - 1), UInt32.max)
        checkThrows(try advancedCounter(UInt32.max))
        checkThrows(try advancedCounter(0))
    }

    func testIdentityBindingCrossLanguageAndEveryBoundField() throws {
        let v = try vectors()[0]
        let pending = try PendingIdentityBinding(trusted: v.identity(), boot: v.boot, role: .guest, challenge: v.bytes(\.challenge))
        checkEqual(PendingIdentityBinding.context(gateway: v.gateway, boot: v.boot, transcript: v.transcript,
                                                      challenge: v.challenge, role: v.role), v.bytes(\.binding_context))
        try pending.verify(response: v.bytes(\.binding_response), transcript: v.bytes(\.transcript))
        var wrongDigest = v.bytes(\.transcript); wrongDigest[0] ^= 1
        checkThrows(try pending.verify(response: v.bytes(\.binding_response), transcript: wrongDigest))
        for field in ["gateway_id", "boot", "transcript", "challenge", "role", "signature", "op"] {
            var object = try JSONSerialization.jsonObject(with: v.bytes(\.binding_response)) as! [String: Any]
            let old = object[field] as! String
            object[field] = field == "role" ? "owner" : (old.hasPrefix("0") ? "1" : "0") + old.dropFirst()
            let response = try JSONSerialization.data(withJSONObject: object)
            checkThrows(try pending.verify(response: response, transcript: v.bytes(\.transcript)), field)
        }
        let wrongKey = P256.Signing.PrivateKey().publicKey.derRepresentation
        let other = try TrustedGatewayIdentity(gatewayID: v.gateway, publicKeySPKI: wrongKey)
        let wrong = try PendingIdentityBinding(trusted: other, boot: v.boot, role: .guest, challenge: v.bytes(\.challenge))
        checkThrows(try wrong.verify(response: v.bytes(\.binding_response), transcript: v.bytes(\.transcript)))
        let owner = try PendingIdentityBinding(trusted: v.identity(), boot: v.boot, role: .owner, challenge: v.bytes(\.challenge))
        checkThrows(try owner.verify(response: v.bytes(\.binding_response), transcript: v.bytes(\.transcript)))
    }

    func testBindingJSONDuplicateEscapedAliasAndNestedValuesRejected() throws {
        let v = try vectors()[0]
        let pending = try PendingIdentityBinding(trusted: v.identity(), boot: v.boot, role: .guest, challenge: v.bytes(\.challenge))
        let valid = String(decoding: v.bytes(\.binding_response), as: UTF8.self)
        for invalid in [
            valid.replacingOccurrences(of: "\"v\":1", with: "\"v\":0,\"v\":1"),
            valid.replacingOccurrences(of: "\"role\"", with: "\"ro\\u006ce\""),
            valid.replacingOccurrences(of: "\"role\"", with: "\"role\\u0000\""),
            valid.replacingOccurrences(of: "\"v\":1", with: "\"v\":true"),
            valid.replacingOccurrences(of: "\"v\":1", with: "\"v\":{\"v\":1}"),
            valid.replacingOccurrences(of: "\"v\":1", with: "\"v\":01"),
            valid + "{}"
        ] {
            checkThrows(try pending.verify(response: Data(invalid.utf8), transcript: v.bytes(\.transcript)))
        }
    }

    func testIdentityResponseFailureClosesAndRemovesRole() async throws {
        let v = try vectors()[0]
        let session = try await handshake(v)
        _ = try await session.makeIdentityBindingRequest(trustedGateway: v.identity(), expectedBoot: v.boot,
                                                          expectedRole: .owner, challenge: v.bytes(\.challenge))
        await expectClosed(session) { try await session.verifyIdentityBindingResponse(v.bytes(\.binding_ciphertext)) }
    }

    func testTrustedIdentityAndContextInputBounds() throws {
        let v = try vectors()[0]
        for name in ["", "UPPER", "a.b", "a_b", "a\nb", String(repeating: "a", count: 49)] {
            checkThrows(try TrustedGatewayIdentity(gatewayID: name, publicKeySPKI: v.bytes(\.spki)))
        }
        checkThrows(try TrustedGatewayIdentity(gatewayID: "203-gateway", publicKeySPKI: Data(repeating: 0, count: 129)))
        for boot in ["0", "0123456789abcdeF", "0123456789abcdef\n"] {
            checkThrows(try PendingIdentityBinding(trusted: v.identity(), boot: boot, role: .guest, challenge: v.bytes(\.challenge)))
        }
        checkThrows(try PendingIdentityBinding(trusted: v.identity(), boot: v.boot, role: .guest, challenge: Data(repeating: 0, count: 31)))
    }

    func testVerifierGenerationIncludingLeadingZeroDigest() throws {
        let url = Bundle.module.url(forResource: "idf61", withExtension: "json", subdirectory: "Fixtures")!
        let fixture = try JSONDecoder().decode(VerifierFixtures.self, from: Data(contentsOf: url))
        for vector in [fixture.verifier_fixture, fixture.leading_zero_verifier_fixture] {
            let generated = try SRPVerifier.make(username: vector.username, password: Data(vector.password.utf8), salt: Data(hex: vector.salt)!)
            checkEqual(generated.salt.hex, vector.salt)
            checkEqual(generated.verifier.hex, vector.verifier)
            checkEqual(generated.verifier.count, 384)
        }
        let first = try SRPVerifier.generate(username: "203-guest", password: Data("test only password".utf8))
        let second = try SRPVerifier.generate(username: "203-guest", password: Data("test only password".utf8))
        checkEqual(first.salt.count, 16)
        checkNotEqual(first.salt, second.salt)
        checkNotEqual(first.verifier, second.verifier)
        checkThrows(try SRPVerifier.generate(username: "", password: Data([1])))
        checkThrows(try SRPVerifier.generate(username: "203-guest", password: Data()))
        checkThrows(try SRPVerifier.generate(username: "203\0guest", password: Data([1])))
    }
}
