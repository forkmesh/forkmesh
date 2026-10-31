// swift-tools-version:5.9
import PackageDescription

let package = Package(
    name: "OpusPackage",
    platforms: [.iOS(.v17)],
    products: [.library(name: "Opus", targets: ["Opus"])],
    targets: [.binaryTarget(name: "Opus", path: "Opus.xcframework")]
)
