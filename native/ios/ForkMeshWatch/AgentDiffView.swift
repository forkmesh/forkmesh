import FMWatchWire
import SwiftUI
import WatchKit

struct AgentDiffView: View {
    let key: String
    @Environment(WatchStore.self) private var store
    @State private var diff: WatchDiff?
    @State private var problem: String?
    @State private var loading = false

    var body: some View {
        List {
            if let problem {
                Text(problem)
                    .font(.footnote)
                    .foregroundStyle(.secondary)
            }
            if let diff {
                DiffSummary(diff: diff)
                if diff.files.isEmpty {
                    Text(diff.emptyNote)
                        .font(.footnote)
                        .foregroundStyle(.secondary)
                }
                ForEach(diff.sections) { (section: WatchDiffSection) in
                    Section(diff.sections.count > 1 ? section.title : "Files") {
                        ForEach(section.files) { (file: WatchDiffFile) in
                            NavigationLink {
                                DiffFileView(diff: diff, startID: file.id)
                            } label: {
                                DiffFileRow(file: file, reviewed: store.isReviewed(file, in: diff))
                            }
                        }
                    }
                }
                if diff.hiddenFiles > 0 || diff.truncated {
                    Text(diff.hiddenFiles > 0
                        ? "\(diff.hiddenFiles) more files are on your iPhone."
                        : "This diff is large. The rest is on your iPhone.")
                        .font(.caption2)
                        .foregroundStyle(.secondary)
                }
            } else if loading {
                HStack {
                    ProgressView()
                    Text("Asking your iPhone…").font(.footnote)
                }
            }
        }
        .navigationTitle("Changes")
        .refreshable { await load() }
        .task { await load() }
    }

    private func load() async {
        if loading { return }
        loading = true
        defer { loading = false }
        let next = await store.diff(key)
        if next.ok {
            diff = next
            problem = nil
        } else {
            problem = next.message
        }
    }
}

private struct DiffSummary: View {
    let diff: WatchDiff
    @Environment(WatchStore.self) private var store

    var body: some View {
        VStack(alignment: .leading, spacing: 2) {
            if !diff.branchLine.isEmpty {
                Text(diff.branchLine)
                    .font(.caption2)
                    .lineLimit(2)
            }
            HStack(spacing: 6) {
                DiffCounts(added: diff.added, removed: diff.removed)
                Text(diff.files.count == 1 ? "1 file" : "\(diff.files.count) files")
                    .font(.caption2)
                    .foregroundStyle(.secondary)
            }
            if !diff.files.isEmpty {
                let done = diff.files.filter { store.isReviewed($0, in: diff) }.count
                Text(done == diff.files.count ? "All reviewed" : "\(done) of \(diff.files.count) reviewed")
                    .font(.caption2)
                    .foregroundStyle(done == diff.files.count ? Color.green : Color.secondary)
            }
        }
        .listRowBackground(Color.clear)
    }
}

private struct DiffCounts: View {
    let added: Int
    let removed: Int

    var body: some View {
        HStack(spacing: 4) {
            Text("+\(added)").foregroundStyle(.green)
            Text("\u{2212}\(removed)").foregroundStyle(.red)
        }
        .font(.caption2.monospacedDigit())
    }
}

private struct DiffFileRow: View {
    let file: WatchDiffFile
    let reviewed: Bool

    var body: some View {
        HStack(spacing: 6) {
            Text(file.status)
                .font(.system(size: 11, weight: .bold, design: .monospaced))
                .foregroundStyle(DiffColors.status(file.status))
                .frame(width: 14)
            VStack(alignment: .leading, spacing: 1) {
                Text(file.name)
                    .font(.footnote)
                    .lineLimit(1)
                    .truncationMode(.middle)
                if !file.folder.isEmpty {
                    Text(file.folder)
                        .font(.caption2)
                        .foregroundStyle(.secondary)
                        .lineLimit(1)
                        .truncationMode(.head)
                }
                DiffCounts(added: file.added, removed: file.removed)
            }
            Spacer(minLength: 0)
            if reviewed {
                Image(systemName: "checkmark.circle.fill")
                    .foregroundStyle(.green)
                    .accessibilityLabel("Reviewed")
            }
        }
        .opacity(reviewed ? 0.6 : 1)
    }
}

struct DiffFileView: View {
    let diff: WatchDiff
    @State private var fileID: String
    @Environment(WatchStore.self) private var store

    init(diff: WatchDiff, startID: String) {
        self.diff = diff
        _fileID = State(initialValue: startID)
    }

    private var file: WatchDiffFile? { diff.files.first { $0.id == fileID } }

    var body: some View {
        Group {
            if let file {
                ScrollViewReader { (proxy: ScrollViewProxy) in
                    ScrollView {
                        LazyVStack(alignment: .leading, spacing: 0) {
                            DiffFileHeader(file: file)
                                .id("top")
                            ForEach(file.lines.indices, id: \.self) { (index: Int) in
                                DiffLineRow(line: file.lines[index])
                            }
                            if file.hiddenLines > 0 {
                                Text("\(file.hiddenLines) more lines are on your iPhone.")
                                    .font(.caption2)
                                    .foregroundStyle(.secondary)
                                    .padding(.top, 6)
                            }
                            if !file.note.isEmpty {
                                Text(file.note)
                                    .font(.caption2)
                                    .foregroundStyle(.secondary)
                                    .padding(.top, 6)
                            }
                        }
                    }
                    .onChange(of: fileID) { (_: String, _: String) in
                        proxy.scrollTo("top", anchor: .top)
                    }
                }
                .navigationTitle(file.name)
                .toolbar {
                    ToolbarItemGroup(placement: .bottomBar) {
                        reviewButton(file)
                        Spacer()
                        if let next = diff.next(after: file.id) {
                            Button {
                                fileID = next.id
                            } label: {
                                Image(systemName: "chevron.down")
                            }
                            .accessibilityLabel("Next file")
                        }
                    }
                }
            } else {
                Text("This file is no longer in the diff.")
                    .font(.footnote)
                    .foregroundStyle(.secondary)
            }
        }
    }

    private func reviewButton(_ file: WatchDiffFile) -> some View {
        let reviewed = store.isReviewed(file, in: diff)
        return Button {
            store.setReviewed(file, in: diff, !reviewed)
            if reviewed {
                WKInterfaceDevice.current().play(.click)
                return
            }
            let allDone = diff.files.allSatisfy { store.isReviewed($0, in: diff) }
            WKInterfaceDevice.current().play(allDone ? .success : .click)
            if let next = diff.files.drop(while: { $0.id != file.id }).dropFirst()
                .first(where: { !store.isReviewed($0, in: diff) })
            {
                fileID = next.id
            }
        } label: {
            Image(systemName: reviewed ? "checkmark.circle.fill" : "checkmark.circle")
        }
        .tint(reviewed ? .green : nil)
        .accessibilityLabel(reviewed ? "Reviewed" : "Mark reviewed")
    }
}

private struct DiffFileHeader: View {
    let file: WatchDiffFile

    var body: some View {
        VStack(alignment: .leading, spacing: 2) {
            if !file.folder.isEmpty {
                Text(file.folder)
                    .font(.caption2)
                    .foregroundStyle(.secondary)
                    .lineLimit(2)
            }
            if !file.oldPath.isEmpty {
                Text("from \(file.oldPath)")
                    .font(.caption2)
                    .foregroundStyle(.secondary)
                    .lineLimit(2)
            }
            HStack(spacing: 6) {
                Text(file.status)
                    .font(.system(size: 11, weight: .bold, design: .monospaced))
                    .foregroundStyle(DiffColors.status(file.status))
                DiffCounts(added: file.added, removed: file.removed)
            }
        }
        .padding(.bottom, 4)
    }
}

private struct DiffLineRow: View {
    let line: WatchDiffLine

    var body: some View {
        switch line.kind {
        case .hunk:
            Text(line.number > 0 ? "Line \(line.number)  \(line.text)" : line.text)
                .font(.system(size: 10, design: .monospaced))
                .foregroundStyle(.cyan)
                .lineLimit(1)
                .frame(maxWidth: .infinity, alignment: .leading)
                .padding(.vertical, 3)
                .padding(.horizontal, 2)
                .background(Color.cyan.opacity(0.12))
                .padding(.top, 4)
        default:
            HStack(alignment: .firstTextBaseline, spacing: 3) {
                Text(line.kind.rawValue)
                    .foregroundStyle(DiffColors.marker(line.kind))
                Text(line.text.isEmpty ? " " : line.text)
                    .foregroundStyle(line.kind == .context ? Color.secondary : Color.primary)
                    .fixedSize(horizontal: false, vertical: true)
            }
            .font(.system(size: 11, design: .monospaced))
            .frame(maxWidth: .infinity, alignment: .leading)
            .padding(.horizontal, 2)
            .background(DiffColors.background(line.kind))
            .accessibilityElement(children: .combine)
            .accessibilityLabel(DiffColors.spoken(line))
        }
    }
}

private enum DiffColors {
    static func status(_ letter: String) -> Color {
        switch letter {
        case "A": return .green
        case "D": return .red
        case "R", "C": return .blue
        default: return .orange
        }
    }

    static func marker(_ kind: WatchDiffLine.Kind) -> Color {
        switch kind {
        case .added: return .green
        case .removed: return .red
        default: return .secondary
        }
    }

    static func background(_ kind: WatchDiffLine.Kind) -> Color {
        switch kind {
        case .added: return Color.green.opacity(0.18)
        case .removed: return Color.red.opacity(0.18)
        default: return .clear
        }
    }

    static func spoken(_ line: WatchDiffLine) -> String {
        switch line.kind {
        case .added: return "Added: \(line.text)"
        case .removed: return "Removed: \(line.text)"
        default: return line.text
        }
    }
}
