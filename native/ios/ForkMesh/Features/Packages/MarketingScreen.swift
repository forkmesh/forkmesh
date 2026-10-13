import FMApi
import FMPackages
import SwiftUI

struct MarketingDestination: View {
    let section: String?

    var body: some View {
        PackagesGate(feature: "Marketing") { client in
            MarketingScreen(client: client, section: MarketingSection(route: section))
        }
    }
}

enum MarketingSheet: Identifiable {
    case form(MarketingDraft)
    case campaign(String)
    case channel(String)
    case assistant(String)
    case report
    case link
    case importKeywords
    case addChannels
    case settings

    var id: String {
        switch self {
        case .form(let draft): return "form-\(draft.kind.rawValue)-\(draft.id)"
        case .campaign(let id): return "campaign-" + id
        case .channel(let id): return "channel-" + id
        case .assistant(let id): return "assistant-" + id
        case .report: return "report"
        case .link: return "link"
        case .importKeywords: return "import"
        case .addChannels: return "add-channels"
        case .settings: return "settings"
        }
    }
}

struct MarketingScreen: View {
    @State private var model: MarketingModel
    @State private var section: MarketingSection
    @State private var sheet: MarketingSheet?
    @State private var notice: FmNotice?
    @State private var visible = false

    init(client: PackagesClient, section: MarketingSection) {
        _model = State(initialValue: MarketingModel(client: client))
        _section = State(initialValue: section)
    }

    var body: some View {
        List {
            Section {
                OrganizationPicker(organizations: model.packages.organizations, selection: Bindable(model.packages).organization) {
                    await model.reload()
                }
                MarketingSectionChips(section: $section)
                PackagesErrorBanner(text: model.errorText) { await model.refresh() }
                if notice != nil {
                    FmNoticeLine($notice)
                }
            }
            if let overview = model.overview {
                MarketingSectionBody(model: model, overview: overview, section: section, present: { sheet = $0 },
                                     select: { section = $0 })
            } else if !model.packages.loading && model.packages.installedOrganizations.isEmpty {
                PackageNotInstalledView(package: "Marketing", organizations: model.packages.organizations)
            }
        }
        .navigationTitle("Marketing")
        .toolbar {
            if model.overview != nil, let add = addAction {
                ToolbarItem(placement: .primaryAction) {
                    Button(add.title, systemImage: "plus") { sheet = add.sheet }
                }
            }
        }
        .overlay {
            if model.packages.loading && model.overview == nil { ProgressView() }
        }
        .fmRefreshable { await model.reload() }
        .task { await model.refresh() }
        .sheet(item: $sheet) { item in
            NavigationStack {
                MarketingSheetHost(model: model, sheet: item, present: { next in
                    sheet = nil
                    if let next {
                        Task { @MainActor in
                            try? await Task.sleep(for: .milliseconds(350))
                            sheet = next
                        }
                    }
                })
            }
        }
        .onAppear { visible = true }
        .onDisappear { visible = false }
        .onChange(of: model.notice) { _, text in
            guard visible, !text.isEmpty else { return }
            notice = FmNotice(text)
            model.notice = ""
        }
    }

    private var addAction: (title: String, sheet: MarketingSheet)? {
        let kind: MarketingKind
        switch section {
        case .overview, .plan: kind = .content
        case .campaigns: kind = .campaign
        case .channels: kind = .post
        case .ads: kind = .ad
        case .growth: kind = .experiment
        case .reddit: kind = .redditThread
        case .performance: return nil
        }
        guard let draft = model.newDraft(kind) else { return nil }
        return ("New " + kind.title.lowercased(), .form(draft))
    }
}

struct MarketingSectionChips: View {
    @Binding var section: MarketingSection

    var body: some View {
        ScrollView(.horizontal, showsIndicators: false) {
            HStack(spacing: 8) {
                ForEach(MarketingSection.allCases, id: \.self) { item in
                    Button {
                        section = item
                        FmHaptics.selection()
                    } label: {
                        Label(item.title, systemImage: item.symbol)
                            .font(.subheadline.weight(section == item ? .semibold : .regular))
                            .padding(.horizontal, 12)
                            .padding(.vertical, 6)
                            .background(section == item ? Color.accentColor.opacity(0.18) : Color.secondary.opacity(0.1), in: Capsule())
                    }
                    .buttonStyle(.plain)
                    .accessibilityAddTraits(section == item ? .isSelected : [])
                }
            }
            .padding(.vertical, 2)
        }
        .listRowInsets(EdgeInsets(top: 6, leading: 12, bottom: 6, trailing: 12))
    }
}

struct MarketingSheetHost: View {
    let model: MarketingModel
    let sheet: MarketingSheet
    let present: (MarketingSheet?) -> Void

    var body: some View {
        switch sheet {
        case .form(let draft):
            MarketingFormSheet(model: model, draft: draft, present: present)
        case .campaign(let id):
            MarketingCampaignSheet(model: model, campaignId: id, present: present)
        case .channel(let id):
            MarketingChannelSheet(model: model, channel: id, present: present)
        case .assistant(let campaignId):
            MarketingAssistantSheet(model: model, campaignId: campaignId, present: present)
        case .report:
            MarketingReportSheet(model: model)
        case .link:
            MarketingLinkSheet(model: model)
        case .importKeywords:
            MarketingKeywordImportSheet(model: model)
        case .addChannels:
            MarketingAddChannelsSheet(model: model)
        case .settings:
            MarketingSettingsSheet(model: model, present: present)
        }
    }
}
