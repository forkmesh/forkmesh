import FMAccount
import FMApi
import FMBase
import SwiftUI

enum ShellSidebarItem: Hashable {
    case tab(AppTab)
    case route(Route)
}

struct ShellSidebar: View {
    @Environment(\.fmPalette) private var fm
    let router: AppRouter
    let badges: [AppTab: Int]
    var activeTabs: Set<AppTab> = []
    var activeHub: Set<String> = []
    let guest: Bool
    let onSelectTab: (AppTab) -> Void
    let onOpen: (Route) -> Void

    private var groups: [HubGroup] {
        HubGroups.current(guest: guest)
            .filter { $0.id != "app" }
    }

    private var tabs: [AppTab] {
        let installed: Set<String> = PackageGateSync.publish()
        return AppTab.barOrder.filter { PackageGate.allows(PackageGate.packageId(for: $0), installed: installed) }
    }

    private var selection: Binding<ShellSidebarItem?> {
        Binding<ShellSidebarItem?>(
            get: { .tab(router.selectedTab) },
            set: { (item: ShellSidebarItem?) in
                switch item {
                case let .tab(tab)?:
                    onSelectTab(tab)
                case let .route(route)?:
                    onOpen(route)
                case nil:
                    break
                }
            })
    }

    private func row(_ title: String, symbol: String, active: Bool) -> some View {
        Label {
            Text(title)
        } icon: {
            Image(systemName: symbol)
                .fmBusyRing(active, size: 28)
        }
    }

    var body: some View {
        List(selection: selection) {
            Section {
                ForEach(tabs, id: \.self) { (tab: AppTab) in
                    row(tab.title, symbol: tab.symbol, active: activeTabs.contains(tab))
                        .badge(badges[tab] ?? 0)
                        .tag(ShellSidebarItem.tab(tab))
                        .accessibilityIdentifier("sidebar-\(tab.rawValue)")
                }
            }
            ForEach(groups) { (group: HubGroup) in
                Section(group.title) {
                    ForEach(group.items) { (item: HubItem) in
                        if let route = item.route {
                            row(item.title, symbol: item.symbol, active: activeHub.contains(item.id))
                                .tag(ShellSidebarItem.route(route))
                                .accessibilityIdentifier("sidebar-\(item.id)")
                        }
                    }
                }
            }
            Section {
                Label("Search", systemImage: "magnifyingglass")
                    .tag(ShellSidebarItem.route(.search(query: "", scope: .everything)))
                Label("Settings", systemImage: "gearshape")
                    .tag(ShellSidebarItem.route(.settings(section: nil)))
            }
        }
        .listStyle(.sidebar)
        .navigationTitle(Services.get(SettingsService.self)?.project.label ?? "ForkMesh")
        .toolbar {
            ToolbarItem(placement: .topBarLeading) {
                ProjectSwitcherButton(compact: true)
                    .accessibilityIdentifier("sidebar-project-switcher")
            }
        }
    }
}
