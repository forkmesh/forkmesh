// `UserStatusBuilder` (repaints the moment a custom status lapses) and `UserStatusEmoji` (the
// status emoji after a member's name, its text on tap). `PresenceDot` is in
// Kit/ConnectionDot.swift.

import FMApi
import FMBase
import SwiftUI

/// Draws `content` with the status as it should be painted now, and again the
/// moment its custom part lapses, so a "🚌 Commuting" set to clear in thirty
/// minutes disappears on time even on a screen nothing else repaints.
struct UserStatusBuilder<Content: View>: View {
    let status: UserStatus
    let content: (UserStatus) -> Content

    @State var tick: Int = 0

    init(status: UserStatus, @ViewBuilder content: @escaping (UserStatus) -> Content) {
        self.status = status
        self.content = content
    }

    private var scheduleKey: String {
        return "\(status.expiresAt)-\(status.emoji)-\(status.text)-\(tick)"
    }

    var body: some View {
        content(status.visibleAt(Date()))
            .task(id: scheduleKey) {
                guard let delay = userStatusRepaintDelay(status, now: Date()) else {
                    return
                }
                try? await Task.sleep(nanoseconds: UInt64(delay * 1_000_000_000) + 1_000_000)
                if !Task.isCancelled {
                    tick += 1
                }
            }
    }
}

/// The status emoji after a member's name. A tap shows the status text and
/// when it clears. Nothing when the member has no unexpired custom status.
struct UserStatusEmoji: View {
    @Environment(\.fmPalette) var fm
    let status: UserStatus
    /// Font size of the emoji; the surrounding text's when nil.
    var size: CGFloat?

    @State var showing: Bool = false

    init(status: UserStatus, size: CGFloat? = nil) {
        self.status = status
        self.size = size
    }

    @ViewBuilder private func glyph(_ visible: UserStatus) -> some View {
        if !visible.emoji.isEmpty {
            if let size = size {
                Text(verbatim: visible.emoji)
                    .font(Font.system(size: size))
                    .lineLimit(1)
            } else {
                Text(verbatim: visible.emoji)
                    .lineLimit(1)
            }
        } else {
            FmIcon("chat_bubble_outline", size: size ?? FmType.meta, color: fm.textTertiary)
        }
    }

    var body: some View {
        UserStatusBuilder(status: status) { (visible: UserStatus) in
            if visible.hasCustom {
                let description: String = visible.describe(Date())
                glyph(visible)
                    .padding(.horizontal, FmSpace.x1)
                    .contentShape(Rectangle())
                    .onTapGesture { showing = true }
                    .popover(isPresented: $showing) {
                        Text(description)
                            .fmText(.bodySmall, color: fm.textPrimary)
                            .padding(FmSpace.x3)
                            .presentationCompactAdaptation(.popover)
                    }
                    .accessibilityLabel(Text(verbatim: "Status: \(description)"))
            }
        }
    }
}
