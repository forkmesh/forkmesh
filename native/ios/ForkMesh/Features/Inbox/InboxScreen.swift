import FMAccount
import FMApi
import FMBase
import SwiftUI

@MainActor
enum InboxServices {
    static func makeModel(filter: InboxFilter) -> InboxModel? {
        guard let api = Services.get(ApiService.self), let prefs = Services.get(KeyValueStore.self) else {
            return nil
        }
        let auth: AuthService? = Services.get(AuthService.self)
        let model: InboxModel = InboxModel(
            client: api, node: { auth?.session?.nodeName ?? "" }, store: prefs, filter: filter, localPings: InboxLocalPings.log,
            snapshot: Services.get(PingSnapshot.self))
        model.onChanged = {
            Task { await Services.get(NotificationService.self)?.refresh(silent: true) }
        }
        return model
    }
}

struct InboxScreen: View {
    @Environment(\.fmPalette) private var fm
    @Environment(\.scenePhase) private var scenePhase
    @State private var model: InboxModel?
    @State private var composing: InboxComposeBox?
    @State private var notice: FmNotice?
    @State private var showingSettings: Bool = false
    @State private var opened: InboxItem?
    private let pings: NotificationService? = Services.get(NotificationService.self)

    init(filter: InboxFilter) {
        _model = State(initialValue: InboxServices.makeModel(filter: filter))
    }

    var body: some View {
        Group {
            if let model = model {
                InboxList(
                    model: model, composing: $composing, notice: $notice,
                    menu: InboxToolbarMenu(
                        model: model, onNotice: { (next: FmNotice) in notice = next },
                        onOpenSettings: { showingSettings = true },
                        onDeleteAll: { deleteAll(model) }))
                    .shellDetail(item: $opened) { (item: InboxItem) in
                        InboxItemScreen(model: model, item: item)
                            .shellPageClearance()
                    }
                    .onChange(of: pings?.openRequest) { (_: String?, _: String?) in
                        openRequestedPing()
                    }
                    .task {
                        openRequestedPing()
                        await model.loadIfStale()
                    }
                    .fmRefreshable { await model.load() }
                    .onChange(of: scenePhase) { (_: ScenePhase, phase: ScenePhase) in
                        if phase == .active {
                            Task { await model.loadIfStale() }
                        }
                    }
            } else {
                FmEmptyState(icon: "inbox_outlined", title: "The inbox is unavailable")
            }
        }
        .background(fm.canvas)
        .navigationTitle("Inbox")
        .navigationBarTitleDisplayMode(.inline)
        .shellDetail(isPresented: $showingSettings) {
            RouteDestinationView(route: .pingSettings)
                .shellPageClearance()
        }
        .inboxComposeSheet($composing)
    }
}

extension InboxScreen {
    private func deleteAll(_ model: InboxModel) {
        Task {
            let failure: String? = await model.deleteAllPings()
            notice = failure.map { (text: String) in FmNotice.error(text) } ?? FmNotice("Pings deleted")
        }
    }

    private func openRequestedPing() {
        guard let model = model, let pings = pings, pings.openRequest != nil, let id = pings.takeOpenRequest(),
              let row = pings.notifications.first(where: { $0.id == id }),
              let item = model.item(forPing: row)
        else { return }
        model.markReadLocally(item)
        opened = model.items.first { $0.id == item.id } ?? item
    }
}

struct InboxComposeBox: Identifiable {
    let target: ComposeTarget
    let id: UUID = UUID()
}

struct InboxToolbarMenu: View {
    @Environment(\.fmPalette) private var fm
    let model: InboxModel
    let onNotice: (FmNotice) -> Void
    let onOpenSettings: () -> Void
    let onDeleteAll: () -> Void

    private var hasFailedSync: Bool {
        model.syncQueue.operations.contains(where: { $0.attempts >= PingSyncQueue.maxAttempts })
    }

    var body: some View {
        Menu {
            Button {
                Task { await model.markAllRead() }
            } label: {
                Label("Mark all read", systemImage: "envelope.open")
            }
            Toggle(isOn: Binding(get: { model.unreadOnly }, set: { model.unreadOnly = $0 })) {
                Label("Unread only", systemImage: "line.3.horizontal.decrease")
            }
            Menu {
                ForEach(PingPauseChoice.allCases, id: \.self) { (choice: PingPauseChoice) in
                    Button(choice.label) { pause(choice) }
                }
                if model.isPaused {
                    Button("Resume pings") { pause(nil) }
                }
            } label: {
                Label(model.isPaused ? "Pings paused" : "Pause pings", systemImage: model.isPaused ? "bell.slash" : "moon.zzz")
            }
            if hasFailedSync {
                Button(role: .destructive) {
                    model.syncQueue.discardFailed()
                    Task { await model.load() }
                } label: {
                    Label("Discard changes that could not sync", systemImage: "exclamationmark.arrow.triangle.2.circlepath")
                }
            }
            Divider()
            Button(action: onOpenSettings) {
                Label("Ping settings", systemImage: "gearshape")
            }
            Button(role: .destructive, action: onDeleteAll) {
                Label("Delete all pings", systemImage: "trash")
            }
            .disabled(!model.canDeleteAllPings)
            .accessibilityIdentifier("inbox-delete-all")
        } label: {
            Image(systemName: "ellipsis")
                .font(.system(size: 17, weight: .semibold))
                .foregroundStyle(fm.textPrimary)
                .frame(width: 44, height: 44)
                .overlay(alignment: .topTrailing) {
                    if model.unreadOnly || model.isPaused || hasFailedSync {
                        Circle()
                            .fill(hasFailedSync ? fm.danger : (model.isPaused ? fm.warning : fm.accent))
                            .frame(width: 8, height: 8)
                            .offset(x: -8, y: 8)
                            .accessibilityHidden(true)
                    }
                }
                .contentShape(Circle())
        }
        .inboxGlass(Circle(), tint: nil, fallback: fm.bgRaised)
        .accessibilityLabel("Inbox actions")
        .accessibilityIdentifier("inbox-actions")
    }

    private func pause(_ choice: PingPauseChoice?) {
        Task {
            if let failure = await model.pause(choice) {
                onNotice(FmNotice.error(failure))
            } else {
                onNotice(FmNotice(choice == nil ? "Pings resumed" : "Pings paused"))
            }
        }
    }
}

struct InboxList: View {
    @Environment(\.fmPalette) private var fm
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    let model: InboxModel
    @Binding var composing: InboxComposeBox?
    @Binding var notice: FmNotice?
    let menu: InboxToolbarMenu

    var body: some View {
        ScrollViewReader { (proxy: ScrollViewProxy) in
            list
                .onTabReselect(.inbox) {
                    withAnimation(.easeOut(duration: 0.25)) { proxy.scrollTo(TabTopAnchor.id, anchor: .top) }
                }
        }
    }

    private var list: some View {
        List {
            Section {
                TabTopAnchor()
                    .listRowInsets(EdgeInsets())
                    .listRowSeparator(.hidden)
                    .listRowBackground(Color.clear)
                    .accessibilityHidden(true)
                InboxTitleBar(model: model, menu: menu)
                    .listRowInsets(EdgeInsets(top: 0, leading: 0, bottom: 0, trailing: 0))
                    .listRowBackground(Color.clear)
                    .listRowSeparator(.hidden)
                if notice != nil {
                    FmNoticeLine($notice)
                        .listRowBackground(Color.clear)
                        .accessibilityIdentifier("inbox-notice")
                }
                if model.isPaused {
                    Label(model.pauseLabel, systemImage: "bell.slash").foregroundStyle(fm.warning)
                        .fmMessageTools(model.pauseLabel)
                }
                if model.pendingSyncCount > 0 {
                    Label("\(model.pendingSyncCount) changes waiting to sync", systemImage: "arrow.triangle.2.circlepath")
                        .foregroundStyle(fm.textSecondary)
                        .fmMessageTools("\(model.pendingSyncCount) changes waiting to sync")
                }
                if !model.signedOut {
                    ForEach(model.errors, id: \.self) { (error: String) in
                        Text(error).foregroundStyle(fm.danger)
                            .fmMessageTools(error)
                    }
                }
            }
            Section {
                if model.loading && !model.loaded {
                    FmLinearProgressIndicator(height: 2, period: 0.8)
                        .listRowInsets(EdgeInsets())
                        .listRowSeparator(.hidden)
                        .listRowBackground(Color.clear)
                        .accessibilityIdentifier("inbox-loading")
                }
                if model.signedOut {
                    InboxSignedOut(compact: !model.visible.isEmpty)
                        .listRowInsets(EdgeInsets())
                        .listRowSeparator(.hidden)
                        .listRowBackground(Color.clear)
                } else if model.visible.isEmpty && model.loaded {
                    Text(model.filter.emptyMessage).foregroundStyle(fm.textSecondary)
                }
                ForEach(model.visible) { (item: InboxItem) in
                    InboxRowLink(model: model, item: item, composing: $composing)
                }
            }
        }
        .listStyle(.plain)
        .scrollContentBackground(.hidden)
        .contentMargins(.top, FmSpace.x1, for: .scrollContent)
        .environment(\.defaultMinListRowHeight, 0)
        .animation(FmMotion.easeOutCubic(FmMotion.normal, reduceMotion: reduceMotion), value: model.loaded)
    }
}

struct InboxTitleBar: View {
    @Environment(\.fmPalette) private var fm
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    let model: InboxModel
    let menu: InboxToolbarMenu

    static let order: [InboxFilter] = [.approvals, .reviews, .assigned, .mentions, .pings]

    private var subtitle: String {
        let unread: Int = model.unreadCounts[model.filter] ?? 0
        var parts: [String] = [model.filter.menuTitle]
        if model.unreadOnly {
            parts.append("unread only")
        } else if model.loaded && !model.signedOut {
            parts.append(unread == 0 ? "all read" : "\(unread) unread")
        }
        return parts.joined(separator: " · ")
    }

    var body: some View {
        HStack(alignment: .top, spacing: FmSpace.x3) {
            Menu {
                filterButton(.all)
                Divider()
                ForEach(InboxTitleBar.order, id: \.self) { (filter: InboxFilter) in
                    filterButton(filter)
                }
            } label: {
                VStack(alignment: .leading, spacing: 2) {
                    HStack(alignment: .firstTextBaseline, spacing: FmSpace.x1 + 2) {
                        Text("Inbox")
                            .fmText(.displaySmall)
                        Image(systemName: "chevron.down")
                            .font(.system(size: 15, weight: .semibold))
                            .foregroundStyle(fm.textSecondary)
                    }
                    Text(subtitle)
                        .fmText(.bodyLarge, color: fm.textSecondary, height: FmType.leadingTight)
                        .lineLimit(1)
                        .contentTransition(.numericText())
                }
                .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            .tint(fm.textPrimary)
            .sensoryFeedback(.selection, trigger: model.filter)
            .accessibilityLabel(Text("Inbox, \(subtitle)"))
            .accessibilityHint(Text("Choose what the inbox shows"))
            .accessibilityIdentifier("inbox-filter-menu")
            Spacer(minLength: 0)
            menu
        }
        .padding(.horizontal, FmSpace.x4)
        .padding(.top, FmSpace.x2)
        .padding(.bottom, FmSpace.x2)
        .genieKeepsClear()
    }

    private func filterButton(_ filter: InboxFilter) -> some View {
        let count: Int = model.unreadCounts[filter] ?? 0
        return Button {
            withAnimation(reduceMotion ? nil : .snappy) {
                model.filter = filter
            }
        } label: {
            Label {
                Text(filter.menuTitle)
                if count > 0 {
                    Text(count == 1 ? "1 unread" : "\(count) unread")
                }
            } icon: {
                Image(systemName: model.filter == filter ? "checkmark" : filter.icon)
            }
        }
        .accessibilityAddTraits(model.filter == filter ? .isSelected : [])
    }
}

extension InboxFilter {
    var menuTitle: String {
        self == .all ? "All activity" : label
    }

    var icon: String {
        switch self {
        case .all: return "tray.full"
        case .pings: return "bell"
        case .reviews: return "arrow.triangle.pull"
        case .assigned: return "person.crop.circle.badge.checkmark"
        case .mentions: return "at"
        case .approvals: return "checkmark.shield"
        }
    }
}

struct InboxGlassGroup<Content: View>: View {
    @ViewBuilder let content: () -> Content

    var body: some View {
        #if compiler(>=6.2)
        if #available(iOS 26.0, *) {
            GlassEffectContainer(spacing: 6) { content() }
        } else {
            content()
        }
        #else
        content()
        #endif
    }
}

extension View {
    @ViewBuilder func inboxGlass<S: Shape>(_ shape: S, tint: Color?, fallback: Color) -> some View {
        #if compiler(>=6.2)
        if #available(iOS 26.0, *) {
            self.glassEffect(tint.map { Glass.regular.tint($0.opacity(0.35)).interactive() } ?? Glass.regular.interactive(), in: shape)
        } else {
            self.background(fallback, in: shape)
        }
        #else
        self.background(fallback, in: shape)
        #endif
    }
}

struct InboxRowLink: View {
    @Environment(\.fmPalette) private var fm
    let model: InboxModel
    let item: InboxItem
    @Binding var composing: InboxComposeBox?

    var body: some View {
        InboxRow(item: item)
            .padding(.horizontal, FmSpace.x4)
            .padding(.vertical, FmSpace.x2)
            .overlay(alignment: .bottom) {
                Rectangle().fill(fm.border).frame(height: 0.5)
            }
            .contentShape(Rectangle())
            .shellPaneRowLink(id: AnyHashable("inbox-\(item.id)")) {
                InboxItemScreen(model: model, item: item)
                    .shellPageClearance()
            }
            .listRowInsets(EdgeInsets())
            .listRowSeparator(.hidden)
            .listRowBackground(Color.clear)
            .swipeActions(edge: .trailing) {
                Button("Delete", role: .destructive) { Task { await model.delete(item) } }
            }
            .swipeActions(edge: .leading) {
                if item.unread {
                    Button("Read") { Task { await model.markRead(item) } }
                        .tint(.blue)
                }
            }
            .textSelection(.disabled)
            .contextMenu {
                Button("Send to agent") { InboxComposeBox.send(model.prompt(for: item), fallback: $composing) }
                if item.unread {
                    Button("Mark read") { Task { await model.markRead(item) } }
                }
                Button("Delete", role: .destructive) { Task { await model.delete(item) } }
            }
    }
}

struct InboxRow: View {
    @Environment(\.fmPalette) private var fm
    @State private var settings: SettingsService? = Services.get(SettingsService.self)
    let item: InboxItem

    private var instance: RelayProfile? {
        item.source == .local ? nil : settings?.project
    }

    private var tint: Color {
        switch item.tone {
        case .danger: return fm.danger
        case .success: return fm.success
        case .warning: return fm.warning
        case .info, .accent, .neutral: return fm.textSecondary
        }
    }

    var body: some View {
        HStack(alignment: .top, spacing: 10) {
            VStack(spacing: FmSpace.x1) {
                FmIcon(item.icon, size: 18, color: tint)
                    .frame(width: 22, height: 22)
                if let instance {
                    ProjectIcon(project: instance, size: 16)
                        .accessibilityIdentifier("inbox-row-instance-logo")
                }
            }
            VStack(alignment: .leading, spacing: 1) {
                HStack(spacing: 6) {
                    Text(item.kindLabel).font(.caption.weight(.semibold)).foregroundStyle(item.unread ? fm.textSecondary : fm.textTertiary)
                    if !item.repo.isEmpty {
                        Text(item.repo).font(.caption).foregroundStyle(fm.textTertiary).lineLimit(1)
                    }
                    Spacer()
                    Text(notificationTimeLabel(item.ts)).font(.caption).foregroundStyle(item.unread ? fm.accent : fm.textTertiary)
                }
                Text(item.title)
                    .font(.subheadline.weight(item.unread ? .semibold : .regular))
                    .foregroundStyle(item.unread ? fm.textPrimary : fm.textSecondary)
                    .lineLimit(2)
                if !item.body.isEmpty {
                    Text(item.body).font(.footnote).foregroundStyle(item.unread ? fm.textSecondary : fm.textTertiary).lineLimit(1)
                }
                if item.sync == .pending || item.sync == .failed {
                    Label(item.sync == .pending ? "Waiting to sync" : "Could not sync", systemImage: "arrow.triangle.2.circlepath")
                        .font(.caption2)
                        .foregroundStyle(item.sync == .failed ? fm.danger : fm.textTertiary)
                }
            }
            Circle()
                .fill(item.unread ? fm.accent : Color.clear)
                .frame(width: 8, height: 8)
                .padding(.top, 5)
        }
        .accessibilityElement(children: .combine)
        .accessibilityLabel("\(item.unread ? "Unread" : "Read"). \(item.kindLabel). \(item.title)\(instance.map { ". From \($0.label)" } ?? "")")
    }
}

struct InboxSignedOut: View {
    @Environment(\.fmPalette) private var fm
    let compact: Bool

    private func signIn() {
        Task { _ = await AccountFeature.requireAccount(action: "see your inbox") }
    }

    var body: some View {
        if compact {
            HStack(spacing: FmSpace.x3) {
                Text("Sign in to see reviews, approvals and mentions.")
                    .fmText(.bodyMedium, color: fm.textSecondary)
                    .frame(maxWidth: .infinity, alignment: .leading)
                FmFilledButton("Sign in", minHeight: 36, onPressed: signIn)
            }
            .padding(.horizontal, FmSpace.x4)
            .padding(.vertical, FmSpace.x3)
        } else {
            FmEmptyState(
                icon: "inbox_outlined",
                title: "Reviews, pings and approvals land here",
                message: "Sign in to ForkMesh to see them.",
                actionLabel: "Sign in",
                onAction: signIn)
                .frame(minHeight: 360)
                .accessibilityIdentifier("inbox-signed-out")
        }
    }
}
