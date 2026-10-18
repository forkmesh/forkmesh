// AvatarWithDot, the initials-or-photo avatar it is built on, and PresenceDot.
// UserStatusBuilder and UserStatusEmoji need the UserStatus model and are left
// to the feature that owns it.

import SwiftUI
import UIKit

enum FmAvatarImages {
    static func image(_ base64: String) -> UIImage? {
        FmImageCache.image(base64: base64)
    }
}

/// A round avatar: the base64 PNG when `avatarPng` decodes, otherwise the
/// first character of `name` in white on its sender colour. The picture is
/// painted over the initial, so one that fails to decode leaves the initial
/// showing instead of an empty disc.
struct FmAvatar: View {
    private let name: String
    private let avatarPng: String
    private let size: CGFloat
    private let fontSize: CGFloat
    private let corner: CGFloat?

    /// `fontSize` defaults to CircleAvatar's titleMedium.
    init(name: String, avatarPng: String = "", size: CGFloat = 40, fontSize: CGFloat = FmType.heading, corner: CGFloat? = nil) {
        self.name = name
        self.avatarPng = avatarPng
        self.size = size
        self.fontSize = fontSize
        self.corner = corner
    }

    private var shape: AnyShape {
        if let corner { return AnyShape(RoundedRectangle(cornerRadius: corner, style: .continuous)) }
        return AnyShape(Circle())
    }

    /// `label.characters.first.toUpperCase()`: the first grapheme, or "?" for
    /// an empty name.
    static func initial(_ name: String) -> String {
        guard let first = name.first else {
            return "?"
        }
        return String(first).uppercased()
    }

    @ViewBuilder private var photo: some View {
        if let image = FmAvatarImages.image(avatarPng) {
            Image(uiImage: image)
                .resizable()
                .scaledToFill()
                .frame(width: size, height: size)
                .clipShape(shape)
        }
    }

    var body: some View {
        Text(FmAvatar.initial(name))
            .font(FmFonts.ui(size: fontSize, weight: FmType.emphasis))
            .foregroundStyle(Color.white)
            .frame(width: size, height: size)
            .background(FmColors.senderColor(name))
            .clipShape(shape)
            .overlay { photo }
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
    private let rounded: Bool

    init(
        state: FmConnectionState, label: String, avatarPng: String = "", statusEmoji: String = "", away: Bool = false,
        diagnosticsTroubled: Bool = false, size: CGFloat = 34, rounded: Bool = false
    ) {
        self.state = state
        self.label = label
        self.avatarPng = avatarPng
        self.statusEmoji = statusEmoji
        self.away = away
        self.diagnosticsTroubled = diagnosticsTroubled
        self.size = size
        self.rounded = rounded
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

    @ViewBuilder private var badge: some View {
        if !statusEmoji.isEmpty {
            Text(verbatim: statusEmoji)
                .font(Font.system(size: size * 0.42))
                .lineLimit(1)
                .fixedSize()
                .shadow(color: Color.black.opacity(0.35), radius: 1, x: 0, y: 0.5)
                .offset(x: -size * 0.3, y: size * 0.22)
        }
    }

    var body: some View {
        if rounded {
            FmAvatar(name: label, avatarPng: avatarPng, size: size, corner: size * 0.28)
                .overlay {
                    RoundedRectangle(cornerRadius: size * 0.28 + 3, style: .continuous)
                        .strokeBorder(dotColor, lineWidth: 2)
                        .padding(-3)
                }
                .overlay(alignment: .bottomLeading) { badge }
                .accessibilityElement(children: .ignore)
                .accessibilityLabel(Text(verbatim: "Connection status: \(statusLabel). Account \(label)"))
        } else {
            HStack(spacing: size * 0.12) {
                emoji
                FmAvatar(name: label, avatarPng: avatarPng, size: size)
                    .overlay(alignment: .bottomTrailing) { dot }
            }
            .accessibilityElement(children: .ignore)
            .accessibilityLabel(Text(verbatim: "Connection status: \(statusLabel). Account \(label)"))
        }
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
