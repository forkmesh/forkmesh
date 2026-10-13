// swift-tools-version:5.9
import Foundation
import PackageDescription

// Everything in the iOS app that is not a SwiftUI view lives here, so it
// builds and tests on Linux as well as on a Mac (see ../README.md).
//
// One module per area. `deps` lists only other modules in this package; there
// are no package dependencies (first-party only, as in native/android).
struct Module {
    let name: String
    let deps: [String]
    var resources: Bool = false
}

let modules: [Module] = [
    // Ordered JSON, encodings, formatting, storage protocols, app config.
    Module(name: "FMBase", deps: []),
    // Primitives (Ed25519, X25519, ML-KEM-768, AEADs, KDFs) and the ForkMesh
    // protocols built on them: room seal, owner crypto, identity.
    Module(name: "FMCrypto", deps: ["FMBase"]),
    // HTTP and WebSocket clients behind protocols tests can fake.
    Module(name: "FMNet", deps: ["FMBase"]),
    // Git objects, packs, refs, the index and the smart HTTP protocol.
    Module(name: "FMGit", deps: ["CZlib", "CSha1DC"], resources: true),
    // Unified diffs, syntax highlighting, patch building: text in, values out.
    Module(name: "FMDiff", deps: []),
    // The password vault's fmv1 suite and client.
    Module(name: "FMVault", deps: ["FMBase", "FMCrypto", "FMNet", "CZlib"]),
    // Settings, auth and the session, trust lists, sync status, stall log and
    // the performance monitor: what ApiService itself is built on.
    Module(name: "FMAccount", deps: ["FMBase", "FMCrypto", "FMNet"]),
    // Models, ApiService, inbox, notifications, account events, user status.
    Module(name: "FMApi", deps: ["FMBase", "FMCrypto", "FMNet", "FMAccount"]),
    // Relay, rooms, local mesh, node sync.
    Module(name: "FMChat", deps: ["FMBase", "FMCrypto", "FMNet", "FMAccount", "FMApi", "FMLocalAI"]),
    // Repositories, diffs, highlighting, collaboration, on-device Git, and the
    // node-sync adapter over the on-device repositories (hence FMChat).
    Module(name: "FMCode", deps: ["FMBase", "FMCrypto", "FMNet", "FMAccount", "FMApi", "FMChat", "FMGit", "FMDiff", "FMControl"]),
    // On-device models (Gemma 4 through MLX in the app target): the model
    // catalog, weight downloads, device fit, and the engine protocol the app
    // implements. No MLX here, so it builds and tests on Linux.
    Module(name: "FMLocalAI", deps: ["FMBase", "FMNet"]),
    // Agent sessions, launch model, terminal, on-device coding agents.
    Module(name: "FMAgents", deps: ["FMBase", "FMCrypto", "FMNet", "FMAccount", "FMApi", "FMChat", "FMCode", "FMGit", "FMDiff", "FMLocalAI", "FMAgentWatch", "FMWatchWire", "FMControl"]),
    // Activity, telemetry, notes, tasks, calories, vault, bug reports.
    Module(name: "FMTools", deps: ["FMBase", "FMCrypto", "FMNet", "FMAccount", "FMApi", "FMVault"]),
    // The Listen sonifier and the Watch field's model (WS4): motions, voices,
    // the marimba synth, haptic patterns and the controller. FMBase only, so
    // it builds for watchOS as well as iOS and Linux.
    Module(name: "FMAgentWatch", deps: ["FMBase"]),
    // The phone-to-watch frames and commands (docs/native/PARITY.md, section
    // 4): shared by the app's WatchBridge and the watchOS app, so it builds
    // for watchOS: beats travel as FMAgentWatch motions.
    Module(name: "FMWatchWire", deps: ["FMBase", "FMAgentWatch"]),
    // The owner's desktop from anywhere (WS7): one control link over the LAN or through the relay,
    // and every agent the owner can steer (desktop and node runs) in one list.
    Module(name: "FMControl", deps: ["FMBase", "FMCrypto", "FMNet", "FMAccount", "FMApi", "FMLocalAI", "FMChat"]),
    // Host management and live host logs through the desktop (WS7).
    Module(name: "FMHosts", deps: ["FMBase", "FMChat", "FMControl"]),
    // Gate 2 (docs/native/DESKTOP_PARITY.md §19): one module per new workstream area. Each is owned
    // by its workstream; only the coordinator edits this list.
    // Widgets, Live Activities, App Intents and Spotlight: snapshot models (WS12).
    Module(name: "FMSurfaces", deps: ["FMBase", "FMApi", "FMPlanning"]),
    // Discord bridge and other desktop-side chat over the `discord.` family (WS13).
    Module(name: "FMDiscord", deps: ["FMBase", "FMChat", "FMControl"]),
    // Calendar, timers, projects and milestones (WS15).
    Module(name: "FMPlanning", deps: ["FMBase", "FMNet", "FMAccount", "FMApi", "FMTools"]),
    // Host operations, desktop files, logs and health over `fleet.`, `files.browse.`, `diag.` (WS16).
    Module(name: "FMDesktopRemote", deps: ["FMBase", "FMChat", "FMControl", "FMHosts"]),
    // Teammates over `teammates.` (WS17).
    Module(name: "FMTeammates", deps: ["FMBase", "FMApi", "FMChat", "FMControl"]),
    // The Market and the business packages: CRM, marketing, analytics, ledger (WS19).
    Module(name: "FMPackages", deps: ["FMBase", "FMCrypto", "FMNet", "FMAccount", "FMApi"]),
    // Stats, health, logs and the admin console (WS20).
    Module(name: "FMInsights", deps: ["FMBase", "FMNet", "FMAccount", "FMApi", "FMControl"]),
    // Coves (encrypted file vaults) and secrets (WS21).
    Module(name: "FMCoves", deps: ["FMBase", "FMCrypto", "FMNet", "FMAccount", "FMApi"]),
    Module(name: "FMAssistant", deps: ["FMBase", "FMCrypto", "FMNet", "FMAccount", "FMApi", "FMChat", "FMControl", "FMPackages", "FMPlanning", "FMLocalAI"]),
]

// What the watchOS app links (docs/native/PARITY.md, section 4.4): only
// modules that build on watchOS.
let watchModules = ["FMBase", "FMAgentWatch", "FMWatchWire"]

// FM_ONLY=FMCrypto limits the package to that module, what it depends on and
// its tests, so work on one module is never blocked by another that does not
// compile yet. Several names may be given, comma separated.
let only = (ProcessInfo.processInfo.environment["FM_ONLY"] ?? "")
    .split(separator: ",").map { $0.trimmingCharacters(in: .whitespaces) }.filter { !$0.isEmpty }

func closure(_ names: [String]) -> Set<String> {
    var seen = Set<String>()
    var queue = names
    while let next = queue.popLast() {
        guard seen.insert(next).inserted else { continue }
        queue.append(contentsOf: modules.first { $0.name == next }?.deps ?? [])
    }
    return seen
}

let wanted = only.isEmpty ? Set(modules.map(\.name)) : closure(only)
let tested = only.isEmpty ? wanted : Set(only)
let selected = modules.filter { wanted.contains($0.name) }

var targets: [Target] = [
    .systemLibrary(name: "CZlib", path: "Sources/CZlib"),
    .target(name: "CSha1DC", path: "Sources/CSha1DC", exclude: ["LICENSE.txt"]),
]
for module in selected {
    targets.append(.target(
        name: module.name,
        dependencies: module.deps.map { .byName(name: $0) },
        path: "Sources/\(module.name)"
    ))
    if tested.contains(module.name) {
        targets.append(.testTarget(
            name: "\(module.name)Tests",
            dependencies: [.byName(name: module.name)],
            path: "Tests/\(module.name)Tests",
            resources: module.resources ? [.copy("Fixtures")] : []
        ))
    }
}

var products: [Product] = [
    .library(name: "ForkMeshCore", targets: selected.map(\.name)),
]
let watchSelected = watchModules.filter { wanted.contains($0) }
if !watchSelected.isEmpty {
    products.append(.library(name: "ForkMeshWatchCore", targets: watchSelected))
}

let package = Package(
    name: "ForkMeshCore",
    platforms: [.iOS(.v17), .macOS("15.0"), .watchOS(.v10)],
    products: products,
    targets: targets
)
