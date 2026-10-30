// swift-tools-version:5.9
import PackageDescription

let package = Package(
    name: "FmmPackage",
    platforms: [.iOS(.v17)],
    products: [.library(name: "Fmm", targets: ["Fmm", "FmmLink"])],
    targets: [
        .binaryTarget(name: "Fmm", path: "Fmm.xcframework"),
        .target(name: "FmmLink", dependencies: ["Fmm"], path: "Link", linkerSettings: [.linkedLibrary("resolv")]),
    ]
)
