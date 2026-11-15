import Foundation

public enum WiZReplyTrust: Equatable, Sendable {
    /// Matching identity/method are consistency checks, not authentication.
    /// A gateway must isolate this LAN, validate the source peer and reconcile
    /// observed state. env:pro and the presence of hmac confer no trust here.
    case unverifiedLAN
}

public struct WiZPilotState: Equatable, Sendable {
    public let mac: WiZMAC
    public let power: Bool
    public let brightnessPercent: Int?
    public let rgb: WiZRGB?
    public let colorTemperature: Int?
    public let sceneID: Int?
    public let effectSpeed: Int?
    public let dualZoneRatio: Int?
    public let rssi: Int?
    public let trust: WiZReplyTrust
}

public struct WiZAcknowledgement: Equatable, Sendable {
    public let method: WiZMethod
    /// Accepted is not observed/applied; read getPilot and compare after a write.
    public let accepted: Bool
    public let trust: WiZReplyTrust
}

public enum WiZResponseDecoder {
    public static func pilot(_ data: Data, expectedMAC: WiZMAC) throws -> WiZPilotState {
        let result = try envelope(data, method: .getPilot)
        guard let rawMAC = result["mac"]?.string else { throw WiZProtocolError.invalidResult("mac") }
        let mac = try WiZMAC(rawMAC)
        guard mac == expectedMAC else { throw WiZProtocolError.deviceIdentityMismatch }
        guard let power = result["state"]?.boolean else { throw WiZProtocolError.invalidResult("state") }
        let red = try integer(result, "r", range: 0...255)
        let green = try integer(result, "g", range: 0...255)
        let blue = try integer(result, "b", range: 0...255)
        guard [red, green, blue].compactMap({ $0 }).count % 3 == 0 else { throw WiZProtocolError.invalidResult("partial RGB") }
        let rgb: WiZRGB?
        if let red, let green, let blue { rgb = try WiZRGB(red: red, green: green, blue: blue) } else { rgb = nil }
        let temperature = try integer(result, "temp", range: 0...10000)
        if let temperature, temperature > 0 && temperature < 1000 { throw WiZProtocolError.invalidResult("temp") }
        return WiZPilotState(
            mac: mac,
            power: power,
            brightnessPercent: try integer(result, "dimming", range: 0...100),
            rgb: rgb,
            colorTemperature: temperature == 0 ? nil : temperature,
            sceneID: try integer(result, "sceneId", range: 0...65535),
            effectSpeed: try integer(result, "speed", range: 0...200),
            dualZoneRatio: try integer(result, "ratio", range: 0...100),
            rssi: try integer(result, "rssi", range: -127...0),
            trust: .unverifiedLAN
        )
    }

    public static func acknowledgement(_ data: Data, expectedMethod: WiZMethod = .setPilot) throws -> WiZAcknowledgement {
        guard expectedMethod == .setPilot else { throw WiZProtocolError.invalidValue("Acknowledgements are for setPilot") }
        let result = try envelope(data, method: expectedMethod)
        guard result["success"]?.boolean == true else { throw WiZProtocolError.acknowledgementRejected }
        return WiZAcknowledgement(method: expectedMethod, accepted: true, trust: .unverifiedLAN)
    }

    private static func envelope(_ data: Data, method: WiZMethod) throws -> [String: JSONValue] {
        var decoder = try StrictJSON(data)
        guard let object = try decoder.parse().object else { throw WiZProtocolError.malformedJSON }
        guard object["method"]?.string == method.rawValue else { throw WiZProtocolError.unexpectedMethod }
        guard !(object["error"] != nil && object["result"] != nil) else { throw WiZProtocolError.invalidResult("ambiguous result and error") }
        if let error = object["error"] {
            guard let error = error.object, let code = error["code"]?.integer,
                  let message = error["message"]?.string, message.utf8.count <= 512 else { throw WiZProtocolError.invalidResult("error") }
            throw WiZProtocolError.deviceError(code: code, message: message)
        }
        guard let result = object["result"]?.object else { throw WiZProtocolError.missingResult }
        return result
    }

    private static func integer(_ object: [String: JSONValue], _ key: String, range: ClosedRange<Int>) throws -> Int? {
        guard let raw = object[key] else { return nil }
        guard let value = raw.integer, range.contains(value) else { throw WiZProtocolError.invalidResult(key) }
        return value
    }
}
