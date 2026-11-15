import Foundation
import Darwin

@main struct LightsGatewayProbeMain {
    @MainActor static func main() async {
        do {
            switch try ProbeOptions.parse(Array(CommandLine.arguments.dropFirst())) {
            case .help: print(ProbeOptions.usage)
            case .check: try ProbeChecks.run()
            case .live(let options): exit(await ReadOnlyGatewayProbe.run(options))
            }
        } catch {
            FileHandle.standardError.write(Data("Probe input rejected. Use --help; no live connection was attempted.\n".utf8))
            exit(2)
        }
    }
}
