// swift-tools-version: 6.0
import PackageDescription

let package = Package(
    name: "203Lights",
    platforms: [.macOS(.v14)],
    products: [.executable(name: "203Lights", targets: ["LightsApp"]),
               .executable(name: "LightsMenuletChecks", targets: ["LightsMenuletChecks"]),
               .executable(name: "LightsGatewayProbe", targets: ["LightsGatewayProbe"]),
               .executable(name: "LightsMenuletSnapshots", targets: ["LightsMenuletSnapshots"])],
    dependencies: [.package(path: "../core"), .package(path: "../security")],
    targets: [
        .target(name: "LightsMenulet", dependencies: [.product(name: "LightsCore", package: "core"), .product(name: "LightsSecurity", package: "security")]),
        .executableTarget(name: "LightsApp", dependencies: ["LightsMenulet"]),
        .executableTarget(name: "LightsMenuletChecks", dependencies: ["LightsMenulet", .product(name: "LightsCore", package: "core"), .product(name: "LightsSecurity", package: "security")]),
        .executableTarget(name: "LightsGatewayProbe", dependencies: ["LightsMenulet", .product(name: "LightsCore", package: "core"), .product(name: "LightsSecurity", package: "security")]),
        .executableTarget(name: "LightsMenuletSnapshots", dependencies: ["LightsMenulet"])
    ]
)
