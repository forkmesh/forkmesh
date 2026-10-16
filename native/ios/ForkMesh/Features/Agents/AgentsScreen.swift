import FMAccount
import FMAgentWatch
import FMAgents
import FMApi
import FMAssistant
import FMBase
import FMChat
import FMControl
import FMLocalAI
import SwiftUI

struct AgentsScreen: View {
    @Environment(\.fmPalette) private var fm
    @Environment(\.scenePhase) private var scenePhase
    @State private var model: AgentsListModel
    @State private var notice: FmNotice?
    @State private var bulkNotice: FmNotice?
    @State private var loader: AgentAttachmentLoader = AgentAttachmentsSupport.loader()
    @State private var attention: RunnerAttentionModel? = Services.get(RunnerAttentionModel.self)
    @State private var runnerAccounts: RunnerAccountsModel? = Services.get(RunnerAccountsModel.self)
    @State private var desktopTerminals: Bool?
    @State private var showingRunnerAccounts = false
    @State private var listView = AgentsScreen.storedListView()
    @State private var listSort = AgentLineSort(
        stored: Services.get(KeyValueStore.self)?.string(forKey: AgentLineSort.storageKey))
    @State private var eagle = AgentEagleFeed()
    @State private var reads = AgentReadTracker(store: Services.get(KeyValueStore.self)) { (id: Int) in
        Task { await HostsFeature.agents?.markRead(id: id) }
    }
    @State private var dots: AgentDotsWatch?
    @State private var swipedOpen: String?
    @State private var deleting: Set<String> = []
    @State private var confirmDelete: AgentLineDeleteRequest?
    @State private var leaving: Task<Void, Never>?
    @State private var confirmStopAll = false
    @State private var stoppingAll = false
    @State private var stopAllCount = 0
    @State private var folderFolds = AgentFolderFolds.load(Services.get(KeyValueStore.self))
    @State private var opened: AgentOpenedLine?

    private static let leaveDelay: Duration = .milliseconds(500)

    init() {
        let api: AgentsApi = Services.require(ApiService.self)
        let auth: AuthService? = Services.get(AuthService.self)
        _model = State(initialValue: AgentsListModel(
            api: api,
            signal: Services.get(AgentListSignal.self),
            ownerAccountName: { auth?.session?.nodeName ?? "" },
            hasDesktopNode: {
                guard let session = auth?.session else {
                    return true
                }
                return !session.desktopDevices.isEmpty
            },
            refusals: AgentReadRefusals.shared
        ))
    }

    private var canStart: Bool {
        return Services.get(ApiService.self) != nil
    }

    private var lines: [AgentLine] {
        let pending: AgentPendingDeletes = .shared
        let center: AgentControlCenter? = HostsFeature.agents
        return reads.lines(listSort.sorted(AgentLines.merge(
            entries: model.entries, rows: center?.rows ?? [], cover: center?.desktopCover)))
            .filter { !pending.isHidden(AgentLines.deletion($0)?.key ?? "") }
    }

    private var runningIds: [String] {
        Self.runningIds(lines)
    }

    private static func swimmers(_ shown: [AgentLine]) -> [AquariumSwimmer] {
        shown.filter { $0.isActive || $0.status == .waiting }.prefix(AquariumTank.most).map { (line: AgentLine) -> AquariumSwimmer in
            AquariumSwimmer(id: line.id, name: line.title, model: line.face.isEmpty ? line.provider : line.face, waiting: line.status == .waiting)
        }
    }

    private static func runningIds(_ shown: [AgentLine]) -> [String] {
        shown.filter { $0.status == .running }.map(\.id)
    }

    var body: some View {
        let shown: [AgentLine] = lines
        return tabBody(shown)
        .background(fm.canvas)
        .onAppear {
            leaving?.cancel()
            leaving = nil
            AgentDotsFeed.shared.list = model
            model.enabled = true
            model.start()
            followDialogs()
            syncDots()
        }
        .task { await HostsFeature.agents?.refresh() }
        .task { await attention?.refresh() }
        .task { await runnerAccounts?.refresh() }
        .task {
            if let center = HostsFeature.agents {
                desktopTerminals = await DesktopTerminalSupport.check(center.hub)
            }
        }
        .onDisappear(perform: leave)
        .onChange(of: model.groups) { (_: [AgentRepoGroup], _: [AgentRepoGroup]) in
            syncDots()
        }
        .onChange(of: Self.runningIds(shown)) { (_: [String], _: [String]) in
            syncDots()
        }
        .onChange(of: scenePhase) { (_: ScenePhase, phase: ScenePhase) in
            if phase == .active {
                dots?.controller.resume()
            } else {
                dots?.controller.pause()
            }
        }
        .onChange(of: eagleWatch) { (_: String, _: String) in
            followDialogs()
        }
        .onChange(of: shown.filter(\.isActive).count, initial: true) { (_: Int, running: Int) in
            ShellCounts.shared.agents = running
        }
        .onChange(of: shown.count, initial: true) { (_: Int, total: Int) in
            ShellCounts.shared.agentsTotal = total
        }
        .onChange(of: Self.swimmers(shown), initial: true) { (_: [AquariumSwimmer], next: [AquariumSwimmer]) in
            ShellCounts.shared.swimmers = next
        }
        .onChange(of: AgentLines.runnerCounts(shown), initial: true) { (_: [String: Int], counts: [String: Int]) in
            ShellCounts.shared.runnerAgents = counts
        }
        .navigationDestination(item: $opened) { (target: AgentOpenedLine) in
            page(target)
        }
        .agentOutsideDeleteConfirmation($confirmDelete) { delete($0) }
        .confirmationDialog(
            AgentStopAll.question(stopAllCount), isPresented: $confirmStopAll, titleVisibility: .visible
        ) {
            Button("Stop all", role: .destructive) { stopAll() }
            Button("Cancel", role: .cancel) {}
        }
    }

    private var stopAllTargets: [UnifiedAgentRow] {
        AgentStopAll.targets(HostsFeature.agents?.rows ?? [])
    }

    private func stopAll() {
        guard let center = HostsFeature.agents, !stoppingAll else { return }
        stoppingAll = true
        Task {
            let outcome: AgentStopAllOutcome = await center.stopAll()
            stoppingAll = false
            if outcome.stopped > 0 { FmHaptics.commit() }
            notice = FmNotice(outcome.message, isError: outcome.failed > 0)
            await model.refresh()
        }
    }

    private var eagleWatch: String {
        guard listView == .eagle, let center = HostsFeature.agents else { return "" }
        return "eagle:" + center.rows.map { $0.isActive ? "+" + $0.id : $0.id }.joined(separator: ",")
    }

    private func followDialogs() {
        if listView == .eagle, let center = HostsFeature.agents {
            eagle.follow(center.rows, feeds: { ControlWatchFeeds(center) }) { row in
                (try? await center.transcript(row, since: 0).text) ?? ""
            }
        } else {
            eagle.stop()
        }
    }

    private func syncDots() {
        guard !runningIds.isEmpty,
              let screen = AgentWatchScreen.forAgentsTab(
                model.groups, center: HostsFeature.agents, ownerAccount: { model.ownerAccount(for: $0) }) else {
            stopDots()
            return
        }
        if let dots {
            dots.add(screen)
        } else {
            dots = AgentDotsWatch(screen)
        }
    }

    private func leave() {
        AgentUndoDeleteLine.markSeen()
        let list: AgentsListModel = model
        let feed: AgentEagleFeed = eagle
        let watch: AgentDotsWatch? = dots
        leaving?.cancel()
        leaving = Task { @MainActor in
            try? await Task.sleep(for: Self.leaveDelay)
            guard !Task.isCancelled else { return }
            list.stop()
            feed.stop()
            watch?.stop()
            if dots === watch { dots = nil }
        }
    }

    private func stopDots() {
        dots?.stop()
        dots = nil
    }

    private var tabBar: some View {
        tools
            .frame(maxWidth: .infinity)
            .padding(.horizontal, FmSpace.x2)
            .frame(height: 46)
    }

    private var tools: some View {
        HStack(spacing: 6) {
            if stoppingAll || !stopAllTargets.isEmpty {
                Button {
                    stopAllCount = stopAllTargets.count
                    confirmStopAll = true
                } label: {
                    Group {
                        if stoppingAll {
                            FmCircularProgressIndicator(size: 12, strokeWidth: 1.5)
                        } else {
                            Image(systemName: "stop.fill")
                                .font(.system(size: 12, weight: .semibold))
                                .foregroundStyle(fm.danger)
                        }
                    }
                    .frame(width: 30)
                    .modifier(AgentToolChip())
                }
                .buttonStyle(ShellBarButtonStyle())
                .disabled(stoppingAll)
                .accessibilityLabel(Text("Stop all agents"))
                .accessibilityIdentifier("agents-stop-all")
            }
            Menu {
                ForEach(AgentLineSort.allCases, id: \.self) { (option: AgentLineSort) in
                    Button {
                        withAnimation(.easeOut(duration: 0.2)) { listSort = option }
                        Services.get(KeyValueStore.self)?.set(option.rawValue, forKey: AgentLineSort.storageKey)
                    } label: {
                        Label(option.caption, systemImage: option == listSort ? "checkmark" : option.symbol)
                    }
                }
            } label: {
                VStack(spacing: 1) {
                    Image(systemName: listSort.symbol)
                        .font(.system(size: 7, weight: .bold))
                        .foregroundStyle(fm.textPrimary)
                        .frame(height: 8)
                        .contentTransition(.symbolEffect(.replace))
                    Image(systemName: "arrow.up.arrow.down")
                        .font(.system(size: 11, weight: .semibold))
                        .foregroundStyle(fm.accent)
                }
                .frame(width: 30)
                .modifier(AgentToolChip())
            }
            .buttonStyle(ShellBarButtonStyle())
            .accessibilityLabel(Text("Sort agents: \(listSort.caption)"))
            .accessibilityIdentifier("agents-list-sort")
            Button {
                listView = listView.next
                Services.get(KeyValueStore.self)?.set(listView.rawValue, forKey: AgentListView.storageKey)
            } label: {
                toolIcon(system: listViewSymbol)
            }
            .buttonStyle(ShellBarButtonStyle())
            .accessibilityLabel(Text("Agent list view: \(listView.caption)"))
            .accessibilityIdentifier("agents-list-view")
            if let center = HostsFeature.agents, center.queue != nil {
                DesktopQueueMenu(center: center)
                    .modifier(AgentToolChip())
            }
            if let client = AgentOpsFeature.client {
                AgentUsageBars(client: client)
            } else if let runnerAccounts = runnerAccounts, runnerAccounts.visible {
                Button {
                    showingRunnerAccounts = true
                } label: {
                    toolIcon(system: "person.2.circle")
                }
                .buttonStyle(ShellBarButtonStyle())
                .accessibilityLabel(Text("Runner accounts: \(runnerAccounts.summary)"))
                .accessibilityIdentifier("runner-accounts-entry")
                .sheet(isPresented: $showingRunnerAccounts) {
                    RunnerAccountsSheet(model: runnerAccounts, attention: attention)
                }
            }
            if let center = HostsFeature.agents, center.hub.desktop != nil, desktopTerminals != false {
                NavigationLink {
                    DesktopTerminalScreen(center: center)
                        .shellPageClearance()
                } label: {
                    toolIcon(system: "apple.terminal")
                }
                .buttonStyle(ShellBarButtonStyle())
                .accessibilityLabel(Text("Start an agent in a desktop terminal"))
                .accessibilityIdentifier("agents-desktop-terminal")
            }
            if let client = AgentOpsFeature.client, HostsFeature.agents?.hub.desktop != nil {
                NavigationLink {
                    RecycleBinScreen(client: client)
                        .shellPageClearance()
                } label: {
                    toolIcon(system: "trash")
                }
                .buttonStyle(ShellBarButtonStyle())
                .accessibilityLabel(Text("Deleted agents on the desktop"))
                .accessibilityIdentifier("agents-recycle-bin")
            }
            if let watch = AgentWatchScreen.forAgentsTab(
                model.groups, center: HostsFeature.agents, ownerAccount: { model.ownerAccount(for: $0) }) {
                NavigationLink {
                    watch
                        .shellPageClearance()
                } label: {
                    toolIcon(system: "waveform")
                }
                .buttonStyle(ShellBarButtonStyle())
                .accessibilityLabel(Text("Watch, hear and feel agents"))
                .accessibilityIdentifier("agents-watch")
            }
            if let local = Services.get(LocalModelManager.self), local.engineAvailability.isAvailable {
                NavigationLink {
                    OnDeviceChatView(manager: local)
                        .shellPageClearance()
                } label: {
                    FmIcon("smart_toy_outlined", size: 15, color: fm.textSecondary)
                        .frame(width: 30)
                        .modifier(AgentToolChip())
                }
                .buttonStyle(ShellBarButtonStyle())
                .accessibilityLabel(Text("Chat with \(local.selectedModel.displayName) on this iPhone"))
                .accessibilityIdentifier("agents-on-device-chat")
            }
        }
    }

    private static func storedListView() -> AgentListView {
        let store = Services.get(KeyValueStore.self)
        let defaulted = store?.string(forKey: AgentListView.eagleDefaultKey) != nil
        let view = AgentListView(stored: store?.string(forKey: AgentListView.storageKey), eagleDefaulted: defaulted)
        if !defaulted, let store {
            store.set(view.rawValue, forKey: AgentListView.storageKey)
            store.set("1", forKey: AgentListView.eagleDefaultKey)
        }
        return view
    }

    private var listViewSymbol: String {
        switch listView {
        case .compact: return "list.bullet"
        case .zen: return "moon"
        case .full: return "eye"
        case .eagle: return "bird"
        }
    }

    private func toolIcon(system: String) -> some View {
        Image(systemName: system)
            .font(.system(size: 13, weight: .semibold))
            .foregroundStyle(fm.textSecondary)
            .frame(width: 30)
            .modifier(AgentToolChip())
    }

    private func tabBody(_ shown: [AgentLine]) -> some View {
        ScrollViewReader { (proxy: ScrollViewProxy) in
            ScrollView {
                VStack(spacing: 0) {
                    TabTopAnchor()
                    listTab(shown)
                        .padding(EdgeInsets.fmLTRB(FmSpace.x2, FmSpace.x2, FmSpace.x2, FmSpace.x5))
                }
            }
            .fmRefreshable {
                Task { await HostsFeature.agents?.refresh() }
                await model.refresh()
            }
            .safeAreaInset(edge: .top, spacing: 0) {
                tabBar
            }
            .onTabReselect(.agents) {
                withAnimation(.easeOut(duration: 0.25)) { proxy.scrollTo(TabTopAnchor.id, anchor: .top) }
            }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .top)
    }

    @ViewBuilder private var pending: some View {
        if model.isPendingFirstLoad {
            FmSkeletonList(rows: 4, label: "Loading agents")
                .frame(height: 320)
        } else if model.isFailed {
            FmCard(radius: FmRadius.lg) {
                VStack(spacing: FmSpace.x3) {
                    Text(model.loadError ?? "")
                        .fmText(.bodyMedium, color: fm.textSecondary)
                    FmMessageTools(model.loadError ?? "")
                    FmToneButton(label: "Retry", onPressed: { Task { await model.refresh() } })
                }
                .frame(maxWidth: .infinity)
            }
        }
    }

    private func listTab(_ shown: [AgentLine]) -> some View {
        VStack(alignment: .leading, spacing: 3) {
            if let attention = attention {
                RunnerAttentionBanner(model: attention)
                    .padding(.bottom, FmSpace.x2)
            }
            if let queue = AgentOpsFeature.approvals, AgentOpsFeature.client != nil {
                AgentApprovalsSection(queue: queue)
            }
            if AgentUndoDeleteLine.hasNotice {
                AgentUndoDeleteLine()
                    .padding(.horizontal, FmSpace.x2)
            }
            FmNoticeLine($notice)
                .padding(.horizontal, FmSpace.x2)
            FmSwap(stateKey: listPhase(shown)) {
                if !shown.isEmpty {
                    lineList(shown)
                } else if model.isPendingFirstLoad || model.isFailed {
                    pending
                } else if model.sealedCount == 0 {
                    emptyList
                }
            }
            if model.sealedCount > 0 {
                SealedSessionsNotice(count: model.sealedCount)
            }
            controlNotes
            bulkActions
        }
    }

    private func listPhase(_ shown: [AgentLine]) -> Int {
        if !shown.isEmpty { return 0 }
        if model.isPendingFirstLoad { return 1 }
        if model.isFailed { return 2 }
        return 3
    }

    private func ask(_ request: AgentLineDeleteRequest) {
        if request.asksFirst {
            confirmDelete = request
        } else {
            delete(request)
        }
    }

    private func delete(_ request: AgentLineDeleteRequest) {
        guard deleting.insert(request.id).inserted else { return }
        Task {
            let outcome: AgentDeleteOutcome = await request.run()
            deleting.remove(request.id)
            notice = FmNotice(outcome.message, isError: outcome.failed)
            guard !outcome.failed else { return }
            await HostsFeature.agents?.refresh()
            await model.refresh()
        }
    }

    private func lineList(_ shown: [AgentLine]) -> some View {
        TimelineView(.periodic(from: .now, by: 30)) { (context: TimelineViewDefaultContext) in
            let nowMs: Int = Int(context.date.timeIntervalSince1970 * 1000)
            let folders: [LocalAgentFolder] = HostsFeature.agents?.desktopFolders ?? []
            let sections: [AgentFolderSection] = AgentLines.folderSections(shown, folders: folders)
            let showRepo: Bool = Set(folders.map { $0.repoName.lowercased() }).count > 1
            LazyVStack(alignment: .leading, spacing: listView == .eagle ? -AgentLineRow.ruleWidth : 3) {
                ForEach(sections) { (section: AgentFolderSection) in
                    let collapsed: Bool = folderFolds.isCollapsed(section)
                    if section.isFolder {
                        folderHeader(section, collapsed: collapsed, showRepo: showRepo)
                    }
                    if !collapsed {
                        ForEach(section.lines) { (line: AgentLine) in
                            let request: AgentLineDeleteRequest? = AgentLineDeleteRequest(line)
                            lineRow(line, nowMs: nowMs)
                                .contentShape(.contextMenuPreview, FmRoundedShape(radius: FmRadius.sm))
                                .contextMenu { lineMenu(line, request) }
                                .agentSwipeToDelete(id: line.id, openId: $swipedOpen, enabled: request != nil && !deleting.contains(line.id)) {
                                    if let request { ask(request) }
                                }
                        }
                    }
                }
            }
        }
    }

    private func folderHeader(_ section: AgentFolderSection, collapsed: Bool, showRepo: Bool) -> some View {
        let running: Int = section.lines.filter(\.isActive).count
        let summary: String = [
            section.lines.count == 1 ? "1 agent" : "\(section.lines.count) agents",
            running > 0 ? "\(running) running" : "",
        ].filter { !$0.isEmpty }.joined(separator: " · ")
        return Button {
            withAnimation(.easeOut(duration: 0.2)) { folderFolds.set(section.id, collapsed: !collapsed) }
            folderFolds.save(Services.get(KeyValueStore.self))
        } label: {
            HStack(spacing: 6) {
                Image(systemName: "chevron.right")
                    .font(.system(size: 10, weight: .semibold))
                    .foregroundStyle(fm.textTertiary)
                    .rotationEffect(.degrees(collapsed ? 0 : 90))
                Image(systemName: collapsed ? "folder" : "folder.fill")
                    .font(.system(size: 12, weight: .semibold))
                    .foregroundStyle(fm.accent)
                Text(section.name)
                    .fmText(.labelMedium)
                    .lineLimit(1)
                if showRepo {
                    Text(section.repoName)
                        .fmText(.bodySmall, color: fm.textTertiary)
                        .lineLimit(1)
                }
                Spacer(minLength: 4)
                Text(summary)
                    .fmText(.bodySmall, color: running > 0 ? fm.accent : fm.textSecondary)
                    .lineLimit(1)
            }
            .padding(.horizontal, FmSpace.x2)
            .frame(maxWidth: .infinity, minHeight: 36, alignment: .leading)
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .padding(.top, FmSpace.x1)
        .accessibilityLabel(Text("Folder \(section.name), \(summary)"))
        .accessibilityHint(Text(collapsed ? "Shows its agents" : "Hides its agents"))
        .accessibilityIdentifier("agents-folder-\(section.id)")
    }

    @ViewBuilder private func lineMenu(_ line: AgentLine, _ request: AgentLineDeleteRequest?) -> some View {
        if case .control(let row) = line.source, row.canStop, let center = HostsFeature.agents {
            Button(role: .destructive) {
                Task { try? await center.stop(row) }
            } label: {
                Label("Stop", systemImage: "stop.fill")
            }
        }
        if let request, !deleting.contains(line.id) {
            Button(role: .destructive) {
                swipedOpen = nil
                ask(request)
            } label: {
                Label("Delete", systemImage: "trash")
            }
            .accessibilityIdentifier("agents-line-menu-delete")
        }
    }

    @ViewBuilder private func lineRow(_ line: AgentLine, nowMs: Int) -> some View {
        if case .control = line.source, HostsFeature.agents == nil {
            EmptyView()
        } else {
            Button {
                open(line)
            } label: {
                AgentLineRow(
                    line: line, nowMs: nowMs, loader: loader, view: listView, trailing: retryTrailing(line),
                    eagleFeed: listView == .eagle ? eagle : nil, watch: dots?.controller, watchKeys: watchKeys(line))
            }
            .buttonStyle(.plain)
        }
    }

    private func retryTrailing(_ line: AgentLine) -> AnyView? {
        guard case .entry(.launched(let repo, let session, _, _)) = line.source, session.isRetryable else { return nil }
        return AnyView(retryButton(repo, session))
    }

    private func open(_ line: AgentLine) {
        var run: UnifiedAgentRow?
        if case .entry(.launched(let repo, let session, _, _)) = line.source, let center = HostsFeature.agents {
            run = AgentLines.runRow(repo, session, rows: center.rows)
        }
        opened = AgentOpenedLine(line: line, run: run)
    }

    @ViewBuilder private func page(_ target: AgentOpenedLine) -> some View {
        readPage(target.line) {
            switch target.line.source {
            case .entry(.desktop(let repo, let session)):
                AgentSessionDetailScreen(repo: repo, session: session, ownerAccount: model.ownerAccount(for: repo))
                    .shellPageClearance()
            case .entry(.launched(let repo, let session, let pulls, _)):
                if let center = HostsFeature.agents, let run = target.run {
                    AgentRunScreen(center: center, row: run)
                } else {
                    LaunchedAgentSessionScreen(repo: repo, session: session, pulls: pulls)
                }
            case .control(let row):
                if let center = HostsFeature.agents {
                    AgentRunScreen(center: center, row: row)
                }
            }
        }
    }

    private func readPage<Page: View>(_ line: AgentLine, @ViewBuilder _ page: @escaping () -> Page) -> some View {
        AgentLazyPage(page)
            .onAppear { reads.open(line) }
            .onDisappear { reads.close(lines.first { $0.readKey == line.readKey } ?? line) }
    }

    private func watchKeys(_ line: AgentLine) -> [String] {
        line.status == .running ? AgentLines.watchKeys(line, rows: HostsFeature.agents?.rows ?? []) : []
    }

    @ViewBuilder private func retryButton(_ repo: Repository, _ session: AgentLaunchSession) -> some View {
        if model.retrying.contains(session.sessionId) {
            FmCircularProgressIndicator(size: 12, strokeWidth: 1.5)
                .frame(width: 24, height: 24)
        } else {
            Button {
                Task {
                    if let message = await model.retry(repo, session) {
                        notice = FmNotice(message)
                    }
                }
            } label: {
                FmIcon("refresh", size: 15, color: fm.textSecondary)
                    .frame(width: 24, height: 24)
            }
            .buttonStyle(.plain)
            .accessibilityLabel(Text(session.canResume ? "Retry on its branch" : "Retry"))
        }
    }

    @ViewBuilder private var controlNotes: some View {
        if let center = HostsFeature.agents {
            ForEach([center.desktopNote, center.cloudNote].filter { !$0.isEmpty }, id: \.self) { (note: String) in
                Label(note, systemImage: "exclamationmark.triangle")
                    .font(.system(size: 10))
                    .foregroundStyle(fm.warning)
                    .lineLimit(2)
                    .fmMessageTools(note)
            }
        }
    }

    @ViewBuilder private var bulkActions: some View {
        if let client = AgentOpsFeature.client, !model.desktopRunning.isEmpty {
            BulkAgentActionsBar(
                client: client,
                candidates: model.desktopRunning.map { (item: AgentRunningItem) -> BulkCandidate in
                    BulkCandidate(id: item.session.id, isActive: item.session.isActive, stoppable: item.session.isActive, merged: item.session.merged)
                },
                onFinished: { (message: String) in
                    bulkNotice = FmNotice(message)
                    Task { await model.refresh() }
                }
            )
            .padding(.top, FmSpace.x2)
            FmNoticeLine($bulkNotice)
                .padding(.horizontal, FmSpace.x2)
        }
    }

    private var emptyList: some View {
        VStack(alignment: .leading, spacing: FmSpace.x3) {
            FmEmptyState(
                icon: "smart_toy_outlined",
                title: "No agent sessions yet",
                message: canStart
                    ? "Send a prompt from the compose button and follow it here from the mirror that runs it to its branch, pull request and merge."
                    : "Agent sessions pushed by desktop nodes will appear here."
            )
            if !model.hasDesktopNode {
                FmNeedsDesktopNote(
                    detail: "This account has no desktop or server node enrolled. A prompt still goes to a mirror of the repository when one can run agents; otherwise it runs on this phone, against a mirror of the repository, on your own provider key or with Gemma 4 on this iPhone.",
                    icon: "smart_toy_outlined"
                )
            }
        }
    }
}

/// Shown when the relay holds owner-sealed sessions this phone cannot open.
struct SealedSessionsNotice: View {
    @Environment(\.fmPalette) private var fm
    let count: Int

    var body: some View {
        HStack(alignment: .top, spacing: FmSpace.x2) {
            FmIcon("lock_outline", size: 18, color: fm.textTertiary)
            VStack(alignment: .leading, spacing: FmSpace.x1) {
                Text(count == 1 ? "1 session is encrypted for another device" : "\(count) sessions are encrypted for another device")
                    .fmText(.labelMedium)
                Text("Agent transcripts are readable only on the owner devices they were sealed to. Sign in on this device and let it enroll, then the desktop seals new sessions to it too.")
                    .fmText(.bodySmall, color: fm.textSecondary, height: 1.4)
                    .fixedSize(horizontal: false, vertical: true)
            }
        }
        .padding(FmSpace.x3)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(fm.bgOverlay)
        .clipShape(FmRoundedShape(radius: FmRadius.md))
        .overlay(RoundedRectangle(cornerRadius: FmRadius.md).stroke(fm.border, lineWidth: 1))
    }
}

struct AgentOpenedLine: Identifiable, Hashable {
    let id = UUID()
    let line: AgentLine
    let run: UnifiedAgentRow?

    static func == (a: AgentOpenedLine, b: AgentOpenedLine) -> Bool { a.id == b.id }

    func hash(into hasher: inout Hasher) { hasher.combine(id) }
}

struct AgentLazyPage<Page: View>: View {
    let build: () -> Page

    init(@ViewBuilder _ build: @escaping () -> Page) {
        self.build = build
    }

    var body: some View { build() }
}

extension AgentLineSort {
    var symbol: String {
        switch self {
        case .recent: return "clock"
        case .running: return "play.fill"
        case .status: return "circle.lefthalf.filled"
        case .priority: return "exclamationmark"
        case .name: return "textformat"
        case .repo: return "folder"
        case .changes: return "plus.forwardslash.minus"
        }
    }
}

struct AgentToolChip: ViewModifier {
    func body(content: Content) -> some View {
        content
            .frame(height: 30)
            .shellGlass(RoundedRectangle(cornerRadius: FmRadius.md, style: .continuous))
            .contentShape(RoundedRectangle(cornerRadius: FmRadius.md, style: .continuous))
    }
}
