import FMApi
import FMBase
import SwiftUI

enum ShellBarShapes {
    static func bar(_ style: ShellBarStyle, height: CGFloat, rows: Int = 1) -> AnyShape {
        switch style.shape {
        case .capsule where rows > 1:
            return AnyShape(
                RoundedRectangle(cornerRadius: CGFloat(style.barRadius(height: Double(height))), style: .continuous))
        case .capsule:
            return AnyShape(Capsule())
        case .panel:
            return AnyShape(
                RoundedRectangle(cornerRadius: CGFloat(style.barRadius(height: Double(height))), style: .continuous))
        case .squares, .circles, .icons:
            return AnyShape(RoundedRectangle(cornerRadius: 18, style: .continuous))
        }
    }

    static func cell(
        _ style: ShellBarStyle, side: CGFloat, first: Bool, last: Bool, top: Bool = true, bottom: Bool = true
    ) -> AnyShape {
        switch style.shape {
        case .capsule, .panel:
            let outer: CGFloat = max(0, CGFloat(style.barRadius(height: Double(side)) - ShellBarStyle.padding))
            let inner: CGFloat = min(ShellBottomBar.innerRadius, outer)
            func corner(_ edge: Bool, _ row: Bool) -> CGFloat { edge && row ? outer : inner }
            return AnyShape(
                UnevenRoundedRectangle(
                    topLeadingRadius: corner(first, top), bottomLeadingRadius: corner(first, bottom),
                    bottomTrailingRadius: corner(last, bottom), topTrailingRadius: corner(last, top),
                    style: .continuous))
        case .circles:
            return AnyShape(Circle())
        case .squares:
            return AnyShape(
                RoundedRectangle(cornerRadius: CGFloat(style.cellRadius(side: Double(side))), style: .continuous))
        case .icons:
            return AnyShape(RoundedRectangle(cornerRadius: min(14, side * 0.3), style: .continuous))
        }
    }
}

struct ShellBarBackdrop: ViewModifier {
    @Environment(\.fmPalette) var fm

    let shape: AnyShape
    let fill: ShellBarFill
    var interactive: Bool = true
    var lift: CGFloat = 12

    func body(content: Content) -> some View {
        if fill == .solid {
            content
                .background(shape.fill(fm.dark ? fm.bgElevated : fm.bgRaised))
                .overlay(shape.stroke(fm.border.opacity(0.8), lineWidth: 0.5))
                .shadow(color: Color.black.opacity(fm.dark ? 0.35 : 0.12), radius: lift, y: lift / 3)
        } else {
            #if compiler(>=6.2)
            if #available(iOS 26.0, *) {
                content.glassEffect(glass, in: shape)
            } else {
                material(content)
            }
            #else
            material(content)
            #endif
        }
    }

    #if compiler(>=6.2)
    @available(iOS 26.0, *)
    private var glass: Glass {
        switch fill {
        case .glass, .solid: return .regular.interactive(interactive)
        case .clear: return .clear.interactive(interactive)
        case .tinted: return .regular.tint(fm.accent.opacity(0.35)).interactive(interactive)
        }
    }
    #endif

    private func material<Inner: View>(_ inner: Inner) -> some View {
        inner
            .background {
                ZStack {
                    shape.fill(.ultraThinMaterial)
                        .opacity(fill == .clear ? 0.55 : 1)
                    if fill == .tinted {
                        shape.fill(fm.accent.opacity(0.18))
                    }
                }
            }
            .overlay(shape.stroke(fm.border.opacity(0.6), lineWidth: 0.5))
            .shadow(color: Color.black.opacity(0.12), radius: lift, y: lift / 3)
    }
}

struct ShellBarSlotChrome: ViewModifier {
    @Environment(\.fmPalette) var fm

    let shape: AnyShape
    let style: ShellBarStyle
    let selected: Bool

    func body(content: Content) -> some View {
        if style.shape.separate && style.shape.backed {
            marked(content).modifier(ShellBarBackdrop(shape: shape, fill: style.fill, lift: 5))
        } else {
            marked(content)
        }
    }

    private func marked<Inner: View>(_ inner: Inner) -> some View {
        inner
            .background {
                if selected {
                    under
                }
            }
            .overlay {
                if selected {
                    over
                }
            }
    }

    @ViewBuilder private var under: some View {
        switch style.selection {
        case .fill:
            shape.fill(fm.accent.opacity(style.shape.separate ? 0.2 : 0.14))
        case .glow:
            GeometryReader { (proxy: GeometryProxy) in
                let side: CGFloat = min(proxy.size.width, proxy.size.height)
                Circle()
                    .fill(
                        RadialGradient(
                            colors: [fm.accent.opacity(0.42), fm.accent.opacity(0)], center: .center, startRadius: 0,
                            endRadius: side * 0.6)
                    )
                    .frame(width: side * 1.2, height: side * 1.2)
                    .position(x: proxy.size.width / 2, y: proxy.size.height / 2)
            }
            .allowsHitTesting(false)
        case .outline, .dot, .underline, .none:
            EmptyView()
        }
    }

    @ViewBuilder private var over: some View {
        switch style.selection {
        case .outline:
            shape.stroke(fm.accent, lineWidth: 1.5)
                .padding(0.75)
        case .dot:
            Circle()
                .fill(fm.accent)
                .frame(width: 5, height: 5)
                .frame(maxHeight: .infinity, alignment: .bottom)
                .padding(.bottom, 2)
        case .underline:
            Capsule()
                .fill(fm.accent)
                .frame(width: 18, height: 3)
                .frame(maxHeight: .infinity, alignment: .bottom)
                .padding(.bottom, 1.5)
        case .fill, .glow, .none:
            EmptyView()
        }
    }
}

extension View {
    func shellBarBackdrop(_ shape: AnyShape, fill: ShellBarFill, interactive: Bool = true) -> some View {
        modifier(ShellBarBackdrop(shape: shape, fill: fill, interactive: interactive))
    }

    func shellBarSlotChrome(_ shape: AnyShape, style: ShellBarStyle, selected: Bool) -> some View {
        modifier(ShellBarSlotChrome(shape: shape, style: style, selected: selected))
    }

    @ViewBuilder func shellBarLift(_ on: Bool, dark: Bool) -> some View {
        if on {
            self.shadow(color: Color.black.opacity(dark ? 0.55 : 0.2), radius: 2, y: 1)
        } else {
            self
        }
    }

    @ViewBuilder func shellBarBacked(_ style: ShellBarStyle, shape: AnyShape, interactive: Bool = true) -> some View {
        if style.shape.separate {
            self
        } else {
            self.clipShape(shape).shellBarBackdrop(shape, fill: style.fill, interactive: interactive)
        }
    }
}

struct ShellBarPreview: View {
    @Environment(\.fmPalette) var fm

    let style: ShellBarStyle
    let iconSize: CGFloat
    let cellWidth: CGFloat
    let cellHeight: CGFloat

    @State private var available: CGFloat = 0

    private static let samples: [ShellBarItem] = [
        ShellBarItem(
            id: "preview-home", title: "Home", symbol: "square.grid.2x2", badge: 0, selected: false, kind: .action,
            action: {}),
        ShellBarItem(
            id: "preview-agents", title: "Agents", symbol: "sparkles", badge: 0, selected: true, kind: .action,
            active: true, live: 2, action: {}),
        ShellBarItem(
            id: "preview-inbox", title: "Inbox", symbol: "tray", badge: 4, selected: false, kind: .action, count: 12,
            action: {}),
        ShellBarItem(
            id: "preview-chat", title: "Chat", symbol: "bubble.left.and.bubble.right", badge: 0, selected: false,
            kind: .action, count: 3, action: {}),
        ShellBarItem(
            id: "preview-repos", title: "Repos", symbol: "books.vertical", badge: 0, selected: false, kind: .action,
            count: 248, action: {}),
    ]

    private var shown: [ShellBarItem] {
        Array(ShellBarPreview.samples.prefix(max(1, min(style.capacity, ShellBarPreview.samples.count))))
    }

    var body: some View {
        let items: [ShellBarItem] = shown
        let metrics: ShellBarMetrics = style.metrics(
            available: Double(available), count: items.count, cell: Double(cellWidth), height: Double(cellHeight))
        let glyph: CGFloat = (iconSize * CGFloat(style.size.scale)).rounded()
        let side: CGFloat = CGFloat(metrics.height)
        let lines: [[ShellBarItem]] = style.rowSplit(items)
        VStack(alignment: .leading, spacing: CGFloat(metrics.rowGap)) {
            ForEach(Array(lines.enumerated()), id: \.offset) { (line: (offset: Int, element: [ShellBarItem])) in
                HStack(spacing: CGFloat(metrics.gap)) {
                    ForEach(Array(line.element.enumerated()), id: \.element.id) {
                        (entry: (offset: Int, element: ShellBarItem)) in
                        let shape: AnyShape = ShellBarShapes.cell(
                            style, side: side, first: entry.offset == 0,
                            last: entry.offset == line.element.count - 1 && line.element.count == metrics.columns,
                            top: line.offset == 0, bottom: line.offset == lines.count - 1)
                        ShellBarCellLabel(
                            item: entry.element, iconSize: glyph, showsCounts: style.showCounts, look: style,
                            heat: style.highlightFrequent && entry.element.id == "preview-chat" ? 0.9 : 0)
                            .frame(width: CGFloat(metrics.slot), height: side)
                            .shellBarSlotChrome(shape, style: style, selected: entry.element.selected)
                    }
                }
            }
        }
        .padding(CGFloat(metrics.padding))
        .frame(width: style.width == .fit ? CGFloat(metrics.width) : nil)
        .frame(maxWidth: style.width == .fit ? nil : .infinity, alignment: .leading)
        .shellBarBacked(style, shape: ShellBarShapes.bar(style, height: side, rows: metrics.rows), interactive: false)
        .frame(maxWidth: .infinity)
        .onGeometryChange(for: CGFloat.self, of: { $0.size.width }) { (width: CGFloat) in
            available = width
        }
        .padding(.vertical, FmSpace.x4)
        .padding(.horizontal, FmSpace.x2)
        .background {
            LinearGradient(
                colors: [fm.accent.opacity(fm.dark ? 0.45 : 0.3), fm.bgBase, fm.accent.opacity(fm.dark ? 0.2 : 0.12)],
                startPoint: .topLeading, endPoint: .bottomTrailing)
        }
        .allowsHitTesting(false)
        .accessibilityElement(children: .ignore)
        .accessibilityLabel(Text("Preview of the bottom bar"))
        .accessibilityIdentifier("shell-bar-preview")
    }
}
