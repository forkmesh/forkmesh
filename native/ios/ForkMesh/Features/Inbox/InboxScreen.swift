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

    var body: some View {
        HStack(spacing: InboxSquare.spacing) {
            square("envelope.open", color: fm.textPrimary, label: "Mark all read") {
                Task { await model.markAllRead() }
            }
            square(
                "line.3.horizontal.decrease", color: model.unreadOnly ? fm.accent : fm.textPrimary,
                tint: model.unreadOnly ? fm.accent : nil, label: "Unread only"
            ) {
                model.unreadOnly.toggle()
            }
            .accessibilityAddTraits(model.unreadOnly ? .isSelected : [])
            pauseMenu
            if model.syncQueue.operations.contains(where: { $0.attempts >= PingSyncQueue.maxAttempts }) {
                square("exclamationmark.arrow.triangle.2.circlepath", color: fm.danger, label: "Discard changes that could not sync") {
                    model.syncQueue.discardFailed()
                    Task { await model.load() }
                }
            }
            square("gearshape", color: fm.textPrimary, label: "Ping settings", action: onOpenSettings)
            square("trash", color: model.canDeleteAllPings ? fm.danger : fm.textDisabled, label: "Delete all pings", action: onDeleteAll)
                .disabled(!model.canDeleteAllPings)
                .accessibilityIdentifier("inbox-delete-all")
        }
    }

    private func square(
        _ icon: String, color: Color, tint: Color? = nil, label: String, action: @escaping () -> Void
    ) -> some View {
        FmHoldButton(label, action: action) {
            InboxSquare(icon: icon, color: color)
        }
        .buttonStyle(.plain)
        .inboxGlass(InboxSquare.shape, tint: tint, fallback: tint == nil ? fm.bgRaised : fm.accentSubtle)
        .accessibilityLabel(label)
    }

    private var pauseMenu: some View {
        Menu {
            ForEach(PingPauseChoice.allCases, id: \.self) { (choice: PingPauseChoice) in
                Button(choice.label) { pause(choice) }
            }
            if model.isPaused {
                Button("Resume pings") { pause(nil) }
            }
        } label: {
            InboxSquare(icon: model.isPaused ? "bell.slash" : "moon.zzz", color: model.isPaused ? fm.warning : fm.textPrimary)
        }
        .inboxGlass(InboxSquare.shape, tint: nil, fallback: fm.bgRaised)
        .fmHoldName(model.isPaused ? "Pings paused" : "Pause pings")
        .accessibilityLabel(model.isPaused ? "Pings paused" : "Pause pings")
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
                InboxFilterBar(model: model, menu: menu)
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
                ForEach(model.errors, id: \.self) { (error: String) in
                    Text(error).foregroundStyle(fm.danger)
                        .fmMessageTools(error)
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
                if model.visible.isEmpty && model.loaded {
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

struct InboxFilterBar: View {
    @Environment(\.fmPalette) private var fm
    let model: InboxModel
    let menu: InboxToolbarMenu

    var body: some View {
        ViewThatFits(in: .horizontal) {
            row.frame(maxWidth: .infinity)
            ScrollView(.horizontal, showsIndicators: false) { row }
        }
    }

    private var row: some View {
        let counts: [InboxFilter: Int] = model.unreadCounts
        return InboxGlassGroup {
            HStack(spacing: FmSpace.x2) {
                HStack(spacing: InboxSquare.spacing) {
                    ForEach(InboxFilter.allCases, id: \.self) { (filter: InboxFilter) in
                        chip(filter, count: counts[filter] ?? 0)
                    }
                }
                menu
            }
            .padding(.horizontal, FmSpace.x2)
            .padding(.vertical, FmSpace.x1 + 2)
        }
        .overlayPreferenceValue(InboxBadgeKey.self) { (badges: [InboxBadge]) in
            GeometryReader { (proxy: GeometryProxy) in
                ForEach(badges, id: \.filter) { (badge: InboxBadge) in
                    let rect: CGRect = proxy[badge.anchor]
                    Color.clear
                        .frame(width: rect.width, height: rect.height)
                        .overlay(alignment: .topTrailing) { badgeLabel(badge.count) }
                        .position(x: rect.midX, y: rect.midY)
                }
            }
            .allowsHitTesting(false)
        }
    }

    private func chip(_ filter: InboxFilter, count: Int) -> some View {
        let selected: Bool = model.filter == filter
        return FmHoldButton(filter.label, action: {
            model.filter = filter
        }) {
            InboxSquare(icon: filter.icon, color: selected ? fm.accent : fm.textPrimary)
        }
        .buttonStyle(.plain)
        .inboxGlass(InboxSquare.shape, tint: selected ? fm.accent : nil, fallback: selected ? fm.accentSubtle : fm.bgRaised)
        .anchorPreference(key: InboxBadgeKey.self, value: .bounds) { (anchor: Anchor<CGRect>) in
            count > 0 ? [InboxBadge(filter: filter, count: count, anchor: anchor)] : []
        }
        .accessibilityLabel(filter.label)
        .accessibilityValue(count > 0 ? "\(count) unread" : "")
        .accessibilityAddTraits(selected ? .isSelected : [])
    }

    private func badgeLabel(_ count: Int) -> some View {
        Text(count > 99 ? "99+" : "\(count)")
            .font(.system(size: 9, weight: .bold).monospacedDigit())
            .foregroundStyle(Color.white)
            .padding(.horizontal, 3)
            .frame(minWidth: 14, minHeight: 14)
            .background(fm.count, in: Capsule())
            .fixedSize()
            .offset(x: 5, y: -5)
    }
}

struct InboxBadge {
    let filter: InboxFilter
    let count: Int
    let anchor: Anchor<CGRect>
}

struct InboxBadgeKey: PreferenceKey {
    static var defaultValue: [InboxBadge] { [] }

    static func reduce(value: inout [InboxBadge], nextValue: () -> [InboxBadge]) {
        value.append(contentsOf: nextValue())
    }
}

struct InboxSquare: View {
    static let side: CGFloat = 28
    static let spacing: CGFloat = 4
    static var shape: RoundedRectangle { RoundedRectangle(cornerRadius: 8, style: .continuous) }
    let icon: String
    let color: Color

    var body: some View {
        Image(systemName: icon)
            .font(.footnote.weight(.semibold))
            .foregroundStyle(color)
            .frame(width: Self.side, height: Self.side)
            .contentShape(Self.shape)
    }
}

extension InboxFilter {
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
            .shellPaneRowLink {
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
        case .info: return .blue
        case .accent: return .purple
        case .neutral: return fm.textSecondary
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
                    Text(item.kindLabel).font(.caption.weight(.semibold)).foregroundStyle(item.unread ? tint : fm.textTertiary)
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
