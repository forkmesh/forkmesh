import FMFiles
import Foundation
import Observation
import PhotosUI
import SwiftUI
import UniformTypeIdentifiers

struct FileTransferState {
    var title: String
    var fraction: Double?
}

@MainActor
@Observable
final class FolderModel {
    let source: FileSource
    var path: String
    var entries: [FileEntry] = []
    var loaded = false
    var loading = false
    var error: String?
    var notice: String?
    var noticeIsError = false
    var filter = ""
    var transfer: FileTransferState?
    var ascending = true

    @ObservationIgnored private var transferTask: Task<Void, Never>?
    let library = FilesLibrary.shared

    init(source: FileSource, path: String = "") {
        self.source = source
        self.path = FileBrowsing.clean(path)
    }

    var visible: [FileEntry] {
        FileBrowsing.sorted(FileBrowsing.visible(entries, showHidden: library.showHidden, filter: filter), by: library.sort, ascending: ascending)
    }

    var crumbs: [(title: String, path: String)] { FileBrowsing.crumbs(path, root: source.title) }

    var title: String { path.isEmpty ? source.title : FileBrowsing.name(path) }

    func show(_ text: String, error: Bool = false) {
        notice = text
        noticeIsError = error
    }

    func message(_ error: Error) -> String {
        (error as? LocalizedError)?.errorDescription ?? error.localizedDescription
    }

    func load() async {
        loading = true
        defer { loading = false }
        do {
            entries = try await source.list(path)
            error = nil
        } catch {
            entries = []
            self.error = message(error)
        }
        loaded = true
    }

    func go(_ target: String) async {
        path = FileBrowsing.clean(target)
        filter = ""
        loaded = false
        await load()
    }

    func up() async {
        guard !path.isEmpty else { return }
        await go(FileBrowsing.parent(path))
    }

    private func run(_ success: String?, _ work: () async throws -> Void) async {
        do {
            try await work()
            if let success { show(success) }
        } catch is CancellationError {
            show("Cancelled.")
        } catch {
            show(message(error), error: true)
        }
        await load()
    }

    func makeFolder(_ name: String) async {
        let trimmed = name.trimmingCharacters(in: .whitespaces)
        if let problem = FileBrowsing.nameProblem(trimmed) { return show(problem, error: true) }
        await run(nil) { try await source.makeFolder(FileBrowsing.join(path, trimmed)) }
    }

    func makeFile(_ name: String) async {
        let trimmed = name.trimmingCharacters(in: .whitespaces)
        if let problem = FileBrowsing.nameProblem(trimmed) { return show(problem, error: true) }
        await run(nil) { try await source.makeFile(FileBrowsing.join(path, trimmed)) }
    }

    func rename(_ entry: FileEntry, to name: String) async {
        let trimmed = name.trimmingCharacters(in: .whitespaces)
        if let problem = FileBrowsing.nameProblem(trimmed) { return show(problem, error: true) }
        guard trimmed != entry.name else { return }
        await run(nil) { try await source.rename(entry.path, to: FileBrowsing.join(FileBrowsing.parent(entry.path), trimmed)) }
    }

    func delete(_ entry: FileEntry) async {
        await run("\(entry.name) deleted.") { try await source.delete(entry.path, isFolder: entry.isFolder) }
    }

    func copy(_ entry: FileEntry, mode: FileTransferMode) {
        library.clipboard = FileClipboard(entry: entry, source: source, mode: mode)
        show(mode == .copy ? "\(entry.name) copied. Open a folder and choose Paste." : "\(entry.name) will move. Open a folder and choose Paste.")
    }

    func paste() {
        guard let clip = library.clipboard else { return }
        startTransfer(clip.mode == .copy ? "Copying \(clip.entry.name)…" : "Moving \(clip.entry.name)…") { [self] progress in
            try await FileTransfer.paste(clip.entry, from: clip.source, to: source, into: path, mode: clip.mode, progress: progress)
            if clip.mode == .move { library.clipboard = nil }
            show(clip.mode == .copy ? "\(clip.entry.name) copied here." : "\(clip.entry.name) moved here.")
        }
    }

    func upload(_ urls: [URL]) {
        guard !urls.isEmpty else { return }
        startTransfer(urls.count == 1 ? "Uploading \(urls[0].lastPathComponent)…" : "Uploading \(urls.count) files…") { [self] progress in
            var existing = Set(try await source.list(path).map(\.name))
            for url in urls {
                let granted = url.startAccessingSecurityScopedResource()
                defer { if granted { url.stopAccessingSecurityScopedResource() } }
                let name = FileBrowsing.uniqueName(url.lastPathComponent, existing: existing)
                existing.insert(name)
                try await source.upload(url, to: FileBrowsing.join(path, name), progress: progress)
            }
            show(urls.count == 1 ? "\(urls[0].lastPathComponent) uploaded." : "\(urls.count) files uploaded.")
        }
    }

    func uploadPhotos(_ items: [PhotosPickerItem]) {
        guard !items.isEmpty else { return }
        startTransfer(items.count == 1 ? "Uploading a photo…" : "Uploading \(items.count) photos…") { [self] progress in
            var existing = Set(try await source.list(path).map(\.name))
            let stamp = Self.photoStamp.string(from: Date())
            for (index, item) in items.enumerated() {
                try Task.checkCancellation()
                guard let data = try await item.loadTransferable(type: Data.self) else { continue }
                let ext = item.supportedContentTypes.first?.preferredFilenameExtension ?? "jpg"
                let base = items.count == 1 ? "Photo \(stamp).\(ext)" : "Photo \(stamp) \(index + 1).\(ext)"
                let name = FileBrowsing.uniqueName(base, existing: existing)
                existing.insert(name)
                let temp = FileTransfer.temporaryURL(name)
                defer { FileTransfer.discard(temp) }
                try data.write(to: temp)
                try await source.upload(temp, to: FileBrowsing.join(path, name), progress: progress)
            }
            show(items.count == 1 ? "Photo uploaded." : "\(items.count) photos uploaded.")
        }
    }

    static let photoStamp: DateFormatter = {
        let formatter = DateFormatter()
        formatter.dateFormat = "yyyy-MM-dd HHmmss"
        return formatter
    }()

    func exportURL(_ entry: FileEntry, done: @escaping @MainActor (URL) -> Void) {
        if let local = source as? LocalFileSource, let url = try? local.url(entry.path) {
            done(url)
            return
        }
        startTransfer("Downloading \(entry.name)…", reload: false) { [self] progress in
            let folder = FileManager.default.temporaryDirectory.appendingPathComponent("files-export-\(UUID().uuidString)", isDirectory: true)
            try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
            let target = folder.appendingPathComponent(entry.name)
            try await source.download(entry.path, to: target, progress: progress)
            done(target)
        }
    }

    func startTransfer(_ title: String, reload: Bool = true, _ work: @escaping @MainActor (@escaping FileProgress) async throws -> Void) {
        transferTask?.cancel()
        transfer = FileTransferState(title: title, fraction: nil)
        transferTask = Task { @MainActor [self] in
            let progress: FileProgress = { done, total in
                Task { @MainActor [weak self] in
                    guard total > 0 else { return }
                    self?.transfer?.fraction = min(1, Double(done) / Double(total))
                }
            }
            do {
                try await work(progress)
            } catch is CancellationError {
                show("Cancelled.")
            } catch {
                show(message(error), error: true)
            }
            transfer = nil
            if reload { await load() }
        }
    }

    func cancelTransfer() {
        transferTask?.cancel()
    }

    func info(_ entry: FileEntry) -> [(String, String)] {
        var rows: [(String, String)] = [("Name", entry.name), ("Kind", kindTitle(entry))]
        if !entry.isFolder { rows.append(("Size", FileBrowsing.formatSize(entry.size))) }
        if let modified = entry.modified { rows.append(("Modified", modified.formatted(date: .abbreviated, time: .shortened))) }
        rows.append(("Location", source.describe(entry.path)))
        return rows
    }

    func kindTitle(_ entry: FileEntry) -> String {
        switch entry.kind {
        case .folder: return "Folder"
        case .text: return "Text"
        case .image: return "Image"
        case .pdf: return "PDF document"
        case .other:
            let ext = FileKinds.pathExtension(entry.name)
            return ext.isEmpty ? "File" : ext.uppercased() + " file"
        }
    }
}
