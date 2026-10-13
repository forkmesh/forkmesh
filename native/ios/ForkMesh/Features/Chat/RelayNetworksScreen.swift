// Every relay's repositories, side by side.
// The one deliberate exception to "one relay at a time": it answers "what is on my other
// network?" without moving the app there; opening a repository switches to its relay first.

import FMAccount
import FMApi
import FMBase
import FMChat
import FMNet
import SwiftUI

struct RelayNetworksScreen: View {
    @State private var model: RelayNetworksModel? = RelayNetworksScreen.makeModel()

    init() {}

    @MainActor
    static func makeModel() -> RelayNetworksModel? {
        guard let settings = Services.get(SettingsService.self), let auth = Services.get(AuthService.self) else {
            return nil
        }
        let http: HTTPClient = Services.get(HTTPClient.self) ?? URLSessionHTTPClient()
        let catalog = CrossRelayCatalog(settings: settings, auth: auth, http: http)
        return RelayNetworksModel(catalog: catalog, settings: settings, switcher: Services.get(RelaySwitchController.self))
    }

    var body: some View {
        if let model = model {
            RelayNetworksBody(model: model)
        } else {
            FmEmptyState(icon: "hub_outlined", title: "All networks")
        }
    }
}

private struct RelayNetworksBody: View {
    @Environment(\.fmPalette) private var fm
    let model: RelayNetworksModel
    @State private var adding: Bool = false
    @State private var opened: Repository?

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 0) {
                Text(model.summary)
                    .fmText(.bodySmall, color: fm.textSecondary)
                    .padding(.bottom, FmSpace.x4)
                ForEach(model.catalog.entries) { (entry: RelayCatalogEntry) in
                    RelaySection(
                        entry: entry,
                        onOpen: { (repo: Repository) in open(repo, on: entry.relay) },
                        onSwitch: {
                            FmHaptics.tap()
                            Task { await model.switchTo(entry.relay) }
                        })
                        .padding(.bottom, FmSpace.x5)
                }
            }
            .padding(FmSpace.x4)
        }
        .background(fm.bgBase)
        .fmRefreshable { await model.refresh() }
        .navigationTitle("All networks")
        .toolbar {
            ToolbarItem(placement: .topBarTrailing) {
                Button(action: { adding = true }) {
                    FmIcon("add", color: fm.textPrimary)
                }
                .accessibilityLabel("Add relay")
            }
        }
        .sheet(isPresented: $adding) {
            AddRelaySheet(onAdded: { (_: RelayProfile) in
                adding = false
                Task { await model.refresh() }
            })
        }
        .navigationDestination(item: $opened) { (repo: Repository) in
            RepoDetailScreen(repo: repo)
                .shellPageClearance()
        }
        .task { await model.refresh() }
    }

    private func open(_ repo: Repository, on relay: RelayProfile) {
        FmHaptics.tap()
        Task {
            // The repository screens read the active relay, which is now this one.
            if await model.open(on: relay) {
                opened = repo
            }
        }
    }
}

private struct RelaySection: View {
    @Environment(\.fmPalette) private var fm
    let entry: RelayCatalogEntry
    let onOpen: (Repository) -> Void
    let onSwitch: () -> Void

    var body: some View {
        VStack(alignment: .leading, spacing: FmSpace.x2) {
            HStack(spacing: FmSpace.x2) {
                FmSectionHeader(
                    title: RelayNetworksModel.sectionTitle(entry),
                    icon: entry.active ? "radio_button_checked" : "radio_button_off")
                if !entry.active {
                    FmToneButton(label: "Switch", tone: .accent, onPressed: onSwitch)
                }
            }
            FmCard(radius: FmRadius.lg) {
                sectionBody
            }
        }
    }

    @ViewBuilder private var sectionBody: some View {
        if !entry.signedIn {
            SectionNote(icon: "lock_outline", message: RelayNetworksModel.signedOutNote(entry.relay), danger: false)
        } else if entry.loading {
            FmSkeletonList(rows: 3)
        } else if !entry.error.isEmpty {
            SectionNote(icon: "error_outline", message: entry.error, danger: true)
        } else if entry.repositories.isEmpty {
            SectionNote(icon: "inbox_outlined", message: "No repositories on this relay yet.", danger: false)
        } else {
            VStack(alignment: .leading, spacing: 0) {
                ForEach(Array(entry.repositories.enumerated()), id: \.offset) { (item: (offset: Int, element: Repository)) in
                    if item.offset > 0 {
                        FmRowDivider()
                    }
                    FmListRow(
                        title: item.element.name, subtitle: item.element.owner,
                        leading: AnyView(FmIcon("folder_outlined", color: fm.textSecondary)),
                        onTap: { onOpen(item.element) })
                }
            }
        }
    }
}

private struct SectionNote: View {
    @Environment(\.fmPalette) private var fm
    let icon: String
    let message: String
    let danger: Bool

    var body: some View {
        let color: Color = danger ? fm.danger : fm.textSecondary
        HStack(alignment: .top, spacing: FmSpace.x3) {
            FmIcon(icon, size: 18, color: color)
            Text(message)
                .fmText(.bodySmall, color: color, height: 1.35)
                .frame(maxWidth: .infinity, alignment: .leading)
                .fmMessageTools(message)
        }
        .padding(FmSpace.x3)
    }
}
