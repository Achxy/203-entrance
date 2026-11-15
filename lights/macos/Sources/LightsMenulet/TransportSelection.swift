import Foundation

public enum GatewayRoute: String, Hashable, Sendable, CaseIterable { case internet, bluetooth }
public struct RouteDecision: Equatable, Sendable {
    public let route: GatewayRoute?
    public let requiresFreshAuthentication: Bool
}

/// Pure, bounded selection policy. Call only with actual observations of the
/// intended gateway. It performs no probes, radio scans, connection, or retry.
public struct TransportSelectionPolicy: Sendable {
    private struct Health: Sendable {
        var latency: Double = .infinity
        var lastSuccess = -Double.infinity
        var observedAt = -Double.infinity
        var failures = 0
        var successes = 0
    }
    private var paths: [GatewayRoute: Health] = [:]
    public private(set) var selected: GatewayRoute?
    private var selectedAt = -Double.infinity
    public init() {}

    /// Times are monotonic uptime seconds, not calendar time. Invalid or old
    /// samples are ignored. No preference or advertisement establishes trust.
    public mutating func observe(_ route: GatewayRoute, succeeded: Bool, roundTripMilliseconds: Double?, at now: TimeInterval) {
        guard now.isFinite, now >= 0 else { return }
        var state = paths[route] ?? Health()
        guard now >= state.observedAt else { return }
        if succeeded {
            guard let latency = roundTripMilliseconds, latency.isFinite, (0...60_000).contains(latency) else { return }
            state.latency = state.latency.isFinite ? state.latency * 0.75 + latency * 0.25 : latency
            state.lastSuccess = now; state.failures = 0; state.successes = min(3, state.successes + 1)
        } else { state.failures = min(2, state.failures + 1); state.successes = 0 }
        state.observedAt = now
        paths[route] = state
    }

    public mutating func choose(preference: ConnectionPreference = .automatic, at now: TimeInterval) -> RouteDecision {
        func usable(_ route: GatewayRoute) -> Bool {
            guard let state = paths[route], now.isFinite, now >= state.lastSuccess else { return false }
            return now - state.lastSuccess <= 30 && state.failures < 2
        }
        let previous = selected
        let available = GatewayRoute.allCases.filter(usable)
        if preference != .automatic {
            let requested: GatewayRoute = preference == .internet ? .internet : .bluetooth
            selected = usable(requested) ? requested : nil
        } else if let current = selected, usable(current), let currentHealth = paths[current] {
            if now - selectedAt >= 30,
               let alternative = available.filter({ $0 != current }).first,
               let alternateHealth = paths[alternative], alternateHealth.successes >= 3,
               alternateHealth.latency + max(50, currentHealth.latency * 0.25) <= currentHealth.latency {
                selected = alternative
            }
        } else {
            selected = available.min { (paths[$0]?.latency ?? .infinity) < (paths[$1]?.latency ?? .infinity) }
        }
        if selected != previous { selectedAt = now }
        return RouteDecision(route: selected, requiresFreshAuthentication: selected != previous)
    }
    public mutating func reset() { paths = [:]; selected = nil; selectedAt = -Double.infinity }
}
