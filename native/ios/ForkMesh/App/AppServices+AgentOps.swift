// WS10 (agents over desktop control: approvals, merge & clean up, bulk actions, usage): this workstream's startup hook, called by
// AppServices.installAgentOps() (docs/native/DESKTOP_PARITY.md section 19). Only WS10
// edits this file. Gate 2 creates it empty.
//
// Register destinations here, e.g.
//     services.routes.register(.calendar, by: "WS10") { route in AnyView(CalendarScreen(route: route)) }

import FMApi
import FMBase
import SwiftUI

@MainActor
struct AgentOpsInstaller: AppServicesInstaller {
    func install(into services: AppServices) {
        AgentOpsFeature.register()
        _ = Services.put(AgentOpsApprovalDecider(), as: (any AgentApprovalDeciding).self)
        services.routes.register(.approval, by: "WS10") { route in
            guard case .approval(let id) = route else { return AnyView(EmptyView()) }
            return AnyView(ApprovalDestination(approvalId: id))
        }
        services.routes.register(.agent, by: "WS10") { route in
            guard case .agent(let id) = route else { return AnyView(EmptyView()) }
            return AnyView(AgentRouteDestination(id: id))
        }
        services.routes.register(.agents, by: "WS10") { route in
            guard case .agents(let filter) = route else { return AnyView(AgentsScreen()) }
            return AnyView(AgentsRouteDestination(filter: filter))
        }
    }
}
