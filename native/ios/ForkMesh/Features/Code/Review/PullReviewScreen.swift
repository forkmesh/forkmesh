import FMApi
import FMBase
import FMCode
import FMDiff
import SwiftUI

struct PullReviewScreen: View {
    let repo: RepoRef
    let number: Int
    @State private var session: PullReviewSession
    @State private var notice: CollabNotice?
    @State private var finishing = false

    init(repo: RepoRef, number: Int) {
        self.repo = repo
        self.number = number
        _session = State(initialValue: PullReviewSession(
            owner: repo.owner, name: repo.name, number: number, reader: ReviewEnv.api,
            writer: ReviewEnv.writer(owner: repo.owner, name: repo.name)))
    }

    var body: some View {
        content
            .navigationTitle("Review #\(number)")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .topBarTrailing) {
                    Button {
                        finishing = true
                    } label: {
                        Text(session.drafts.isEmpty ? "Review" : "Review (\(session.drafts.count))")
                    }
                    .disabled(!session.canWrite || session.record == nil)
                }
            }
            .sheet(isPresented: $finishing) {
                FinishReviewSheet(session: session) { result in
                    notice = result
                }
            }
            .task {
                if session.record == nil { await session.load() }
            }
            .fmRefreshable {
                await session.load()
            }
    }

    @ViewBuilder private var content: some View {
        if let record = session.record {
            List {
                Section {
                    Text(record.title).font(.headline)
                    HStack {
                        Text("\(record.head.isEmpty ? "patch" : record.head) → \(record.base)")
                            .font(.footnote.monospaced())
                            .foregroundStyle(.secondary)
                        Spacer()
                        Text(session.viewedSummary).font(.caption).foregroundStyle(.secondary)
                    }
                    reviewSummaryRow
                    if !session.canWrite {
                        Label("This device has no signing key, so it can read the review but not comment.", systemImage: "key")
                            .font(.footnote)
                            .foregroundStyle(.secondary)
                            .frame(maxWidth: .infinity, alignment: .leading)
                            .fmMessageTools("This device has no signing key, so it can read the review but not comment.")
                    }
                    ReviewNoticeBanner(notice: notice)
                }
                Section(changedFilesTitle(session.files.count)) {
                    if session.files.isEmpty {
                        Text(PullDetail.noPatch).foregroundStyle(.secondary)
                            .frame(maxWidth: .infinity, alignment: .leading)
                            .fmMessageTools(PullDetail.noPatch)
                    }
                    ForEach(session.files.indices, id: \.self) { (index: Int) in
                        let file = session.files[index]
                        NavigationLink {
                            ReviewFileScreen(session: session, repo: repo, path: file.path)
                                .shellPageClearance()
                        } label: {
                            fileRow(file)
                        }
                    }
                }
                let fileless = session.snapshot.threads.filter { $0.path.isEmpty }
                if !fileless.isEmpty {
                    Section("Other threads") {
                        ForEach(fileless) { (thread: ReviewThread) in
                            ReviewThreadCard(session: session, repo: repo, thread: thread) { notice = $0 }
                        }
                    }
                }
            }
            .listStyle(.insetGrouped)
        } else if let error = session.loadError {
            ContentUnavailableView {
                Label("Could not load the review", systemImage: "exclamationmark.triangle")
            } description: {
                Text(error)
            } actions: {
                Button("Try again") { Task { await session.load() } }
                FmMessageTools(error)
            }
        } else {
            ProgressView("Loading the pull request…")
                .frame(maxWidth: .infinity, maxHeight: .infinity)
        }
    }

    private var reviewSummaryRow: some View {
        let snap = session.snapshot
        return HStack(spacing: 12) {
            Label("\(snap.unresolvedThreads) open", systemImage: "bubble.left.and.bubble.right")
            if snap.resolvedThreads > 0 {
                Label("\(snap.resolvedThreads) resolved", systemImage: "checkmark.bubble")
            }
            if snap.outdatedThreads > 0 {
                Label("\(snap.outdatedThreads) outdated", systemImage: "clock.arrow.circlepath")
            }
            Spacer()
            if snap.reviewSummary == "approved" {
                Label("Approved", systemImage: "checkmark.seal").foregroundStyle(.green)
            } else if snap.reviewSummary == "changes_requested" {
                Label("Changes requested", systemImage: "xmark.octagon").foregroundStyle(.red)
            }
        }
        .font(.caption)
        .labelStyle(.titleAndIcon)
    }

    private func fileRow(_ file: DiffFile) -> some View {
        let summary = session.snapshot.files[file.path]
        let drafts = session.drafts.drafts(inFile: file.path).count
        return HStack {
            Image(systemName: session.viewed.contains(file.path) ? "checkmark.circle.fill" : "doc.text")
                .foregroundStyle(session.viewed.contains(file.path) ? Color.green : Color.secondary)
            VStack(alignment: .leading, spacing: 2) {
                Text(DiffFileRowText.name(file)).font(.body.monospaced()).lineLimit(1)
                let directory = DiffFileRowText.directory(file)
                if !directory.isEmpty {
                    Text(directory).font(.caption.monospaced()).foregroundStyle(.secondary).lineLimit(1)
                }
            }
            Spacer()
            if let summary, summary.unresolvedThreads > 0 {
                Label("\(summary.unresolvedThreads)", systemImage: "bubble.left").font(.caption)
            }
            if drafts > 0 {
                Label("\(drafts)", systemImage: "pencil").font(.caption).foregroundStyle(.orange)
            }
            Text("+\(file.additions) −\(file.deletions)").font(.caption.monospaced()).foregroundStyle(.secondary)
        }
    }
}

struct ReviewFileScreen: View {
    @Bindable var session: PullReviewSession
    let repo: RepoRef
    let path: String
    @State private var composing = false
    @State private var notice: CollabNotice?

    var body: some View {
        Group {
            if let file = session.file(path) {
                fileBody(file)
            } else {
                ContentUnavailableView("File not in this diff", systemImage: "doc.questionmark")
            }
        }
        .navigationTitle((path as NSString).lastPathComponent)
        .navigationBarTitleDisplayMode(.inline)
        .toolbar {
            ToolbarItemGroup(placement: .topBarTrailing) {
                Button {
                    session.toggleViewed(path)
                } label: {
                    Image(systemName: session.viewed.contains(path) ? "checkmark.circle.fill" : "circle")
                }
                .accessibilityLabel(session.viewed.contains(path) ? "Mark as not viewed" : "Mark as viewed")
            }
        }
        .safeAreaInset(edge: .bottom) {
            if let selection = session.selection, selection.path == path {
                HStack {
                    Text(selection.label).font(.footnote.monospaced()).lineLimit(1)
                    Spacer()
                    Button("Clear") { session.clearSelection() }
                    Button("Comment") { composing = true }
                        .buttonStyle(.borderedProminent)
                        .disabled(!session.canWrite)
                }
                .padding(.horizontal, 16)
                .padding(.vertical, 10)
                .shellGlass(Capsule())
                .padding(.horizontal, FmSpace.x2)
                .padding(.vertical, FmSpace.x1)
            }
        }
        .sheet(isPresented: $composing) {
            InlineCommentSheet(session: session) { notice = $0 }
        }
    }

    private func fileBody(_ file: DiffFile) -> some View {
        let grouped = session.snapshot.threads(inFile: path)
        let drafts = session.drafts.drafts(inFile: path)
        return ScrollView(.vertical) {
            LazyVStack(alignment: .leading, spacing: 0) {
                ReviewNoticeBanner(notice: notice).padding(12)
                if !grouped.outdated.isEmpty {
                    VStack(alignment: .leading, spacing: 8) {
                        Label("Outdated threads", systemImage: "clock.arrow.circlepath")
                            .font(.caption.weight(.semibold))
                            .foregroundStyle(.secondary)
                        ForEach(grouped.outdated) { (thread: ReviewThread) in
                            ReviewThreadCard(session: session, repo: repo, thread: thread) { notice = $0 }
                        }
                    }
                    .padding(12)
                }
                if file.binary {
                    Text("Binary file").foregroundStyle(.secondary).padding(12)
                }
                ForEach(file.hunks.indices, id: \.self) { (h: Int) in
                    let hunk = file.hunks[h]
                    Text(hunk.header)
                        .font(.caption.monospaced())
                        .foregroundStyle(.secondary)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .padding(.horizontal, 8)
                        .padding(.vertical, 4)
                        .background(Color.secondary.opacity(0.08))
                    ForEach(hunk.lines) { (line: DiffLine) in
                        lineRow(line)
                        ForEach(threadsAfter(line, grouped.byLine)) { (thread: ReviewThread) in
                            ReviewThreadCard(session: session, repo: repo, thread: thread) { notice = $0 }
                                .padding(.horizontal, 8)
                                .padding(.vertical, 4)
                        }
                        ForEach(drafts.filter { draftEndsAt($0, line) }) { (draft: ReviewDraft) in
                            draftCard(draft)
                        }
                    }
                }
            }
        }
    }

    private func anchors(_ line: DiffLine) -> [(DiffSide, Int)] {
        switch line.kind {
        case .removed: return [(.old, line.oldNumber)]
        case .added: return [(.new, line.newNumber)]
        case .context: return [(.new, line.newNumber), (.old, line.oldNumber)]
        }
    }

    private func threadsAfter(_ line: DiffLine, _ byLine: [DiffSide: [Int: [ReviewThread]]]) -> [ReviewThread] {
        anchors(line).flatMap { byLine[$0.0]?[$0.1] ?? [] }
    }

    private func draftEndsAt(_ draft: ReviewDraft, _ line: DiffLine) -> Bool {
        anchors(line).contains { $0.0 == draft.selection.side && $0.1 == draft.selection.lineEnd }
    }

    private func lineRow(_ line: DiffLine) -> some View {
        let selected = session.selection?.contains(line) ?? false
        let background: Color = selected ? Color.accentColor.opacity(0.25)
            : line.kind == .added ? Color.green.opacity(0.12)
            : line.kind == .removed ? Color.red.opacity(0.12) : Color.clear
        let marker = line.kind == .added ? "+" : line.kind == .removed ? "−" : " "
        return HStack(alignment: .firstTextBaseline, spacing: 6) {
            Text(line.oldNumber > 0 ? "\(line.oldNumber)" : "")
                .frame(width: 34, alignment: .trailing)
            Text(line.newNumber > 0 ? "\(line.newNumber)" : "")
                .frame(width: 34, alignment: .trailing)
            Text(marker)
            Text(line.text.isEmpty ? " " : line.text)
                .frame(maxWidth: .infinity, alignment: .leading)
        }
        .font(.caption.monospaced())
        .foregroundStyle(.primary)
        .padding(.vertical, 1)
        .padding(.horizontal, 4)
        .background(background)
        .contentShape(Rectangle())
        .onTapGesture {
            session.tap(path: path, line: line)
        }
        .accessibilityAddTraits(.isButton)
        .accessibilityLabel("Line \(line.kind == .removed ? line.oldNumber : line.newNumber), \(line.kind.rawValue): \(line.text)")
    }

    private func draftCard(_ draft: ReviewDraft) -> some View {
        HStack(alignment: .top) {
            Image(systemName: "pencil.circle").foregroundStyle(.orange)
            VStack(alignment: .leading, spacing: 2) {
                Text("Pending · \(draft.selection.isRange ? "lines \(draft.selection.lineStart)–\(draft.selection.lineEnd)" : "line \(draft.selection.lineStart)")")
                    .font(.caption2)
                    .foregroundStyle(.secondary)
                Text(draft.body).font(.footnote)
            }
            Spacer()
            Button(role: .destructive) {
                session.drafts.remove(draft.id)
            } label: {
                Image(systemName: "trash")
            }
            .buttonStyle(.borderless)
            .accessibilityLabel("Discard pending comment")
        }
        .padding(10)
        .background(Color.orange.opacity(0.08), in: RoundedRectangle(cornerRadius: 10))
        .padding(.horizontal, 8)
        .padding(.vertical, 4)
    }
}

struct ReviewThreadCard<Session: ReviewThreadActing>: View {
    let session: Session
    let repo: RepoRef
    let thread: ReviewThread
    let onNotice: (CollabNotice) -> Void
    @State private var replying = false
    @State private var reply = ""
    @State private var expanded = true

    init(session: Session, repo: RepoRef, thread: ReviewThread, onNotice: @escaping (CollabNotice) -> Void) {
        self.session = session
        self.repo = repo
        self.thread = thread
        self.onNotice = onNotice
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack(spacing: 6) {
                Image(systemName: thread.resolved ? "checkmark.bubble" : "bubble.left")
                    .foregroundStyle(thread.resolved ? Color.green : ReviewEnv.placementTint(thread.placement))
                Text(thread.path.isEmpty ? "Thread" : lineLabel)
                    .font(.caption.weight(.semibold))
                if !thread.placementLabel.isEmpty {
                    Text(thread.placementLabel)
                        .font(.caption2)
                        .padding(.horizontal, 6)
                        .padding(.vertical, 2)
                        .background(ReviewEnv.placementTint(thread.placement).opacity(0.15), in: Capsule())
                }
                if thread.hasSuggestion {
                    Text(thread.suggestionState.isEmpty ? "Suggestion" : "Suggestion \(thread.suggestionState)")
                        .font(.caption2)
                        .foregroundStyle(.secondary)
                }
                Spacer()
                Button {
                    expanded.toggle()
                } label: {
                    Image(systemName: expanded ? "chevron.up" : "chevron.down")
                }
                .buttonStyle(.borderless)
                .accessibilityLabel(expanded ? "Collapse thread" : "Expand thread")
            }
            if expanded {
                if thread.isOutdated, !thread.anchorLines.isEmpty {
                    Text(thread.anchorLines)
                        .font(.caption2.monospaced())
                        .foregroundStyle(.secondary)
                        .padding(6)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .background(Color.secondary.opacity(0.08), in: RoundedRectangle(cornerRadius: 6))
                }
                ForEach(thread.events.indices, id: \.self) { (index: Int) in
                    eventRow(thread.events[index])
                }
                if replying {
                    TextField("Reply", text: $reply, axis: .vertical)
                        .lineLimit(2...6)
                        .textFieldStyle(.roundedBorder)
                    HStack {
                        Button("Cancel") {
                            replying = false
                            reply = ""
                        }
                        Spacer()
                        Button("Send") {
                            Task {
                                let result = await session.reply(thread, body: reply)
                                onNotice(result)
                                if !result.isError {
                                    reply = ""
                                    replying = false
                                }
                            }
                        }
                        .buttonStyle(.borderedProminent)
                        .disabled(session.busy || reply.dartTrimmed.isEmpty)
                    }
                } else if session.canWrite && !thread.id.hasPrefix("legacy:") {
                    HStack {
                        Button("Reply") { replying = true }
                        Spacer()
                        Button(thread.resolved ? "Reopen" : "Resolve") {
                            Task { onNotice(await session.setResolved(thread, !thread.resolved)) }
                        }
                        .disabled(session.busy)
                    }
                    .font(.footnote)
                    .buttonStyle(.bordered)
                }
            }
        }
        .padding(10)
        .background(Color.secondary.opacity(0.06), in: RoundedRectangle(cornerRadius: 10))
    }

    private var lineLabel: String {
        let start = thread.isOutdated ? thread.lineStart : thread.currentLineStart
        let end = thread.isOutdated ? thread.lineEnd : thread.currentLineEnd
        if start <= 0 { return "File" }
        return end > start ? "Lines \(start)–\(end)" : "Line \(start)"
    }

    private func eventRow(_ event: ReviewEvent) -> some View {
        VStack(alignment: .leading, spacing: 2) {
            HStack(spacing: 4) {
                Text(event.displayAuthor.isEmpty ? "Someone" : event.displayAuthor)
                    .font(.caption.weight(.semibold))
                    .lineLimit(1)
                if event.type == "thread-state" {
                    Text(event.state == "resolved" ? "resolved the thread" : "reopened the thread")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
                if session.isPending(event) {
                    Text("Pending").font(.caption2).foregroundStyle(.orange)
                }
            }
            if !event.body.isEmpty {
                ReviewMarkdownText(text: event.body, repo: repo).font(.footnote)
            }
            if !event.suggestionPatch.isEmpty {
                Text(event.suggestionPatch)
                    .font(.caption2.monospaced())
                    .padding(6)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .background(Color.green.opacity(0.08), in: RoundedRectangle(cornerRadius: 6))
            }
        }
    }
}

struct InlineCommentSheet: View {
    @Bindable var session: PullReviewSession
    let onNotice: (CollabNotice) -> Void
    @Environment(\.dismiss) private var dismiss
    @State private var body_ = ""
    @State private var suggestion = ""
    @State private var suggesting = false
    @State private var error: String?
    @State private var confirmDiscard = false

    var body: some View {
        NavigationStack {
            Form {
                if let selection = session.selection {
                    Section {
                        Text(selection.label).font(.footnote.monospaced())
                        let anchor = ReviewAnchoring.anchorLines(
                            patch: session.patch, path: selection.path, side: selection.side,
                            lineStart: selection.lineStart, lineEnd: selection.lineEnd)
                        if !anchor.isEmpty {
                            Text(anchor).font(.caption.monospaced()).foregroundStyle(.secondary)
                        }
                    }
                }
                Section("Comment") {
                    TextField("Leave feedback. Markdown works.", text: $body_, axis: .vertical)
                        .lineLimit(4...12)
                }
                Section {
                    Toggle("Suggest a change", isOn: $suggesting)
                    if suggesting {
                        TextField("Replacement lines", text: $suggestion, axis: .vertical)
                            .font(.footnote.monospaced())
                            .lineLimit(3...10)
                    }
                }
                if let error {
                    Text(error).foregroundStyle(.red).font(.footnote)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .fmMessageTools(error)
                }
            }
            .navigationTitle("Comment")
            .navigationBarTitleDisplayMode(.inline)
            .interactiveDismissDisabled(!body_.isEmpty)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Cancel") {
                        if body_.isEmpty { dismiss() } else { confirmDiscard = true }
                    }
                }
                ToolbarItemGroup(placement: .bottomBar) {
                    Button("Add to review") {
                        if let problem = session.addDraft(body: body_, suggestion: suggestionPatch) {
                            error = problem
                        } else {
                            dismiss()
                        }
                    }
                    Spacer()
                    Button("Comment now") {
                        Task {
                            let result = await session.commentNow(body: body_, suggestion: suggestionPatch)
                            if result.isError {
                                error = result.message
                            } else {
                                onNotice(result)
                                dismiss()
                            }
                        }
                    }
                    .buttonStyle(.borderedProminent)
                    .disabled(session.busy)
                }
            }
            .confirmationDialog("Discard this comment?", isPresented: $confirmDiscard) {
                Button("Discard", role: .destructive) { dismiss() }
            }
        }
    }

    private var suggestionPatch: String {
        guard suggesting, let selection = session.selection, !suggestion.isEmpty else { return "" }
        let old = ReviewAnchoring.anchorLines(
            patch: session.patch, path: selection.path, side: selection.side,
            lineStart: selection.lineStart, lineEnd: selection.lineEnd)
        let removed = old.split(separator: "\n", omittingEmptySubsequences: false).map { "-\($0)" }
        let added = suggestion.split(separator: "\n", omittingEmptySubsequences: false).map { "+\($0)" }
        let count = max(1, removed.count)
        return (["@@ -\(selection.lineStart),\(count) +\(selection.lineStart),\(max(1, added.count)) @@"] + removed + added)
            .joined(separator: "\n") + "\n"
    }
}

struct FinishReviewSheet: View {
    @Bindable var session: PullReviewSession
    let onNotice: (CollabNotice) -> Void
    @Environment(\.dismiss) private var dismiss
    @State private var verdict: ReviewVerdict = .comment
    @State private var note = ""
    @State private var error: String?

    var body: some View {
        NavigationStack {
            Form {
                Section {
                    Picker("Review", selection: $verdict) {
                        ForEach(ReviewVerdict.allCases, id: \.self) { (value: ReviewVerdict) in
                            Text(value.title).tag(value)
                        }
                    }
                    .pickerStyle(.segmented)
                    Text(PullReview.composerHelper(verdict.rawValue)).font(.footnote).foregroundStyle(.secondary)
                }
                Section("Summary") {
                    TextField(PullReview.reviewHint, text: $note, axis: .vertical)
                        .lineLimit(3...10)
                }
                Section(session.drafts.summary) {
                    ForEach(session.drafts.drafts) { (draft: ReviewDraft) in
                        VStack(alignment: .leading) {
                            Text(draft.selection.label).font(.caption.monospaced()).foregroundStyle(.secondary)
                            Text(draft.body).font(.footnote)
                        }
                    }
                    .onDelete { offsets in
                        let ids = offsets.map { session.drafts.drafts[$0].id }
                        for id in ids { session.drafts.remove(id) }
                    }
                }
                if let error {
                    Text(error).foregroundStyle(.red).font(.footnote)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .fmMessageTools(error)
                }
            }
            .navigationTitle("Finish review")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Close") { dismiss() }
                }
                ToolbarItem(placement: .confirmationAction) {
                    Button(PullReview.submitLabel) {
                        Task {
                            let result = await session.submit(verdict, body: note)
                            if result.isError {
                                error = result.message
                            } else {
                                FmHaptics.commit()
                                onNotice(result)
                                dismiss()
                            }
                        }
                    }
                    .disabled(session.busy)
                }
            }
        }
    }
}
