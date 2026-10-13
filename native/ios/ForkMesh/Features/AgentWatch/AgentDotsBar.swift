import FMAccount
import FMAgentWatch
import FMAgents
import FMBase
import FMControl
import SwiftUI

@MainActor
@Observable
final class AgentDotsFeed {
    static let shared = AgentDotsFeed()

    var list: AgentsListModel?
}

@MainActor
final class AgentDotsWatch {
    let controller: AgentWatchController
    private var unsubscribe: (@MainActor () -> Void)?
    private var roster: ControlWatchRoster?

    init(_ screen: AgentWatchScreen) {
        let made = AgentWatchController(
            targets: screen.targets + (screen.roster?.targets() ?? []), output: SilentAgentWatchOutput(),
            debounce: screen.debounce, sound: false, feel: false)
        controller = made
        unsubscribe = screen.trigger?.subscribe { [weak made] in made?.ring() }
        if let roster = screen.roster {
            self.roster = roster
            roster.feeds.onChange = { [weak made] in made?.ring() }
            roster.watch { [weak made] (fresh: [AgentWatchTarget]) in
                guard let made else { return }
                made.add(fresh)
                made.ring()
            }
        }
        Task { await made.start() }
    }

    func add(_ screen: AgentWatchScreen) {
        controller.add(screen.targets)
        controller.ring()
    }

    func stop() {
        unsubscribe?()
        unsubscribe = nil
        roster?.stop()
        roster = nil
        controller.dispose()
    }
}


struct AgentDotsBar: View {
    static let height: CGFloat = 34
    static let dot: CGFloat = 26
    static let gap: CGFloat = 7
    static let margin: CGFloat = 4
    static let most: Int = 12

    @Environment(\.fmPalette) private var fm
    @Environment(\.scenePhase) private var scenePhase
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @State private var settings: SettingsService? = Services.get(SettingsService.self)
    @State private var feed: AgentDotsFeed = AgentDotsFeed.shared
    @State private var flight: ShellLaunchFlight = ShellLaunchFlight.shared
    @State private var watch: AgentDotsWatch?
    @State private var animating: Bool = true
    @State private var opened: Bool = false

    var body: some View {
        if settings?.showAgentDots ?? false {
            bar
        }
    }

    private var groups: [AgentRepoGroup] { feed.list?.groups ?? [] }

    private var rowIds: [String] { HostsFeature.agents?.rows.map(\.id) ?? [] }

    private var launching: Int { flight.spawns.count }

    private var seatIds: [Int] {
        guard let controller = watch?.controller else { return [] }
        let _ = controller.revision
        return controller.agents.map(\.id)
    }

    private var bar: some View {
        Group {
            if let controller = watch?.controller, AgentDotsBar.seats(controller) || launching > 0 {
                dots(controller)
            } else if launching > 0 {
                dots(nil)
            } else {
                empty
            }
        }
        .padding(.horizontal, FmSpace.x2)
        .frame(maxWidth: .infinity, alignment: .leading)
        .frame(height: AgentDotsBar.height)
        .background(fm.bgRaised)
        .overlay(alignment: .bottom) {
            Rectangle().fill(fm.border).frame(height: 1)
        }
        .onGeometryChange(for: CGRect.self, of: { $0.frame(in: .global) }) { (frame: CGRect) in
            flight.row = frame
        }
        .onAppear(perform: sync)
        .onDisappear {
            flight.row = .zero
            stop()
        }
        .onChange(of: groups) { (_: [AgentRepoGroup], _: [AgentRepoGroup]) in sync() }
        .onChange(of: rowIds) { (_: [String], _: [String]) in sync() }
        .onChange(of: seatIds, initial: true) { (_: [Int], ids: [Int]) in flight.claim(ids) }
        .onChange(of: launching) { (_: Int, _: Int) in animating = true }
        .onChange(of: scenePhase) { (_: ScenePhase, phase: ScenePhase) in
            if phase == .active {
                watch?.controller.resume()
                animating = true
            } else {
                watch?.controller.pause()
            }
        }
        .sheet(isPresented: $opened) {
            watchSheet
        }
        .accessibilityIdentifier("agent-dots")
    }

    private var empty: some View {
        HStack(spacing: FmSpace.x2) {
            Circle()
                .strokeBorder(fm.textTertiary.opacity(0.6), style: StrokeStyle(lineWidth: 1.5, dash: [2.5, 3]))
                .frame(width: AgentDotsBar.dot - 6, height: AgentDotsBar.dot - 6)
                .padding(.leading, AgentDotsBar.margin + 3)
            Text("No agents yet")
                .font(Font.system(size: FmType.micro, weight: FmType.medium))
                .foregroundStyle(fm.textTertiary)
        }
        .accessibilityElement(children: .combine)
    }

    private func dots(_ controller: AgentWatchController?) -> some View {
        let _ = controller?.revision
        let launching = self.launching
        return TimelineView(.animation(minimumInterval: 1.0 / 30.0, paused: !animating)) { _ in
            let _ = controller?.advance()
            let now = controller?.now ?? ProcessInfo.processInfo.systemUptime
            let agents = controller?.agents ?? []
            let seated = AgentDotsBar.seated(agents)
            let extra = min(launching, AgentDotsBar.most - seated.count)
            HStack(spacing: FmSpace.x2) {
                Canvas { (ctx: inout GraphicsContext, size: CGSize) in
                    let painter = AgentDotsPainter(
                        controller: controller, agents: seated, now: now, spin: !reduceMotion,
                        cutout: fm.bgRaised, waiting: AgentDotsPainter.paused(fm.warning), failed: fm.danger,
                        done: fm.success, launching: extra)
                    painter.paint(ctx, size)
                    settle(controller)
                }
                .frame(width: AgentDotsBar.width(seated.count + extra))
                if !seated.isEmpty || extra > 0 {
                    Text(AgentDotsBar.summary(agents, launching: extra))
                        .font(Font.system(size: FmType.micro, weight: FmType.medium).monospacedDigit())
                        .foregroundStyle(fm.textSecondary)
                        .lineLimit(1)
                        .minimumScaleFactor(0.8)
                    Spacer(minLength: 0)
                    Image(systemName: "waveform")
                        .font(.system(size: 12, weight: .semibold))
                        .foregroundStyle(fm.accent)
                }
            }
        }
        .contentShape(Rectangle())
        .onTapGesture {
            if controller?.agents.isEmpty ?? true { return }
            FmHaptics.selection()
            opened = true
        }
        .onChange(of: controller?.revision ?? 0) { (_: Int, _: Int) in animating = true }
        .accessibilityElement(children: .ignore)
        .accessibilityLabel(Text("Agent dots: \(AgentDotsBar.summary(controller?.agents ?? [], launching: launching))"))
        .accessibilityHint(Text("Opens Watch agents"))
        .accessibilityAddTraits(.isButton)
    }

    @ViewBuilder private var watchSheet: some View {
        let list = feed.list
        NavigationStack {
            if let screen = AgentWatchScreen.forAgentsTab(
                groups, center: HostsFeature.agents, ownerAccount: { list?.ownerAccount(for: $0) ?? "" }) {
                screen
                    .toolbar {
                        ToolbarItem(placement: .cancellationAction) {
                            Button("Done") { opened = false }
                        }
                    }
            } else {
                Text("No agents to watch")
                    .fmText(.bodySmall, color: fm.textSecondary)
                    .toolbar {
                        ToolbarItem(placement: .cancellationAction) {
                            Button("Done") { opened = false }
                        }
                    }
            }
        }
    }

    private func sync() {
        let list = feed.list
        guard let screen = AgentWatchScreen.forAgentsTab(
            groups, center: HostsFeature.agents, ownerAccount: { list?.ownerAccount(for: $0) ?? "" }) else { return }
        if let watch {
            watch.add(screen)
        } else {
            watch = AgentDotsWatch(screen)
        }
        animating = true
    }

    private func stop() {
        watch?.stop()
        watch = nil
    }

    private func settle(_ controller: AgentWatchController?) {
        if launching > 0 { return }
        guard let controller else { return }
        if controller.moving { return }
        if !reduceMotion && controller.agents.contains(where: { $0.state.isLive }) { return }
        DispatchQueue.main.async {
            if !controller.moving { animating = false }
        }
    }

    static func seats(_ controller: AgentWatchController) -> Bool {
        let _ = controller.revision
        return !controller.agents.isEmpty
    }

    static func seated(_ agents: [WatchedAgent]) -> [WatchedAgent] {
        let live = agents.filter { $0.state.isLive }
        let rest: [WatchedAgent] = agents.filter { !$0.state.isLive }.reversed()
        return Array((live + rest).prefix(most))
    }

    static func width(_ count: Int) -> CGFloat {
        let n = CGFloat(max(count, 1))
        return n * dot + (n - 1) * gap + margin * 2
    }

    static func seat(_ index: Int, in row: CGRect) -> CGPoint {
        let place = CGFloat(min(max(index, 0), most - 1))
        return CGPoint(x: row.minX + FmSpace.x2 + margin + dot / 2 + place * (dot + gap), y: row.midY)
    }

    static func summary(_ agents: [WatchedAgent], launching: Int = 0) -> String {
        if agents.isEmpty && launching == 0 { return "No agents yet" }
        let working = agents.filter { $0.state == .running || $0.state == .starting }.count
        let waiting = agents.filter { $0.state == .waiting }.count
        let failed = agents.filter { $0.state == .failed }.count
        var parts: [String] = []
        if launching > 0 { parts.append("\(launching) starting") }
        if working > 0 { parts.append("\(working) working") }
        if waiting > 0 { parts.append("\(waiting) waiting") }
        if failed > 0 { parts.append("\(failed) failed") }
        if parts.isEmpty { parts.append("\(agents.count) done") }
        return parts.joined(separator: " · ")
    }
}

@MainActor
struct AgentDotsPainter {
    let controller: AgentWatchController?
    let agents: [WatchedAgent]
    let now: Double
    let spin: Bool
    let cutout: Color
    let waiting: Color
    let failed: Color
    let done: Color
    var launching: Int = 0

    static let flying: Int = 6
    static let stars: Int = 22
    static let launchHue: Double = 0.74

    static func paused(_ fallback: Color) -> Color {
        Color.fmHex("#e3742f") ?? fallback
    }

    func paint(_ ctx: GraphicsContext, _ size: CGSize) {
        let side = Double(AgentDotsBar.dot)
        let step = side + Double(AgentDotsBar.gap)
        let mid = Double(size.height) / 2
        let r = side / 2 - 2.5
        let centre = { (index: Int) -> CGPoint in
            CGPoint(x: Double(AgentDotsBar.margin) + side / 2 + Double(index) * step, y: mid)
        }
        for (index, agent) in agents.enumerated() {
            paintDot(ctx, agent, centre(index), r)
        }
        for index in 0..<max(launching, 0) {
            paintLaunching(ctx, centre(agents.count + index), r, seed: 7919 + index)
        }
    }

    private func circle(_ c: CGPoint, _ r: Double) -> Path {
        Path(ellipseIn: CGRect(x: Double(c.x) - r, y: Double(c.y) - r, width: r * 2, height: r * 2))
    }

    private func paintDot(_ ctx: GraphicsContext, _ agent: WatchedAgent, _ c: CGPoint, _ r: Double) {
        let hue = agent.hue
        let energy = agent.energyAt(now)
        let state = agent.state
        let live = state == .running || state == .starting
        let ended = state == .done || state == .failed
        let pulses = Array(
            (controller?.pulses ?? []).filter { $0.agent === agent && !$0.pending(now) && !$0.motion.isFaint }
                .suffix(AgentDotsPainter.flying))

        if live || energy > 0.02 {
            drawWatchGlow(ctx, c, r + 3 + 3 * energy, watchInk(hue, 0.95, 0.6, 0.16 + 0.42 * energy))
        }
        if live {
            paintPortal(ctx, c, r, hue: hue, seed: agent.id, energy: energy, pulses: pulses)
        } else {
            paintFace(ctx, agent, c, r, ended: ended, pulses: pulses)
        }
        paintRing(ctx, agent, c, r, live: live, ended: ended)
        paintBadge(ctx, state, c, r)
    }

    private func paintFace(
        _ ctx: GraphicsContext, _ agent: WatchedAgent, _ c: CGPoint, _ r: Double, ended: Bool, pulses: [WatchPulse]
    ) {
        let hue = agent.hue
        let disc = circle(c, r)
        ctx.fill(
            disc,
            with: WatchShader.radial(
                centre: c, radius: r,
                colours: [watchInk(hue, 0.55, 0.3), watchInk(hue, 0.6, 0.1)], stops: nil
            ).shading)
        let name = watchFaceName(provider: agent.target.provider, model: agent.target.model)
        var face = ctx
        face.clip(to: circle(c, r - 0.6))
        face.opacity = ended ? 0.5 : (pulses.isEmpty ? 0.95 : 0.55)
        face.draw(
            Image("agent-face-\(name)"),
            in: CGRect(x: Double(c.x) - r, y: Double(c.y) - r, width: r * 2, height: r * 2))
        if !pulses.isEmpty {
            ctx.fill(disc, with: .color(Color.black.opacity(0.32)))
        }
        var inner = ctx
        inner.clip(to: circle(c, r - 0.8))
        for pulse in pulses {
            paintFlyer(inner, pulse, c, r)
        }
    }

    private func paintPortal(
        _ ctx: GraphicsContext, _ c: CGPoint, _ r: Double, hue: Double, seed: Int, energy: Double, pulses: [WatchPulse]
    ) {
        ctx.fill(
            circle(c, r),
            with: WatchShader.radial(
                centre: c, radius: r,
                colours: [watchInk(hue, 0.8, 0.03), watchInk(hue, 0.7, 0.1), watchInk(hue, 0.75, 0.3)],
                stops: [0, 0.62, 1]
            ).shading)
        var inner = ctx
        inner.clip(to: circle(c, r - 0.6))
        drawWatchGlow(inner, c, r * 0.6, watchInk(hue, 0.9, 0.72, 0.16 + 0.34 * energy))
        let t = spin ? now : 0
        let shown = 9 + 13 * min(max(energy, 0), 1)
        for index in 0..<AgentDotsPainter.stars {
            let weight = min(max(shown - Double(index), 0), 1)
            if weight <= 0 { break }
            let angle = AgentDotsPainter.noise(seed, index, 1) * 2 * Double.pi
            let rate = 0.28 + 0.22 * AgentDotsPainter.noise(seed, index, 3)
            let z = (AgentDotsPainter.noise(seed, index, 2) + t * rate).truncatingRemainder(dividingBy: 1)
            let depth = z * z
            let reach = r * (0.06 + 0.94 * depth)
            let tail = r * depth * (0.08 + 0.22 * energy)
            var streak = Path()
            streak.move(to: watchPolar(c, max(0, reach - tail), angle))
            streak.addLine(to: watchPolar(c, reach, angle))
            let alpha = weight * min(1, z * 4) * (0.55 + 0.45 * energy)
            inner.stroke(
                streak, with: .color(watchInk(hue, 0.35, 0.92, alpha).color),
                style: StrokeStyle(lineWidth: 0.5 + 1.2 * depth, lineCap: .round))
        }
        for pulse in pulses {
            paintStreak(inner, pulse, c, r)
        }
    }

    private func paintStreak(_ ctx: GraphicsContext, _ pulse: WatchPulse, _ c: CGPoint, _ r: Double) {
        let life = pulse.life(now)
        let angle = pulse.angle * 2 * Double.pi
        let strength = min(max(pulse.strength, 0), 1)
        let reach = r * (0.08 + 0.92 * life * life)
        let tail = r * (0.15 + 0.35 * life)
        let alpha = pow(1.0 - life, 0.6)
        let head = watchPolar(c, reach, angle)
        var streak = Path()
        streak.move(to: watchPolar(c, max(0, reach - tail), angle))
        streak.addLine(to: head)
        drawWatchGlow(ctx, head, 2.5 + 2.5 * strength, watchInk(pulse.motion.hue, 0.95, 0.62, 0.6 * alpha))
        ctx.stroke(
            streak, with: .color(watchInk(pulse.motion.hue, 0.95, 0.7, alpha).color),
            style: StrokeStyle(lineWidth: 1.2 + 0.8 * strength, lineCap: .round))
        ctx.fill(circle(head, 0.9 + 0.5 * strength), with: .color(Color.white.opacity(alpha)))
    }

    private func paintLaunching(_ ctx: GraphicsContext, _ c: CGPoint, _ r: Double, seed: Int) {
        let hue = AgentDotsPainter.launchHue
        drawWatchGlow(ctx, c, r + 5, watchInk(hue, 0.95, 0.6, 0.45))
        paintPortal(ctx, c, r, hue: hue, seed: seed, energy: 0.7, pulses: [])
        ctx.stroke(circle(c, r + 0.6), with: .color(watchInk(hue, 0.8, 0.6, 0.5).color), lineWidth: 1.5)
        if spin {
            paintComet(ctx, c, r, hue: hue, seed: seed)
        }
    }

    private func paintFlyer(_ ctx: GraphicsContext, _ pulse: WatchPulse, _ c: CGPoint, _ r: Double) {
        let life = pulse.life(now)
        let turn = pulse.angle * 2 * Double.pi + life * 1.8
        let reach = (r - 4.5) * watchEaseOut(min(1.0, life * 1.5))
        let p = watchPolar(c, reach, turn)
        let pop = life < 0.12 ? 0.4 + life / 0.12 * 0.95 : max(1.0, 1.35 - (life - 0.12) * 2.0)
        let size = (2.3 + 1.3 * min(max(pulse.strength, 0), 1)) * pop
        let alpha = pow(1.0 - life, 0.7)
        drawWatchGlow(ctx, p, size * 2.4, watchInk(pulse.motion.hue, 0.95, 0.62, 0.45 * alpha))
        paintWatchIcon(ctx, pulse.motion, p, size, alpha)
    }

    private func paintRing(
        _ ctx: GraphicsContext, _ agent: WatchedAgent, _ c: CGPoint, _ r: Double, live: Bool, ended: Bool
    ) {
        let ring = circle(c, r + 0.6)
        if agent.state == .waiting {
            ctx.stroke(ring, with: .color(waiting), lineWidth: 2.2)
            return
        }
        let kinds = Array((controller?.kindsOf(agent) ?? []).filter { !$0.0.isFaint }.prefix(6))
        let total = Double(kinds.reduce(0) { $0 + $1.1 })
        let alpha = ended ? 0.4 : 0.7
        if total > 0 {
            let gap = kinds.count > 1 ? 0.22 : 0.0
            var start = -Double.pi / 2
            for (motion, count) in kinds {
                let sweep = Double(count) / total * 2 * Double.pi
                var arc = Path()
                arc.addArc(
                    center: c, radius: r + 0.6, startAngle: .radians(start + gap / 2),
                    endAngle: .radians(start + max(sweep - gap / 2, gap / 2 + 0.05)), clockwise: false)
                ctx.stroke(
                    arc, with: .color(watchInk(motion.hue, 0.9, 0.62, alpha).color),
                    style: StrokeStyle(lineWidth: 2, lineCap: .round))
                start += sweep
            }
        } else {
            ctx.stroke(ring, with: .color(watchInk(agent.hue, 0.8, 0.6, alpha * 0.7).color), lineWidth: 1.5)
        }
        if live && spin {
            paintComet(ctx, c, r, hue: agent.hue, seed: agent.id)
        }
    }

    private func paintComet(_ ctx: GraphicsContext, _ c: CGPoint, _ r: Double, hue: Double, seed: Int) {
        let head = now * 2 * Double.pi * 0.7 + Double(seed % 11) * 0.6
        let bright = watchInk(hue, 0.95, 0.78)
        ctx.stroke(
            circle(c, r + 0.6),
            with: .conicGradient(
                Gradient(colors: [bright.withAlpha(0).color, bright.withAlpha(0).color, bright.color]),
                center: c, angle: .radians(head)),
            lineWidth: 2.4)
        let tip = watchPolar(c, r + 0.6, head)
        drawWatchGlow(ctx, tip, 3.5, watchInk(hue, 1.0, 0.85, 0.9))
        ctx.fill(circle(tip, 1.3), with: .color(.white))
    }

    private func paintBadge(_ ctx: GraphicsContext, _ state: WatchState, _ c: CGPoint, _ r: Double) {
        let colour: Color
        switch state {
        case .failed: colour = failed
        case .done: colour = done
        case .waiting, .running, .starting: return
        }
        let at = watchPolar(c, r * 0.92, Double.pi / 4)
        ctx.fill(circle(at, 4), with: .color(cutout))
        ctx.fill(circle(at, 3), with: .color(colour))
        var mark = Path()
        if state == .done {
            mark.move(to: CGPoint(x: at.x - 1.4, y: at.y))
            mark.addLine(to: CGPoint(x: at.x - 0.3, y: at.y + 1.1))
            mark.addLine(to: CGPoint(x: at.x + 1.5, y: at.y - 1.1))
        } else {
            mark.move(to: CGPoint(x: at.x - 1.1, y: at.y - 1.1))
            mark.addLine(to: CGPoint(x: at.x + 1.1, y: at.y + 1.1))
            mark.move(to: CGPoint(x: at.x + 1.1, y: at.y - 1.1))
            mark.addLine(to: CGPoint(x: at.x - 1.1, y: at.y + 1.1))
        }
        ctx.stroke(mark, with: .color(.white), style: StrokeStyle(lineWidth: 0.9, lineCap: .round, lineJoin: .round))
    }

    static func noise(_ seed: Int, _ index: Int, _ salt: Int) -> Double {
        var x = UInt64(bitPattern: Int64(seed)) &* 0x9E37_79B9_7F4A_7C15
        x ^= UInt64(index &+ 1) &* 0xBF58_476D_1CE4_E5B9
        x ^= UInt64(salt &+ 1) &* 0x94D0_49BB_1331_11EB
        x ^= x >> 31
        x &*= 0xD6E8_FEB8_6659_FD93
        x ^= x >> 32
        return Double(x >> 11) / Double(UInt64(1) << 53)
    }
}

struct AgentLineDot: View {
    static let standard: CGFloat = 26

    @Environment(\.fmPalette) private var fm
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @Environment(\.shellTabHidden) private var tabHidden
    let line: AgentLine
    var watch: AgentWatchController? = nil
    var keys: [String] = []
    var side: CGFloat = AgentLineDot.standard

    private var running: Bool { line.status == .running }

    var body: some View {
        Group {
            if running {
                TimelineView(.animation(minimumInterval: 1.0 / 30.0, paused: reduceMotion || tabHidden)) { _ in
                    let _ = watch?.advance()
                    canvas(now: watch?.now ?? ProcessInfo.processInfo.systemUptime)
                }
            } else {
                canvas(now: 0)
            }
        }
        .frame(width: side, height: side)
        .accessibilityHidden(true)
    }

    private func watched() -> WatchedAgent? {
        guard let watch, !keys.isEmpty else { return nil }
        let _ = watch.revision
        return watch.agents.first { keys.contains($0.target.key) }
    }

    private func canvas(now: Double) -> some View {
        let agent: WatchedAgent? = watched()
        let seed: Int = AgentLineDotPainter.seed(line.id)
        var pulses: [WatchPulse] = []
        var kinds: [(AgentMotion, Int)] = []
        if let watch, let agent, running {
            pulses = Array(
                watch.pulses.filter { $0.agent === agent && !$0.pending(now) && !$0.motion.isFaint }
                    .suffix(AgentDotsPainter.flying))
            kinds = Array(watch.kindsOf(agent).filter { !$0.0.isFaint }.prefix(6))
        }
        let painter = AgentLineDotPainter(
            faceName: line.face, status: line.status, seed: seed, hue: agent?.hue ?? watchScatter(seed),
            energy: agent?.energyAt(now) ?? 0, pulses: pulses, kinds: kinds, now: now, spin: running && !reduceMotion,
            cutout: fm.bgOverlay, waiting: AgentDotsPainter.paused(fm.warning), failed: fm.danger, done: fm.success,
            merged: Color.fmHex("#a371f7") ?? fm.accent, idle: fm.textTertiary)
        return Canvas { (ctx: inout GraphicsContext, size: CGSize) in
            painter.paint(ctx, size)
        }
    }
}

@MainActor
struct AgentLineDotPainter {
    let faceName: String
    let status: AgentLineStatus
    let seed: Int
    let hue: Double
    let energy: Double
    let pulses: [WatchPulse]
    let kinds: [(AgentMotion, Int)]
    let now: Double
    let spin: Bool
    let cutout: Color
    let waiting: Color
    let failed: Color
    let done: Color
    let merged: Color
    let idle: Color

    static let stars: Int = 30
    static let sparks: Int = 4

    static func seed(_ id: String) -> Int {
        var hash: UInt64 = 0xCBF2_9CE4_8422_2325
        for byte in id.utf8 {
            hash ^= UInt64(byte)
            hash = hash &* 0x0000_0100_0000_01B3
        }
        return Int(truncatingIfNeeded: hash >> 1)
    }

    func paint(_ ctx: GraphicsContext, _ size: CGSize) {
        let c = CGPoint(x: Double(size.width) / 2, y: Double(size.height) / 2)
        let r: Double = Double(min(size.width, size.height)) / 2 - 3
        if status == .running {
            paintRunning(ctx, c, r)
        } else {
            paintResting(ctx, c, r)
        }
    }

    private func circle(_ c: CGPoint, _ r: Double) -> Path {
        Path(ellipseIn: CGRect(x: Double(c.x) - r, y: Double(c.y) - r, width: r * 2, height: r * 2))
    }

    private func shifted(_ by: Double) -> Double {
        (hue + by + 2).truncatingRemainder(dividingBy: 1)
    }

    private func drawFace(_ ctx: GraphicsContext, _ c: CGPoint, _ r: Double, opacity: Double) {
        var face = ctx
        face.clip(to: circle(c, r - 0.6))
        face.opacity = opacity
        face.draw(
            Image("agent-face-\(faceName)"),
            in: CGRect(x: Double(c.x) - r, y: Double(c.y) - r, width: r * 2, height: r * 2))
    }

    private func paintRunning(_ ctx: GraphicsContext, _ c: CGPoint, _ r: Double) {
        let t: Double = spin ? now : 0
        let beat: Double = 0.5 + 0.5 * sin(t * 5.3 + Double(seed % 17))
        let lively: Double = min(1, 0.6 + 0.4 * min(max(energy, 0), 1) + 0.15 * beat)
        drawWatchGlow(ctx, c, r + 1.5 + 1.5 * lively, watchInk(hue, 0.95, 0.6, 0.3 + 0.35 * lively))
        ctx.fill(
            circle(c, r),
            with: WatchShader.radial(
                centre: c, radius: r,
                colours: [watchInk(hue, 0.8, 0.03), watchInk(hue, 0.7, 0.12), watchInk(hue, 0.8, 0.34)],
                stops: [0, 0.6, 1]
            ).shading)
        var inner = ctx
        inner.clip(to: circle(c, r - 0.6))
        drawFace(inner, c, r, opacity: 0.2)
        drawWatchGlow(inner, c, r * (0.4 + 0.3 * beat), watchInk(shifted(0.06), 0.9, 0.75, 0.3 + 0.4 * lively))
        paintStars(inner, c, r, t: t, lively: lively)
        paintSparks(inner, c, r, t: t)
        for pulse in pulses {
            paintStreak(inner, pulse, c, r)
        }
        paintRing(ctx, c, r)
        if spin {
            let ahead: Double = t * 2 * Double.pi * 1.1 + Double(seed % 11) * 0.6
            let behind: Double = -t * 2 * Double.pi * 0.75 + 2.4
            paintComet(ctx, c, r, head: ahead, colour: watchInk(hue, 0.95, 0.8), clockwise: true)
            paintComet(ctx, c, r, head: behind, colour: watchInk(shifted(0.5), 0.9, 0.75), clockwise: false)
        }
    }

    private func paintStars(_ ctx: GraphicsContext, _ c: CGPoint, _ r: Double, t: Double, lively: Double) {
        for index in 0..<AgentLineDotPainter.stars {
            let turn: Double = index % 2 == 0 ? 0.35 : -0.35
            let angle: Double = AgentDotsPainter.noise(seed, index, 1) * 2 * Double.pi + t * turn
            let rate: Double = 0.55 + 0.6 * AgentDotsPainter.noise(seed, index, 3)
            let z: Double = (AgentDotsPainter.noise(seed, index, 2) + t * rate).truncatingRemainder(dividingBy: 1)
            let depth: Double = z * z
            let reach: Double = r * (0.05 + 0.95 * depth)
            let tail: Double = r * depth * (0.18 + 0.3 * lively)
            let tint: Double = shifted(0.14 * (AgentDotsPainter.noise(seed, index, 4) - 0.5))
            let alpha: Double = min(1, z * 4) * (0.6 + 0.4 * lively)
            var streak = Path()
            streak.move(to: watchPolar(c, max(0, reach - tail), angle))
            streak.addLine(to: watchPolar(c, reach, angle))
            ctx.stroke(
                streak, with: .color(watchInk(tint, 0.5, 0.9, alpha).color),
                style: StrokeStyle(lineWidth: 0.5 + 1.3 * depth, lineCap: .round))
        }
    }

    private func paintSparks(_ ctx: GraphicsContext, _ c: CGPoint, _ r: Double, t: Double) {
        for index in 0..<AgentLineDotPainter.sparks {
            let way: Double = index % 2 == 0 ? 1 : -1
            let speed: Double = (1.4 + 0.9 * AgentDotsPainter.noise(seed, index, 5)) * way
            let angle: Double = AgentDotsPainter.noise(seed, index, 6) * 2 * Double.pi + t * speed
            let swing: Double = 0.5 + 0.5 * sin(t * (2.1 + Double(index)) + Double(index))
            let p: CGPoint = watchPolar(c, r * (0.25 + 0.45 * swing), angle)
            drawWatchGlow(ctx, p, 2.8, watchInk(shifted(0.2 * Double(index)), 1, 0.7, 0.75))
            ctx.fill(circle(p, 0.8), with: .color(Color.white.opacity(0.9)))
        }
    }

    private func paintStreak(_ ctx: GraphicsContext, _ pulse: WatchPulse, _ c: CGPoint, _ r: Double) {
        let life: Double = pulse.life(now)
        let angle: Double = pulse.angle * 2 * Double.pi
        let strength: Double = min(max(pulse.strength, 0), 1)
        let reach: Double = r * (0.08 + 0.92 * life * life)
        let tail: Double = r * (0.15 + 0.35 * life)
        let alpha: Double = pow(1.0 - life, 0.6)
        let head: CGPoint = watchPolar(c, reach, angle)
        var streak = Path()
        streak.move(to: watchPolar(c, max(0, reach - tail), angle))
        streak.addLine(to: head)
        drawWatchGlow(ctx, head, 2.5 + 2.5 * strength, watchInk(pulse.motion.hue, 0.95, 0.62, 0.6 * alpha))
        ctx.stroke(
            streak, with: .color(watchInk(pulse.motion.hue, 0.95, 0.7, alpha).color),
            style: StrokeStyle(lineWidth: 1.2 + 0.8 * strength, lineCap: .round))
        ctx.fill(circle(head, 0.9 + 0.5 * strength), with: .color(Color.white.opacity(alpha)))
    }

    private func paintRing(_ ctx: GraphicsContext, _ c: CGPoint, _ r: Double) {
        let total = Double(kinds.reduce(0) { $0 + $1.1 })
        guard total > 0 else {
            ctx.stroke(circle(c, r + 0.6), with: .color(watchInk(hue, 0.8, 0.6, 0.6).color), lineWidth: 1.5)
            return
        }
        let gap: Double = kinds.count > 1 ? 0.22 : 0.0
        var start: Double = -Double.pi / 2
        for (motion, count) in kinds {
            let sweep: Double = Double(count) / total * 2 * Double.pi
            var arc = Path()
            arc.addArc(
                center: c, radius: r + 0.6, startAngle: .radians(start + gap / 2),
                endAngle: .radians(start + max(sweep - gap / 2, gap / 2 + 0.05)), clockwise: false)
            ctx.stroke(
                arc, with: .color(watchInk(motion.hue, 0.9, 0.62, 0.75).color),
                style: StrokeStyle(lineWidth: 2, lineCap: .round))
            start += sweep
        }
    }

    private func paintComet(
        _ ctx: GraphicsContext, _ c: CGPoint, _ r: Double, head: Double, colour: WatchRGBA, clockwise: Bool
    ) {
        let clear: Color = colour.withAlpha(0).color
        let colours: [Color] = clockwise ? [clear, clear, colour.color] : [colour.color, clear, clear]
        ctx.stroke(
            circle(c, r + 0.6),
            with: .conicGradient(Gradient(colors: colours), center: c, angle: .radians(head)),
            lineWidth: 2.2)
        let tip: CGPoint = watchPolar(c, r + 0.6, head)
        drawWatchGlow(ctx, tip, 3, colour.withAlpha(0.9))
        ctx.fill(circle(tip, 1.1), with: .color(.white))
    }

    private func paintResting(_ ctx: GraphicsContext, _ c: CGPoint, _ r: Double) {
        let ended: Bool = status == .success || status == .merged || status == .failed || status == .stopped
        ctx.fill(
            circle(c, r),
            with: WatchShader.radial(
                centre: c, radius: r,
                colours: [watchInk(hue, 0.55, 0.3), watchInk(hue, 0.6, 0.1)], stops: nil
            ).shading)
        drawFace(ctx, c, r, opacity: ended ? 0.55 : (status == .queued ? 0.7 : 0.95))
        let ring = circle(c, r + 0.6)
        switch status {
        case .waiting:
            ctx.stroke(ring, with: .color(waiting), lineWidth: 2.2)
        case .queued:
            ctx.stroke(ring, with: .color(idle), style: StrokeStyle(lineWidth: 1.5, dash: [2, 2.2]))
        case .success:
            ctx.stroke(ring, with: .color(done.opacity(0.7)), lineWidth: 1.5)
        case .merged:
            ctx.stroke(ring, with: .color(merged.opacity(0.75)), lineWidth: 1.5)
        case .failed:
            ctx.stroke(ring, with: .color(failed.opacity(0.75)), lineWidth: 1.5)
        case .stopped, .other, .running:
            ctx.stroke(ring, with: .color(idle.opacity(0.7)), lineWidth: 1.2)
        }
        paintBadge(ctx, c, r)
    }

    private func paintBadge(_ ctx: GraphicsContext, _ c: CGPoint, _ r: Double) {
        let colour: Color
        switch status {
        case .success: colour = done
        case .merged: colour = merged
        case .failed, .stopped: colour = failed
        case .running, .queued, .waiting, .other: return
        }
        let at: CGPoint = watchPolar(c, r * 0.92, Double.pi / 4)
        ctx.fill(circle(at, 4), with: .color(cutout))
        ctx.fill(circle(at, 3), with: .color(colour))
        if status == .stopped {
            ctx.fill(Path(CGRect(x: at.x - 1.1, y: at.y - 1.1, width: 2.2, height: 2.2)), with: .color(.white))
            return
        }
        var mark = Path()
        if status == .failed {
            mark.move(to: CGPoint(x: at.x - 1.1, y: at.y - 1.1))
            mark.addLine(to: CGPoint(x: at.x + 1.1, y: at.y + 1.1))
            mark.move(to: CGPoint(x: at.x + 1.1, y: at.y - 1.1))
            mark.addLine(to: CGPoint(x: at.x - 1.1, y: at.y + 1.1))
        } else {
            mark.move(to: CGPoint(x: at.x - 1.4, y: at.y))
            mark.addLine(to: CGPoint(x: at.x - 0.3, y: at.y + 1.1))
            mark.addLine(to: CGPoint(x: at.x + 1.5, y: at.y - 1.1))
        }
        ctx.stroke(mark, with: .color(.white), style: StrokeStyle(lineWidth: 0.9, lineCap: .round, lineJoin: .round))
    }
}
