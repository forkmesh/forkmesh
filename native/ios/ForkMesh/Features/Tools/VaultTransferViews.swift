import FMBase
import FMVault
import SwiftUI
import UniformTypeIdentifiers

struct VaultCSVDocument: FileDocument {
    static var readableContentTypes: [UTType] { [.commaSeparatedText] }

    let text: String

    init(text: String) {
        self.text = text
    }

    init(configuration: ReadConfiguration) throws {
        text = configuration.file.regularFileContents.flatMap { String(data: $0, encoding: .utf8) } ?? ""
    }

    func fileWrapper(configuration: WriteConfiguration) throws -> FileWrapper {
        FileWrapper(regularFileWithContents: Data(text.utf8))
    }
}

enum VaultFileImport {
    static let types: [UTType] = [.commaSeparatedText, .zip, .plainText, .text]

    static func read(_ url: URL) throws -> Data {
        let scoped = url.startAccessingSecurityScopedResource()
        defer {
            if scoped { url.stopAccessingSecurityScopedResource() }
        }
        let size = (try? url.resourceValues(forKeys: [.fileSizeKey]).fileSize) ?? 0
        if size > VaultTransfer.maxFileBytes { throw VaultError("csv_too_large") }
        return try Data(contentsOf: url)
    }
}

struct VaultTransferSection: View {
    @Environment(\.fmPalette) private var fm
    @Bindable var model: VaultTransferModel
    @State private var importing = false

    var body: some View {
        VStack(alignment: .leading, spacing: FmSpace.x3) {
            FmSectionHeader(title: VaultTransferModel.header)
            Text(VaultTransferModel.explanation)
                .fmText(.bodySmall, color: fm.textSecondary)
            FmOutlinedButton(VaultTransferModel.importFileButton, icon: "upload_outlined", fillWidth: true, onPressed: model.busy ? nil : {
                importing = true
            })
            .accessibilityIdentifier("vault-import-file")
            FmOutlinedButton(VaultTransferModel.exportFileButton, icon: "download_outlined", fillWidth: true, onPressed: model.busy ? nil : {
                model.requestExport()
            })
            .accessibilityIdentifier("vault-export-file")
            Text(VaultTransferModel.fileHelp)
                .fmText(.bodySmall, color: fm.textSecondary)
            if VaultCredentialExchange.supported {
                FmOutlinedButton(VaultTransferModel.exchangeExportButton, icon: "ios_share", fillWidth: true, onPressed: model.busy ? nil : {
                    Task { await sendToAnotherApp() }
                })
                .accessibilityIdentifier("vault-exchange-export")
                Text(VaultTransferModel.exchangeNote)
                    .fmText(.bodySmall, color: fm.textSecondary)
            }
            if !model.message.isEmpty {
                Text(model.message)
                    .fmText(.bodyMedium)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .accessibilityIdentifier("vault-transfer-message")
                    .fmMessageTools(model.message)
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .fileImporter(isPresented: $importing, allowedContentTypes: VaultFileImport.types) {
            (result: Result<URL, Error>) in
            switch result {
            case .success(let url):
                do {
                    model.prepareFile(try VaultFileImport.read(url))
                } catch {
                    model.fileFailed(error)
                }
            case .failure(let error):
                model.fileFailed(error)
            }
        }
        .fileExporter(
            isPresented: exportShown, document: model.exportFile.map { VaultCSVDocument(text: $0.text) },
            contentType: .commaSeparatedText, defaultFilename: VaultTransfer.exportFileName
        ) { (result: Result<URL, Error>) in
            switch result {
            case .success(let url):
                model.finishExport(savedAs: url.lastPathComponent)
            case .failure(let error):
                model.finishExport(savedAs: nil, failure: error.localizedDescription)
            }
        }
    }

    private var exportShown: Binding<Bool> {
        Binding<Bool>(get: { model.exportFile != nil }, set: { (shown: Bool) in
            if !shown && model.exportFile != nil {
                model.finishExport(savedAs: nil)
            }
        })
    }

    private func sendToAnotherApp() async {
        guard let logins = model.exportLogins() else { return }
        do {
            if let destination = try await VaultCredentialExchange.export(logins) {
                model.exported(logins.count, to: destination)
            }
        } catch {
            model.exportFailed(VaultCredentialExchange.failureText(error))
        }
    }
}

struct VaultImportReviewDialog: View {
    let review: VaultImportReview
    let onCancel: () -> Void
    let onImport: () -> Void

    var body: some View {
        FmConfirmDialog(
            title: review.title, message: review.message, confirmLabel: VaultTransferModel.importButton,
            onCancel: onCancel, onConfirm: onImport)
    }
}

private struct VaultTransferDialogs: ViewModifier {
    @Bindable var model: VaultTransferModel

    func body(content: Content) -> some View {
        content
            .fmDialog(isPresented: $model.confirmingExport) {
                FmConfirmDialog(
                    title: VaultTransferModel.exportConfirmTitle, message: VaultTransferModel.exportConfirmMessage,
                    confirmLabel: VaultTransferModel.exportConfirmButton, destructive: true,
                    onCancel: { model.confirmingExport = false },
                    onConfirm: { model.confirmExport() })
            }
            .fmDialog(isPresented: reviewShown) {
                if let review = model.review {
                    VaultImportReviewDialog(review: review, onCancel: { model.cancelImport() }, onImport: {
                        Task { await model.confirmImport() }
                    })
                }
            }
    }

    private var reviewShown: Binding<Bool> {
        Binding<Bool>(get: { model.review != nil }, set: { (shown: Bool) in
            if !shown {
                model.cancelImport()
            }
        })
    }
}

extension View {
    func vaultTransferDialogs(_ model: VaultTransferModel) -> some View {
        modifier(VaultTransferDialogs(model: model))
    }
}
