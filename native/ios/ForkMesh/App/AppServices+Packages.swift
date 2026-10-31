// WS19 (the Market and the business packages): this workstream's startup hook, called by
// AppServices.installPackages() (docs/native/DESKTOP_PARITY.md section 19). Only WS19
// edits this file. Gate 2 creates it empty.
//
// Register destinations here, e.g.
//     services.routes.register(.calendar, by: "WS19") { route in AnyView(CalendarScreen(route: route)) }

import FMApi
import FMPackages
import SwiftUI

@MainActor
struct PackagesInstaller: AppServicesInstaller {
    func install(into services: AppServices) {
        services.routes.register(.market, by: "WS19") { route in
            guard case .market(let filter) = route else { return AnyView(MarketDestination(filter: nil)) }
            return AnyView(MarketDestination(filter: filter))
        }
        services.routes.register(.package, by: "WS19") { route in
            guard case .package(let id) = route else { return AnyView(EmptyView()) }
            return AnyView(PackageDestination(id: id))
        }
        services.routes.register(.crm, by: "WS19") { route in
            guard case .crm(let pipeline) = route else { return AnyView(CrmDestination(organization: nil, flow: nil)) }
            let target = PackageRoutes.organization(fromPipeline: pipeline)
            return AnyView(CrmDestination(organization: target.organization, flow: target.flow))
        }
        services.routes.register(.contact, by: "WS19") { route in
            guard case .contact(let id) = route else { return AnyView(EmptyView()) }
            return AnyView(CrmContactDestination(routeId: id))
        }
        services.routes.register(.crmOrg, by: "WS19") { route in
            guard case .crmOrg(let id) = route else { return AnyView(EmptyView()) }
            return AnyView(CrmOrgDestination(organization: id))
        }
        services.routes.register(.marketing, by: "WS19") { route in
            guard case .marketing(let section) = route else { return AnyView(MarketingDestination(section: nil)) }
            return AnyView(MarketingDestination(section: section))
        }
        services.routes.register(.analytics, by: "WS19") { route in
            guard case .analytics(let section) = route else { return AnyView(AnalyticsDestination(section: nil)) }
            return AnyView(AnalyticsDestination(section: section))
        }
        services.routes.register(.competitors, by: "WS19") { _ in AnyView(CompetitorsDestination()) }
        services.routes.register(.invoices, by: "WS19") { route in
            guard case .invoices(let organization, let invoice) = route else {
                return AnyView(InvoicesDestination(organization: nil, invoice: nil))
            }
            return AnyView(InvoicesDestination(organization: organization, invoice: invoice))
        }
        services.routes.register(.ads, by: "WS19") { _ in AnyView(AdsDestination()) }
        services.routes.register(.ledger, by: "WS19") { route in
            guard case .ledger(let section) = route else { return AnyView(LedgerDestination(section: nil)) }
            return AnyView(LedgerDestination(section: section))
        }
    }
}
