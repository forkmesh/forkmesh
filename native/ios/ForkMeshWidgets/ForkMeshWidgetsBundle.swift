import SwiftUI
import WidgetKit

@main
struct ForkMeshWidgetsBundle: WidgetBundle {
    var body: some Widget {
        OverviewWidget()
        AgentsWidget()
        InboxWidget()
        TimerWidget()
        SummaryLiveActivity()
        WalkLiveActivity()
        if #available(iOS 18.0, *) {
            NewAgentControl()
            ApprovalsControl()
        }
    }
}
