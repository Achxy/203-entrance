import Foundation

// The installed macOS Command Line Tools include neither XCTest nor Swift
// Testing. Keep these protocol checks executable without downloading a test
// runtime or requiring a full Xcode installation.
@MainActor private var failedAssertions = 0

@MainActor
func XCTFail(_ message: String, file: StaticString = #filePath, line: UInt = #line) {
    failedAssertions += 1
    print("FAIL \(file):\(line): \(message)")
}

@MainActor
func XCTAssertEqual<T: Equatable>(_ actual: @autoclosure () throws -> T, _ expected: @autoclosure () throws -> T,
                                   file: StaticString = #filePath, line: UInt = #line) {
    do {
        let actual = try actual(), expected = try expected()
        if actual != expected { XCTFail("Expected \(expected), got \(actual)", file: file, line: line) }
    } catch { XCTFail("Unexpected error: \(error)", file: file, line: line) }
}

@MainActor
func XCTAssertTrue(_ expression: @autoclosure () throws -> Bool, file: StaticString = #filePath, line: UInt = #line) {
    XCTAssertEqual(try expression(), true, file: file, line: line)
}

@MainActor
func XCTAssertFalse(_ expression: @autoclosure () throws -> Bool, file: StaticString = #filePath, line: UInt = #line) {
    XCTAssertEqual(try expression(), false, file: file, line: line)
}

@MainActor
func XCTAssertNil<T>(_ expression: @autoclosure () throws -> T?, file: StaticString = #filePath, line: UInt = #line) {
    do { if try expression() != nil { XCTFail("Expected nil", file: file, line: line) } }
    catch { XCTFail("Unexpected error: \(error)", file: file, line: line) }
}

@MainActor
func XCTAssertNoThrow<T>(_ expression: @autoclosure () throws -> T, file: StaticString = #filePath, line: UInt = #line) {
    do { _ = try expression() }
    catch { XCTFail("Unexpected error: \(error)", file: file, line: line) }
}

@MainActor
func XCTAssertThrowsError<T>(_ expression: @autoclosure () throws -> T, _ message: String = "",
                             file: StaticString = #filePath, line: UInt = #line,
                             _ handler: (any Error) -> Void = { _ in }) {
    do { _ = try expression(); XCTFail("Expected an error \(message)", file: file, line: line) }
    catch { handler(error) }
}

@main
@MainActor
struct CheckRunner {
    static func main() {
        let suite = WiZCoreTests()
        let tests: [(String, () throws -> Void)] = [
            ("MAC normalization and strict shape", suite.testMACNormalizationAndStrictShape),
            ("Pinned pywizlight signing vector", suite.testSigningMatchesPinnedPywizlightKnownVectorExactly),
            ("Invalid keys and no downgrade", suite.testSigningRejectsMalformedKeysAndNeverDowngrades),
            ("Explicit legacy read encoding", suite.testLegacyIsExplicitAndReadHasEmptyParams),
            ("Device dimming limits and explicit off", suite.testDimmingUsesPercentReportedMinimumAndExplicitOff),
            ("Power/style and empty-write rejection", suite.testPowerOffCannotCarryStyleAndNoEmptyWrite),
            ("Exclusive color modes and model bounds", suite.testColorModesAreExclusiveAndDeviceBounded),
            ("Fail-closed unknown capabilities", suite.testUnknownCapabilitiesDoNotEnableControls),
            ("Scene speed ratio and white channels", suite.testSceneSpeedRatioAndWhiteChannelLimits),
            ("Expected identity and typed observed state", suite.testPilotReadRequiresActualExpectedIdentityAndTypedState),
            ("Acceptance and trust distinction", suite.testAcknowledgementIsOnlyAcceptedNotObservedOrAuthenticated),
            ("Duplicate keys and malformed JSON", suite.testDuplicateKeysEscapedDuplicatesAndMalformedJSONAreRejected),
            ("Parser resource bounds and extensions", suite.testBoundsAndUnknownExtensionFields),
            ("Public strict gateway JSON validator", suite.testPublicStrictGatewayValidation)
        ]
        for (name, test) in tests {
            let before = failedAssertions
            do { try test() } catch { XCTFail("\(name): unexpected error \(error)") }
            if failedAssertions == before { print("PASS \(name)") }
        }
        print("\(tests.count) protocol check groups; \(failedAssertions) failed assertions")
        if failedAssertions != 0 { exit(1) }
    }
}
