// AvatarWithDot, the initials-or-photo avatar it is built on, and PresenceDot.
// UserStatusBuilder and UserStatusEmoji need the UserStatus model and are left
// to the feature that owns it.

import SwiftUI
import UIKit

/// A round avatar: the base64 PNG when `avatarPng` decodes, otherwise the
/// first character of `name` in white on its sender colour. The picture is
/// painted over the initial, so one that fails to decode leaves the initial
/// showing instead of an empty disc.
struct FmAvatar: View {
    @State private var image: UIImage? = nil

    private let name: String
    private let avatarPng: String
    private let size: CGFloat
    private let fontSize: CGFloat

    /// `fontSize` defaults to CircleAvatar's titleMedium.
    init(name: String, avatarPng: String = "", size: CGFloat = 40, fontSize: CGFloat = FmType.heading) {
        self.name = name
        self.avatarPng = avatarPng
        self.size = size
        self.fontSize = fontSize
    }

    /// `label.characters.first.toUpperCase()`: the first grapheme, or "?" for
    /// an empty name.
    static func initial(_ name: String) -> String {
        guard let first = name.first else {
            return "?"
        }
        return String(first).uppercased()
    }

    private static func decode(_ base64: String) -> UIImage? {
        if base64.isEmpty {
            return nil
        }
        guard let data = Data(base64Encoded: base64, options: .ignoreUnknownCharacters) else {
            return nil
        }
        return UIImage(data: data)
    }

    @ViewBuilder private var photo: some View {
        if let image = image {
            Image(uiImage: image)
                .resizable()
                .scaledToFill()
                .frame(width: size, height: size)
                .clipShape(Circle())
        }
    }

    var body: some View {
        Text(FmAvatar.initial(name))
            .font(FmFonts.ui(size: fontSize, weight: FmType.emphasis))
            .foregroundStyle(Color.white)
            .frame(width: size, height: size)
            .background(FmColors.senderColor(name))
            .clipShape(Circle())
            .overlay { photo }
            // Decoded once per picture: decoding on every rebuild of the top
            // bar would make the picture flicker.
            .task(id: avatarPng) {
                image = FmAvatar.decode(avatarPng)
            }
    }
}

/// AvatarWithDot: the account's avatar with a presence dot, mirroring the Qt
/// client's top-right avatar: green online, amber connecting or away, grey
/// offline. The dot uses the light palette in both themes.
struct AvatarWithDot: View {
    private let state: FmConnectionState
    private let label: String
    private let avatarPng: String
    private let statusEmoji: String
    private let away: Bool
    private let diagnosticsTroubled: Bool
    private let size: CGFloat

    init(
        state: FmConnectionState, label: String, avatarPng: String = "", statusEmoji: String = "", away: Bool = false,
        diagnosticsTroubled: Bool = false, size: CGFloat = 34
    ) {
        self.state = state
        self.label = label
        self.avatarPng = avatarPng
        self.statusEmoji = statusEmoji
        self.away = away
        self.diagnosticsTroubled = diagnosticsTroubled
        self.size = size
    }

    private var dotColor: Color {
        switch state {
        case .connected: return diagnosticsTroubled ? FmColors.danger : (away ? FmColors.warning : FmColors.success)
        case .connecting: return FmColors.warning
        case .offline: return FmColors.offline
        }
    }

    private var statusLabel: String {
        switch state {
        case .connected: return diagnosticsTroubled ? "needs attention" : (away ? "away" : "online")
        case .connecting: return "connecting"
        case .offline: return "offline"
        }
    }

    private var dot: some View {
        Circle()
            .fill(dotColor)
            .frame(width: 12, height: 12)
            .offset(x: 1, y: 1)
    }

    @ViewBuilder private var emoji: some View {
        if !statusEmoji.isEmpty {
            Text(verbatim: statusEmoji)
                .font(Font.system(size: size * 0.5))
                .lineLimit(1)
                .fixedSize()
        }
    }

    var body: some View {
        HStack(spacing: size * 0.12) {
            emoji
            FmAvatar(name: label, avatarPng: avatarPng, size: size)
                .overlay(alignment: .bottomTrailing) { dot }
        }
        .accessibilityElement(children: .ignore)
        .accessibilityLabel(Text(verbatim: "Connection status: \(statusLabel). Account \(label)"))
    }
}

/// PresenceDot: the online dot beside a member: filled green online, a hollow
/// amber ring while they have marked themselves away, grey offline.
struct PresenceDot: View {
    @Environment(\.fmPalette) private var fm

    private let online: Bool
    private let away: Bool
    private let size: CGFloat

    init(online: Bool, away: Bool = false, size: CGFloat = 10) {
        self.online = online
        self.away = away
        self.size = size
    }

    private var showAway: Bool {
        return online && away
    }

    private var label: String {
        if showAway {
            return "Away"
        }
        return online ? "Online" : "Offline"
    }

    private var fillColor: Color {
        if showAway {
            return Color.clear
        }
        return online ? FmColors.success : FmColors.offline
    }

    var body: some View {
        Circle()
            .fill(fillColor)
            .overlay {
                Circle().strokeBorder(showAway ? fm.warning : Color.clear, lineWidth: 2)
            }
            .frame(width: size, height: size)
            .accessibilityElement(children: .ignore)
            .accessibilityLabel(Text(label))
    }
}
