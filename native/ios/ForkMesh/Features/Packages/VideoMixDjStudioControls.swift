import FMPackages
import QuartzCore
import SwiftUI
import UIKit

enum DjTheme {
    static func rgb(_ hex: UInt32) -> Color {
        Color(red: Double((hex >> 16) & 0xFF) / 255, green: Double((hex >> 8) & 0xFF) / 255, blue: Double(hex & 0xFF) / 255)
    }

    static let backdrop = LinearGradient(colors: [rgb(0x3E4249), rgb(0x2B2E33), rgb(0x24272B)], startPoint: .top, endPoint: .bottom)
    static let bar = LinearGradient(colors: [rgb(0x26292D), rgb(0x1C1E21)], startPoint: .top, endPoint: .bottom)
    static let control = LinearGradient(colors: [rgb(0x3A3D43), rgb(0x27292D)], startPoint: .top, endPoint: .bottom)
    static let panel = rgb(0x1B1D20)
    static let edge = Color.white.opacity(0.08)
    static let ink = Color.white
    static let muted = rgb(0xA3A8B0)
    static let play = rgb(0x66D93D)
    static let record = rgb(0xD0161F)
    static let active = rgb(0x4DA8FF)
    static let playhead = rgb(0xE0242C)
    static let waveColors: [Color] = (0..<24).map { Color(hue: Double($0) / 24, saturation: 0.72, brightness: 1) }

    @MainActor private static let tapper = UIImpactFeedbackGenerator(style: .light)

    @MainActor static func tick() {
        guard !UIAccessibility.isReduceMotionEnabled, FmHaptics.enabled else { return }
        tapper.impactOccurred()
        tapper.prepare()
    }
}

struct DjRoundButton<Label: View>: View {
    let size: CGFloat
    var lit = false
    let action: () -> Void
    @ViewBuilder let label: () -> Label

    var body: some View {
        Button {
            DjTheme.tick()
            action()
        } label: {
            DjRoundFace(size: size, lit: lit, label: label)
        }
        .buttonStyle(DjPressStyle())
    }
}

struct DjRoundFace<Label: View>: View {
    let size: CGFloat
    var lit = false
    @ViewBuilder let label: () -> Label

    var body: some View {
        ZStack {
            Circle().fill(DjTheme.control)
            Circle().strokeBorder(Color.black.opacity(0.55), lineWidth: 1)
            Circle().inset(by: 1).strokeBorder(lit ? DjTheme.active.opacity(0.9) : Color.white.opacity(0.1), lineWidth: lit ? 1.5 : 0.75)
            label()
                .font(.system(size: size * 0.42, weight: .semibold))
                .foregroundStyle(lit ? DjTheme.active : DjTheme.muted)
        }
        .frame(width: size, height: size)
        .shadow(color: .black.opacity(0.45), radius: 2, y: 1)
        .contentShape(Circle())
    }
}

struct DjPressStyle: ButtonStyle {
    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .brightness(configuration.isPressed ? 0.12 : 0)
            .scaleEffect(configuration.isPressed ? 0.96 : 1)
    }
}

struct DjInfoButton: View {
    let model: DjStudioModel
    let size: CGFloat

    var body: some View {
        Button {
            model.sheet = .info
        } label: {
            ZStack {
                Circle().fill(DjTheme.muted)
                Image(systemName: "info")
                    .font(.system(size: size * 0.42, weight: .heavy))
                    .italic()
                    .foregroundStyle(DjTheme.rgb(0x23252A))
            }
            .frame(width: size * 0.62, height: size * 0.62)
            .frame(width: size, height: size)
            .contentShape(Rectangle())
        }
        .buttonStyle(DjPressStyle())
        .accessibilityLabel("Track info")
    }
}

struct DjResetButton: View {
    let model: DjStudioModel
    let size: CGFloat

    var body: some View {
        Button {
            model.confirmingReset = true
        } label: {
            Image(systemName: "arrow.counterclockwise")
                .font(.system(size: size * 0.5, weight: .bold))
                .foregroundStyle(DjTheme.muted)
                .frame(width: size, height: size)
                .contentShape(Rectangle())
        }
        .buttonStyle(DjPressStyle())
        .accessibilityLabel("Reset all")
    }
}

struct DjLibraryHandle: View {
    let model: DjStudioModel
    let side: DjDeckSide
    let u: CGFloat

    var body: some View {
        Button {
            DjTheme.tick()
            model.openLoad(side)
        } label: {
            VStack(spacing: 6 * u) {
                RoundedRectangle(cornerRadius: 2 * u, style: .continuous)
                    .fill(DjTheme.control)
                    .overlay(RoundedRectangle(cornerRadius: 2 * u, style: .continuous).strokeBorder(Color.black.opacity(0.6), lineWidth: 1))
                    .frame(width: 34 * u, height: 9 * u)
                Grid(horizontalSpacing: 4 * u, verticalSpacing: 4 * u) {
                    ForEach(0..<4, id: \.self) { _ in
                        GridRow {
                            Circle().fill(Color.black.opacity(0.55)).frame(width: 3.5 * u, height: 3.5 * u)
                            Circle().fill(Color.black.opacity(0.55)).frame(width: 3.5 * u, height: 3.5 * u)
                        }
                    }
                }
            }
            .frame(width: 40 * u)
            .frame(maxHeight: .infinity, alignment: .top)
            .padding(.top, 4 * u)
            .contentShape(Rectangle())
        }
        .buttonStyle(DjPressStyle())
        .accessibilityLabel("Load Deck \(side.label)")
    }
}

struct DjDeckInfo: View {
    let model: DjStudioModel
    let deck: DjDeck
    let mirrored: Bool
    let u: CGFloat

    var body: some View {
        HStack(alignment: .center, spacing: 12 * u) {
            if mirrored {
                titles
                clock
            } else {
                clock
                titles
            }
        }
        .lineLimit(1)
    }

    private var clock: some View {
        DjClock(deck: deck, u: u)
            .onTapGesture { deck.showsRemaining.toggle() }
    }

    private var titles: some View {
        VStack(alignment: mirrored ? .trailing : .leading, spacing: 0) {
            Text(deck.loaded ? (deck.title.isEmpty ? "Loading…" : deck.title) : "Deck \(deck.side.label)")
                .font(.system(size: 15 * u, weight: .regular))
                .foregroundStyle(DjTheme.ink.opacity(0.92))
            Text(deck.loaded ? (deck.author.isEmpty ? "YouTube" : deck.author) : "Tap to load a video")
                .font(.system(size: 13 * u, weight: .regular))
                .foregroundStyle(DjTheme.muted)
        }
        .frame(maxWidth: 220 * u, alignment: mirrored ? .trailing : .leading)
        .contentShape(Rectangle())
        .onTapGesture { model.openLoad(deck.side) }
    }
}

struct DjClock: View {
    let deck: DjDeck
    let u: CGFloat

    var body: some View {
        HStack(alignment: .firstTextBaseline, spacing: 4 * u) {
            TimelineView(.animation(minimumInterval: 0.25, paused: !deck.playing || deck.scratching)) { _ in
                let position = deck.position()
                Text(deck.showsRemaining ? "-" + DjStudio.clock(max(0, deck.duration - position)) : DjStudio.clock(position))
                    .font(.system(size: 30 * u, weight: .light))
                    .monospacedDigit()
                    .foregroundStyle(DjTheme.ink)
            }
            Text("/ " + (deck.duration > 0 ? DjStudio.clock(deck.duration) : "--:--"))
                .font(.system(size: 14 * u, weight: .regular))
                .monospacedDigit()
                .foregroundStyle(DjTheme.muted)
                .opacity(deck.loaded ? 1 : 0.5)
        }
        .fixedSize()
        .accessibilityElement(children: .ignore)
        .accessibilityLabel("Deck \(deck.side.label) time")
        .accessibilityValue(deck.duration > 0 ? "\(DjStudio.clock(deck.time)) of \(DjStudio.clock(deck.duration))" : DjStudio.clock(deck.time))
    }
}

struct DjWaveform: View {
    let model: DjStudioModel
    let deck: DjDeck
    @State private var dragX: CGFloat?

    var body: some View {
        GeometryReader { geometry in
            let size = geometry.size
            TimelineView(.animation(paused: !deck.moving)) { _ in
                let time = deck.shown() - Double((dragX ?? 0) / max(1, size.width)) * DjWaveDrawing.window
                let bars = deck.waveform
                let duration = deck.duration
                Canvas(rendersAsynchronously: false) { context, area in
                    DjWaveDrawing.zoomed(context, area, bars: bars, time: time, duration: duration)
                }
            }
            .contentShape(Rectangle())
            .gesture(
                DragGesture(minimumDistance: 0)
                    .onChanged { value in
                        guard deck.duration > 0 else { return }
                        dragX = value.translation.width
                    }
                    .onEnded { value in
                        defer { dragX = nil }
                        guard deck.duration > 0, size.width > 0 else {
                            if !deck.loaded { model.openLoad(deck.side) }
                            return
                        }
                        model.seek(deck.side, to: deck.position() - Double(value.translation.width / size.width) * DjWaveDrawing.window)
                    })
        }
        .overlay {
            if deck.loaded, deck.waveform.isEmpty, deck.reading {
                Text("Reading the audio…")
                    .font(.system(size: 10, weight: .medium))
                    .foregroundStyle(DjTheme.muted)
                    .padding(.horizontal, 6)
                    .background(DjTheme.panel.opacity(0.85), in: Capsule())
                    .allowsHitTesting(false)
            }
        }
        .clipped()
        .accessibilityElement()
        .accessibilityLabel("Deck \(deck.side.label) waveform")
        .accessibilityValue(DjStudio.clock(deck.time))
    }
}

enum DjWaveDrawing {
    static let window: Double = 16

    static func centreLine(_ context: GraphicsContext, _ size: CGSize) {
        context.fill(Path(CGRect(x: 0, y: size.height / 2 - 0.5, width: size.width, height: 1)), with: .color(.white.opacity(0.22)))
    }

    static func zoomed(_ context: GraphicsContext, _ size: CGSize, bars: [DjWaveBar], time: Double, duration: Double) {
        centreLine(context, size)
        defer { context.fill(Path(CGRect(x: size.width / 2 - 0.75, y: 0, width: 1.5, height: size.height)), with: .color(DjTheme.playhead)) }
        guard !bars.isEmpty, duration > 0, size.width > 0 else { return }
        var paths = [Path](repeating: Path(), count: DjTheme.waveColors.count)
        let step: CGFloat = 3
        let secondsPerPoint = window / Double(size.width)
        let first = time - window / 2
        let offset = CGFloat((first / secondsPerPoint).truncatingRemainder(dividingBy: Double(step)))
        var x: CGFloat = -offset
        while x < size.width {
            let moment = first + Double(x) * secondsPerPoint
            if moment >= 0, moment <= duration {
                let bar = bars[min(bars.count - 1, Int(moment / duration * Double(bars.count)))]
                add(&paths, bar, x: x, width: step * 0.7, size: size)
            }
            x += step
        }
        fill(context, paths)
    }

    private static func add(_ paths: inout [Path], _ bar: DjWaveBar, x: CGFloat, width: CGFloat, size: CGSize) {
        let height = max(1, CGFloat(bar.level) * size.height * 0.92)
        let bucket = min(paths.count - 1, max(0, Int(bar.hue * Double(paths.count))))
        paths[bucket].addRect(CGRect(x: x, y: (size.height - height) / 2, width: width, height: height))
    }

    private static func fill(_ context: GraphicsContext, _ paths: [Path]) {
        for (index, path) in paths.enumerated() where !path.isEmpty {
            context.fill(path, with: .color(DjTheme.waveColors[index]))
        }
    }
}

struct DjBackButton: View {
    let model: DjStudioModel
    let size: CGFloat

    var body: some View {
        DjRoundButton(size: size, action: { model.leave() }) {
            Image(systemName: "chevron.left")
        }
        .accessibilityLabel("Back")
        .accessibilityHint("Closes the studio. Playing decks keep playing.")
    }
}

struct DjTransport: View {
    let model: DjStudioModel
    let deck: DjDeck
    let u: CGFloat
    var vertical = false
    var cell = CGSize(width: 58, height: 32)

    var body: some View {
        let layout = vertical ? AnyLayout(VStackLayout(spacing: 0)) : AnyLayout(HStackLayout(spacing: 0))
        let atStart = !deck.playing && deck.position() < 0.05
        layout {
            button("START", symbol: "play.fill", tint: DjTheme.play, lit: deck.playing) { model.start(deck.side) }
                .accessibilityLabel("Start Deck \(deck.side.label)")
                .accessibilityValue(deck.playing ? "Playing" : "Stopped")
            divider
            button("PAUSE", symbol: "pause.fill", tint: DjTheme.ink, lit: deck.loaded && !deck.playing && !atStart) { model.pauseDeck(deck.side) }
                .accessibilityLabel("Pause Deck \(deck.side.label)")
            divider
            button("STOP", symbol: "stop.fill", tint: DjTheme.record, lit: deck.loaded && !deck.playing && atStart) { model.stop(deck.side) }
                .accessibilityLabel("Stop Deck \(deck.side.label) and go back to the beginning")
            divider
            button("RESTART", symbol: "backward.end.fill", tint: DjTheme.active, lit: false) { model.restart(deck.side) }
                .accessibilityLabel("Start Deck \(deck.side.label) over from the beginning")
        }
        .buttonStyle(DjPressStyle())
        .background(DjTheme.control, in: RoundedRectangle(cornerRadius: 4 * u, style: .continuous))
        .overlay(RoundedRectangle(cornerRadius: 4 * u, style: .continuous).strokeBorder(Color.black.opacity(0.6), lineWidth: 1))
        .shadow(color: .black.opacity(0.35), radius: 2, y: 1)
        .animation(.easeOut(duration: 0.15), value: deck.playing)
    }

    private var divider: some View {
        Rectangle().fill(Color.black.opacity(0.55)).frame(width: vertical ? cell.width * u : 1, height: vertical ? 1 : cell.height * u)
    }

    private func button(_ title: String, symbol: String, tint: Color, lit: Bool, action: @escaping () -> Void) -> some View {
        Button {
            DjTheme.tick()
            action()
        } label: {
            VStack(spacing: 2 * u) {
                Image(systemName: symbol)
                    .font(.system(size: 12 * u, weight: .bold))
                    .foregroundStyle(tint.opacity(deck.loaded ? 1 : 0.45))
                    .shadow(color: tint.opacity(lit ? 0.9 : 0), radius: 5 * u)
                Text(title)
                    .font(.system(size: 8 * u, weight: .bold))
                    .lineLimit(1)
                    .minimumScaleFactor(0.7)
                    .foregroundStyle(DjTheme.ink.opacity(deck.loaded ? 0.9 : 0.5))
            }
            .padding(.horizontal, 2 * u)
            .frame(width: cell.width * u, height: cell.height * u)
            .background(tint.opacity(lit ? 0.16 : 0))
            .contentShape(Rectangle())
        }
    }
}

struct DjCueButtons: View {
    let model: DjStudioModel
    let deck: DjDeck
    let u: CGFloat
    var vertical = false
    var segment: CGFloat = 58

    var body: some View {
        let layout = vertical ? AnyLayout(VStackLayout(spacing: 0)) : AnyLayout(HStackLayout(spacing: 0))
        layout {
            Button {
                DjTheme.tick()
                model.setCue(deck.side)
            } label: {
                HStack(spacing: 4 * u) {
                    Text("SET").font(.system(size: 12 * u, weight: .bold))
                    Circle().fill(DjTheme.ink).frame(width: 8 * u, height: 8 * u)
                }
                .frame(width: segment * u, height: 34 * u)
                .contentShape(Rectangle())
            }
            .accessibilityLabel("Set cue on Deck \(deck.side.label)")
            divider
            Button {
                DjTheme.tick()
                model.jumpToCue(deck.side)
            } label: {
                ZStack(alignment: .bottomTrailing) {
                    Image(systemName: "arrow.clockwise")
                        .font(.system(size: 17 * u, weight: .semibold))
                    Circle().fill(DjTheme.ink).frame(width: 6 * u, height: 6 * u).offset(x: 3 * u, y: 2 * u)
                }
                .frame(width: segment * u, height: 34 * u)
                .contentShape(Rectangle())
            }
            .accessibilityLabel("Jump to Deck \(deck.side.label) cue")
            divider
            Button {
                DjTheme.tick()
                model.sample(deck.side)
            } label: {
                let look = sampleLook
                HStack(spacing: 3 * u) {
                    Circle()
                        .fill(look.tint.opacity(look.lit ? 1 : 0.35))
                        .frame(width: 7 * u, height: 7 * u)
                        .shadow(color: look.tint.opacity(look.lit ? 1 : 0), radius: 4 * u)
                    Text(look.title).font(.system(size: 10 * u, weight: .bold))
                }
                .lineLimit(1)
                .minimumScaleFactor(0.7)
                .frame(width: segment * u, height: 34 * u)
                .background(look.tint.opacity(look.lit ? 0.3 : 0))
                .contentShape(Rectangle())
            }
            .animation(.easeOut(duration: 0.2), value: deck.sample)
            .accessibilityLabel(sampleLook.label)
        }
        .buttonStyle(DjPressStyle())
        .foregroundStyle(deck.loaded ? DjTheme.ink : DjTheme.muted)
        .background(DjTheme.control, in: RoundedRectangle(cornerRadius: 4 * u, style: .continuous))
        .overlay(RoundedRectangle(cornerRadius: 4 * u, style: .continuous).strokeBorder(Color.black.opacity(0.6), lineWidth: 1))
        .shadow(color: .black.opacity(0.35), radius: 2, y: 1)
    }

    private var sampleLook: (title: String, tint: Color, lit: Bool, label: String) {
        switch deck.sample {
        case .idle: return ("SAMPLE", DjTheme.muted, false, "Start sampling Deck \(deck.side.label)")
        case .recording: return ("REC", DjTheme.record, true, "Stop sampling Deck \(deck.side.label)")
        case .armed: return ("ARMED", DjTheme.active, true, "Play Deck \(deck.side.label)’s sample")
        }
    }

    private var divider: some View {
        Rectangle().fill(Color.black.opacity(0.55)).frame(width: vertical ? segment * u : 1, height: vertical ? 1 : 34 * u)
    }
}

struct DjPitchStrip: View {
    let model: DjStudioModel
    let deck: DjDeck
    let u: CGFloat

    var body: some View {
        VStack(spacing: 6 * u) {
            DjRoundButton(size: 30 * u, action: { model.sheet = .deck(deck.side) }) {
                Image(systemName: "slider.vertical.3")
            }
            .accessibilityLabel("Deck \(deck.side.label) settings")
            VStack(spacing: 6 * u) {
                Button {
                    model.tapTempo(deck.side)
                } label: {
                    Text(deck.bpm > 0 ? String(format: "%.1f", deck.shownBpm) : abs(deck.effectiveRate - 1) > 0.0005 ? String(format: "%+.1f%%", (deck.effectiveRate - 1) * 100) : "TAP")
                        .font(.system(size: (deck.bpm > 0 ? 16 : 13) * u, weight: .regular))
                        .monospacedDigit()
                        .foregroundStyle(deck.bpm > 0 || abs(deck.effectiveRate - 1) > 0.0005 ? DjTheme.ink : DjTheme.muted)
                        .lineLimit(1)
                        .minimumScaleFactor(0.6)
                        .frame(maxWidth: .infinity, minHeight: 28 * u)
                        .background(Color.black.opacity(0.35), in: RoundedRectangle(cornerRadius: 3 * u, style: .continuous))
                }
                .buttonStyle(DjPressStyle())
                .contextMenu {
                    Button("Clear BPM", systemImage: "xmark") { model.clearBpm(deck.side) }
                }
                .accessibilityLabel(deck.bpm > 0 ? "\(String(format: "%.1f", deck.shownBpm)) BPM, tap to retap" : "Tap tempo")
                Button {
                    DjTheme.tick()
                    model.sync(deck.side)
                } label: {
                    Text("SYNC")
                        .font(.system(size: 11 * u, weight: .bold))
                        .foregroundStyle(DjTheme.ink.opacity(0.9))
                        .frame(maxWidth: .infinity, minHeight: 24 * u)
                        .background(DjTheme.control, in: RoundedRectangle(cornerRadius: 3 * u, style: .continuous))
                        .overlay(RoundedRectangle(cornerRadius: 3 * u, style: .continuous).strokeBorder(Color.black.opacity(0.6), lineWidth: 1))
                }
                .buttonStyle(DjPressStyle())
                DjPitchFader(model: model, deck: deck, u: u)
                    .frame(maxHeight: .infinity)
            }
            .padding(6 * u)
            .background(DjTheme.panel, in: RoundedRectangle(cornerRadius: 5 * u, style: .continuous))
            .overlay(RoundedRectangle(cornerRadius: 5 * u, style: .continuous).strokeBorder(DjTheme.edge, lineWidth: 1))
            HStack(spacing: 4 * u) {
                DjHoldButton(symbol: "minus", u: u, label: "Slow Deck \(deck.side.label) while held") { held in
                    model.bend(deck.side, held ? -1 : 0)
                }
                DjHoldButton(symbol: "plus", u: u, label: "Speed up Deck \(deck.side.label) while held") { held in
                    model.bend(deck.side, held ? 1 : 0)
                }
            }
        }
        .frame(width: 58 * u)
    }
}

struct DjHoldButton: View {
    let symbol: String
    let u: CGFloat
    let label: String
    let changed: (Bool) -> Void
    @GestureState private var held = false

    var body: some View {
        Image(systemName: symbol)
            .font(.system(size: 16 * u, weight: .light))
            .foregroundStyle(held ? DjTheme.active : DjTheme.muted)
            .frame(width: 26 * u, height: 26 * u)
            .contentShape(Rectangle())
            .gesture(DragGesture(minimumDistance: 0).updating($held) { _, state, _ in state = true })
            .onChange(of: held) { _, now in changed(now) }
            .accessibilityElement()
            .accessibilityLabel(label)
            .accessibilityAddTraits(.isButton)
    }
}

struct DjPitchFader: View {
    let model: DjStudioModel
    let deck: DjDeck
    let u: CGFloat
    @State private var startRate: Double?
    @GestureState private var dragging = false

    var body: some View {
        GeometryReader { geometry in
            let height = geometry.size.height
            let knob = CGSize(width: 40 * u, height: 22 * u)
            let travel = max(1, height - knob.height)
            let fraction = (deck.rate - (1 - DjStudio.pitchRange)) / (2 * DjStudio.pitchRange)
            let y = knob.height / 2 + CGFloat(1 - fraction) * travel
            ZStack(alignment: .top) {
                ForEach(0..<9, id: \.self) { index in
                    Rectangle()
                        .fill(Color.white.opacity(index == 4 ? 0.35 : 0.12))
                        .frame(width: index == 4 ? 26 * u : 18 * u, height: 1)
                        .offset(y: knob.height / 2 + travel * CGFloat(index) / 8)
                }
                Capsule()
                    .fill(Color.black.opacity(0.7))
                    .overlay(Capsule().strokeBorder(Color.white.opacity(0.06), lineWidth: 1))
                    .frame(width: 6 * u, height: travel)
                    .offset(y: knob.height / 2)
                Circle().fill(DjTheme.play).frame(width: 3 * u, height: 3 * u)
                    .offset(x: -17 * u, y: knob.height / 2 + travel / 2 - 1.5 * u)
                DjFaderKnob(size: knob, horizontal: false)
                    .offset(y: y - knob.height / 2)
            }
            .frame(width: geometry.size.width, height: height)
            .contentShape(Rectangle())
            .gesture(
                DragGesture(minimumDistance: 1)
                    .updating($dragging) { _, state, _ in state = true }
                    .onChanged { value in
                        let start = startRate ?? deck.rate
                        if startRate == nil { startRate = start }
                        model.setRate(deck.side, start - Double(value.translation.height / travel) * 2 * DjStudio.pitchRange)
                    }
                    .onEnded { _ in startRate = nil })
            .onChange(of: dragging) { _, now in if !now { startRate = nil } }
            .onTapGesture(count: 2) {
                DjTheme.tick()
                model.setRate(deck.side, 1)
            }
        }
        .accessibilityElement()
        .accessibilityLabel("Deck \(deck.side.label) tempo")
        .accessibilityValue(String(format: "%+.1f percent", (deck.rate - 1) * 100))
        .accessibilityAdjustableAction { direction in
            model.setRate(deck.side, deck.rate + (direction == .increment ? 0.005 : -0.005))
        }
    }
}

struct DjLevelFader: View {
    let model: DjStudioModel
    let deck: DjDeck
    let u: CGFloat
    @State private var startLevel: Double?

    var body: some View {
        VStack(spacing: 4 * u) {
            Image(systemName: deck.level < 0.01 ? "speaker.slash.fill" : deck.level < 0.5 ? "speaker.wave.1.fill" : "speaker.wave.2.fill")
                .font(.system(size: 11 * u, weight: .semibold))
                .foregroundStyle(DjTheme.muted)
                .frame(height: 16 * u)
            GeometryReader { geometry in
                let knob = CGSize(width: min(geometry.size.width, 24 * u), height: 14 * u)
                let travel = max(1, geometry.size.height - knob.height)
                let y = knob.height / 2 + CGFloat(1 - deck.level) * travel
                ZStack(alignment: .top) {
                    Capsule()
                        .fill(Color.black.opacity(0.7))
                        .frame(width: 5 * u, height: travel)
                        .offset(y: knob.height / 2)
                    Capsule()
                        .fill(DjTheme.play.opacity(0.85))
                        .frame(width: 5 * u, height: max(0, travel - (y - knob.height / 2)))
                        .offset(y: y)
                    DjFaderKnob(size: knob, horizontal: false)
                        .offset(y: y - knob.height / 2)
                }
                .frame(width: geometry.size.width, height: geometry.size.height)
                .contentShape(Rectangle())
                .gesture(
                    DragGesture(minimumDistance: 0)
                        .onChanged { value in
                            let start = startLevel ?? deck.level
                            if startLevel == nil { startLevel = start }
                            model.setLevel(deck.side, start - Double(value.translation.height / travel))
                        }
                        .onEnded { _ in startLevel = nil })
                .onTapGesture(count: 2) {
                    DjTheme.tick()
                    model.setLevel(deck.side, 1)
                }
            }
            Text("\(Int((deck.level * 100).rounded()))")
                .font(.system(size: 10 * u, weight: .semibold))
                .monospacedDigit()
                .foregroundStyle(DjTheme.muted)
                .frame(height: 14 * u)
        }
        .padding(.vertical, 4 * u)
        .background(DjTheme.panel, in: RoundedRectangle(cornerRadius: 5 * u, style: .continuous))
        .overlay(RoundedRectangle(cornerRadius: 5 * u, style: .continuous).strokeBorder(DjTheme.edge, lineWidth: 1))
        .accessibilityElement()
        .accessibilityLabel("Deck \(deck.side.label) volume")
        .accessibilityValue("\(Int((deck.level * 100).rounded())) percent")
        .accessibilityAdjustableAction { direction in
            model.setLevel(deck.side, deck.level + (direction == .increment ? 0.05 : -0.05))
        }
    }
}

struct DjFaderKnob: View, Equatable {
    let size: CGSize
    let horizontal: Bool

    var body: some View {
        ZStack {
            RoundedRectangle(cornerRadius: min(size.width, size.height) * 0.22, style: .continuous)
                .fill(LinearGradient(colors: [DjTheme.rgb(0xFAFAFB), DjTheme.rgb(0xBFC3C9)], startPoint: horizontal ? .leading : .top, endPoint: horizontal ? .trailing : .bottom))
            RoundedRectangle(cornerRadius: min(size.width, size.height) * 0.22, style: .continuous)
                .strokeBorder(Color.black.opacity(0.35), lineWidth: 1)
            if horizontal {
                Rectangle().fill(DjTheme.rgb(0x8A8F97)).frame(width: 2, height: size.height * 0.92)
            } else {
                Rectangle().fill(DjTheme.rgb(0x8A8F97)).frame(width: size.width * 0.92, height: 2)
            }
        }
        .frame(width: size.width, height: size.height)
        .shadow(color: .black.opacity(0.5), radius: 3, y: 2)
    }
}

struct DjCrossfader: View {
    let model: DjStudioModel
    let u: CGFloat
    @State private var start: Double?
    @GestureState private var dragging = false

    var body: some View {
        HStack(spacing: 10 * u) {
            arrow(.a)
            GeometryReader { geometry in
                let knob = CGSize(width: 34 * u, height: min(geometry.size.height, 54 * u))
                let travel = max(1, geometry.size.width - knob.width)
                ZStack {
                    Capsule()
                        .fill(LinearGradient(colors: [DjTheme.rgb(0x8F949B), DjTheme.rgb(0x5B5F66)], startPoint: .top, endPoint: .bottom))
                        .overlay(Capsule().strokeBorder(Color.black.opacity(0.55), lineWidth: 1))
                        .frame(height: 12 * u)
                        .padding(.horizontal, knob.width / 2 - 6 * u)
                    Rectangle().fill(Color.white.opacity(0.18)).frame(width: 1, height: knob.height * 0.9)
                    DjFaderKnob(size: knob, horizontal: true)
                        .offset(x: CGFloat(model.crossfader) * travel / 2)
                }
                .frame(width: geometry.size.width, height: geometry.size.height)
                .contentShape(Rectangle())
                .gesture(
                    DragGesture(minimumDistance: 1)
                        .updating($dragging) { _, state, _ in state = true }
                        .onChanged { value in
                            let origin = start ?? model.crossfader
                            if start == nil { start = origin }
                            model.setCrossfader(origin + Double(value.translation.width / travel) * 2)
                        }
                        .onEnded { _ in start = nil })
                .onChange(of: dragging) { _, now in if !now { start = nil } }
                .onTapGesture(count: 2) {
                    DjTheme.tick()
                    model.setCrossfader(0)
                }
            }
            .accessibilityElement()
            .accessibilityLabel("Crossfader")
            .accessibilityValue(model.crossfader == 0 ? "Centre" : model.crossfader < 0 ? "\(Int(-model.crossfader * 100)) percent Deck A" : "\(Int(model.crossfader * 100)) percent Deck B")
            .accessibilityAdjustableAction { direction in
                model.setCrossfader(model.crossfader + (direction == .increment ? 0.1 : -0.1))
            }
            arrow(.b)
        }
    }

    private func arrow(_ side: DjDeckSide) -> some View {
        Button {
            DjTheme.tick()
            model.slam(side)
        } label: {
            Image(systemName: side == .a ? "arrowtriangle.left.fill" : "arrowtriangle.right.fill")
                .font(.system(size: 13 * u))
                .foregroundStyle(Color.black.opacity(0.45))
                .frame(width: 24 * u, height: 40 * u)
                .contentShape(Rectangle())
        }
        .buttonStyle(DjPressStyle())
        .accessibilityLabel("Crossfade fully to Deck \(side.label)")
    }
}

struct DjAutomixButton: View {
    let model: DjStudioModel
    let u: CGFloat

    var body: some View {
        let on = model.automixOn
        let height = 28 * u
        Button {
            DjTheme.tick()
            model.automix()
        } label: {
            HStack(spacing: 6 * u) {
                Image(systemName: model.automixing ? "arrow.left.arrow.right" : model.crate.focus != .everything ? "scope" : "shuffle")
                    .font(.system(size: 12 * u, weight: .bold))
                    .frame(width: 14 * u)
                Text("AUTOMIX")
                    .font(.system(size: 12 * u, weight: .bold))
                    .tracking(0.6 * u)
            }
            .foregroundStyle(on ? DjTheme.rgb(0x08172A) : DjTheme.ink)
            .padding(.horizontal, 14 * u)
            .frame(height: height)
            .background {
                Capsule()
                    .fill(on ? AnyShapeStyle(DjTheme.active) : AnyShapeStyle(DjTheme.control))
                    .shadow(color: DjTheme.active.opacity(on ? 0.65 : 0), radius: 7 * u)
            }
            .overlay {
                Capsule().strokeBorder(on ? Color.white.opacity(0.35) : DjTheme.active.opacity(0.55), lineWidth: 1)
            }
            .shadow(color: .black.opacity(0.45), radius: 2, y: 1)
            .frame(minWidth: 44, minHeight: 44)
            .contentShape(Rectangle())
        }
        .buttonStyle(DjPressStyle())
        .contextMenu {
            Button(model.automixing ? "Stop the Fade" : "Mix to the Other Deck Now", systemImage: "arrow.left.arrow.right") { model.mixNow() }
            Menu {
                Button("Everything") { model.focusAutomix(.everything, start: false) }
                ForEach(model.crate.playlists) { (list: DjPlaylist) in
                    Button(list.name) { model.focusAutomix(.playlist(list.id), start: false) }
                }
                ForEach(model.crate.tagCounts) { (tag: DjTagCount) in
                    Button("#\(tag.name)") { model.focusAutomix(.tag(tag.name), start: false) }
                }
            } label: {
                Label("Plays \(model.crate.label(model.crate.focus))", systemImage: "scope")
            }
            Button("Playlists…", systemImage: "music.note.list") { model.sheet = .crate }
        }
        .animation(.easeOut(duration: 0.2), value: on)
        .accessibilityLabel("Automix")
        .accessibilityValue((on ? "On" : "Off") + (model.crate.focus == .everything ? "" : ", playing \(model.crate.label(model.crate.focus))"))
        .accessibilityAddTraits(on ? .isSelected : [])
        .accessibilityHint("When on, queued songs, then songs you have played or the chosen playlist or tag, play one after another with an eight-second crossfade")
        .accessibilityAction(named: "Mix to the other deck now") { model.mixNow() }
    }
}

struct DjNotice: View {
    let model: DjStudioModel

    var body: some View {
        ZStack {
            if let notice = model.notice { bubble(notice) }
        }
        .allowsHitTesting(false)
        .animation(.easeOut(duration: 0.2), value: model.notice)
    }

    private func bubble(_ notice: String) -> some View {
        Text(notice)
            .font(.footnote.weight(.medium))
            .foregroundStyle(DjTheme.ink)
            .multilineTextAlignment(.center)
            .padding(.horizontal, 14)
            .padding(.vertical, 8)
            .background(.ultraThinMaterial, in: Capsule())
            .environment(\.colorScheme, .dark)
            .padding(.top, 6)
            .transition(.move(edge: .top).combined(with: .opacity))
            .accessibilityAddTraits(.updatesFrequently)
            .allowsHitTesting(false)
    }
}
