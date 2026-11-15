import Foundation
import LightsSecurity

/// Public identity material only, copied from the owner's checked-in relay
/// configuration. No password, private key, WiZ home key, or grant is bundled.
public enum Room203Defaults {
    public static let gatewayID = "entrance-c7f088"
    public static let relayEndpoint = URL(string: "wss://203-arcade-relay.achyuthanugraha.workers.dev/v1/lights")!
    public static func trustedIdentity() throws -> TrustedGatewayIdentity {
        let encoded = "MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAE/Z4kg40nfjYTBW54Y/pivy09FHfiruvh62ZVPm9rGcZ0LubXXEtD4Sl22Sd7vAE3n3t1mS1rjIOU+zr/cnTFuw=="
        guard let publicKey = Data(base64Encoded: encoded) else { throw RelayChannelError.invalidFrame }
        return try TrustedGatewayIdentity(gatewayID: gatewayID, publicKeySPKI: publicKey)
    }
}
