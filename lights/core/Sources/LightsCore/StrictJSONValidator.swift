import Foundation

/// A syntax and resource check before decoding an authenticated gateway payload.
/// This validates duplicate decoded keys, UTF-8/escapes, nesting, value count and
/// trailing bytes. It does not authenticate the sender or validate a schema.
public enum StrictJSONValidator {
    public static func validate(_ input: Data, maximumBytes: Int = 16_384) throws {
        guard maximumBytes > 0, maximumBytes <= WiZRequestEncoder.maximumDatagramBytes else {
            throw WiZProtocolError.invalidValue("JSON byte limit must be 1...16384")
        }
        guard input.count <= maximumBytes else { throw WiZProtocolError.payloadTooLarge }
        var parser = try StrictJSON(input)
        _ = try parser.parse()
    }
}
