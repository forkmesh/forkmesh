import FMCodeInsight
import SwiftUI

extension StrataRGB {
    var color: Color { Color(.sRGB, red: red, green: green, blue: blue, opacity: 1) }
    var readable: Color { prefersDarkText ? Color(fmARGB: 0xFF0D1117) : Color.white }
}

enum StrataViewKind: String, CaseIterable, Identifiable {
    case dashboard, graph, matrix, scatter, treemap, rules

    var id: String { rawValue }

    var label: String {
        switch self {
        case .dashboard: return "Dashboard"
        case .graph: return "Graph"
        case .matrix: return "Matrix"
        case .scatter: return "Scatter"
        case .treemap: return "Treemap"
        case .rules: return "Rules"
        }
    }

    var icon: String {
        switch self {
        case .dashboard: return "dashboard"
        case .graph: return "account_tree"
        case .matrix: return "grid_on"
        case .scatter: return "scatter_plot"
        case .treemap: return "view_quilt"
        case .rules: return "rule"
        }
    }
}

enum StrataFocus: Hashable {
    case module(Int)
    case file(Int)
    case cell(Int, Int)

    var module: Int? {
        if case .module(let index) = self { return index }
        return nil
    }

    var file: Int? {
        if case .file(let index) = self { return index }
        return nil
    }
}

struct StrataSheetItem: Identifiable {
    let focus: StrataFocus
    var id: String {
        switch focus {
        case .module(let m): return "m\(m)"
        case .file(let f): return "f\(f)"
        case .cell(let a, let b): return "c\(a)-\(b)"
        }
    }
}

struct StrataTones {
    let accent: Color
    let dependent: Color
    let cycle: Color
    let surface: Color
    let raised: Color
    let text: Color
    let muted: Color
    let faint: Color
    let border: Color
    let canvas: Color
    let dark: Bool

    init(_ fm: FmPalette) {
        accent = fm.accent
        dependent = fm.dark ? Color(fmARGB: 0xFFBC8CFF) : Color(fmARGB: 0xFF8250DF)
        cycle = StrataPalette.unhealthy.color
        surface = fm.bgRaised
        raised = fm.bgOverlay
        text = fm.textPrimary
        muted = fm.textSecondary
        faint = fm.bgElevated
        border = fm.border
        canvas = fm.canvas == Color.clear ? fm.bgBase : fm.canvas
        dark = fm.dark
    }
}

struct StrataRatingChip: View {
    let rating: Int
    var size: CGFloat = 22

    var body: some View {
        let tone = StrataPalette.rating(rating)
        Text(Strata.ratingLetter(rating))
            .font(.system(size: size * 0.55, weight: .semibold))
            .foregroundStyle(tone.readable)
            .frame(width: size, height: size)
            .background(tone.color)
            .clipShape(RoundedRectangle(cornerRadius: size * 0.25, style: .continuous))
            .accessibilityLabel("Rating \(Strata.ratingLetter(rating))")
    }
}

struct StrataDot: View {
    let color: Color
    var size: CGFloat = 8

    var body: some View {
        Circle().fill(color).frame(width: size, height: size).accessibilityHidden(true)
    }
}

struct StrataChip: View {
    @Environment(\.fmPalette) private var fm
    let label: String
    var icon: String?
    let active: Bool
    let action: () -> Void

    var body: some View {
        Button {
            FmHaptics.selection()
            action()
        } label: {
            HStack(spacing: FmSpace.x1) {
                if let icon {
                    FmIcon(icon, size: 16, color: active ? fm.accent : fm.textSecondary)
                }
                Text(label).fmText(.labelMedium, color: active ? fm.accent : fm.textSecondary)
            }
            .padding(.horizontal, FmSpace.x3)
            .frame(height: 32)
            .background(active ? fm.accentSubtle : fm.bgRaised)
            .clipShape(Capsule())
        }
        .buttonStyle(.plain)
        .accessibilityAddTraits(active ? .isSelected : [])
    }
}

struct StrataMenuLabel: View {
    @Environment(\.fmPalette) private var fm
    let icon: String
    let text: String
    var highlighted = false

    var body: some View {
        HStack(spacing: FmSpace.x1) {
            FmIcon(icon, size: 16, color: highlighted ? fm.accent : fm.textSecondary)
            Text(text).fmText(.labelMedium, color: highlighted ? fm.accent : fm.textSecondary).lineLimit(1)
        }
        .padding(.horizontal, FmSpace.x3)
        .frame(height: 32)
        .background(fm.bgRaised)
        .clipShape(Capsule())
    }
}

struct StrataGradientBar: View {
    let colors: [Color]
    var height: CGFloat = 8

    var body: some View {
        LinearGradient(colors: colors, startPoint: .leading, endPoint: .trailing)
            .frame(height: height)
            .clipShape(Capsule())
            .accessibilityHidden(true)
    }
}

struct StrataZoom: Equatable {
    var scale: CGFloat = 1
    var offset: CGSize = .zero

    func world(_ point: CGPoint) -> CGPoint {
        CGPoint(x: (point.x - offset.width) / scale, y: (point.y - offset.height) / scale)
    }

    static func fit(_ bounds: StrataRect, in size: CGSize, inset: CGFloat = 20, maxScale: CGFloat = 1.25) -> StrataZoom {
        guard bounds.width > 0, bounds.height > 0, size.width > 0, size.height > 0 else { return StrataZoom() }
        let width = max(1, size.width - inset * 2)
        let height = max(1, size.height - inset * 2)
        let scale = min(maxScale, max(0.03, min(width / bounds.width, height / bounds.height)))
        let offset = CGSize(width: size.width / 2 - bounds.centerX * scale, height: size.height / 2 - bounds.centerY * scale)
        return StrataZoom(scale: scale, offset: offset)
    }
}

struct StrataLiveGesture: Equatable {
    var magnification: CGFloat = 1
    var anchor: CGPoint = .zero
    var translation: CGSize = .zero
}

extension StrataZoom {
    func applying(_ live: StrataLiveGesture) -> StrataZoom {
        let m = live.magnification
        let x = live.anchor.x - (live.anchor.x - offset.width) * m + live.translation.width
        let y = live.anchor.y - (live.anchor.y - offset.height) * m + live.translation.height
        return StrataZoom(scale: scale * m, offset: CGSize(width: x, height: y))
    }
}

struct StrataZoomPan: ViewModifier {
    @Binding var zoom: StrataZoom
    @Binding var live: StrataLiveGesture
    var minScale: CGFloat = 0.03
    var maxScale: CGFloat = 4

    private func clampedMagnification(_ value: CGFloat) -> CGFloat {
        let target = min(maxScale, max(minScale, zoom.scale * value))
        return target / max(zoom.scale, 0.0001)
    }

    func body(content: Content) -> some View {
        content.gesture(
            SimultaneousGesture(
                MagnifyGesture()
                    .onChanged { (value: MagnifyGesture.Value) in
                        live.magnification = clampedMagnification(value.magnification)
                        live.anchor = value.startLocation
                    }
                    .onEnded { (value: MagnifyGesture.Value) in
                        let m = clampedMagnification(value.magnification)
                        let anchor = value.startLocation
                        zoom = zoom.applying(StrataLiveGesture(magnification: m, anchor: anchor, translation: .zero))
                        live.magnification = 1
                    },
                DragGesture(minimumDistance: 6)
                    .onChanged { (value: DragGesture.Value) in live.translation = value.translation }
                    .onEnded { (value: DragGesture.Value) in
                        zoom.offset.width += value.translation.width
                        zoom.offset.height += value.translation.height
                        live.translation = .zero
                    }
            )
        )
    }
}

extension View {
    func strataZoomPan(_ zoom: Binding<StrataZoom>, live: Binding<StrataLiveGesture>, maxScale: CGFloat = 4) -> some View {
        modifier(StrataZoomPan(zoom: zoom, live: live, maxScale: maxScale))
    }
}

extension StrataRect {
    var cg: CGRect { CGRect(x: x, y: y, width: width, height: height) }
}

extension StrataPoint {
    var cg: CGPoint { CGPoint(x: x, y: y) }
}

enum StrataText {
    static func moduleName(_ node: InsightModuleNode) -> String {
        node.name.isEmpty ? (node.path.isEmpty ? "(root)" : node.path) : node.name
    }

    static func pathOrRoot(_ path: String) -> String {
        path.isEmpty ? "(root)" : path
    }
}
