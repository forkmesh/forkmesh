import FMCodeInsight
import SwiftUI

struct StrataBuildKey: Equatable {
    let depth: Int
    let analyzedAt: Int64
    let head: String
    let files: Int
    let lines: Int
}

struct StrataPage: View {
    let context: CodeInsightContext

    @Environment(\.fmPalette) private var fm
    @State private var kind: StrataViewKind = .dashboard
    @State private var depth = 0
    @State private var model: StrataModel?
    @State private var stamp = 0
    @State private var building = false
    @State private var focus: StrataFocus?
    @State private var sheet: StrataSheetItem?
    @State private var query = ""
    @State private var drill: Int?
    @State private var reportURL: URL?
    @State private var severity: StrataSeverity?
    @State private var expandedRule: Int?

    private var buildKey: StrataBuildKey {
        let snapshot = context.snapshot
        return StrataBuildKey(depth: depth, analyzedAt: snapshot.analyzedAt, head: snapshot.head, files: snapshot.files.count, lines: snapshot.totalCodeLines)
    }

    var body: some View {
        VStack(spacing: 0) {
            controls
            FmHairline()
            content
                .frame(maxWidth: .infinity, maxHeight: .infinity)
        }
        .background(fm.canvas)
        .searchable(text: $query, prompt: "Modules, files and rules")
        .task(id: buildKey) { await rebuild() }
        .sheet(item: $sheet) { (item: StrataSheetItem) in
            if let model {
                StrataDetailsSheet(
                    model: model,
                    root: item.focus,
                    openFile: { (path: String, line: Int) in
                        sheet = nil
                        context.openFile(path, line)
                    },
                    showFiles: showFiles)
            }
        }
    }

    private var controls: some View {
        FmEdgeFadeScroll {
            HStack(spacing: FmSpace.x2) {
                ForEach(StrataViewKind.allCases) { (option: StrataViewKind) in
                    StrataChip(label: option.label, icon: option.icon, active: kind == option) { kind = option }
                        .accessibilityIdentifier("strata-view-\(option.rawValue)")
                }
                depthMenu
                exportButton
            }
            .padding(.horizontal, FmSpace.x4)
            .padding(.vertical, FmSpace.x2)
        }
    }

    private var depthMenu: some View {
        Menu {
            Picker("Modules", selection: $depth) {
                Text("Auto").tag(0)
                ForEach(1...4, id: \.self) { (level: Int) in
                    Text(level == 1 ? "1 folder level" : "\(level) folder levels").tag(level)
                }
            }
        } label: {
            StrataMenuLabel(icon: "layers", text: depthLabel, highlighted: depth != 0)
        }
        .accessibilityLabel("Modules depth, \(depthLabel)")
        .accessibilityIdentifier("strata-depth")
    }

    private var depthLabel: String {
        if depth == 0 {
            if let model { return "Modules: Auto (\(model.report.graph.depth))" }
            return "Modules: Auto"
        }
        return "Modules: \(depth)"
    }

    @ViewBuilder private var exportButton: some View {
        if let reportURL {
            ShareLink(item: reportURL, subject: Text("Strata report"), preview: SharePreview(reportURL.lastPathComponent)) {
                StrataMenuLabel(icon: "ios_share", text: "Export report")
            }
            .accessibilityIdentifier("strata-export")
        } else {
            StrataMenuLabel(icon: "ios_share", text: "Export report")
                .opacity(0.4)
                .accessibilityLabel("Export report, preparing")
        }
    }

    @ViewBuilder private var content: some View {
        if context.snapshot.files.isEmpty {
            FmEmptyState(icon: "layers", title: "Nothing to analyze", message: context.snapshot.error.isEmpty ? "No source code was found in \(context.repoTitle)." : context.snapshot.error)
        } else if let model {
            page(model)
                .overlay(alignment: .top) {
                    if building { FmLinearProgressIndicator(height: 2) }
                }
        } else {
            VStack(spacing: FmSpace.x3) {
                FmCircularProgressIndicator()
                Text("Building the module graph…").fmText(.bodySmall, color: fm.textSecondary)
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity)
        }
    }

    @ViewBuilder private func page(_ model: StrataModel) -> some View {
        switch kind {
        case .dashboard:
            StrataDashboardView(model: model, query: query, open: open, showRule: { (rule: Int) in
                expandedRule = rule
                severity = nil
                kind = .rules
            })
        case .graph:
            StrataGraphView(model: model, stamp: stamp, query: query, focus: $focus, drill: $drill, open: open, openFile: context.openFile)
        case .matrix:
            StrataMatrixView(model: model, focus: $focus, open: open)
        case .scatter:
            StrataScatterView(model: model, query: query, focus: $focus, open: open, showFiles: showFiles)
        case .treemap:
            StrataTreemapView(model: model, stamp: stamp, query: query, focus: $focus, open: open, openFile: context.openFile, showFiles: showFiles)
        case .rules:
            StrataRulesView(model: model, query: query, severity: $severity, expanded: $expandedRule, open: open, openFile: context.openFile)
        }
    }

    private func showFiles(_ module: Int) {
        FmHaptics.tap()
        sheet = nil
        focus = .module(module)
        drill = module
        kind = .graph
    }

    private func open(_ target: StrataFocus) {
        FmHaptics.tap()
        if case .cell = target {} else { focus = target }
        sheet = StrataSheetItem(focus: target)
    }

    @MainActor
    private func rebuild() async {
        let snapshot = context.snapshot
        let level = depth
        let title = context.repoTitle.isEmpty ? snapshot.name : context.repoTitle
        building = true
        let built: StrataModel = await Task.detached(priority: .userInitiated) {
            Strata.buildModel(snapshot, depth: level)
        }.value
        if Task.isCancelled { return }
        if let current = model, current.report.graph.nodes.map(\.path) != built.report.graph.nodes.map(\.path) {
            focus = nil
            drill = nil
        }
        model = built
        stamp += 1
        building = false
        context.setSummary(built.summary)
        reportURL = await Task.detached(priority: .utility) { () -> URL? in
            StrataExport.write(built, title: title)
        }.value
    }
}

enum StrataExport {
    static func write(_ model: StrataModel, title: String) -> URL? {
        let safe = title.isEmpty ? "repository" : title.replacingOccurrences(of: "/", with: "-").replacingOccurrences(of: ":", with: "-")
        let url = FileManager.default.temporaryDirectory.appendingPathComponent("\(safe)-strata-report.html")
        let html = Strata.reportHtml(model.snapshot, model.report, title: title.isEmpty ? "repository" : title)
        do {
            try Data(html.utf8).write(to: url, options: .atomic)
            return url
        } catch {
            return nil
        }
    }
}
