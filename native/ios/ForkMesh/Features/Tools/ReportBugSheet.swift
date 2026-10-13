// Compose and file one bug report (`POST /api/bug-reports`). One required field, it says what it
// attaches, and a refusal is shown in the sheet with the text still there. State: FMTools'
// ReportBugModel.

import FMAccount
import FMApi
import FMBase
import FMTools
import SwiftUI

struct ReportBugSheet: View {
    @Environment(\.fmPalette) private var fm
    @Environment(\.dismiss) private var dismiss
    @State private var model: ReportBugModel
    private let onClose: (() -> Void)?
    private let onFinished: ((Bool) -> Void)?

    /// `screen` is where in the app the report was opened from; `onFinished` hears whether a
    /// report was filed once the sheet closes itself.
    init(screen: String = "", onClose: (() -> Void)? = nil, onFinished: ((Bool) -> Void)? = nil) {
        let build: AppBuild = Services.get(AppConfig.self)?.build
            ?? AppBuild(infoDictionary: Bundle.main.infoDictionary)
        let signedIn: Bool = Services.get(AuthService.self)?.isAuthenticated ?? false
        _model = State(initialValue: ReportBugModel(
            screen: screen, build: build, signedIn: signedIn, api: Services.get(ApiService.self)))
        self.onClose = onClose
        self.onFinished = onFinished
    }

    var body: some View {
        VStack(spacing: 0) {
            header
            ScrollView {
                form
                    .padding(.horizontal, FmSpace.x4)
                    .padding(.bottom, FmSpace.x4)
            }
            .scrollDismissesKeyboard(.interactively)
            FmBottomActionBar(padding: EdgeInsets.fmSymmetric(horizontal: FmSpace.x4, vertical: FmSpace.x3)) {
                sendButton
            }
        }
        .fmPreGlassBackground(fm.bgRaised)
        .presentationDetents([.large])
        .presentationDragIndicator(.hidden)
        .interactiveDismissDisabled(model.busy)
        .accessibilityIdentifier("report-bug-sheet")
    }

    private var header: some View {
        HStack(alignment: .center, spacing: FmSpace.x3) {
            VStack(alignment: .leading, spacing: 2) {
                Text(ReportBugModel.title)
                    .fmText(.titleLarge)
                Text(ReportBugModel.subtitle)
                    .fmText(.bodySmall, color: fm.textTertiary)
            }
            .frame(maxWidth: .infinity, alignment: .leading)
            FmIconButton(icon: "close", label: "Close", onPressed: model.busy ? nil : { close(filed: false) })
        }
        .padding(.leading, FmSpace.x4)
        .padding(.trailing, FmSpace.x3)
        .padding(.top, FmSpace.x4)
        .padding(.bottom, FmSpace.x3)
    }

    private var form: some View {
        VStack(alignment: .leading, spacing: FmSpace.x4) {
            FmTextField(text: $model.message, hint: ReportBugModel.messageHint, multiline: true, autofocus: true)
                .frame(minHeight: 96, alignment: .top)
                .disabled(model.busy)
                .accessibilityIdentifier("report-bug-message")
            FmField(label: ReportBugModel.severityLabel) {
                severityRow
            }
            FmField(label: model.contactLabel) {
                FmTextField(text: $model.contact, hint: model.contactHint, onSubmit: send)
                    .keyboardType(.emailAddress)
                    .textInputAutocapitalization(.never)
                    .autocorrectionDisabled(true)
                    .disabled(model.busy)
            }
            attached
            if !model.problem.isEmpty {
                problemNote
            }
        }
    }

    private var severityRow: some View {
        HStack(spacing: FmSpace.x2) {
            ForEach(BugSeverity.all, id: \.value) { (severity: BugSeverity) in
                FmToneButton(
                    label: severity.label,
                    tone: severity.value == model.severity ? .accent : .neutral,
                    filled: severity.value == model.severity,
                    onPressed: model.busy ? nil : { model.severity = severity.value })
                    .accessibilityIdentifier("report-bug-severity-\(severity.value)")
            }
        }
    }

    private var attached: some View {
        HStack(alignment: .top, spacing: FmSpace.x2) {
            FmIcon("info_outline_rounded", size: 15, color: fm.textDisabled)
            Text(model.attached)
                .fmText(.bodySmall, color: fm.textTertiary)
                .frame(maxWidth: .infinity, alignment: .leading)
        }
    }

    private var problemNote: some View {
        HStack(alignment: .top, spacing: FmSpace.x2) {
            FmIcon("error_outline_rounded", size: 16, color: fm.danger)
            Text(model.problem)
                .fmText(.bodySmall, color: fm.textPrimary)
                .frame(maxWidth: .infinity, alignment: .leading)
                .fmMessageTools(model.problem)
        }
        .padding(FmSpace.x3)
        .background(fm.danger.opacity(0.12))
        .clipShape(FmRoundedShape(radius: FmRadius.md))
        .accessibilityIdentifier("report-bug-problem")
    }

    private var sendButton: some View {
        FmFilledButton(
            model.submitLabel, icon: model.busy ? nil : "send_rounded",
            background: fm.accent, foreground: Color.white, minHeight: 48, radius: FmRadius.lg,
            fillWidth: true, role: .titleSmall,
            onPressed: model.busy ? nil : send)
            .accessibilityIdentifier("report-bug-submit")
    }

    private func send() {
        Task {
            if await model.submit() {
                close(filed: true)
            }
        }
    }

    private func close(filed: Bool) {
        onFinished?(filed)
        if let onClose = onClose {
            onClose()
        } else {
            dismiss()
        }
    }
}
