// Watch, hear and feel agents work, the desktop's Watch window on the phone.
//
// The field is the desktop's, drawn the same way: each agent a body in the
// colour of the instrument it plays, its model's face inside it, its project
// and task circling it and its last reply scrolling on a dot display under
// it, seated in rows by what it is doing. Every note it plays is the same
// note the desktop plays, on the same grid, and the same figure bursts off
// its body as the note lands. The list of sounds names every kind, lit while
// it sounds, with the desktop's two switches on each row.
//
// Tap an agent for its card; double tap to take it in or out of the mix.
// Logic: FMAgentWatch's AgentWatchController.

import FMAgentWatch
import FMAgents
import FMApi
import FMBase
import FMChat
import FMControl
import SwiftUI

/// What rings the controller's doorbell: the runs may have moved on.
struct AgentWatchTrigger {
    /// Starts listening; the returned closure stops.
    let subscribe: @MainActor (@escaping @MainActor () -> Void) -> (@MainActor () -> Void)

    /// The relay's "agent sessions changed" bell (account events `agents`).
    static func agentList(_ signal: AgentListSignal) -> AgentWatchTrigger {
        AgentWatchTrigger { (fire: @escaping @MainActor () -> Void) in
            let token = signal.addListener {
                MainActor.assumeIsolated { fire() }
            }
            return { signal.removeListener(token) }
        }
    }

    /// The local network's `agents.changed`, which re-reads the list and
    /// notifies; a local read is cheap, so it follows closely.
    static func localControl(_ control: LocalControlService) -> AgentWatchTrigger {
        AgentWatchTrigger { (fire: @escaping @MainActor () -> Void) in
            let token = control.addListener {
                MainActor.assumeIsolated { fire() }
            }
            return { control.removeListener(token) }
        }
    }

    /// The desktop's `agents.changed` over whichever link the control center holds (LAN or relay).
    /// The desktop also rings it when an agent started outside ForkMesh writes to its transcript.
    static func controlCenter(_ hub: DesktopControlHub) -> AgentWatchTrigger {
        AgentWatchTrigger { (fire: @escaping @MainActor () -> Void) in
            let id = hub.addEventTap { name, _ in
                if name == "agents.changed" { fire() }
            }
            return { hub.removeEventTap(id) }
        }
    }

    static func liveReports(_ signal: AgentListSignal) -> AgentWatchTrigger {
        AgentWatchTrigger { (fire: @escaping @MainActor () -> Void) in
            let token = signal.addLiveListener {
                MainActor.assumeIsolated { fire() }
            }
            return { signal.removeLiveListener(token) }
        }
    }

    /// Every one of `triggers`.
    static func all(_ triggers: [AgentWatchTrigger]) -> AgentWatchTrigger {
        AgentWatchTrigger { (fire: @escaping @MainActor () -> Void) in
            let stops = triggers.map { $0.subscribe(fire) }
            return { stops.forEach { $0() } }
        }
    }

    /// The run on this phone: every event it adds.
    static func agentSessions(_ sessions: AgentSessionService) -> AgentWatchTrigger {
        AgentWatchTrigger { (fire: @escaping @MainActor () -> Void) in
            let token = sessions.addListener {
                MainActor.assumeIsolated { fire() }
            }
            return { sessions.removeListener(token) }
        }
    }
}

/// What the app registers for the Watch screen (AgentWatchInstaller): where
/// its sound and touch go. Tests and previews register a silent one.
@MainActor
final class AgentWatchEnvironment {
    let makeOutput: @MainActor () -> AgentWatchOutput

    init(makeOutput: @escaping @MainActor () -> AgentWatchOutput) {
        self.makeOutput = makeOutput
    }
}

/// The local-network target for one row of the desktop's Agents list.
@MainActor
func localWatchTarget(_ control: LocalControlService, _ row: LocalAgentRow) -> AgentWatchTarget {
    let carried = LocalControlCarrier(control)
    return localNetworkWatchTarget(
        repoLabel: row.repoLabel,
        name: row.name,
        title: row.title,
        prompt: row.prompt,
        session: row.session,
        transcript: { (id: Int) in try await carried.transcript(id) },
        currentStatus: { (id: Int) in await carried.status(id) })
}

/// LocalControlService for the target's `@Sendable` loads; every call hops to
/// the main actor the service lives on.
private final class LocalControlCarrier: @unchecked Sendable {
    private let control: LocalControlService

    init(_ control: LocalControlService) { self.control = control }

    func transcript(_ id: Int) async throws -> String {
        try await control.transcript(id)
    }

    func status(_ id: Int) async -> String? {
        await MainActor.run { control.agents?.sessions.first { $0.session.id == id }?.session.status }
    }
}

struct AgentWatchScreen: View {
    static let soundKey = "agentWatch/sound"
    static let feelKey = "agentWatch/feel"
    static let hiddenKey = "agentWatch/hidden"
    static let mutedKey = "agentWatch/muted"
    static let backdropKey = "agentWatch/backdrop"
    static let workingOnlyKey = "agentWatch/workingOnly"
    static let motionSoundKey = "agentWatch/motionSound"

    let targets: [AgentWatchTarget]
    let title: String
    let trigger: AgentWatchTrigger?
    /// How long a burst of rings is gathered into one read.
    let debounce: Double
    let roster: ControlWatchRoster?

    init(
        targets: [AgentWatchTarget], title: String = "Watch agents", trigger: AgentWatchTrigger? = nil,
        debounce: Double = 0.8, roster: ControlWatchRoster? = nil
    ) {
        self.targets = targets
        self.title = title
        self.trigger = trigger
        self.debounce = debounce
        self.roster = roster
    }

    @Environment(\.scenePhase) private var scenePhase
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @State private var controller: AgentWatchController?
    @State private var unsubscribe: (@MainActor () -> Void)?
    @State private var following: ControlWatchRoster?
    @State private var catchUp: Task<Void, Never>?
    @State private var leds = WatchLeds()
    @State private var hidden: Set<AgentMotion> = []
    @State private var backdrop: WatchBackdrop = .space
    @State private var showSounds = false
    @State private var selected: WatchedAgent?
    @State private var animating = true
    @State private var tilt = WatchTilt()
    @State private var motionSound: WatchMotionSound?
    @State private var motionOn = true

    var body: some View {
        Group {
            if let controller {
                content(controller)
            } else {
                watchBackground.color
            }
        }
        .background(watchBackground.color.ignoresSafeArea())
        .navigationTitle("")
        .navigationBarTitleDisplayMode(.inline)
        .toolbarBackground(.hidden, for: .navigationBar)
        .toolbarColorScheme(.dark, for: .navigationBar)
        .toolbar { toolbar }
        .bareShellTop()
        .onAppear(perform: appear)
        .onDisappear(perform: disappear)
        .onChange(of: scenePhase) { _, phase in
            if phase == .active {
                controller?.resume()
                animating = true
            } else {
                controller?.pause()
            }
            followTilt()
        }
        .onChange(of: reduceMotion) { _, _ in followTilt() }
    }

    // MARK: Lifecycle

    private var hearsMotion: Bool {
        motionOn && (controller?.sound ?? false) && scenePhase == .active && motionSound != nil
    }

    private func followTilt() {
        if scenePhase == .active && (!reduceMotion || hearsMotion) {
            tilt.start()
        } else {
            tilt.stop()
        }
        motionSound?.play(hearsMotion)
    }

    private func appear() {
        if motionSound == nil {
            let made = WatchMotionSound()
            motionSound = made
            tilt.onReading = { [weak made] (reading: WatchMotionReading) in made?.feel(reading) }
        }
        motionOn = prefs?.bool(forKey: Self.motionSoundKey) ?? true
        if controller != nil {
            followTilt()
            return
        }
        let environment = Services.get(AgentWatchEnvironment.self)
        let output = environment?.makeOutput() ?? PlatformAgentWatchOutput()
        let made = AgentWatchController(
            targets: targets + (roster?.targets() ?? []), output: output, debounce: debounce)
        loadPreferences(made)
        controller = made
        unsubscribe = trigger?.subscribe { [weak made] in made?.ring() }
        if let roster {
            following = roster
            roster.feeds.onChange = { [weak made] in made?.ring() }
            roster.watch { [weak made] (fresh: [AgentWatchTarget]) in
                guard let made else { return }
                if made.add(fresh) > 0 { animating = true }
                made.ring()
            }
            let feeds = roster.feeds
            catchUp = Task { @MainActor [weak made] in
                while !Task.isCancelled {
                    try? await Task.sleep(nanoseconds: 3_000_000_000)
                    if Task.isCancelled { return }
                    if feeds.lagging { made?.ring() }
                }
            }
        }
        followTilt()
        Task { await made.start() }
    }

    private func disappear() {
        tilt.stop()
        motionSound?.stop()
        unsubscribe?()
        unsubscribe = nil
        catchUp?.cancel()
        catchUp = nil
        following?.stop()
        following = nil
        controller?.dispose()
    }

    private var prefs: KeyValueStore? { Services.get(KeyValueStore.self) }

    private func loadPreferences(_ controller: AgentWatchController) {
        let prefs = self.prefs
        func byLabel(_ key: String) -> Set<AgentMotion> {
            let labels = Set(prefs?.stringList(forKey: key) ?? [])
            return Set(AgentMotion.allCases.filter { labels.contains($0.label) })
        }
        controller.sound = prefs?.bool(forKey: Self.soundKey) ?? true
        controller.feel = prefs?.bool(forKey: Self.feelKey) ?? true
        controller.workingOnly = prefs?.bool(forKey: Self.workingOnlyKey) ?? false
        for motion in byLabel(Self.mutedKey) {
            controller.setMuted(motion, true)
        }
        hidden = byLabel(Self.hiddenKey)
        backdrop = WatchBackdrop(rawValue: prefs?.string(forKey: Self.backdropKey) ?? "") ?? .space
    }

    // MARK: Toolbar

    @ToolbarContentBuilder private var toolbar: some ToolbarContent {
        ToolbarItemGroup(placement: .topBarTrailing) {
            if let controller {
                Button {
                    controller.sound.toggle()
                    prefs?.set(controller.sound, forKey: Self.soundKey)
                    followTilt()
                } label: {
                    Image(systemName: controller.sound ? "speaker.wave.2" : "speaker.slash")
                        .foregroundColor(watchForeground.color)
                }
                .accessibilityLabel(Text(controller.sound ? "Stop hearing everything" : "Hear everything"))
                .accessibilityIdentifier("agent-watch-sound")
                Button {
                    controller.feel.toggle()
                    prefs?.set(controller.feel, forKey: Self.feelKey)
                    if controller.feel { FmHaptics.commit(.toggleOn) }
                } label: {
                    Image(systemName: controller.feel ? "iphone.radiowaves.left.and.right" : "iphone.slash")
                        .foregroundColor(watchForeground.color)
                }
                .accessibilityLabel(Text(controller.feel ? "Stop vibrating" : "Feel the agents"))
                .accessibilityIdentifier("agent-watch-feel")
                Button {
                    withAnimation(.spring(response: 0.32, dampingFraction: 0.86)) { showSounds.toggle() }
                    animating = true
                } label: {
                    Image(systemName: showSounds ? "chevron.up.circle.fill" : "list.bullet.below.rectangle")
                        .foregroundColor(watchForeground.color)
                }
                .accessibilityLabel(Text(showSounds ? "Hide activity" : "Show activity"))
                .accessibilityIdentifier("agent-watch-sounds-toggle")
                Menu {
                    Toggle("Sound follows motion", isOn: Binding(
                        get: { motionOn },
                        set: { (value: Bool) in
                            motionOn = value
                            prefs?.set(value, forKey: Self.motionSoundKey)
                            followTilt()
                        }))
                    Toggle("Working only", isOn: Binding(
                        get: { controller.workingOnly },
                        set: { (value: Bool) in
                            controller.workingOnly = value
                            prefs?.set(value, forKey: Self.workingOnlyKey)
                            animating = true
                        }))
                    Divider()
                    Picker("Backdrop", selection: Binding(
                        get: { backdrop },
                        set: { (value: WatchBackdrop) in
                            backdrop = value
                            prefs?.set(value.label, forKey: Self.backdropKey)
                        })
                    ) {
                        ForEach(WatchBackdrop.allCases, id: \.self) { (choice: WatchBackdrop) in
                            Text(choice.label).tag(choice)
                        }
                    }
                    .pickerStyle(.inline)
                } label: {
                    Image(systemName: "ellipsis.circle")
                        .foregroundColor(watchForeground.color)
                }
                .accessibilityLabel(Text("View"))
                .accessibilityIdentifier("agent-watch-menu")
            }
        }
    }

    // MARK: Content

    @ViewBuilder private func content(_ controller: AgentWatchController) -> some View {
        // Read so SwiftUI redraws on every change the controller reports.
        let _ = controller.revision
        GeometryReader { (proxy: GeometryProxy) in
            let insets = proxy.safeAreaInsets
            let wide = proxy.size.width >= 700
            let area = CGSize(
                width: max(60, proxy.size.width - insets.leading - insets.trailing), height: proxy.size.height)
            let panelWidth = wide ? min(max(proxy.size.width * 0.3, 280), 360) : min(proxy.size.width - 20, 360)
            let panelHeight = max(160, (proxy.size.height - insets.top - insets.bottom) * 0.62)
            ZStack(alignment: .topLeading) {
                WatchSkyView(backdrop: backdrop, tilt: tilt, still: reduceMotion)
                    .ignoresSafeArea()
                TimelineView(.animation(
                    minimumInterval: animating ? nil : 1.0, paused: !animating && !showSounds)
                ) { _ in
                    let _ = controller.advance()
                    let now = controller.now
                    ZStack(alignment: .topTrailing) {
                        field(controller, now, area, insets, packed: !wide)
                        if showSounds {
                            WatchSoundList(
                                controller: controller, now: now, hidden: hidden, onToggle: toggleKind,
                                title: title, summary: summaryLines(controller, now))
                                .frame(width: panelWidth, height: panelHeight)
                                .watchPanel()
                                .padding(.top, insets.top + 6)
                                .padding(.trailing, insets.trailing + 10)
                                .transition(.move(edge: .top).combined(with: .opacity))
                        }
                    }
                }
            }
        }
        .ignoresSafeArea()
        .onChange(of: controller.revision) { _, _ in animating = true }
    }

    private func field(
        _ controller: AgentWatchController, _ now: Double, _ area: CGSize, _ insets: EdgeInsets, packed: Bool
    ) -> some View {
        let width = Double(area.width)
        let shown = Double(area.height - insets.top - insets.bottom)
        let height = max(shown, WatchFieldLayout.heightFor(width, controller.agents, packed: packed))
        let size = CGSize(width: width, height: height)
        let layout = WatchFieldLayout(size, controller.agents, packed: packed)
        return ScrollView(.vertical) {
            Canvas { (ctx: inout GraphicsContext, canvasSize: CGSize) in
                let painter = WatchFieldPainter(
                    controller: controller, layout: layout, leds: leds, faces: faces(controller),
                    hidden: hidden, selected: selected, nodeLabel: nodeLabel,
                    traffic: trafficLine(controller, now), now: now)
                painter.paint(ctx, canvasSize)
                settle(controller, layout, now)
            }
            .frame(width: size.width, height: size.height)
            .contentShape(Rectangle())
            .gesture(
                SpatialTapGesture(count: 2)
                    .onEnded { (value: SpatialTapGesture.Value) in doubleTap(controller, layout, value.location) }
                    .exclusively(
                        before: SpatialTapGesture(count: 1)
                            .onEnded { (value: SpatialTapGesture.Value) in tap(controller, layout, value.location) }))
            .accessibilityElement()
            .accessibilityLabel(Text(headline(controller)))
            .accessibilityIdentifier("agent-watch-field")
            .padding(EdgeInsets(
                top: insets.top, leading: insets.leading, bottom: insets.bottom, trailing: insets.trailing))
        }
        .scrollIndicators(.hidden)
        .watchEdgesClear()
        .frame(width: area.width + insets.leading + insets.trailing, height: area.height)
        .ignoresSafeArea()
    }

    /// Stops the frames once nothing moves: a quiet field costs nothing. The
    /// next change the controller reports starts them again.
    private func settle(_ controller: AgentWatchController, _ layout: WatchFieldLayout, _ now: Double) {
        let gliding = layout.glide(now)
        if gliding || controller.moving || WatchFieldPainter.ledsMoving(controller, leds, layout) { return }
        DispatchQueue.main.async {
            if !controller.moving { animating = false }
        }
    }

    private func faces(_ controller: AgentWatchController) -> [ObjectIdentifier: Image] {
        var faces: [ObjectIdentifier: Image] = [:]
        for agent in controller.agents {
            let name = watchFaceName(provider: agent.target.provider, model: agent.target.model)
            faces[ObjectIdentifier(agent)] = Image("agent-face-\(name)")
        }
        return faces
    }

    private var nodeLabel: String {
        roster == nil && !targets.isEmpty && targets.allSatisfy { $0.subtitle == "On this phone" }
            ? "this phone" : "your desktop"
    }

    // MARK: Gestures

    private func tap(_ controller: AgentWatchController, _ layout: WatchFieldLayout, _ at: CGPoint) {
        if showSounds {
            withAnimation(.spring(response: 0.32, dampingFraction: 0.86)) { showSounds = false }
            animating = true
            return
        }
        let agent = layout.agentAt(at, controller.now, shown: { controller.shown($0) })
        selected = agent === selected ? nil : agent
        if let agent { controller.greet(agent) }
        animating = true
    }

    private func doubleTap(_ controller: AgentWatchController, _ layout: WatchFieldLayout, _ at: CGPoint) {
        guard let agent = layout.agentAt(at, controller.now, shown: { controller.shown($0) }) else { return }
        FmHaptics.commit(.select)
        controller.toggleListening(agent)
        selected = agent
        animating = true
    }

    private func toggleKind(_ motion: AgentMotion, _ sound: Bool) {
        guard let controller else { return }
        if sound {
            controller.setMuted(motion, !controller.muted.contains(motion))
            prefs?.set(controller.muted.map(\.label).sorted(), forKey: Self.mutedKey)
        } else {
            if hidden.remove(motion) == nil { hidden.insert(motion) }
            prefs?.set(hidden.map(\.label).sorted(), forKey: Self.hiddenKey)
        }
        animating = true
    }

    /// The fleet in one line, and the tapped agent's card: what the field
    /// says to VoiceOver (`headline`, `hoverLines`).
    private func headline(_ controller: AgentWatchController) -> String {
        let agents = controller.agents
        if agents.isEmpty {
            return "No agents to watch. Start one, or open a session to replay it."
        }
        let working = agents.filter { $0.state == .running || $0.state == .starting }.count
        let waiting = agents.filter { $0.state == .waiting }.count
        var parts = [
            controller.loading
                ? "Reading \(agents.count == 1 ? "the run" : "the runs")"
                : "\(agents.count) \(agents.count == 1 ? "agent" : "agents")",
        ]
        if working > 0 { parts.append("\(working) working") }
        if waiting > 0 { parts.append("\(waiting) waiting on you") }
        var lines = [parts.joined(separator: ", ")]
        if let selected { lines += watchCardLines(controller, selected) }
        return lines.joined(separator: "\n")
    }
}

private struct WatchSkyView: View {
    let backdrop: WatchBackdrop
    let tilt: WatchTilt
    var still = false

    var body: some View {
        let look = still ? CGPoint.zero : tilt.look
        Canvas { (ctx: inout GraphicsContext, size: CGSize) in
            WatchSky.paint(ctx, size, backdrop, look: look)
        }
    }
}

extension AgentWatchScreen {
    private func trafficLine(_ controller: AgentWatchController, _ now: Double) -> String {
        var parts = ["\u{2193} \(watchBytes(controller.bytesPerSecond(now)))/s"]
        if let feeds = following?.feeds, feeds.count > 0 {
            parts.append("\(feeds.live)/\(feeds.count) live")
        }
        return parts.joined(separator: " \u{00B7} ")
    }

    private func summaryLines(_ controller: AgentWatchController, _ now: Double) -> [String] {
        let agents = controller.agents
        let working = agents.filter { $0.state == .running || $0.state == .starting }.count
        let waiting = agents.filter { $0.state == .waiting }.count
        let resting = agents.count - working - waiting
        var lines = ["\(working) working \u{00B7} \(waiting) waiting on you \u{00B7} \(resting) idle or done"]
        var network = "Network \u{2193} \(watchBytes(controller.bytesPerSecond(now)))/s"
        if let feeds = following?.feeds, feeds.count > 0 {
            network += " \u{00B7} \(feeds.live) of \(feeds.count) streams live \u{00B7} \(watchBytes(Double(feeds.bytesIn))) in"
        } else {
            network += " \u{00B7} \(watchBytes(Double(controller.bytesIn))) in"
        }
        lines.append(network)
        let since = now - controller.lastReadSeconds
        let read = controller.readsDone == 0 ? "no reads yet" : since < 2 ? "read just now" : "read \(Int(since)) s ago"
        let events = controller.landCount.values.reduce(0, +)
        lines.append("\(controller.eventsPerMinute(now)) events/min \u{00B7} \(events) so far \u{00B7} \(read)")
        let ranked = agents.sorted { $0.rank < $1.rank }
        if let top = ranked.first(where: { now - $0.lastEventSeconds < 60 }) {
            let name = top.id > 0 ? "#\(top.id) \(top.target.projectName)" : top.target.projectName
            let doing = top.last.map { " \u{00B7} \($0.motion.label)" } ?? ""
            lines.append("Busiest: \(name)\(doing)")
        }
        return lines
    }
}

func watchBytes(_ bytes: Double) -> String {
    if bytes < 1024 { return "\(Int(bytes.rounded())) B" }
    if bytes < 1024 * 1024 { return String(format: "%.1f KB", bytes / 1024) }
    return String(format: "%.1f MB", bytes / (1024 * 1024))
}

extension View {
    @ViewBuilder func watchEdgesClear() -> some View {
        #if compiler(>=6.2)
        if #available(iOS 26.0, *) {
            self.scrollEdgeEffectHidden(true, for: .all)
        } else {
            self
        }
        #else
        self
        #endif
    }

    @ViewBuilder func watchPanel() -> some View {
        let shape = RoundedRectangle(cornerRadius: 22, style: .continuous)
        #if compiler(>=6.2)
        if #available(iOS 26.0, *) {
            self.clipShape(shape)
                .glassEffect(.regular.tint(watchBackground.withAlpha(0.45).color), in: shape)
        } else {
            self.background(watchBackground.withAlpha(0.82).color, in: shape)
                .clipShape(shape)
        }
        #else
        self.background(watchBackground.withAlpha(0.82).color, in: shape)
            .clipShape(shape)
        #endif
    }
}

// MARK: - Entry points

/// The "Watch & listen" face of a link to the Watch screen, in the agent
/// screens' outlined style.
struct AgentWatchEntryLabel: View {
    @Environment(\.fmPalette) private var fm
    let text: String

    var body: some View {
        HStack(spacing: FmSpace.x2) {
            Image(systemName: "waveform")
                .font(.system(size: 14, weight: .semibold))
                .foregroundColor(fm.accent)
            Text(text)
                .fmText(.labelMedium, color: fm.accent)
        }
        .padding(.horizontal, FmSpace.x3)
        .frame(height: 36)
        .overlay(
            RoundedRectangle(cornerRadius: FmRadius.full)
                .stroke(fm.border, lineWidth: 1)
        )
    }
}

extension AgentWatchScreen {
    /// Every desktop session that is running, or, when none is, the few that
    /// ran last to replay. nil with nothing to watch.
    @MainActor
    static func forRelay(_ groups: [AgentRepoGroup], ownerAccount: (Repository) -> String) -> AgentWatchScreen? {
        guard let api = Services.get(ApiService.self) else { return nil }
        let all: [(Repository, AgentSession)] = groups.flatMap { group in group.sessions.map { (group.repo, $0) } }
        if all.isEmpty { return nil }
        var chosen = all.filter { $0.1.isActive }
        let live = !chosen.isEmpty
        if !live {
            chosen = Array(all.sorted { a, b in
                max(a.1.startedAtMs, a.1.createdAtMs) > max(b.1.startedAtMs, b.1.createdAtMs)
            }.prefix(3))
        }
        return AgentWatchScreen(
            targets: chosen.prefix(12).map { relayWatchTarget(api, $0.0, $0.1, ownerAccount: ownerAccount($0.0)) },
            title: live ? "Watch agents" : "Replay agents",
            trigger: Services.get(AgentListSignal.self).map { AgentWatchTrigger.agentList($0) })
    }

    /// One desktop session read through the relay (the session screen's
    /// "Watch & listen").
    @MainActor
    static func forRelaySession(_ repo: Repository, _ session: AgentSession, ownerAccount: String) -> AgentWatchScreen? {
        if let center = HostsFeature.agents, let row = center.rows.first(where: { (row: UnifiedAgentRow) -> Bool in
            row.isDesktop && !row.outside && !row.elsewhere && row.localId == session.id
                && row.repoName.lowercased() == repo.name.lowercased()
        }) {
            return forControl(center, [row])
        }
        guard let api = Services.get(ApiService.self) else { return nil }
        return AgentWatchScreen(
            targets: [relayWatchTarget(api, repo, session, ownerAccount: ownerAccount)],
            title: session.isActive ? "Watch agent" : "Replay agent",
            trigger: Services.get(AgentListSignal.self).map { AgentWatchTrigger.agentList($0) })
    }

    /// The Agents tab's Watch: every running agent the control center lists, streamed from the desktop
    /// while it is in reach, the running relay sessions it does not list, and each agent that starts while
    /// the screen shows. With nothing running it replays the last few. Without a control center it is `forRelay`.
    @MainActor
    static func forAgentsTab(
        _ groups: [AgentRepoGroup], center: AgentControlCenter?, ownerAccount: (Repository) -> String
    ) -> AgentWatchScreen? {
        guard let center else { return forRelay(groups, ownerAccount: ownerAccount) }
        let listed = Set(center.rows.filter { $0.isDesktop && !$0.elsewhere }.map { "\($0.repoName.lowercased())#\($0.localId)" })
        let live = center.rows.contains(where: \.isActive)
        var targets: [AgentWatchTarget] = []
        if let api = Services.get(ApiService.self) {
            for group in groups {
                for session in group.sessions
                where session.isActive && !listed.contains("\(group.repo.name.lowercased())#\(session.id)") {
                    targets.append(relayWatchTarget(api, group.repo, session, ownerAccount: ownerAccount(group.repo)))
                }
            }
        }
        var replay = false
        if targets.isEmpty && !live {
            if let relay = forRelay(groups, ownerAccount: ownerAccount) {
                targets = relay.targets
            } else if center.rows.isEmpty {
                return nil
            }
            replay = true
        }
        let replayRows = replay && targets.isEmpty
        let roster = ControlWatchRoster(center) { (center: AgentControlCenter) -> [UnifiedAgentRow] in
            let running = center.rows.filter(\.isActive)
            return running.isEmpty && replayRows ? Array(center.rows.prefix(3)) : running
        }
        return AgentWatchScreen(
            targets: Array(targets.prefix(12)), title: replay ? "Replay agents" : "Watch agents",
            trigger: .all(watchTriggers(center)), debounce: 0.4, roster: roster)
    }

    /// Rows of the control center's list (the run screen's waveform), each streamed from the desktop.
    @MainActor
    static func forControl(_ center: AgentControlCenter, _ rows: [UnifiedAgentRow]) -> AgentWatchScreen? {
        if rows.isEmpty { return nil }
        let chosen = Array(rows.prefix(12))
        return AgentWatchScreen(
            targets: [],
            title: (rows.contains { $0.isActive } ? "Watch agent" : "Replay agent") + (rows.count == 1 ? "" : "s"),
            trigger: .all(watchTriggers(center)),
            debounce: 0.4,
            roster: ControlWatchRoster(center) { _ in chosen })
    }

    @MainActor
    private static func watchTriggers(_ center: AgentControlCenter) -> [AgentWatchTrigger] {
        var triggers: [AgentWatchTrigger] = [.controlCenter(center.hub)]
        if let signal = Services.get(AgentListSignal.self) {
            triggers.append(.agentList(signal))
            triggers.append(.liveReports(signal))
        }
        return triggers
    }

    /// The desktop's sessions over the local network: the running ones, or
    /// the last few to replay when nothing is running.
    @MainActor
    static func forLocal(_ control: LocalControlService, _ rows: [LocalAgentRow]) -> AgentWatchScreen {
        let live = rows.filter { $0.session.isActive }
        let chosen = (live.isEmpty ? Array(rows.prefix(3)) : live).prefix(12)
        return AgentWatchScreen(
            targets: chosen.map { localWatchTarget(control, $0) },
            title: live.isEmpty ? "Replay agents" : "Watch agents",
            trigger: .localControl(control),
            debounce: 0.4)
    }

    /// The run on this phone (the coding agent screen's watch button).
    @MainActor
    static func forDevice(_ sessions: AgentSessionService, repoName: String, running: Bool) -> AgentWatchScreen {
        AgentWatchScreen(
            targets: [deviceWatchTarget(sessions, repoName)],
            title: running ? "Watch agent" : "Replay agent",
            trigger: .agentSessions(sessions),
            debounce: 0.12)
    }
}
