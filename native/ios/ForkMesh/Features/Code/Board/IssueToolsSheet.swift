import FMApi
import FMBase
import FMCode
import FMCrypto
import LocalAuthentication
import SwiftUI
import UIKit

struct IssueToolsMenu: View {
    let repo: RepoRef
    let issue: Issue
    let onChanged: () -> Void
    @State private var editing = false
    @State private var asking = false
    @State private var linking = false
    @State private var confirmDelete = false
    @State private var message: String?
    @State private var linkNumber = ""

    var body: some View {
        Menu {
            Button { editing = true } label: { Label("Edit fields", systemImage: "slider.horizontal.3") }
            Button { asking = true } label: { Label("Ask AI", systemImage: "sparkles") }
            Button { linking = true } label: { Label("Link a pull request", systemImage: "link") }
            NavigationLink(value: Route.board(repo)) { Label("Board", systemImage: "rectangle.split.3x1") }
            Divider()
            Button {
                collabCopy(IssueThreadText.copyText(repo: repo.slug, issue: issue, includeThread: false), label: "Issue")
            } label: {
                Label("Copy issue", systemImage: "doc.on.doc")
            }
            Button {
                collabCopy(IssueThreadText.copyText(repo: repo.slug, issue: issue, includeThread: true), label: "Thread")
            } label: {
                Label("Copy thread", systemImage: "text.bubble")
            }
            Button {
                collabCopy(IssueThreadText.promptText(repo: repo.slug, issue: issue), label: "Prompt")
            } label: {
                Label("Copy as agent prompt", systemImage: "text.cursor")
            }
            if let author = CollabEnv.identity?.publicKeyB64url, IssueFields.canDeleteOwn(issue, author: author) {
                Divider()
                Button(role: .destructive) { confirmDelete = true } label: { Label("Delete my issue", systemImage: "trash") }
            }
        } label: {
            Image(systemName: "ellipsis.circle")
        }
        .accessibilityLabel("Issue actions")
        .sheet(isPresented: $editing) {
            IssueEditSheet(repo: repo, issue: issue) { notice in
                message = notice.message
                if !notice.isError { onChanged() }
            }
        }
        .sheet(isPresented: $asking) {
            AskAISheet(repo: repo, issue: issue) { message = $0 }
        }
        .alert("Link a pull request", isPresented: $linking) {
            TextField("Pull request number", text: $linkNumber).keyboardType(.numberPad)
            Button("Link") { Task { await link() } }
            Button("Cancel", role: .cancel) {}
        }
        .confirmationDialog("Delete issue #\(issue.number)?", isPresented: $confirmDelete, titleVisibility: .visible) {
            Button("Delete", role: .destructive) { Task { await deleteOwn() } }
        } message: {
            Text("A signed delete hides the issue everywhere once the owner's node applies it. Only you, its author, can do this.")
        }
        .alert("Issue", isPresented: Binding(get: { message != nil }, set: { if !$0 { message = nil } })) {
            Button("Copy") { UIPasteboard.general.string = message ?? "" }
            Button("Send to agent") { FmMessageHandoff.send(message ?? "") }
            Button("OK", role: .cancel) {}
        } message: {
            Text(message ?? "")
        }
    }

    private func link() async {
        guard let pull = Int(linkNumber.trimmingCharacters(in: CharacterSet(charactersIn: "# "))), pull > 0 else { return }
        linkNumber = ""
        let writer = ReviewEnv.writer(owner: repo.owner, name: repo.name)
        do {
            try await writer.sendIssue(issue.number, .comment(IssueLinks.linkComment(pull: pull)))
            try await writer.sendPull(pull, ReviewComposer.comment(IssueLinks.closingLine(issue: issue.number)))
            message = "Linked #\(issue.number) and pull request #\(pull)."
        } catch {
            message = CollabCopy.runFailure(error)
        }
    }

    private func deleteOwn() async {
        guard await IssueToolsAuth.confirm("Delete issue #\(issue.number)") else { return }
        do {
            try await ReviewEnv.writer(owner: repo.owner, name: repo.name).sendIssue(issue.number, .deleteOwnIssue())
            message = "Delete sent."
            onChanged()
        } catch {
            message = CollabCopy.runFailure(error)
        }
    }
}

@MainActor
enum IssueToolsAuth {
    static func confirm(_ reason: String) async -> Bool {
        let context = LAContext()
        var error: NSError?
        guard context.canEvaluatePolicy(.deviceOwnerAuthentication, error: &error) else { return true }
        return (try? await context.evaluatePolicy(.deviceOwnerAuthentication, localizedReason: reason)) ?? false
    }
}

struct IssueEditSheet: View {
    let repo: RepoRef
    let issue: Issue
    let onDone: (CollabNotice) -> Void
    @Environment(\.dismiss) private var dismiss
    @State private var title = ""
    @State private var labels = ""
    @State private var assignees = ""
    @State private var milestone = ""
    @State private var priority = 0
    @State private var progress = 0.0
    @State private var sliding = false
    @State private var open = true
    @State private var saving = false
    @State private var error: String?

    private var edits: [IssueEdit] {
        IssueFields.diff(
            issue, title: title, labels: IssueFields.parseList(labels), assignees: IssueFields.parseList(assignees),
            priority: priority, progress: Int(progress), milestone: milestone, open: open)
    }

    var body: some View {
        NavigationStack {
            Form {
                Section("Title") { TextField("Title", text: $title, axis: .vertical) }
                Section {
                    TextField("Labels, comma separated", text: $labels)
                        .textInputAutocapitalization(.never)
                    TextField("Assignees, comma separated", text: $assignees)
                        .textInputAutocapitalization(.never)
                        .autocorrectionDisabled()
                    TextField("Milestone", text: $milestone)
                }
                Section {
                    Picker("Priority", selection: $priority) {
                        ForEach(IssueFields.priorities.indices, id: \.self) { (index: Int) in
                            Text(IssueFields.priorities[index].1).tag(IssueFields.priorities[index].0)
                        }
                    }
                    VStack(alignment: .leading) {
                        Text("Progress \(Int(progress))%")
                        Slider(value: $progress, in: 0...100, step: 5, onEditingChanged: { (editing: Bool) in sliding = editing })
                            .onChange(of: progress) { (_: Double, _: Double) in
                                if sliding { FmSoundCues.play(.select) }
                            }
                    }
                    Toggle("Open", isOn: $open)
                }
                if let error { Text(error).foregroundStyle(.red).font(.footnote).frame(maxWidth: .infinity, alignment: .leading).fmMessageTools(error) }
            }
            .navigationTitle("Edit #\(issue.number)")
            .navigationBarTitleDisplayMode(.inline)
            .interactiveDismissDisabled(!edits.isEmpty)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) { Button("Cancel") { dismiss() } }
                ToolbarItem(placement: .confirmationAction) {
                    Button(saving ? "Saving…" : "Save") { Task { await save() } }
                        .disabled(edits.isEmpty || saving)
                }
            }
            .onAppear {
                title = issue.title
                labels = issue.labels.joined(separator: ", ")
                assignees = issue.assignees.joined(separator: ", ")
                milestone = issue.milestone
                priority = IssueFields.clampPriority(issue.priority)
                progress = Double(min(max(issue.progress, 0), 100))
                open = issue.isOpen
            }
        }
    }

    private func save() async {
        saving = true
        defer { saving = false }
        do {
            try await ReviewEnv.writer(owner: repo.owner, name: repo.name).applyIssue(issue.number, edits)
            FmHaptics.commit()
            onDone(CollabNotice("Changes sent. They appear once the owner's node applies them."))
            dismiss()
        } catch {
            self.error = CollabCopy.runFailure(error)
        }
    }
}

struct AskAISheet: View {
    let repo: RepoRef
    let issue: Issue
    let onMessage: (String) -> Void
    @Environment(\.dismiss) private var dismiss
    @State private var question = ""
    @State private var sending = false

    private var mentions: [ModelMention] { IssueModelMentions.parse(question) }

    private var aliasList: String { IssueModelMentions.aliases.map { (alias: String) -> String in "@" + alias }.joined(separator: ", ") }

    var body: some View {
        NavigationStack {
            Form {
                Section {
                    TextField("Ask about this issue, or @opus fix it", text: $question, axis: .vertical)
                        .lineLimit(3...8)
                } footer: {
                    Text("Mention a model (\(aliasList)) to pick who answers. Asking for a fix starts a coding agent.")
                }
                if let first = mentions.first {
                    Section {
                        LabeledContent("Model", value: first.model)
                        LabeledContent("Intent", value: first.intent == .work ? "Work on the issue" : "Answer in the thread")
                    }
                }
                Section {
                    Button(sending ? "Sending…" : "Reprioritize the backlog") { Task { await send(prompt: IssueModelMentions.reprioritizePrompt(openCount: 0, fromReadme: false), title: "Reprioritize backlog", provider: "auto") } }
                    Button("Prioritize from the README") { Task { await send(prompt: IssueModelMentions.reprioritizePrompt(openCount: 0, fromReadme: true), title: "Prioritize from README", provider: "auto") } }
                }
                .disabled(sending)
            }
            .navigationTitle("Ask AI")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) { Button("Cancel") { dismiss() } }
                ToolbarItem(placement: .confirmationAction) {
                    Button("Send") {
                        let mention = mentions.first
                        let prompt: String = mention?.intent == .work
                            ? IssueThreadText.promptText(repo: repo.slug, issue: issue) + "\n\n" + (mention?.request ?? question)
                            : IssueModelMentions.agentPrompt(issueNumber: issue.number, title: issue.title, question: mention?.request ?? question)
                        Task {
                            let provider = mention.map { $0.provider.isEmpty ? "auto" : $0.provider } ?? "auto"
                            await send(prompt: prompt, title: "Issue #\(issue.number)", provider: provider,
                                       model: mention?.model ?? "")
                        }
                    }
                    .disabled(question.dartTrimmed.isEmpty || sending)
                }
            }
        }
    }

    private func send(prompt: String, title: String, provider: String, model: String = "", createPr: Bool = false) async {
        sending = true
        defer { sending = false }
        do {
            let result = try await ReviewEnv.api.launchAgentSession(
                repo.owner, repo.name, prompt: prompt, provider: provider, model: model, title: title, createPr: createPr,
                issueNumber: issue.number)
            onMessage(result.message.isEmpty ? "Agent queued." : result.message)
            dismiss()
        } catch {
            onMessage(dartErrorText(error))
        }
    }
}

struct IssueChecklistCard: View {
    let repo: RepoRef
    let issue: Issue

    var body: some View {
        let items = IssueChecklist.items(issue.body)
        if !items.isEmpty {
            let progress = IssueChecklist.progress(issue.body)
            CollabInfoCard(title: "Checklist \(progress.done)/\(progress.total)") {
                VStack(alignment: .leading, spacing: 6) {
                    ProgressView(value: Double(progress.done), total: Double(max(progress.total, 1)))
                    ForEach(items) { (item: IssueChecklistItem) in
                        Label(item.text.isEmpty ? "(empty)" : item.text, systemImage: item.done ? "checkmark.square" : "square")
                            .font(.footnote)
                            .foregroundStyle(item.done ? Color.secondary : Color.primary)
                    }
                }
            }
        }
    }
}
