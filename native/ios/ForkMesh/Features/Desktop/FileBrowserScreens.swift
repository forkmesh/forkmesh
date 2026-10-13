import FMDesktopRemote
import SwiftUI
#if canImport(UIKit)
import UIKit
#endif

struct FileRootsScreen: View {
    @Bindable var store: FilesBrowserStore

    var body: some View {
        List {
            if store.unsupported {
                Section { DesktopOutdatedRow() }
            }
            ForEach(store.groups) { group in
                Section(group.repo.label) {
                    NavigationLink {
                        DirectoryScreen(store: store, root: group.repo, path: "")
                            .shellPageClearance()
                    } label: {
                        RootRow(root: group.repo)
                    }
                    ForEach(group.worktrees) { worktree in
                        NavigationLink {
                            DirectoryScreen(store: store, root: worktree, path: "")
                                .shellPageClearance()
                        } label: {
                            RootRow(root: worktree)
                        }
                    }
                }
            }
            if store.roots.isEmpty && !store.loadingRoots && store.error.isEmpty {
                Section {
                    Text("The desktop has no local repositories.")
                        .foregroundStyle(.secondary)
                }
            }
            if !store.error.isEmpty && !store.unsupported {
                Section {
                    Label(store.error, systemImage: "exclamationmark.triangle")
                        .foregroundStyle(.red)
                        .font(.footnote)
                        .fmMessageTools(store.error)
                }
            }
        }
        .overlay {
            if store.loadingRoots && store.roots.isEmpty { ProgressView() }
        }
        .navigationTitle("Files")
        .fmRefreshable { await store.loadRoots() }
        .task { await store.loadRoots() }
    }
}

private struct RootRow: View {
    let root: FileRoot

    var body: some View {
        HStack(spacing: 10) {
            Image(systemName: root.kind == .repo ? "folder.fill" : "arrow.triangle.branch")
                .foregroundStyle(root.kind == .repo ? Color.accentColor : Color.purple)
            VStack(alignment: .leading, spacing: 2) {
                Text(root.kind == .repo ? "Main checkout" : root.label)
                    .font(.body)
                if !root.branch.isEmpty {
                    Text(root.branch)
                        .font(.caption)
                        .foregroundStyle(.secondary)
                        .lineLimit(1)
                }
            }
        }
    }
}

struct DirectoryScreen: View {
    @Bindable var store: FilesBrowserStore
    let root: FileRoot
    let path: String

    @State private var listing: FileListing?
    @State private var error = ""
    @State private var filter = ""

    private var entries: [FileEntry] {
        let all = listing?.entries ?? []
        let needle = filter.trimmingCharacters(in: .whitespaces)
        return needle.isEmpty ? all : all.filter { $0.name.localizedCaseInsensitiveContains(needle) }
    }

    var body: some View {
        List {
            if path.isEmpty {
                Section {
                    NavigationLink {
                        GitStatusScreen(store: store, root: root)
                            .shellPageClearance()
                    } label: {
                        Label("Changes and sync", systemImage: "arrow.triangle.2.circlepath")
                    }
                }
            }
            Section {
                ForEach(entries) { entry in
                    entryRow(entry)
                }
                if listing?.truncated == true {
                    Text("Showing \(listing?.entries.count ?? 0) of \(listing?.total ?? 0). Filter to find the rest.")
                        .font(.footnote)
                        .foregroundStyle(.secondary)
                }
            } header: {
                if !path.isEmpty {
                    Text(path).textCase(nil)
                }
            }
            if !error.isEmpty {
                Section {
                    Label(error, systemImage: "exclamationmark.triangle")
                        .foregroundStyle(.red)
                        .font(.footnote)
                        .fmMessageTools(error)
                }
            }
        }
        .overlay {
            if listing == nil && error.isEmpty { ProgressView() }
        }
        .searchable(text: $filter, prompt: "Filter this folder")
        .navigationTitle(path.isEmpty ? root.label : String(path.split(separator: "/").last ?? ""))
        .navigationBarTitleDisplayMode(.inline)
        .fmRefreshable { await load() }
        .task { await load() }
    }

    @ViewBuilder
    private func entryRow(_ entry: FileEntry) -> some View {
        let child = RemotePath.join(path, entry.name)
        if entry.kind.isFolder && entry.canOpen {
            NavigationLink {
                DirectoryScreen(store: store, root: root, path: child)
                    .shellPageClearance()
            } label: {
                FileEntryRow(entry: entry)
            }
        } else if entry.canOpen {
            NavigationLink {
                FileViewerScreen(store: store, root: root, path: child)
                    .shellPageClearance()
            } label: {
                FileEntryRow(entry: entry)
            }
        } else {
            FileEntryRow(entry: entry)
                .foregroundStyle(.secondary)
        }
    }

    private func load() async {
        if listing == nil, let cached = store.cached(root: root.id, path: path) {
            listing = cached
        }
        do {
            listing = try await store.list(root: root.id, path: path)
            error = ""
        } catch {
            self.error = RemoteErrorText.describe(error)
        }
    }
}

private struct FileEntryRow: View {
    let entry: FileEntry

    var body: some View {
        HStack(spacing: 10) {
            Image(systemName: icon)
                .foregroundStyle(entry.kind.isFolder ? Color.accentColor : Color.secondary)
                .frame(width: 22)
            VStack(alignment: .leading, spacing: 2) {
                Text(entry.name).lineLimit(1)
                if entry.escapes {
                    Text("Points outside the repository").font(.caption2).foregroundStyle(.orange)
                } else if entry.sensitive {
                    Text("Looks like a credential; stays on the desktop").font(.caption2).foregroundStyle(.orange)
                }
            }
            Spacer()
            Text(entry.sizeLabel)
                .font(.caption)
                .foregroundStyle(.secondary)
        }
    }

    private var icon: String {
        if entry.sensitive { return "key" }
        switch entry.kind {
        case .dir: return "folder"
        case .dirlink: return "folder.badge.gearshape"
        case .symlink: return "link"
        case .file: return "doc.text"
        }
    }
}

struct FileViewerScreen: View {
    let store: FilesBrowserStore
    let root: FileRoot
    let path: String

    @State private var text = ""
    @State private var imageData: Data?
    @State private var binary = false
    @State private var size: Double = 0
    @State private var loaded: Double = 0
    @State private var eof = true
    @State private var loading = false
    @State private var error = ""
    @State private var wrap = true

    private let maxLoaded: Double = 4 * 1024 * 1024

    private var textAxes: Axis.Set { wrap ? .vertical : [.vertical, .horizontal] }

    var body: some View {
        Group {
            if let imageData, let image = platformImage(imageData) {
                ScrollView {
                    image
                        .resizable()
                        .scaledToFit()
                        .padding()
                }
            } else if binary {
                ContentUnavailableView(
                    "Binary file", systemImage: "doc.zipper",
                    description: Text("\(RemoteFormat.bytes(size)). It cannot be shown as text."))
            } else {
                ScrollView(textAxes) {
                    VStack(alignment: .leading, spacing: 12) {
                        Text(text)
                            .font(.system(.caption, design: .monospaced))
                            .textSelection(.enabled)
                            .fixedSize(horizontal: !wrap, vertical: false)
                            .frame(maxWidth: .infinity, alignment: .leading)
                        if !eof {
                            Button {
                                Task { await loadMore() }
                            } label: {
                                HStack {
                                    Text("Load more (\(RemoteFormat.bytes(loaded)) of \(RemoteFormat.bytes(size)))")
                                    if loading { ProgressView() }
                                }
                            }
                            .disabled(loading || loaded >= maxLoaded)
                        }
                    }
                    .padding()
                }
            }
        }
        .overlay {
            if loading && text.isEmpty && imageData == nil { ProgressView() }
            if !error.isEmpty {
                ContentUnavailableView {
                    Label("Could not open the file", systemImage: "exclamationmark.triangle")
                } description: {
                    Text(error)
                } actions: {
                    FmMessageTools(error)
                }
            }
        }
        .navigationTitle(String(path.split(separator: "/").last ?? ""))
        .navigationBarTitleDisplayMode(.inline)
        .toolbar {
            ToolbarItem(placement: .primaryAction) {
                Menu {
                    Toggle("Wrap lines", isOn: $wrap)
                    if !text.isEmpty {
                        ShareLink(item: text, subject: Text(path))
                    }
                } label: {
                    Image(systemName: "ellipsis.circle")
                }
            }
        }
        .task { await loadFirst() }
    }

    private func loadFirst() async {
        loading = true
        defer { loading = false }
        do {
            let slice = try await store.read(root: root.id, path: path)
            apply(slice, append: false)
        } catch {
            self.error = RemoteErrorText.describe(error)
        }
    }

    private func loadMore() async {
        loading = true
        defer { loading = false }
        do {
            let slice = try await store.read(root: root.id, path: path, offset: loaded)
            apply(slice, append: true)
        } catch {
            self.error = RemoteErrorText.describe(error)
        }
    }

    private func apply(_ slice: FileSlice, append: Bool) {
        size = slice.size
        loaded = slice.nextOffset
        eof = slice.eof
        if slice.binary {
            if !append, slice.eof, RemotePath.isImage(path), let data = slice.data {
                imageData = data
            } else {
                binary = true
            }
            return
        }
        text = append ? text + slice.text : slice.text
    }

    private func platformImage(_ data: Data) -> Image? {
        #if canImport(UIKit)
        guard let image = UIImage(data: data) else { return nil }
        return Image(uiImage: image)
        #else
        return nil
        #endif
    }
}

struct GitStatusScreen: View {
    @Bindable var store: FilesBrowserStore
    let root: FileRoot

    @State private var status: GitWorkingStatus?
    @State private var error = ""
    @State private var pending: PendingConfirmation?
    @State private var preparing: GitRemoteAction?

    var body: some View {
        List {
            if let status {
                Section {
                    LabeledContent("Branch", value: status.branch.isEmpty ? "—" : status.branch)
                    Text(status.syncLine).font(.subheadline)
                    Text(status.changesLine).font(.subheadline).foregroundStyle(.secondary)
                }
                Section {
                    ForEach(GitRemoteAction.allCases) { action in
                        Button {
                            Task { await prepare(action) }
                        } label: {
                            HStack {
                                Text(action.title)
                                Spacer()
                                if preparing == action { ProgressView() }
                            }
                        }
                        .disabled(preparing != nil || (action == .pull && status.behind == 0 && !status.upstream.isEmpty))
                    }
                } footer: {
                    Text("Pull only fast-forwards. Staging, committing and pushing stay on the desktop.")
                }
                if !status.files.isEmpty {
                    Section("Changes") {
                        ForEach(status.files) { change in
                            HStack(spacing: 10) {
                                Text(change.kind.letter)
                                    .font(.system(.caption, design: .monospaced).weight(.bold))
                                    .foregroundStyle(color(change.kind))
                                    .frame(width: 18)
                                VStack(alignment: .leading, spacing: 1) {
                                    Text(change.path).font(.caption).lineLimit(2)
                                    if !change.from.isEmpty {
                                        Text("from \(change.from)").font(.caption2).foregroundStyle(.secondary)
                                    }
                                }
                                Spacer()
                                if change.isStaged {
                                    Text("staged").font(.caption2).foregroundStyle(.green)
                                }
                            }
                        }
                        if status.truncated {
                            Text("Showing \(status.files.count) of \(status.total).")
                                .font(.footnote)
                                .foregroundStyle(.secondary)
                        }
                    }
                }
            }
            let outcomes = store.outcomes.filter { $0.subject == root.id }
            if !outcomes.isEmpty {
                Section("From this phone") {
                    ForEach(outcomes) { RemoteOutcomeRow(outcome: $0) }
                }
            }
            if !error.isEmpty {
                Section {
                    Label(error, systemImage: "exclamationmark.triangle")
                        .foregroundStyle(.red)
                        .font(.footnote)
                        .fmMessageTools(error)
                }
            }
        }
        .overlay {
            if status == nil && error.isEmpty { ProgressView() }
        }
        .navigationTitle(root.label)
        .navigationBarTitleDisplayMode(.inline)
        .fmRefreshable { await load() }
        .task { await load() }
        .onChange(of: store.changedRoots.contains(root.id)) { _, changed in
            if changed { Task { await load() } }
        }
        .sheet(item: $pending) { item in
            NavigationStack {
                RemoteConfirmSheet(title: item.title, confirmation: item.confirmation) { confirmation, _ in
                    try await store.run(confirmation)
                }
            }
        }
    }

    private func load() async {
        do {
            status = try await store.status(root: root.id)
            error = ""
        } catch {
            self.error = RemoteErrorText.describe(error)
        }
    }

    private func prepare(_ action: GitRemoteAction) async {
        preparing = action
        defer { preparing = nil }
        do {
            let confirmation = try await store.prepare(action, root: root.id)
            pending = PendingConfirmation(title: action.title, confirmation: confirmation, destructive: false)
        } catch {
            self.error = RemoteErrorText.describe(error)
        }
    }

    private func color(_ kind: GitChangeKind) -> Color {
        switch kind {
        case .added, .untracked: return .green
        case .deleted: return .red
        case .conflicted: return .orange
        default: return .blue
        }
    }
}
