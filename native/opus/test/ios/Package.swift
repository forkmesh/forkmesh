// swift-tools-version:5.9
import PackageDescription

let package = Package(
    name: "OpusSmoke",
    platforms: [.iOS(.v17)],
    targets: [
        .binaryTarget(name: "Opus", path: "Opus.xcframework"),
        .executableTarget(name: "OpusSmoke", dependencies: ["Opus"]),
    ]
)
