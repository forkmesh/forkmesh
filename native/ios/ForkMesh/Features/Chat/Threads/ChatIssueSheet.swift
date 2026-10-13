import FMAccount
import FMApi
import FMBase
import FMChat
import SwiftUI

struct ChatIssueSheet: View {
    @Environment(\.dismiss) private var dismiss
    @State private var repo: String
    @State private var title: String
    @State private var bodyText: String
    @State private var working: Bool = false
    @State private var error: String?
    let onFiled: (String) -> Void

    init(draft: ChatIssueDraft, onFiled: @escaping (String) -> Void) {
        _repo = State(initialValue: draft.repo.slug)
        _title = State(initialValue: draft.title)
        _bodyText = State(initialValue: draft.body)
        self.onFiled = onFiled
    }

    static func draft(fromCommand argument: String, channel: String, author: String) -> Result<ChatIssueDraft, ChatIssueDraft.Failure> {
        let first: String = String(argument.trimmingCharacters(in: .whitespaces).prefix { !$0.isWhitespace })
        var repos: [RepoRef] = [RepoRef(owner: "forkmesh", name: "forkmesh")]
        if let named = RepoRef(slug: first) { repos.insert(named, at: 0) }
        return ChatIssueDraft.parse(argument, repos: repos, current: nil, channel: channel, author: author, now: Date())
    }

    var body: some View {
        NavigationStack {
            Form {
                Section("Repository") {
                    TextField("owner/name", text: $repo)
                        .textInputAutocapitalization(.never)
                        .autocorrectionDisabled()
                }
                Section("Title") {
                    TextField("Title", text: $title)
                }
                Section("Description") {
                    TextEditor(text: $bodyText).frame(minHeight: 160)
                }
                if let error {
                    Section { Text(error).foregroundStyle(.red).frame(maxWidth: .infinity, alignment: .leading).fmMessageTools(error) }
                }
            }
            .navigationTitle("New issue")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) { Button("Cancel") { dismiss() } }
                ToolbarItem(placement: .confirmationAction) {
                    Button("File") { Task { await file() } }
                        .disabled(working || RepoRef(slug: repo) == nil || title.trimmingCharacters(in: .whitespaces).isEmpty)
                }
            }
        }
    }

    private func file() async {
        guard let target = RepoRef(slug: repo.trimmingCharacters(in: .whitespaces)) else { return }
        guard let inbox = Services.get(InboxService.self) else {
            error = "Sign in to file issues."
            return
        }
        working = true
        defer { working = false }
        do {
            try await inbox.submitNewIssue(target.owner, target.name, title: title, body: bodyText)
            onFiled("Issue \"\(title)\" was signed and sent to \(target.slug).")
            dismiss()
        } catch {
            self.error = "\(error)"
        }
    }
}
