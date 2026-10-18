import FMAssistant
import SwiftUI

struct ShellAutoSendPresenter: ViewModifier {
    @State private var autoSend: ShellAutoSend = ShellAutoSend.shared

    func body(content: Content) -> some View {
        content
            .task { await autoSend.run() }
            .sheet(item: $autoSend.review, onDismiss: { autoSend.next() }) { (receipt: GenieAutoSendReceipt) in
                ShellAutoSendReview(receipt: receipt)
            }
    }
}

struct ShellAutoSendReview: View {
    @Environment(\.fmPalette) private var fm
    @Environment(\.dismiss) private var dismiss
    let receipt: GenieAutoSendReceipt
    private let autoSend: ShellAutoSend = ShellAutoSend.shared
    @State private var rating: Int = 0
    @State private var note: String = ""
    @State private var saving: Bool = false
    @State private var failure: String = ""

    var body: some View {
        NavigationStack {
            ScrollView {
                VStack(alignment: .leading, spacing: FmSpace.x3) {
                    header
                    Text(receipt.subject)
                        .font(.headline)
                        .foregroundStyle(fm.textPrimary)
                        .textSelection(.enabled)
                        .accessibilityIdentifier("auto-send-review-subject")
                    Text(receipt.text)
                        .font(.body)
                        .foregroundStyle(fm.textPrimary)
                        .textSelection(.enabled)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .accessibilityIdentifier("auto-send-review-body")
                    Divider()
                    stars
                    TextField("Adjust the next ones (optional)", text: $note, axis: .vertical)
                        .lineLimit(2...4)
                        .textFieldStyle(.roundedBorder)
                        .accessibilityIdentifier("auto-send-review-note")
                    if !failure.isEmpty {
                        Text(failure)
                            .font(.footnote)
                            .foregroundStyle(fm.danger)
                    }
                }
                .padding(FmSpace.x4)
            }
            .navigationTitle(title)
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Skip") { dismiss() }
                        .accessibilityIdentifier("auto-send-review-skip")
                }
                ToolbarItem(placement: .confirmationAction) {
                    Button("Save") { save() }
                        .disabled(rating == 0 || saving)
                        .accessibilityIdentifier("auto-send-review-save")
                }
            }
        }
        .presentationDetents([.medium, .large])
        .accessibilityIdentifier("auto-send-review")
    }

    private var title: String {
        autoSend.waiting > 0 ? "Sent · \(autoSend.waiting) more" : "Sent"
    }

    private var header: some View {
        VStack(alignment: .leading, spacing: 2) {
            Text("To \(receipt.contactName) <\(receipt.to)>")
                .font(.subheadline.weight(.semibold))
                .foregroundStyle(fm.textPrimary)
            Text(details)
                .font(.caption)
                .foregroundStyle(fm.textSecondary)
        }
        .accessibilityElement(children: .combine)
    }

    private var details: String {
        var parts: [String] = []
        if !receipt.fromEmail.isEmpty { parts.append("From \(receipt.fromEmail)") }
        if !receipt.viaLabel.isEmpty { parts.append("via \(receipt.viaLabel)") }
        if !receipt.model.isEmpty { parts.append("written by \(receipt.model)") }
        parts.append(autoSend.sentToday == 1 ? "1 sent today" : "\(autoSend.sentToday) sent today")
        return parts.joined(separator: " · ")
    }

    private var stars: some View {
        HStack(spacing: FmSpace.x2) {
            ForEach(1...5, id: \.self) { (value: Int) in
                Button {
                    FmHaptics.tap()
                    rating = value
                } label: {
                    Image(systemName: value <= rating ? "star.fill" : "star")
                        .font(.title2)
                        .foregroundStyle(value <= rating ? fm.accent : fm.textTertiary)
                        .frame(minWidth: 44, minHeight: 44)
                        .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
                .accessibilityLabel(Text(value == 1 ? "1 star" : "\(value) stars"))
                .accessibilityAddTraits(value == rating ? .isSelected : [])
            }
        }
        .accessibilityIdentifier("auto-send-review-stars")
    }

    private func save() {
        saving = true
        failure = ""
        let trimmed: String = note.trimmingCharacters(in: .whitespacesAndNewlines)
        Task { @MainActor in
            let error: String? = await autoSend.rate(receipt, rating: rating, note: trimmed)
            saving = false
            if let error {
                failure = error
                return
            }
            FmHaptics.success()
            dismiss()
        }
    }
}
