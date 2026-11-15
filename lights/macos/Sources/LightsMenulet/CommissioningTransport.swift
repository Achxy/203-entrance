import Foundation

@MainActor public protocol GatewayCommissioningTransport: MenuletTransport {
    func startDiscovery() async throws
    func cancelDiscovery() async throws
    func candidate(at index: Int) async throws -> GatewayDiscoveryPage
    func adoptCandidate(_ index: Int, name: String, allowLegacyControl: Bool) async throws -> RoomSnapshot
}

extension GatewayAdapter: GatewayCommissioningTransport {}
