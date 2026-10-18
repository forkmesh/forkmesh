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

    var body: some View {
        TimelineView(.animation(minimumInterval: 1.0 / 30, paused: !deck.moving)) { _ in
            let time = deck.shown()
            let turn = deck.spin.turn
            let beat = DjStudio.amplitude(deck.waveform, time: time, duration: deck.duration)
            ZStack {
                ZStack {
                    Circle().fill(DjTheme.rgb(0x111214))
                    if let artwork = deck.artwork {
                        Image(uiImage: artwork)
                            .resizable()
                            .interpolation(.low)
                            .frame(width: size * 0.92, height: size * 0.92)
                            .opacity(0.8)
                    }
                    DjMiniGrooves(size: size).equatable()
                }
                .rotationEffect(.degrees(DjStudio.turnDegrees(turn)))
                DjMiniPulse(level: deck.playing ? beat.level : 0, hue: beat.hue, size: size * 0.3)
            }
            .frame(width: size, height: size)
        }
    }
}

struct DjMiniPulse: View {
    let level: Double
    let hue: Double
    let size: CGFloat

    var body: some View {
        let colour = DjTheme.waveColors[min(DjTheme.waveColors.count - 1, max(0, Int(hue * Double(DjTheme.waveColors.count))))]
        let amount = CGFloat(pow(min(1, max(0, level)), 2))
        ZStack {
            Circle().fill(Color.black.opacity(0.8))
            Circle()
                .fill(colour.opacity(0.35))
                .frame(width: size * (0.3 + 0.7 * amount), height: size * (0.3 + 0.7 * amount))
                .blur(radius: size * 0.08)
            Circle()
                .fill(colour)
                .frame(width: size * (0.18 + 0.5 * amount), height: size * (0.18 + 0.5 * amount))
        }
        .frame(width: size, height: size)
        .clipShape(Circle())
        .overlay(Circle().strokeBorder(Color.white.opacity(0.3), lineWidth: 0.5))
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
                    with: .color(.white.opacity(0.18)), lineWidth: 0.5)
            }
            var marker = Path()
            marker.move(to: CGPoint(x: centre.x, y: area.height * 0.03))
            marker.addLine(to: CGPoint(x: centre.x, y: area.height * 0.22))
            context.stroke(marker, with: .color(.white.opacity(0.85)), style: StrokeStyle(lineWidth: 1.5, lineCap: .round))
        }
        .frame(width: size, height: size)
    }
}
