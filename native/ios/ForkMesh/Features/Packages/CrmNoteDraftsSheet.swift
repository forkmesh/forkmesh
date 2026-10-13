import FMPackages
import SwiftUI
import UIKit

struct CrmNoteDraftsSheet: View {
    @Bindable var model: CrmModel
    let present: (CrmSheet?) -> Void
    @State private var replying: CrmNoteDraft?
    @Environment(\.dismiss) private var dismiss
    @Environment(\.openURL) private var openURL

    var body: some View {
        let drafts = model.noteDrafts
        List {
            Section {
                if drafts.isEmpty {
                    Text("No unsent drafts. A draft is a contact note paragraph that starts with \"\(CrmNoteDraft.mark)\"")
                        .foregroundStyle(.secondary)
                }
                ForEach(drafts) { draft in
                    VStack(alignment: .leading, spacing: 4) {
                        HStack {
                            Text(draft.name).font(.body.weight(.medium))
                            Spacer()
                            Text(draft.kindTitle).font(.caption).foregroundStyle(.secondary)
                        }
                        if !draft.to.isEmpty { Text(draft.to).font(.caption).foregroundStyle(.secondary) }
                        Text(draft.subject.isEmpty ? draft.body : draft.subject + " — " + draft.body)
                            .font(.callout)
                            .lineLimit(3)
                        HStack {
                            Button(draft.mastodon ? "Reply on Mastodon" : "Send") { send(draft) }
                                .buttonStyle(.borderedProminent)
                                .disabled(!draft.canSend)
                            Button("Mark sent") { Task { await model.markNoteDraftSent(draft.contactId, stampContacted: draft.mastodon) } }
                                .buttonStyle(.bordered)
                        }
                        .font(.caption)
                        if !draft.canSend {
                            Text(draft.mastodon ? "No outreach toot is linked to this contact." : "The contact has no email address.")
                                .font(.caption2)
                                .foregroundStyle(.orange)
                                .fmMessageTools(draft.mastodon ? "No outreach toot is linked to this contact." : "The contact has no email address.")
                        }
                    }
                    .padding(.vertical, 2)
                }
            } footer: {
                if !drafts.isEmpty {
                    let mastodon = drafts.filter(\.mastodon).count
                    Text("\(drafts.count) unsent: \(drafts.count - mastodon) by email, \(mastodon) as Mastodon replies. Sending an email draft marks it sent in the contact's notes.")
                }
            }
            PackagesErrorBanner(text: model.errorText)
        }
        .navigationTitle("Unsent drafts")
        .navigationBarTitleDisplayMode(.inline)
        .toolbar { ToolbarItem(placement: .cancellationAction) { Button("Done") { dismiss() } } }
        .confirmationDialog(
            "Mark this draft as sent?", isPresented: Binding(get: { replying != nil }, set: { if !$0 { replying = nil } }),
            titleVisibility: .visible
        ) {
            Button("Mark sent") {
                guard let draft = replying else { return }
                replying = nil
                Task { await model.markNoteDraftSent(draft.contactId, stampContacted: true) }
            }
        } message: {
            Text("The reply is on the clipboard and the thread is open. Paste it there and post it, then mark the draft sent.")
        }
    }

    private func send(_ draft: CrmNoteDraft) {
        if draft.mastodon {
            UIPasteboard.general.string = draft.body
            if let url = URL(string: draft.thread) { openURL(url) }
            replying = draft
            return
        }
        present(.compose(draft.emailDraft(model.contact(draft.contactId)), outreach: false))
    }
}
