// The Code area's wiring and the small shared pieces every remote-repository
// tab uses: cards, chips, badges, the loading, error and empty states,
// key-value rows and copying text.

import FMAccount
import FMApi
import FMBase
import FMCode
import FMCrypto
import SwiftUI
import UIKit

/// The Code area's services. The remote-repository screens need nothing
/// beyond ApiService, AuthService and the device key, which AppServices
/// registers before this runs.
@MainActor
enum CodeFeature {
    static func register() {}
}

/// Where the repository screens find their dependencies.
@MainActor
enum RepoServices {
    static var client: RepoClient {
        return Services.require(ApiService.self)
    }

    static var overviewClient: RepoOverviewClient? {
        return Services.get(ApiService.self)
    }

    static var session: AuthSession? {
        return Services.get(AuthService.self)?.session
    }

    static var signer: RepoSigner {
        let auth: AuthService? = Services.get(AuthService.self)
        return RepoSigner(
            session: { auth?.session },
            identity: Services.get(DeviceIdentity.self),
            clock: SystemClock())
    }
}

/// Puts `value` on the pasteboard and answers the inline notice.
@MainActor
func repoCopy(_ label: String, _ value: String) -> FmNotice {
    UIPasteboard.general.string = value
    return FmNotice("\(label) copied")
}

extension FmPalette {
    /// A core colour name as FmTheme.
    func repoTone(_ tone: RepoBadgeTone) -> Color {
        switch tone {
        case .success: return success
        case .danger: return danger
        case .warning: return warning
        case .accent: return accent
        case .secondary: return textSecondary
        case .tertiary: return textTertiary
        }
    }
}

/// A titled card.
struct RepoInfoCard<Content: View>: View {
    @Environment(\.fmPalette) private var fm

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
                    .fmText(.titleMedium, color: fm.textPrimary)
                    .padding(.bottom, FmSpace.x3)
                content
            }
        }
    }
}

/// Icon and label on a pill.
struct RepoChip: View {
    @Environment(\.fmPalette) private var fm

    private let icon: String
    private let label: String

    init(icon: String, label: String) {
        self.icon = icon
        self.label = label
    }

    var body: some View {
        HStack(spacing: FmSpace.x1) {
            FmIcon(icon, size: 14, color: fm.textTertiary)
            Text(label)
                .fmText(.bodySmall, color: fm.textSecondary, weight: FmType.emphasis)
                .lineLimit(1)
        }
        .padding(.horizontal, FmSpace.x3)
        .padding(.vertical, FmSpace.x2)
        .background(fm.bgBase)
        .clipShape(Capsule())
    }
}

/// A Wrap of chips: rows that break onto the next line when full.
struct RepoWrap: Layout {
    var spacing: CGFloat = 8
    var runSpacing: CGFloat = 8

    func sizeThatFits(proposal: ProposedViewSize, subviews: Subviews, cache: inout ()) -> CGSize {
        let maxWidth: CGFloat = proposal.width ?? CGFloat.infinity
        var x: CGFloat = 0
        var y: CGFloat = 0
        var rowHeight: CGFloat = 0
        var widest: CGFloat = 0
        for subview in subviews {
            let size: CGSize = subview.sizeThatFits(ProposedViewSize(width: maxWidth, height: nil))
            if x > 0 && x + size.width > maxWidth {
                y += rowHeight + runSpacing
                x = 0
                rowHeight = 0
            }
            x += size.width + spacing
            widest = max(widest, x - spacing)
            rowHeight = max(rowHeight, size.height)
        }
        return CGSize(width: min(widest, maxWidth), height: y + rowHeight)
    }

    func placeSubviews(in bounds: CGRect, proposal: ProposedViewSize, subviews: Subviews, cache: inout ()) {
        var x: CGFloat = bounds.minX
        var y: CGFloat = bounds.minY
        var rowHeight: CGFloat = 0
        for subview in subviews {
            let size: CGSize = subview.sizeThatFits(ProposedViewSize(width: bounds.width, height: nil))
            if x > bounds.minX && x + size.width > bounds.maxX {
                y += rowHeight + runSpacing
                x = bounds.minX
                rowHeight = 0
            }
            subview.place(at: CGPoint(x: x, y: y), proposal: ProposedViewSize(width: size.width, height: size.height))
            x += size.width + spacing
            rowHeight = max(rowHeight, size.height)
        }
    }
}

/// One chip of a RepoChipWrap.
struct RepoChipItem: Identifiable {
    let id: Int
    let icon: String
    let label: String
}

/// `Wrap(spacing: 8, runSpacing: 8, children: [_Chip...])`.
struct RepoChipWrap: View {
    private let items: [RepoChipItem]

    init(_ chips: [(icon: String, label: String)]) {
        var items: [RepoChipItem] = []
        for (index, chip) in chips.enumerated() {
            items.append(RepoChipItem(id: index, icon: chip.icon, label: chip.label))
        }
        self.items = items
    }

    var body: some View {
        RepoWrap(spacing: 8, runSpacing: 8) {
            ForEach(items) { (item: RepoChipItem) in
                RepoChip(icon: item.icon, label: item.label)
            }
        }
    }
}

/// A status badge coloured from its text.
struct RepoSmallBadge: View {
    @Environment(\.fmPalette) private var fm

    private let text: String

    init(_ text: String) {
        self.text = text
    }

    var body: some View {
        FmStatusBadge(label: text, color: fm.repoTone(RepoDisplay.badgeTone(text)))
    }
}

/// The explorer-style row of the mirror, action, agent and
/// worktree lists.
struct RepoMobileCard: View {
    @Environment(\.fmPalette) private var fm

    private let icon: String
    private let iconColor: Color
    private let title: String
    private let subtitle: String
    private let badge: String
    private let chips: [String]
    private let trailing: AnyView?
    private let onTap: (() -> Void)?

    init(
        icon: String,
        iconColor: Color,
        title: String,
        subtitle: String,
        badge: String = "",
        chips: [String] = [],
        trailing: AnyView? = nil,
        onTap: (() -> Void)? = nil
    ) {
        self.icon = icon
        self.iconColor = iconColor
        self.title = title
        self.subtitle = subtitle
        self.badge = badge
        self.chips = chips
        self.trailing = trailing
        self.onTap = onTap
    }

    private var titleView: AnyView {
        return AnyView(
            Text(title)
                .fmText(.titleSmall, color: fm.textPrimary, height: 1.2)
                .lineLimit(2)
                .truncationMode(.tail)
        )
    }

    private var chipsView: some View {
        VStack(alignment: .leading, spacing: 0) {
            if !subtitle.isEmpty {
                Text(subtitle)
                    .fmText(.bodySmall, color: fm.textTertiary, weight: FmType.emphasis, height: 1.2)
                    .lineLimit(1)
                    .truncationMode(.tail)
            }
            RepoWrap(spacing: 6, runSpacing: 6) {
                ForEach(Array(chips.enumerated()), id: \.offset) { (entry: (offset: Int, element: String)) in
                    RepoSmallBadge(entry.element)
                }
            }
            .padding(.top, FmSpace.x1)
        }
    }

    private var trailingView: AnyView? {
        if let trailing = trailing {
            return trailing
        }
        if badge.isEmpty {
            return nil
        }
        return AnyView(RepoSmallBadge(badge))
    }

    var body: some View {
        FmListRow(
            title: title,
            titleView: titleView,
            subtitle: subtitle,
            subtitleView: chips.isEmpty ? nil : AnyView(chipsView),
            leading: AnyView(FmIconTile(icon: icon, tint: iconColor)),
            trailing: trailingView,
            onTap: onTap,
            showChevron: onTap != nil
        )
    }
}

struct RepoLoadingCard: View {
    private let label: String

    init(label: String) {
        self.label = label
    }

    var body: some View {
        RepoInfoCard(title: label) {
            FmLinearProgressIndicator()
        }
    }
}

struct RepoErrorCard: View {
    @Environment(\.fmPalette) private var fm

    private let message: String
    private let onRetry: () -> Void

    init(message: String, onRetry: @escaping () -> Void) {
        self.message = message
        self.onRetry = onRetry
    }

    var body: some View {
        RepoInfoCard(title: "Something went wrong") {
            VStack(alignment: .leading, spacing: 12) {
                Text(message)
                    .fmText(.bodyMedium, color: fm.textSecondary)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .fmMessageTools(message)
                FmOutlinedButton("Retry", onPressed: onRetry)
            }
        }
    }
}

struct RepoEmptyCard: View {
    @Environment(\.fmPalette) private var fm

    private let message: String

    init(message: String) {
        self.message = message
    }

    var body: some View {
        RepoInfoCard(title: "Nothing here yet") {
            Text(message)
                .fmText(.bodyMedium, color: fm.textSecondary)
                .frame(maxWidth: .infinity, alignment: .leading)
                .fmMessageTools(message)
        }
    }
}

/// The node that served a read.
struct RepoSourceChip: View {
    @Environment(\.fmPalette) private var fm

    private let source: String

    init(source: String) {
        self.source = source
    }

    var body: some View {
        if let label = RepoDisplay.sourceLabel(source) {
            HStack(spacing: FmSpace.x1) {
                FmIcon("dns_outlined", size: 14, color: fm.textTertiary)
                Text(label)
                    .fmText(.bodySmall, color: fm.textSecondary, weight: FmType.emphasis)
                    .lineLimit(1)
                    .truncationMode(.tail)
            }
            .padding(.horizontal, FmSpace.x3)
            .padding(.vertical, FmSpace.x2)
            .background(fm.bgBase)
            .clipShape(Capsule())
            .overlay {
                Capsule().strokeBorder(fm.border, lineWidth: 1)
            }
            .frame(maxWidth: 220, alignment: .leading)
            .fixedSize(horizontal: false, vertical: true)
        }
    }
}

/// Rows with explorer dividers between them.
struct RepoDividedRows<Item, Row: View>: View {
    private let items: [Item]
    private let row: (Item) -> Row

    init(_ items: [Item], @ViewBuilder row: @escaping (Item) -> Row) {
        self.items = items
        self.row = row
    }

    var body: some View {
        VStack(spacing: 0) {
            ForEach(Array(items.indices), id: \.self) { (index: Int) in
                row(items[index])
                if index != items.count - 1 {
                    FmRowDivider()
                }
            }
        }
    }
}

/// A refresh icon button of the tab headers.
struct RepoRefreshButton: View {
    private let label: String
    private let action: () -> Void

    init(label: String, action: @escaping () -> Void) {
        self.label = label
        self.action = action
    }

    var body: some View {
        FmIconButton(icon: "refresh", label: label, size: 18, onPressed: action)
    }
}
