import Foundation

public enum BluetoothOperation: UInt8, Sendable { case identity = 0, session = 1, data = 2, close = 3 }

public enum BluetoothChannelError: LocalizedError, Equatable {
    case permissionDenied, unavailable, notOpen, inFlight, timeout, invalidFrame, payloadTooLarge, unsupported, rejected(Int), disconnected
    public var errorDescription: String? {
        switch self {
        case .permissionDenied: "Allow Bluetooth for 203 Lights in System Settings to use nearby control."
        case .unavailable: "Bluetooth is unavailable. Internet control can still be used when the gateway is online."
        case .notOpen: "Connect to the nearby gateway before sending a request."
        case .inFlight: "Wait for the current gateway response."
        case .timeout: "The Bluetooth gateway did not respond in time. Reconnect to authenticate again."
        case .invalidFrame: "The Bluetooth gateway returned an invalid response. Reconnect to authenticate again."
        case .payloadTooLarge: "The gateway message exceeds its bounded Bluetooth limit."
        case .unsupported: "This nearby device does not provide the required lighting service."
        case .rejected: "The gateway rejected the Bluetooth request. Reconnect to authenticate again."
        case .disconnected: "The Bluetooth gateway disconnected. Reconnect to authenticate again."
        }
    }
}

/// Byte framing only. The service UUID and a successful GATT transaction do not
/// authenticate a gateway or grant permission to change any lights.
public enum BluetoothFrameCodec {
    public static let serviceUUID = UUID(uuidString: "89f5e330-0ba9-4d7d-b7e4-320303000001")!
    public static let receiveUUID = UUID(uuidString: "89f5e330-0ba9-4d7d-b7e4-320303000002")!
    public static let transmitUUID = UUID(uuidString: "89f5e330-0ba9-4d7d-b7e4-320303000003")!
    public static let headerBytes = 12
    public static let maximumRequestBytes = 1024
    public static let maximumResponseBytes = 1026
    public static let maximumAttributeBytes = 512

    public static func fragments(operation: BluetoothOperation, id: UInt32, payload: Data,
                                 maximumWriteBytes: Int) throws -> [Data] {
        guard id > 0, maximumWriteBytes >= 20 else { throw BluetoothChannelError.invalidFrame }
        guard payload.count <= maximumRequestBytes else { throw BluetoothChannelError.payloadTooLarge }
        guard operation == .close ? payload.isEmpty : !payload.isEmpty else { throw BluetoothChannelError.invalidFrame }
        let capacity = min(maximumWriteBytes, maximumAttributeBytes) - headerBytes
        let bytes = [UInt8](payload)
        var result: [Data] = []
        var offset = 0
        repeat {
            let count = min(capacity, bytes.count - offset)
            let flags: UInt8 = (offset == 0 ? 1 : 0) | (offset + count == bytes.count ? 2 : 0)
            var packet = [UInt8](repeating: 0, count: headerBytes)
            packet[0] = 1; packet[1] = operation.rawValue; packet[2] = flags
            packet[4] = UInt8(truncatingIfNeeded: id >> 24); packet[5] = UInt8(truncatingIfNeeded: id >> 16)
            packet[6] = UInt8(truncatingIfNeeded: id >> 8); packet[7] = UInt8(truncatingIfNeeded: id)
            packet[8] = UInt8(bytes.count >> 8); packet[9] = UInt8(truncatingIfNeeded: bytes.count)
            packet[10] = UInt8(offset >> 8); packet[11] = UInt8(truncatingIfNeeded: offset)
            packet.append(contentsOf: bytes[offset..<(offset + count)])
            result.append(Data(packet)); offset += count
        } while offset < bytes.count
        return result
    }
}

public struct BluetoothResponseAssembler: Sendable {
    private let operation: BluetoothOperation
    private let id: UInt32
    private var total: Int?
    private var body = Data()
    private var finished = false

    public init(operation: BluetoothOperation, id: UInt32) throws {
        guard id > 0 else { throw BluetoothChannelError.invalidFrame }
        self.operation = operation; self.id = id
    }

    /// Returns the complete binary payload only after every contiguous fragment
    /// and a200 status. Every error requires closing the secure connection.
    public mutating func consume(_ data: Data) throws -> Data? {
        guard !finished, (12...512).contains(data.count) else { throw BluetoothChannelError.invalidFrame }
        let p = [UInt8](data)
        let messageID = UInt32(p[4]) << 24 | UInt32(p[5]) << 16 | UInt32(p[6]) << 8 | UInt32(p[7])
        let size = Int(p[8]) << 8 | Int(p[9]), offset = Int(p[10]) << 8 | Int(p[11])
        let count = p.count - 12, first = p[2] & 1 != 0, last = p[2] & 2 != 0
        guard p[0] == 1, p[1] == operation.rawValue | 0x80, p[2] <= 3, p[3] == 0,
              messageID == id, (2...1026).contains(size), offset == body.count,
              count > 0, offset + count <= size, last == (offset + count == size),
              first == (total == nil), total == nil || total == size else { throw BluetoothChannelError.invalidFrame }
        total = size; body.append(contentsOf: p[12...])
        guard last else { return nil }
        finished = true
        let bytes = [UInt8](body), status = Int(bytes[0]) << 8 | Int(bytes[1])
        guard (200...599).contains(status) else { throw BluetoothChannelError.invalidFrame }
        guard status == 200 else { throw BluetoothChannelError.rejected(status) }
        let payload = body.dropFirst(2)
        guard operation == .close ? payload.isEmpty : !payload.isEmpty else { throw BluetoothChannelError.invalidFrame }
        return Data(payload)
    }
}
