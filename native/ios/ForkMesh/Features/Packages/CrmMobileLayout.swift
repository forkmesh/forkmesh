import FMAccount
import FMApi
import FMPackages
import SwiftUI

@MainActor
enum CrmLayout {
    static var settings: SettingsService? { AppServices.current?.settings }

    static var mode: LayoutMode { settings?.layoutMode ?? .controlTower }

    static var dense: Bool { mode == .controlTower }
}

struct CrmSectionBar: View {
    @Bindable var model: CrmModel
    let data: CrmOrganizationData

    var body: some View {
        HStack(spacing: 2) {
            ForEach(CrmSection.allCases) { section in item(section) }
        }
        .padding(3)
        .shellGlass(Capsule())
        .padding(.horizontal)
    }

    private func badge(_ section: CrmSection) -> Int? {
        switch section {
        case .contacts: return data.contacts.isEmpty ? nil : data.contacts.count
        default: return nil
        }
    }

    private func item(_ section: CrmSection) -> some View {
        let active = model.section == section
        return Button {
            FmHaptics.selection()
            withAnimation(.snappy(duration: 0.2)) { model.show(section) }
        } label: {
            VStack(spacing: 1) {
                Image(systemName: section.icon)
                    .symbolVariant(active ? .fill : .none)
                    .font(.system(size: 15, weight: active ? .semibold : .regular))
                    .frame(height: 18)
                Text(section.title).font(.system(size: 10, weight: active ? .semibold : .medium)).lineLimit(1)
            }
            .frame(maxWidth: .infinity, minHeight: 40)
            .foregroundStyle(active ? Color.white : Color.primary.opacity(0.75))
            .background(active ? AnyShapeStyle(Color.accentColor) : AnyShapeStyle(Color.clear), in: Capsule())
            .contentShape(Capsule())
        }
        .buttonStyle(.plain)
        .accessibilityLabel(badge(section).map { "\(section.title), \($0)" } ?? section.title)
        .accessibilityAddTraits(active ? .isSelected : [])
        .accessibilityIdentifier("crm-section-\(section.rawValue)")
    }
}

struct CrmTabChips: View {
    @Bindable var model: CrmModel
    let items: [CrmTabItem]

    var body: some View {
        ScrollViewReader { proxy in
            ScrollView(.horizontal, showsIndicators: false) {
                HStack(spacing: 6) {
                    ForEach(items) { item in chip(item) }
                }
                .padding(.horizontal)
            }
            .onAppear { proxy.scrollTo(model.activeTab.id, anchor: .center) }
        }
    }

    private func chip(_ item: CrmTabItem) -> some View {
        let active = model.activeTab == item.tab
        return Button {
            FmHaptics.selection()
            model.select(item.tab)
        } label: {
            HStack(spacing: 4) {
                Image(systemName: item.icon).imageScale(.small)
                Text(item.title).lineLimit(1)
                if let count = item.count {
                    Text("\(count)").font(.caption2.monospacedDigit().weight(.bold)).opacity(0.75)
                }
            }
            .font(.footnote.weight(active ? .semibold : .regular))
            .padding(.horizontal, 10)
            .frame(height: 30)
            .foregroundStyle(active ? Color.accentColor : Color.primary)
            .background(active ? Color.accentColor.opacity(0.16) : Color.secondary.opacity(0.1), in: Capsule())
        }
        .buttonStyle(.plain)
        .id(item.tab.id)
        .accessibilityAddTraits(active ? .isSelected : [])
    }
}

struct CrmModePill: View {
    var body: some View {
        if let settings = CrmLayout.settings {
            Menu {
                Picker("Layout", selection: Binding(get: { settings.layoutMode }, set: { settings.setLayoutMode($0) })) {
                    ForEach(LayoutMode.allCases) { Label($0.label, systemImage: CrmModeSwitcher.icon($0)).tag($0) }
                }
                Picker("Theme", selection: Binding(get: { settings.themeMode }, set: { settings.setThemeMode($0) })) {
                    ForEach(ThemeMode.allCases, id: \.self) { Label(CrmModeSwitcher.title($0), systemImage: CrmModeSwitcher.icon($0)).tag($0) }
                }
            } label: {
                Label(settings.layoutMode.label, systemImage: CrmModeSwitcher.icon(settings.layoutMode))
            }
            .accessibilityIdentifier("crm-mode-menu")
        }
    }
}

struct CrmModeSwitcher: View {
    let settings: SettingsService

    static func icon(_ mode: LayoutMode) -> String {
        mode == .controlTower ? "square.grid.2x2" : "rectangle.portrait"
    }

    static func icon(_ theme: ThemeMode) -> String {
        switch theme {
        case .system: return "circle.lefthalf.filled"
        case .light: return "sun.max"
        case .dark: return "moon"
        }
    }

    static func title(_ theme: ThemeMode) -> String {
        switch theme {
        case .system: return "System"
        case .light: return "Light"
        case .dark: return "Dark"
        }
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            Picker("Layout", selection: Binding(get: { settings.layoutMode }, set: { mode in
                FmHaptics.selection()
                withAnimation(.snappy) { settings.setLayoutMode(mode) }
            })) {
                ForEach(LayoutMode.allCases) { Text($0.label).tag($0) }
            }
            .pickerStyle(.segmented)
            .accessibilityIdentifier("crm-layout-mode")
            Text(settings.layoutMode.note).font(.caption).foregroundStyle(.secondary)
            Picker("Theme", selection: Binding(get: { settings.themeMode }, set: { theme in
                FmHaptics.selection()
                settings.setThemeMode(theme)
            })) {
                ForEach(ThemeMode.allCases, id: \.self) { Text(Self.title($0)).tag($0) }
            }
            .pickerStyle(.segmented)
            .accessibilityIdentifier("crm-theme-mode")
        }
        .padding(.vertical, 4)
    }
}

struct CrmKpiTile: View {
    let value: String
    let label: String
    let icon: String
    let tint: Color
    var detail = ""
    var dense = true

    var body: some View {
        VStack(alignment: .leading, spacing: dense ? 2 : 4) {
            HStack(spacing: 5) {
                Image(systemName: icon).font(.caption2.weight(.semibold))
                Text(label).font(.caption.weight(.medium)).lineLimit(1)
            }
            .foregroundStyle(dense ? Color.white.opacity(0.9) : tint)
            Text(value)
                .font((dense ? Font.title3 : Font.title2).weight(.bold).monospacedDigit())
                .foregroundStyle(dense ? Color.white : Color.primary)
                .lineLimit(1)
                .minimumScaleFactor(0.6)
            if !detail.isEmpty {
                Text(detail).font(.caption2.weight(.medium)).lineLimit(1)
                    .foregroundStyle(dense ? Color.white.opacity(0.85) : Color.secondary)
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(.horizontal, 12)
        .padding(.vertical, dense ? 9 : 14)
        .background {
            RoundedRectangle(cornerRadius: 14, style: .continuous)
                .fill(dense ? AnyShapeStyle(tint.gradient) : AnyShapeStyle(Color(uiColor: .secondarySystemGroupedBackground)))
        }
        .accessibilityElement(children: .combine)
    }
}

struct CrmPipelineBar: View {
    @Bindable var model: CrmModel
    let flow: CrmFlow
    let contacts: [CrmContact]

    var body: some View {
        let stats = flow.stats(contacts)
        let total = max(1, contacts.count)
        VStack(alignment: .leading, spacing: 8) {
            HStack {
                Label(flow.name, systemImage: CrmFlowIcon.symbol(flow.icon)).font(.subheadline.weight(.semibold))
                Spacer()
                Text(CrmText.money(stats.reduce(0) { $0 + $1.value })).font(.subheadline.weight(.semibold).monospacedDigit())
            }
            GeometryReader { proxy in
                HStack(spacing: 2) {
                    ForEach(stats.filter { $0.count > 0 }) { stat in
                        Rectangle()
                            .fill(CrmPalette.stage(stat.stage, in: flow))
                            .frame(width: max(4, proxy.size.width * CGFloat(stat.count) / CGFloat(total) - 2))
                    }
                    Spacer(minLength: 0)
                }
            }
            .frame(height: 8)
            .clipShape(Capsule())
            .background(Color.secondary.opacity(0.12), in: Capsule())
            ScrollView(.horizontal, showsIndicators: false) {
                HStack(spacing: 10) {
                    ForEach(stats) { stat in
                        Button {
                            FmHaptics.selection()
                            if let tab = model.data?.tab(forFlow: flow.id) { model.select(tab) }
                            model.filter.stage = stat.stage == "other" ? "" : stat.stage
                            model.show(.contacts)
                        } label: {
                            HStack(spacing: 4) {
                                Circle().fill(CrmPalette.stage(stat.stage, in: flow)).frame(width: 7, height: 7)
                                Text(CrmStageLabel.title(stat.stage)).font(.caption2).foregroundStyle(.secondary)
                                Text("\(stat.count)").font(.caption2.weight(.bold).monospacedDigit())
                            }
                        }
                        .buttonStyle(.plain)
                    }
                }
            }
        }
        .crmCard()
    }
}

struct CrmHomePane: View {
    @Bindable var model: CrmModel
    let data: CrmOrganizationData
    let present: (CrmSheet) -> Void
    @Environment(\.openURL) private var openURL

    private var dense: Bool { CrmLayout.dense }

    private var userName: String {
        let name = AppServices.current?.auth.session?.nodeName ?? ""
        return name.isEmpty ? "" : ", " + name
    }

    var body: some View {
        let board = model.dashboard
        ScrollView {
            VStack(alignment: .leading, spacing: dense ? 10 : 16) {
                Text(CrmDashboard.greeting(hour: Calendar.current.component(.hour, from: Date())) + userName)
                    .font(dense ? Font.headline : Font.title3.weight(.semibold))
                    .lineLimit(1)
                CrmZohoAlert(model: model, data: data)
                if dense {
                    kpis(board)
                    pipeline
                    nextUp
                } else {
                    nextUp
                    kpis(board)
                    pipeline
                }
                recent
            }
            .padding(.horizontal)
            .padding(.vertical, 8)
        }
        .background(Color(uiColor: .systemGroupedBackground))
        .fmRefreshable { await model.load() }
    }

    private func kpis(_ board: CrmDashboard) -> some View {
        let columns = Array(repeating: GridItem(.flexible(), spacing: 8), count: 2)
        return LazyVGrid(columns: columns, spacing: 8) {
            CrmKpiTile(
                value: "\(board.contacts)", label: "Contacts", icon: "person.2.fill", tint: .blue,
                detail: board.newThisWeek > 0 ? "+\(board.newThisWeek) this week" : "", dense: dense)
                .onTapGesture { model.show(.contacts) }
            CrmKpiTile(
                value: CrmText.money(board.openValue), label: "Open deals", icon: "chart.line.uptrend.xyaxis", tint: .teal,
                detail: "\(board.openDeals) open", dense: dense)
                .onTapGesture { model.show(.pipeline) }
            CrmKpiTile(
                value: "\(board.quiet + board.neverContacted)", label: "To contact", icon: "bell.badge.fill", tint: .orange,
                detail: "\(board.neverContacted) new · \(board.quiet) quiet", dense: dense)
            CrmKpiTile(
                value: CrmText.money(board.wonValue), label: "Won", icon: "checkmark.seal.fill", tint: .indigo,
                detail: "\(board.wonDeals) won", dense: dense)
        }
    }

    @ViewBuilder private var pipeline: some View {
        if let flow = CrmDashboard.pipelineFlow(data, active: model.activeFlow) {
            CrmPipelineBar(model: model, flow: flow, contacts: data.contacts.filter { $0.flow == flow.id })
                .onTapGesture { model.show(.pipeline) }
        }
    }

    private var nextUp: some View {
        VStack(alignment: .leading, spacing: 6) {
            Text("Up next").font(.footnote.weight(.semibold)).foregroundStyle(.secondary).textCase(.uppercase)
            VStack(spacing: 0) {
                ForEach(CrmNextContact.Mode.allCases) { mode in
                    nextRow(mode)
                    if mode != CrmNextContact.Mode.allCases.last { Divider().padding(.leading, 46) }
                }
            }
            .crmCard()
        }
    }

    @ViewBuilder private func nextRow(_ mode: CrmNextContact.Mode) -> some View {
        let next = model.nextContact(mode, everyone: true)
        if let contact = next.contact {
            HStack(spacing: 10) {
                NavigationLink(value: Route.contact(id: contact.routeId)) {
                    HStack(spacing: 10) {
                        CrmAvatar(name: contact.name, email: contact.email, size: dense ? 34 : 40)
                        VStack(alignment: .leading, spacing: 1) {
                            HStack(spacing: 5) {
                                Text(contact.displayName).font(.subheadline.weight(.semibold)).lineLimit(1)
                                CrmChip(text: mode.title, tint: mode == .reachOut ? Color.blue : Color.orange)
                            }
                            Text(next.reason).font(.caption).foregroundStyle(.secondary).lineLimit(dense ? 1 : 2)
                        }
                        Spacer(minLength: 0)
                    }
                    .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
                reach(contact)
                Button {
                    Task { if await model.markContacted([contact.id]) { FmHaptics.commit() } }
                } label: {
                    Image(systemName: "checkmark").font(.footnote.weight(.bold)).frame(width: 32, height: 32)
                        .background(Color.green.opacity(0.15), in: Circle()).foregroundStyle(.green)
                }
                .buttonStyle(.plain)
                .accessibilityLabel("Mark \(contact.displayName) contacted")
            }
            .padding(.vertical, 6)
            .contextMenu {
                Button("Skip for now", systemImage: "forward") { model.nextSkipped.insert(contact.id) }
            }
        } else {
            HStack(spacing: 10) {
                Image(systemName: mode == .reachOut ? "person.badge.plus" : "arrowshape.turn.up.left")
                    .foregroundStyle(.secondary).frame(width: 34)
                Text(mode == .reachOut ? "No new leads to reach out to." : "Nobody due for a follow-up.")
                    .font(.footnote).foregroundStyle(.secondary)
                Spacer()
                if !model.nextSkipped.isEmpty {
                    Button("Show skipped") { model.nextSkipped.removeAll() }.font(.caption)
                }
            }
            .padding(.vertical, 8)
        }
    }

    @ViewBuilder private func reach(_ contact: CrmContact) -> some View {
        if contact.email.contains("@") {
            Button {
                present(.compose(CrmEmailDraft(contact: contact), outreach: false))
            } label: {
                Image(systemName: "envelope.fill").font(.footnote).frame(width: 32, height: 32)
                    .background(Color.accentColor.opacity(0.15), in: Circle())
            }
            .buttonStyle(.plain)
            .accessibilityLabel("Email \(contact.displayName)")
        } else if let url = CrmText.profileURL("mastodon", contact.mastodon) ?? CrmText.profileURL("twitter", contact.twitter) {
            Button {
                openURL(url)
            } label: {
                Image(systemName: "bubble.left.fill").font(.footnote).frame(width: 32, height: 32)
                    .background(Color.accentColor.opacity(0.15), in: Circle())
            }
            .buttonStyle(.plain)
            .accessibilityLabel("Message \(contact.displayName)")
        }
    }

    private var recent: some View {
        let items = Array(data.activity.sorted { $0.at > $1.at }.prefix(dense ? 5 : 3))
        return VStack(alignment: .leading, spacing: 6) {
            HStack {
                Text("Recent activity").font(.footnote.weight(.semibold)).foregroundStyle(.secondary).textCase(.uppercase)
                Spacer()
                Button("All") { model.show(.activity) }
                .font(.footnote)
            }
            VStack(spacing: 8) {
                if items.isEmpty {
                    Text("No emails yet.").font(.footnote).foregroundStyle(.secondary).frame(maxWidth: .infinity, alignment: .leading)
                }
                ForEach(items) { item in
                    Button {
                        present(.email(item))
                    } label: {
                        CrmActivityRow(activity: item, contact: data.contact(id: item.contactId))
                    }
                    .buttonStyle(.plain)
                }
            }
            .crmCard()
        }
    }
}

struct CrmZohoAlert: View {
    @Bindable var model: CrmModel
    let data: CrmOrganizationData

    var body: some View {
        let zoho = data.settings.zoho
        if zoho.connected, zoho.lastSyncError.isEmpty {
            EmptyView()
        } else {
            Button {
                model.show(.more)
            } label: {
                HStack(spacing: 8) {
                    Image(systemName: "envelope.badge").foregroundStyle(.orange)
                    Text(zoho.connected ? "Zoho Mail sync failed" : "Zoho Mail is not connected")
                        .font(.footnote.weight(.medium))
                    Spacer()
                    Image(systemName: "chevron.right").font(.caption2).foregroundStyle(.tertiary)
                }
                .padding(.horizontal, 12)
                .padding(.vertical, 8)
                .background(Color.orange.opacity(0.1), in: RoundedRectangle(cornerRadius: 12, style: .continuous))
            }
            .buttonStyle(.plain)
        }
    }
}

struct CrmPipelinePane: View {
    @Bindable var model: CrmModel
    let data: CrmOrganizationData
    let present: (CrmSheet) -> Void
    @State private var board: Bool?
    @State private var open: Set<String> = []

    private var pipelineTabs: [CrmTabItem] { data.contactTabs.filter { $0.flow != nil } }

    private var flow: CrmFlow? { model.activeFlow ?? pipelineTabs.first?.flow }

    var body: some View {
        let showBoard = board ?? CrmLayout.dense
        VStack(spacing: 0) {
            HStack(spacing: 8) {
                Menu {
                    ForEach(pipelineTabs) { item in
                        Button {
                            model.select(item.tab)
                        } label: {
                            Label("\(item.title) (\(item.count ?? 0))", systemImage: item.icon)
                        }
                    }
                } label: {
                    HStack(spacing: 4) {
                        Text(model.activeFlow == nil ? (pipelineTabs.first?.title ?? "Pipeline") : model.activeTitle)
                            .font(.subheadline.weight(.semibold))
                        Image(systemName: "chevron.down").font(.caption2.weight(.bold))
                    }
                }
                Spacer()
                Picker("View", selection: Binding(get: { showBoard }, set: { board = $0 })) {
                    Image(systemName: "rectangle.split.3x1").tag(true).accessibilityLabel("Board")
                    Image(systemName: "list.bullet").tag(false).accessibilityLabel("Stages")
                }
                .pickerStyle(.segmented)
                .frame(width: 96)
            }
            .padding(.horizontal)
            .padding(.bottom, 6)
            if let flow {
                if showBoard {
                    CrmBoard(model: model, data: data, flow: flow, present: present)
                } else {
                    stages(flow)
                }
            } else {
                ContentUnavailableView("No pipelines yet", systemImage: "rectangle.split.3x1")
            }
        }
    }

    private func stages(_ flow: CrmFlow) -> some View {
        let rows = model.visibleContacts
        return List {
            ForEach(flow.stats(rows)) { stat in
                let group = rows.filter { stat.stage == "other" ? !flow.stages.contains($0.stage) : $0.stage == stat.stage }
                Section {
                    if open.contains(stat.stage) {
                        ForEach(group) { contact in
                            NavigationLink(value: Route.contact(id: contact.routeId)) {
                                CrmContactRow(contact: contact, flow: flow, showStage: false, dense: false)
                            }
                            .swipeActions(edge: .leading, allowsFullSwipe: true) {
                                if let next = flow.next(after: contact.stage) {
                                    Button {
                                        Task { if await model.move(contact, toStage: next) { FmHaptics.selection() } }
                                    } label: {
                                        Label(CrmStageLabel.title(next), systemImage: "arrow.right")
                                    }
                                    .tint(CrmPalette.stage(next, in: flow))
                                }
                            }
                        }
                    }
                } header: {
                    Button {
                        FmHaptics.selection()
                        withAnimation(.snappy) {
                            if open.contains(stat.stage) { open.remove(stat.stage) } else { open.insert(stat.stage) }
                        }
                    } label: {
                        HStack(spacing: 8) {
                            Circle().fill(CrmPalette.stage(stat.stage, in: flow)).frame(width: 10, height: 10)
                            Text(CrmStageLabel.title(stat.stage)).font(.subheadline.weight(.semibold)).foregroundStyle(.primary)
                            Text("\(stat.count)").font(.subheadline.monospacedDigit()).foregroundStyle(.secondary)
                            Spacer()
                            if stat.value > 0 {
                                Text(CrmText.money(stat.value)).font(.subheadline.monospacedDigit()).foregroundStyle(.secondary)
                            }
                            Image(systemName: open.contains(stat.stage) ? "chevron.up" : "chevron.down")
                                .font(.caption.weight(.semibold)).foregroundStyle(.tertiary)
                        }
                        .textCase(nil)
                        .contentShape(Rectangle())
                    }
                    .buttonStyle(.plain)
                    .disabled(group.isEmpty)
                }
            }
        }
        .listStyle(.insetGrouped)
        .listSectionSpacing(.compact)
        .fmRefreshable { await model.load() }
    }
}

struct CrmActivitySection: View {
    @Bindable var model: CrmModel
    let data: CrmOrganizationData
    let present: (CrmSheet) -> Void

    var body: some View {
        CrmTabContent(
            model: model, data: data, selecting: .constant(false), selection: .constant([]), present: present)
    }
}

struct CrmMorePane: View {
    @Bindable var model: CrmModel
    let data: CrmOrganizationData
    let present: (CrmSheet) -> Void
    let importCSV: () -> Void
    let export: () async -> Void
    @Environment(\.openURL) private var openURL
    @Environment(\.pluginMenu) private var pluginMenu

    var body: some View {
        List {
            Section {
                Button {
                    present(.settings)
                } label: {
                    Label("CRM settings", systemImage: "gearshape")
                }
                if let settings = CrmLayout.settings {
                    CrmModeSwitcher(settings: settings)
                }
            } header: {
                Text("Settings")
            } footer: {
                Text("Layout and theme apply across the app.")
            }
            Section("Mail") {
                CrmZohoBar(model: model, data: data)
                    .listRowInsets(EdgeInsets(top: 4, leading: 8, bottom: 4, trailing: 8))
                    .listRowBackground(Color.clear)
                Button("Sync mail", systemImage: "arrow.triangle.2.circlepath") {
                    Task { if let url = await model.syncMail() { openURL(url) } }
                }
                if let url = data.settings.composeURL(to: "") {
                    Button(data.settings.mailProvider == "gmail" ? "Open Gmail" : "Open Zoho Mail", systemImage: "envelope") {
                        openURL(url)
                    }
                }
                Button(
                    model.noteDrafts.isEmpty ? "Unsent drafts" : "Unsent drafts (\(model.noteDrafts.count))",
                    systemImage: "pencil.and.list.clipboard"
                ) { present(.noteDrafts) }
            }
            Section("Organize") {
                Button("Pipelines", systemImage: "point.3.connected.trianglepath.dotted") { present(.pipelines) }
                Button("New list", systemImage: "list.bullet.rectangle") { present(.newList) }
                if model.canManage {
                    Button("New pipeline", systemImage: "plus.rectangle.on.rectangle") { present(.pipeline(CrmFlowDraft())) }
                }
            }
            Section("Data") {
                Button("Import CSV", systemImage: "square.and.arrow.down") { importCSV() }
                Button("Export CSV", systemImage: "square.and.arrow.up") { Task { await export() } }
                Button("Refresh", systemImage: "arrow.clockwise") { Task { await model.load() } }
            }
            if let pluginMenu {
                Section {
                    Button {
                        pluginMenu.open()
                    } label: {
                        Label(PluginMenu.optionsLabel, systemImage: "star.bubble")
                    }
                }
            }
        }
        .listStyle(.insetGrouped)
        .listSectionSpacing(.compact)
    }
}
