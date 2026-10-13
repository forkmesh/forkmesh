import FMApi
import SwiftUI

struct ShellHomeGroupInfo: Equatable, Identifiable {
    let id: String
    let title: String
    let count: Int

    static func symbol(_ id: String) -> String {
        switch id {
        case "main": return "arrow.up.right.circle"
        case "repo": return "folder"
        case "workspace": return "briefcase"
        case "operate": return "gearshape.2"
        case "business": return "chart.line.uptrend.xyaxis"
        case "personal": return "heart"
        case "insights": return "lightbulb"
        case "app": return "app.badge"
        default: return "square.grid.2x2"
        }
    }
}

struct ShellHomeIcon: View {
    @Environment(\.fmPalette) var fm

    let item: ShellBarItem
    let style: ShellHomeIconStyle
    let iconSize: CGFloat
    let labelSize: CGFloat
    var showsCounts: Bool = true
    var heat: Double = 0

    static let boxRatio: CGFloat = 1.75
    static let gap: CGFloat = 5

    private var box: CGFloat { (iconSize * ShellHomeIcon.boxRatio).rounded() }

    private var lit: Bool { item.selected || item.active }

    private var tiled: Bool { style == .tiles }

    private var corner: CGFloat { box * 0.3 }

    var body: some View {
        VStack(spacing: ShellHomeIcon.gap) {
            glyphBox
                .overlay(alignment: .topTrailing) { countAnchor }
                .overlay(alignment: .topLeading) { liveAnchor }
            Text(item.title)
                .font(.system(size: labelSize, weight: item.selected ? .semibold : .medium))
                .foregroundStyle(item.selected ? fm.accent : fm.textSecondary)
                .lineLimit(1)
                .minimumScaleFactor(0.75)
                .allowsTightening(true)
        }
    }

    private var glyphBox: some View {
        ZStack {
            if heat > 0 && !tiled {
                Circle()
                    .fill(
                        RadialGradient(
                            colors: [fm.accent.opacity(0.1 + 0.3 * heat), fm.accent.opacity(0)], center: .center,
                            startRadius: 0, endRadius: box * (0.5 + 0.25 * CGFloat(heat)))
                    )
                    .frame(width: box * 1.6, height: box * 1.6)
                    .allowsHitTesting(false)
                    .accessibilityHidden(true)
            }
            if tiled {
                tile
            }
            if item.active {
                ShellHomeActivityRing(size: tiled ? box + 5 : iconSize + 14, corner: tiled ? corner + 2.5 : nil)
                    .transition(.scale.combined(with: .opacity))
            }
            glyph
        }
        .frame(width: box, height: box)
    }

    private var glyph: some View {
        Image(systemName: item.symbol)
            .font(.system(size: tiled ? iconSize * 0.92 : iconSize, weight: lit ? .semibold : .medium))
            .symbolVariant(style == .filled ? .fill : .none)
            .symbolRenderingMode(.hierarchical)
            .foregroundStyle(glyphFill)
            .symbolEffect(.bounce, value: item.pulse)
            .shadow(color: Color.black.opacity(fm.dark ? 0.45 : 0.1), radius: fm.dark ? 1.5 : 1, y: 1)
    }

    private var glyphFill: LinearGradient {
        let top: Color
        let bottom: Color
        if lit || style == .accent {
            top = fm.accent
            bottom = fm.accent.opacity(0.72)
        } else {
            top = fm.textPrimary
            bottom = fm.textPrimary.opacity(fm.dark ? 0.74 : 0.82)
        }
        return LinearGradient(colors: [top, bottom], startPoint: .top, endPoint: .bottom)
    }

    private var tile: some View {
        let shape: RoundedRectangle = RoundedRectangle(cornerRadius: corner, style: .continuous)
        let raised: Color = fm.dark ? fm.bgElevated : fm.bgRaised
        return shape
            .fill(LinearGradient(colors: [raised, fm.bgOverlay], startPoint: .top, endPoint: .bottom))
            .overlay {
                if heat > 0 {
                    shape.fill(
                        RadialGradient(
                            colors: [fm.accent.opacity(0.06 + 0.22 * heat), fm.accent.opacity(0)], center: .top,
                            startRadius: 0, endRadius: box))
                }
            }
            .overlay {
                shape.strokeBorder(
                    LinearGradient(
                        colors: [
                            Color.white.opacity(fm.dark ? 0.16 : 0.9),
                            (heat > 0 ? fm.accent.opacity(0.25 * heat) : Color.white.opacity(fm.dark ? 0.03 : 0.2)),
                        ], startPoint: .top, endPoint: .bottom),
                    lineWidth: 0.75)
            }
            .shadow(color: Color.black.opacity(fm.dark ? 0.5 : 0.1), radius: fm.dark ? 4 : 3, y: 2)
            .accessibilityHidden(true)
    }

    private var anchorInset: CGFloat {
        tiled ? min(12, box * 0.28) : (box - iconSize) / 2 + iconSize * 0.3
    }

    private var anchorDrop: CGFloat {
        tiled ? min(10, box * 0.22) : (box - iconSize) / 2 + 6
    }

    @ViewBuilder private var countAnchor: some View {
        Color.clear
            .frame(width: 0, height: 0)
            .overlay(alignment: .bottomLeading) {
                if item.badge > 0 {
                    ShellHomeCountMark(text: ShellBottomBar.countText(item.badge), value: item.badge, unread: true)
                        .keyframeAnimator(initialValue: 1.0, trigger: item.pulse) { badge, scale in
                            badge.scaleEffect(scale, anchor: .bottomLeading)
                        } keyframes: { (_: Double) in
                            KeyframeTrack {
                                SpringKeyframe(1.4, duration: 0.18)
                                SpringKeyframe(0.92, duration: 0.16)
                                SpringKeyframe(1.0, duration: 0.2)
                            }
                        }
                        .transition(.scale(scale: 0.4, anchor: .bottomLeading).combined(with: .opacity))
                } else if showsCounts, let count = item.count, count > 0 {
                    ShellHomeCountMark(text: ShellBottomBar.countText(count), value: count, unread: false)
                        .transition(.scale(scale: 0.4, anchor: .bottomLeading).combined(with: .opacity))
                }
            }
            .offset(x: -anchorInset, y: anchorDrop)
            .accessibilityHidden(true)
    }

    @ViewBuilder private var liveAnchor: some View {
        if item.live > 0 {
            Color.clear
                .frame(width: 0, height: 0)
                .overlay(alignment: .bottomTrailing) {
                    ShellHeartbeatCount(count: item.live)
                        .fixedSize()
                }
                .offset(x: anchorInset, y: anchorDrop)
                .transition(.scale.combined(with: .opacity))
        }
    }
}

struct ShellHomeCountMark: View {
    @Environment(\.fmPalette) var fm

    let text: String
    let value: Int
    let unread: Bool

    var body: some View {
        Text(verbatim: text)
            .font(.system(.caption2, design: .rounded, weight: .bold).monospacedDigit())
            .foregroundStyle(unread ? Color.white : fm.count)
            .contentTransition(.numericText(value: Double(value)))
            .lineLimit(1)
            .fixedSize()
            .padding(.horizontal, 5.5)
            .frame(minWidth: 19, minHeight: 19)
            .background {
                if unread {
                    Capsule()
                        .fill(LinearGradient(colors: [fm.count, fm.count.opacity(0.82)], startPoint: .top, endPoint: .bottom))
                        .overlay(Capsule().strokeBorder(Color.white.opacity(0.22), lineWidth: 0.5))
                        .shadow(color: fm.count.opacity(fm.dark ? 0.45 : 0.25), radius: 3, y: 1)
                } else {
                    Capsule()
                        .fill(fm.dark ? fm.bgElevated : fm.bgRaised)
                        .overlay(Capsule().fill(fm.count.opacity(0.16)))
                        .overlay(Capsule().strokeBorder(fm.count.opacity(0.3), lineWidth: 0.5))
                }
            }
            .padding(1.5)
            .background(Capsule().fill(fm.bgBase))
            .fixedSize()
    }
}

struct ShellHomeActivityRing: View {
    @Environment(\.fmPalette) var fm
    @Environment(\.accessibilityReduceMotion) var reduceMotion

    let size: CGFloat
    var corner: CGFloat? = nil

    var body: some View {
        if let corner = corner {
            TimelineView(.animation(minimumInterval: 1.0 / 30.0, paused: reduceMotion)) {
                (context: TimelineViewDefaultContext) in
                let t: Double = context.date.timeIntervalSinceReferenceDate
                let spin: Double =
                    reduceMotion ? 0 : t.truncatingRemainder(dividingBy: ShellActivityHalo.turn) / ShellActivityHalo.turn
                let start: Double = 360 * spin
                let sweep: Double = 360 * Double(ShellActivityHalo.arc)
                RoundedRectangle(cornerRadius: corner, style: .continuous)
                    .strokeBorder(
                        reduceMotion
                            ? AnyShapeStyle(fm.accent)
                            : AnyShapeStyle(
                                AngularGradient(
                                    colors: [fm.accent.opacity(0), fm.accent], center: .center,
                                    startAngle: .degrees(start), endAngle: .degrees(start + sweep))),
                        lineWidth: 1.75)
                    .background(
                        RoundedRectangle(cornerRadius: corner, style: .continuous)
                            .strokeBorder(fm.accent.opacity(0.14), lineWidth: 1.75))
            }
            .frame(width: size, height: size)
            .allowsHitTesting(false)
            .accessibilityHidden(true)
        } else {
            ShellActivityHalo(size: size)
        }
    }
}

struct ShellHomeIconPreview: View {
    let style: ShellHomeIconStyle
    let iconSize: CGFloat
    let labelSize: CGFloat
    let showsCounts: Bool
    let highlight: Bool

    private static let samples: [ShellBarItem] = [
        ShellBarItem(
            id: "preview-agents", title: "Agents", symbol: "sparkles", badge: 3, selected: false, kind: .action,
            active: true, action: {}),
        ShellBarItem(
            id: "preview-inbox", title: "Inbox", symbol: "tray", badge: 0, selected: false, kind: .action, count: 12,
            action: {}),
        ShellBarItem(
            id: "preview-repos", title: "Repos", symbol: "books.vertical", badge: 0, selected: false, kind: .action,
            count: 248, action: {}),
        ShellBarItem(
            id: "preview-notes", title: "Notes", symbol: "note.text", badge: 0, selected: false, kind: .action,
            action: {}),
    ]

    var body: some View {
        HStack(spacing: 0) {
            ForEach(Array(ShellHomeIconPreview.samples.enumerated()), id: \.element.id) {
                (entry: (offset: Int, element: ShellBarItem)) in
                ShellHomeIcon(
                    item: entry.element, style: style, iconSize: iconSize, labelSize: labelSize,
                    showsCounts: showsCounts, heat: highlight && entry.offset == 1 ? 0.9 : 0)
                    .frame(maxWidth: .infinity)
            }
        }
        .padding(.top, FmSpace.x4)
        .padding(.bottom, FmSpace.x2)
        .accessibilityElement(children: .ignore)
        .accessibilityLabel(Text("Preview of the Home icons"))
    }
}
