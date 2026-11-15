import Darwin
import Foundation
import LightsCore

public enum WiZHomeImportError: LocalizedError {
    case localFileRequired, tooLarge, invalidFormat, unsupportedVersion, invalidKey
    public var errorDescription: String? {
        switch self {
        case .localFileRequired: "Choose a local, regular WiZ JSON export file."
        case .tooLarge: "This export is larger than the supported 16 KB limit."
        case .invalidFormat: "This is not a valid WiZ local-integration JSON export."
        case .unsupportedVersion: "This WiZ export version is not supported yet."
        case .invalidKey: "The export does not contain a valid local-integration key."
        }
    }
}

/// Reads only the key from the documented version-1.0 export. Device records,
/// names, URLs and exported capabilities are neither trusted nor retained.
public enum WiZHomeImport {
    public static let maximumBytes = 16_384
    private struct Export: Decodable { let version: String; let udp_signing_key: String }
    public static func key(from data: Data) throws -> Data {
        guard data.count <= maximumBytes else { throw WiZHomeImportError.tooLarge }
        do { try StrictJSONValidator.validate(data, maximumBytes: maximumBytes) }
        catch { throw WiZHomeImportError.invalidFormat }
        guard let value = try? JSONDecoder().decode(Export.self, from: data) else { throw WiZHomeImportError.invalidFormat }
        guard value.version == "1.0" else { throw WiZHomeImportError.unsupportedVersion }
        let text = value.udp_signing_key
        guard text.utf8.count == 32, text.utf8.allSatisfy({ (48...57).contains($0) || (65...70).contains($0) || (97...102).contains($0) }) else { throw WiZHomeImportError.invalidKey }
        var key = Data()
        var at = text.startIndex
        while at != text.endIndex {
            let end = text.index(at, offsetBy: 2)
            guard let byte = UInt8(text[at..<end], radix: 16) else { throw WiZHomeImportError.invalidKey }
            key.append(byte); at = end
        }
        return key
    }
    public static func key(fromLocalFile url: URL) throws -> Data {
        guard url.isFileURL else { throw WiZHomeImportError.localFileRequired }
        let descriptor = url.path.withCString { Darwin.open($0, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK) }
        guard descriptor >= 0 else { throw WiZHomeImportError.localFileRequired }
        let handle = FileHandle(fileDescriptor: descriptor, closeOnDealloc: true)
        defer { try? handle.close() }
        var status = stat()
        guard fstat(descriptor, &status) == 0, (status.st_mode & S_IFMT) == S_IFREG else { throw WiZHomeImportError.localFileRequired }
        guard status.st_size <= maximumBytes else { throw WiZHomeImportError.tooLarge }
        guard let data = try handle.read(upToCount: maximumBytes + 1) else { throw WiZHomeImportError.invalidFormat }
        return try key(from: data)
    }
}
