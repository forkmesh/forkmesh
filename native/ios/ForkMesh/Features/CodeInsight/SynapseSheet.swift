import FMCodeInsight
import SwiftUI

struct SynapseSheet: View {
    let model: SynapseModel
    let open: (String) -> Void
    @Environment(\.fmPalette) private var fm

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: FmSpace.x3) {
                let selected = model.selected
                if selected >= 0 && selected < model.snapshot.files.count {
                    SynapseFileDetail(model: model, file: selected, open: open)
                } else {
                    SynapseOverview(model: model)
                }
            }
            .padding(FmSpace.x4)
            .frame(maxWidth: .infinity, alignment: .leading)
        }
        .background(fm.canvas)
    }
}

enum SynapseSheetStyle {
    static let red = SynapseRenderer.color(SynapsePalette.cycle)

    static func number(_ value: Int) -> String { value.formatted() }

    static func health(_ value: Double) -> String {
        value.formatted(.number.precision(.fractionLength(1)))
    }
}

struct SynapseFileRow: View {
    let model: SynapseModel
    let file: Int
    var detail: String?
    @Environment(\.fmPalette) private var fm

    var body: some View {
        let info = model.snapshot.files[file]
        Button {
            FmHaptics.selection()
            model.select(file, fly: true)
        } label: {
            HStack(spacing: FmSpace.x2) {
                if info.cycle >= 0 {
                    Circle().fill(SynapseSheetStyle.red).frame(width: 6, height: 6)
                }
                VStack(alignment: .leading, spacing: 1) {
                    Text(Synapse.fileName(info.path)).fmText(.bodyMedium, color: fm.accent).lineLimit(1)
                    Text(info.path).fmText(.bodySmall, color: fm.textTertiary).lineLimit(1).truncationMode(.middle)
                }
                Spacer(minLength: FmSpace.x2)
                if let detail {
                    Text(detail).fmText(.bodySmall, color: fm.textSecondary, tabular: true)
                }
            }
            .padding(.vertical, 6)
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .accessibilityElement(children: .ignore)
        .accessibilityLabel([info.path, detail, info.cycle >= 0 ? "in a circular dependency" : nil].compactMap { $0 }.joined(separator: ", "))
        .accessibilityHint("Selects the file and flies to it")
        .accessibilityAddTraits(.isButton)
    }
}

struct SynapseFileList: View {
    let model: SynapseModel
    let title: String
    let files: [Int]
    let cap: Int
    let empty: String
    var detail: ((Int) -> String)?
    @Environment(\.fmPalette) private var fm

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            FmSectionHeader(title: title, count: files.count)
            if files.isEmpty {
                Text(empty).fmText(.bodySmall, color: fm.textSecondary)
            }
            ForEach(files.prefix(cap), id: \.self) { (file: Int) in
                SynapseFileRow(model: model, file: file, detail: detail?(file))
            }
            if files.count > cap {
                Text("and \(SynapseSheetStyle.number(files.count - cap)) more").fmText(.bodySmall, color: fm.textSecondary)
                    .padding(.top, FmSpace.x1)
            }
        }
    }
}

struct SynapseFileDetail: View {
    let model: SynapseModel
    let file: Int
    let open: (String) -> Void
    @Environment(\.fmPalette) private var fm

    var body: some View {
        let snapshot = model.snapshot
        let info = snapshot.files[file]
        let imports = Synapse.sortedByPath(snapshot, info.dependsOn)
        let importers = Synapse.sortedByPath(snapshot, info.dependedOnBy)
        let packages = Synapse.externalPackages(info)
        VStack(alignment: .leading, spacing: FmSpace.x3) {
            VStack(alignment: .leading, spacing: FmSpace.x1) {
                Text(Synapse.fileName(info.path)).fmText(.titleMedium)
                    .accessibilityAddTraits(.isHeader)
                Text(info.path).fmText(.bodySmall, color: fm.textSecondary)
                    .textSelection(.enabled)
                facts(info)
                if let module = model.moduleName(file) {
                    (Text("Module ").foregroundStyle(fm.textSecondary) + Text(module).foregroundStyle(fm.textPrimary))
                        .fmText(.bodySmall)
                }
            }
            HStack(spacing: FmSpace.x2) {
                FmFilledButton("Open file", icon: "open_in_new") { open(info.path) }
                    .accessibilityHint("Opens \(info.path) in the editor")
                FmOutlinedButton("Fly to", icon: "flight") { model.flyToFile(file) }
                    .disabled(model.graph?.nodeFor(file) ?? -1 < 0)
                FmTextButton("Clear", icon: "close") { model.clearSelection() }
            }
            if info.cycle >= 0 && info.cycle < snapshot.cycles.count {
                loop(snapshot, info)
            }
            SynapseFileList(model: model, title: "Imports", files: imports, cap: 60, empty: "Imports no file of this repository.")
            SynapseFileList(model: model, title: "Imported by", files: importers, cap: 60, empty: "No file imports it.")
            if !packages.isEmpty {
                VStack(alignment: .leading, spacing: FmSpace.x1) {
                    FmSectionHeader(title: "External packages", count: packages.count)
                    ForEach(packages.prefix(40), id: \.self) { (name: String) in
                        Text(name).fmText(.bodySmall, color: fm.textSecondary).textSelection(.enabled)
                    }
                    if packages.count > 40 {
                        Text("and \(SynapseSheetStyle.number(packages.count - 40)) more").fmText(.bodySmall, color: fm.textSecondary)
                    }
                }
            }
        }
    }

    private func facts(_ info: InsightFile) -> some View {
        let health = SynapseRenderer.color(SynapsePalette.health(info.health))
        var parts: [Text] = []
        if !info.language.isEmpty { parts.append(Text(info.language)) }
        parts.append(Text("\(SynapseSheetStyle.number(info.codeLines)) lines"))
        parts.append(Text("health \(SynapseSheetStyle.health(info.health))/10").foregroundStyle(health))
        if info.test { parts.append(Text("test")) }
        var line = Text("")
        for (index, part) in parts.enumerated() {
            line = index == 0 ? part : line + Text(" · ") + part
        }
        return line.fmText(.bodySmall, color: fm.textSecondary)
    }

    private func loop(_ snapshot: InsightSnapshot, _ info: InsightFile) -> some View {
        let path = Synapse.cyclePath(snapshot, snapshot.cycles[info.cycle])
        let size = snapshot.cycles[info.cycle].count
        let graphCycle = model.graphCycle(for: file)
        return VStack(alignment: .leading, spacing: FmSpace.x1) {
            FmSectionHeader(title: "Circular dependency")
            HStack(alignment: .firstTextBaseline, spacing: FmSpace.x1) {
                Circle().fill(SynapseSheetStyle.red).frame(width: 8, height: 8)
                Text(Synapse.cycleText(snapshot, path)).fmText(.bodyMedium)
                    .textSelection(.enabled)
            }
            .accessibilityElement(children: .combine)
            .accessibilityLabel("Loop: " + Synapse.cycleText(snapshot, path).replacingOccurrences(of: " → ", with: ", imports "))
            if size > max(0, path.count - 1) {
                Text("\(SynapseSheetStyle.number(size)) files take part in this loop").fmText(.bodySmall, color: fm.textSecondary)
            }
            ForEach(Array(Set(path)).sorted { pathOrder(snapshot, $0, $1) }, id: \.self) { (step: Int) in
                if step != file { SynapseFileRow(model: model, file: step) }
            }
            if graphCycle >= 0 {
                FmTextButton("Show only this loop", icon: "visibility") { model.isolateCycle(graphCycle) }
            }
        }
    }

    private func pathOrder(_ snapshot: InsightSnapshot, _ a: Int, _ b: Int) -> Bool {
        snapshot.files[a].path < snapshot.files[b].path
    }
}

struct SynapseOverview: View {
    let model: SynapseModel
    @Environment(\.fmPalette) private var fm

    var body: some View {
        let _ = model.revision
        let snapshot = model.snapshot
        let graph = model.graph
        let imported = model.mostImported
        let importing = model.mostImporting
        let imports = model.imports
        VStack(alignment: .leading, spacing: FmSpace.x3) {
            Text("Overview").fmText(.titleMedium)
                .accessibilityAddTraits(.isHeader)
            LazyVGrid(columns: [GridItem(.flexible(), alignment: .leading), GridItem(.flexible(), alignment: .leading)],
                      alignment: .leading, spacing: FmSpace.x3) {
                stat(graph.map(Synapse.connectedCount) ?? 0, "connected files")
                stat(imports, "imports")
                stat(snapshot.cycles.count, "circular dependencies", colour: snapshot.cycles.isEmpty ? nil : SynapseSheetStyle.red)
                stat(model.orphans.count, "orphans")
                stat(model.leaves.count, "leaves")
                stat(graph?.lobes.count ?? 0, "modules")
            }
            if model.isolatedCycle >= 0 || model.showMode != .everything {
                FmTextButton("Show everything again", icon: "visibility") { model.setShowMode(.everything) }
            }
            if let graph, !graph.cycles.isEmpty {
                cycles(graph, snapshot)
            }
            if !imported.isEmpty {
                SynapseFileList(model: model, title: "Most imported", files: imported, cap: 10, empty: "",
                                detail: { SynapseSheetStyle.number(snapshot.files[$0].dependedOnBy.count) })
            }
            if !importing.isEmpty {
                SynapseFileList(model: model, title: "Most importing", files: importing, cap: 10, empty: "",
                                detail: { SynapseSheetStyle.number(snapshot.files[$0].dependsOn.count) })
            }
            if !model.orphans.isEmpty {
                VStack(alignment: .leading, spacing: FmSpace.x1) {
                    Text("Files no other file imports and that import nothing here. Show → Orphans places them in the brain.")
                        .fmText(.bodySmall, color: fm.textSecondary)
                    SynapseFileList(model: model, title: "Orphans", files: model.orphans, cap: 30, empty: "")
                }
            }
            if snapshot.cycles.isEmpty && imports > 0 {
                VStack(alignment: .leading, spacing: FmSpace.x1) {
                    FmSectionHeader(title: "Circular dependencies")
                    Text("None. Every import points one way.")
                        .fmText(.bodyMedium, color: SynapseRenderer.color(SynapsePalette.health(10)))
                }
            }
        }
    }

    private func stat(_ value: Int, _ label: String, colour: Color? = nil) -> some View {
        VStack(alignment: .leading, spacing: 2) {
            Text(SynapseSheetStyle.number(value)).fmText(.titleLarge, color: colour, tabular: true)
            Text(label).fmText(.bodySmall, color: fm.textSecondary)
        }
        .accessibilityElement(children: .combine)
    }

    private func cycles(_ graph: SynapseGraph, _ snapshot: InsightSnapshot) -> some View {
        let order = Synapse.cycleOrder(graph)
        return VStack(alignment: .leading, spacing: 0) {
            FmSectionHeader(title: "Circular dependencies", count: order.count)
            ForEach(order.prefix(40), id: \.self) { (c: Int) in
                cycleRow(graph, snapshot, c)
            }
            if order.count > 40 {
                Text("and \(SynapseSheetStyle.number(order.count - 40)) more").fmText(.bodySmall, color: fm.textSecondary)
                    .padding(.top, FmSpace.x1)
            }
        }
    }

    private func cycleRow(_ graph: SynapseGraph, _ snapshot: InsightSnapshot, _ c: Int) -> some View {
        let info = graph.cycles[c]
        var text = Synapse.cycleText(snapshot, info.path)
        if text.isEmpty { text = "\(SynapseSheetStyle.number(info.files.count)) files" }
        let extra = info.files.count > max(0, info.path.count - 1) ? "\(SynapseSheetStyle.number(info.files.count)) files" : nil
        let active = model.isolatedCycle == c
        return Button {
            FmHaptics.selection()
            model.isolateCycle(c)
        } label: {
            HStack(alignment: .firstTextBaseline, spacing: FmSpace.x2) {
                Circle().fill(SynapseSheetStyle.red).frame(width: 8, height: 8)
                Text(text).fmText(.bodyMedium, color: fm.accent, weight: active ? .semibold : nil)
                    .multilineTextAlignment(.leading)
                Spacer(minLength: FmSpace.x1)
                if let extra {
                    Text(extra).fmText(.bodySmall, color: fm.textSecondary, tabular: true)
                }
            }
            .padding(.vertical, 6)
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .accessibilityElement(children: .ignore)
        .accessibilityLabel("Loop: " + text.replacingOccurrences(of: " → ", with: ", imports ") + (extra.map { ", \($0) take part" } ?? ""))
        .accessibilityHint("Shows only this loop and flies to it")
        .accessibilityAddTraits(active ? [.isButton, .isSelected] : .isButton)
    }
}
