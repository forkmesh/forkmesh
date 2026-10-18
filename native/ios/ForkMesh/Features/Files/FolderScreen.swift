import FMFiles
import PhotosUI
import SwiftUI
import UIKit
import UniformTypeIdentifiers

struct FolderScreen: View {
    @State private var model: FolderModel
    @State private var library = FilesLibrary.shared
    @State private var opening: FileEntry?
    @State private var naming: NamePrompt?
    @State private var nameText = ""
    @State private var deleting: FileEntry?
    @State private var inspecting: FileEntry?
    @State private var uploading = false
    @State private var photoItems: [PhotosPickerItem] = []
    @State private var choosingPhotos = false
    @State private var sharing: SharedFile?

    init(source: FileSource, path: String = "") {
        _model = State(initialValue: FolderModel(source: source, path: path))
    }

    enum NamePrompt: Identifiable {
        case folder, file, rename(FileEntry)

        var id: String {
            switch self {
            case .folder: return "folder"
            case .file: return "file"
            case .rename(let entry): return "rename:" + entry.path
            }
        }

        var title: String {
            switch self {
            case .folder: return "New folder"
            case .file: return "New file"
            case .rename: return "Rename"
            }
        }
    }

    var body: some View {
        List {
            breadcrumb
            if let notice = model.notice {
                Text(notice)
                    .font(.callout)
                    .foregroundStyle(model.noticeIsError ? Color.red : Color.secondary)
                    .onTapGesture { model.notice = nil }
            }
            if let clip = library.clipboard {
                Button {
                    model.paste()
                } label: {
                    Label("Paste \(clip.entry.name) here", systemImage: "doc.on.clipboard")
                }
                .disabled(model.transfer != nil)
                .accessibilityIdentifier("files-paste")
            }
            if let error = model.error {
                ContentUnavailableView("Cannot open this folder", systemImage: "exclamationmark.triangle", description: Text(error))
            } else if model.loaded && model.visible.isEmpty {
                Text(model.filter.isEmpty ? FileBrowsing.emptyFolder : "Nothing here matches \"\(model.filter)\".")
                    .foregroundStyle(.secondary)
            }
            ForEach(model.visible) { entry in
                row(entry)
            }
        }
        .listStyle(.plain)
        .navigationTitle(model.title)
        .navigationBarTitleDisplayMode(.inline)
        .searchable(text: $model.filter, prompt: "Filter this folder")
        .refreshable { await model.load() }
        .task { if !model.loaded { await model.load() } }
        .overlay { if model.loading && !model.loaded { ProgressView() } }
        .safeAreaInset(edge: .bottom) { transferBar }
        .toolbar { toolbar }
        .navigationDestination(item: $opening) { entry in
            FileOpenScreen(source: model.source, entry: entry)
        }
        .alert(naming?.title ?? "", isPresented: Binding(get: { naming != nil }, set: { if !$0 { naming = nil } })) {
            TextField("Name", text: $nameText)
                .textInputAutocapitalization(.never)
                .autocorrectionDisabled()
            Button("Save") { submitName() }
            Button("Cancel", role: .cancel) { naming = nil }
        }
        .confirmationDialog(deleting.map(FileBrowsing.deleteQuestion) ?? "", isPresented: Binding(get: { deleting != nil }, set: { if !$0 { deleting = nil } }), titleVisibility: .visible) {
            Button("Delete", role: .destructive) {
                if let entry = deleting { Task { await model.delete(entry) } }
            }
        }
        .sheet(item: $inspecting) { entry in
            NavigationStack {
                List(model.info(entry), id: \.0) { row in
                    LabeledContent(row.0) { Text(row.1).textSelection(.enabled).multilineTextAlignment(.trailing) }
                }
                .navigationTitle("Info")
                .navigationBarTitleDisplayMode(.inline)
                .toolbar { ToolbarItem(placement: .confirmationAction) { Button("Done") { inspecting = nil } } }
            }
            .presentationDetents([.medium])
        }
        .sheet(item: $sharing) { file in
            ActivitySheet(items: [file.url])
        }
        .fileImporter(isPresented: $uploading, allowedContentTypes: [.item], allowsMultipleSelection: true) { result in
            if case .success(let urls) = result { model.upload(urls) }
        }
        .photosPicker(isPresented: $choosingPhotos, selection: $photoItems, matching: .any(of: [.images, .videos]))
        .onChange(of: photoItems) { (_: [PhotosPickerItem], items: [PhotosPickerItem]) in
            guard !items.isEmpty else { return }
            model.uploadPhotos(items)
            photoItems = []
        }
    }

    @ViewBuilder private var breadcrumb: some View {
        if !model.path.isEmpty {
            ScrollView(.horizontal, showsIndicators: false) {
                HStack(spacing: 4) {
                    ForEach(Array(model.crumbs.enumerated()), id: \.offset) { index, crumb in
                        if index > 0 { Image(systemName: "chevron.right").font(.caption2).foregroundStyle(.secondary) }
                        Button(crumb.title) { Task { await model.go(crumb.path) } }
                            .buttonStyle(.borderless)
                            .font(.footnote)
                            .disabled(index == model.crumbs.count - 1)
                    }
                }
            }
            .accessibilityIdentifier("files-breadcrumb")
        }
    }

    private func row(_ entry: FileEntry) -> some View {
        Button {
            if entry.isFolder {
                Task { await model.go(entry.path) }
            } else {
                opening = entry
            }
        } label: {
            HStack(spacing: 12) {
                Image(systemName: FileKinds.symbol(entry))
                    .foregroundStyle(entry.isFolder ? Color.accentColor : Color.secondary)
                    .frame(width: 24)
                VStack(alignment: .leading, spacing: 2) {
                    Text(entry.name)
                        .foregroundStyle(entry.hidden ? Color.secondary : Color.primary)
                        .lineLimit(2)
                    Text(detail(entry))
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
                Spacer(minLength: 0)
                if entry.isFolder { Image(systemName: "chevron.right").font(.caption).foregroundStyle(.tertiary) }
            }
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .accessibilityIdentifier("files-row-\(entry.name)")
        .contextMenu { actions(entry) }
        .swipeActions(edge: .trailing) {
            Button("Delete", role: .destructive) { deleting = entry }
            Button("Rename") {
                nameText = entry.name
                naming = .rename(entry)
            }
        }
        .swipeActions(edge: .leading) {
            if !entry.isFolder {
                Button(model.source.isNetwork ? "Download" : "Share") { share(entry) }.tint(.accentColor)
            }
        }
    }

    private func detail(_ entry: FileEntry) -> String {
        var parts: [String] = []
        if !entry.isFolder { parts.append(FileBrowsing.formatSize(entry.size)) }
        if let modified = entry.modified { parts.append(modified.formatted(date: .abbreviated, time: .shortened)) }
        return parts.isEmpty ? (entry.isFolder ? "Folder" : "") : parts.joined(separator: " · ")
    }

    @ViewBuilder private func actions(_ entry: FileEntry) -> some View {
        Button {
            if entry.isFolder { Task { await model.go(entry.path) } } else { opening = entry }
        } label: { Label("Open", systemImage: "arrow.up.forward.app") }
        Button {
            nameText = entry.name
            naming = .rename(entry)
        } label: { Label("Rename", systemImage: "pencil") }
        Button { model.copy(entry, mode: .copy) } label: { Label("Copy", systemImage: "doc.on.doc") }
        Button { model.copy(entry, mode: .move) } label: { Label("Move", systemImage: "folder") }
        if !entry.isFolder {
            if model.source.isNetwork {
                Button { share(entry) } label: { Label("Download", systemImage: "arrow.down.circle") }
            }
            Button { share(entry) } label: { Label("Share", systemImage: "square.and.arrow.up") }
        }
        Button { inspecting = entry } label: { Label("Info", systemImage: "info.circle") }
        Button(role: .destructive) { deleting = entry } label: { Label("Delete", systemImage: "trash") }
    }

    private func share(_ entry: FileEntry) {
        model.exportURL(entry) { url in sharing = SharedFile(url: url) }
    }

    @ToolbarContentBuilder private var toolbar: some ToolbarContent {
        ToolbarItemGroup(placement: .topBarTrailing) {
            if !model.path.isEmpty {
                Button { Task { await model.up() } } label: { Image(systemName: "arrow.up") }
                    .accessibilityLabel("Up")
                    .accessibilityIdentifier("files-up")
            }
            Menu {
                Button {
                    nameText = ""
                    naming = .folder
                } label: { Label("New folder", systemImage: "folder.badge.plus") }
                Button {
                    nameText = "Untitled.txt"
                    naming = .file
                } label: { Label("New file", systemImage: "doc.badge.plus") }
                Button { uploading = true } label: { Label("Upload files", systemImage: "square.and.arrow.down") }
                Button { choosingPhotos = true } label: { Label("Upload photos", systemImage: "photo.on.rectangle") }
                if library.clipboard != nil {
                    Button { model.paste() } label: { Label("Paste", systemImage: "doc.on.clipboard") }
                }
                Divider()
                Picker("Sort", selection: $library.sort) {
                    ForEach(FileSort.allCases, id: \.self) { Text($0.title).tag($0) }
                }
                Toggle(isOn: $model.ascending) { Label("Ascending", systemImage: "arrow.up.arrow.down") }
                Toggle(isOn: $library.showHidden) { Label("Show hidden files", systemImage: "eye") }
                Button { Task { await model.load() } } label: { Label("Refresh", systemImage: "arrow.clockwise") }
            } label: {
                Image(systemName: "ellipsis.circle")
            }
            .accessibilityLabel("Folder actions")
            .accessibilityIdentifier("files-menu")
        }
    }

    @ViewBuilder private var transferBar: some View {
        if let transfer = model.transfer {
            HStack(spacing: 12) {
                VStack(alignment: .leading, spacing: 4) {
                    Text(transfer.title).font(.footnote).lineLimit(1)
                    if let fraction = transfer.fraction {
                        ProgressView(value: fraction)
                    } else {
                        ProgressView().progressViewStyle(.linear)
                    }
                }
                Button("Cancel") { model.cancelTransfer() }
                    .accessibilityIdentifier("files-transfer-cancel")
            }
            .padding(12)
            .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 12))
            .padding(.horizontal)
        }
    }

    private func submitName() {
        guard let prompt = naming else { return }
        naming = nil
        let name = nameText
        Task {
            switch prompt {
            case .folder: await model.makeFolder(name)
            case .file: await model.makeFile(name)
            case .rename(let entry): await model.rename(entry, to: name)
            }
        }
    }
}

struct SharedFile: Identifiable {
    let url: URL
    var id: String { url.path }
}

struct ActivitySheet: UIViewControllerRepresentable {
    let items: [Any]

    func makeUIViewController(context: Context) -> UIActivityViewController {
        UIActivityViewController(activityItems: items, applicationActivities: nil)
    }

    func updateUIViewController(_ controller: UIActivityViewController, context: Context) {}
}
