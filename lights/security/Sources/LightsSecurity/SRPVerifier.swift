import Foundation
import EspressifSRP
import Security

public struct SRPVerifierProfile: Sendable {
    public let salt: Data
    public let verifier: Data
}

/// Generate only for an explicit authenticated owner credential change. The
/// verifier is password-equivalent server material and must never be logged.
public enum SRPVerifier {
    public static func generate(username: String, password: Data) throws -> SRPVerifierProfile {
        var salt = Data(count: 16)
        let result = salt.withUnsafeMutableBytes { SecRandomCopyBytes(kSecRandomDefault, $0.count, $0.baseAddress!) }
        guard result == errSecSuccess else { throw Security2Error.cryptographicFailure }
        return try make(username: username, password: password, salt: salt)
    }

    package static func make(username: String, password: Data, salt: Data) throws -> SRPVerifierProfile {
        let name = Data(username.utf8)
        guard !name.contains(0) else { throw Security2Error.invalidCredentials }
        do {
            return try SRPVerifierProfile(salt: salt, verifier: SRPVerifierMaterial.make(username: name, password: password, salt: salt))
        } catch { throw Security2Error.invalidCredentials }
    }
}
