import CryptoKit
import Foundation

/// Imported from the owner's WiZ home export. Never embed it in a distributed
/// app or guest grant. The type intentionally exposes no key-data accessor.
public struct WiZHomeKey: Sendable {
    private let key: SymmetricKey

    public init(hex: String) throws {
        let bytes = Array(hex.utf8)
        guard bytes.count == 32 else { throw WiZProtocolError.invalidSigningKey }
        var decoded = [UInt8]()
        decoded.reserveCapacity(16)
        func digit(_ byte: UInt8) -> UInt8? {
            switch byte {
            case 48...57: byte - 48
            case 65...70: byte - 65 + 10
            case 97...102: byte - 97 + 10
            default: nil
            }
        }
        for offset in stride(from: 0, to: bytes.count, by: 2) {
            guard let high = digit(bytes[offset]), let low = digit(bytes[offset + 1]) else {
                throw WiZProtocolError.invalidSigningKey
            }
            decoded.append(high << 4 | low)
        }
        key = SymmetricKey(data: decoded)
    }

    fileprivate func authenticate(_ data: Data) -> String {
        Data(HMAC<SHA256>.authenticationCode(for: data, using: key)).base64EncodedString()
    }
}

public enum WiZLocalSecurity: Sendable {
    /// Explicit opt-in for enrolled firmware verified to support legacy control.
    case legacyUnsigned
    /// A missing key is an error. There is no unsigned retry or fallback.
    case verifiedOnly(homeKey: WiZHomeKey?)
}

public enum WiZMethod: String, Sendable {
    case getPilot, getSystemConfig, getModelConfig, getUserConfig, getPower, setPilot
}

public struct WiZEncodedRequest: Sendable {
    public let method: WiZMethod
    public let bytes: Data
    public let signed: Bool
    public let effectiveBrightnessPercent: Int?
}

public enum WiZRequestEncoder {
    public static let maximumDatagramBytes = 16_384

    public static func read(
        _ method: WiZMethod,
        security: WiZLocalSecurity,
        unixTime: Int64? = nil,
        clockIsTrusted: Bool = false
    ) throws -> WiZEncodedRequest {
        guard method != .setPilot else { throw WiZProtocolError.invalidValue("Use setPilot with a validated patch") }
        return try encode(method, fields: [], security: security, unixTime: unixTime, clockIsTrusted: clockIsTrusted)
    }

    public static func setPilot(
        _ patch: WiZLightingPatch,
        capabilities: WiZCapabilities,
        security: WiZLocalSecurity,
        unixTime: Int64? = nil,
        clockIsTrusted: Bool = false
    ) throws -> WiZEncodedRequest {
        let hasColor = patch.rgb != nil || patch.colorTemperature != nil || patch.sceneID != nil || patch.whiteChannels != nil
        let hasStyle = hasColor || patch.effectSpeed != nil || patch.dualZoneRatio != nil
        guard patch.power != nil || patch.brightnessPercent != nil || hasStyle else { throw WiZProtocolError.emptyPatch }
        let modes = [patch.rgb != nil || patch.whiteChannels != nil, patch.colorTemperature != nil, patch.sceneID != nil].filter { $0 }.count
        guard modes <= 1 else { throw WiZProtocolError.conflictingFields("RGB/channels, CCT and scene are mutually exclusive") }
        if let brightness = patch.brightnessPercent {
            guard (0...100).contains(brightness) else { throw WiZProtocolError.invalidValue("Brightness must be 0...100 percent") }
            guard capabilities.brightness else { throw WiZProtocolError.unsupported("brightness") }
        }
        if patch.power == false && (hasStyle || (patch.brightnessPercent ?? 0) > 0) {
            throw WiZProtocolError.conflictingFields("Power off cannot include lighting styles")
        }
        if patch.brightnessPercent == 0 {
            guard patch.power != true && !hasStyle else { throw WiZProtocolError.conflictingFields("Zero brightness is off") }
            return try encode(.setPilot, fields: [("state", "false")], security: security, unixTime: unixTime, clockIsTrusted: clockIsTrusted, brightness: 0)
        }
        var fields: [(String, String)] = []
        if let power = patch.power { fields.append(("state", power ? "true" : "false")) }
        var effectiveBrightness: Int?
        if let value = patch.brightnessPercent {
            effectiveBrightness = max(capabilities.minimumDimming, value)
            fields.append(("dimming", String(effectiveBrightness!)))
        }
        if let rgb = patch.rgb {
            guard capabilities.rgb else { throw WiZProtocolError.unsupported("RGB") }
            fields += [("r", String(rgb.red)), ("g", String(rgb.green)), ("b", String(rgb.blue))]
        }
        if let channels = patch.whiteChannels {
            if let warm = channels.warm {
                guard capabilities.warmWhite else { throw WiZProtocolError.unsupported("warm white channel") }
                fields.append(("w", String(warm)))
            }
            if let cold = channels.cold {
                guard capabilities.coldWhite else { throw WiZProtocolError.unsupported("cold white channel") }
                fields.append(("c", String(cold)))
            }
        }
        if let temperature = patch.colorTemperature {
            guard let range = capabilities.kelvinRange else { throw WiZProtocolError.unsupported("color temperature") }
            guard range.contains(temperature) else { throw WiZProtocolError.invalidValue("Color temperature is outside the device range") }
            fields.append(("temp", String(temperature)))
        }
        if let scene = patch.sceneID {
            guard capabilities.supportedSceneIDs.contains(scene) else { throw WiZProtocolError.unsupported("scene") }
            fields.append(("sceneId", String(scene)))
        }
        if let speed = patch.effectSpeed {
            guard let range = capabilities.effectSpeedRange else { throw WiZProtocolError.unsupported("effect speed") }
            guard range.contains(speed) else { throw WiZProtocolError.invalidValue("Effect speed is outside the device range") }
            fields.append(("speed", String(speed)))
        }
        if let ratio = patch.dualZoneRatio {
            guard capabilities.dualZoneRatio else { throw WiZProtocolError.unsupported("dual-zone ratio") }
            guard (0...100).contains(ratio) else { throw WiZProtocolError.invalidValue("Ratio must be 0...100") }
            fields.append(("ratio", String(ratio)))
        }
        return try encode(.setPilot, fields: fields, security: security, unixTime: unixTime, clockIsTrusted: clockIsTrusted, brightness: effectiveBrightness)
    }

    private static func encode(
        _ method: WiZMethod,
        fields: [(String, String)],
        security: WiZLocalSecurity,
        unixTime: Int64?,
        clockIsTrusted: Bool,
        brightness: Int? = nil
    ) throws -> WiZEncodedRequest {
        var fields = fields
        let homeKey: WiZHomeKey?
        switch security {
        case .legacyUnsigned: homeKey = nil
        case .verifiedOnly(let key):
            guard let key else { throw WiZProtocolError.signingKeyRequired }
            guard clockIsTrusted else { throw WiZProtocolError.trustedClockRequired }
            guard let unixTime, unixTime > 0 else { throw WiZProtocolError.invalidTimestamp }
            fields.append(("sigTs", String(unixTime)))
            homeKey = key
        }
        // Field names are fixed protocol constants, and values are validated
        // integer/boolean literals. Sign the exact params bytes put on the wire.
        let params = "{" + fields.map { "\"\($0.0)\":\($0.1)" }.joined(separator: ",") + "}"
        let envelope: String
        if let homeKey {
            envelope = "{\"method\":\"\(method.rawValue)\",\"env\":\"pro\",\"params\":\(params),\"hmac\":\"\(homeKey.authenticate(Data(params.utf8)))\"}"
        } else {
            envelope = "{\"method\":\"\(method.rawValue)\",\"params\":\(params)}"
        }
        let bytes = Data(envelope.utf8)
        guard bytes.count <= maximumDatagramBytes else { throw WiZProtocolError.payloadTooLarge }
        return WiZEncodedRequest(method: method, bytes: bytes, signed: homeKey != nil, effectiveBrightnessPercent: brightness)
    }
}
