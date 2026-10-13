// The small views the collaboration screens share: cards, chips, badges, the
// loading and empty states, copyable values, the text prompt, the inline notice,
// copying text, ComposeIdentityBar and a wrap layout. Services come from
// FMBase's registry; the rules from FMCode/Collab.

import FMAccount
import FMApi
import FMBase
import FMCode
import FMCrypto
import FMDiff
import Observation
import SwiftUI
import UIKit

// MARK: - Services

/// The services the collaboration screens read.
@MainActor
enum CollabEnv {
    static var api: ApiService { Services.require(ApiService.self) }

    /// Nil on a device without a signing identity; writes then fail with a
    /// readable inline notice.
    static var inbox: InboxService? { Services.get(InboxService.self) }

    static var auth: AuthService? { Services.get(AuthService.self) }

    static var identity: DeviceIdentity? { Services.get(DeviceIdentity.self) }

    /// The signed-in session. Reading it inside a view body
    /// subscribes the view to session changes.
    static func account() -> CollabAccount {
        CollabAccount(session: auth?.session)
    }

    static var isGuest: Bool { AccountFeature.isGuest }

    static func requireAccount(_ action: String) async -> Bool {
        await AccountFeature.requireAccount(action: action)
    }

    static func writer(_ repo: Repository) -> CollabWriter {
        CollabWriter(repo: repo, inbox: inbox)
    }
}

/// The inline notice of the collaboration screens: one at a time, shown by
/// whichever collaboration screen is on top. A composer that closes itself
/// after submitting (the new issue page, the new pull and discussion sheets)
/// posts here, so the sentence lands on the screen below.
@MainActor
@Observable
final class CollabMessenger {
    static let shared = CollabMessenger()

    var notice: FmNotice?

    func show(_ message: String) {
        notice = FmNotice(message)
    }

    func show(_ notice: CollabNotice?) {
        guard let notice = notice else {
            return
        }
        self.notice = FmNotice(notice.message, isError: notice.isError)
    }
}

/// The collaboration notice, in the flow of this screen.
struct CollabNoticeLine: View {
    var horizontalPadding: CGFloat = 0

    var body: some View {
        let messenger: CollabMessenger = CollabMessenger.shared
        if messenger.notice != nil {
            FmNoticeLine(Bindable(messenger).notice)
                .padding(.horizontal, horizontalPadding)
        }
    }
}

/// The clipboard, then "$label copied".
@MainActor
func collabCopy(_ value: String, label: String) {
    UIPasteboard.general.string = value
    FmHaptics.tap()
    CollabMessenger.shared.show(CollabCopy.copied(label))
}

// MARK: - Palette

extension FmPalette {
    func collabTone(_ tone: CollabTone) -> Color {
        switch tone {
        case .success: return success
        case .danger: return danger
        case .accent: return accent
        case .accentEdge: return accentEdge
        case .secondary: return textSecondary
        case .tertiary: return textTertiary
        }
    }

    /// The highlighter's colour for a token kind (`codeBaseColor` for plain).
    func codeColor(_ kind: TokenKind) -> Color {
        switch kind {
        case .plain: return dark ? Color(fmARGB: CodeHighlighter.baseColorDarkARGB) : textPrimary
        case .keyword, .heading: return codeKeyword
        case .type: return codeType
        case .string: return codeString
        case .comment: return codeComment
        case .number: return codeNumber
        case .function: return codeFunction
        }
    }
}

extension PatchChangeKind {
    var fmKind: FmChangeKind {
        switch self {
        case .added: return .added
        case .modified: return .modified
        case .deleted: return .deleted
        case .renamed: return .renamed
        }
    }
}

// MARK: - Layout

/// Children left to right, onto a new run when the line is full.
struct CollabWrap: Layout {
    var spacing: CGFloat
    var runSpacing: CGFloat

    init(spacing: CGFloat = FmSpace.x2, runSpacing: CGFloat = FmSpace.x2) {
        self.spacing = spacing
        self.runSpacing = runSpacing
    }

    private func measure(_ subview: LayoutSubview, maxWidth: CGFloat) -> CGSize {
        let ideal: CGSize = subview.sizeThatFits(ProposedViewSize.unspecified)
        if ideal.width <= maxWidth {
            return ideal
        }
        return subview.sizeThatFits(ProposedViewSize(width: maxWidth, height: nil))
    }

    func sizeThatFits(proposal: ProposedViewSize, subviews: Subviews, cache: inout ()) -> CGSize {
        let maxWidth: CGFloat = proposal.width ?? CGFloat.infinity
        var x: CGFloat = 0
        var y: CGFloat = 0
        var runHeight: CGFloat = 0
        var widest: CGFloat = 0
        for subview in subviews {
            let size: CGSize = measure(subview, maxWidth: maxWidth)
            if x > 0 && x + size.width > maxWidth {
                y += runHeight + runSpacing
                x = 0
                runHeight = 0
            }
            x += size.width + spacing
            runHeight = max(runHeight, size.height)
            widest = max(widest, x - spacing)
        }
        let width: CGFloat = maxWidth.isFinite ? maxWidth : widest
        return CGSize(width: width, height: subviews.isEmpty ? 0 : y + runHeight)
    }

    func placeSubviews(in bounds: CGRect, proposal: ProposedViewSize, subviews: Subviews, cache: inout ()) {
        let maxWidth: CGFloat = bounds.width
        var x: CGFloat = 0
        var y: CGFloat = 0
        var runHeight: CGFloat = 0
        for subview in subviews {
            let size: CGSize = measure(subview, maxWidth: maxWidth)
            if x > 0 && x + size.width > maxWidth {
                y += runHeight + runSpacing
                x = 0
                runHeight = 0
            }
            subview.place(
                at: CGPoint(x: bounds.minX + x, y: bounds.minY + y),
                anchor: UnitPoint.topLeading,
                proposal: ProposedViewSize(width: size.width, height: size.height))
            x += size.width + spacing
            runHeight = max(runHeight, size.height)
        }
    }
}

// MARK: - Cards

/// A titled card.
struct CollabInfoCard<Content: View>: View {
    private let title: String
    private let content: Content

    init(title: String, @ViewBuilder content: () -> Content) {
        self.title = title
        self.content = content()
    }

    var body: some View {
        FmCard(padding: EdgeInsets.fmAll(FmSpace.x4), radius: FmRadius.lg) {
            VStack(alignment: .leading, spacing: 0) {
                Text(title)
                    .fmText(.titleMedium)
                    .padding(.bottom, FmSpace.x3)
                content
            }
            .frame(maxWidth: .infinity, alignment: .leading)
        }
    }
}

struct CollabChipView: View {
    @Environment(\.fmPalette) private var fm

    let chip: CollabChip

    init(chip: CollabChip) {
        self.chip = chip
    }

    var body: some View {
        HStack(spacing: FmSpace.x1) {
            FmIcon(chip.icon, size: 14, color: fm.textTertiary)
            Text(chip.label)
                .fmText(.bodySmall, color: fm.textSecondary, weight: FmType.emphasis)
                .lineLimit(1)
        }
        .padding(.horizontal, FmSpace.x3)
        .padding(.vertical, FmSpace.x2)
        .background(fm.bgBase)
        .clipShape(Capsule())
    }
}

/// A wrapping row of chips.
struct CollabChips: View {
    let chips: [CollabChip]

    var body: some View {
        CollabWrap(spacing: FmSpace.x2, runSpacing: FmSpace.x2) {
            ForEach(chips.indices, id: \.self) { (index: Int) in
                CollabChipView(chip: chips[index])
            }
        }
    }
}

/// A status badge coloured by its words.
struct CollabBadge: View {
    @Environment(\.fmPalette) private var fm

    let text: String

    init(text: String) {
        self.text = text
    }

    var body: some View {
        FmStatusBadge(label: text, color: fm.collabTone(collabBadgeTone(text)))
    }
}

/// A list row with an icon tile, a two-line title, a subtitle
/// and a badge. Wrap it in a NavigationLink to make it open something.
struct CollabRow: View {
    @Environment(\.fmPalette) private var fm

    let icon: String
    let tone: CollabTone
    let title: String
    let subtitle: String
    let badge: String
    let showChevron: Bool

    init(icon: String, tone: CollabTone, title: String, subtitle: String, badge: String, showChevron: Bool = true) {
        self.icon = icon
        self.tone = tone
        self.title = title
        self.subtitle = subtitle
        self.badge = badge
        self.showChevron = showChevron
    }

    private var titleView: AnyView {
        AnyView(
            Text(title)
                .fmText(.titleSmall, height: 1.2)
                .lineLimit(2)
                .truncationMode(.tail)
                .multilineTextAlignment(.leading))
    }

    private var trailing: AnyView? {
        if badge.isEmpty {
            return nil
        }
        return AnyView(CollabBadge(text: badge))
    }

    var body: some View {
        FmListRow(
            title: title,
            titleView: titleView,
            subtitle: subtitle,
            leading: AnyView(FmIconTile(icon: icon, tint: fm.collabTone(tone))),
            trailing: trailing,
            showChevron: showChevron)
            .contentShape(Rectangle())
    }
}

struct CollabLoadingCard: View {
    var label: String = CollabCopy.loading

    var body: some View {
        CollabInfoCard(title: label) {
            FmLinearProgressIndicator()
        }
    }
}

struct CollabEmptyCard: View {
    @Environment(\.fmPalette) private var fm

    let message: String

    init(message: String) {
        self.message = message
    }

    var body: some View {
        CollabInfoCard(title: CollabCopy.emptyTitle) {
            Text(message)
                .fmText(.bodyMedium, color: fm.textSecondary)
                .textSelection(.enabled)
                .frame(maxWidth: .infinity, alignment: .leading)
                .fmMessageTools(message)
        }
    }
}

/// A padded scroll of one card: the loading, error and empty states of the
/// tabs and detail screens.
struct CollabStateScroll<Content: View>: View {
    private let content: Content

    init(@ViewBuilder content: () -> Content) {
        self.content = content()
    }

    var body: some View {
        ScrollView {
            content
                .padding(FmSpace.x4)
        }
    }
}

struct CollabCopyableValue: View {
    @Environment(\.fmPalette) private var fm

    let row: BountyCopyRow

    init(row: BountyCopyRow) {
        self.row = row
    }

    var body: some View {
        VStack(alignment: .leading, spacing: FmSpace.x1) {
            Text(row.label)
                .fmText(.bodySmall, color: fm.textSecondary, weight: FmType.emphasis)
            HStack(alignment: .top, spacing: FmSpace.x2) {
                Text(row.value)
                    .fmText(.bodyMedium, color: fm.textPrimary, height: 1.35)
                    .textSelection(.enabled)
                    .frame(maxWidth: .infinity, alignment: .leading)
                FmIconButton(label: "Copy \(row.label)", onPressed: { collabCopy(row.value, label: row.copyLabel) }) {
                    FmIcon("copy_all_outlined", size: 18, color: fm.textSecondary)
                }
                .accessibilityIdentifier(row.key)
            }
        }
    }
}

/// ComposeIdentityBar: "Posting as <account>" above a compose surface;
/// nothing without a session.
struct CollabIdentityBar: View {
    @Environment(\.fmPalette) private var fm

    let verb: String

    init(verb: String) {
        self.verb = verb
    }

    private var session: AuthSession? { CollabEnv.auth?.session }

    var body: some View {
        if let session = session {
            let name: String = session.nodeName.isEmpty ? session.email : session.nodeName
            HStack(spacing: FmSpace.x2) {
                FmAvatar(name: name, avatarPng: session.avatarPng, size: 28, fontSize: FmType.meta)
                (Text(verb.isEmpty ? "as " : "\(verb) as ").foregroundColor(fm.textSecondary)
                    + Text(name).fontWeight(FmType.emphasis).foregroundColor(fm.textPrimary))
                    .fmText(.bodyMedium)
                    .lineLimit(1)
                    .truncationMode(.tail)
            }
            .padding(.vertical, 6)
        }
    }
}

/// One entry of a signed timeline (issue, pull request, discussion or mention).
struct CollabTimelineTile: View {
    @Environment(\.fmPalette) private var fm

    let icon: String
    let tone: CollabTone
    let title: String
    let author: String
    let text: String
    let repo: RepoRef?

    init(icon: String, tone: CollabTone, title: String, author: String, text: String, repo: RepoRef? = nil) {
        self.icon = icon
        self.tone = tone
        self.title = title
        self.author = author
        self.text = text
        self.repo = repo
    }

    var body: some View {
        HStack(alignment: .top, spacing: 10) {
            FmIcon(icon, size: 18, color: fm.collabTone(tone))
            VStack(alignment: .leading, spacing: 0) {
                Text(title)
                    .fmText(.bodyMedium, color: fm.textPrimary, weight: FmType.emphasis)
                if !author.isEmpty {
                    Text(author)
                        .fmText(.bodySmall, color: fm.textSecondary)
                        .padding(.top, 2)
                }
                if !text.dartTrimmed.isEmpty {
                    LinkedText(text.dartTrimmed, repo: repo)
                        .fmText(.bodyMedium, color: fm.textPrimary)
                        .textSelection(.enabled)
                        .padding(.top, FmSpace.x2)
                }
            }
            .frame(maxWidth: .infinity, alignment: .leading)
        }
        .padding(12)
        .background(fm.bgBase)
        .clipShape(FmRoundedShape(radius: FmRadius.lg))
    }
}

/// A body of text under a card title (`SelectableText(..., height: 1.45)`).
struct CollabBodyText: View {
    @Environment(\.fmPalette) private var fm

    let text: String
    let repo: RepoRef?

    init(text: String, repo: RepoRef? = nil) {
        self.text = text
        self.repo = repo
    }

    var body: some View {
        LinkedText(text, repo: repo)
            .fmText(.bodyMedium, color: fm.textPrimary, height: 1.45)
            .textSelection(.enabled)
            .frame(maxWidth: .infinity, alignment: .leading)
    }
}

// MARK: - Prompt

/// A titled dialog with one text field.
struct CollabPromptDialog: View {
    @Binding var text: String

    let title: String
    let multiline: Bool
    let onCancel: () -> Void
    let onSubmit: () -> Void

    var body: some View {
        FmDialog(
            title: title,
            content: {
                VStack(alignment: .leading, spacing: FmSpace.x2) {
                    CollabIdentityBar(verb: "Commenting")
                    FmTextField(text: $text, multiline: multiline, autofocus: true)
                }
            },
            actions: {
                FmDialogActions(onCancel: onCancel, onConfirm: onSubmit, confirmLabel: "Submit")
            })
    }
}

/// A row of the action sheets, with a leading icon.
struct CollabSheetRow: View {
    @Environment(\.fmPalette) private var fm

    let icon: String
    let label: String
    let tone: CollabTone?
    let onTap: () -> Void

    init(icon: String, label: String, tone: CollabTone? = nil, onTap: @escaping () -> Void) {
        self.icon = icon
        self.label = label
        self.tone = tone
        self.onTap = onTap
    }

    var body: some View {
        Button(action: {
            FmHaptics.tap()
            onTap()
        }) {
            HStack(spacing: FmSpace.x4) {
                FmIcon(icon, size: 24, color: tone.map { fm.collabTone($0) } ?? fm.textSecondary)
                Text(label)
                    .fmText(.bodyLarge, color: fm.textPrimary)
                Spacer(minLength: 0)
            }
            .padding(.horizontal, FmSpace.x4)
            .frame(minHeight: 56)
            .contentShape(Rectangle())
        }
        .buttonStyle(FmPressStyle(highlight: fm.textPrimary.opacity(0.08)))
    }
}

/// The header row of the action sheets.
struct CollabSheetHeader: View {
    @Environment(\.fmPalette) private var fm

    let title: String
    let subtitle: String

    init(title: String, subtitle: String) {
        self.title = title
        self.subtitle = subtitle
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 2) {
            Text(title)
                .fmText(.bodyLarge, color: fm.textPrimary, weight: FmType.emphasis)
            if !subtitle.isEmpty {
                Text(subtitle)
                    .fmText(.bodyMedium, color: fm.textSecondary)
                    .lineLimit(4)
                    .truncationMode(.tail)
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(.horizontal, FmSpace.x4)
        .padding(.top, FmSpace.x5)
        .padding(.bottom, FmSpace.x2)
    }
}
