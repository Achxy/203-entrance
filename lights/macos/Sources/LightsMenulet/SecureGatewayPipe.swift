import Foundation
import LightsSecurity

/// Transport metadata remains untrusted until it is covered by the signed
/// Security2 transcript binding. An adapter for BLE supplies the same seam.
public protocol GatewayByteChannel: Sendable {
    func open() async throws -> RelayChannelIdentity
    func exchange(_ operation: RelayOperation, payload: Data, roleHint: AccessRole?) async throws -> Data
    func close() async
}

extension RelayWebSocketChannel: GatewayByteChannel {}

public enum SecureGatewayPipeError: LocalizedError {
    case invalidState, changedSession
    public var errorDescription: String? {
        switch self {
        case .invalidState: "The secure gateway connection is not ready for this request."
        case .changedSession: "The gateway connection changed. Authenticate again before continuing."
        }
    }
}

/// The application adapter receives plaintext only after both SRP and the
/// uniquely pinned gateway's signature are verified. This actor does not decode
/// grants or grant a UI role; the application layer must still resume/enroll.
public actor SecureGatewayPipe {
    public enum State: Sendable { case closed, authenticating, ready, exchanging, closing }
    public private(set) var state: State = .closed
    public private(set) var lastRoundTripMilliseconds: Double?
    private let channel: any GatewayByteChannel
    private let trustedIdentity: TrustedGatewayIdentity
    private var session: Security2Session?
    private var epoch: UInt64 = 0
    private var role: AccessRole?
    private let clock = ContinuousClock()
    private var lastExchange: ContinuousClock.Instant?

    public init(channel: any GatewayByteChannel, trustedIdentity: TrustedGatewayIdentity) {
        self.channel = channel; self.trustedIdentity = trustedIdentity
    }

    public func authenticate(username: String, password: Data, role: CredentialRole) async throws -> RelayChannelIdentity {
        guard state == .closed else { throw SecureGatewayPipeError.invalidState }
        state = .authenticating
        let generation = epoch
        do {
            let session = try Security2Session(username: username, password: password, securityPatchVersion: 1)
            self.session = session
            // Expensive ephemeral SRP preparation precedes the relay's total
            // 10-second admission budget; every attempt still uses fresh values.
            let first = try await session.startHandshake()
            try valid(generation)
            let context = try await channel.open()
            try valid(generation)
            let hint: AccessRole = role == .owner ? .owner : .guest
            try await pace(generation)
            let challenge = try await channel.exchange(.session, payload: first, roleHint: hint)
            try valid(generation)
            let proof = try await session.receiveChallenge(challenge)
            try await pace(generation)
            let server = try await channel.exchange(.session, payload: proof, roleHint: hint)
            try valid(generation)
            try await session.verifyServer(server)
            try await pace(generation)
            let binding = try await session.makeIdentityBindingRequest(trustedGateway: trustedIdentity, expectedBoot: context.boot, expectedRole: role)
            let bindingStarted = ProcessInfo.processInfo.systemUptime
            let bound = try await channel.exchange(.data, payload: binding, roleHint: hint)
            let bindingRoundTrip = (ProcessInfo.processInfo.systemUptime - bindingStarted) * 1000
            try valid(generation)
            try await session.verifyIdentityBindingResponse(bound)
            try valid(generation)
            lastRoundTripMilliseconds = bindingRoundTrip
            self.role = hint
            state = .ready
            return context
        } catch {
            if generation == epoch { await close() }
            throw error
        }
    }

    public func request(_ plaintext: Data) async throws -> Data {
        guard state == .ready, let session else { throw SecureGatewayPipeError.invalidState }
        let generation = epoch
        state = .exchanging
        do {
            try await pace(generation)
            let ciphertext = try await session.sealRequest(plaintext)
            try valid(generation)
            let started = ProcessInfo.processInfo.systemUptime
            let response = try await channel.exchange(.data, payload: ciphertext, roleHint: role)
            let roundTrip = (ProcessInfo.processInfo.systemUptime - started) * 1000
            try valid(generation)
            let opened = try await session.openResponse(response)
            try valid(generation)
            lastRoundTripMilliseconds = roundTrip
            state = .ready
            return opened
        } catch {
            if generation == epoch { await close() }
            throw error
        }
    }

    public func close() async {
        guard state != .closing else { return }
        epoch += 1
        state = .closing
        let previous = session
        session = nil; role = nil; lastRoundTripMilliseconds = nil
        await previous?.close()
        await channel.close()
        state = .closed
    }
    private func valid(_ generation: UInt64) throws {
        try Task.checkCancellation()
        guard generation == epoch, state != .closing, state != .closed else { throw SecureGatewayPipeError.changedSession }
    }
    private func pace(_ generation: UInt64) async throws {
        if let lastExchange {
            let next = lastExchange.advanced(by: .milliseconds(500))
            if next > clock.now { try await clock.sleep(until: next) }
        }
        try valid(generation)
        lastExchange = clock.now
    }
}
