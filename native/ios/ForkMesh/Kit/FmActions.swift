// FmChangeKind, FmChangeBadge, FmTone, FmToneButton, FmBottomBar,
// FmBranchLabel, FmDialogActions, FmNeedsDesktopNote and FmInboxPendingNote.

import SwiftUI

/// Git change status for FmChangeBadge.
enum FmChangeKind {
    case added
    case modified
    case deleted
    case renamed

    var letter: String {
        switch self {
        case .added: return "A"
        case .modified: return "M"
        case .deleted: return "D"
        case .renamed: return "R"
        }
    }

    var semanticLabel: String {
        switch self {
        case .added: return "Added"
        case .modified: return "Modified"
        case .deleted: return "Deleted"
        case .renamed: return "Renamed"
        }
    }
}

/// FmChangeBadge: the square M / A / D / R letter for a changed file. VoiceOver
/// reads the status word, not the bare letter.
struct FmChangeBadge: View {
    @Environment(\.fmPalette) private var fm

    private let kind: FmChangeKind
    private let size: CGFloat

    init(kind: FmChangeKind, size: CGFloat = 24) {
        self.kind = kind
        self.size = size
    }

    private var tone: Color {
        switch kind {
        case .added: return fm.success
        case .modified: return fm.warning
        case .deleted: return fm.danger
        case .renamed: return fm.accent
        }
    }

    var body: some View {
        Text(kind.letter)
            .fmMono(size: size * 0.5, weight: FmType.emphasis, color: tone, height: 1)
            .frame(width: size, height: size)
            .background(tone.opacity(fm.dark ? 0.22 : 0.15))
            .clipShape(FmRoundedShape(radius: FmRadius.sm + 2))
            .accessibilityElement(children: .ignore)
            .accessibilityLabel(Text(kind.semanticLabel))
    }
}

/// Semantic tone for FmToneButton.
enum FmTone {
    case accent
    case success
    case warning
    case danger
    case neutral
}

/// FmToneButton: the compact tinted action ("Pull", "Commit", "Stage all").
///
/// A filled button is a solid block of its tone, so its label is white. Amber
/// is the one exception: it is light enough that white text on it fails
/// contrast, so that tone keeps a dark label. Disabled buttons dim to 45%.
/// `tooltip`, when set, is the accessible name instead of the label.
struct FmToneButton: View {
    @Environment(\.fmPalette) private var fm

    private let label: String
    private let onPressed: (() -> Void)?
    private let icon: String?
    private let tone: FmTone
    private let filled: Bool
    private let tooltip: String?
    private let height: CGFloat

    init(
        label: String,
        icon: String? = nil,
        tone: FmTone = .neutral,
        filled: Bool = false,
        tooltip: String? = nil,
        height: CGFloat = 34,
        onPressed: (() -> Void)?
    ) {
        self.label = label
        self.icon = icon
        self.tone = tone
        self.filled = filled
        self.tooltip = tooltip
        self.height = height
        self.onPressed = onPressed
    }

    private var toneColor: Color {
        switch tone {
        case .accent: return fm.accent
        case .success: return fm.success
        case .warning: return fm.warning
        case .danger: return fm.danger
        case .neutral: return fm.textSecondary
        }
    }

    private var background: Color {
        if filled {
            return toneColor
        }
        if tone == .neutral {
            return fm.bgOverlay
        }
        return toneColor.opacity(fm.dark ? 0.20 : 0.13)
    }

    private var foreground: Color {
        if !filled {
            return toneColor
        }
        return tone == .warning ? FmColors.darkBgBase : Color.white
    }

    private var face: some View {
        HStack(spacing: 6) {
            if let icon = icon {
                FmIcon(icon, size: 16, color: foreground)
            }
            Text(label)
                .fmText(.labelMedium, color: foreground, height: 1)
                .lineLimit(1)
                .truncationMode(.tail)
        }
        .padding(.horizontal, icon == nil ? 12 : 10)
        .frame(height: height)
        .background(background)
        .clipShape(FmRoundedShape(radius: FmRadius.md))
    }

    private func press() {
        guard let onPressed = onPressed else {
            return
        }
        FmHaptics.tap(.forLabel(label, fallback: .click))
        onPressed()
    }

    var body: some View {
        Button(action: press) {
            face
        }
        .buttonStyle(FmPressStyle(highlight: foreground.opacity(0.12), cornerRadius: FmRadius.md))
        .disabled(onPressed == nil)
        .opacity(onPressed == nil ? 0.45 : 1)
        .accessibilityLabel(Text(tooltip ?? label))
    }
}

/// FmBottomBar: the slim source-control footer: a leading label (branch, path)
/// plus trailing actions on a glass capsule, pinned under a list. Place it like
/// FmBottomActionBar.
struct FmBottomBar<Leading: View>: View {
    @Environment(\.fmPalette) private var fm

    private let actions: [AnyView]
    private let height: CGFloat
    private let leading: Leading

    init(actions: [AnyView] = [], height: CGFloat = 50, @ViewBuilder leading: () -> Leading) {
        self.actions = actions
        self.height = height
        self.leading = leading()
    }

    private var row: some View {
        HStack(spacing: FmSpace.x2) {
            leading
                .frame(maxWidth: .infinity, alignment: .leading)
            ForEach(Array(actions.indices), id: \.self) { (position: Int) in
                actions[position]
            }
        }
        .padding(.horizontal, FmSpace.x3)
        .frame(height: height)
    }

    var body: some View {
        row
            .shellGlass(Capsule())
            .padding(.horizontal, FmSpace.x2)
            .padding(.vertical, FmSpace.x1)
    }
}

/// FmBranchLabel: a branch glyph and name in the success tone, like a status
/// bar ref indicator.
struct FmBranchLabel: View {
    @Environment(\.fmPalette) private var fm

    private let name: String
    private let color: Color?
    private let onTap: (() -> Void)?
    private let trailingIcon: String?

    init(name: String, color: Color? = nil, trailingIcon: String? = nil, onTap: (() -> Void)? = nil) {
        self.name = name
        self.color = color
        self.trailingIcon = trailingIcon
        self.onTap = onTap
    }

    private var tone: Color {
        return color ?? fm.success
    }

    private var row: some View {
        HStack(spacing: 0) {
            FmIcon("account_tree_outlined", size: 16, color: tone)
            Text(name)
                .fmText(.labelMedium, color: tone)
                .lineLimit(1)
                .truncationMode(.tail)
                .padding(.leading, 6)
            if let trailingIcon = trailingIcon {
                FmIcon(trailingIcon, size: 18, color: fm.textTertiary)
                    .padding(.leading, 2)
            }
        }
    }

    var body: some View {
        if let onTap = onTap {
            Button(action: {
                FmHaptics.tap()
                onTap()
            }) {
                row.padding(FmSpace.x2)
            }
            .buttonStyle(FmPressStyle(highlight: fm.textPrimary.opacity(0.08), cornerRadius: FmRadius.md))
        } else {
            row
        }
    }
}

/// FmDialogActions: a quiet cancel and an accent (or danger) filled confirm,
/// "Cancel · Create". Confirming gives a commit haptic, or a failure haptic
/// when destructive; `busy` disables both and spins in the confirm button.
struct FmDialogActions: View {
    @Environment(\.fmPalette) private var fm

    private let onCancel: (() -> Void)?
    private let onConfirm: (() -> Void)?
    private let confirmLabel: String
    private let cancelLabel: String
    private let destructive: Bool
    private let busy: Bool

    init(
        onCancel: (() -> Void)?,
        onConfirm: (() -> Void)?,
        confirmLabel: String = "Create",
        cancelLabel: String = "Cancel",
        destructive: Bool = false,
        busy: Bool = false
    ) {
        self.onCancel = onCancel
        self.onConfirm = onConfirm
        self.confirmLabel = confirmLabel
        self.cancelLabel = cancelLabel
        self.destructive = destructive
        self.busy = busy
    }

    private var tone: Color {
        return destructive ? fm.danger : fm.accent
    }

    private var confirmEnabled: Bool {
        return !busy && onConfirm != nil
    }

    private func confirm() {
        guard confirmEnabled, let onConfirm = onConfirm else {
            return
        }
        if destructive {
            FmHaptics.failure(.delete)
        } else {
            FmHaptics.commit(.forLabel(confirmLabel, fallback: .click))
        }
        onConfirm()
    }

    @ViewBuilder private var confirmFace: some View {
        if busy {
            FmCircularProgressIndicator(size: 18, strokeWidth: 2, color: Color.white)
        } else {
            Text(confirmLabel)
                .fmText(.labelLarge, color: confirmEnabled ? Color.white : Color.white.opacity(0.7))
                .lineLimit(1)
        }
    }

    private var confirmButton: some View {
        Button(action: confirm) {
            confirmFace
                .padding(.horizontal, FmSpace.x5)
                .frame(minWidth: 112, minHeight: 44)
                .background(confirmEnabled ? tone : tone.opacity(0.45))
                .clipShape(FmRoundedShape(radius: FmRadius.md))
        }
        .buttonStyle(FmPressStyle(highlight: Color.white.opacity(0.12), cornerRadius: FmRadius.md))
        .disabled(!confirmEnabled)
    }

    var body: some View {
        HStack(spacing: FmSpace.x2) {
            Spacer(minLength: 0)
            FmTextButton(cancelLabel, color: fm.textPrimary, onPressed: busy ? nil : onCancel)
            confirmButton
        }
    }
}

/// FmNeedsDesktopNote: the one honest answer for work a mobile node
/// structurally cannot do.
///
/// A `mobile_node` device key carries `browse,comment,submit_issue,submit_pr,
/// publish_repo,owner_sign` and deliberately never `host_repo` or
/// `mirror_repo`. Running agents, executing Actions, and serving repository
/// bytes all need a host, so a phone-only account must say so plainly rather
/// than show an empty list or spin forever. Callers show it when the account
/// has no desktop device.
struct FmNeedsDesktopNote: View {
    @Environment(\.fmPalette) private var fm

    static let headline: String = "Needs a desktop or server node on this account"

    private let detail: String
    private let icon: String

    /// `detail` is one extra sentence naming what is waiting on that node.
    init(detail: String = "", icon: String = "desktop_access_disabled_outlined") {
        self.detail = detail
        self.icon = icon
    }

    private var explanation: String {
        if detail.isEmpty {
            return "This phone publishes and signs as the owner, but it never hosts or mirrors, so it cannot run this work."
        }
        return detail
    }

    private var copy: some View {
        VStack(alignment: .leading, spacing: FmSpace.x1) {
            Text(verbatim: FmNeedsDesktopNote.headline)
                .fmText(.bodyMedium, weight: FmType.emphasis)
            Text(explanation)
                .fmText(.bodySmall, height: FmType.leadingRelaxed)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
    }

    var body: some View {
        HStack(alignment: .top, spacing: FmSpace.x2) {
            FmIcon(icon, size: 18, color: fm.warning)
            copy
            FmMessageTools("\(FmNeedsDesktopNote.headline)\n\(explanation)")
        }
        .padding(FmSpace.x3)
        .background(fm.bgOverlay)
        .clipShape(FmRoundedShape(radius: FmRadius.md))
        .overlay {
            FmRoundedShape(radius: FmRadius.md).strokeBorder(fm.border, lineWidth: 1)
        }
    }
}

/// FmInboxPendingNote: a signed inbox action waits for the repository owner to
/// apply it on their node; shown under every compose surface.
struct FmInboxPendingNote: View {
    @Environment(\.fmPalette) private var fm

    init() {}

    var body: some View {
        HStack(spacing: FmSpace.x1) {
            FmIcon("verified_user_outlined", size: 14, color: fm.textTertiary)
            Text(verbatim: "Signed inbox action - pending the repo owner applying it.")
                .fmText(.bodySmall)
        }
        .padding(.horizontal, FmSpace.x3)
        .padding(.vertical, FmSpace.x2)
        .background(fm.bgBase)
        .clipShape(FmRoundedShape(radius: FmRadius.lg))
        .overlay {
            FmRoundedShape(radius: FmRadius.lg).strokeBorder(fm.border, lineWidth: 1)
        }
    }
}
