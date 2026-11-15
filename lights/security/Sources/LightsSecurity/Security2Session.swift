import Foundation
import CryptoKit
import EspressifSRP
import SwiftProtobuf
import Darwin

/// Errors contain no credentials, peer payloads, proofs, or key material.
public enum Security2Error: Error, Equatable, Sendable {
    case unsupportedSecurityPatch
    case invalidCredentials
    case invalidState
    case messageTooLarge
    case invalidMessage
    case authenticationFailed
    case nonceExhausted
    case cryptographicFailure
}

/// A transport-independent ESP-IDF 6.1 Security2 patch-1 client.
///
/// This actor authenticates possession of the SRP credential. A shared guest
/// password does NOT establish a unique gateway identity; see IDENTITY.md.
/// Each instance supports exactly one ordered request/response exchange at a
/// time. Close it on timeout, cancellation, disconnect, or transport change.
public actor Security2Session {
    public enum State: String, Sendable {
        case readyForHandshake, awaitingChallenge, awaitingServerProof
        case readyForBinding, awaitingBindingResponse, readyForRequest, awaitingResponse, closed
    }

    public nonisolated static let maximumHandshakeBytes = 1024
    public nonisolated static let maximumCiphertextBytes = 1024
    public nonisolated static let maximumPlaintextBytes = 1008
    public nonisolated static let supportedPatchVersion = 1

    public private(set) var state: State = .readyForHandshake
    public private(set) var authenticatedRole: CredentialRole?
    private var username: Data
    private var password: Data
    private var testingEphemeral: Data?
    private var srp: SRPClient?
    private var key: SymmetricKey?
    private var noncePrefix = Data()
    private var nextCounter: UInt32 = 1
    private var transcript = SHA256()
    private var transcriptHash: Data?
    private var binding: PendingIdentityBinding?

    /// Password bytes are copied only for the handshake and then released.
    /// The caller owns Keychain storage and must avoid logging this argument.
    public init(username: String, password: Data, securityPatchVersion: Int) throws {
        guard securityPatchVersion == Self.supportedPatchVersion else {
            throw Security2Error.unsupportedSecurityPatch
        }
        let name = Data(username.utf8)
        guard (1...64).contains(name.count), !name.contains(0),
              (1...256).contains(password.count) else {
            throw Security2Error.invalidCredentials
        }
        self.username = name
        self.password = password
    }

    // Available only to tests inside this package, never to app consumers.
    package init(testingUsername: String, password: Data, ephemeral: Data) throws {
        guard (1...64).contains(testingUsername.utf8.count),
              (1...256).contains(password.count), ephemeral.count == 32 else {
            throw Security2Error.invalidCredentials
        }
        username = Data(testingUsername.utf8)
        self.password = password
        testingEphemeral = ephemeral
    }

    deinit {
        srp?.clear()
        wipe(&password)
        if testingEphemeral != nil { wipe(&testingEphemeral!) }
    }

    public func startHandshake() throws -> Data {
        try failClosed {
            guard state == .readyForHandshake else { throw Security2Error.invalidState }
            defer {
                wipe(&password)
                if testingEphemeral != nil { wipe(&testingEphemeral!) }
                testingEphemeral = nil
            }
            if let testingEphemeral {
                srp = try SRPClient(username: username, password: password, ephemeral: testingEphemeral)
            } else {
                srp = try SRPClient(username: username, password: password)
            }
            var message = SessionData()
            message.secVer = .secScheme2
            message.sec2.msg = .s2SessionCommand0
            message.sec2.sc0.clientUsername = username
            message.sec2.sc0.clientPubkey = srp!.publicKey
            let bytes = try message.serializedData()
            appendTranscript(bytes)
            state = .awaitingChallenge
            return bytes
        }
    }

    /// Consumes response 0 and returns command 1 (client proof).
    public func receiveChallenge(_ bytes: Data) throws -> Data {
        try failClosed {
            guard state == .awaitingChallenge, let srp else { throw Security2Error.invalidState }
            let message = try decode(bytes)
            guard case .sec2(let payload)? = message.proto,
                  payload.msg == .s2SessionResponse0,
                  case .sr0(let response)? = payload.payload,
                  response.status == .success,
                  response.unknownFields.data.isEmpty,
                  (16...64).contains(response.deviceSalt.count),
                  (1...384).contains(response.devicePubkey.count) else {
                throw Security2Error.invalidMessage
            }
            let proof = try srp.processChallenge(salt: response.deviceSalt, publicKey: response.devicePubkey)
            appendTranscript(bytes)
            var request = SessionData()
            request.secVer = .secScheme2
            request.sec2.msg = .s2SessionCommand1
            request.sec2.sc1.clientProof = proof
            let result = try request.serializedData()
            appendTranscript(result)
            state = .awaitingServerProof
            return result
        }
    }

    /// Completes mutual SRP authentication. Failure permanently closes this object.
    public func verifyServer(_ bytes: Data) throws {
        try failClosed {
            guard state == .awaitingServerProof, let srp else { throw Security2Error.invalidState }
            let message = try decode(bytes)
            guard case .sec2(let payload)? = message.proto,
                  payload.msg == .s2SessionResponse1,
                  case .sr1(let response)? = payload.payload,
                  response.status == .success,
                  response.unknownFields.data.isEmpty,
                  response.deviceProof.count == 64,
                  response.deviceNonce.count == 12,
                  response.deviceNonce.suffix(4) == Data([0, 0, 0, 1]) else {
                throw Security2Error.invalidMessage
            }
            key = try srp.verifyServer(proof: response.deviceProof)
            noncePrefix = Data(response.deviceNonce.prefix(8))
            nextCounter = 1
            appendTranscript(bytes)
            transcriptHash = Data(transcript.finalize())
            self.srp = nil
            wipe(&username)
            state = .readyForBinding
        }
    }

    /// The first encrypted exchange binds this SRP session to the independently
    /// trusted gateway key. Application commands remain blocked until it succeeds.
    public func makeIdentityBindingRequest(trustedGateway: TrustedGatewayIdentity,
                                          expectedBoot: String,
                                          expectedRole: CredentialRole) throws -> Data {
        try failClosed {
            try makeIdentityBindingRequest(trustedGateway: trustedGateway, expectedBoot: expectedBoot,
                                           expectedRole: expectedRole, challenge: randomChallenge())
        }
    }

    package func makeIdentityBindingRequest(trustedGateway: TrustedGatewayIdentity,
                                    expectedBoot: String, expectedRole: CredentialRole,
                                    challenge: Data) throws -> Data {
        try failClosed {
            guard state == .readyForBinding, let key else { throw Security2Error.invalidState }
            let pending = try PendingIdentityBinding(trusted: trustedGateway, boot: expectedBoot,
                                                     role: expectedRole, challenge: challenge)
            let plaintext = try pending.request()
            let result = try AES.GCM.seal(plaintext, using: key, nonce: nonce())
            binding = pending
            nextCounter = try advancedCounter(nextCounter)
            state = .awaitingBindingResponse
            return result.ciphertext + result.tag
        }
    }

    public func verifyIdentityBindingResponse(_ ciphertext: Data) throws {
        try failClosed {
            guard state == .awaitingBindingResponse, let key, let binding, let transcriptHash else {
                throw Security2Error.invalidState
            }
            guard ciphertext.count <= Self.maximumCiphertextBytes else { throw Security2Error.messageTooLarge }
            guard ciphertext.count >= 16 else { throw Security2Error.invalidMessage }
            let box = try AES.GCM.SealedBox(nonce: nonce(), ciphertext: ciphertext.dropLast(16), tag: ciphertext.suffix(16))
            let plaintext = try AES.GCM.open(box, using: key)
            try binding.verify(response: plaintext, transcript: transcriptHash)
            nextCounter = try advancedCounter(nextCounter)
            authenticatedRole = binding.role
            self.binding = nil
            state = .readyForRequest
        }
    }

    /// Ciphertext is raw encrypted bytes followed by the 16-byte tag, without IV.
    /// Once returned, this record is consumed even if its transport write fails.
    public func sealRequest(_ plaintext: Data) throws -> Data {
        try failClosed {
            guard state == .readyForRequest, let key else { throw Security2Error.invalidState }
            guard plaintext.count <= Self.maximumPlaintextBytes else { throw Security2Error.messageTooLarge }
            // Reserve a nonce for the response. Never wrap through zero.
            guard nextCounter < UInt32.max else { throw Security2Error.nonceExhausted }
            let sealed = try AES.GCM.seal(plaintext, using: key, nonce: nonce())
            nextCounter = try advancedCounter(nextCounter)
            state = .awaitingResponse
            return sealed.ciphertext + sealed.tag
        }
    }

    public func openResponse(_ ciphertext: Data) throws -> Data {
        try failClosed {
            guard state == .awaitingResponse, let key else { throw Security2Error.invalidState }
            guard ciphertext.count <= Self.maximumCiphertextBytes else { throw Security2Error.messageTooLarge }
            guard ciphertext.count >= 16 else { throw Security2Error.invalidMessage }
            guard nextCounter < UInt32.max else { throw Security2Error.nonceExhausted }
            let box = try AES.GCM.SealedBox(nonce: nonce(), ciphertext: ciphertext.dropLast(16), tag: ciphertext.suffix(16))
            let plaintext = try AES.GCM.open(box, using: key)
            nextCounter = try advancedCounter(nextCounter)
            state = .readyForRequest
            return plaintext
        }
    }

    /// Diagnostic transcript material, not independent device authentication.
    /// Never sign a client-supplied digest as if it were a device-owned transcript.
    public func authenticatedTranscriptSHA256() throws -> Data {
        guard [.readyForBinding, .awaitingBindingResponse, .readyForRequest, .awaitingResponse].contains(state),
              let transcriptHash else {
            throw Security2Error.invalidState
        }
        return transcriptHash
    }

    public func close() { clear() }

    private func nonce() throws -> AES.GCM.Nonce {
        guard noncePrefix.count == 8, nextCounter > 0 else { throw Security2Error.invalidState }
        var counter = nextCounter.bigEndian
        let bytes = withUnsafeBytes(of: &counter) { Data($0) }
        return try AES.GCM.Nonce(data: noncePrefix + bytes)
    }

    private func decode(_ bytes: Data) throws -> SessionData {
        guard bytes.count <= Self.maximumHandshakeBytes else { throw Security2Error.messageTooLarge }
        guard !bytes.isEmpty else { throw Security2Error.invalidMessage }
        var options = BinaryDecodingOptions()
        options.messageDepthLimit = 4
        let message = try SessionData(serializedBytes: bytes, options: options)
        guard message.secVer == .secScheme2,
              message.unknownFields.data.isEmpty,
              case .sec2(let payload)? = message.proto,
              payload.unknownFields.data.isEmpty,
              try message.serializedData() == bytes else {
            throw Security2Error.invalidMessage
        }
        return message
    }

    private func appendTranscript(_ bytes: Data) {
        var length = UInt32(bytes.count).bigEndian
        withUnsafeBytes(of: &length) { transcript.update(bufferPointer: $0) }
        transcript.update(data: bytes)
    }

    private func failClosed<T>(_ operation: () throws -> T) throws -> T {
        do { return try operation() }
        catch let error as Security2Error { clear(); throw error }
        catch is SRPError { clear(); throw Security2Error.authenticationFailed }
        catch is CryptoKitError { clear(); throw Security2Error.cryptographicFailure }
        catch { clear(); throw Security2Error.invalidMessage }
    }

    private func clear() {
        srp?.clear()
        srp = nil
        key = nil
        wipe(&password)
        wipe(&username)
        if testingEphemeral != nil { wipe(&testingEphemeral!) }
        testingEphemeral = nil
        noncePrefix.removeAll(keepingCapacity: false)
        transcriptHash = nil
        binding = nil
        authenticatedRole = nil
        nextCounter = 0
        state = .closed
    }
}

package func advancedCounter(_ counter: UInt32) throws -> UInt32 {
    guard counter > 0, counter < UInt32.max else { throw Security2Error.nonceExhausted }
    return counter + 1
}

private func wipe(_ bytes: inout Data) {
    bytes.withUnsafeMutableBytes { buffer in
        if let address = buffer.baseAddress { _ = memset_s(address, buffer.count, 0, buffer.count) }
    }
    bytes.removeAll(keepingCapacity: false)
}
