import FMApi
import FMBase
import FMCode
import SwiftUI

enum RepoGraphPalette {
    static let lanes: [UInt32] = [0xFF58A6FF, 0xFFD29922, 0xFFDB61A2, 0xFFBC8CFF, 0xFF39C5CF, 0xFF3FB950]

    static func lane(_ index: Int) -> Color {
        let count = lanes.count
        return Color(fmARGB: lanes[((index % count) + count) % count])
    }

    static func surface(_ surface: RepoSurface) -> Color {
        switch surface {
        case .desktop: return Color(fmARGB: 0xFF8B7CFF)
        case .web: return Color(fmARGB: 0xFF2FBF71)
        case .mobile: return Color(fmARGB: 0xFFF0883E)
        }
    }

    static func icon(_ surface: RepoSurface) -> String {
        switch surface {
        case .desktop: return "desktop_mac_outlined"
        case .web: return "public"
        case .mobile: return "phone_iphone"
        }
    }
}

struct RepoGraphView: View {
    let model: RepoCommitsTabModel
    let marks: RepoSurfaceMarksModel
    let onOpen: (JSONObject) -> Void

    var body: some View {
        ScrollViewReader { (proxy: ScrollViewProxy) in
            ScrollView {
                if model.commits.loading {
                    RepoLoadingCard(label: "Loading commits...")
                        .padding(FmSpace.x4)
                } else if let message = model.commitsErrorMessage {
                    RepoErrorCard(message: message, onRetry: { Task { await model.refresh() } })
                        .padding(FmSpace.x4)
                } else if (model.commits.data ?? []).isEmpty {
                    RepoEmptyCard(message: "No commits.")
                        .padding(FmSpace.x4)
                } else {
                    content(model.graph(marks: marks.marks), proxy: proxy)
                }
            }
        }
    }

    @ViewBuilder private func content(_ graph: RepoGraph, proxy: ScrollViewProxy) -> some View {
        if RepoCommitsTabModel.showsSurfaces(graph, repo: model.repo) {
            RepoSurfaceLegend(spots: graph.spots, checking: !marks.loaded) { (index: Int) in
                withAnimation(.easeInOut(duration: 0.3)) {
                    proxy.scrollTo(index, anchor: .center)
                }
            }
            .padding(EdgeInsets.fmLTRB(FmSpace.x4, FmSpace.x4, FmSpace.x4, FmSpace.x2))
        }
        if let notice = model.refNotice {
            RepoRefNoticeCard(message: notice)
                .padding(EdgeInsets.fmLTRB(FmSpace.x4, FmSpace.x2, FmSpace.x4, FmSpace.x2))
        }
        LazyVStack(spacing: 0) {
            ForEach(graph.rows) { (row: RepoGraphRow) in
                RepoGraphRowView(row: row, lanes: graph.lanes, onTap: { onOpen(row.row.commit) })
                    .id(row.row.index)
            }
        }
        .padding(.bottom, FmSpace.x4)
    }
}

struct RepoSurfaceLegend: View {
    @Environment(\.fmPalette) private var fm

    let spots: [RepoSurfaceSpot]
    let checking: Bool
    let onJump: (Int) -> Void

    private func detail(_ spot: RepoSurfaceSpot) -> String {
        if checking && spot.mark == nil { return "Checking..." }
        guard let mark = spot.mark else { return spot.detail }
        return "\(mark.short) · \(spot.detail)"
    }

    private func row(_ spot: RepoSurfaceSpot) -> some View {
        let tint: Color = RepoGraphPalette.surface(spot.surface)
        let jump: Int? = spot.rowIndex
        return Button(action: {
            if let jump { onJump(jump) }
        }) {
            HStack(spacing: FmSpace.x3) {
                FmIconTile(icon: RepoGraphPalette.icon(spot.surface), tint: tint, size: 32, iconSize: 17)
                VStack(alignment: .leading, spacing: 2) {
                    Text(spot.surface.label)
                        .fmText(.titleSmall, color: fm.textPrimary)
                    Text(detail(spot))
                        .fmText(.bodySmall, color: spot.rowIndex == nil ? fm.textTertiary : fm.textSecondary)
                        .lineLimit(1)
                }
                .frame(maxWidth: .infinity, alignment: .leading)
                if jump != nil {
                    FmIcon("adjust", size: 18, color: tint)
                }
            }
            .padding(.vertical, FmSpace.x1)
            .contentShape(Rectangle())
        }
        .buttonStyle(FmPressStyle(highlight: fm.accentSubtle))
        .disabled(jump == nil)
        .accessibilityLabel(Text("\(spot.surface.label): \(detail(spot))"))
    }

    var body: some View {
        RepoInfoCard(title: "Where each app is") {
            VStack(alignment: .leading, spacing: FmSpace.x2) {
                ForEach(spots) { (spot: RepoSurfaceSpot) in
                    row(spot)
                }
            }
        }
    }
}

struct RepoSurfaceChip: View {
    let surface: RepoSurface

    var body: some View {
        let tint: Color = RepoGraphPalette.surface(surface)
        HStack(spacing: 4) {
            FmIcon(RepoGraphPalette.icon(surface), size: 13, color: tint)
            Text(surface.label)
                .font(.system(size: FmType.micro, weight: FmType.emphasis))
                .foregroundStyle(tint)
        }
        .padding(.horizontal, FmSpace.x2)
        .padding(.vertical, 3)
        .background(tint.opacity(0.16))
        .overlay {
            Capsule().strokeBorder(tint.opacity(0.55), lineWidth: 1)
        }
        .clipShape(Capsule())
    }
}

struct RepoGraphRowView: View {
    @Environment(\.fmPalette) private var fm

    let row: RepoGraphRow
    let lanes: Int
    let onTap: () -> Void

    private var gutterWidth: CGFloat { RepoGraphGutter.width(lanes: lanes) }

    private var headBadge: some View {
        Text("HEAD")
            .font(.system(size: FmType.micro, weight: FmType.emphasis))
            .foregroundStyle(fm.success)
            .padding(.horizontal, FmSpace.x2)
            .padding(.vertical, 2)
            .background(fm.success.opacity(0.18))
            .clipShape(FmRoundedShape(radius: FmRadius.sm))
    }

    private var lines: some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack(spacing: 0) {
                if row.row.isHead {
                    headBadge
                        .padding(.trailing, FmSpace.x2)
                }
                Text(row.row.subject)
                    .fmText(.titleSmall, color: fm.textPrimary, height: 1.15)
                    .lineLimit(1)
                    .truncationMode(.tail)
                    .frame(maxWidth: .infinity, alignment: .leading)
                if !row.row.time.isEmpty {
                    Text(row.row.time)
                        .fmText(.bodySmall, color: fm.textTertiary, weight: FmType.emphasis)
                        .padding(.leading, FmSpace.x1)
                }
            }
            HStack(spacing: FmSpace.x2) {
                if !row.row.short.isEmpty {
                    Text(row.row.short)
                        .font(FmFonts.monoFont(size: FmType.meta, weight: FmType.emphasis))
                        .foregroundStyle(RepoGraphPalette.lane(row.graph.column))
                }
                Text(row.row.author)
                    .fmText(.bodySmall, color: fm.textSecondary)
                    .lineLimit(1)
                    .truncationMode(.tail)
                    .frame(maxWidth: .infinity, alignment: .leading)
            }
            if !row.marks.isEmpty {
                HStack(spacing: 6) {
                    ForEach(row.marks) { (surface: RepoSurface) in
                        RepoSurfaceChip(surface: surface)
                    }
                }
            }
        }
        .padding(EdgeInsets.fmLTRB(0, FmSpace.x3, FmSpace.x2, FmSpace.x3))
    }

    var body: some View {
        Button(action: onTap) {
            HStack(spacing: 0) {
                Color.clear
                    .frame(width: gutterWidth)
                lines
                FmIcon("chevron_right", color: fm.textTertiary)
                    .padding(.trailing, FmSpace.x3)
            }
            .frame(minHeight: 64)
            .background(alignment: .leading) {
                RepoGraphGutter(
                    row: row.graph, marks: row.marks, isHead: row.row.isHead, surface: fm.listSurface
                )
                .frame(width: gutterWidth)
            }
            .contentShape(Rectangle())
        }
        .buttonStyle(FmPressStyle(highlight: fm.accentSubtle))
    }
}

struct RepoGraphGutter: View {
    let row: CommitGraphRow
    let marks: [RepoSurface]
    let isHead: Bool
    let surface: Color

    static let laneWidth: CGFloat = 16
    static let margin: CGFloat = 18
    static let pen: CGFloat = 2.2
    static let nodeOuter: CGFloat = 6.5

    static func width(lanes: Int) -> CGFloat {
        margin * 2 + CGFloat(max(lanes, 1) - 1) * laneWidth
    }

    var body: some View {
        Canvas { (context: inout GraphicsContext, size: CGSize) in
            draw(&context, size: size)
        }
        .accessibilityHidden(true)
    }

    private func x(_ lane: Int) -> CGFloat { RepoGraphGutter.margin + CGFloat(lane) * RepoGraphGutter.laneWidth }

    private func draw(_ context: inout GraphicsContext, size: CGSize) {
        let yTop: CGFloat = 0
        let yBot: CGFloat = size.height
        let yMid: CGFloat = (size.height / 2).rounded()
        let node: Int = row.column
        let top: Set<Int> = Set(row.lanesIn)
        let bottom: Set<Int> = Set(row.lanesOut)
        let ins: Set<Int> = Set(row.edges.filter { $0.to == node && $0.from != node }.map(\.from))
        let outs: Set<Int> = Set(row.edges.filter { $0.from == node && $0.to != node }.map(\.to))
        let radius: CGFloat = isHead || !marks.isEmpty ? RepoGraphGutter.nodeOuter + 1 : RepoGraphGutter.nodeOuter
        let trim: CGFloat = row.isMerge ? radius : 0
        let style = StrokeStyle(lineWidth: RepoGraphGutter.pen, lineCap: .round, lineJoin: .round)

        func stroke(_ path: Path, _ lane: Int) {
            context.stroke(path, with: .color(RepoGraphPalette.lane(lane)), style: style)
        }
        func straight(_ lane: Int, _ y0: CGFloat, _ y1: CGFloat) {
            var path = Path()
            path.move(to: CGPoint(x: x(lane), y: y0))
            path.addLine(to: CGPoint(x: x(lane), y: y1))
            stroke(path, lane)
        }
        func loopIn(_ lane: Int) {
            let x0 = x(lane)
            let x1 = x(node)
            let bend = max(0, min(abs(x1 - x0) - trim, yMid - yTop))
            let sx: CGFloat = x1 > x0 ? 1 : -1
            var path = Path()
            path.move(to: CGPoint(x: x0, y: yTop))
            path.addLine(to: CGPoint(x: x0, y: yMid - bend))
            path.addQuadCurve(to: CGPoint(x: x0 + sx * bend, y: yMid), control: CGPoint(x: x0, y: yMid))
            path.addLine(to: CGPoint(x: x1 - sx * trim, y: yMid))
            stroke(path, lane)
        }
        func loopOut(_ lane: Int) {
            let x0 = x(node)
            let x1 = x(lane)
            let bend = max(0, min(abs(x1 - x0) - trim, yBot - yMid))
            let sx: CGFloat = x1 > x0 ? 1 : -1
            var path = Path()
            path.move(to: CGPoint(x: x0 + sx * trim, y: yMid))
            path.addLine(to: CGPoint(x: x1 - sx * bend, y: yMid))
            path.addQuadCurve(to: CGPoint(x: x1, y: yMid + bend), control: CGPoint(x: x1, y: yMid))
            path.addLine(to: CGPoint(x: x1, y: yBot))
            stroke(path, lane)
        }

        let highest: Int = ([node] + row.lanesIn + row.lanesOut).max() ?? node
        for lane in 0...max(highest, 0) where lane != node {
            let passes = top.contains(lane) && bottom.contains(lane) && !ins.contains(lane)
            if passes {
                straight(lane, yTop, yBot)
            } else if top.contains(lane) && !ins.contains(lane) {
                straight(lane, yTop, yMid)
            } else if bottom.contains(lane) && !outs.contains(lane) {
                straight(lane, yMid, yBot)
            }
            if ins.contains(lane) { loopIn(lane) }
            if outs.contains(lane) { loopOut(lane) }
        }

        if top.contains(node) { straight(node, yTop, yMid - trim) }
        if bottom.contains(node) { straight(node, yMid + trim, yBot) }

        let center = CGPoint(x: x(node), y: yMid)
        let color: Color = RepoGraphPalette.lane(node)
        func circle(_ r: CGFloat) -> Path {
            Path(ellipseIn: CGRect(x: center.x - r, y: center.y - r, width: r * 2, height: r * 2))
        }
        if let mark = marks.first {
            context.stroke(circle(radius + 4), with: .color(RepoGraphPalette.surface(mark)), lineWidth: 2.5)
        }
        if row.isMerge {
            context.fill(circle(radius), with: .color(surface))
            context.stroke(circle(radius - 1), with: .color(color), lineWidth: RepoGraphGutter.pen)
            context.fill(circle(radius * 0.42), with: .color(color))
        } else {
            context.fill(circle(radius), with: .color(color))
            if isHead && marks.isEmpty {
                context.stroke(circle(radius + 3), with: .color(color.opacity(0.35)), lineWidth: 3)
            }
        }
    }
}
