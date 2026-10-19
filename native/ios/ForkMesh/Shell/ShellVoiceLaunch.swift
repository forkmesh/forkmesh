import AVFoundation
import FMAgentWatch
import FMBase
import Observation
import SwiftUI
import UIKit

@MainActor
final class VoiceLaunchSounds {
    static let shared: VoiceLaunchSounds = VoiceLaunchSounds()

    private var players: [VoiceLaunchChime: AVAudioPlayer] = [:]
    private var loading: Bool = false

    @discardableResult
    func play(_ chime: VoiceLaunchChime) -> Double {
        guard FmSoundCues.isEnabled, !FmCuePlayer.shared.isQuiet, !MessagesVoiceRecorder.isRecording,
              !PromptDictation.isListening, !OtherAppCall.inProgress
        else { return 0 }
        guard let player = players[chime] else {
            load()
            return 0
        }
        FmCuePlayer.shared.activateSession()
        player.currentTime = 0
        player.play()
        return player.duration
    }

    func load() {
        guard players.isEmpty, !loading else { return }
        loading = true
        Task.detached(priority: .utility) {
            var clips: [VoiceLaunchChime: Data] = [:]
            for chime in VoiceLaunchChime.allCases {
                clips[chime] = wavData(chime.render())
            }
            let rendered: [VoiceLaunchChime: Data] = clips
            await MainActor.run { VoiceLaunchSounds.shared.install(rendered) }
        }
    }

    private func install(_ clips: [VoiceLaunchChime: Data]) {
        loading = false
        for (chime, clip) in clips {
            guard let player = try? AVAudioPlayer(data: clip) else { continue }
            player.volume = 0.85
            player.prepareToPlay()
            players[chime] = player
        }
    }
}

@MainActor
@Observable
final class ShellVoiceLaunch {
    enum Phase: Equatable {
        case closed
        case waking
        case listening
        case sending
        case sent
        case leaving
        case missed(String)
    }

    static let shared: ShellVoiceLaunch = ShellVoiceLaunch()
    static let pause: UInt64 = 3
    static let shortestLead: Double = VoiceLaunchChime.summonSeconds
    static let longestLead: Double = 0.8
    static let sentHold: Double = 0.8
    static let leaveHold: Double = 0.42
    static let missHold: Double = 2.2

    private(set) var phase: Phase = .closed
    private(set) var words: String = ""
    private(set) var origin: CGRect = .zero
    private(set) var openedAt: Date = .distantPast
    private(set) var heardAt: Date?
    private(set) var sentAt: Date = .distantPast
    private(set) var held: Bool = false
    @ObservationIgnored var orb: CGRect = .zero
    @ObservationIgnored private var pressing: Bool = false
    @ObservationIgnored private var dictation: PromptDictation?
    @ObservationIgnored private var attempt: Int = 0
    @ObservationIgnored private var onSend: ((String) -> Void)?

    var isOpen: Bool { phase != .closed }

    var heard: String { words.trimmingCharacters(in: .whitespacesAndNewlines) }

    var missed: String? {
        if case .missed(let message) = phase { return message }
        return nil
    }

    func prime() {
        let recognizer: PromptDictation = dictation ?? PromptDictation()
        recognizer.quick = true
        dictation = recognizer
        recognizer.prime()
    }

    func press() {
        pressing = true
    }

    func letGo() {
        pressing = false
        guard held else { return }
        held = false
        guard phase == .listening else { return }
        if heard.isEmpty && !DictationMeter.shared.voicing {
            dictation?.keepListening()
            return
        }
        dictation?.finishSpeaking()
        phase = .sending
        FmHaptics.impact(.rigid)
    }

    func open(from frame: CGRect, onSend: @escaping (String) -> Void) {
        let pressed: Bool = pressing
        pressing = false
        guard phase == .closed else { return }
        attempt += 1
        let mine: Int = attempt
        origin = frame
        words = ""
        heardAt = nil
        openedAt = Date()
        self.onSend = onSend
        held = pressed
        phase = .waking
        FmHaptics.impact(.medium)
        prime()
        let chime: Double = VoiceLaunchSounds.shared.play(.summon)
        let lead: Double = pressed ? ShellVoiceLaunch.shortestLead : min(max(chime, ShellVoiceLaunch.shortestLead), ShellVoiceLaunch.longestLead)
        Task { @MainActor [weak self] in
            try? await Task.sleep(for: .seconds(lead))
            guard let self = self, self.attempt == mine, self.phase == .waking else { return }
            self.listen(mine)
        }
    }

    func sendNow() {
        guard phase == .listening, !heard.isEmpty else { return }
        dictation?.stop()
    }

    func cancel() {
        guard isOpen, phase != .leaving, phase != .sent, phase != .sending else { return }
        let wasHearing: Bool = phase == .listening
        attempt += 1
        held = false
        if wasHearing {
            dictation?.stop()
        }
        phase = .leaving
        FmHaptics.impact(.light)
        closeLater(ShellVoiceLaunch.leaveHold)
    }

    func sent() {
        attempt += 1
        sentAt = Date()
        phase = .sent
        FmHaptics.impact(.heavy)
        VoiceLaunchSounds.shared.play(.launch)
        closeLater(ShellVoiceLaunch.sentHold)
    }

    func dismiss() {
        let wasHearing: Bool = dictation?.listening == true
        attempt += 1
        held = false
        if wasHearing {
            dictation?.stop()
        }
        phase = .closed
    }

    private func listen(_ mine: Int) {
        let recognizer: PromptDictation = dictation ?? PromptDictation()
        recognizer.pause = ShellVoiceLaunch.pause
        recognizer.quick = true
        recognizer.holds = held
        dictation = recognizer
        phase = .listening
        FmHaptics.impact(.soft)
        UIAccessibility.post(notification: .announcement, argument: "Listening")
        Task { @MainActor in
            _ = await recognizer.start(
                onWords: { [weak self] (spoken: String) in
                    guard let self = self, self.attempt == mine, self.phase == .listening || self.phase == .sending else { return }
                    if spoken != self.words {
                        self.words = spoken
                        self.heardAt = Date()
                    }
                },
                onDone: { [weak self] (_: Bool) in self?.ended(mine) },
                onError: { [weak self] (message: String) in self?.miss(message, mine, closes: false) })
        }
    }

    private func ended(_ mine: Int) {
        guard attempt == mine, phase == .listening || phase == .sending else { return }
        held = false
        let text: String = heard
        guard !text.isEmpty else {
            miss("I didn't hear anything. Hold the button and try again.", mine, closes: true)
            return
        }
        if phase == .listening {
            phase = .sending
            FmHaptics.impact(.rigid)
        }
        onSend?(text)
    }

    private func miss(_ message: String, _ mine: Int, closes: Bool) {
        guard attempt == mine else { return }
        attempt += 1
        phase = .missed(message)
        FmHaptics.impact(.light)
        Task { @MainActor in
            VoiceLaunchSounds.shared.play(.fizzle)
        }
        if closes {
            closeLater(ShellVoiceLaunch.missHold)
        }
    }

    private func closeLater(_ seconds: Double) {
        let mine: Int = attempt
        Task { @MainActor [weak self] in
            try? await Task.sleep(for: .seconds(seconds))
            guard let self = self, self.attempt == mine, self.phase != .closed else { return }
            if self.phase == .leaving || self.phase == .sent {
                self.phase = .closed
                return
            }
            self.phase = .leaving
            self.closeLater(ShellVoiceLaunch.leaveHold)
        }
    }
}

struct ShellVoiceLaunchOverlay: View {
    @Environment(\.fmPalette) var fm
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @Environment(\.accessibilityReduceTransparency) private var reduceTransparency
    let voice: ShellVoiceLaunch
    let onType: () -> Void
    let onCommands: () -> Void

    var body: some View {
        if voice.isOpen {
            ShellVoiceStage(
                voice: voice, accent: fm.accent, reduceMotion: reduceMotion, reduceTransparency: reduceTransparency,
                onType: onType, onCommands: onCommands)
                .id(voice.openedAt)
        }
    }
}

private struct ShellVoiceStage: View {
    let voice: ShellVoiceLaunch
    let accent: Color
    let reduceMotion: Bool
    let reduceTransparency: Bool
    let onType: () -> Void
    let onCommands: () -> Void

    @State private var shown: Bool = false
    @State private var meter: DictationMeter = DictationMeter.shared

    private var phase: ShellVoiceLaunch.Phase { voice.phase }
    private var away: Bool { phase == .leaving || phase == .sent }
    private var hearing: Bool { phase == .listening }

    var body: some View {
        GeometryReader { (proxy: GeometryProxy) in
            let global: CGPoint = proxy.frame(in: .global).origin
            let size: CGSize = proxy.size
            let diameter: CGFloat = min(196, size.width * 0.5)
            let center: CGPoint = CGPoint(x: size.width / 2, y: size.height * 0.36)
            let start: CGPoint = CGPoint(x: voice.origin.midX - global.x, y: voice.origin.midY - global.y)
            let seed: CGFloat = voice.origin.width > 0 ? voice.origin.width / diameter : 0.22
            let home: Bool = !shown || phase == .leaving
            ZStack {
                backdrop(center: center, diameter: diameter)
                    .opacity(shown && !away ? 1 : 0)
                    .animation(.easeOut(duration: phase == .sent ? 0.5 : 0.3), value: away)
                    .onTapGesture { voice.cancel() }
                if phase == .sent && !reduceMotion {
                    ShellVoiceBurst(sentAt: voice.sentAt, center: center, diameter: diameter, accent: accent)
                }
                orb(diameter: diameter)
                    .onGeometryChange(for: CGRect.self, of: { $0.frame(in: .global) }) { (frame: CGRect) in
                        voice.orb = frame
                    }
                    .scaleEffect(home && !reduceMotion ? seed : (phase == .sent ? 0.3 : 1))
                    .opacity(away && (reduceMotion || phase == .sent) ? 0 : (home && reduceMotion ? 0 : 1))
                    .position(home && !reduceMotion ? start : center)
                VStack(spacing: FmSpace.x3) {
                    transcript
                    hint
                }
                .frame(maxWidth: min(size.width - FmSpace.x6 * 2, 520))
                .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .top)
                .padding(.top, center.y + diameter * 0.8)
                .padding(.bottom, 150)
                .opacity(shown && !away ? 1 : 0)
                .offset(y: shown && !away ? 0 : 18)
                .animation(.easeOut(duration: 0.28).delay(shown && !away ? 0.12 : 0), value: shown && !away)
                controls
                    .position(x: size.width / 2, y: size.height - 60)
                    .opacity(shown && !away ? 1 : 0)
                    .offset(y: shown && !away ? 0 : 40)
                    .animation(.spring(response: 0.45, dampingFraction: 0.8).delay(shown && !away ? 0.18 : 0), value: shown && !away)
            }
            .animation(motion, value: home)
            .animation(.spring(response: 0.35, dampingFraction: 0.6), value: phase == .sent)
            .allowsHitTesting(!away)
        }
        .environment(\.colorScheme, .dark)
        .accessibilityAddTraits(.isModal)
        .accessibilityAction(.escape) { voice.cancel() }
        .onAppear { shown = true }
    }

    private var motion: Animation {
        reduceMotion ? .easeOut(duration: 0.2) : .spring(response: 0.55, dampingFraction: 0.68)
    }

    @ViewBuilder private func backdrop(center: CGPoint, diameter: CGFloat) -> some View {
        ZStack {
            if reduceTransparency {
                Color.black.opacity(0.92)
            } else {
                Rectangle().fill(.ultraThinMaterial)
                Color.black.opacity(0.5)
            }
            TimelineView(.animation(minimumInterval: 1.0 / 30.0, paused: reduceMotion)) { (context: TimelineViewDefaultContext) in
                let level: CGFloat = CGFloat(ShellVoiceStage.loudness(meter.levels))
                let time: Double = context.date.timeIntervalSinceReferenceDate
                let breathe: CGFloat = reduceMotion ? 0 : CGFloat(sin(time * 1.6)) * 0.04
                RadialGradient(
                    colors: [accent.opacity(0.55 + Double(level) * 0.35), accent.opacity(0.12), Color.clear],
                    center: UnitPoint(x: 0.5, y: 0.36),
                    startRadius: diameter * 0.2,
                    endRadius: diameter * (2.1 + breathe + level * 0.9))
            }
        }
        .ignoresSafeArea()
        .contentShape(Rectangle())
        .accessibilityHidden(true)
    }

    private func orb(diameter: CGFloat) -> some View {
        ZStack {
            ShellVoiceCorona(
                meter: meter, accent: accent, diameter: diameter, hearing: hearing, quietSince: voice.held ? nil : voice.heardAt,
                waiting: phase == .sending, still: reduceMotion)
                .frame(width: diameter * 2, height: diameter * 2)
                .opacity(phase == .waking ? 0.35 : 1)
                .animation(.easeOut(duration: 0.4), value: phase == .waking)
            ShellVoiceCore(meter: meter, accent: accent, diameter: diameter, still: reduceMotion, plain: reduceTransparency)
            Image(systemName: symbol)
                .font(.system(size: diameter * 0.26, weight: .semibold))
                .foregroundStyle(Color.white)
                .shadow(color: Color.black.opacity(0.35), radius: 4, y: 2)
                .contentTransition(.symbolEffect(.replace))
                .symbolEffect(.variableColor.iterative.reversing, isActive: hearing && !reduceMotion)
                .symbolEffect(.pulse, isActive: phase == .sending && !reduceMotion)
        }
        .frame(width: diameter, height: diameter)
        .contentShape(Circle())
        .onTapGesture { voice.sendNow() }
        .accessibilityElement()
        .accessibilityLabel(Text(orbLabel))
        .accessibilityAddTraits(.isButton)
        .accessibilityAction { voice.sendNow() }
        .accessibilityIdentifier("shell-voice-orb")
    }

    private var symbol: String {
        switch phase {
        case .waking: return "sparkles"
        case .listening: return "waveform"
        case .sending: return "arrow.up"
        case .sent: return "checkmark"
        case .missed: return "mic.slash"
        case .leaving, .closed: return "xmark"
        }
    }

    private var orbLabel: String {
        switch phase {
        case .listening: return voice.heard.isEmpty ? "Listening" : "Send now"
        case .sending: return "Starting the agent"
        case .sent: return "Agent started"
        default: return "Voice prompt"
        }
    }

    @ViewBuilder private var transcript: some View {
        let missed: String? = voice.missed
        if let missed = missed {
            Text(missed)
                .font(.system(.title3, design: .rounded, weight: .semibold))
                .foregroundStyle(Color.white.opacity(0.9))
                .multilineTextAlignment(.center)
                .transition(.opacity)
        } else if voice.words.isEmpty {
            Text(phase == .waking ? "Get ready…" : "Listening…")
                .font(.system(.title2, design: .rounded, weight: .semibold))
                .foregroundStyle(Color.white.opacity(0.55))
                .contentTransition(.opacity)
                .modifier(ShellVoiceShimmer(active: hearing && !reduceMotion))
        } else {
            Text(voice.words)
                .font(.system(.title2, design: .rounded, weight: .semibold))
                .foregroundStyle(
                    LinearGradient(
                        colors: [Color.white, Color.white.opacity(0.78)], startPoint: .top, endPoint: .bottom))
                .multilineTextAlignment(.center)
                .lineLimit(7)
                .minimumScaleFactor(0.6)
                .contentTransition(.interpolate)
                .animation(.easeOut(duration: 0.18), value: voice.words)
                .accessibilityAddTraits(.updatesFrequently)
        }
    }

    @ViewBuilder private var hint: some View {
        let line: String? = {
            switch phase {
            case .listening where voice.held: return voice.heard.isEmpty ? "Say what the agent should do · let go to send" : "Let go to send"
            case .listening: return voice.heard.isEmpty ? "Say what the agent should do" : "Pause to send · tap the orb to send now"
            case .sending: return "Starting your agent…"
            case .sent: return "On its way"
            default: return nil
            }
        }()
        if let line = line {
            Text(line)
                .font(.footnote.weight(.medium))
                .foregroundStyle(Color.white.opacity(0.6))
                .contentTransition(.opacity)
                .animation(.easeOut(duration: 0.2), value: line)
        }
    }

    private var controls: some View {
        HStack(spacing: FmSpace.x6) {
            ShellVoiceControl(symbol: "command", label: "Commands", action: onCommands)
            ShellVoiceControl(symbol: "xmark", label: "Cancel", prominent: true, action: { voice.cancel() })
            ShellVoiceControl(symbol: "keyboard", label: "Type", action: onType)
        }
        .disabled(phase == .sending)
    }

    nonisolated static func loudness(_ levels: [Float]) -> Double {
        Double(levels.suffix(3).max() ?? 0)
    }
}

private struct ShellVoiceControl: View {
    let symbol: String
    let label: String
    var prominent: Bool = false
    let action: () -> Void

    var body: some View {
        Button(action: action) {
            VStack(spacing: 6) {
                Image(systemName: symbol)
                    .font(.system(size: prominent ? 22 : 18, weight: .semibold))
                    .foregroundStyle(Color.white)
                    .frame(width: prominent ? 64 : 52, height: prominent ? 64 : 52)
                    .background(Circle().fill(Color.white.opacity(prominent ? 0.2 : 0.12)))
                    .overlay(Circle().strokeBorder(Color.white.opacity(0.18), lineWidth: 1))
                Text(label)
                    .font(.caption.weight(.medium))
                    .foregroundStyle(Color.white.opacity(0.75))
            }
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .accessibilityLabel(Text(label))
        .accessibilityIdentifier("shell-voice-\(label.lowercased())")
    }
}

private struct ShellVoiceCore: View {
    let meter: DictationMeter
    let accent: Color
    let diameter: CGFloat
    let still: Bool
    let plain: Bool

    var body: some View {
        TimelineView(.animation(minimumInterval: 1.0 / 30.0, paused: still)) { (context: TimelineViewDefaultContext) in
            let level: Double = ShellVoiceStage.loudness(meter.levels)
            let time: Double = context.date.timeIntervalSinceReferenceDate
            let swell: CGFloat = still ? 1 : 1 + CGFloat(level) * 0.12 + CGFloat(sin(time * 2.1)) * 0.015
            ZStack {
                Circle().fill(
                    LinearGradient(
                        colors: [accent.opacity(0.95), accent, accent.opacity(0.7)],
                        startPoint: .top, endPoint: .bottom))
                ShellSparkleField(
                    tilt: still ? .zero : CGSize(width: sin(time * 0.7) * 0.4, height: cos(time * 0.5) * 0.3),
                    energy: still ? 0 : min(1, level * 1.4), paused: still, stars: ShellSparkleField.most, plain: plain)
            }
            .clipShape(Circle())
            .shadow(color: accent.opacity(0.6 + level * 0.4), radius: 24 + CGFloat(level) * 30)
            .scaleEffect(swell)
        }
    }
}

private struct ShellVoiceCorona: View {
    let meter: DictationMeter
    let accent: Color
    let diameter: CGFloat
    let hearing: Bool
    let quietSince: Date?
    let waiting: Bool
    let still: Bool

    static let spokes: Int = 72

    var body: some View {
        TimelineView(.animation(minimumInterval: 1.0 / 60.0, paused: false)) { (context: TimelineViewDefaultContext) in
            Canvas { (canvas: inout GraphicsContext, size: CGSize) in
                draw(&canvas, size: size, now: context.date)
            }
        }
        .allowsHitTesting(false)
        .accessibilityHidden(true)
    }

    private var colors: [Color] {
        [accent, Color(red: 0.45, green: 0.95, blue: 1), Color(red: 1, green: 0.55, blue: 0.88), Color(red: 1, green: 0.84, blue: 0.3), accent]
    }

    private func draw(_ canvas: inout GraphicsContext, size: CGSize, now: Date) {
        let center: CGPoint = CGPoint(x: size.width / 2, y: size.height / 2)
        let radius: CGFloat = diameter / 2
        let time: Double = still ? 0 : now.timeIntervalSinceReferenceDate
        let levels: [Float] = meter.levels
        let level: Double = ShellVoiceStage.loudness(levels)
        let spin: Angle = .radians(time * 0.35)
        let wash: GraphicsContext.Shading = .conicGradient(Gradient(colors: colors), center: center, angle: spin)
        if !still {
            for ring in 0..<3 {
                let cycle: Double = 2.4 - level * 1.1
                let age: Double = (time / cycle + Double(ring) / 3).truncatingRemainder(dividingBy: 1)
                let reach: CGFloat = radius * (1.02 + CGFloat(age) * (0.75 + CGFloat(level) * 0.5))
                let path: Path = Path(ellipseIn: CGRect(x: center.x - reach, y: center.y - reach, width: reach * 2, height: reach * 2))
                var faded: GraphicsContext = canvas
                faded.opacity = (1 - age) * (hearing ? 0.55 : 0.25)
                faded.stroke(path, with: wash, lineWidth: 1.5 + CGFloat(1 - age) * 2)
            }
        }
        var glow: GraphicsContext = canvas
        glow.blendMode = .plusLighter
        for spoke in 0..<ShellVoiceCorona.spokes {
            let turn: Double = Double(spoke) / Double(ShellVoiceCorona.spokes)
            let mirrored: Double = turn < 0.5 ? turn * 2 : (1 - turn) * 2
            let index: Int = min(levels.count - 1, max(0, Int(mirrored * Double(levels.count - 1))))
            let heard: CGFloat = levels.isEmpty ? 0 : CGFloat(levels[levels.count - 1 - index])
            let ripple: CGFloat = still ? 0 : CGFloat(sin(time * 3 + turn * .pi * 8)) * 0.5 + 0.5
            let length: CGFloat = radius * (0.05 + (hearing ? heard * 0.42 : 0) + ripple * 0.04)
            let angle: Double = turn * 2 * .pi + spin.radians * 0.5 - .pi / 2
            let inner: CGFloat = radius * 1.08
            let from: CGPoint = CGPoint(x: center.x + CGFloat(cos(angle)) * inner, y: center.y + CGFloat(sin(angle)) * inner)
            let to: CGPoint = CGPoint(
                x: center.x + CGFloat(cos(angle)) * (inner + length), y: center.y + CGFloat(sin(angle)) * (inner + length))
            var line: Path = Path()
            line.move(to: from)
            line.addLine(to: to)
            glow.opacity = 0.35 + Double(heard) * 0.65
            glow.stroke(line, with: wash, style: StrokeStyle(lineWidth: 2.4, lineCap: .round))
        }
        if let since = quietSince, hearing {
            let quiet: Double = now.timeIntervalSince(since) / Double(ShellVoiceLaunch.pause)
            if quiet > 0.08 && level < Double(DictationMeter.voice) {
                drawArc(&canvas, center: center, radius: radius * 0.98, upTo: min(1, quiet), spin: .zero)
            }
        }
        if waiting {
            drawArc(&canvas, center: center, radius: radius * 0.98, upTo: 0.28, spin: .radians(time * 6))
        }
    }

    private func drawArc(_ canvas: inout GraphicsContext, center: CGPoint, radius: CGFloat, upTo: Double, spin: Angle) {
        var arc: Path = Path()
        let start: Double = spin.radians - Double.pi / 2
        arc.addArc(
            center: center, radius: radius, startAngle: .radians(start), endAngle: .radians(start + 2 * Double.pi * upTo),
            clockwise: false)
        canvas.stroke(arc, with: .color(Color.white.opacity(0.9)), style: StrokeStyle(lineWidth: 3.5, lineCap: .round))
    }
}

private struct ShellVoiceBurst: View {
    let sentAt: Date
    let center: CGPoint
    let diameter: CGFloat
    let accent: Color

    static let sparks: Int = 36
    static let life: Double = 0.8

    var body: some View {
        TimelineView(.animation) { (context: TimelineViewDefaultContext) in
            Canvas { (canvas: inout GraphicsContext, _: CGSize) in
                let age: Double = context.date.timeIntervalSince(sentAt) / ShellVoiceBurst.life
                guard age >= 0, age < 1 else { return }
                draw(&canvas, age: age)
            }
        }
        .allowsHitTesting(false)
        .accessibilityHidden(true)
    }

    private func draw(_ canvas: inout GraphicsContext, age: Double) {
        let ease: CGFloat = CGFloat(1 - pow(1 - age, 3))
        let radius: CGFloat = diameter / 2
        let wave: CGFloat = radius * (0.6 + ease * 2.2)
        var ring: GraphicsContext = canvas
        ring.opacity = 1 - age
        ring.stroke(
            Path(ellipseIn: CGRect(x: center.x - wave, y: center.y - wave, width: wave * 2, height: wave * 2)),
            with: .color(Color.white), lineWidth: 6 * CGFloat(1 - age) + 1)
        let flash: CGFloat = radius * (0.8 + ease * 0.6)
        ring.opacity = max(0, 0.7 - age * 1.6)
        ring.fill(
            Path(ellipseIn: CGRect(x: center.x - flash, y: center.y - flash, width: flash * 2, height: flash * 2)),
            with: .radialGradient(
                Gradient(colors: [Color.white, accent.opacity(0.6), accent.opacity(0)]),
                center: center, startRadius: 0, endRadius: flash))
        let palette: [Color] = [Color.white, accent, Color(red: 1, green: 0.84, blue: 0.3), Color(red: 1, green: 0.55, blue: 0.88)]
        for spark in 0..<ShellVoiceBurst.sparks {
            let n: Double = Double(spark)
            let angle: Double = n * ShellSparkleField.goldenAngle
            let speed: CGFloat = 0.9 + CGFloat((n * 0.618).truncatingRemainder(dividingBy: 1)) * 1.4
            let reach: CGFloat = radius * (0.4 + ease * 2.4 * speed)
            let point: CGPoint = CGPoint(x: center.x + CGFloat(cos(angle)) * reach, y: center.y + CGFloat(sin(angle)) * reach)
            let size: CGFloat = (2 + CGFloat(spark % 4)) * CGFloat(1 - age * 0.7)
            var dot: GraphicsContext = canvas
            dot.opacity = 1 - age
            dot.fill(ShellSparkleField.star(at: point, radius: size * 1.6), with: .color(palette[spark % palette.count]))
        }
    }
}

private struct ShellVoiceShimmer: ViewModifier {
    let active: Bool

    func body(content: Content) -> some View {
        if active {
            content.overlay {
                TimelineView(.animation) { (context: TimelineViewDefaultContext) in
                    let phase: Double = context.date.timeIntervalSinceReferenceDate.truncatingRemainder(dividingBy: 1.8) / 1.8
                    LinearGradient(
                        colors: [Color.clear, Color.white.opacity(0.9), Color.clear],
                        startPoint: UnitPoint(x: phase * 2 - 0.6, y: 0.5), endPoint: UnitPoint(x: phase * 2 - 0.1, y: 0.5))
                }
                .mask(content)
                .allowsHitTesting(false)
            }
        } else {
            content
        }
    }
}
