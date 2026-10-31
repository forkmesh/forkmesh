// Compose and file one bug report (`POST /api/bug-reports`). One required field, it says what it
// attaches, and a refusal is shown in the sheet with the text still there. State: FMTools'
// ReportBugModel.

import FMAccount
import FMApi
import FMBase
import FMTools
import SwiftUI
import UniformTypeIdentifiers

struct ReportBugSheet: View {
    @Environment(\.fmPalette) private var fm
    @Environment(\.dismiss) private var dismiss
    @State private var model: ReportBugModel
    @State private var importing = false
    @State private var inbox: BugReportInboxModel?
    @State private var tab = 0
    private let onClose: (() -> Void)?
    private let onFinished: ((Bool) -> Void)?

    /// `screen` is where in the app the report was opened from; `onFinished` hears whether a
    /// report was filed once the sheet closes itself.
    init(
        screen: String = "", package: String = "", packageName: String = "", onClose: (() -> Void)? = nil,
        onFinished: ((Bool) -> Void)? = nil
    ) {
        let build: AppBuild = Services.get(AppConfig.self)?.build
            ?? AppBuild(infoDictionary: Bundle.main.infoDictionary)
        let signedIn: Bool = Services.get(AuthService.self)?.isAuthenticated ?? false
        _model = State(initialValue: ReportBugModel(
            screen: screen, package: package, packageName: packageName, build: build, signedIn: signedIn,
            api: Services.get(ApiService.self)))
        let admin = package.isEmpty && (Services.get(AuthService.self)?.session?.isAdmin ?? false)
        _inbox = State(initialValue: admin ? BugReportInboxModel(api: Services.get(ApiService.self)) : nil)
        self.onClose = onClose
        self.onFinished = onFinished
    }

    var body: some View {
        VStack(spacing: 0) {
            header
            if let inbox {
                FmSegmentTabBar(
                    tabs: [FmSegmentTab(label: BugReportInboxModel.tabReport),
                           FmSegmentTab(label: BugReportInboxModel.tabReported, count: inbox.counts["new"])],
                    index: tab, onChanged: { tab = $0 })
                    .accessibilityIdentifier("report-bug-tabs")
            }
            if let inbox, tab == 1 {
                BugReportInboxView(model: inbox)
                    .frame(maxHeight: .infinity, alignment: .top)
            } else {
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
        }
        .fmPreGlassBackground(fm.wallpaper ? fm.canvas : fm.bgRaised)
        .presentationDetents([.large])
        .presentationDragIndicator(.hidden)
        .interactiveDismissDisabled(model.busy)
        .accessibilityIdentifier("report-bug-sheet")
        .task { if let inbox, !inbox.loaded { await inbox.reload() } }
        .onChange(of: inbox?.counts) { (_: [String: Int]?, counts: [String: Int]?) in
            if let counts { ShellNavigatorCounts.shared.adoptBugReports(counts) }
        }
        .fileImporter(isPresented: $importing, allowedContentTypes: [.item], allowsMultipleSelection: true) { result in
            guard case let .success(urls) = result else { return }
            let files = urls.prefix(5 - model.attachments.count).compactMap { url -> BugReportAttachment? in
                let scoped = url.startAccessingSecurityScopedResource()
                defer { if scoped { url.stopAccessingSecurityScopedResource() } }
                guard let data = try? Data(contentsOf: url), !data.isEmpty, data.count <= 4 * 1024 * 1024 else { return nil }
                let contentType = UTType(filenameExtension: url.pathExtension)?.preferredMIMEType ?? ""
                return BugReportAttachment(name: url.lastPathComponent, contentType: contentType, data: data)
            }
            model.attachments += files
        }
    }

    private var header: some View {
        HStack(alignment: .center, spacing: FmSpace.x3) {
            VStack(alignment: .leading, spacing: 2) {
                Text(model.heading)
                    .fmText(.titleLarge)
                Text(model.intro)
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
            FmTextField(text: $model.message, hint: model.hint, multiline: true, autofocus: true)
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
            FmToneButton(label: "Add files", tone: .neutral, filled: false, busy: false,
                         onPressed: model.busy || model.attachments.count >= 5 ? nil : { importing = true })
            if !model.attachments.isEmpty {
                Text(model.attachments.map(\.name).joined(separator: " · "))
                    .fmText(.bodySmall, color: fm.textTertiary)
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
