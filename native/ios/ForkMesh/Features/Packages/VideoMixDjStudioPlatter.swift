import FMPackages
import SwiftUI
import UIKit

final class DjTouchTrack {
    var lastAngle: Double?
    var pressed = false
}

struct DjPlatter: View {
    let model: DjStudioModel
    let deck: DjDeck
    let d: CGFloat
    @State private var track = DjTouchTrack()
    @GestureState private var touching = false

    var body: some View {
        ZStack {
            DjPlinth(d: d).equatable()
            DjRecord(model: model, deck: deck, d: d)
                .contentShape(Circle().scale(0.86))
                .gesture(scratch)
                .onChange(of: touching) { _, now in
                    guard !now else { return }
                    if track.lastAngle != nil {
                        track.lastAngle = nil
                        model.scratchEnded(deck.side)
                    } else if track.pressed {
                        model.touchEnded(deck.side)
                    }
                    track.pressed = false
                }
                .accessibilityElement()
                .accessibilityLabel("Deck \(deck.side.label) turntable")
                .accessibilityValue(deck.loaded ? (deck.playing ? "Playing" : "Paused") : "Empty")
                .accessibilityHint(deck.loaded ? "Touch the record to slow it briefly; drag around it to scratch" : "Double-tap to load a video")
                .accessibilityAction { if !deck.loaded { model.openLoad(deck.side) } }
            DjTonearm(deck: deck, d: d)
                .allowsHitTesting(false)
            DjRoundButton(size: d * 0.13, action: { model.openLoad(deck.side) }) {
                ZStack(alignment: .bottomTrailing) {
                    Image(systemName: "music.note")
                    Image(systemName: "plus.circle.fill")
                        .font(.system(size: d * 0.035, weight: .bold))
                        .offset(x: d * 0.012, y: d * 0.004)
                }
            }
            .accessibilityLabel("Load Deck \(deck.side.label)")
            .position(x: d * 0.13, y: d * 0.13)
            DjCueLight(model: model, deck: deck, d: d)
                .position(x: d * 0.12, y: d * 0.88)
            DjDropRing(model: model, side: deck.side, d: d)
        }
        .frame(width: d, height: d)
        .onGeometryChange(for: CGRect.self, of: { $0.frame(in: .named(DjStudioView.space)) }) { frame in
            model.deckFrames[deck.side] = frame
        }
    }

    private var scratch: some Gesture {
        DragGesture(minimumDistance: 0)
            .updating($touching) { _, state, _ in state = true }
            .onChanged { value in
                if !track.pressed {
                    track.pressed = true
                    model.touchBegan(deck.side)
                }
                let dx = Double(value.location.x - d / 2)
                let dy = Double(value.location.y - d / 2)
                let angle = atan2(dy, dx)
                guard let last = track.lastAngle else {
                    let threshold = max(DjStudio.scratchThreshold, Double(d) * 0.05)
                    guard deck.loaded, DjStudio.isScratch(dx: Double(value.translation.width), dy: Double(value.translation.height), threshold: threshold) else { return }
                    track.lastAngle = angle
                    model.scratchBegan(deck.side)
                    return
                }
                track.lastAngle = angle
                guard (dx * dx + dy * dy).squareRoot() > Double(d) * 0.07 else { return }
                var change = angle - last
                if change > Double.pi { change -= 2 * Double.pi } else if change < -Double.pi { change += 2 * Double.pi }
                model.scratchMoved(deck.side, radians: change)
            }
            .onEnded { value in
                let scratched = track.lastAngle != nil
                track.lastAngle = nil
                track.pressed = false
                if scratched {
                    model.scratchEnded(deck.side)
                } else if deck.loaded {
                    model.touchEnded(deck.side)
                } else if abs(value.translation.width) + abs(value.translation.height) < 10 {
                    model.openLoad(deck.side)
                }
            }
    }
}

struct DjRecord: View {
    let model: DjStudioModel
    let deck: DjDeck
    let d: CGFloat

    var body: some View {
        let r = d * 0.82
        ZStack {
            DjVinyl(d: r).equatable()
            TimelineView(.animation(paused: !deck.moving)) { _ in
                let angle = Angle.degrees(DjStudio.turnDegrees(deck.shown()))
                ZStack {
                    if let artwork = deck.artwork {
                        Image(uiImage: artwork)
                            .resizable()
                            .interpolation(.medium)
                            .frame(width: r, height: r)
                            .modifier(DjGainOpacity(model: model, side: deck.side))
                            .rotationEffect(angle)
                    }
                    DjRimDots(d: d).equatable().rotationEffect(angle)
                    DjLabel(d: d).equatable().rotationEffect(angle)
                }
            }
        }
        .frame(width: d, height: d)
    }
}

struct DjDropRing: View {
    let model: DjStudioModel
    let side: DjDeckSide
    let d: CGFloat

    var body: some View {
        let dragging = model.draggedId != nil
        let target = model.dropSide == side
        Circle()
            .strokeBorder(DjTheme.active.opacity(target ? 1 : dragging ? 0.35 : 0), style: StrokeStyle(lineWidth: target ? 4 : 2, dash: target ? [] : [6, 5]))
            .background(Circle().fill(DjTheme.active.opacity(target ? 0.18 : 0)))
            .frame(width: d * 0.92, height: d * 0.92)
            .animation(.easeOut(duration: 0.15), value: target)
            .animation(.easeOut(duration: 0.15), value: dragging)
            .allowsHitTesting(false)
            .accessibilityHidden(true)
    }
}

struct DjGainOpacity: ViewModifier {
    let model: DjStudioModel
    let side: DjDeckSide

    func body(content: Content) -> some View {
        content.opacity(0.4 + 0.6 * DjStudio.gain(side, crossfader: model.crossfader))
    }
}

struct DjPlinth: View, Equatable {
    let d: CGFloat

    var body: some View {
        let face = LinearGradient(colors: [DjTheme.rgb(0x33363C), DjTheme.rgb(0x1F2125)], startPoint: .top, endPoint: .bottom)
        ZStack {
            Circle().fill(face).frame(width: d * 0.96, height: d * 0.96)
                .shadow(color: .black.opacity(0.6), radius: d * 0.025, y: d * 0.012)
            Circle().fill(face).frame(width: d * 0.2, height: d * 0.2).position(x: d * 0.13, y: d * 0.13)
            Circle().fill(DjTheme.rgb(0x17181B)).frame(width: d * 0.22, height: d * 0.22).position(x: d * 0.885, y: d * 0.115)
            Circle().fill(face).frame(width: d * 0.12, height: d * 0.12).position(x: d * 0.12, y: d * 0.88)
            Circle().strokeBorder(Color.black.opacity(0.5), lineWidth: 1).frame(width: d * 0.96, height: d * 0.96)
            Circle()
                .fill(LinearGradient(colors: [DjTheme.rgb(0x4A4D53), DjTheme.rgb(0x2A2C30)], startPoint: .top, endPoint: .bottom))
                .frame(width: d * 0.9, height: d * 0.9)
            Circle().strokeBorder(Color.black.opacity(0.55), lineWidth: 1.5).frame(width: d * 0.83, height: d * 0.83)
        }
        .frame(width: d, height: d)
    }
}

struct DjVinyl: View, Equatable {
    let d: CGFloat

    var body: some View {
        Canvas { context, size in
            let centre = CGPoint(x: size.width / 2, y: size.height / 2)
            context.fill(Path(ellipseIn: CGRect(origin: .zero, size: size)), with: .color(DjTheme.rgb(0x111214)))
            var radius = size.width * 0.17
            while radius < size.width * 0.49 {
                context.stroke(
                    Path(ellipseIn: CGRect(x: centre.x - radius, y: centre.y - radius, width: radius * 2, height: radius * 2)),
                    with: .color(.white.opacity(0.045)), lineWidth: 0.6)
                radius += max(2, size.width * 0.012)
            }
        }
        .frame(width: d, height: d)
    }
}

struct DjRimDots: View, Equatable {
    let d: CGFloat

    var body: some View {
        Canvas { context, size in
            let centre = CGPoint(x: size.width / 2, y: size.height / 2)
            let radius = size.width * 0.435
            let dot = max(1.2, size.width * 0.007)
            var path = Path()
            for index in 0..<110 {
                let angle = Double(index) / 110 * 2 * Double.pi
                let point = CGPoint(x: centre.x + CGFloat(cos(angle)) * radius, y: centre.y + CGFloat(sin(angle)) * radius)
                path.addEllipse(in: CGRect(x: point.x - dot / 2, y: point.y - dot / 2, width: dot, height: dot))
            }
            context.fill(path, with: .color(.white.opacity(0.55)))
        }
        .frame(width: d, height: d)
        .allowsHitTesting(false)
    }
}

struct DjLabel: View, Equatable {
    let d: CGFloat

    var body: some View {
        ZStack {
            Circle().fill(Color.white.opacity(0.24)).frame(width: d * 0.27, height: d * 0.27)
            Circle().fill(Color.white.opacity(0.35)).frame(width: d * 0.018, height: d * 0.018).offset(x: d * 0.09, y: -d * 0.03)
            Circle()
                .fill(RadialGradient(colors: [DjTheme.rgb(0xD9DCE0), DjTheme.rgb(0x6D7178)], center: .center, startRadius: 0, endRadius: d * 0.014))
                .frame(width: d * 0.026, height: d * 0.026)
        }
        .frame(width: d, height: d)
        .allowsHitTesting(false)
    }
}

struct DjTonearm: View {
    let deck: DjDeck
    let d: CGFloat

    var body: some View {
        let progress = deck.duration > 0 ? min(1, max(0, deck.time / deck.duration)) : 0
        DjTonearmShape(d: d).equatable()
            .rotationEffect(.degrees(deck.loaded ? 12 + 16 * progress : 0), anchor: UnitPoint(x: 0.885, y: 0.115))
            .animation(.easeInOut(duration: 0.5), value: deck.loaded)
    }
}

struct DjTonearmShape: View, Equatable {
    let d: CGFloat

    var body: some View {
        Canvas { context, size in
            let w = size.width
            let pivot = CGPoint(x: w * 0.885, y: w * 0.115)
            let elbow = CGPoint(x: w * 0.885, y: w * 0.72)
            let head = CGPoint(x: w * 0.865, y: w * 0.8)
            let silver = GraphicsContext.Shading.linearGradient(
                Gradient(colors: [DjTheme.rgb(0x8C9097), DjTheme.rgb(0xF4F5F7), DjTheme.rgb(0x9A9EA5)]),
                startPoint: CGPoint(x: pivot.x - w * 0.02, y: 0), endPoint: CGPoint(x: pivot.x + w * 0.02, y: 0))
            let ring = w * 0.075
            context.fill(Path(ellipseIn: CGRect(x: pivot.x - ring, y: pivot.y - ring, width: ring * 2, height: ring * 2)), with: .color(DjTheme.rgb(0x2B2D32)))
            context.stroke(Path(ellipseIn: CGRect(x: pivot.x - ring, y: pivot.y - ring, width: ring * 2, height: ring * 2)), with: .color(.white.opacity(0.25)), lineWidth: 1)
            context.fill(
                Path(roundedRect: CGRect(x: pivot.x - w * 0.032, y: pivot.y - w * 0.11, width: w * 0.064, height: w * 0.085), cornerRadius: w * 0.012),
                with: silver)
            var arm = Path()
            arm.move(to: pivot)
            arm.addLine(to: elbow)
            arm.addLine(to: head)
            context.stroke(arm, with: silver, style: StrokeStyle(lineWidth: w * 0.02, lineCap: .round, lineJoin: .round))
            var shell = Path()
            shell.move(to: CGPoint(x: head.x + w * 0.012, y: head.y - w * 0.015))
            shell.addLine(to: CGPoint(x: head.x - w * 0.022, y: head.y + w * 0.04))
            context.stroke(shell, with: silver, style: StrokeStyle(lineWidth: w * 0.04, lineCap: .round))
            context.fill(
                Path(roundedRect: CGRect(x: head.x - w * 0.03, y: head.y + w * 0.035, width: w * 0.022, height: w * 0.022), cornerRadius: w * 0.004),
                with: .color(DjTheme.rgb(0x1A1B1E)))
            let cap = w * 0.03
            context.fill(Path(ellipseIn: CGRect(x: pivot.x - cap, y: pivot.y - cap, width: cap * 2, height: cap * 2)), with: silver)
        }
        .frame(width: d, height: d)
        .shadow(color: .black.opacity(0.55), radius: d * 0.012, x: d * 0.008, y: d * 0.012)
    }
}

struct DjCueLight: View {
    let model: DjStudioModel
    let deck: DjDeck
    let d: CGFloat

    var body: some View {
        Button {
            DjTheme.tick()
            model.cueStop(deck.side)
        } label: {
            ZStack {
                Circle().fill(DjTheme.rgb(0x101113)).frame(width: d * 0.075, height: d * 0.075)
                Circle()
                    .fill(RadialGradient(
                        colors: [deck.playing ? DjTheme.rgb(0xFF4040) : DjTheme.rgb(0x3A1416), DjTheme.rgb(0x200A0B)],
                        center: .center, startRadius: 0, endRadius: d * 0.03))
                    .frame(width: d * 0.05, height: d * 0.05)
                    .shadow(color: DjTheme.record.opacity(deck.playing ? 0.9 : 0), radius: d * 0.04)
                Circle().strokeBorder(Color.white.opacity(0.15), lineWidth: 1).frame(width: d * 0.075, height: d * 0.075)
            }
            .frame(width: max(32, d * 0.11), height: max(32, d * 0.11))
            .contentShape(Circle())
        }
        .buttonStyle(DjPressStyle())
        .animation(.easeOut(duration: 0.2), value: deck.playing)
        .accessibilityLabel("Return Deck \(deck.side.label) to cue and stop")
    }
}
