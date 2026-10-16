import FMApi
import FMPackages
import SwiftUI
import UIKit

struct DjNowPlayingChip: View {
    private let hub = DjStudioHub.shared

    var body: some View {
        if let model = hub.model, !model.onScreen, let deck = model.audibleDeck {
            Button { SurfaceRouter.open(.videoMixDjStudio) } label: {
                DjMiniRecord(deck: deck, size: 34)
                    .padding(5)
                    .shellGlass(Circle())
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
        TimelineView(.animation(minimumInterval: 1.0 / 30, paused: !deck.playing)) { _ in
            let time = deck.position()
            ZStack {
                ZStack {
                    Circle().fill(DjTheme.rgb(0x111214))
                    if let artwork = deck.artwork {
                        Image(uiImage: artwork)
                            .resizable()
                            .interpolation(.low)
                            .frame(width: size * 0.92, height: size * 0.92)
                            .clipShape(Circle())
                            .opacity(0.8)
                    }
                    DjMiniGrooves(size: size).equatable()
                }
                .rotationEffect(.degrees(DjStudio.turnDegrees(time)))
                Canvas { context, area in
                    DjWaveDrawing.mini(context, area, bars: DjStudio.miniBars(deck.waveform, time: time, duration: deck.duration))
                }
                .frame(width: size * 0.52, height: size * 0.52)
                .background(Circle().fill(Color.black.opacity(0.8)))
                .clipShape(Circle())
                .overlay(Circle().strokeBorder(Color.white.opacity(0.3), lineWidth: 0.5))
            }
            .frame(width: size, height: size)
        }
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
