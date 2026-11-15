import CryptoKit
import Foundation
import LightsSecurity

public protocol GatewayReadinessChecking: Sendable {
    func isOnline(endpoint: URL, trustedIdentity: TrustedGatewayIdentity) async throws -> Bool
}

private final class ReadinessRedirectPolicy: NSObject, URLSessionTaskDelegate, @unchecked Sendable {
    func urlSession(_ session: URLSession, task: URLSessionTask, willPerformHTTPRedirection response: HTTPURLResponse,
                    newRequest request: URLRequest, completionHandler: @escaping @Sendable (URLRequest?) -> Void) { completionHandler(nil) }
}

/// This public descriptor is an availability hint, never an authentication
/// grant. It is fetched only when an enabled adapter requests an Internet route.
public struct HTTPSGatewayReadiness: GatewayReadinessChecking {
    private struct Descriptor: Decodable {
        let `protocol`: Int
        let online: Bool
        let gateway_id: String
        let public_key: String
    }
    public init() {}
    public func isOnline(endpoint: URL, trustedIdentity: TrustedGatewayIdentity) async throws -> Bool {
        _ = try RelayWebSocketChannel(endpoint: endpoint)
        guard var address = URLComponents(url: endpoint, resolvingAgainstBaseURL: false) else { throw RelayChannelError.invalidEndpoint }
        address.scheme = "https"; address.path = "/v1/lights/info"
        guard let url = address.url else { throw RelayChannelError.invalidEndpoint }
        let configuration = URLSessionConfiguration.ephemeral
        configuration.urlCache = nil; configuration.httpCookieStorage = nil; configuration.urlCredentialStorage = nil
        configuration.timeoutIntervalForRequest = 8; configuration.timeoutIntervalForResource = 8
        configuration.waitsForConnectivity = false
        let session = URLSession(configuration: configuration, delegate: ReadinessRedirectPolicy(), delegateQueue: nil)
        defer { session.invalidateAndCancel() }
        let (stream, response) = try await session.bytes(for: URLRequest(url: url, cachePolicy: .reloadIgnoringLocalCacheData))
        guard let http = response as? HTTPURLResponse, http.statusCode == 200 else { return false }
        guard response.expectedContentLength <= 4096 else { throw RelayChannelError.payloadTooLarge }
        var data = Data()
        for try await byte in stream {
            guard data.count < 4096 else { throw RelayChannelError.payloadTooLarge }
            data.append(byte)
        }
        var scanner = FlatRelayObject(data: Array(data))
        guard try scanner.parse() == ["protocol", "online", "gateway_id", "public_key"],
              let descriptor = try? JSONDecoder().decode(Descriptor.self, from: data), descriptor.protocol == 1,
              descriptor.gateway_id == trustedIdentity.gatewayID,
              let key = try? P256.Signing.PublicKey(pemRepresentation: descriptor.public_key),
              key.derRepresentation == trustedIdentity.publicKeySPKI else { throw RelayChannelError.invalidFrame }
        return descriptor.online
    }
}

/// No sockets or radios are opened by construction. Every returned route is a
/// fresh byte channel; the adapter must complete SRP, device binding and grant
/// resumption before exposing it as usable. Readiness cannot bypass those steps.
@MainActor public final class GatewayRouteCoordinator {
    private let trusted: TrustedGatewayIdentity
    private let readiness: any GatewayReadinessChecking
    private var selection = TransportSelectionPolicy()
    private var nearbyPeripheralID: UUID?
    private var readinessCache: (endpoint: URL, observed: TimeInterval, online: Bool)?

    public init(trustedIdentity: TrustedGatewayIdentity, nearbyPeripheralID: UUID? = nil,
                readiness: any GatewayReadinessChecking = HTTPSGatewayReadiness()) {
        trusted = trustedIdentity; self.nearbyPeripheralID = nearbyPeripheralID; self.readiness = readiness
    }
    public func selectNearbyPeripheral(_ value: UUID?) { nearbyPeripheralID = value }

    public func makeRoute(_ request: AccessRequest, excluding: Set<GatewayRoute>) async throws -> GatewayAdapterRoute {
        if let chosen = request.nearbyPeripheralID { nearbyPeripheralID = chosen }
        let now = ProcessInfo.processInfo.systemUptime
        let suggested = selection.choose(preference: request.preference, at: now).route
        var order: [GatewayRoute]
        switch request.preference {
        case .internet: order = [.internet]
        case .bluetooth: order = [.bluetooth]
        case .automatic:
            if let suggested { order = [suggested] + GatewayRoute.allCases.filter { $0 != suggested } }
            else { order = [.internet, .bluetooth] }
        }
        for candidate in order where !excluding.contains(candidate) {
            switch candidate {
            case .bluetooth:
                if let nearbyPeripheralID {
                    return GatewayAdapterRoute(channel: BluetoothGatewayChannel(peripheralID: nearbyPeripheralID, trustedIdentity: trusted), route: .bluetooth)
                }
            case .internet:
                guard let endpoint = URL(string: request.endpoint) else { throw RelayChannelError.invalidEndpoint }
                do {
                    let online: Bool
                    if let cached = readinessCache, cached.endpoint == endpoint, now >= cached.observed, now - cached.observed < 15 {
                        online = cached.online
                    } else {
                        online = try await readiness.isOnline(endpoint: endpoint, trustedIdentity: trusted)
                        readinessCache = (endpoint, ProcessInfo.processInfo.systemUptime, online)
                    }
                    if online { return GatewayAdapterRoute(channel: try RelayWebSocketChannel(endpoint: endpoint), route: .internet) }
                } catch is CancellationError { throw CancellationError() }
                catch {
                    readinessCache = nil
                    selection.observe(.internet, succeeded: false, roundTripMilliseconds: nil, at: ProcessInfo.processInfo.systemUptime)
                    if request.preference == .internet { throw error }
                }
            }
        }
        throw GatewayAdapterError.unavailable
    }

    public func recordResult(_ route: GatewayRoute, succeeded: Bool, roundTripMilliseconds: Double?) {
        let now = ProcessInfo.processInfo.systemUptime
        selection.observe(route, succeeded: succeeded, roundTripMilliseconds: roundTripMilliseconds, at: now)
        if succeeded {
            // The adapter has actually authenticated this route. Record it as
            // current so the next choice observes dwell instead of oscillating.
            _ = selection.choose(preference: route == .internet ? .internet : .bluetooth, at: now)
        } else if route == .internet { readinessCache = nil }
    }
}
