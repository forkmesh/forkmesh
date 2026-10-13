import FMAccount
import FMBase
import FMChat
import SwiftUI
import UIKit

struct ProjectInviteSheet: View {
    @Environment(\.fmPalette) private var fm
    @Environment(\.dismiss) private var dismiss
    let project: RelayProfile
    @State private var account: String = ""
    @State private var email: String = ""
    @State private var sending: Bool = false
    @State private var notice: RelayNotice?

    private var me: String {
        (Services.get(AuthService.self)?.session?.nodeName ?? "").trimmingCharacters(in: .whitespacesAndNewlines)
            .lowercased()
    }

    private var invitation: ProjectInvitation {
        ProjectInvitation(projectName: project.label, inviter: me, origin: project.hasInstance ? project.httpOrigin : "")
    }

    private var host: String { ProjectInvite.host(invitation) }

    private var direct: DirectMessageService? { Services.get(DirectMessageService.self) }

    private var canInviteAccount: Bool { project.hasInstance && !me.isEmpty && direct != nil }

    private var accountNote: String? {
        if !project.hasInstance { return "This project has no instance yet, so invite people by email." }
        if me.isEmpty { return "Sign in to invite people on \(host) directly." }
        if direct == nil { return "Messages are not connected yet." }
        return nil
    }

    var body: some View {
        NavigationStack {
            ScrollView {
                VStack(alignment: .leading, spacing: 0) {
                    Text(
                        project.hasInstance
                            ? "Invite someone who already has an account on \(host), or email anyone else an invitation link."
                            : "Email someone an invitation to this project."
                    )
                    .fmText(.bodySmall, color: fm.textSecondary, height: 1.35)
                    if project.hasInstance {
                        accountSection.padding(.top, FmSpace.x4)
                    }
                    emailSection.padding(.top, FmSpace.x5)
                    if let notice {
                        RelayNoticeView(notice: notice).padding(.top, FmSpace.x4)
                    }
                }
                .padding(FmSpace.x4)
            }
            .fmPreGlassBackground(fm.bgBase)
            .navigationTitle("Invite to \(project.label)")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button("Done") { dismiss() }
                }
            }
        }
    }

    private var accountSection: some View {
        VStack(alignment: .leading, spacing: FmSpace.x3) {
            FmField(label: "Someone on \(host)") {
                TextField("Their ForkMesh account name", text: $account)
                    .textInputAutocapitalization(.never)
                    .autocorrectionDisabled(true)
                    .submitLabel(.send)
                    .onSubmit { sendInvite() }
                    .accessibilityIdentifier("project-invite-account")
            }
            if let accountNote {
                Text(accountNote).fmText(.bodySmall, color: fm.textTertiary)
                    .fmMessageTools(accountNote)
            }
            FmFilledButton(
                sending ? "Sending…" : "Send invite", icon: "send",
                onPressed: canInviteAccount && !sending ? { sendInvite() } : nil)
                .accessibilityIdentifier("project-invite-send")
        }
    }

    private var emailSection: some View {
        let link: String = ProjectInvite.invitationLink(invitation)
        return VStack(alignment: .leading, spacing: FmSpace.x3) {
            FmField(label: "Anyone else, by email") {
                TextField("name@example.com (optional)", text: $email)
                    .textInputAutocapitalization(.never)
                    .autocorrectionDisabled(true)
                    .keyboardType(.emailAddress)
                    .textContentType(.emailAddress)
                    .accessibilityIdentifier("project-invite-email")
            }
            HStack(spacing: FmSpace.x3) {
                FmFilledButton("Open email", icon: "alternate_email", onPressed: { openEmail() })
                    .accessibilityIdentifier("project-invite-open-email")
                if !link.isEmpty {
                    FmOutlinedButton("Copy link", icon: "content_copy", onPressed: {
                        UIPasteboard.general.string = link
                        notice = RelayNotice(message: "The invitation link was copied.", error: false)
                    })
                }
            }
            if !link.isEmpty {
                Text(link)
                    .fmText(.bodySmall, color: fm.textTertiary)
                    .textSelection(.enabled)
            }
        }
    }

    private func openEmail() {
        let recipients: ProjectInvite.Recipients = ProjectInvite.emailRecipients(email)
        guard recipients.invalid.isEmpty else {
            notice = RelayNotice(message: "“\(recipients.invalid)” is not an email address.", error: true)
            return
        }
        let current: ProjectInvitation = invitation
        guard let url = URL(string: ProjectInvite.mailtoUrl(recipients.addresses, current)) else {
            copyEmail(current)
            return
        }
        Task {
            if await UIApplication.shared.open(url) {
                notice = RelayNotice(message: "Your email app has the invitation ready to send.", error: false)
            } else {
                copyEmail(current)
            }
        }
    }

    private func copyEmail(_ current: ProjectInvitation) {
        UIPasteboard.general.string = ProjectInvite.emailSubject(current) + "\n\n" + ProjectInvite.emailBody(current)
        notice = RelayNotice(
            message: "No email app opened, so the invitation was copied. Paste it into an email.", error: false)
    }

    private func sendInvite() {
        guard canInviteAccount, !sending, let direct else { return }
        var who: String = account.trimmingCharacters(in: .whitespacesAndNewlines)
        if who.hasPrefix("@") { who.removeFirst() }
        who = who.lowercased()
        guard !who.isEmpty else {
            notice = RelayNotice(message: "Enter their account name on \(host).", error: true)
            return
        }
        if ProjectInvite.looksLikeEmail(account) {
            email = account.trimmingCharacters(in: .whitespacesAndNewlines)
            account = ""
            notice = RelayNotice(
                message: "That is an email address, so it moved to the email invitation. Tap Open email to send it.",
                error: false)
            return
        }
        guard who != me else {
            notice = RelayNotice(message: "You are already in this project.", error: true)
            return
        }
        let text: String = ProjectInvite.directMessageText(invitation)
        sending = true
        notice = nil
        Task {
            let failure: String?
            do {
                let conversation: DirectConversation = try await direct.start(username: who)
                failure = await direct.send(conversation.conversation, text: text)
                    ? nil : "Could not send the invitation to @\(who)."
            } catch DirectMessageError.refused("user_not_found") {
                failure = "@\(who) is not on \(host). Invite them by email below instead."
            } catch let error as DirectMessageError {
                failure = error.description
            } catch {
                failure = "Could not send the invitation to @\(who)."
            }
            sending = false
            if let failure {
                notice = RelayNotice(message: failure, error: true)
            } else {
                account = ""
                FmHaptics.success()
                notice = RelayNotice(message: "Invitation sent to @\(who). It is in your direct messages.", error: false)
            }
        }
    }
}
