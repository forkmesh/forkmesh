import AppIntents
import FMApi
import SwiftUI
import WidgetKit

@available(iOS 18.0, *)
struct NewAgentControl: ControlWidget {
    var body: some ControlWidgetConfiguration {
        StaticControlConfiguration(kind: "com.forkmesh.control.new-agent") {
            ControlWidgetButton(action: OpenSurfaceRouteIntent(route: .compose(ComposeTarget()))) {
                Label("New agent", systemImage: "sparkles")
            }
        }
        .displayName("New agent")
        .description("Opens the ForkMesh composer.")
    }
}

@available(iOS 18.0, *)
struct ApprovalsControl: ControlWidget {
    var body: some ControlWidgetConfiguration {
        StaticControlConfiguration(kind: "com.forkmesh.control.approvals") {
            ControlWidgetButton(action: OpenSurfaceRouteIntent(route: .inbox(.approvals))) {
                Label("Approvals", systemImage: "hand.raised")
            }
        }
        .displayName("Approvals")
        .description("Opens the requests your agents are waiting on.")
    }
}
