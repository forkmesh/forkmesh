// Small pieces the Tools screens share: palette colours for the core's tones, sheet boxes, a
// labelled text field, a text prompt dialog and a date/time picker sheet.

import FMTools
import SwiftUI

extension ToolsTone {
    func color(_ fm: FmPalette) -> Color {
        switch self {
        case .tertiary: return fm.textTertiary
        case .secondary: return fm.textSecondary
        case .success: return fm.success
        case .warning: return fm.warning
        case .danger: return fm.danger
        case .accent: return fm.accent
        }
    }
}

extension LogCategoryInk {
    func color(_ fm: FmPalette) -> Color {
        switch self {
        case .tertiary: return fm.textTertiary
        case .danger: return fm.danger
        case .warning: return fm.warning
        case .primary: return fm.textPrimary
        }
    }
}

/// Wraps any value as a `.sheet(item:)` item.
struct ToolsSheet<Value>: Identifiable {
    let id: UUID = UUID()
    let value: Value

    init(_ value: Value) {
        self.value = value
    }
}

/// A label above an FmTextField, as the Material forms with `labelText` read.
struct ToolsLabeledField: View {
    let label: String
    @Binding var text: String
    var hint: String = ""
    var secure: Bool = false
    var multiline: Bool = false
    var mono: Bool = false

    var body: some View {
        FmField(label: label) {
            FmTextField(text: $text, hint: hint, secure: secure, multiline: multiline, mono: mono)
        }
    }
}

/// `showDialog` with one text field: returns the typed text (Cancel returns nothing).
struct ToolsPromptDialog: View {
    let title: String
    let hint: String
    let action: String
    let multiline: Bool
    @Binding var text: String
    let onCancel: () -> Void
    let onSubmit: (String) -> Void

    var body: some View {
        FmDialog(
            title: title,
            content: {
                FmTextField(text: $text, hint: hint, multiline: multiline, autofocus: true)
            },
            actions: {
                FmDialogActions(onCancel: onCancel, onConfirm: { onSubmit(text) }, confirmLabel: action)
            }
        )
    }
}

/// A sheet with a graphical date picker (or a wheel time picker) and Done.
struct ToolsDateSheet: View {
    let title: String
    let range: ClosedRange<Date>?
    let timeOnly: Bool
    let onDone: (Date) -> Void
    @State private var date: Date
    @Environment(\.dismiss) private var dismiss

    init(title: String, initial: Date, range: ClosedRange<Date>? = nil, timeOnly: Bool = false, onDone: @escaping (Date) -> Void) {
        self.title = title
        self.range = range
        self.timeOnly = timeOnly
        self.onDone = onDone
        _date = State(initialValue: initial)
    }

    @ViewBuilder private var picker: some View {
        if timeOnly {
            DatePicker("", selection: $date, displayedComponents: [.hourAndMinute])
                .datePickerStyle(.wheel)
                .labelsHidden()
        } else if let range = range {
            DatePicker("", selection: $date, in: range, displayedComponents: [.date])
                .datePickerStyle(.graphical)
                .labelsHidden()
        } else {
            DatePicker("", selection: $date, displayedComponents: [.date])
                .datePickerStyle(.graphical)
                .labelsHidden()
        }
    }

    var body: some View {
        VStack(alignment: .leading, spacing: FmSpace.x3) {
            FmPanelHeader(title: title, trailing: FmTextButton("Done", onPressed: {
                onDone(date)
                dismiss()
            }).fmAny)
            picker
                .padding(.horizontal, FmSpace.x4)
        }
        .padding(.bottom, FmSpace.x4)
        .fmSheetStyle()
    }
}

/// Children left to right, onto a new line when the row is full.
struct ToolsFlowLayout: Layout {
    var spacing: CGFloat
    var lineSpacing: CGFloat

    func sizeThatFits(proposal: ProposedViewSize, subviews: Subviews, cache: inout ()) -> CGSize {
        let width: CGFloat = proposal.width ?? CGFloat.infinity
        let rows: [[CGSize]] = arrange(width: width, subviews: subviews)
        var height: CGFloat = 0
        var widest: CGFloat = 0
        for (index, row) in rows.enumerated() {
            let rowHeight: CGFloat = row.map { $0.height }.max() ?? 0
            height += rowHeight + (index > 0 ? lineSpacing : 0)
            let rowWidth: CGFloat = row.reduce(0) { $0 + $1.width } + spacing * CGFloat(max(0, row.count - 1))
            widest = max(widest, rowWidth)
        }
        return CGSize(width: proposal.width ?? widest, height: height)
    }

    func placeSubviews(in bounds: CGRect, proposal: ProposedViewSize, subviews: Subviews, cache: inout ()) {
        var x: CGFloat = bounds.minX
        var y: CGFloat = bounds.minY
        var rowHeight: CGFloat = 0
        for subview in subviews {
            let size: CGSize = subview.sizeThatFits(.unspecified)
            if x > bounds.minX && x + size.width > bounds.maxX {
                x = bounds.minX
                y += rowHeight + lineSpacing
                rowHeight = 0
            }
            subview.place(at: CGPoint(x: x, y: y), proposal: ProposedViewSize(size))
            x += size.width + spacing
            rowHeight = max(rowHeight, size.height)
        }
    }

    private func arrange(width: CGFloat, subviews: Subviews) -> [[CGSize]] {
        var rows: [[CGSize]] = [[]]
        var x: CGFloat = 0
        for subview in subviews {
            let size: CGSize = subview.sizeThatFits(.unspecified)
            if x > 0 && x + size.width > width {
                rows.append([])
                x = 0
            }
            rows[rows.count - 1].append(size)
            x += size.width + spacing
        }
        return rows
    }
}
