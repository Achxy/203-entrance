import Foundation
import LightsCore

@MainActor
final class WiZCoreTests {
    private func capabilities(minimum: Int = 10) throws -> WiZCapabilities {
        try WiZCapabilities(brightness: true, minimumDimming: minimum, rgb: true,
                            kelvinRange: 2200...6500, supportedSceneIDs: [1, 4, 11, 201],
                            effectSpeedRange: 10...200, dualZoneRatio: true,
                            warmWhite: true, coldWhite: true)
    }

    private func encoded(_ patch: WiZLightingPatch, minimum: Int = 10) throws -> WiZEncodedRequest {
        try WiZRequestEncoder.setPilot(patch, capabilities: capabilities(minimum: minimum), security: .legacyUnsigned)
    }

    private func string(_ request: WiZEncodedRequest) -> String { String(decoding: request.bytes, as: UTF8.self) }
    private func assertError<T>(_ expected: WiZProtocolError, _ body: () throws -> T, file: StaticString = #filePath, line: UInt = #line) {
        XCTAssertThrowsError(try body(), file: file, line: line) { XCTAssertEqual($0 as? WiZProtocolError, expected, file: file, line: line) }
    }

    func testMACNormalizationAndStrictShape() throws {
        let expected = try WiZMAC("a8bb5006033d")
        XCTAssertEqual(try WiZMAC("A8:BB:50:06:03:3D"), expected)
        XCTAssertEqual(try WiZMAC("A8-BB-50-06-03-3D"), expected)
        XCTAssertEqual(try JSONDecoder().decode(WiZMAC.self, from: JSONEncoder().encode(expected)), expected)
        for invalid in ["", "a8bb5006033", "a8bb5006033dd", " A8BB5006033D", "A8:BB-50:06:03:3D", "Ａ8bb5006033d", "a8bb5006033z", "a8 bb 50 06 03 3d"] {
            assertError(.invalidMAC) { try WiZMAC(invalid) }
        }
    }

    func testSigningMatchesPinnedPywizlightKnownVectorExactly() throws {
        let key = try WiZHomeKey(hex: "00112233445566778899aabbccddeeff")
        let request = try WiZRequestEncoder.setPilot(.init(power: true), capabilities: capabilities(),
            security: .verifiedOnly(homeKey: key), unixTime: 1790441723, clockIsTrusted: true)
        XCTAssertEqual(string(request), #"{"method":"setPilot","env":"pro","params":{"state":true,"sigTs":1790441723},"hmac":"vARMv4JpVsxYabcBovABd25ISBzYT8+71v0CUtpU1sg="}"#)
        XCTAssertTrue(request.signed)
    }

    func testSigningRejectsMalformedKeysAndNeverDowngrades() throws {
        for key in ["", String(repeating: "a", count: 31), String(repeating: "a", count: 33), String(repeating: "z", count: 32), "00112233445566778899aabbccddeef "] {
            assertError(.invalidSigningKey) { try WiZHomeKey(hex: key) }
        }
        assertError(.signingKeyRequired) { try WiZRequestEncoder.read(.getPilot, security: .verifiedOnly(homeKey: nil), unixTime: 1790441723, clockIsTrusted: true) }
        let key = try WiZHomeKey(hex: "00112233445566778899AABBCCDDEEFF")
        assertError(.trustedClockRequired) { try WiZRequestEncoder.read(.getPilot, security: .verifiedOnly(homeKey: key), unixTime: 1790441723) }
        for timestamp: Int64? in [nil, 0, -1] {
            assertError(.invalidTimestamp) { try WiZRequestEncoder.read(.getPilot, security: .verifiedOnly(homeKey: key), unixTime: timestamp, clockIsTrusted: true) }
        }
        XCTAssertTrue(try WiZRequestEncoder.read(.getPilot, security: .verifiedOnly(homeKey: key), unixTime: 1790441723, clockIsTrusted: true).signed)
    }

    func testLegacyIsExplicitAndReadHasEmptyParams() throws {
        for method in [WiZMethod.getPilot, .getSystemConfig, .getModelConfig, .getUserConfig, .getPower] {
            let result = try WiZRequestEncoder.read(method, security: .legacyUnsigned)
            XCTAssertFalse(result.signed)
            XCTAssertEqual(string(result), "{\"method\":\"\(method.rawValue)\",\"params\":{}}")
        }
        XCTAssertThrowsError(try WiZRequestEncoder.read(.setPilot, security: .legacyUnsigned))
    }

    func testDimmingUsesPercentReportedMinimumAndExplicitOff() throws {
        let legacy = try encoded(.init(power: true, brightnessPercent: 1))
        XCTAssertEqual(legacy.effectiveBrightnessPercent, 10)
        XCTAssertEqual(string(legacy), #"{"method":"setPilot","params":{"state":true,"dimming":10}}"#)
        XCTAssertEqual(try encoded(.init(brightnessPercent: 1), minimum: 1).effectiveBrightnessPercent, 1)
        XCTAssertEqual(try encoded(.init(brightnessPercent: 50)).effectiveBrightnessPercent, 50)
        XCTAssertEqual(try encoded(.init(brightnessPercent: 100)).effectiveBrightnessPercent, 100)
        let zero = try encoded(.init(brightnessPercent: 0))
        XCTAssertEqual(zero.effectiveBrightnessPercent, 0)
        XCTAssertEqual(string(zero), #"{"method":"setPilot","params":{"state":false}}"#)
        for value in [-1, 101, Int.max] { XCTAssertThrowsError(try encoded(.init(brightnessPercent: value))) }
        XCTAssertThrowsError(try encoded(.init(power: true, brightnessPercent: 0)))
        XCTAssertThrowsError(try encoded(.init(power: false, brightnessPercent: 50)))
    }

    func testPowerOffCannotCarryStyleAndNoEmptyWrite() throws {
        assertError(.emptyPatch) { try encoded(.init()) }
        XCTAssertThrowsError(try encoded(.init(power: false, colorTemperature: 2700)))
        XCTAssertThrowsError(try encoded(.init(brightnessPercent: 0, sceneID: 4)))
        XCTAssertThrowsError(try encoded(.init(power: false, effectSpeed: 50)))
        XCTAssertThrowsError(try encoded(.init(power: false, dualZoneRatio: 0)))
        XCTAssertEqual(string(try encoded(.init(power: false))), #"{"method":"setPilot","params":{"state":false}}"#)
    }

    func testColorModesAreExclusiveAndDeviceBounded() throws {
        let rgb = try WiZRGB(red: 0, green: 127, blue: 255)
        XCTAssertThrowsError(try encoded(.init(rgb: rgb, colorTemperature: 2700)))
        XCTAssertThrowsError(try encoded(.init(rgb: rgb, sceneID: 4)))
        XCTAssertThrowsError(try encoded(.init(colorTemperature: 2700, sceneID: 4)))
        XCTAssertThrowsError(try encoded(.init(colorTemperature: 2700, whiteChannels: .init(warm: 1))))
        XCTAssertEqual(string(try encoded(.init(rgb: rgb))), #"{"method":"setPilot","params":{"r":0,"g":127,"b":255}}"#)
        XCTAssertThrowsError(try encoded(.init(colorTemperature: 2199)))
        XCTAssertThrowsError(try encoded(.init(colorTemperature: 6501)))
        for temperature in [2200, 6500] { XCTAssertNoThrow(try encoded(.init(colorTemperature: temperature))) }
        for channel in [-1, 256, Int.max] { XCTAssertThrowsError(try WiZRGB(red: channel, green: 0, blue: 0)) }
    }

    func testUnknownCapabilitiesDoNotEnableControls() throws {
        let unknown = try WiZCapabilities()
        for patch in [WiZLightingPatch(brightnessPercent: 50), .init(rgb: try .init(red: 1, green: 2, blue: 3)), .init(colorTemperature: 2700), .init(sceneID: 4), .init(effectSpeed: 100), .init(dualZoneRatio: 50), .init(whiteChannels: try .init(warm: 255))] {
            XCTAssertThrowsError(try WiZRequestEncoder.setPilot(patch, capabilities: unknown, security: .legacyUnsigned))
        }
        XCTAssertNoThrow(try WiZRequestEncoder.setPilot(.init(power: false), capabilities: unknown, security: .legacyUnsigned))
        XCTAssertThrowsError(try WiZCapabilities(minimumDimming: 0))
        XCTAssertThrowsError(try WiZCapabilities(minimumDimming: 101))
        XCTAssertThrowsError(try WiZCapabilities(kelvinRange: 0...10000))
        XCTAssertThrowsError(try WiZCapabilities(supportedSceneIDs: [0]))
        XCTAssertThrowsError(try WiZCapabilities(supportedSceneIDs: Set(1...513)))
        XCTAssertThrowsError(try WiZCapabilities(effectSpeedRange: 1...200))
    }

    func testSceneSpeedRatioAndWhiteChannelLimits() throws {
        XCTAssertNoThrow(try encoded(.init(sceneID: 201)))
        XCTAssertThrowsError(try encoded(.init(sceneID: 202)))
        for speed in [10, 200] { XCTAssertNoThrow(try encoded(.init(effectSpeed: speed))) }
        for speed in [9, 201] { XCTAssertThrowsError(try encoded(.init(effectSpeed: speed))) }
        for ratio in [0, 100] { XCTAssertNoThrow(try encoded(.init(dualZoneRatio: ratio))) }
        for ratio in [-1, 101] { XCTAssertThrowsError(try encoded(.init(dualZoneRatio: ratio))) }
        XCTAssertThrowsError(try WiZWhiteChannels())
        XCTAssertThrowsError(try WiZWhiteChannels(warm: 256))
        let rgb = try WiZRGB(red: 1, green: 2, blue: 3)
        let channels = try WiZWhiteChannels(warm: 4, cold: 5)
        XCTAssertEqual(string(try encoded(.init(rgb: rgb, whiteChannels: channels))), #"{"method":"setPilot","params":{"r":1,"g":2,"b":3,"w":4,"c":5}}"#)
    }

    func testPilotReadRequiresActualExpectedIdentityAndTypedState() throws {
        let mac = try WiZMAC("a8bb5006033d")
        let body = #"{"method":"getPilot","env":"pro","result":{"mac":"a8bb5006033d","state":true,"dimming":50,"r":1,"g":2,"b":3,"temp":2700,"sceneId":0,"rssi":-65},"hmac":"not-proof"}"#
        let state = try WiZResponseDecoder.pilot(Data(body.utf8), expectedMAC: mac)
        XCTAssertTrue(state.power)
        XCTAssertEqual(state.mac, mac)
        XCTAssertEqual(state.brightnessPercent, 50)
        XCTAssertEqual(state.colorTemperature, 2700)
        XCTAssertEqual(state.rgb, try WiZRGB(red: 1, green: 2, blue: 3))
        XCTAssertEqual(state.trust, .unverifiedLAN)
        assertError(.deviceIdentityMismatch) { try WiZResponseDecoder.pilot(Data(body.utf8), expectedMAC: WiZMAC("a8bb5006033e")) }
        for result in [#"{"mac":"a8bb5006033d","state":1}"#, #"{"mac":"a8bb5006033d","state":true,"dimming":true}"#, #"{"mac":"a8bb5006033d","state":true,"dimming":50.0}"#, #"{"mac":"a8bb5006033d","state":true,"r":1}"#, #"{"mac":"a8bb5006033d","state":true,"temp":999}"#, #"{"state":true}"#, #"{"mac":"a8bb5006033d"}"#] {
            XCTAssertThrowsError(try WiZResponseDecoder.pilot(Data("{\"method\":\"getPilot\",\"result\":\(result)}".utf8), expectedMAC: mac))
        }
        let color = Data(#"{"method":"getPilot","result":{"mac":"a8bb5006033d","state":true,"temp":0}}"#.utf8)
        XCTAssertNil(try WiZResponseDecoder.pilot(color, expectedMAC: mac).colorTemperature)
    }

    func testAcknowledgementIsOnlyAcceptedNotObservedOrAuthenticated() throws {
        let accepted = try WiZResponseDecoder.acknowledgement(Data(#"{"method":"setPilot","env":"pro","result":{"success":true},"hmac":"anything"}"#.utf8))
        XCTAssertTrue(accepted.accepted)
        XCTAssertEqual(accepted.trust, .unverifiedLAN)
        for result in ["false", "1", "null", "\"true\""] {
            assertError(.acknowledgementRejected) { try WiZResponseDecoder.acknowledgement(Data("{\"method\":\"setPilot\",\"result\":{\"success\":\(result)}}".utf8)) }
        }
        XCTAssertThrowsError(try WiZResponseDecoder.acknowledgement(Data(#"{"method":"setPilot","params":{"state":true}}"#.utf8)))
        assertError(.unexpectedMethod) { try WiZResponseDecoder.acknowledgement(Data(#"{"method":"getPilot","result":{"success":true}}"#.utf8)) }
        assertError(.deviceError(code: -32605, message: "Security error")) { try WiZResponseDecoder.acknowledgement(Data(#"{"method":"setPilot","error":{"code":-32605,"message":"Security error"}}"#.utf8)) }
        XCTAssertThrowsError(try WiZResponseDecoder.acknowledgement(Data(#"{"method":"setPilot","result":{"success":true},"error":{"code":-1,"message":"no"}}"#.utf8)))
    }

    func testDuplicateKeysEscapedDuplicatesAndMalformedJSONAreRejected() throws {
        for body in [#"{"method":"setPilot","method":"getPilot","result":{"success":true}}"#, #"{"method":"setPilot","result":{"success":true,"su\u0063cess":false}}"#] {
            XCTAssertThrowsError(try WiZResponseDecoder.acknowledgement(Data(body.utf8))) { error in
                guard case WiZProtocolError.duplicateKey = error else { return XCTFail("Expected duplicate-key rejection") }
            }
        }
        for body in ["", "{}{}", "[]", "null", #"{"method":"setPilot",}"#, #"{"method":"setPilot","result":{"success":tru}}"#, #"{"method":"setPilot","result":{"success":true},"n":01}"#, #"{"method":"setPilot","result":{"success":true},"n":NaN}"#, #"{"method":"setPilot","result":{"success":true},"n":1.}"#, #"{"method":"setPilot","result":{"success":true},"n":1e}"#, #"{"method":"setPilot","result":{"success":true},"bad":"\q"}"#] {
            XCTAssertThrowsError(try WiZResponseDecoder.acknowledgement(Data(body.utf8)), body)
        }
        XCTAssertThrowsError(try WiZResponseDecoder.acknowledgement(Data([0xff, 0xfe])))
    }

    func testBoundsAndUnknownExtensionFields() throws {
        assertError(.payloadTooLarge) { try WiZResponseDecoder.acknowledgement(Data(repeating: 32, count: 16_385)) }
        let nested = String(repeating: "[", count: 14) + "0" + String(repeating: "]", count: 14)
        XCTAssertThrowsError(try WiZResponseDecoder.acknowledgement(Data("{\"method\":\"setPilot\",\"result\":{\"success\":true},\"extension\":\(nested)}".utf8)))
        let manyValues = "[" + Array(repeating: "0", count: 1024).joined(separator: ",") + "]"
        XCTAssertThrowsError(try WiZResponseDecoder.acknowledgement(Data("{\"method\":\"setPilot\",\"result\":{\"success\":true},\"extension\":\(manyValues)}".utf8)))
        let valid = Data(#"{"method":"setPilot","result":{"success":true},"extension":[null,{"valid":true},-1.25e+2,"hello"]}"#.utf8)
        XCTAssertTrue(try WiZResponseDecoder.acknowledgement(valid).accepted)
    }

    func testPublicStrictGatewayValidation() throws {
        let valid = Data(#"{"capabilities":[{"features":15,"cct":[2200,6500]}],"reported":{"state":true}}"#.utf8)
        XCTAssertNoThrow(try StrictJSONValidator.validate(valid, maximumBytes: 1008))
        for body in [#"{"reported":{"state":true,"st\u0061te":false}}"#, #"{"capabilities":[],"capabilities":{}}"#, #"{"a":1}{}"#, #"{"a":01}"#, #"{"a":"\q"}"#] {
            XCTAssertThrowsError(try StrictJSONValidator.validate(Data(body.utf8), maximumBytes: 1008))
        }
        XCTAssertThrowsError(try StrictJSONValidator.validate(valid, maximumBytes: 0))
        XCTAssertThrowsError(try StrictJSONValidator.validate(valid, maximumBytes: 16_385))
        assertError(.payloadTooLarge) { try StrictJSONValidator.validate(Data(repeating: 32, count: 1009), maximumBytes: 1008) }
    }
}
