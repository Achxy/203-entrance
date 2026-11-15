import Foundation
import Darwin

// These checks deliberately use a standalone runner so they work with Apple's
// Command Line Tools alone. XCTest is only bundled with full Xcode on this Mac.
private enum Results {
    static let lock = NSLock()
    static var assertions = 0
    static var failures = 0
    static func record(_ passed: Bool, _ message: String, file: StaticString, line: UInt) {
        lock.lock(); defer { lock.unlock() }
        assertions += 1
        if !passed {
            failures += 1
            print("FAIL \(file):\(line) \(message)")
        }
    }
}

func failCheck(_ text: String, file: StaticString = #filePath, line: UInt = #line) {
    Results.record(false, text, file: file, line: line)
}
func checkTrue(_ value: @autoclosure () throws -> Bool, _ label: String = "", file: StaticString = #filePath, line: UInt = #line) {
    do { Results.record(try value(), "Expected true. " + label, file: file, line: line) }
    catch { failCheck("Unexpected error. " + label, file: file, line: line) }
}
func checkEqual<T: Equatable>(_ left: @autoclosure () throws -> T, _ right: @autoclosure () throws -> T,
                               _ label: String = "", file: StaticString = #filePath, line: UInt = #line) {
    do { Results.record(try left() == right(), "Expected equality. " + label, file: file, line: line) }
    catch { failCheck("Unexpected error. " + label, file: file, line: line) }
}
func checkNotEqual<T: Equatable>(_ left: @autoclosure () throws -> T, _ right: @autoclosure () throws -> T,
                                  _ label: String = "", file: StaticString = #filePath, line: UInt = #line) {
    do { Results.record(try left() != right(), "Expected inequality. " + label, file: file, line: line) }
    catch { failCheck("Unexpected error. " + label, file: file, line: line) }
}
func checkNil<T>(_ value: @autoclosure () throws -> T?, file: StaticString = #filePath, line: UInt = #line) {
    do { Results.record(try value() == nil, "Expected nil", file: file, line: line) }
    catch { failCheck("Unexpected error", file: file, line: line) }
}
func checkThrows<T>(_ expression: @autoclosure () throws -> T, _ label: String = "", file: StaticString = #filePath, line: UInt = #line) {
    do { _ = try expression(); failCheck("Expected rejection. " + label, file: file, line: line) }
    catch { Results.record(true, "", file: file, line: line) }
}

@main enum CheckRunner {
    static func main() async {
        let suite = Security2Checks()
        let tests: [(String, () async throws -> Void)] = [
            ("IDF Python/C fixtures and identity binding", suite.testPinnedIDFPythonInteropAndIdentityBinding),
            ("Reject legacy security and invalid credentials", suite.testNoLegacySecurityOrInvalidCredentials),
            ("Fresh random ephemeral public keys", suite.testClientPublicKeyIsFresh),
            ("Malformed and bounded handshake records", suite.testHandshakeMalformedPacketsFailClosed),
            ("Server proof, nonce, status validation", suite.testServerProofNonceAndStatusFailClosed),
            ("Wrong password rejection", suite.testWrongPasswordNeverEstablishesSession),
            ("Block commands before device binding", suite.testCommandsBlockedBeforeGatewayBinding),
            ("Replay, truncation, tampering, oversize", suite.testReplayTruncationTamperingAndOversizeCloseSession),
            ("Record limits and ordering", suite.testLimitsAndUnexpectedOrdering),
            ("Close cannot resume after transport failure", suite.testCloseForTimeoutCancellationOrTransportChangeCannotResume),
            ("Concurrent requests fail closed", suite.testConcurrentRequestsFailClosed),
            ("Nonce exhaustion never wraps", suite.testCounterCannotWrapOrUseZero),
            ("Every signed identity field and wrong key", suite.testIdentityBindingCrossLanguageAndEveryBoundField),
            ("Duplicate, escaped, and nested binding JSON", suite.testBindingJSONDuplicateEscapedAliasAndNestedValuesRejected),
            ("Bad identity binding removes session role", suite.testIdentityResponseFailureClosesAndRemovesRole),
            ("Trusted identity input bounds", suite.testTrustedIdentityAndContextInputBounds),
            ("Verifier generation and leading-zero inner digest", suite.testVerifierGenerationIncludingLeadingZeroDigest)
        ]
        for (name, run) in tests {
            let before = Results.failures
            do { try await run() }
            catch { failCheck("Unhandled test error in " + name) }
            print("\(Results.failures == before ? "PASS" : "FAIL") \(name)")
        }
        print("\(tests.count) checks, \(Results.assertions) assertions, \(Results.failures) failures")
        if Results.failures != 0 { exit(1) }
    }
}
