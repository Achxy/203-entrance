import CryptoKit
import Foundation
import LocalAuthentication
import Security

public enum StoredSecretKind: String, Sendable { case ownerKey, guestPassword, guestGrant, gatewayIdentity }

public struct SecretRecordID: Sendable {
    let account: String
    /// Use the gateway identity verified through the owner handoff, not an SSID,
    /// display name, advertisement, or untrusted relay response.
    public init(trustedGatewayIdentity: Data, kind: StoredSecretKind) throws {
        guard (16...4096).contains(trustedGatewayIdentity.count) else { throw CredentialStoreError.invalidValue }
        let digest = SHA256.hash(data: trustedGatewayIdentity).map { String(format: "%02x", $0) }.joined()
        account = "v1/\(digest)/\(kind.rawValue)"
    }
}

public enum CredentialStoreError: LocalizedError, Equatable {
    case invalidValue, status(OSStatus)
    public var errorDescription: String? {
        switch self {
        case .invalidValue: "The credential has an invalid size or identity."
        case .status(let status): "Keychain could not complete this request (\(status)). Unlock your Mac and try again."
        }
    }
}

/// Nothing is saved at app startup or in preview mode. A secure adapter calls
/// this only after gateway authentication and explicit credential retention.
public struct KeychainSecretStore: Sendable {
    private let service: String
    public init(service: String = "dev.achyuth.203lights.credentials") { self.service = service }

    public func save(_ value: Data, for record: SecretRecordID) throws {
        guard (1...4096).contains(value.count) else { throw CredentialStoreError.invalidValue }
        let base = query(for: record)
        let attributes: [String: Any] = [kSecValueData as String: value,
                                         kSecAttrAccessible as String: kSecAttrAccessibleWhenUnlockedThisDeviceOnly]
        var status = SecItemUpdate(base as CFDictionary, attributes as CFDictionary)
        if status == errSecItemNotFound {
            status = SecItemAdd(base.merging(attributes) { _, new in new } as CFDictionary, nil)
        }
        guard status == errSecSuccess else { throw CredentialStoreError.status(status) }
    }

    public func read(_ record: SecretRecordID) throws -> Data? {
        var query = query(for: record)
        query[kSecReturnData as String] = true
        query[kSecMatchLimit as String] = kSecMatchLimitOne
        let authentication = LAContext()
        authentication.interactionNotAllowed = true
        query[kSecUseAuthenticationContext as String] = authentication
        var result: CFTypeRef?
        let status = SecItemCopyMatching(query as CFDictionary, &result)
        if status == errSecItemNotFound { return nil }
        guard status == errSecSuccess else { throw CredentialStoreError.status(status) }
        guard let data = result as? Data, (1...4096).contains(data.count) else { throw CredentialStoreError.invalidValue }
        return data
    }

    public func remove(_ record: SecretRecordID) throws {
        let status = SecItemDelete(query(for: record) as CFDictionary)
        guard status == errSecSuccess || status == errSecItemNotFound else { throw CredentialStoreError.status(status) }
    }

    private func query(for record: SecretRecordID) -> [String: Any] {
        [kSecClass as String: kSecClassGenericPassword, kSecAttrService as String: service,
         kSecAttrAccount as String: record.account, kSecAttrSynchronizable as String: false,
         kSecUseDataProtectionKeychain as String: true]
    }
}
