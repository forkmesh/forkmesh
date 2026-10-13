// The signed thread alert toggle and the fediverse replies of an issue, pull or
// discussion. State: FMCode's ThreadSubscriptionModel and
// FederatedRepliesModel.

import FMApi
import FMBase
import FMCode
import FMCrypto
import SwiftUI

/// Subscribe or unsubscribe this thread's alerts,
/// signed with the account's paired node key.
struct ThreadSubscriptionButton: View {
    @Environment(\.fmPalette) private var fm
    @State private var model: ThreadSubscriptionModel
    @State private var notice: FmNotice?

    /// `source` is "issue" or "pull".
    init(repo: Repository, source: String, number: Int) {
        _model = State(initialValue: ThreadSubscriptionModel(
            repo: repo, source: source, number: number, inbox: CollabEnv.inbox,
            account: { CollabEnv.account() }, identityPublicKey: CollabEnv.identity?.publicKeyB64url))
    }

    private var guest: Bool { CollabEnv.isGuest }

    private var action: (() -> Void)? {
        if !model.enabled(guest: guest) {
            return nil
        }
        return { tapped() }
    }

    var body: some View {
        VStack(alignment: .leading, spacing: FmSpace.x1) {
            button
            FmNoticeLine($notice)
        }
    }

    private var button: some View {
        Button(action: { action?() }) {
            HStack(spacing: FmSpace.x2) {
                if model.busy {
                    FmCircularProgressIndicator(size: 16, strokeWidth: 2)
                } else {
                    FmIcon(model.icon(guest: guest), size: 18, color: labelColor)
                }
                Text(model.label(guest: guest))
                    .fmText(.labelLarge, color: labelColor)
                    .lineLimit(1)
            }
            .padding(.horizontal, FmSpace.x5)
            .frame(minHeight: 40)
            .overlay {
                Capsule().strokeBorder(fm.border, lineWidth: 1)
            }
            .contentShape(Capsule())
        }
        .buttonStyle(FmPressStyle(highlight: fm.textPrimary.opacity(0.08), cornerRadius: FmRadius.full))
        .disabled(action == nil)
    }

    private var labelColor: Color {
        action == nil ? fm.textPrimary.opacity(0.38) : fm.textPrimary
    }

    private func tapped() {
        let model: ThreadSubscriptionModel = self.model
        if guest {
            Task {
                _ = await CollabEnv.requireAccount(ThreadSubscription.guestAction)
            }
            return
        }
        Task {
            if let result = await model.toggle() {
                notice = FmNotice(result.message, isError: result.isError)
            }
        }
    }
}

/// Remote ActivityPub replies, apart from the
/// signed native log.
struct FederatedRepliesCard: View {
    @Environment(\.fmPalette) private var fm
    @State private var model: FederatedRepliesModel

    /// `kind` is "issue", "pull" or "discussion".
    init(repo: Repository, kind: String, number: Int) {
        _model = State(initialValue: FederatedRepliesModel(repo: repo, kind: kind, number: number, api: CollabEnv.api))
    }

    var body: some View {
        CollabInfoCard(title: FederatedThread.title) {
            VStack(alignment: .leading, spacing: 12) {
                Text(FederatedThread.intro)
                    .fmText(.bodySmall, color: fm.textSecondary, height: 1.35)
                replies
            }
        }
        .task {
            await model.start()
        }
    }

    @ViewBuilder private var replies: some View {
        switch model.state {
        case .loading:
            FmLinearProgressIndicator()
        case .failed:
            Text(FederatedThread.unavailableText)
                .fmText(.bodyMedium, color: fm.textTertiary)
                .frame(maxWidth: .infinity, alignment: .leading)
                .fmMessageTools(FederatedThread.unavailableText)
        case .ready(let list):
            if list.isEmpty {
                Text(FederatedThread.empty)
                    .fmText(.bodyMedium, color: fm.textTertiary)
            } else {
                VStack(alignment: .leading, spacing: 0) {
                    ForEach(list.indices, id: \.self) { (index: Int) in
                        FederatedReplyTile(reply: list[index], repo: RepoRef(owner: model.repo.owner, name: model.repo.name))
                    }
                }
            }
        }
    }
}

private struct FederatedReplyTile: View {
    @Environment(\.fmPalette) private var fm
    @Environment(\.openURL) private var openURL

    let reply: FederatedReply
    let repo: RepoRef

    private var unavailable: Bool { FederatedThread.unavailable(reply) }

    var body: some View {
        HStack(spacing: 0) {
            Rectangle()
                .fill(fm.border)
                .frame(width: 2)
            VStack(alignment: .leading, spacing: 6) {
                header
                LinkedText(reply.displayBody, repo: repo)
                    .fmText(.bodyMedium, color: unavailable ? fm.textTertiary : fm.textPrimary, height: 1.4)
                    .italic(unavailable)
                    .textSelection(.enabled)
                footer
            }
            .padding(.leading, 12)
            .frame(maxWidth: .infinity, alignment: .leading)
        }
        .fixedSize(horizontal: false, vertical: true)
        .padding(.leading, CGFloat(FederatedThread.indent(reply)))
        .padding(.vertical, FmSpace.x2)
    }

    private var header: some View {
        CollabWrap(spacing: FmSpace.x2, runSpacing: FmSpace.x1) {
            Text(FederatedThread.author(reply))
                .fmText(.bodyMedium, color: fm.textPrimary, weight: FmType.emphasis)
            if reply.edited {
                Text(FederatedThread.edited)
                    .fmText(.labelSmall, color: fm.textTertiary)
            }
            if !FederatedThread.provenance(reply).isEmpty {
                Text(FederatedThread.provenance(reply))
                    .fmText(.labelSmall, color: fm.textTertiary)
            }
        }
    }

    private var footer: some View {
        CollabWrap(spacing: FmSpace.x2, runSpacing: FmSpace.x1) {
            Text(FederatedThread.notNative)
                .fmText(.labelSmall, color: fm.textTertiary)
                .padding(.vertical, FmSpace.x2)
            if let url = FederatedThread.backlinkURL(reply.backlink) {
                FmTextButton(FederatedThread.original, icon: "open_in_new", minHeight: 32) {
                    openURL(url)
                }
            }
        }
    }
}
