import Foundation

indirect enum JSONValue {
    case object([String: JSONValue]), array([JSONValue]), string(String), number(String), bool(Bool), null
    var object: [String: JSONValue]? { if case .object(let value) = self { value } else { nil } }
    var string: String? { if case .string(let value) = self { value } else { nil } }
    var integer: Int? { if case .number(let value) = self { Int(value) } else { nil } }
    var boolean: Bool? { if case .bool(let value) = self { value } else { nil } }
}

/// Bounded JSON scanner so Foundation's duplicate-key and bool/number coercions
/// cannot turn malformed datagrams into plausible device state.
struct StrictJSON {
    private let data: [UInt8]
    private var at = 0
    private var values = 0
    init(_ input: Data) throws {
        guard input.count <= WiZRequestEncoder.maximumDatagramBytes else { throw WiZProtocolError.payloadTooLarge }
        guard !input.isEmpty else { throw WiZProtocolError.malformedJSON }
        data = Array(input)
    }
    mutating func parse() throws -> JSONValue {
        let result = try value(depth: 0)
        space()
        guard at == data.count else { throw WiZProtocolError.malformedJSON }
        return result
    }
    private mutating func space() { while at < data.count && [9, 10, 13, 32].contains(data[at]) { at += 1 } }
    private mutating func take(_ byte: UInt8) -> Bool {
        space()
        guard at < data.count && data[at] == byte else { return false }
        at += 1
        return true
    }
    private mutating func value(depth: Int) throws -> JSONValue {
        space()
        values += 1
        guard depth <= 12, values <= 1024, at < data.count else { throw WiZProtocolError.malformedJSON }
        switch data[at] {
        case 123:
            at += 1
            var object: [String: JSONValue] = [:]
            if take(125) { return .object(object) }
            repeat {
                space()
                let key = try string()
                guard object[key] == nil else { throw WiZProtocolError.duplicateKey(key) }
                guard take(58) else { throw WiZProtocolError.malformedJSON }
                object[key] = try value(depth: depth + 1)
                if take(125) { return .object(object) }
            } while take(44)
            throw WiZProtocolError.malformedJSON
        case 91:
            at += 1
            var array: [JSONValue] = []
            if take(93) { return .array(array) }
            repeat {
                array.append(try value(depth: depth + 1))
                if take(93) { return .array(array) }
            } while take(44)
            throw WiZProtocolError.malformedJSON
        case 34: return .string(try string())
        case 116: try literal("true"); return .bool(true)
        case 102: try literal("false"); return .bool(false)
        case 110: try literal("null"); return .null
        case 45, 48...57: return .number(try number())
        default: throw WiZProtocolError.malformedJSON
        }
    }
    private mutating func literal(_ value: String) throws {
        let expected = Array(value.utf8)
        guard at + expected.count <= data.count, Array(data[at..<(at + expected.count)]) == expected else { throw WiZProtocolError.malformedJSON }
        at += expected.count
    }
    private mutating func string() throws -> String {
        guard at < data.count && data[at] == 34 else { throw WiZProtocolError.malformedJSON }
        let start = at
        at += 1
        var escaped = false
        while at < data.count {
            let byte = data[at]
            at += 1
            if !escaped && byte == 34 {
                guard let decoded = try? JSONDecoder().decode(String.self, from: Data(data[start..<at])) else { throw WiZProtocolError.malformedJSON }
                return decoded
            }
            if !escaped && byte == 92 { escaped = true } else { escaped = false }
        }
        throw WiZProtocolError.malformedJSON
    }
    private mutating func number() throws -> String {
        let start = at
        if data[at] == 45 { at += 1 }
        guard at < data.count else { throw WiZProtocolError.malformedJSON }
        if data[at] == 48 {
            at += 1
        } else {
            guard (49...57).contains(data[at]) else { throw WiZProtocolError.malformedJSON }
            while at < data.count && (48...57).contains(data[at]) { at += 1 }
        }
        if at < data.count && data[at] == 46 {
            at += 1
            let fraction = at
            while at < data.count && (48...57).contains(data[at]) { at += 1 }
            guard at > fraction else { throw WiZProtocolError.malformedJSON }
        }
        if at < data.count && (data[at] == 69 || data[at] == 101) {
            at += 1
            if at < data.count && (data[at] == 43 || data[at] == 45) { at += 1 }
            let exponent = at
            while at < data.count && (48...57).contains(data[at]) { at += 1 }
            guard at > exponent else { throw WiZProtocolError.malformedJSON }
        }
        return String(decoding: data[start..<at], as: UTF8.self)
    }
}
