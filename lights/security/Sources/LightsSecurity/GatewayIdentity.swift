import Foundation
import CryptoKit
import Security

public enum CredentialRole: String, Codable, Sendable { case owner, guest }

/// Must come from an owner-verified invitation or existing trusted Keychain item.
/// Discovery, a relay response, and a BLE advertisement are not trust sources.
public struct TrustedGatewayIdentity: Sendable {
    public let gatewayID: String
    public let publicKeySPKI: Data
    let key: P256.Signing.PublicKey

    public init(gatewayID: String, publicKeySPKI: Data) throws {
        guard (1...48).contains(gatewayID.utf8.count), gatewayID.utf8.allSatisfy({
            (48...57).contains($0) || (97...122).contains($0) || $0 == 45
        }), publicKeySPKI.count <= 128 else { throw Security2Error.invalidMessage }
        do { key = try P256.Signing.PublicKey(derRepresentation: publicKeySPKI) }
        catch { throw Security2Error.invalidMessage }
        self.gatewayID = gatewayID
        self.publicKeySPKI = publicKeySPKI
    }
}

package struct PendingIdentityBinding {
    let trusted: TrustedGatewayIdentity
    let boot: String
    let role: CredentialRole
    let challenge: Data

    package init(trusted: TrustedGatewayIdentity, boot: String, role: CredentialRole, challenge: Data) throws {
        guard isLowercaseHex(boot, bytes: 8), challenge.count == 32 else {
            throw Security2Error.invalidMessage
        }
        self.trusted = trusted
        self.boot = boot
        self.role = role
        self.challenge = challenge
    }

    package func request() throws -> Data {
        try JSONEncoder().encode(Request(v: 1, op: "bind", challenge: challenge.hex))
    }

    package func verify(response: Data, transcript: Data) throws {
        guard transcript.count == 32, response.count <= Security2Session.maximumPlaintextBytes else {
            throw Security2Error.invalidMessage
        }
        try StrictBindingJSON.validate(response)
        let reply = try JSONDecoder().decode(Reply.self, from: response)
        guard reply.v == 1, reply.op == "bound", reply.gateway_id == trusted.gatewayID,
              reply.boot == boot, reply.transcript == transcript.hex,
              reply.challenge == challenge.hex, reply.role == role.rawValue,
              (16...144).contains(reply.signature.count),
              reply.signature.count.isMultiple(of: 2),
              isLowercaseHex(reply.signature, bytes: reply.signature.count / 2),
              let signatureBytes = Data(hex: reply.signature) else {
            throw Security2Error.authenticationFailed
        }
        let signature = try P256.Signing.ECDSASignature(derRepresentation: signatureBytes)
        let context = Self.context(gateway: reply.gateway_id, boot: reply.boot,
                                   transcript: reply.transcript, challenge: reply.challenge, role: reply.role)
        guard trusted.key.isValidSignature(signature, for: context) else {
            throw Security2Error.authenticationFailed
        }
    }

    /// This is a new application extension, not part of Espressif Security2.
    /// The ESP must compute the transcript from its own four handshake records.
    package static func context(gateway: String, boot: String, transcript: String, challenge: String, role: String) -> Data {
        Data("203-lights-channel-v1\n\(gateway)\n\(boot)\n\(transcript)\n\(challenge)\n\(role)\n".utf8)
    }

    private struct Request: Encodable { let v: Int; let op: String; let challenge: String }
    private struct Reply: Decodable {
        let v: Int
        let op: String
        let gateway_id: String
        let boot: String
        let transcript: String
        let challenge: String
        let role: String
        let signature: String

        init(from decoder: Decoder) throws {
            let values = try decoder.container(keyedBy: Field.self)
            guard Set(values.allKeys.map(\.stringValue)) == Set([
                "v", "op", "gateway_id", "boot", "transcript", "challenge", "role", "signature"
            ]) else { throw Security2Error.invalidMessage }
            v = try values.decode(Int.self, forKey: Field("v"))
            op = try values.decode(String.self, forKey: Field("op"))
            gateway_id = try values.decode(String.self, forKey: Field("gateway_id"))
            boot = try values.decode(String.self, forKey: Field("boot"))
            transcript = try values.decode(String.self, forKey: Field("transcript"))
            challenge = try values.decode(String.self, forKey: Field("challenge"))
            role = try values.decode(String.self, forKey: Field("role"))
            signature = try values.decode(String.self, forKey: Field("signature"))
        }
    }
    private struct Field: CodingKey {
        let stringValue: String
        var intValue: Int? { nil }
        init(_ value: String) { stringValue = value }
        init?(stringValue: String) { self.stringValue = stringValue }
        init?(intValue: Int) { return nil }
    }
}

/// The binding wire profile is a flat object with eight unique, unescaped ASCII
/// keys and string/integer values. Reject aliases and duplicate keys before
/// JSONDecoder, which otherwise silently accepts the last duplicate occurrence.
enum StrictBindingJSON {
    static func validate(_ data: Data) throws {
        guard data.count <= 1008 else { throw Security2Error.messageTooLarge }
        let bytes = Array(data)
        var index = 0
        var keys = Set<String>()
        func whitespace() {
            while index < bytes.count && [9, 10, 13, 32].contains(bytes[index]) { index += 1 }
        }
        func consume(_ expected: UInt8) throws {
            whitespace()
            guard index < bytes.count, bytes[index] == expected else { throw Security2Error.invalidMessage }
            index += 1
        }
        func string() throws -> String {
            try consume(34)
            let start = index
            while index < bytes.count && bytes[index] != 34 {
                guard (32...126).contains(bytes[index]), bytes[index] != 92 else { throw Security2Error.invalidMessage }
                index += 1
            }
            guard index < bytes.count else { throw Security2Error.invalidMessage }
            let value = String(decoding: bytes[start..<index], as: UTF8.self)
            index += 1
            return value
        }
        try consume(123)
        while true {
            let key = try string()
            guard keys.count < 8, keys.insert(key).inserted else { throw Security2Error.invalidMessage }
            try consume(58)
            whitespace()
            guard index < bytes.count else { throw Security2Error.invalidMessage }
            if bytes[index] == 34 { _ = try string() }
            else {
                let start = index
                while index < bytes.count && (48...57).contains(bytes[index]) { index += 1 }
                guard index > start, index - start <= 10,
                      index - start == 1 || bytes[start] != 48 else { throw Security2Error.invalidMessage }
            }
            whitespace()
            guard index < bytes.count else { throw Security2Error.invalidMessage }
            if bytes[index] == 125 { index += 1; break }
            try consume(44)
        }
        whitespace()
        guard index == bytes.count, keys.count == 8 else { throw Security2Error.invalidMessage }
    }
}

func randomChallenge() throws -> Data {
    var bytes = Data(count: 32)
    let result = bytes.withUnsafeMutableBytes { SecRandomCopyBytes(kSecRandomDefault, $0.count, $0.baseAddress!) }
    guard result == errSecSuccess else { throw Security2Error.cryptographicFailure }
    return bytes
}

func isLowercaseHex(_ text: String, bytes: Int) -> Bool {
    text.utf8.count == bytes * 2 && text.utf8.allSatisfy { (48...57).contains($0) || (97...102).contains($0) }
}

extension Data {
    package var hex: String { map { String(format: "%02x", $0) }.joined() }
    package init?(hex: String) {
        guard hex.count.isMultiple(of: 2) else { return nil }
        var bytes = [UInt8]()
        bytes.reserveCapacity(hex.count / 2)
        var index = hex.startIndex
        while index != hex.endIndex {
            let next = hex.index(index, offsetBy: 2)
            guard let value = UInt8(hex[index..<next], radix: 16) else { return nil }
            bytes.append(value)
            index = next
        }
        self.init(bytes)
    }
}
