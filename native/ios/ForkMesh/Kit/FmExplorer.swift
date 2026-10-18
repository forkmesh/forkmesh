// The explorer vocabulary: FmIconTile, FmListRow, FmRowDivider, FmCountPill,
// FmSegmentTab, FmSegmentTabBar and FmGroupHeader. The action widgets of the
// same section are in FmActions.swift.

import SwiftUI

/// FmIconTile: the rounded-square leading tile of explorer rows.
///
/// With a `tint` the tile is filled with a translucent wash of that colour and
/// the glyph is drawn in the tint (folders, branches, open issues). Without a
/// tint it falls back to the neutral overlay surface (plain files).
struct FmIconTile: View {
    @Environment(\.fmPalette) private var fm

    private let icon: String
    private let tint: Color?
    private let size: CGFloat
    private let iconSize: CGFloat
    private let radius: CGFloat

    init(icon: String, tint: Color? = nil, size: CGFloat = 38, iconSize: CGFloat = 19, radius: CGFloat = FmRadius.md) {
        self.icon = icon
        self.tint = tint
        self.size = size
        self.iconSize = iconSize
        self.radius = radius
    }

    private var background: Color {
        guard let tint = tint else {
            return fm.bgOverlay
        }
        return tint.opacity(fm.dark ? 0.20 : 0.13)
    }

    var body: some View {
        FmIcon(icon, size: iconSize, color: tint ?? fm.textSecondary)
            .frame(width: size, height: size)
            .background(background)
            .clipShape(FmRoundedShape(radius: radius))
    }
}

/// FmListRow: leading tile, bold title, muted meta line and a trailing
/// chevron. Rows sit directly on the list surface and are separated with
/// FmRowDivider rather than wrapped in cards. Taps give a light haptic, long
/// presses a commit haptic, and the press highlight is the accent wash.
///
/// `titleView` and `subtitleView` replace the text lines; `title` stays the
/// accessible fallback.
struct FmListRow: View {
    @Environment(\.fmPalette) private var fm
    @State private var pressing: Bool = false

    private let title: String
    private let titleView: AnyView?
    private let subtitle: String?
    private let subtitleView: AnyView?
    private let leading: AnyView?
    private let trailing: AnyView?
    private let onTap: (() -> Void)?
    private let onLongPress: (() -> Void)?
    private let showChevron: Bool
    private let titleColor: Color?
    private let minHeight: CGFloat
    private let padding: EdgeInsets

    init(
        title: String,
        titleView: AnyView? = nil,
        subtitle: String? = nil,
        subtitleView: AnyView? = nil,
        leading: AnyView? = nil,
        trailing: AnyView? = nil,
        onTap: (() -> Void)? = nil,
        onLongPress: (() -> Void)? = nil,
        showChevron: Bool = true,
        titleColor: Color? = nil,
        minHeight: CGFloat = 60,
        padding: EdgeInsets? = nil
    ) {
        self.title = title
        self.titleView = titleView
        self.subtitle = subtitle
        self.subtitleView = subtitleView
        self.leading = leading
        self.trailing = trailing
        self.onTap = onTap
        self.onLongPress = onLongPress
        self.showChevron = showChevron
        self.titleColor = titleColor
        self.minHeight = minHeight
        self.padding = padding ?? EdgeInsets.fmSymmetric(horizontal: FmSpace.x4, vertical: FmSpace.x2)
    }

    @ViewBuilder private var titleLine: some View {
        if let titleView = titleView {
            titleView
        } else {
            Text(title)
                .fmText(.titleSmall, color: titleColor)
                .lineLimit(1)
                .truncationMode(.tail)
        }
    }

    @ViewBuilder private var subtitleLine: some View {
        if let subtitleView = subtitleView {
            subtitleView.padding(.top, 3)
        } else if let subtitle = subtitle, !subtitle.isEmpty {
            Text(subtitle)
                .fmText(.bodySmall, color: fm.textTertiary, height: FmType.leadingTight)
                .lineLimit(1)
                .truncationMode(.tail)
                .padding(.top, 3)
        }
    }

    private var lines: some View {
        VStack(alignment: .leading, spacing: 0) {
            titleLine
            subtitleLine
        }
        .frame(maxWidth: .infinity, alignment: .leading)
    }

    private var content: some View {
        HStack(spacing: 0) {
            if let leading = leading {
                leading.padding(.trailing, FmSpace.x3)
            }
            lines
            if let trailing = trailing {
                trailing.padding(.leading, FmSpace.x2)
            }
            if showChevron {
                FmIcon("chevron_right", size: 20, color: fm.textDisabled)
                    .padding(.leading, FmSpace.x1)
            }
        }
        .padding(padding)
        .frame(maxWidth: .infinity, minHeight: minHeight, alignment: .leading)
    }

    private func tapped() {
        guard let onTap = onTap else {
            return
        }
        FmHaptics.tap()
        onTap()
    }

    private func longPressed() {
        guard let onLongPress = onLongPress else {
            return
        }
        CaptureHold.noteHandled()
        FmHaptics.commit(.open)
        onLongPress()
    }

    /// Tap only: a Button, so VoiceOver and the keyboard get a real control.
    private var tapRow: some View {
        Button(action: tapped) {
            content
        }
        .buttonStyle(FmPressStyle(highlight: fm.accentSubtle))
    }

    /// With a long press a Button would also fire its tap when the finger
    /// lifts, so the row handles both gestures itself. The tap gesture is
    /// declared first: that order is what keeps a scroll view scrolling under
    /// a long-press recogniser.
    private var gestureRow: some View {
        content
            .background(pressing ? fm.accentSubtle : Color.clear)
            .contentShape(Rectangle())
            .onTapGesture {
                tapped()
            }
            .onLongPressGesture(
                minimumDuration: 0.5,
                maximumDistance: 10,
                perform: {
                    longPressed()
                },
                onPressingChanged: { (isPressing: Bool) in
                    pressing = isPressing
                }
            )
            .accessibilityElement(children: .combine)
            .accessibilityAddTraits(.isButton)
            .accessibilityAction(named: Text(verbatim: "More actions")) {
                longPressed()
            }
    }

    var body: some View {
        if onLongPress != nil {
            gestureRow
        } else if onTap != nil {
            tapRow
        } else {
            content
        }
    }
}

/// FmRowDivider: a hairline between explorer rows, inset past the leading tile.
struct FmRowDivider: View {
    private let indent: CGFloat

    init(indent: CGFloat = 66) {
        self.indent = indent
    }

    var body: some View {
        FmHairline()
            .padding(.leading, indent)
    }
}

/// FmCountPill: the small count beside a tab or section label ("History 50").
struct FmCountPill: View {
    @Environment(\.fmPalette) private var fm

    private let count: Int
    private let active: Bool

    init(count: Int, active: Bool = false) {
        self.count = count
        self.active = active
    }

    var body: some View {
        Text(String(count))
            .fmText(
                .labelSmall,
                color: active ? Color.white : fm.textSecondary,
                height: FmType.leadingTight,
                tracking: 0,
                tabular: true
            )
            .multilineTextAlignment(.center)
            .padding(.horizontal, 7)
            .padding(.vertical, 2)
            .frame(minWidth: 20)
            .background(active ? fm.accent : fm.bgElevated)
            .clipShape(Capsule())
    }
}

/// FmSegmentTab: one entry of FmSegmentTabBar.
struct FmSegmentTab: Equatable {
    let label: String
    let count: Int?
    let icon: String?

    init(label: String, count: Int? = nil, icon: String? = nil) {
        self.label = label
        self.count = count
        self.icon = icon
    }

    var accessibilityText: String {
        guard icon != nil, let count = count else {
            return label
        }
        return "\(label), \(count)"
    }
}

/// FmSegmentTabBar: the underlined "Changes / History / Branches" strip with
/// optional count pills, its tabs on glass.
struct FmSegmentTabBar: View {
    @Environment(\.fmPalette) private var fm

    private let tabs: [FmSegmentTab]
    private let index: Int
    private let onChanged: (Int) -> Void
    private let trailing: AnyView?

    init(tabs: [FmSegmentTab], index: Int, trailing: AnyView? = nil, onChanged: @escaping (Int) -> Void) {
        self.tabs = tabs
        self.index = index
        self.trailing = trailing
        self.onChanged = onChanged
    }

    private var segments: some View {
        HStack(spacing: 0) {
            ForEach(Array(tabs.indices), id: \.self) { (position: Int) in
                FmSegmentTabItem(tab: tabs[position], selected: position == index) {
                    onChanged(position)
                }
            }
        }
        .padding(.horizontal, FmSpace.x1)
        .shellGlass(RoundedRectangle(cornerRadius: FmRadius.lg, style: .continuous))
    }

    private var strip: some View {
        HStack(spacing: FmSpace.x2) {
            segments
            Spacer(minLength: 0)
            if let trailing = trailing {
                trailing
            }
        }
        .padding(.horizontal, FmSpace.x2)
        .padding(.vertical, FmSpace.x1)
        .frame(height: 46)
    }

    var body: some View {
        strip
    }
}

/// One tab of FmSegmentTabBar. The indicator sits on the bottom edge as an
/// overlay, so it takes the width of the label.
struct FmSegmentTabItem: View {
    @Environment(\.fmPalette) private var fm

    private let tab: FmSegmentTab
    private let selected: Bool
    private let onTap: () -> Void

    init(tab: FmSegmentTab, selected: Bool, onTap: @escaping () -> Void) {
        self.tab = tab
        self.selected = selected
        self.onTap = onTap
    }

    private var face: some View {
        HStack(spacing: 6) {
            if let icon = tab.icon {
                FmIcon(icon, size: 20, color: selected ? fm.textPrimary : fm.textTertiary)
            } else {
                Text(tab.label)
                    .fmText(
                        .titleSmall,
                        color: selected ? fm.textPrimary : fm.textTertiary,
                        weight: selected ? FmType.emphasis : FmType.regular
                    )
                    .lineLimit(1)
            }
            if let count = tab.count {
                FmCountPill(count: count, active: selected)
            }
        }
        .frame(maxHeight: .infinity)
        .overlay(alignment: .bottom) {
            FmRoundedShape(radius: 2)
                .fill(selected ? fm.activeUnderline : Color.clear)
                .frame(height: 2)
        }
        .padding(.horizontal, FmSpace.x2)
    }

    var body: some View {
        Button(action: {
            if !selected {
                FmHaptics.selection()
            }
            onTap()
        }) {
            face
        }
        .buttonStyle(FmPressStyle(highlight: fm.textPrimary.opacity(0.06)))
        .accessibilityLabel(Text(tab.accessibilityText))
        .accessibilityAddTraits(selected ? [.isButton, .isSelected] : [.isButton])
    }
}

/// FmGroupHeader: "STAGED · 6  [Unstage all]", an uppercase group label in a
/// semantic colour with a count and trailing actions.
struct FmGroupHeader: View {
    @Environment(\.fmPalette) private var fm

    private let label: String
    private let count: Int?
    private let color: Color?
    private let actions: [AnyView]
    private let padding: EdgeInsets

    init(
        label: String,
        count: Int? = nil,
        color: Color? = nil,
        actions: [AnyView] = [],
        padding: EdgeInsets? = nil
    ) {
        self.label = label
        self.count = count
        self.color = color
        self.actions = actions
        self.padding = padding ?? EdgeInsets.fmLTRB(FmSpace.x4, FmSpace.x3, FmSpace.x3, FmSpace.x2)
    }

    private var text: String {
        guard let count = count else {
            return label.uppercased()
        }
        return "\(label.uppercased()) \u{00B7} \(count)"
    }

    var body: some View {
        HStack(spacing: FmSpace.x2) {
            Text(text)
                .fmText(.labelSmall, color: color ?? fm.textTertiary)
                .lineLimit(1)
                .truncationMode(.tail)
                .frame(maxWidth: .infinity, alignment: .leading)
            ForEach(Array(actions.indices), id: \.self) { (position: Int) in
                actions[position]
            }
        }
        .padding(padding)
    }
}
