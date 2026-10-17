// swift-tools-version:6.0
import PackageDescription

struct Module {
    let name: String
    let deps: [String]
    var appleOnly = false
    var tested = true
}

let modules: [Module] = [
    Module(name: "NexusCore", deps: []),
    Module(name: "NexusDNS", deps: ["NexusCore"]),
    Module(name: "NexusWeb", deps: ["NexusCore"]),
    Module(name: "NexusConnectivity", deps: ["NexusCore", "NexusDNS"]),
    Module(name: "NexusScan", deps: ["NexusCore", "NexusDNS"]),
    Module(name: "NexusDiscovery", deps: ["NexusCore", "NexusDNS", "NexusConnectivity"]),
    Module(name: "NexusRemote", deps: ["NexusCore"]),
    Module(name: "NexusKit", deps: ["NexusCore", "NexusDNS", "NexusWeb", "NexusConnectivity", "NexusScan", "NexusDiscovery", "NexusRemote"]),
    Module(name: "NexusStore", deps: ["NexusCore", "NexusKit"], appleOnly: true),
    Module(name: "NexusUI", deps: ["NexusCore", "NexusKit", "NexusStore", "NexusWeb"], appleOnly: true, tested: false),
]

#if os(Linux) || os(Windows)
let available = modules.filter { !$0.appleOnly }
#else
let available = modules
#endif

var targets: [Target] = []
for module in available {
    targets.append(.target(
        name: module.name,
        dependencies: module.deps.map { .byName(name: $0) },
        path: "Sources/\(module.name)"
    ))
    if module.tested {
        targets.append(.testTarget(
            name: "\(module.name)Tests",
            dependencies: [.byName(name: module.name), "NexusTestSupport"] + module.deps.map { .byName(name: $0) },
            path: "Tests/\(module.name)Tests"
        ))
    }
}
targets.append(.target(name: "NexusTestSupport", dependencies: ["NexusCore"], path: "Tests/NexusTestSupport"))

var products: [Product] = [
    .library(name: "NexusKit", targets: available.filter { !$0.appleOnly }.map(\.name)),
]
if available.contains(where: { $0.name == "NexusUI" }) {
    products.append(.library(name: "NexusUI", targets: available.map(\.name)))
}

let package = Package(
    name: "Nexus",
    platforms: [.iOS(.v17), .macOS(.v14)],
    products: products,
    targets: targets
)
