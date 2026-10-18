import FMCodeInsight
import SwiftUI

struct StrataDashboardView: View {
    let model: StrataModel
    let query: String
    let open: (StrataFocus) -> Void
    let showRule: (Int) -> Void

    @Environment(\.fmPalette) private var fm

    private var report: StrataReport { model.report }

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: FmSpace.x3) {
                if !query.isEmpty { searchResults }
                ratingCard
                tiles
                gatesCard
                severityCard
                if !report.graph.cycles.isEmpty { cyclesCard }
                rankedCard(title: "Modules with the most debt", items: Strata.topModules(report), modules: true)
                rankedCard(title: "Files with the most debt", items: Strata.topFiles(report), modules: false)
                allModules
            }
            .padding(FmSpace.x4)
        }
        .accessibilityIdentifier("strata-dashboard")
    }

    private var ratingCard: some View {
        let total = report.total
        let tone = StrataPalette.rating(total.rating)
        return FmCard {
            HStack(alignment: .center, spacing: FmSpace.x4) {
                Text(Strata.ratingLetter(total.rating))
                    .font(.system(size: 52, weight: .semibold))
                    .foregroundStyle(tone.readable)
                    .frame(width: 88, height: 88)
                    .background(LinearGradient(colors: [tone.color.opacity(0.9), tone.color], startPoint: .topLeading, endPoint: .bottomTrailing))
                    .clipShape(RoundedRectangle(cornerRadius: 18, style: .continuous))
                    .accessibilityLabel("Rating \(Strata.ratingLetter(total.rating))")
                VStack(alignment: .leading, spacing: FmSpace.x1) {
                    Text("Technical debt ratio").fmText(.bodySmall, color: fm.textSecondary)
                    Text(Strata.formatPercent(total.ratio)).fmText(.displaySmall, tabular: true)
                    Text("Total debt").fmText(.bodySmall, color: fm.textSecondary)
                    Text(Strata.formatDebt(total.debt)).fmText(.titleSmall, tabular: true)
                    Text("Annual interest").fmText(.bodySmall, color: fm.textSecondary)
                    Text(report.history ? Strata.formatDebt(total.interest) + " a year" : "Needs git history")
                        .fmText(.titleSmall, color: report.history ? fm.textPrimary : fm.textSecondary, tabular: true)
                }
                Spacer(minLength: 0)
            }
            .accessibilityElement(children: .combine)
        }
    }

    private var tiles: some View {
        LazyVGrid(columns: [GridItem(.flexible(), spacing: FmSpace.x3), GridItem(.flexible(), spacing: FmSpace.x3)], spacing: FmSpace.x3) {
            ForEach(Strata.dashboardTiles(model)) { (tile: StrataTile) in
                VStack(alignment: .leading, spacing: 2) {
                    Text(tile.label).fmText(.bodySmall, color: fm.textSecondary).lineLimit(1)
                    Text(tile.value).fmText(.titleLarge, tabular: true).lineLimit(1).minimumScaleFactor(0.6)
                    Text(tile.note.isEmpty ? " " : tile.note).fmText(.labelSmall, color: fm.textTertiary).lineLimit(1).truncationMode(.middle)
                }
                .padding(FmSpace.x3)
                .frame(maxWidth: .infinity, alignment: .leading)
                .background(fm.bgRaised)
                .clipShape(RoundedRectangle(cornerRadius: FmRadius.md, style: .continuous))
                .accessibilityElement(children: .combine)
            }
        }
    }

    private var gatesCard: some View {
        let headline = Strata.gatesHeadline(report)
        return FmCard {
            VStack(alignment: .leading, spacing: FmSpace.x2) {
                HStack {
                    Text("Quality gates").fmText(.titleSmall)
                    Spacer()
                    Text(headline.text).fmText(.labelMedium, color: StrataPalette.gate(headline.status).color)
                }
                ForEach(Array(report.gates.enumerated()), id: \.offset) { (_: Int, gate: StrataGate) in
                    gateRow(gate)
                }
            }
        }
        .accessibilityIdentifier("strata-gates")
    }

    private func gateRow(_ gate: StrataGate) -> some View {
        let tone = StrataPalette.gate(gate.status).color
        return VStack(alignment: .leading, spacing: 2) {
            HStack(alignment: .firstTextBaseline, spacing: FmSpace.x2) {
                FmStatusBadge(label: gate.status.name, color: tone)
                Text(gate.name).fmText(.bodyMedium).frame(maxWidth: .infinity, alignment: .leading)
            }
            Text(gate.measured).fmText(.bodySmall, color: fm.textPrimary, tabular: true)
            Text(gate.detail).fmText(.labelSmall, color: fm.textTertiary)
        }
        .padding(.vertical, FmSpace.x1)
        .accessibilityElement(children: .combine)
    }

    private var severityCard: some View {
        let total = max(1, report.total.violations)
        return FmCard {
            VStack(alignment: .leading, spacing: FmSpace.x3) {
                Text("Rule violations by severity").fmText(.titleSmall)
                GeometryReader { (proxy: GeometryProxy) in
                    HStack(spacing: 0) {
                        ForEach(StrataSeverity.allCases, id: \.self) { (level: StrataSeverity) in
                            Rectangle()
                                .fill(StrataPalette.severity(level).color)
                                .frame(width: proxy.size.width * CGFloat(report.bySeverity[level.rawValue]) / CGFloat(total))
                        }
                        Spacer(minLength: 0)
                    }
                    .background(fm.bgElevated)
                    .clipShape(Capsule())
                }
                .frame(height: 12)
                .accessibilityHidden(true)
                StrataSeverityLegend(items: StrataSeverity.allCases.map { ($0, report.bySeverity[$0.rawValue]) })
                ForEach(Strata.topRules(report), id: \.self) { (index: Int) in
                    let rule = report.rules[index]
                    Button {
                        showRule(index)
                    } label: {
                        HStack(spacing: FmSpace.x2) {
                            StrataDot(color: StrataPalette.severity(StrataSeverity.at(rule.worst)).color)
                            Text(rule.title).fmText(.bodyMedium).lineLimit(1).frame(maxWidth: .infinity, alignment: .leading)
                            Text("\(Strata.grouped(rule.violations.count)) · \(Strata.formatDebt(rule.debt))")
                                .fmText(.bodySmall, color: fm.textSecondary, tabular: true)
                        }
                        .frame(minHeight: 32)
                        .contentShape(Rectangle())
                    }
                    .buttonStyle(.plain)
                    .accessibilityHint("Shows the rule's violations")
                }
            }
        }
    }

    private var cyclesCard: some View {
        FmCard {
            VStack(alignment: .leading, spacing: FmSpace.x2) {
                Text("Module cycles").fmText(.titleSmall, color: StrataPalette.unhealthy.color)
                ForEach(Array(report.graph.cycles.enumerated()), id: \.offset) { (_: Int, cycle: [Int]) in
                    StrataFlowNames(names: cycle.map { (StrataText.moduleName(report.graph.nodes[$0]), $0) }, separator: "⇄") { (module: Int) in
                        open(.module(module))
                    }
                }
            }
        }
    }

    private func rankedCard(title: String, items: [Int], modules: Bool) -> some View {
        let maxDebt = items.first.map { modules ? report.modules[$0].debt : report.files[$0].debt } ?? 1
        return VStack(alignment: .leading, spacing: 0) {
            FmSectionHeader(title: title).padding(.bottom, FmSpace.x2)
            VStack(spacing: 0) {
                if items.isEmpty {
                    Text("No debt found.").fmText(.bodyMedium, color: fm.textSecondary).padding(FmSpace.x4)
                        .frame(maxWidth: .infinity, alignment: .leading)
                }
                ForEach(items, id: \.self) { (index: Int) in
                    let score = modules ? report.modules[index] : report.files[index]
                    let name = modules ? StrataText.pathOrRoot(report.graph.nodes[index].path) : model.snapshot.files[index].path
                    debtRow(name: name, score: score, maxDebt: maxDebt) {
                        open(modules ? .module(index) : .file(index))
                    }
                    if index != items.last { FmRowDivider(indent: 44) }
                }
            }
            .background(fm.bgRaised)
            .clipShape(RoundedRectangle(cornerRadius: FmRadius.lg, style: .continuous))
        }
    }

    private func debtRow(name: String, score: StrataScore, maxDebt: Double, action: @escaping () -> Void) -> some View {
        Button(action: action) {
            HStack(spacing: FmSpace.x3) {
                StrataRatingChip(rating: score.rating)
                VStack(alignment: .leading, spacing: 4) {
                    Text(name).fmText(.bodyMedium).lineLimit(1).truncationMode(.middle)
                    GeometryReader { (proxy: GeometryProxy) in
                        ZStack(alignment: .leading) {
                            Capsule().fill(fm.bgElevated)
                            Capsule().fill(StrataPalette.rating(score.rating).color.opacity(0.9))
                                .frame(width: max(6, proxy.size.width * CGFloat(score.debt / max(1, maxDebt))))
                        }
                    }
                    .frame(height: 5)
                }
                Text(Strata.formatDebt(score.debt)).fmText(.bodySmall, color: fm.textSecondary, tabular: true)
            }
            .padding(.horizontal, FmSpace.x3)
            .padding(.vertical, FmSpace.x2)
            .contentShape(Rectangle())
        }
        .buttonStyle(FmPressStyle(highlight: fm.accentSubtle))
        .accessibilityElement(children: .ignore)
        .accessibilityLabel("\(name), rating \(Strata.ratingLetter(score.rating)), \(Strata.formatDebt(score.debt)) debt")
        .accessibilityAddTraits(.isButton)
    }

    private var allModules: some View {
        VStack(alignment: .leading, spacing: 0) {
            FmSectionHeader(title: "All modules", count: report.graph.nodes.count).padding(.bottom, FmSpace.x2)
            StrataModuleList(model: model, query: query, open: open)
                .background(fm.bgRaised)
                .clipShape(RoundedRectangle(cornerRadius: FmRadius.lg, style: .continuous))
        }
    }

    private var searchResults: some View {
        let hits = Strata.searchHits(model, query)
        return VStack(alignment: .leading, spacing: 0) {
            FmSectionHeader(title: "Matches", count: hits.modules.count + hits.files.count).padding(.bottom, FmSpace.x2)
            VStack(spacing: 0) {
                if hits.modules.isEmpty && hits.files.isEmpty {
                    Text("Nothing matches \"\(query)\".").fmText(.bodyMedium, color: fm.textSecondary).padding(FmSpace.x4)
                        .frame(maxWidth: .infinity, alignment: .leading)
                }
                ForEach(hits.modules, id: \.self) { (m: Int) in
                    FmListRow(title: StrataText.moduleName(report.graph.nodes[m]), subtitle: "Module · " + Strata.moduleCaption(report.graph.nodes[m]),
                              leading: AnyView(StrataRatingChip(rating: report.modules[m].rating)), onTap: { open(.module(m)) }, minHeight: 48)
                }
                ForEach(hits.files, id: \.self) { (f: Int) in
                    FmListRow(title: Strata.fileName(model.snapshot.files[f].path), subtitle: model.snapshot.files[f].path,
                              leading: AnyView(StrataRatingChip(rating: report.files[f].rating)), onTap: { open(.file(f)) }, minHeight: 48)
                }
            }
            .background(fm.bgRaised)
            .clipShape(RoundedRectangle(cornerRadius: FmRadius.lg, style: .continuous))
        }
    }
}

private struct StrataSeverityLegend: View {
    @Environment(\.fmPalette) private var fm
    let items: [(StrataSeverity, Int)]

    var body: some View {
        LazyVGrid(columns: [GridItem(.adaptive(minimum: 110), alignment: .leading)], alignment: .leading, spacing: FmSpace.x1) {
            ForEach(items, id: \.0) { (item: (StrataSeverity, Int)) in
                HStack(spacing: FmSpace.x1) {
                    StrataDot(color: StrataPalette.severity(item.0).color, size: 9)
                    Text("\(item.0.name) \(Strata.grouped(item.1))").fmText(.bodySmall, tabular: true)
                }
                .accessibilityElement(children: .combine)
            }
        }
    }
}

struct StrataFlowNames: View {
    @Environment(\.fmPalette) private var fm
    let names: [(String, Int)]
    let separator: String
    let tap: (Int) -> Void

    var body: some View {
        LazyVGrid(columns: [GridItem(.adaptive(minimum: 120), alignment: .leading)], alignment: .leading, spacing: FmSpace.x1) {
            ForEach(Array(names.enumerated()), id: \.offset) { (offset: Int, item: (String, Int)) in
                Button {
                    tap(item.1)
                } label: {
                    Text(offset + 1 < names.count ? "\(item.0) \(separator)" : item.0)
                        .fmText(.bodyMedium, color: fm.accent)
                        .lineLimit(1)
                }
                .buttonStyle(.plain)
                .accessibilityLabel(item.0)
            }
        }
    }
}

struct StrataModuleList: View {
    @Environment(\.fmPalette) private var fm
    let model: StrataModel
    let query: String
    let open: (StrataFocus) -> Void
    @State private var expanded: Set<Int> = []

    var body: some View {
        let nodes = model.report.graph.nodes
        let modules = model.order.filter { Strata.moduleMatches(model, $0, query) }
        LazyVStack(spacing: 0) {
            ForEach(modules, id: \.self) { (m: Int) in
                let node = nodes[m]
                HStack(spacing: 0) {
                    FmListRow(
                        title: StrataText.moduleName(node),
                        subtitle: Strata.moduleCaption(node) + (node.cycle >= 0 ? " · cycle" : ""),
                        leading: AnyView(StrataRatingChip(rating: model.report.modules[m].rating)),
                        trailing: AnyView(Text(Strata.formatDebt(model.report.modules[m].debt)).fmText(.bodySmall, color: fm.textSecondary, tabular: true)),
                        onTap: { open(.module(m)) },
                        minHeight: 52)
                    Button {
                        FmHaptics.selection()
                        if expanded.contains(m) { expanded.remove(m) } else { expanded.insert(m) }
                    } label: {
                        FmIcon(expanded.contains(m) ? "expand_less" : "expand_more", size: 22, color: fm.textSecondary)
                            .frame(width: 44, height: 52)
                    }
                    .buttonStyle(.plain)
                    .accessibilityLabel(expanded.contains(m) ? "Hide the files of \(node.name)" : "Show the files of \(node.name)")
                }
                if expanded.contains(m) {
                    ForEach(node.files.filter { query.isEmpty || Strata.matches(model.snapshot.files[$0].path, query) || Strata.matches(node.path, query) }, id: \.self) { (f: Int) in
                        FmListRow(
                            title: Strata.fileName(model.snapshot.files[f].path),
                            subtitle: "\(Strata.grouped(model.snapshot.files[f].codeLines)) LOC · \(Strata.formatDebt(model.report.files[f].debt)) debt",
                            leading: AnyView(StrataRatingChip(rating: model.report.files[f].rating, size: 18)),
                            onTap: { open(.file(f)) },
                            minHeight: 44,
                            padding: EdgeInsets(top: FmSpace.x1, leading: FmSpace.x7, bottom: FmSpace.x1, trailing: FmSpace.x4))
                    }
                }
                if m != modules.last { FmRowDivider(indent: 50) }
            }
        }
    }
}
