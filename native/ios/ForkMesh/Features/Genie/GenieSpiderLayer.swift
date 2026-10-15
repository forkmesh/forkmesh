import FMApi
import FMAssistant
import FMBase
import SwiftUI
import UIKit

struct GenieSpiderLayer: View {
    var hidden: Bool = false
    var floor: CGFloat? = nil

    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @Environment(\.scenePhase) private var scenePhase
    @State private var hub: GenieHub = GenieHub.shared
    @State private var crawler: GenieCrawler = GenieCrawler.shared
    @State private var motion: GenieMotion = GenieMotion.shared
    @State private var cues: GenieCues = GenieCues.shared
    @State private var counts: ShellCounts = ShellCounts.shared
    @State private var coach: GenieHandCoach = GenieHandCoach.shared
    @State private var screen: GenieScreenPlayer = GenieScreenPlayer.shared
    @State private var outreach: GenieOutreachCoach = GenieOutreachCoach.shared
    @State private var notifications: NotificationService? = Services.get(NotificationService.self)
    @State private var area: CGSize = .zero
    @GestureState private var grip: GenieGrip = GenieGrip()
    @State private var movedAt: Date = .distantPast
    @ScaledMetric(relativeTo: .body) private var scaled: CGFloat = GenieCrawler.base

    nonisolated static let clearEvery: Double = 1.5

    private var size: CGFloat { min(scaled, GenieCrawler.base * 1.35) }

    private var scale: CGFloat { GenieHub.fitted(hub.scale * grip.pinch) }

    private var shown: CGFloat { size * scale }

    private var mood: GenieMood { GenieMood(hub.session?.phase ?? .idle) }

    private var paused: Bool {
        hidden || scenePhase != .active || (reduceMotion && mood != .speaking && !crawler.moving)
    }

    var body: some View {
        sessionWatch(sceneWatch(stage))
    }

    private var hearing: String { hub.session?.hearing ?? "" }

    private var overheard: GenieOverheard? { hub.session?.overheard }

    private var phase: GeniePhase { hub.session?.phase ?? .idle }

    private var alwaysOn: Bool { hub.session?.alwaysListening ?? false }

    private var unread: Int { notifications?.unreadCount ?? 0 }

    private func spring(_ response: Double, _ damping: Double) -> Animation? {
        reduceMotion ? nil : .spring(response: response, dampingFraction: damping)
    }

    private var stage: some View {
        GeometryReader { (proxy: GeometryProxy) in
            ZStack(alignment: .topLeading) {
                if coach.on {
                    GenieHandOverlay(coach: coach, crawler: crawler, shown: shown)
                }
                attachments
                spider
            }
            .frame(width: proxy.size.width, height: proxy.size.height, alignment: .topLeading)
            .onAppear { area = proxy.size }
            .onChange(of: proxy.size) { (_: CGSize, next: CGSize) in area = next }
        }
        .ignoresSafeArea()
        .overlay(alignment: .top) { spoken }
        .opacity(hidden ? 0 : 1)
        .allowsHitTesting(!hidden)
        .onChange(of: area) { (_: CGSize, _: CGSize) in layout() }
        .onChange(of: shown) { (_: CGFloat, _: CGFloat) in layout() }
        .onChange(of: floor) { (_: CGFloat?, _: CGFloat?) in layout() }
    }

    private func sceneWatch(_ content: some View) -> some View {
        content
            .onAppear {
                crawler.hidden = hidden
                screen.hidden = hidden
                outreach.stageHidden = hidden
                screen.active = scenePhase == .active
                motion.begin()
                hub.scene(active: scenePhase == .active)
                hub.refreshVitals()
            }
            .onDisappear {
                motion.end()
                coach.pause()
            }
            .onChange(of: hidden) { (_: Bool, now: Bool) in
                crawler.hidden = now
                screen.hidden = now
                outreach.stageHidden = now
                if now {
                    outreach.dismiss()
                    screen.stop()
                    coach.pause()
                } else if scenePhase == .active {
                    coach.resume()
                }
            }
            .task { await crawler.run() }
            .task { await screen.run() }
            .task { await outreach.run() }
            .task {
                while !Task.isCancelled {
                    try? await Task.sleep(for: .seconds(GenieSpiderLayer.clearEvery))
                    if !hidden && scenePhase == .active {
                        crawler.keepClear()
                    }
                }
            }
            .onChange(of: scenePhase) { (_: ScenePhase, phase: ScenePhase) in
                hub.scene(active: phase == .active)
                screen.active = phase == .active
                if phase != .active { screen.stop() }
                if phase == .active { hub.refreshVitals() }
                if phase == .active, !hidden {
                    coach.resume()
                } else {
                    coach.pause()
                }
            }
            .onChange(of: hub.holdsStill) { (_: Bool, still: Bool) in
                if still { crawler.halt() }
            }
            .onChange(of: grip.dragging) { (_: Bool, dragging: Bool) in
                guard !dragging else { return }
                Task { @MainActor in
                    guard crawler.held else { return }
                    release(.zero)
                }
            }
    }

    private func sessionWatch(_ content: some View) -> some View {
        content
            .onChange(of: hearing) { (old: String, words: String) in
                guard !words.isEmpty else { return }
                let spot: GenieSpot? = hub.session?.brain.toolbox.spot(words)
                hub.heard(words, spot: spot, final: false, fresh: old.isEmpty)
            }
            .onChange(of: overheard) { (old: GenieOverheard?, heard: GenieOverheard?) in
                guard let heard = heard else { return }
                hub.heard(heard.text, spot: heard.spot, final: heard.final, fresh: old?.id != heard.id)
            }
            .onChange(of: phase) { (old: GeniePhase, phase: GeniePhase) in
                if phase == .listening && old != .listening {
                    GenieSounds.shared.play(.listen)
                }
            }
            .onChange(of: alwaysOn) { (_: Bool, on: Bool) in
                if !on { hub.alwaysListeningFailed() }
            }
            .onChange(of: unread) { (old: Int, new: Int) in
                cues.unreadChanged(from: old, to: new)
            }
            .onChange(of: counts.agents) { (old: Int, new: Int) in
                hub.agentsChanged(from: old, to: new)
            }
    }

    private func layout() {
        guard area.width > 0, area.height > 0 else { return }
        let full: CGRect = CGRect(origin: .zero, size: area)
        let insets: UIEdgeInsets = GenieSpiderLayer.insets
        let safe: CGRect = CGRect(
            x: insets.left, y: insets.top, width: max(area.width - insets.left - insets.right, 1),
            height: max(area.height - insets.top - insets.bottom, 1))
        crawler.layout(full: full, safe: safe, floor: floor, side: shown)
    }

    @MainActor private static var insets: UIEdgeInsets {
        let scenes: [UIWindowScene] = UIApplication.shared.connectedScenes.compactMap { $0 as? UIWindowScene }
        let scene: UIWindowScene? = scenes.first { $0.activationState == .foregroundActive } ?? scenes.first
        return scene?.windows.first { $0.isKeyWindow }?.safeAreaInsets ?? UIEdgeInsets(top: 47, left: 0, bottom: 34, right: 0)
    }

    private var spider: some View {
        TimelineView(.animation(minimumInterval: 1.0 / frameRate, paused: paused)) {
            (timeline: TimelineViewDefaultContext) in
            let now: Date = timeline.date
            let place: CGPoint = crawler.point(at: now)
            let glimmers: [GenieGlimmer] = magic(at: now, place: place)
            ZStack(alignment: .topLeading) {
                if let effect = screen.marks(at: now) {
                    GenieScreenMarks(effect: effect, now: now, place: place)
                }
                if let anchor = crawler.silk(at: now) {
                    silk(from: anchor, to: place)
                }
                if let end = screen.thread(from: place, at: now) {
                    thread(from: place, to: end)
                }
                if !glimmers.isEmpty {
                    GenieMagicView(glimmers: glimmers, front: false)
                }
                accessible(touchable(avatar(at: now, place: place)))
                    .animation(spring(0.3, 0.75), value: crawler.held)
                    .animation(spring(0.45, 0.7), value: crawler.parked)
                    .position(place)
                if !glimmers.isEmpty {
                    GenieMagicView(glimmers: glimmers, front: true)
                }
            }
        }
    }

    private var frameRate: Double {
        if crawler.track?.flying == true || crawler.tumbling { return 60 }
        return coach.showing?.trick.magic != nil ? 30 : 24
    }

    private func magic(at now: Date, place: CGPoint) -> [GenieGlimmer] {
        guard !reduceMotion, let show = coach.showing, show.trick.magic != nil else { return [] }
        let stage: GenieMagicStage = GenieMagicStage(
            spider: place, side: Double(shown), turn: crawler.rotation(at: now), target: screen.target(at: now),
            full: CGRect(origin: .zero, size: area))
        return show.trick.glimmers(age: now.timeIntervalSince(show.start), stage: stage)
    }

    private func silk(from anchor: CGPoint, to place: CGPoint) -> some View {
        let dx: CGFloat = anchor.x - place.x
        let dy: CGFloat = anchor.y - place.y
        let length: CGFloat = max(hypot(dx, dy), 1)
        let ux: CGFloat = dx / length
        let uy: CGFloat = dy / length
        let top: CGFloat = shown * GenieGravity.stand
        let body: CGFloat = min(shown * 0.28, length)
        return Path { (path: inout Path) in
            path.move(to: CGPoint(x: anchor.x + ux * top, y: anchor.y + uy * top))
            path.addLine(to: CGPoint(x: place.x + ux * body, y: place.y + uy * body))
        }
        .stroke(GenieInk.silver.opacity(0.75), style: StrokeStyle(lineWidth: 0.9, lineCap: .round))
        .allowsHitTesting(false)
        .accessibilityHidden(true)
    }

    private func thread(from place: CGPoint, to end: CGPoint) -> some View {
        Path { (path: inout Path) in
            path.move(to: place)
            path.addLine(to: end)
        }
        .stroke(GenieInk.silver.opacity(0.75), style: StrokeStyle(lineWidth: 0.9, lineCap: .round))
        .allowsHitTesting(false)
        .accessibilityHidden(true)
    }

    private func avatar(at now: Date, place: CGPoint) -> some View {
        let level: Double = hub.session?.hearing.isEmpty == false ? 0.8 : 0.2
        let lean: CGSize = reduceMotion ? .zero : motion.lean
        let alarm: Double = coach.engaged ? coach.state.nearness : 0
        let held: Bool = crawler.held
        let glow: Color = GenieInk.glow.opacity(hub.alwaysListening ? 0.5 : 0.25)
        let lift: CGFloat = held ? 1.12 : 1
        let radius: CGFloat = held ? 16 : 8
        let drop: CGFloat = held ? 10 : 3
        return GenieAvatar(
            mood: mood, level: level, cue: cues.cue, ambient: true,
            lean: lean, vitals: hub.vitals, at: now, stride: crawler.stride(at: now),
            show: coach.showing, gaze: coach.gaze(from: place) ?? screen.gaze(from: place, at: now), alarm: alarm)
            .frame(width: shown, height: shown)
            .overlay { ear }
            .overlay { handBadge }
            .rotationEffect(.degrees(crawler.rotation(at: now)))
            .modifier(GenieTwist(manipulation: coach.manipulation, base: scale))
            .scaleEffect(lift)
            .shadow(color: glow, radius: radius, y: drop)
    }

    private func touchable(_ content: some View) -> some View {
        content
            .contentShape(Circle().scale(0.82))
            .onTapGesture { tapped() }
            .onLongPressGesture(minimumDuration: 0.45, maximumDistance: 10) { hub.openMenu() }
            .simultaneousGesture(handleGesture)
            .hoverEffect(.lift)
    }

    private func tapped() {
        guard !crawler.held, !grip.dragging, Date().timeIntervalSince(movedAt) > 0.35 else { return }
        hub.tap()
    }

    private func accessible(_ content: some View) -> some View {
        let listen: Text = Text(hub.alwaysListening ? "Stop always listening" : "Always listen")
        let tuck: Text = Text(crawler.parked == nil ? "Tuck into the edge" : "Come out")
        let hands: Text = Text(coach.on ? "Stop hand play" : "Play with hands")
        return content
            .accessibilityElement()
            .accessibilityAddTraits(.isButton)
            .accessibilityLabel(Text("Genie"))
            .accessibilityValue(Text(accessibilityValue))
            .accessibilityHint(Text("Tap for a tip. Touch and hold for more options, including Talk and Hide the spider. Drag it to an edge to tuck it away. Pinch to resize. Turn on hand play to use the front camera."))
            .accessibilityAction { hub.tap() }
            .accessibilityAction(named: Text("Talk")) { hub.openBubble() }
            .accessibilityAction(named: Text("More options")) { hub.openMenu() }
            .accessibilityAction(named: listen) { hub.toggleAlwaysListening() }
            .accessibilityAction(named: tuck) { toggleTuck() }
            .accessibilityAction(named: Text("Make bigger")) { hub.resized(to: hub.scale * 1.25) }
            .accessibilityAction(named: Text("Make smaller")) { hub.resized(to: hub.scale * 0.8) }
            .accessibilityAction(named: Text("Do a trick")) { coach.surprise() }
            .accessibilityAction(named: hands) { coach.toggle() }
            .accessibilityAction(named: Text("Hide the spider")) { hub.setSpiderHidden(true, fromMenu: true) }
            .accessibilityIdentifier("shell-genie-button")
    }

    private var accessibilityValue: String {
        var parts: [String] = []
        if hub.alwaysListening { parts.append("Always listening") }
        if coach.on { parts.append("Hand play on") }
        if let show = coach.showing { parts.append(show.trick.title) }
        if let vitals = hub.vitals { parts.append(vitals.level.title) }
        if crawler.parked != nil { parts.append("Tucked away") }
        return parts.joined(separator: ", ")
    }

    private func toggleTuck() {
        withAnimation(reduceMotion ? nil : .spring(response: 0.45, dampingFraction: 0.7)) {
            if crawler.parked == nil {
                crawler.park()
            } else {
                crawler.unpark()
            }
        }
    }

    @ViewBuilder private var ear: some View {
        if hub.alwaysListening {
            TimelineView(.animation(minimumInterval: 1.0 / 20.0, paused: paused)) { (timeline: TimelineViewDefaultContext) in
                let beat: Double = timeline.date.timeIntervalSinceReferenceDate.truncatingRemainder(dividingBy: 2.4) / 2.4
                ZStack {
                    Circle()
                        .strokeBorder(GenieInk.glow.opacity(0.7 - beat * 0.6), lineWidth: 2)
                        .scaleEffect(0.8 + beat * 0.25)
                    Image(systemName: "ear.fill")
                        .font(.system(size: shown * 0.13, weight: .bold))
                        .foregroundStyle(GenieSheet.night)
                        .padding(shown * 0.045)
                        .background(Circle().fill(GenieInk.glow))
                        .offset(x: shown * 0.3, y: -shown * 0.36)
                }
            }
            .allowsHitTesting(false)
        }
    }

    @ViewBuilder private var handBadge: some View {
        if coach.on {
            Image(systemName: coach.engaged ? "hand.raised.fill" : "hand.raised")
                .font(.system(size: shown * 0.12, weight: .bold))
                .foregroundStyle(GenieSheet.night)
                .padding(shown * 0.045)
                .background(Circle().fill(coach.thinking ? GenieInk.violet : GenieInk.glow))
                .offset(x: -shown * 0.32, y: -shown * 0.36)
                .allowsHitTesting(false)
        }
    }

    private var handleGesture: some Gesture {
        SimultaneousGesture(MagnifyGesture(), DragGesture(minimumDistance: 12, coordinateSpace: .global))
            .updating($grip) { (value: SimultaneousGesture<MagnifyGesture, DragGesture>.Value, state: inout GenieGrip, _: inout Transaction) in
                if let magnify = value.first {
                    state.pinch = magnify.magnification
                    state.pinching = true
                }
                state.dragging = !state.pinching && value.second != nil
            }
            .onChanged { (value: SimultaneousGesture<MagnifyGesture, DragGesture>.Value) in
                movedAt = Date()
                guard value.first == nil, !grip.pinching, let translation = value.second?.translation else { return }
                crawler.drag(by: translation)
                GenieTouchCoach.shared.moved(to: crawler.spot)
            }
            .onEnded { (value: SimultaneousGesture<MagnifyGesture, DragGesture>.Value) in
                movedAt = Date()
                if value.first == nil, let drag = value.second, crawler.held {
                    release(drag.velocity)
                }
                if let magnify = value.first {
                    withAnimation(reduceMotion ? nil : .spring(response: 0.35, dampingFraction: 0.75)) {
                        hub.resized(to: hub.scale * magnify.magnification)
                    }
                }
            }
    }

    private func release(_ velocity: CGSize) {
        withAnimation(spring(0.4, 0.72)) {
            let from: CGPoint = crawler.spot
            let parked: Bool = crawler.release(velocity: velocity) != nil
            hub.dropped(parked: parked)
            GenieTouchCoach.shared.released(at: from, velocity: velocity, parked: parked)
        }
    }

    private var anchor: CGPoint { crawler.spot }

    private var onLeft: Bool { anchor.x > area.width / 2 }

    private var growsUp: Bool { anchor.y > area.height * 0.42 }

    private var sideAlignment: Alignment {
        switch (onLeft, growsUp) {
        case (true, true): return .bottomTrailing
        case (true, false): return .topTrailing
        case (false, true): return .bottomLeading
        case (false, false): return .topLeading
        }
    }

    private var bubbleWidth: CGFloat {
        min(280, max(190, area.width - shown - 40))
    }

    private var reach: CGFloat {
        crawler.parked == .left || crawler.parked == .right ? shown * 0.45 : shown * 0.62
    }

    private var attachments: some View {
        Color.clear
            .frame(width: shown, height: shown)
            .overlay(alignment: sideAlignment) { side }
            .overlay(alignment: .top) { flyersView }
            .position(anchor)
    }

    @ViewBuilder private var side: some View {
        Group {
            if hub.menuOpen {
                GenieSpiderMenu(hub: hub, crawler: crawler, coach: coach, onTuck: toggleTuck)
                    .transition(.scale(scale: 0.4, anchor: onLeft ? .trailing : .leading).combined(with: .opacity))
            } else if hub.bubbleOpen, let session = hub.session {
                GenieMiniBubble(session: session, hub: hub, onLeft: onLeft)
                    .transition(.scale(scale: 0.4, anchor: onLeft ? .trailing : .leading).combined(with: .opacity))
            } else if let ticker = hub.ticker {
                GenieTickerView(ticker: ticker, listening: hub.alwaysListening, onLeft: onLeft) {
                    hub.openBubble()
                }
                .transition(.move(edge: onLeft ? .trailing : .leading).combined(with: .opacity))
            } else if hub.alwaysListening && DictationMeter.shared.voicing && !hub.busy {
                GenieWavePill()
                    .transition(.move(edge: onLeft ? .trailing : .leading).combined(with: .opacity))
            }
        }
        .frame(width: bubbleWidth, alignment: onLeft ? .trailing : .leading)
        .fixedSize(horizontal: false, vertical: true)
        .offset(x: onLeft ? -reach : reach)
        .animation(reduceMotion ? nil : .spring(response: 0.35, dampingFraction: 0.8), value: hub.ticker?.id)
        .animation(reduceMotion ? nil : .easeOut(duration: 0.25), value: DictationMeter.shared.voicing)
    }

    @ViewBuilder private var spoken: some View {
        if let line = hub.line, !hub.menuOpen, !hub.bubbleOpen {
            GenieLineView(line: line) { hub.dismissLine() }
                .frame(maxWidth: 360)
                .padding(.top, FmSpace.x2)
                .padding(.horizontal, FmSpace.x4)
                .transition(.move(edge: .top).combined(with: .opacity))
        }
    }

    private var flyersView: some View {
        VStack(spacing: FmSpace.x2) {
            if let offer = outreach.offer {
                GenieOutreachCard(offer: offer, onWrite: { outreach.write() }, onClose: { outreach.dismiss() })
                    .id(offer.id)
                    .transition(.scale(scale: 0.2, anchor: .bottom).combined(with: .offset(y: shown * 0.4)).combined(with: .opacity))
            }
            if let show = coach.showing, !show.line.isEmpty {
                GenieTrickLine(show: show)
                    .id(show.id)
                    .transition(.scale(scale: 0.5, anchor: .bottom).combined(with: .opacity))
            }
            ForEach(hub.flyers.reversed()) { (flyer: GenieFlyer) in
                GenieFlyerView(flyer: flyer) { hub.run(flyer) }
                    .transition(
                        .asymmetric(
                            insertion: .scale(scale: 0.2, anchor: .bottom).combined(with: .offset(y: shown * 0.5)).combined(with: .opacity),
                            removal: .offset(y: -40).combined(with: .opacity)))
            }
        }
        .fixedSize()
        .alignmentGuide(VerticalAlignment.top) { (d: ViewDimensions) in d[VerticalAlignment.bottom] + FmSpace.x1 }
    }
}

struct GenieHintChip: View {
    let hint: GenieHint
    let onRun: () -> Void

    var body: some View {
        Button(action: onRun) {
            HStack(spacing: FmSpace.x1) {
                Image(systemName: hint.spot?.symbol ?? "lightbulb.fill")
                    .font(.caption2.weight(.bold))
                    .foregroundStyle(GenieSheet.night)
                    .frame(width: 20, height: 20)
                    .background(Circle().fill(GenieInk.glow))
                Text(hint.spot?.title ?? hint.text)
                    .font(.caption.weight(.semibold))
                    .foregroundStyle(Color.white)
                    .lineLimit(1)
            }
            .padding(.leading, 4)
            .padding(.trailing, FmSpace.x2)
            .padding(.vertical, 4)
            .background(Capsule().fill(Color.white.opacity(0.12)))
            .overlay(Capsule().strokeBorder(GenieInk.glow.opacity(0.6), lineWidth: 1))
        }
        .buttonStyle(.plain)
        .accessibilityLabel(Text(hint.text))
        .accessibilityIdentifier("genie-hint")
    }
}

struct GenieLineView: View {
    let line: String
    let onClose: () -> Void

    var body: some View {
        Text(line)
            .font(.subheadline.weight(.medium))
            .foregroundStyle(Color.white.opacity(0.95))
            .multilineTextAlignment(.center)
            .padding(.horizontal, FmSpace.x3)
            .padding(.vertical, FmSpace.x2)
            .background(
                RoundedRectangle(cornerRadius: 14, style: .continuous)
                    .fill(LinearGradient(colors: [GenieSheet.night, GenieSheet.dusk], startPoint: UnitPoint.top, endPoint: UnitPoint.bottom)))
            .overlay(
                RoundedRectangle(cornerRadius: 14, style: .continuous)
                    .strokeBorder(GenieInk.glow.opacity(0.45), lineWidth: 1))
            .shadow(color: Color.black.opacity(0.25), radius: 8, y: 3)
            .environment(\.colorScheme, .dark)
            .onTapGesture(perform: onClose)
            .accessibilityAddTraits(.isStaticText)
            .accessibilityIdentifier("genie-line")
    }
}

struct GenieSpiderMenu: View {
    let hub: GenieHub
    let crawler: GenieCrawler
    let coach: GenieHandCoach
    let onTuck: () -> Void
    @State private var outreach: GenieOutreachCoach = GenieOutreachCoach.shared

    var body: some View {
        VStack(alignment: .leading, spacing: FmSpace.x2) {
            HStack {
                Text("Genie")
                    .font(.subheadline.weight(.bold))
                    .foregroundStyle(Color.white)
                Spacer(minLength: 0)
                Button { hub.closeMenu() } label: {
                    Image(systemName: "xmark")
                        .font(.system(size: 11, weight: .bold))
                        .foregroundStyle(Color.white.opacity(0.85))
                        .frame(width: 26, height: 26)
                        .background(Circle().fill(Color.white.opacity(0.12)))
                }
                .buttonStyle(.plain)
                .accessibilityLabel(Text("Close"))
                .accessibilityIdentifier("genie-menu-close")
            }
            GenieHealthCard(vitals: hub.vitals)
            let ideas: [GenieHint] = hub.hints().filter { $0.spot != nil }
            if !ideas.isEmpty {
                Text("Next up")
                    .font(.caption2.weight(.semibold))
                    .foregroundStyle(Color.white.opacity(0.6))
                ForEach(ideas) { (hint: GenieHint) in
                    Button { hub.runHint(hint) } label: {
                        HStack(alignment: .top, spacing: FmSpace.x2) {
                            Image(systemName: hint.spot?.symbol ?? "lightbulb.fill")
                                .font(.caption.weight(.bold))
                                .foregroundStyle(GenieInk.glow)
                                .frame(width: 18)
                            Text(hint.text)
                                .font(.caption)
                                .foregroundStyle(Color.white)
                                .multilineTextAlignment(.leading)
                                .lineLimit(3)
                            Spacer(minLength: 0)
                        }
                        .contentShape(Rectangle())
                    }
                    .buttonStyle(.plain)
                    .accessibilityIdentifier("genie-menu-hint")
                }
            }
            Divider().overlay(Color.white.opacity(0.15))
            row("bubble.left.fill", "Talk", id: "genie-menu-talk") {
                hub.closeMenu()
                hub.openBubble()
            }
            row("arrow.up.left.and.arrow.down.right", "Full screen", id: "genie-expand") { hub.expand() }
            row(hub.alwaysListening ? "ear.fill" : "ear", hub.alwaysListening ? "Stop always listening" : "Always listen", id: "genie-menu-always") {
                hub.toggleAlwaysListening()
            }
            if coach.available {
                row(coach.on ? "hand.raised.slash" : "hand.raised.fill", coach.on ? "Stop hand play" : "Play with hands", id: "genie-menu-hands") {
                    hub.closeMenu()
                    coach.toggle()
                }
            }
            row("wand.and.stars", "Surprise me", id: "genie-menu-surprise") {
                hub.closeMenu()
                coach.surprise()
            }
            if !outreach.off {
                row("person.crop.circle.badge.plus", "Someone to reach out to", id: "genie-menu-outreach") {
                    hub.closeMenu()
                    outreach.suggestNow()
                }
            }
            row(outreach.off ? "person.2.fill" : "person.2.slash", outreach.off ? "Suggest people to contact" : "Stop suggesting people", id: "genie-menu-outreach-toggle") {
                outreach.setOff(!outreach.off)
            }
            if let problem = coach.problem {
                Text(problem)
                    .font(.caption2)
                    .foregroundStyle(Color.white.opacity(0.7))
                    .fixedSize(horizontal: false, vertical: true)
                    .fmMessageTools(problem, color: Color.white.opacity(0.7))
            }
            row(crawler.wanders ? "pause.circle" : "figure.walk", crawler.wanders ? "Stay put" : "Crawl around", id: "genie-menu-wander") {
                crawler.setWanders(!crawler.wanders)
            }
            row(crawler.parked == nil ? "arrow.right.to.line" : "arrow.left.to.line", crawler.parked == nil ? "Tuck into the edge" : "Come out", id: "genie-menu-tuck") {
                hub.closeMenu()
                onTuck()
            }
            row("arrow.counterclockwise", "Reset size and place", id: "genie-menu-reset") {
                hub.closeMenu()
                hub.resetPlace()
            }
            row("eye.slash", "Hide the spider", id: "genie-menu-hide") {
                hub.setSpiderHidden(true, fromMenu: true)
            }
        }
        .padding(FmSpace.x3)
        .background(
            RoundedRectangle(cornerRadius: 18, style: .continuous)
                .fill(LinearGradient(colors: [GenieSheet.night, GenieSheet.dusk], startPoint: UnitPoint.top, endPoint: UnitPoint.bottom)))
        .overlay(
            RoundedRectangle(cornerRadius: 18, style: .continuous)
                .strokeBorder(GenieInk.glow.opacity(0.45), lineWidth: 1))
        .shadow(color: GenieInk.violet.opacity(0.45), radius: 14, y: 5)
        .environment(\.colorScheme, .dark)
        .accessibilityElement(children: .contain)
        .accessibilityIdentifier("genie-menu")
    }

    private func row(_ symbol: String, _ title: String, id: String, action: @escaping () -> Void) -> some View {
        Button(action: action) {
            HStack(spacing: FmSpace.x2) {
                Image(systemName: symbol)
                    .font(.footnote.weight(.semibold))
                    .foregroundStyle(GenieInk.glow)
                    .frame(width: 22)
                Text(title)
                    .font(.footnote.weight(.medium))
                    .foregroundStyle(Color.white)
                Spacer(minLength: 0)
            }
            .frame(minHeight: 32)
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .accessibilityIdentifier(id)
    }
}

struct GenieHealthCard: View {
    let vitals: GenieVitals?

    static let marketingInk: Color = Color(red: 0.98, green: 0.48, blue: 0.78)

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            if let vitals = vitals {
                HStack(spacing: 3) {
                    ForEach(0..<GenieVitals.maxHearts, id: \.self) { (index: Int) in
                        Image(systemName: index < vitals.hearts ? "heart.fill" : "heart")
                            .font(.caption2.weight(.bold))
                            .foregroundStyle(index < vitals.hearts ? GenieHealthCard.marketingInk : Color.white.opacity(0.35))
                    }
                    Text(vitals.level.title)
                        .font(.caption.weight(.semibold))
                        .foregroundStyle(Color.white)
                        .padding(.leading, 4)
                }
                meter("Code", vitals.code, GenieInk.glow)
                meter("Marketing", vitals.marketing, GenieHealthCard.marketingInk)
                Text(vitals.summary)
                    .font(.caption2)
                    .foregroundStyle(Color.white.opacity(0.75))
                    .lineLimit(4)
                    .fixedSize(horizontal: false, vertical: true)
            } else {
                HStack(spacing: FmSpace.x2) {
                    ProgressView().controlSize(.small)
                    Text("Checking how I'm feeling…")
                        .font(.caption)
                        .foregroundStyle(Color.white.opacity(0.75))
                }
            }
        }
        .padding(FmSpace.x2)
        .background(RoundedRectangle(cornerRadius: 12, style: .continuous).fill(Color.white.opacity(0.07)))
        .accessibilityElement(children: .combine)
        .accessibilityIdentifier("genie-health")
    }

    private func meter(_ title: String, _ value: Double, _ ink: Color) -> some View {
        HStack(spacing: FmSpace.x2) {
            Text(title)
                .font(.caption2.weight(.semibold))
                .foregroundStyle(Color.white.opacity(0.8))
                .frame(width: 66, alignment: .leading)
            Capsule()
                .fill(Color.white.opacity(0.12))
                .frame(height: 6)
                .overlay(alignment: .leading) {
                    GeometryReader { (proxy: GeometryProxy) in
                        Capsule()
                            .fill(ink)
                            .frame(width: max(proxy.size.width * CGFloat(min(max(value, 0), 1)), 6))
                    }
                }
            Text("\(Int((value * 100).rounded()))%")
                .font(.caption2.monospacedDigit())
                .foregroundStyle(Color.white.opacity(0.7))
                .frame(width: 34, alignment: .trailing)
        }
        .accessibilityElement(children: .ignore)
        .accessibilityLabel(Text("\(title) \(Int((value * 100).rounded())) percent"))
    }
}

struct GenieTwist: ViewModifier {
    let manipulation: GenieManipulation?
    let base: CGFloat

    func body(content: Content) -> some View {
        let turn: GenieManipulation = manipulation ?? .identity
        let grown: CGFloat = GenieHub.fitted(base * CGFloat(turn.scale)) / max(base, 0.01)
        content
            .rotation3DEffect(.degrees(turn.yaw), axis: (x: 0, y: 1, z: 0), perspective: 0.55)
            .rotation3DEffect(.degrees(turn.pitch), axis: (x: 1, y: 0, z: 0), perspective: 0.55)
            .rotationEffect(.degrees(turn.rotation))
            .scaleEffect(grown)
    }
}

struct GenieTrickLine: View {
    let show: GenieShow

    var body: some View {
        HStack(spacing: 5) {
            Image(systemName: show.byModel ? "sparkles" : show.trick.symbol)
                .font(.caption2.weight(.bold))
                .foregroundStyle(GenieInk.glow)
            Text(show.line)
                .font(.caption.weight(.semibold))
                .foregroundStyle(Color.white)
                .lineLimit(2)
                .multilineTextAlignment(.center)
        }
        .padding(.horizontal, FmSpace.x2)
        .padding(.vertical, 5)
        .background(Capsule().fill(LinearGradient(colors: [GenieSheet.night, GenieSheet.dusk], startPoint: UnitPoint.top, endPoint: UnitPoint.bottom)))
        .overlay(Capsule().strokeBorder(GenieInk.glow.opacity(0.5), lineWidth: 1))
        .frame(maxWidth: 220)
        .environment(\.colorScheme, .dark)
        .accessibilityElement(children: .combine)
        .accessibilityLabel(Text("\(show.trick.title): \(show.line)"))
        .accessibilityIdentifier("genie-trick-line")
    }
}

struct GenieHandOverlay: View {
    let coach: GenieHandCoach
    let crawler: GenieCrawler
    let shown: CGFloat

    var body: some View {
        ZStack(alignment: .topLeading) {
            if let show = coach.showing, show.trick.effect == .web {
                TimelineView(.animation(minimumInterval: 1.0 / 24.0)) { (timeline: TimelineViewDefaultContext) in
                    let place: CGPoint = crawler.point(at: timeline.date)
                    Path { (path: inout Path) in
                        path.move(to: CGPoint(x: place.x, y: 0))
                        path.addLine(to: CGPoint(x: place.x, y: place.y - shown * 0.3))
                    }
                    .stroke(GenieInk.silver.opacity(0.7), style: StrokeStyle(lineWidth: 1.2, lineCap: .round))
                }
            }
            if let second = coach.state.second {
                ring(at: coach.screenPoint(second), pinching: coach.manipulation != nil, symbol: nil)
            }
            if let cursor = coach.cursorPoint {
                ring(at: cursor, pinching: coach.state.pinching || coach.manipulation != nil, symbol: GenieHandOverlay.symbol(coach.state.shape))
            }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
        .allowsHitTesting(false)
        .accessibilityHidden(true)
        .animation(.easeOut(duration: 0.08), value: coach.state.cursor)
    }

    private func ring(at point: CGPoint, pinching: Bool, symbol: String?) -> some View {
        let size: CGFloat = 22 + 26 * CGFloat(1 - coach.state.nearness)
        return ZStack {
            Circle()
                .fill(GenieInk.glow.opacity(pinching ? 0.55 : 0.12))
            Circle()
                .strokeBorder(GenieInk.glow.opacity(0.9), lineWidth: pinching ? 3 : 1.5)
            if let symbol = symbol, !pinching {
                Image(systemName: symbol)
                    .font(.system(size: size * 0.42, weight: .bold))
                    .foregroundStyle(Color.white.opacity(0.9))
            }
        }
        .frame(width: pinching ? size * 0.7 : size, height: pinching ? size * 0.7 : size)
        .shadow(color: GenieInk.glow.opacity(0.6), radius: 6)
        .position(point)
    }

    static func symbol(_ shape: GenieHandShape?) -> String? {
        switch shape {
        case .open: return "hand.raised.fill"
        case .fist: return "hand.raised.fingers.spread"
        case .point: return "hand.point.up.left.fill"
        case .peace: return "hand.peace"
        case .thumbsUp: return "hand.thumbsup.fill"
        case .thumbsDown: return "hand.thumbsdown.fill"
        case .ok: return "checkmark"
        case .rock: return "bolt.fill"
        case .callMe: return "phone.fill"
        case .gun: return "scope"
        default: return nil
        }
    }
}
