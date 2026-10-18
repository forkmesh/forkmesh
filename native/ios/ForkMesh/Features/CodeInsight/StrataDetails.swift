import FMCodeInsight
import SwiftUI
import UIKit

struct StrataDetailsSheet: View {
    let model: StrataModel
    let root: StrataFocus
    let openFile: (String, Int) -> Void
    let showFiles: (Int) -> Void

    @State private var path: [StrataFocus] = []

    var body: some View {
        NavigationStack(path: $path) {
            StrataDetailsPage(model: model, focus: root, openFile: openFile, showFiles: showFiles)
                .navigationDestination(for: StrataFocus.self) { (focus: StrataFocus) in
                    StrataDetailsPage(model: model, focus: focus, openFile: openFile, showFiles: showFiles)
                }
        }
        .presentationDetents([.medium, .large])
        .presentationDragIndicator(.visible)
    }
}

struct StrataDetailsPage: View {
    let model: StrataModel
    let focus: StrataFocus
    let openFile: (String, Int) -> Void
    let showFiles: (Int) -> Void

    @Environment(\.fmPalette) private var fm
    @Environment(\.dismiss) private var dismiss

    private var report: StrataReport { model.report }
    private var files: [InsightFile] { model.snapshot.files }

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: FmSpace.x3) {
                switch focus {
                case .module(let m) where m >= 0 && m < report.graph.nodes.count:
                    moduleDetails(m)
                case .file(let f) where f >= 0 && f < files.count:
                    fileDetails(f)
                case .cell(let a, let b) where a >= 0 && b >= 0 && a < report.graph.nodes.count && b < report.graph.nodes.count:
                    cellDetails(a, b)
                default:
                    Text("This item is no longer in the analysis.").fmText(.bodyMedium, color: fm.textSecondary)
                }
            }
            .padding(FmSpace.x4)
            .frame(maxWidth: .infinity, alignment: .leading)
        }
        .background(fm.canvas)
        .navigationTitle(title)
        .navigationBarTitleDisplayMode(.inline)
        .toolbar {
            ToolbarItem(placement: .confirmationAction) {
                Button("Done") { dismiss() }
            }
        }
    }

    private var title: String {
        switch focus {
        case .module(let m) where m >= 0 && m < report.graph.nodes.count: return StrataText.moduleName(report.graph.nodes[m])
        case .file(let f) where f >= 0 && f < files.count: return Strata.fileName(files[f].path)
        case .cell: return "Dependencies"
        default: return "Strata"
        }
    }

    private func scoreLine(_ score: StrataScore) -> some View {
        HStack(spacing: FmSpace.x2) {
            StrataRatingChip(rating: score.rating, size: 26)
            Text("\(Strata.formatPercent(score.ratio)) debt ratio · \(Strata.formatDebt(score.debt)) debt")
                .fmText(.bodyMedium, tabular: true)
        }
        .accessibilityElement(children: .combine)
    }

    private func facts(_ items: [StrataFact], dots: [String: Color] = [:]) -> some View {
        VStack(spacing: 0) {
            ForEach(items) { (fact: StrataFact) in
                HStack(alignment: .firstTextBaseline, spacing: FmSpace.x3) {
                    Text(fact.label).fmText(.bodySmall, color: fm.textSecondary).frame(width: 120, alignment: .leading)
                    if let dot = dots[fact.label] { StrataDot(color: dot, size: 9) }
                    Text(fact.value).fmText(.bodyMedium, tabular: true).frame(maxWidth: .infinity, alignment: .leading)
                }
                .padding(.vertical, 6)
                .accessibilityElement(children: .combine)
                if fact.id != items.last?.id { FmHairline() }
            }
        }
        .padding(.horizontal, FmSpace.x3)
        .background(fm.bgRaised)
        .clipShape(RoundedRectangle(cornerRadius: FmRadius.lg, style: .continuous))
    }

    private func card<Content: View>(@ViewBuilder _ content: () -> Content) -> some View {
        VStack(alignment: .leading, spacing: 0) { content() }
            .background(fm.bgRaised)
            .clipShape(RoundedRectangle(cornerRadius: FmRadius.lg, style: .continuous))
    }

    private func moduleLink(_ m: Int, subtitle: String, tone: Color? = nil) -> some View {
        NavigationLink(value: StrataFocus.module(m)) {
            HStack(spacing: FmSpace.x3) {
                StrataRatingChip(rating: report.modules[m].rating)
                VStack(alignment: .leading, spacing: 2) {
                    Text(StrataText.moduleName(report.graph.nodes[m])).fmText(.bodyMedium, color: tone ?? fm.accent).lineLimit(1).truncationMode(.middle)
                    Text(subtitle).fmText(.bodySmall, color: fm.textTertiary)
                }
                Spacer(minLength: 0)
                FmIcon("chevron_right", size: 18, color: fm.textDisabled)
            }
            .padding(.horizontal, FmSpace.x3)
            .padding(.vertical, FmSpace.x2)
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
    }

    private func fileLink(_ f: Int, subtitle: String? = nil) -> some View {
        NavigationLink(value: StrataFocus.file(f)) {
            HStack(spacing: FmSpace.x3) {
                StrataRatingChip(rating: report.files[f].rating, size: 20)
                VStack(alignment: .leading, spacing: 2) {
                    Text(files[f].path).fmText(.bodySmall, color: fm.accent).lineLimit(1).truncationMode(.middle)
                    if let subtitle { Text(subtitle).fmText(.labelSmall, color: fm.textTertiary) }
                }
                Spacer(minLength: 0)
                FmIcon("chevron_right", size: 18, color: fm.textDisabled)
            }
            .padding(.horizontal, FmSpace.x3)
            .padding(.vertical, FmSpace.x2)
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
    }

    private func violationRow(_ ref: StrataViolationRef, showFile: Bool) -> some View {
        let rule = report.rules[ref.rule]
        let v = report.violation(ref)
        let target = rule.moduleScope ? nil : Strata.violationTarget(model, rule: ref.rule, v)
        let place = target.map { Strata.fileName($0.path) + ($0.line > 0 ? ":\($0.line)" : "") }
        return Button {
            if let target { openFile(target.path, target.line) }
        } label: {
            HStack(alignment: .top, spacing: FmSpace.x2) {
                StrataDot(color: StrataPalette.severity(v.severity).color, size: 8).padding(.top, 6)
                VStack(alignment: .leading, spacing: 2) {
                    Text(showFile ? rule.title : v.message).fmText(.bodyMedium, color: target != nil && !showFile ? fm.accent : fm.textPrimary)
                    Text([showFile ? place : nil, showFile ? v.message : rule.title, Strata.formatDebt(v.debt)].compactMap { $0 }.joined(separator: " · "))
                        .fmText(.bodySmall, color: fm.textSecondary)
                }
                Spacer(minLength: 0)
            }
            .padding(.horizontal, FmSpace.x3)
            .padding(.vertical, FmSpace.x2)
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .disabled(target == nil)
        .accessibilityElement(children: .combine)
        .accessibilityHint(target == nil ? "" : "Opens the file at the line")
    }

    @ViewBuilder private func moduleDetails(_ m: Int) -> some View {
        let node = report.graph.nodes[m]
        let neighbours = Strata.moduleNeighbours(model, m)
        let worst = report.worstForModule(m, limit: 6)
        let ranked = Strata.moduleFilesByDebt(model, m)
        if node.path != node.name {
            Text(StrataText.pathOrRoot(node.path)).fmText(.bodySmall, color: fm.textSecondary).textSelection(.enabled)
        }
        scoreLine(report.modules[m])
        if let note = Strata.moduleCycleNote(model, m) {
            Text(note).fmText(.bodyMedium, color: StrataPalette.unhealthy.color)
        }
        facts(Strata.moduleFacts(model, m), dots: ["Health": StrataPalette.health(node.health).color, "Distance D": StrataPalette.scale(node.distance).color])
        FmFilledButton("Show its files in the graph", icon: "account_tree", fillWidth: true) { showFiles(m) }
            .accessibilityIdentifier("strata-show-files")
        if !worst.isEmpty {
            FmSectionHeader(title: "Worst rules", count: worst.count)
            card {
                ForEach(worst, id: \.self) { (ref: StrataViolationRef) in
                    violationRow(ref, showFile: true)
                    if ref != worst.last { FmHairline() }
                }
            }
        }
        FmSectionHeader(title: "Uses", count: neighbours.uses.count)
        neighbourList(neighbours.uses, tone: StrataTones(fm).accent)
        FmSectionHeader(title: "Used by", count: neighbours.usedBy.count)
        neighbourList(neighbours.usedBy, tone: StrataTones(fm).dependent)
        if !ranked.isEmpty {
            FmSectionHeader(title: "Files with the most debt", count: ranked.count)
            card {
                ForEach(ranked, id: \.self) { (f: Int) in
                    fileLink(f, subtitle: Strata.formatDebt(report.files[f].debt))
                    if f != ranked.last { FmHairline() }
                }
            }
        }
        let all = node.files.sorted { files[$0].path < files[$1].path }
        FmSectionHeader(title: "All files", count: all.count)
        card {
            ForEach(all, id: \.self) { (f: Int) in
                fileLink(f, subtitle: "\(Strata.grouped(files[f].codeLines)) LOC · \(Strata.formatDebt(report.files[f].debt)) debt")
                if f != all.last { FmHairline() }
            }
        }
    }

    @ViewBuilder private func neighbourList(_ items: [StrataNeighbour], tone: Color) -> some View {
        if items.isEmpty {
            Text("None").fmText(.bodyMedium, color: fm.textSecondary)
        } else {
            card {
                ForEach(items) { (item: StrataNeighbour) in
                    moduleLink(item.module, subtitle: "\(item.weight) imports\(item.cycle ? " · cycle" : "")", tone: tone)
                    if item.id != items.last?.id { FmHairline() }
                }
            }
        }
    }

    @ViewBuilder private func fileDetails(_ f: Int) -> some View {
        let info = files[f]
        let module = f < report.graph.moduleOfFile.count ? report.graph.moduleOfFile[f] : -1
        let refs = Strata.fileViolations(model, f)
        Text(info.path).fmText(.bodySmall, color: fm.textSecondary).textSelection(.enabled)
        scoreLine(report.files[f])
        HStack(spacing: FmSpace.x2) {
            FmFilledButton("Open the file", icon: "open_in_new") { openFile(info.path, 0) }
                .accessibilityIdentifier("strata-open-file")
            FmOutlinedButton("Copy path", icon: "content_copy") {
                UIPasteboard.general.string = info.path
                FmHaptics.success()
            }
        }
        if module >= 0 {
            FmSectionHeader(title: "Module")
            card { moduleLink(module, subtitle: Strata.moduleCaption(report.graph.nodes[module])) }
        }
        facts(Strata.fileFacts(model, f), dots: ["Health": StrataPalette.health(info.health).color])
        if !info.healthIssues.isEmpty {
            FmSectionHeader(title: "Health", count: info.healthIssues.count)
            VStack(alignment: .leading, spacing: FmSpace.x1) {
                ForEach(Array(info.healthIssues.enumerated()), id: \.offset) { (_: Int, issue: String) in
                    Text("• \(issue)").fmText(.bodySmall, color: fm.textSecondary)
                }
            }
        }
        if !refs.isEmpty {
            FmSectionHeader(title: "Rule violations", count: refs.count)
            card {
                ForEach(refs, id: \.self) { (ref: StrataViolationRef) in
                    violationRow(ref, showFile: false)
                    if ref != refs.last { FmHairline() }
                }
            }
        }
        if !info.dependsOn.isEmpty {
            FmSectionHeader(title: "Uses", count: info.dependsOn.count)
            card {
                ForEach(info.dependsOn, id: \.self) { (d: Int) in
                    fileLink(d)
                    if d != info.dependsOn.last { FmHairline() }
                }
            }
        }
        if !info.dependedOnBy.isEmpty {
            FmSectionHeader(title: "Used by", count: info.dependedOnBy.count)
            card {
                ForEach(info.dependedOnBy, id: \.self) { (d: Int) in
                    fileLink(d)
                    if d != info.dependedOnBy.last { FmHairline() }
                }
            }
        }
    }

    @ViewBuilder private func cellDetails(_ a: Int, _ b: Int) -> some View {
        if a == b {
            Text("\(StrataText.moduleName(report.graph.nodes[a])) has \(report.graph.nodes[a].internalEdges) imports between its own files.")
                .fmText(.bodyMedium)
        } else {
            direction(Strata.cellDirection(model, from: a, to: b), tone: StrataTones(fm).accent)
            direction(Strata.cellDirection(model, from: b, to: a), tone: StrataTones(fm).dependent)
        }
    }

    @ViewBuilder private func direction(_ cell: StrataCellDirection, tone: Color) -> some View {
        HStack(spacing: FmSpace.x2) {
            NavigationLink(value: StrataFocus.module(cell.from)) {
                Text(StrataText.moduleName(report.graph.nodes[cell.from])).fmText(.titleSmall, color: tone)
            }
            .buttonStyle(.plain)
            Text("→").fmText(.titleSmall, color: tone)
            NavigationLink(value: StrataFocus.module(cell.to)) {
                Text(StrataText.moduleName(report.graph.nodes[cell.to])).fmText(.titleSmall, color: tone)
            }
            .buttonStyle(.plain)
            Spacer(minLength: 0)
            Text("\(cell.weight) imports").fmText(.bodySmall, color: fm.textSecondary)
        }
        if !cell.exists {
            Text("No imports in this direction.").fmText(.bodyMedium, color: fm.textSecondary)
        } else {
            card {
                ForEach(cell.edges) { (edge: StrataFileEdgeRow) in
                    VStack(alignment: .leading, spacing: 2) {
                        Button { openFile(files[edge.from].path, 0) } label: {
                            Text(files[edge.from].path).fmText(.bodySmall, color: fm.accent).lineLimit(1).truncationMode(.middle)
                        }
                        .buttonStyle(.plain)
                        Text("imports").fmText(.labelSmall, color: fm.textTertiary)
                        Button { openFile(files[edge.to].path, 0) } label: {
                            Text(files[edge.to].path).fmText(.bodySmall, color: fm.accent).lineLimit(1).truncationMode(.middle)
                        }
                        .buttonStyle(.plain)
                    }
                    .padding(.horizontal, FmSpace.x3)
                    .padding(.vertical, FmSpace.x2)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    if edge.id != cell.edges.last?.id { FmHairline() }
                }
            }
            if cell.hidden > 0 {
                Text("and \(cell.hidden) more").fmText(.bodySmall, color: fm.textTertiary)
            }
        }
    }
}
