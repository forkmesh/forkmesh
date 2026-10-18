import FMPackages
import SwiftUI
import UIKit

struct SecureDriveSheetEditor: View {
    let readOnly: Bool
    let onEdit: ([UInt8]) -> Void
    @State private var sheet: SecureDriveSheet
    @State private var row = 0
    @State private var column = 0
    @State private var formula = ""
    @FocusState private var editing: Bool

    static let cellWidth: CGFloat = 104
    static let cellHeight: CGFloat = 34
    static let headerWidth: CGFloat = 48

    init(sheet: SecureDriveSheet, readOnly: Bool, onEdit: @escaping ([UInt8]) -> Void) {
        self.readOnly = readOnly
        self.onEdit = onEdit
        _sheet = State(initialValue: sheet)
        _formula = State(initialValue: sheet.raw(0, 0))
    }

    private var rowsShown: Int { min(SecureDriveSheet.maxRows, max(sheet.rows + 20, 50)) }
    private var columnsShown: Int { min(SecureDriveSheet.maxColumns, max(sheet.columns + 4, 10)) }
    private var cellName: String { SecureDriveSheet.columnName(column) + String(row + 1) }

    var body: some View {
        VStack(spacing: 0) {
            HStack(spacing: 8) {
                Text(cellName)
                    .font(.callout.monospaced().weight(.semibold))
                    .frame(width: 64, alignment: .leading)
                TextField(readOnly ? "" : "Value or =formula, for example =SUM(B2:B5)", text: $formula)
                    .font(.callout.monospaced())
                    .textInputAutocapitalization(.never)
                    .autocorrectionDisabled()
                    .focused($editing)
                    .disabled(readOnly)
                    .submitLabel(.next)
                    .onSubmit { commit(moveDown: true) }
                if !readOnly && editing {
                    Button("Enter") { commit(moveDown: true) }
                        .buttonStyle(.bordered)
                }
            }
            .padding(.horizontal, 12)
            .padding(.vertical, 8)
            Divider()
            ScrollView([.horizontal, .vertical]) {
                LazyVStack(alignment: .leading, spacing: 0) {
                    HStack(spacing: 0) {
                        headerCell("")
                            .frame(width: Self.headerWidth)
                        ForEach(0..<columnsShown, id: \.self) { (c: Int) in
                            headerCell(SecureDriveSheet.columnName(c))
                                .frame(width: Self.cellWidth)
                        }
                    }
                    ForEach(0..<rowsShown, id: \.self) { (r: Int) in
                        HStack(spacing: 0) {
                            headerCell(String(r + 1))
                                .frame(width: Self.headerWidth)
                            ForEach(0..<columnsShown, id: \.self) { (c: Int) in
                                cell(r, c)
                            }
                        }
                    }
                }
            }
        }
    }

    private func headerCell(_ text: String) -> some View {
        Text(text)
            .font(.caption.monospaced())
            .foregroundStyle(.secondary)
            .frame(height: Self.cellHeight)
            .frame(maxWidth: .infinity)
            .background(Color(uiColor: .secondarySystemBackground))
            .overlay(Rectangle().stroke(Color(uiColor: .separator), lineWidth: 0.5))
    }

    private func cell(_ r: Int, _ c: Int) -> some View {
        let shown = sheet.display(r, c)
        let numeric = Double(shown) != nil
        let error = shown.hasPrefix("#") && shown.hasSuffix("!") || shown == "#NAME?"
        let selected = r == row && c == column
        return Text(shown)
            .font(.callout)
            .lineLimit(1)
            .foregroundStyle(error ? Color.red : Color.primary)
            .padding(.horizontal, 6)
            .frame(width: Self.cellWidth, height: Self.cellHeight, alignment: numeric ? .trailing : .leading)
            .background(selected ? Color.accentColor.opacity(0.15) : Color(uiColor: .systemBackground))
            .overlay(Rectangle().stroke(selected ? Color.accentColor : Color(uiColor: .separator), lineWidth: selected ? 2 : 0.5))
            .contentShape(Rectangle())
            .onTapGesture { select(r, c) }
            .accessibilityLabel("\(SecureDriveSheet.columnName(c))\(r + 1): \(shown)")
    }

    private func select(_ r: Int, _ c: Int) {
        if !readOnly && formula != sheet.raw(row, column) { commit(moveDown: false) }
        row = r
        column = c
        formula = sheet.raw(r, c)
    }

    private func commit(moveDown: Bool) {
        guard !readOnly else { return }
        if formula != sheet.raw(row, column) {
            sheet.setRaw(row, column, formula)
            onEdit(sheet.toCsv())
        }
        if moveDown && row + 1 < SecureDriveSheet.maxRows {
            row += 1
            formula = sheet.raw(row, column)
        }
    }
}
