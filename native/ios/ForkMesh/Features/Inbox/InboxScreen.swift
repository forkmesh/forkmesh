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
                    .navigationDestination(item: $opened) { (item: InboxItem) in
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
        .navigationDestination(isPresented: $showingSettings) {
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
        HStack(spacing: FmSpace.x2) {
            deleteAllButton
            menu
        }
    }

    private var deleteAllButton: some View {
        Button(action: onDeleteAll) {
            Image(systemName: "trash")
                .font(.subheadline.weight(.semibold))
                .foregroundStyle(model.canDeleteAllPings ? fm.danger : fm.textDisabled)
                .frame(width: 32, height: 32)
                .contentShape(Circle())
        }
        .buttonStyle(.plain)
        .disabled(!model.canDeleteAllPings)
        .inboxGlass(Circle(), tint: nil, fallback: fm.bgRaised)
        .accessibilityLabel("Delete all pings")
        .accessibilityIdentifier("inbox-delete-all")
    }

    private var menu: some View {
        Menu {
            Button("Mark all read") { Task { await model.markAllRead() } }
            Toggle("Unread only", isOn: Binding(get: { model.unreadOnly }, set: { model.unreadOnly = $0 }))
            Menu("Pause pings") {
                ForEach(PingPauseChoice.allCases, id: \.self) { (choice: PingPauseChoice) in
                    Button(choice.label) { pause(choice) }
                }
                if model.isPaused {
                    Button("Resume pings") { pause(nil) }
                }
            }
            if model.syncQueue.operations.contains(where: { $0.attempts >= PingSyncQueue.maxAttempts }) {
                Button("Discard changes that could not sync", role: .destructive) {
                    model.syncQueue.discardFailed()
                    Task { await model.load() }
                }
            }
            Button {
                onOpenSettings()
            } label: {
                Label("Ping settings", systemImage: "gearshape")
            }
        } label: {
            Image(systemName: model.isPaused ? "bell.slash" : "ellipsis")
                .font(.subheadline.weight(.semibold))
                .foregroundStyle(model.isPaused ? fm.warning : fm.textPrimary)
                .frame(width: 32, height: 32)
                .contentShape(Circle())
        }
        .inboxGlass(Circle(), tint: nil, fallback: fm.bgRaised)
        .accessibilityLabel("Inbox options")
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
        let counts: [InboxFilter: Int] = model.unreadCounts
        HStack(spacing: FmSpace.x2) {
            ScrollView(.horizontal, showsIndicators: false) {
                InboxGlassGroup {
                    HStack(spacing: 6) {
                        ForEach(InboxFilter.allCases, id: \.self) { (filter: InboxFilter) in
                            chip(filter, count: counts[filter] ?? 0)
                        }
                    }
                    .padding(.horizontal, FmSpace.x1)
                    .padding(.vertical, 2)
                }
            }
            menu
        }
        .padding(.trailing, FmSpace.x1)
    }

    private func chip(_ filter: InboxFilter, count: Int) -> some View {
        let selected: Bool = model.filter == filter
        return Button {
            model.filter = filter
        } label: {
            HStack(spacing: 3) {
                Text(filter.label)
                if count > 0 {
                    Text("\(count)").font(.caption2.monospacedDigit()).foregroundStyle(selected ? fm.accent : fm.textSecondary)
                }
            }
            .font(.subheadline.weight(selected ? .semibold : .regular))
            .foregroundStyle(fm.textPrimary)
            .padding(.horizontal, 10)
            .frame(height: 30)
            .contentShape(Capsule())
        }
        .buttonStyle(.plain)
        .inboxGlass(Capsule(), tint: selected ? fm.accent : nil, fallback: selected ? fm.accentSubtle : fm.bgRaised)
        .accessibilityAddTraits(selected ? .isSelected : [])
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
            .background {
                NavigationLink {
                    InboxItemScreen(model: model, item: item)
                        .shellPageClearance()
                } label: {
                    EmptyView()
                }
                .opacity(0)
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
    let item: InboxItem

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
            FmIcon(item.icon, size: 18, color: tint)
                .frame(width: 22, height: 22)
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
        .accessibilityLabel("\(item.unread ? "Unread" : "Read"). \(item.kindLabel). \(item.title)")
    }
}
