import Foundation

public enum WiZProtocolError: Error, Equatable, Sendable {
    case invalidMAC
    case invalidSigningKey
    case signingKeyRequired
    case trustedClockRequired
    case invalidTimestamp
    case invalidValue(String)
    case unsupported(String)
    case conflictingFields(String)
    case emptyPatch
    case payloadTooLarge
    case malformedJSON
    case duplicateKey(String)
    case unexpectedMethod
    case missingResult
    case invalidResult(String)
    case deviceIdentityMismatch
    case deviceError(code: Int, message: String)
    case acknowledgementRejected
}

public struct WiZMAC: Hashable, Codable, Sendable, CustomStringConvertible {
    public let rawValue: String

    public init(_ input: String) throws {
        let bytes = Array(input.utf8)
        let digits: [UInt8]
        if bytes.count == 12 {
            digits = bytes
        } else if bytes.count == 17, let separator = bytes.dropFirst(2).first,
                  separator == 58 || separator == 45 {
            guard [2, 5, 8, 11, 14].allSatisfy({ bytes[$0] == separator }) else {
                throw WiZProtocolError.invalidMAC
            }
            digits = bytes.enumerated().filter { $0.offset % 3 != 2 }.map(\.element)
        } else {
            throw WiZProtocolError.invalidMAC
        }
        guard digits.allSatisfy({ (48...57).contains($0) || (65...70).contains($0) || (97...102).contains($0) }) else {
            throw WiZProtocolError.invalidMAC
        }
        rawValue = String(decoding: digits, as: UTF8.self).lowercased()
    }

    public var description: String { rawValue }

    public init(from decoder: any Decoder) throws {
        try self.init(decoder.singleValueContainer().decode(String.self))
    }

    public func encode(to encoder: any Encoder) throws {
        var container = encoder.singleValueContainer()
        try container.encode(rawValue)
    }
}

public struct WiZRGB: Equatable, Sendable {
    public let red: Int
    public let green: Int
    public let blue: Int

    public init(red: Int, green: Int, blue: Int) throws {
        guard [red, green, blue].allSatisfy({ (0...255).contains($0) }) else {
            throw WiZProtocolError.invalidValue("RGB channels must be 0...255")
        }
        self.red = red
        self.green = green
        self.blue = blue
    }
}

public struct WiZWhiteChannels: Equatable, Sendable {
    public let warm: Int?
    public let cold: Int?

    public init(warm: Int? = nil, cold: Int? = nil) throws {
        guard warm != nil || cold != nil,
              [warm, cold].compactMap({ $0 }).allSatisfy({ (0...255).contains($0) }) else {
            throw WiZProtocolError.invalidValue("At least one white channel must be 0...255")
        }
        self.warm = warm
        self.cold = cold
    }
}

/// Capabilities must come from an enrolled device's model/configuration, not a
/// general list of WiZ features. Unknown features stay disabled.
public struct WiZCapabilities: Equatable, Sendable {
    public let brightness: Bool
    public let minimumDimming: Int
    public let rgb: Bool
    public let kelvinRange: ClosedRange<Int>?
    public let supportedSceneIDs: Set<Int>
    public let effectSpeedRange: ClosedRange<Int>?
    public let dualZoneRatio: Bool
    public let warmWhite: Bool
    public let coldWhite: Bool

    public init(
        brightness: Bool = false,
        minimumDimming: Int = 10,
        rgb: Bool = false,
        kelvinRange: ClosedRange<Int>? = nil,
        supportedSceneIDs: Set<Int> = [],
        effectSpeedRange: ClosedRange<Int>? = nil,
        dualZoneRatio: Bool = false,
        warmWhite: Bool = false,
        coldWhite: Bool = false
    ) throws {
        guard (1...100).contains(minimumDimming),
              supportedSceneIDs.count <= 512,
              supportedSceneIDs.allSatisfy({ (1...65535).contains($0) }) else {
            throw WiZProtocolError.invalidValue("Invalid reported dimming or scene limits")
        }
        if let kelvinRange, !(1000...10000).contains(kelvinRange.lowerBound) || !(1000...10000).contains(kelvinRange.upperBound) {
            throw WiZProtocolError.invalidValue("Invalid reported CCT range")
        }
        if let effectSpeedRange, !(10...200).contains(effectSpeedRange.lowerBound) || !(10...200).contains(effectSpeedRange.upperBound) {
            throw WiZProtocolError.invalidValue("Invalid reported effect speed range")
        }
        self.brightness = brightness
        self.minimumDimming = minimumDimming
        self.rgb = rgb
        self.kelvinRange = kelvinRange
        self.supportedSceneIDs = supportedSceneIDs
        self.effectSpeedRange = effectSpeedRange
        self.dualZoneRatio = dualZoneRatio
        self.warmWhite = warmWhite
        self.coldWhite = coldWhite
    }
}

/// Brightness is expressed in percent, not pywizlight's 0...255 UI units.
/// Zero is an explicit off request. Positive values are clamped to the model's
/// minimum dimming level, and the encoded request reports that effective level.
public struct WiZLightingPatch: Equatable, Sendable {
    public let power: Bool?
    public let brightnessPercent: Int?
    public let rgb: WiZRGB?
    public let colorTemperature: Int?
    public let sceneID: Int?
    public let effectSpeed: Int?
    public let dualZoneRatio: Int?
    public let whiteChannels: WiZWhiteChannels?

    public init(
        power: Bool? = nil,
        brightnessPercent: Int? = nil,
        rgb: WiZRGB? = nil,
        colorTemperature: Int? = nil,
        sceneID: Int? = nil,
        effectSpeed: Int? = nil,
        dualZoneRatio: Int? = nil,
        whiteChannels: WiZWhiteChannels? = nil
    ) {
        self.power = power
        self.brightnessPercent = brightnessPercent
        self.rgb = rgb
        self.colorTemperature = colorTemperature
        self.sceneID = sceneID
        self.effectSpeed = effectSpeed
        self.dualZoneRatio = dualZoneRatio
        self.whiteChannels = whiteChannels
    }
}
