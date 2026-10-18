import FMApi
import FMPackages
import SwiftUI
import UIKit

struct DjNowPlayingChip: View {
    private let hub = DjStudioHub.shared

    var body: some View {
        if let model = hub.model, !model.onScreen, let deck = model.audibleDeck {
            Button { SurfaceRouter.open(.videoMixDjStudio) } label: {
                DjMiniRecord(deck: deck, size: 44)
                    .contentShape(Circle())
            }
            .buttonStyle(ShellBarButtonStyle())
            .contextMenu {
                Button("Stop the Music", systemImage: "stop.fill") { model.stopAll() }
            }
            .transition(.scale.combined(with: .opacity))
            .accessibilityElement(children: .ignore)
            .accessibilityLabel(Text(deck.title.isEmpty ? "Deck \(deck.side.label) is playing" : "Deck \(deck.side.label) is playing \(deck.title)"))
            .accessibilityHint(Text("Opens Video Mix DJ Studio"))
            .accessibilityAddTraits(.isButton)
            .accessibilityIdentifier("top-dj-now-playing")
        }
    }
}

struct DjMiniRecord: View {
    let deck: DjDeck
    let size: CGFloat
    @State private var meter = DjMiniMeter()

    var body: some View {
        TimelineView(.animation(minimumInterval: 1.0 / 30, paused: !deck.moving)) { timeline in
            let time = deck.shown()
            let turn = deck.spin.turn
            let beat = DjStudio.amplitude(deck.waveform, time: time, duration: deck.duration)
            let reading = meter.read(deck, time: time, at: timeline.date.timeIntervalSinceReferenceDate)
            ZStack {
                ZStack {
                    Circle().fill(DjTheme.rgb(0x111214))
                    if let artwork = deck.artwork {
                        Image(uiImage: artwork)
                            .resizable()
                            .interpolation(.medium)
                            .frame(width: size, height: size)
                            .clipShape(Circle())
                    }
                    DjMiniGrooves(size: size).equatable()
                }
                .rotationEffect(.degrees(DjStudio.turnDegrees(turn)))
                DjMiniSpectrum(meter: reading, hue: beat.hue, size: size)
            }
            .frame(width: size, height: size)
        }
    }
}

@MainActor
final class DjMiniMeter {
    private var meter = DjSpectrumMeter()

    func read(_ deck: DjDeck, time: Double, at now: Double) -> DjSpectrumMeter {
        let target: [Double]
        if !deck.playing {
            target = [Double](repeating: 0, count: DjSpectrum.bandCount)
        } else if let audio = deck.scratchAudio, abs(audio.duration - deck.duration) < 2 {
            target = DjSpectrum.bands(audio, at: time)
        } else {
            target = DjSpectrum.bands(deck.waveform, time: time, duration: deck.duration)
        }
        meter.update(target, at: now)
        return meter
    }
}

struct DjMiniSpectrum: View {
    let meter: DjSpectrumMeter
    let hue: Double
    let size: CGFloat

    var body: some View {
        Canvas { context, area in
            let centre = CGPoint(x: area.width / 2, y: area.height / 2)
            let radius = min(area.width, area.height) / 2
            let bass = CGFloat(meter.bass)
            let kick = CGFloat(meter.kick)
            let energy = CGFloat(meter.energy)
            func circle(_ at: CGPoint, _ r: CGFloat) -> Path {
                Path(ellipseIn: CGRect(x: at.x - r, y: at.y - r, width: r * 2, height: r * 2))
            }
            context.fill(circle(centre, radius), with: .radialGradient(
                Gradient(colors: [.black.opacity(0.15 + 0.45 * energy), .black.opacity(0.3 * energy)]),
                center: centre, startRadius: 0, endRadius: radius))
            let base = Color(hue: hue, saturation: 0.8, brightness: 1)
            context.drawLayer { layer in
                layer.blendMode = .plusLighter
                let glow = radius * min(1.2, 0.3 + 0.75 * bass + 0.25 * kick)
                layer.fill(circle(centre, glow), with: .radialGradient(
                    Gradient(colors: [base.opacity(0.2 + 0.6 * bass), base.opacity(0)]),
                    center: centre, startRadius: 0, endRadius: glow))
                let count = meter.values.count
                guard count > 0 else { return }
                let spokes = count * 2
                let inner = radius * (0.22 + 0.12 * bass + 0.1 * kick)
                let reach = radius * 0.95 - inner
                let width = max(1.1, 2 * .pi * inner / CGFloat(spokes) * 0.75)
                for spoke in 0..<spokes {
                    let band = spoke < count ? spoke : spokes - 1 - spoke
                    let angle = meter.phase + Double(spoke) / Double(spokes) * 2 * .pi
                    let dx = CGFloat(cos(angle))
                    let dy = CGFloat(sin(angle))
                    func point(_ distance: CGFloat) -> CGPoint { CGPoint(x: centre.x + dx * distance, y: centre.y + dy * distance) }
                    let place = Double(band) / Double(max(1, count - 1))
                    let colour = Color(hue: (hue + place * 0.7).truncatingRemainder(dividingBy: 1), saturation: 0.75, brightness: 1)
                    var bar = Path()
                    bar.move(to: point(inner))
                    bar.addLine(to: point(inner + max(width * 0.5, reach * CGFloat(meter.values[band]))))
                    layer.stroke(bar, with: .color(colour.opacity(0.95)), style: StrokeStyle(lineWidth: width, lineCap: .round))
                    let peak = CGFloat(meter.peaks[band])
                    if peak > 0.05 {
                        layer.fill(circle(point(inner + reach * peak), width * 0.6), with: .color(.white.opacity(0.35 + 0.55 * peak)))
                    }
                }
            }
            context.fill(circle(centre, radius * (0.07 + 0.06 * bass + 0.12 * kick)), with: .color(.white.opacity(0.5 + 0.5 * kick)))
        }
        .frame(width: size, height: size)
        .clipShape(Circle())
        .overlay(Circle().strokeBorder(Color.white.opacity(0.18), lineWidth: 0.5))
    }
}

struct DjMiniGrooves: View, Equatable {
    let size: CGFloat

    var body: some View {
        Canvas { context, area in
            let centre = CGPoint(x: area.width / 2, y: area.height / 2)
            for fraction: CGFloat in [0.36, 0.42, 0.48] {
                let radius = area.width * fraction
                context.stroke(
                    Path(ellipseIn: CGRect(x: centre.x - radius, y: centre.y - radius, width: radius * 2, height: radius * 2)),
                    with: .color(.white.opacity(0.12)), lineWidth: 0.5)
            }
        }
        .frame(width: size, height: size)
    }
}
