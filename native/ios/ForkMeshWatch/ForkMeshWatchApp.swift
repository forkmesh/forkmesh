// The ForkMesh watch app (docs/native/PARITY.md, section 4): control the
// account's agents, and see and hear them work. The iPhone app is the only
// ForkMesh client; everything here arrives over WatchConnectivity
// (WatchStore) and goes back as commands.

import FMWatchWire
import SwiftUI

@main
struct ForkMeshWatchApp: App {
    @State private var path: [String] = []

    init() {
        WatchStore.shared.activate()
    }

    var body: some Scene {
        WindowGroup {
            NavigationStack(path: $path) {
                AgentListView()
                    .navigationDestination(for: String.self) { (key: String) in
                        AgentLiveView(key: key)
                    }
            }
            // A complication or the Smart Stack opens an agent's live view.
            .onOpenURL { (url: URL) in
                if let key = WatchGroup.agentKey(from: url) {
                    path = [key]
                }
            }
            .environment(WatchStore.shared)
        }
    }
}
