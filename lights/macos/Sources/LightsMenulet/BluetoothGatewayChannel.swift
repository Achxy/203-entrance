import Foundation
import LightsSecurity

public enum BluetoothGatewayIdentityCodec {
    private struct Descriptor: Decodable {
        let v: Int
        let `protocol`: Int
        let connection: String
        let boot: String
        let gateway_id: String
        let security: Int
        let patch: Int
    }
    public static func decode(_ bytes: Data, expectedGatewayID: String) throws -> RelayChannelIdentity {
        guard bytes.count <= 1024 else { throw RelayChannelError.payloadTooLarge }
        var scanner = FlatRelayObject(data: Array(bytes))
        let keys = try scanner.parse()
        guard keys == ["v", "protocol", "connection", "boot", "gateway_id", "security", "patch"],
              let descriptor = try? JSONDecoder().decode(Descriptor.self, from: bytes),
              descriptor.v == 1, descriptor.protocol == 1, descriptor.security == 2, descriptor.patch == 1,
              descriptor.gateway_id == expectedGatewayID,
              (1...48).contains(descriptor.gateway_id.utf8.count),
              descriptor.gateway_id.utf8.allSatisfy({ (97...122).contains($0) || (48...57).contains($0) || $0 == 45 }),
              hex(descriptor.connection, length: 32), hex(descriptor.boot, length: 16) else { throw RelayChannelError.invalidFrame }
        return RelayChannelIdentity(connection: descriptor.connection, boot: descriptor.boot)
    }
    private static func hex(_ text: String, length: Int) -> Bool {
        text.utf8.count == length && text.utf8.allSatisfy { (48...57).contains($0) || (97...102).contains($0) }
    }
}

/// Bridges the negotiated GATT peer into the same Security2 pipe as Internet.
/// This descriptor checks routing metadata only. The pinned signature after SRP
/// is still mandatory before any credential grant or light operation.
@MainActor public final class BluetoothGatewayChannel: GatewayByteChannel {
    private let peripheralID: UUID
    private let expectedGatewayID: String
    private let channel: BluetoothByteChannel
    public init(peripheralID: UUID, trustedIdentity: TrustedGatewayIdentity) {
        self.peripheralID = peripheralID
        expectedGatewayID = trustedIdentity.gatewayID
        channel = BluetoothByteChannel()
    }
    public func open() async throws -> RelayChannelIdentity {
        do {
            try await channel.open(peripheralID: peripheralID)
            let bytes = try await channel.exchange(.identity, payload: Data(#"{"v":1}"#.utf8))
            return try BluetoothGatewayIdentityCodec.decode(bytes, expectedGatewayID: expectedGatewayID)
        } catch { channel.close(); throw error }
    }
    public func exchange(_ operation: RelayOperation, payload: Data, roleHint: AccessRole?) async throws -> Data {
        let operation: BluetoothOperation = switch operation {
        case .identity: .identity
        case .session: .session
        case .data: .data
        case .close: .close
        }
        // BLE has no untrusted role hint field. The SRP username/proof selects
        // the server's credential profile, followed by signed role binding.
        return try await channel.exchange(operation, payload: payload)
    }
    public func close() async { channel.close() }
}
