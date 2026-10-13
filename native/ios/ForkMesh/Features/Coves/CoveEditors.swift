import FMBase
import FMCoves
import SwiftUI
import UIKit

struct CoveDocumentScreen: View {
    @Bindable var model: CovesModel
    let coveId: String
    let documentName: String
    @State private var body_: String = ""
    @State private var original: String = ""
    @State private var loaded = false
    @State private var saving = false
    @State private var error: String?
    @State private var confirmDiscard = false
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        Group {
            if let document = model.cove(coveId)?.document(named: documentName) {
                editor(for: document)
                    .onAppear {
                        if !loaded {
                            body_ = document.body
                            original = document.body
                            loaded = true
                        }
                    }
            } else {
                ContentUnavailableView {
                    Label("Document unavailable", systemImage: "doc.questionmark")
                } description: {
                    Text("The cove was locked or the document was removed.")
                } actions: {
                    FmMessageTools("Document unavailable. The cove was locked or the document was removed.")
                }
            }
        }
        .navigationTitle(documentName)
        .navigationBarTitleDisplayMode(.inline)
        .navigationBarBackButtonHidden(dirty)
        .toolbar {
            if dirty {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Discard") { confirmDiscard = true }
                }
            }
            ToolbarItem(placement: .confirmationAction) {
                if saving {
                    ProgressView()
                } else {
                    Button("Save") { Task { await save() } }.disabled(!dirty)
                }
            }
        }
        .safeAreaInset(edge: .top) {
            if let error {
                Text(error)
                    .font(.footnote)
                    .foregroundStyle(.white)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .fmMessageTools(error, color: .white)
                    .padding(8)
                    .background(Color.red)
            }
        }
        .confirmationDialog("Discard your changes?", isPresented: $confirmDiscard, titleVisibility: .visible) {
            Button("Discard", role: .destructive) {
                body_ = original
                dismiss()
            }
        }
    }

    private var dirty: Bool { loaded && body_ != original }

    @ViewBuilder private func editor(for document: CoveDocument) -> some View {
        switch CoveFiles.editorKind(path: document.name, mime: document.mime) {
        case .spreadsheet:
            CoveSpreadsheetEditor(text: $body_)
        case .pipeline:
            CovePipelineEditor(text: $body_, actor: model.account)
        case .text:
            TextEditor(text: $body_)
                .font(.system(.body, design: document.name.lowercased().hasSuffix(".json") ? .monospaced : .default))
                .autocorrectionDisabled(document.name.lowercased().hasSuffix(".json"))
                .padding(.horizontal, 8)
        }
    }

    private func save() async {
        saving = true
        defer { saving = false }
        do {
            try await model.saveDocument(coveId: coveId, name: documentName, body: body_)
            original = body_
            error = nil
            FmHaptics.commit()
        } catch {
            self.error = "\(error)"
            FmHaptics.failure()
        }
    }
}

struct CoveSpreadsheetEditor: View {
    @Binding var text: String
    @State private var rows: [[String]] = []
    @State private var editing: CellRef?

    struct CellRef: Identifiable, Hashable {
        let row: Int
        let column: Int
        var id: String { "\(row):\(column)" }
    }

    var body: some View {
        ScrollView([.horizontal, .vertical]) {
            Grid(alignment: .leading, horizontalSpacing: 1, verticalSpacing: 1) {
                ForEach(rows.indices, id: \.self) { row in
                    GridRow {
                        ForEach(0..<columnCount, id: \.self) { column in
                            cell(row: row, column: column)
                        }
                    }
                }
            }
            .padding(8)
        }
        .safeAreaInset(edge: .bottom, spacing: 0) {
            HStack {
                glassButton("Add row", systemImage: "plus.rectangle") { addRow() }
                Spacer()
                glassButton("Add column", systemImage: "rectangle.split.3x1") { addColumn() }
                Spacer()
                Menu {
                    Button("Delete last row", role: .destructive) { removeRow() }.disabled(rows.count <= 1)
                    Button("Delete last column", role: .destructive) { removeColumn() }.disabled(columnCount <= 1)
                } label: {
                    Image(systemName: "ellipsis.circle")
                        .font(.system(size: 18, weight: .medium))
                        .frame(width: 44, height: 44)
                        .contentShape(Circle())
                        .shellGlass(Circle())
                }
                .accessibilityLabel("More")
            }
            .padding(.horizontal, FmSpace.x4)
            .padding(.vertical, FmSpace.x2)
        }
        .onAppear { rows = CoveFiles.parseCsv(text) }
        .sheet(item: $editing) { cell in
            CellEditor(value: value(at: cell)) { newValue in
                editing = nil
                if let newValue { set(newValue, at: cell) }
            }
        }
    }

    private var columnCount: Int { max(1, rows.map(\.count).max() ?? 1) }

    private func glassButton(_ title: String, systemImage: String, action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Image(systemName: systemImage)
                .font(.system(size: 18, weight: .medium))
                .foregroundStyle(Color.accentColor)
                .frame(width: 44, height: 44)
                .contentShape(Circle())
                .shellGlass(Circle())
        }
        .buttonStyle(.plain)
        .accessibilityLabel(title)
    }

    private func cell(row: Int, column: Int) -> some View {
        let value: String = column < rows[row].count ? rows[row][column] : ""
        let header: Bool = row == 0
        let font: Font = header ? Font.subheadline.weight(.semibold) : Font.subheadline
        let fill: Color = header ? Color.secondary.opacity(0.15) : Color(uiColor: .secondarySystemBackground)
        return Button {
            editing = CellRef(row: row, column: column)
        } label: {
            Text(value.isEmpty ? " " : value)
                .font(font)
                .lineLimit(2)
                .frame(width: 140, alignment: .leading)
                .frame(minHeight: 44)
                .padding(.horizontal, 6)
                .background(fill)
        }
        .buttonStyle(.plain)
        .accessibilityLabel("Row \(row + 1), column \(column + 1): \(value)")
    }

    private func value(at cell: CellRef) -> String {
        guard cell.row < rows.count, cell.column < rows[cell.row].count else { return "" }
        return rows[cell.row][cell.column]
    }

    private func set(_ value: String, at cell: CellRef) {
        guard cell.row < rows.count else { return }
        while rows[cell.row].count <= cell.column { rows[cell.row].append("") }
        rows[cell.row][cell.column] = value
        commit()
    }

    private func addRow() {
        rows.append(Array(repeating: "", count: columnCount))
        commit()
    }

    private func addColumn() {
        let width = columnCount
        for index in rows.indices {
            while rows[index].count < width { rows[index].append("") }
            rows[index].append("")
        }
        commit()
    }

    private func removeRow() {
        guard rows.count > 1 else { return }
        rows.removeLast()
        commit()
    }

    private func removeColumn() {
        let width = columnCount
        guard width > 1 else { return }
        for index in rows.indices where rows[index].count >= width { rows[index].removeLast() }
        commit()
    }

    private func commit() {
        let width = columnCount
        let padded = rows.map { $0 + Array(repeating: "", count: max(0, width - $0.count)) }
        text = CoveFiles.serializeCsv(padded)
    }

    private struct CellEditor: View {
        let onDone: (String?) -> Void
        @State private var value: String

        init(value: String, onDone: @escaping (String?) -> Void) {
            self.onDone = onDone
            _value = State(initialValue: value)
        }

        var body: some View {
            NavigationStack {
                Form { TextField("Value", text: $value, axis: .vertical).lineLimit(1...8) }
                    .navigationTitle("Edit cell")
                    .navigationBarTitleDisplayMode(.inline)
                    .toolbar {
                        ToolbarItem(placement: .cancellationAction) { Button("Cancel") { onDone(nil) } }
                        ToolbarItem(placement: .confirmationAction) { Button("Done") { onDone(value) } }
                    }
            }
            .presentationDetents([.medium])
        }
    }
}

struct CovePipelineEditor: View {
    @Binding var text: String
    let actor: String
    @State private var pipeline: CovePipeline?
    @State private var problem: String?
    @State private var editingLead: CovePipelineLead?
    @State private var stageFilter: String?

    var body: some View {
        Group {
            if let pipeline {
                List {
                    Section {
                        ScrollView(.horizontal, showsIndicators: false) {
                            HStack(spacing: 8) {
                                ForEach(pipeline.totals()) { total in
                                    stageChip(total)
                                }
                            }
                            .padding(.vertical, 4)
                        }
                    }
                    Section("Leads") {
                        ForEach(visibleLeads(pipeline)) { lead in
                            HStack {
                                Button {
                                    editingLead = lead
                                } label: {
                                    VStack(alignment: .leading, spacing: 2) {
                                        Text(lead.name).foregroundStyle(.primary)
                                        Text([lead.company, lead.owner].filter { !$0.isEmpty }.joined(separator: " \u{00B7} "))
                                            .font(.caption).foregroundStyle(.secondary)
                                    }
                                }
                                .buttonStyle(.plain)
                                Spacer()
                                Menu(pipeline.stageName(lead.stage)) {
                                    ForEach(pipeline.stages) { stage in
                                        Button(stage.name) { mutate { $0.move(leadId: lead.id, to: stage.id, actor: actor) } }
                                    }
                                }
                                .font(.caption)
                            }
                            .swipeActions {
                                Button("Delete", role: .destructive) { mutate { $0.delete(leadId: lead.id, actor: actor) } }
                            }
                        }
                        Button("Add lead", systemImage: "plus") {
                            var added: CovePipelineLead?
                            mutate { added = $0.addLead(actor: actor) }
                            editingLead = added
                        }
                    }
                    if !pipeline.history.isEmpty {
                        Section("History") {
                            ForEach(Array(pipeline.history.suffix(20).reversed().enumerated()), id: \.offset) { _, entry in
                                Text(historyLine(entry, pipeline)).font(.caption)
                            }
                        }
                    }
                }
            } else {
                VStack(spacing: 12) {
                    ContentUnavailableView {
                        Label("Can't read this pipeline", systemImage: "exclamationmark.triangle")
                    } description: {
                        Text(problem ?? "")
                    } actions: {
                        FmMessageTools("Can't read this pipeline. \(problem ?? "")")
                    }
                    TextEditor(text: $text).font(.system(.caption, design: .monospaced)).frame(minHeight: 200).padding(.horizontal)
                }
            }
        }
        .onAppear(perform: parse)
        .sheet(item: $editingLead) { lead in
            LeadEditor(lead: lead) { updated in
                editingLead = nil
                if let updated { mutate { $0.update(updated) } }
            }
        }
    }

    private func stageChip(_ total: CovePipelineTotal) -> some View {
        let selected: Bool = stageFilter == total.stage.id
        let tint: Color = Color(hex: total.stage.color) ?? Color.secondary
        let amount: String = total.value.formatted(.number.precision(.fractionLength(0...2)))
        return Button {
            stageFilter = selected ? nil : total.stage.id
        } label: {
            VStack(alignment: .leading, spacing: 2) {
                Text(total.stage.name).font(.caption.weight(.semibold))
                Text("\(total.count) \u{00B7} \(amount)")
                    .font(.caption2).foregroundStyle(.secondary)
            }
            .padding(8)
            .frame(minHeight: 44)
            .background(
                RoundedRectangle(cornerRadius: 8)
                    .stroke(tint, lineWidth: selected ? 2 : 1))
        }
        .buttonStyle(.plain)
        .accessibilityAddTraits(selected ? .isSelected : [])
    }

    private func visibleLeads(_ pipeline: CovePipeline) -> [CovePipelineLead] {
        guard let stageFilter else { return pipeline.leads }
        return pipeline.leads.filter { $0.stage == stageFilter }
    }

    private func historyLine(_ entry: JSONValue, _ pipeline: CovePipeline) -> String {
        let lead = entry["lead"]?.stringValue ?? ""
        let by = entry["by"]?.stringValue ?? ""
        let to = pipeline.stageName(entry["to"]?.stringValue ?? "")
        switch entry["action"]?.stringValue ?? "" {
        case "created": return "\(by) added \(lead)"
        case "deleted": return "\(by) removed \(lead)"
        default: return "\(by) moved \(lead) to \(to)"
        }
    }

    private func parse() {
        switch CovePipeline.parse(text) {
        case .success(let parsed):
            pipeline = parsed
            problem = nil
        case .failure(let error):
            pipeline = nil
            problem = error.description
        }
    }

    private func mutate(_ change: (inout CovePipeline) -> Void) {
        guard var current = pipeline else { return }
        change(&current)
        pipeline = current
        text = current.body
    }

    private struct LeadEditor: View {
        let onDone: (CovePipelineLead?) -> Void
        @State private var lead: CovePipelineLead
        @State private var value: String

        init(lead: CovePipelineLead, onDone: @escaping (CovePipelineLead?) -> Void) {
            self.onDone = onDone
            _lead = State(initialValue: lead)
            _value = State(initialValue: lead.value == 0 ? "" : String(lead.value))
        }

        var body: some View {
            NavigationStack {
                Form {
                    TextField("Lead", text: $lead.name)
                    TextField("Company", text: $lead.company)
                    TextField("Value", text: $value).keyboardType(.decimalPad)
                    TextField("Owner", text: $lead.owner).textInputAutocapitalization(.never)
                }
                .navigationTitle("Lead")
                .navigationBarTitleDisplayMode(.inline)
                .toolbar {
                    ToolbarItem(placement: .cancellationAction) { Button("Cancel") { onDone(nil) } }
                    ToolbarItem(placement: .confirmationAction) {
                        Button("Done") {
                            var updated = lead
                            updated.value = Double(value.replacingOccurrences(of: ",", with: "")) ?? 0
                            onDone(updated)
                        }
                    }
                }
            }
        }
    }
}

private extension Color {
    init?(hex: String) {
        let clean = hex.trimmingCharacters(in: CharacterSet(charactersIn: "# "))
        guard clean.count == 6, let value = UInt32(clean, radix: 16) else { return nil }
        self.init(
            red: Double((value >> 16) & 0xFF) / 255, green: Double((value >> 8) & 0xFF) / 255, blue: Double(value & 0xFF) / 255)
    }
}
