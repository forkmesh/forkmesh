import FMPackages
import PhotosUI
import SwiftUI
import UIKit
import UniformTypeIdentifiers

struct SecureDriveFilesView: View {
    let model: SecureDriveModel
    let banner: AnyView
    @State private var query = ""
    @State private var importing = false
    @State private var photos: [PhotosPickerItem] = []
    @State private var choosingPhotos = false
    @State private var renaming: SecureDriveFileEntry?
    @State private var newName = ""
    @State private var deleting: SecureDriveFileEntry?
    @State private var versions: SecureDriveFileEntry?
    @State private var warnExport: SecureDriveFileEntry?
    @State private var exportDocument: SecureDriveExportDocument?
    @State private var exportName = ""
    @State private var exportType: UTType = .data
    @State private var exporting = false

    private var shown: [SecureDriveFileEntry] {
        let trimmed = query.trimmingCharacters(in: .whitespaces)
        if trimmed.isEmpty { return model.snapshot.files }
        return model.snapshot.files.filter { $0.name.localizedCaseInsensitiveContains(trimmed) }
    }

    private var canWrite: Bool { !model.snapshot.removed && !model.busy }

    var body: some View {
        List {
            Section { banner }
            Section {
                if model.snapshot.files.isEmpty && model.snapshot.scanned {
                    Text("No files yet. Add files, or make a new document, spreadsheet or drawing.")
                        .foregroundStyle(.secondary)
                } else if shown.isEmpty && !query.isEmpty {
                    Text("No files match.").foregroundStyle(.secondary)
                }
                ForEach(shown) { (entry: SecureDriveFileEntry) in
                    row(entry)
                }
            }
        }
        .listStyle(.insetGrouped)
        .searchable(text: $query, prompt: "Filter files")
        .refreshable { await model.refresh(force: true) }
        .safeAreaInset(edge: .bottom, spacing: 0) { actions }
        .fileImporter(isPresented: $importing, allowedContentTypes: [UTType.item], allowsMultipleSelection: true) { (result: Result<[URL], Error>) in
            if case .success(let urls) = result { Task { await model.addFiles(urls) } }
        }
        .photosPicker(isPresented: $choosingPhotos, selection: $photos, matching: .any(of: [.images, .videos]))
        .onChange(of: photos) { (_: [PhotosPickerItem], picked: [PhotosPickerItem]) in
            guard !picked.isEmpty else { return }
            photos = []
            Task { await addPhotos(picked) }
        }
        .alert("Rename", isPresented: renameShown) {
            TextField("Name", text: $newName)
            Button("Cancel", role: .cancel) {}
            Button("Rename") {
                if let entry = renaming { Task { await model.rename(entry.id, to: newName) } }
            }
        } message: {
            Text("Use a name without slashes, up to 255 characters.")
        }
        .confirmationDialog("Delete \u{201C}\(deleting?.name ?? "")\u{201D}?", isPresented: deleteShown, titleVisibility: .visible) {
            Button("Delete", role: .destructive) {
                if let entry = deleting { Task { await model.delete(entry.id) } }
            }
        } message: {
            Text("Every version of \(deleting?.name ?? "") is removed from the drive for every device. The activity log keeps a record that it was deleted, and by whom.")
        }
        .alert("Save a decrypted copy?", isPresented: warnShown) {
            Button("Cancel", role: .cancel) {}
            Button("Save") {
                if let entry = warnExport { Task { await prepareExport(entry) } }
            }
        } message: {
            Text("The copy is not encrypted and is not part of the drive: anyone who can read where you save it can read it. The drive's activity log records that this iPhone saved a copy.")
        }
        .fileExporter(isPresented: $exporting, document: exportDocument, contentType: exportType, defaultFilename: exportName) { (result: Result<URL, Error>) in
            exportDocument = nil
            switch result {
            case .success(let url): model.say("Saved a decrypted copy to \(url.lastPathComponent).")
            case .failure: model.say(SecureDriveText.errorText("write_failed"), error: true)
            }
        }
        .sheet(item: $versions) { (entry: SecureDriveFileEntry) in
            SecureDriveVersionsSheet(model: model, fileId: entry.id)
        }
    }

    private func row(_ entry: SecureDriveFileEntry) -> some View {
        Button {
            Task { await model.open(entry.id) }
        } label: {
            HStack(spacing: 12) {
                Image(systemName: SecureDriveEditorText.symbol(entry.kind))
                    .foregroundStyle(.secondary)
                    .frame(width: 28)
                VStack(alignment: .leading, spacing: 2) {
                    Text(entry.name).foregroundStyle(.primary).lineLimit(2)
                    Text(detail(entry))
                        .font(.caption)
                        .foregroundStyle(.secondary)
                        .lineLimit(2)
                }
            }
        }
        .disabled(model.busy)
        .contextMenu { menu(entry) }
        .swipeActions(edge: .trailing) {
            if canWrite {
                Button(role: .destructive) {
                    deleting = entry
                } label: {
                    Label("Delete", systemImage: "trash")
                }
            }
            Button {
                versions = entry
            } label: {
                Label("Versions", systemImage: "clock.arrow.circlepath")
            }
        }
    }

    @ViewBuilder private func menu(_ entry: SecureDriveFileEntry) -> some View {
        Button {
            Task { await model.open(entry.id) }
        } label: {
            Label(SecureDriveEditorText.editable(entry.kind) && canWrite ? "Open to Edit" : "Open", systemImage: "doc.text")
        }
        Button {
            Task { await model.openExternally(entry.id) }
        } label: {
            Label("Open in another app", systemImage: "square.and.arrow.up")
        }
        Button {
            versions = entry
        } label: {
            Label("Versions", systemImage: "clock.arrow.circlepath")
        }
        if canWrite {
            Button {
                newName = entry.name
                renaming = entry
            } label: {
                Label("Rename", systemImage: "pencil")
            }
        }
        Button {
            warnExport = entry
        } label: {
            Label("Save a decrypted copy\u{2026}", systemImage: "square.and.arrow.down")
        }
        if canWrite {
            Button(role: .destructive) {
                deleting = entry
            } label: {
                Label("Delete", systemImage: "trash")
            }
        }
    }

    private func detail(_ entry: SecureDriveFileEntry) -> String {
        var parts = [SecureDriveEditorText.kindLabel(entry.kind)]
        if let current = entry.current { parts.append(ByteCountFormatter.string(fromByteCount: current.size, countStyle: .file)) }
        parts.append(SecureDriveEditorText.plural(entry.versions.count, "version"))
        let when = Date(timeIntervalSince1970: Double(entry.modified) / 1000)
        parts.append("changed \(when.formatted(.relative(presentation: .named))) by \(model.snapshot.name(entry.modifiedBy))")
        var text = parts.joined(separator: " \u{00B7} ")
        if entry.conflicts > 0 {
            text += ". Saved on two devices at once \(SecureDriveEditorText.plural(entry.conflicts, "time")); every version is kept under Versions."
        }
        return text
    }

    private var actions: some View {
        HStack(spacing: 12) {
            Menu {
                Button {
                    Task { await model.newFile(kind: "document") }
                } label: {
                    Label("New Document", systemImage: "doc.richtext")
                }
                Button {
                    Task { await model.newFile(kind: "spreadsheet") }
                } label: {
                    Label("New Spreadsheet", systemImage: "tablecells")
                }
                Button {
                    Task { await model.newFile(kind: "drawing") }
                } label: {
                    Label("New Drawing", systemImage: "scribble.variable")
                }
            } label: {
                Label("New", systemImage: "plus")
                    .frame(maxWidth: .infinity)
            }
            .buttonStyle(.borderedProminent)
            .accessibilityIdentifier("secure-drive-new")
            Menu {
                Button {
                    importing = true
                } label: {
                    Label("From Files", systemImage: "folder")
                }
                Button {
                    choosingPhotos = true
                } label: {
                    Label("From Photos", systemImage: "photo.on.rectangle")
                }
            } label: {
                Label("Add Files", systemImage: "square.and.arrow.down.on.square")
                    .frame(maxWidth: .infinity)
            }
            .buttonStyle(.bordered)
            .accessibilityIdentifier("secure-drive-add")
        }
        .disabled(!canWrite)
        .padding(.horizontal, 16)
        .padding(.vertical, 8)
        .background(.bar)
    }

    private func addPhotos(_ picked: [PhotosPickerItem]) async {
        let stamp = DateFormatter()
        stamp.dateFormat = "yyyy-MM-dd HH.mm.ss"
        for (index, item) in picked.enumerated() {
            guard let data = try? await item.loadTransferable(type: Data.self) else {
                model.say("Some photos could not be read.", error: true)
                continue
            }
            let ext = item.supportedContentTypes.first?.preferredFilenameExtension ?? "jpg"
            let suffix = picked.count > 1 ? " \(index + 1)" : ""
            await model.addData(data, name: "Photo \(stamp.string(from: Date()))\(suffix).\(ext)")
        }
    }

    private func prepareExport(_ entry: SecureDriveFileEntry) async {
        guard let data = await model.exportBytes(entry.id) else { return }
        exportName = entry.name
        exportType = UTType(filenameExtension: (entry.name as NSString).pathExtension) ?? .data
        exportDocument = SecureDriveExportDocument(data: data)
        exporting = true
    }

    private var renameShown: Binding<Bool> {
        Binding(get: { renaming != nil }, set: { if !$0 { renaming = nil } })
    }

    private var deleteShown: Binding<Bool> {
        Binding(get: { deleting != nil }, set: { if !$0 { deleting = nil } })
    }

    private var warnShown: Binding<Bool> {
        Binding(get: { warnExport != nil }, set: { if !$0 { warnExport = nil } })
    }
}

struct SecureDriveVersionsSheet: View {
    let model: SecureDriveModel
    let fileId: String
    @Environment(\.dismiss) private var dismiss
    @State private var restoring: Int?

    var body: some View {
        NavigationStack {
            List {
                if let entry = model.snapshot.file(fileId) {
                    Section {
                        ForEach(Array(entry.versions.enumerated().reversed()), id: \.offset) { (item: (offset: Int, element: SecureDriveFileVersion)) in
                            versionRow(entry, item.offset, item.element)
                        }
                    } footer: {
                        Text("Every save is kept as its own encrypted version until the file is deleted.")
                    }
                } else {
                    Text("That file is no longer on the drive.").foregroundStyle(.secondary)
                }
            }
            .navigationTitle("Versions of \(model.snapshot.file(fileId)?.name ?? "")")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button("Done") { dismiss() }
                }
            }
            .confirmationDialog("Make version \((restoring ?? 0) + 1) the current version?", isPresented: restoreShown, titleVisibility: .visible) {
                Button("Make This the Current Version") {
                    if let index = restoring { Task { await model.restore(fileId, version: index) } }
                }
            } message: {
                Text("The newer versions are still kept.")
            }
        }
    }

    private func versionRow(_ entry: SecureDriveFileEntry, _ index: Int, _ version: SecureDriveFileVersion) -> some View {
        let when = Date(timeIntervalSince1970: Double(version.time) / 1000)
        let latest = index == entry.versions.count - 1
        return VStack(alignment: .leading, spacing: 6) {
            Text("Version \(index + 1)\(latest ? " (current)" : "")").font(.headline)
            Text("\(when.formatted(date: .abbreviated, time: .shortened)) \u{00B7} \(model.snapshot.name(version.device)) \u{00B7} \(ByteCountFormatter.string(fromByteCount: version.size, countStyle: .file))")
                .font(.caption)
                .foregroundStyle(.secondary)
            HStack {
                Button("Open read-only") {
                    dismiss()
                    Task { await model.open(fileId, version: index, readOnly: true) }
                }
                .buttonStyle(.bordered)
                if !latest && !model.snapshot.removed {
                    Button("Make current") { restoring = index }
                        .buttonStyle(.bordered)
                }
            }
            .disabled(model.busy)
        }
        .padding(.vertical, 4)
    }

    private var restoreShown: Binding<Bool> {
        Binding(get: { restoring != nil }, set: { if !$0 { restoring = nil } })
    }
}
