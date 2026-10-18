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
            let _ = deck.shown()
            ZStack {
                Circle().fill(DjTheme.rgb(0x111214))
                if let artwork = deck.artwork {
                    Image(uiImage: artwork)
                        .resizable()
                        .interpolation(.medium)
                        .scaledToFill()
                        .frame(width: size, height: size)
                }
            }
            .frame(width: size, height: size)
            .clipShape(Circle())
            .rotationEffect(.degrees(DjStudio.turnDegrees(deck.spin.turn)))
        }
    }
}
