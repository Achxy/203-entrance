// swift-tools-version: 6.0
import PackageDescription

let package = Package(
    name: "LightsCore",
    platforms: [.macOS(.v14)],
    products: [
        .library(name: "LightsCore", targets: ["LightsCore"]),
        .executable(name: "LightsCoreChecks", targets: ["LightsCoreChecks"])
    ],
    targets: [
        .target(name: "LightsCore"),
        .executableTarget(name: "LightsCoreChecks", dependencies: ["LightsCore"], path: "Tests/LightsCoreTests")
    ]
)
