// swift-tools-version: 6.1
import PackageDescription

let package = Package(
    name: "LightsSecurity",
    platforms: [.macOS(.v14)],
    products: [.library(name: "LightsSecurity", targets: ["LightsSecurity"]),
               .executable(name: "LightsSecurityChecks", targets: ["LightsSecurityChecks"])],
    dependencies: [
        .package(url: "https://github.com/apple/swift-protobuf.git", exact: "1.39.0")
    ],
    targets: [
        .target(name: "EspressifSRP"),
        .target(name: "LightsSecurity", dependencies: [
            "EspressifSRP", .product(name: "SwiftProtobuf", package: "swift-protobuf")
        ]),
        .executableTarget(name: "LightsSecurityChecks", dependencies: ["LightsSecurity", "EspressifSRP"],
                          path: "Tests/LightsSecurityTests", resources: [.copy("Fixtures")])
    ],
    swiftLanguageModes: [.v5]
)
