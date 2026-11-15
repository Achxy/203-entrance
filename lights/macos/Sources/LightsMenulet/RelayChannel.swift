import Foundation

public enum RelayOperation: String, Codable, Sendable { case identity, session, data, close }
public struct RelayChannelIdentity: Equatable, Sendable {
    public let connection: String
    public let boot: String
    public init(connection: String, boot: String) { self.connection = connection; self.boot = boot }
}
public enum RelayChannelError: LocalizedError, Equatable {
    case invalidEndpoint, invalidFrame, payloadTooLarge, notOpen, inFlight, timeout, rejected(Int), resetRequired
    public var errorDescription: String? {
        switch self {
        case .invalidEndpoint: "The room relay needs a secure WebSocket address ending in /v1/lights."
        case .invalidFrame: "The relay returned an invalid or mismatched response. Reconnect to authenticate again."
        case .payloadTooLarge: "The gateway message exceeds its bounded transport limit."
        case .notOpen: "The gateway connection is closed."
        case .inFlight: "Wait for the current gateway response."
        case .timeout: "The gateway did not respond in time. Reconnect to authenticate again."
        case .rejected: "The gateway connection was rejected. Reconnect to authenticate again."
        case .resetRequired: "The secure session must be reopened before another request."
        }
    }
}

/// This is framing, not authentication. Neither role_hint nor the relay's role
/// field may grant UI privileges; Security2 plus device-identity binding must.
public enum RelayFrameCodec {
    public static let maximumPayloadBytes = 1024
    public static let maximumFrameBytes = 2048
    private struct Request: Encodable {
        let v = 1
        let id: UInt32
        let op: RelayOperation
        let payload: String
        let role_hint: String?
    }
    private struct Incoming: Decodable {
        let v: Int
        let type: String
        let online: Bool?
        let connection: String
        let boot: String
        let `protocol`: Int?
        let id: UInt32?
        let status: Int?
        let payload: String?
        let role: String?
        let error: String?
    }
    public static func encodeRequest(id: UInt32, operation: RelayOperation, payload: Data, roleHint: AccessRole? = nil) throws -> Data {
        guard id > 0 else { throw RelayChannelError.invalidFrame }
        guard payload.count <= maximumPayloadBytes else { throw RelayChannelError.payloadTooLarge }
        guard operation == .close ? payload.isEmpty : !payload.isEmpty else { throw RelayChannelError.invalidFrame }
        return try JSONEncoder().encode(Request(id: id, op: operation, payload: payload.base64EncodedString(), role_hint: roleHint?.rawValue))
    }
    public static func decodeOnline(_ data: Data) throws -> RelayChannelIdentity {
        let parsed = try decode(data), frame = parsed.frame
        guard parsed.keys == ["v", "type", "online", "connection", "boot", "protocol"] else { throw RelayChannelError.invalidFrame }
        guard frame.type == "light_online", frame.online == true, frame.protocol == 1,
              hex(frame.connection, count: 32), hex(frame.boot, count: 16) else { throw RelayChannelError.invalidFrame }
        return RelayChannelIdentity(connection: frame.connection, boot: frame.boot)
    }
    public static func decodeReply(_ data: Data, expectedID: UInt32, identity: RelayChannelIdentity, expectedOperation: RelayOperation = .data) throws -> Data {
        let parsed = try decode(data), frame = parsed.frame
        let required: Set<String> = ["v", "type", "connection", "id", "boot", "status", "payload"]
        guard required.isSubset(of: parsed.keys), parsed.keys.isSubset(of: required.union(["role", "error"])) else { throw RelayChannelError.invalidFrame }
        guard frame.connection == identity.connection, frame.boot == identity.boot else { throw RelayChannelError.invalidFrame }
        guard frame.type == "light_reply", frame.id == expectedID,
              let status = frame.status, (200...599).contains(status),
              frame.role == nil || ["owner", "guest"].contains(frame.role!) else { throw RelayChannelError.invalidFrame }
        if frame.role != nil && expectedOperation != .session && expectedOperation != .data { throw RelayChannelError.invalidFrame }
        guard status == 200, frame.error == nil else { throw RelayChannelError.rejected(status) }
        guard let encoded = frame.payload, let bytes = Data(base64Encoded: encoded), bytes.base64EncodedString() == encoded,
              bytes.count <= maximumPayloadBytes,
              expectedOperation == .close ? bytes.isEmpty : !bytes.isEmpty else { throw RelayChannelError.invalidFrame }
        return bytes
    }
    private static func decode(_ data: Data) throws -> (frame: Incoming, keys: Set<String>) {
        guard data.count <= maximumFrameBytes else { throw RelayChannelError.payloadTooLarge }
        var scanner = FlatRelayObject(data: Array(data))
        let keys = try scanner.parse()
        guard let value = try? JSONDecoder().decode(Incoming.self, from: data), value.v == 1 else { throw RelayChannelError.invalidFrame }
        return (value, keys)
    }
    private static func hex(_ value: String, count: Int) -> Bool {
        value.utf8.count == count && value.utf8.allSatisfy { (48...57).contains($0) || (97...102).contains($0) }
    }
}

/// The envelope has only flat scalar fields. Validate structure before
/// JSONDecoder can discard duplicate keys or normalize fractional integers.
struct FlatRelayObject {
    let data: [UInt8]
    private var at = 0
    init(data: [UInt8]) { self.data = data }
    mutating func parse() throws -> Set<String> {
        guard take(123) else { throw RelayChannelError.invalidFrame }
        var keys = Set<String>()
        if take(125) { throw RelayChannelError.invalidFrame }
        repeat {
            space()
            let key = try string()
            guard keys.insert(key).inserted, keys.count <= 9, take(58) else { throw RelayChannelError.invalidFrame }
            space()
            guard at < data.count else { throw RelayChannelError.invalidFrame }
            if data[at] == 34 { _ = try string() }
            else if data[at] == 116 { try literal("true") }
            else if data[at] == 102 { try literal("false") }
            else if (48...57).contains(data[at]) {
                let first = data[at]; at += 1
                if first != 48 { while at < data.count && (48...57).contains(data[at]) { at += 1 } }
            } else { throw RelayChannelError.invalidFrame }
            if take(125) {
                space()
                guard at == data.count else { throw RelayChannelError.invalidFrame }
                return keys
            }
        } while take(44)
        throw RelayChannelError.invalidFrame
    }
    private mutating func string() throws -> String {
        guard at < data.count, data[at] == 34 else { throw RelayChannelError.invalidFrame }
        let start = at
        at += 1
        var escaped = false
        while at < data.count {
            let byte = data[at]; at += 1
            if !escaped && byte == 34 {
                guard let text = try? JSONDecoder().decode(String.self, from: Data(data[start..<at])) else { throw RelayChannelError.invalidFrame }
                return text
            }
            if !escaped && byte == 92 { escaped = true } else { escaped = false }
        }
        throw RelayChannelError.invalidFrame
    }
    private mutating func literal(_ text: String) throws {
        let bytes = Array(text.utf8)
        guard at + bytes.count <= data.count, Array(data[at..<(at + bytes.count)]) == bytes else { throw RelayChannelError.invalidFrame }
        at += bytes.count
    }
    private mutating func space() { while at < data.count && [9, 10, 13, 32].contains(data[at]) { at += 1 } }
    private mutating func take(_ value: UInt8) -> Bool {
        space()
        guard at < data.count, data[at] == value else { return false }
        at += 1
        return true
    }
}

private final class NoRedirects: NSObject, URLSessionTaskDelegate, @unchecked Sendable {
    func urlSession(_ session: URLSession, task: URLSessionTask, willPerformHTTPRedirection response: HTTPURLResponse,
                    newRequest request: URLRequest, completionHandler: @escaping @Sendable (URLRequest?) -> Void) {
        completionHandler(nil)
    }
}

/// A bounded request/response pipe. It never retries ciphertext or hands a
/// socket receipt to the UI as an authenticated light state. The UI does not
/// instantiate this until the secure application adapter is configured.
public actor RelayWebSocketChannel {
    private let endpoint: URL
    private var session: URLSession?
    private var socket: URLSessionWebSocketTask?
    private var identity: RelayChannelIdentity?
    private var nextID: UInt32 = 0
    private var inFlight = false
    private var epoch: UInt64 = 0

    public init(endpoint: URL) throws {
        guard endpoint.scheme == "wss", let host = endpoint.host, !host.isEmpty,
              endpoint.path == "/v1/lights", endpoint.user == nil, endpoint.password == nil,
              endpoint.query == nil, endpoint.fragment == nil else { throw RelayChannelError.invalidEndpoint }
        self.endpoint = endpoint
    }

    public func open() async throws -> RelayChannelIdentity {
        guard socket == nil, !inFlight else { throw RelayChannelError.inFlight }
        inFlight = true
        let generation = epoch
        let configuration = URLSessionConfiguration.ephemeral
        configuration.httpCookieStorage = nil
        configuration.urlCredentialStorage = nil
        configuration.urlCache = nil
        configuration.timeoutIntervalForRequest = 12
        configuration.timeoutIntervalForResource = 1800
        configuration.waitsForConnectivity = false
        let session = URLSession(configuration: configuration, delegate: NoRedirects(), delegateQueue: nil)
        let task = session.webSocketTask(with: URLRequest(url: endpoint))
        task.maximumMessageSize = RelayFrameCodec.maximumFrameBytes
        self.session = session; socket = task
        task.resume()
        do {
            let data = try await Self.receive(task, seconds: 10)
            guard epoch == generation else { throw RelayChannelError.resetRequired }
            let opened = try RelayFrameCodec.decodeOnline(data)
            identity = opened; nextID = 0; inFlight = false
            return opened
        } catch {
            if epoch == generation { invalidate() }
            throw error
        }
    }

    public func exchange(_ operation: RelayOperation, payload: Data, roleHint: AccessRole? = nil) async throws -> Data {
        guard !inFlight else { invalidate(); throw RelayChannelError.inFlight }
        guard let socket, let identity else { throw RelayChannelError.notOpen }
        guard nextID < UInt32.max else { invalidate(); throw RelayChannelError.resetRequired }
        let generation = epoch
        nextID += 1
        let id = nextID
        inFlight = true
        do {
            let frame = try RelayFrameCodec.encodeRequest(id: id, operation: operation, payload: payload, roleHint: roleHint)
            let deadline: UInt64 = operation == .data ? 3 : 11
            let data = try await Self.withDeadline(socket, seconds: deadline) {
                guard let text = String(data: frame, encoding: .utf8) else { throw RelayChannelError.invalidFrame }
                try await socket.send(.string(text))
                return try await Self.read(socket)
            }
            guard epoch == generation else { throw RelayChannelError.resetRequired }
            let decoded = try RelayFrameCodec.decodeReply(data, expectedID: id, identity: identity, expectedOperation: operation)
            inFlight = false
            if operation == .close { invalidate() }
            return decoded
        } catch {
            if epoch == generation { invalidate() }
            throw error
        }
    }
    public func close() { invalidate() }
    private func invalidate() {
        epoch += 1
        socket?.cancel(with: .goingAway, reason: nil)
        session?.invalidateAndCancel()
        socket = nil; session = nil; identity = nil; inFlight = false; nextID = 0
    }
    private static func read(_ task: URLSessionWebSocketTask) async throws -> Data {
        switch try await task.receive() {
        case .data(let data): return data
        case .string(let text): return Data(text.utf8)
        @unknown default: throw RelayChannelError.invalidFrame
        }
    }
    private static func receive(_ task: URLSessionWebSocketTask, seconds: UInt64) async throws -> Data {
        try await withDeadline(task, seconds: seconds) { try await read(task) }
    }
    private static func withDeadline(_ task: URLSessionWebSocketTask, seconds: UInt64,
                                     operation: @escaping @Sendable () async throws -> Data) async throws -> Data {
        try await withTaskCancellationHandler {
            try await withThrowingTaskGroup(of: Data.self) { group in
                group.addTask { try await operation() }
                group.addTask {
                    try await Task.sleep(for: .seconds(seconds))
                    task.cancel(with: .goingAway, reason: nil)
                    throw RelayChannelError.timeout
                }
                defer { group.cancelAll() }
                guard let result = try await group.next() else { throw RelayChannelError.resetRequired }
                return result
            }
        } onCancel: { task.cancel(with: .goingAway, reason: nil) }
    }
}
