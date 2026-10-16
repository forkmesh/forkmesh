// A discussion: the thread, its fediverse replies and the signed reply
// composer. State: FMCode's DiscussionDetailModel.

import FMApi
import FMBase
import FMCode
import SwiftUI

struct DiscussionDetailScreen: View {
    @Environment(\.fmPalette) private var fm
    @State private var model: DiscussionDetailModel
    @State private var sending: Bool = false
    @State private var marking: Bool = false

    /// Opened by number (a deep link or a notification).
    init(repo: Repository, number: Int) {
        _model = State(initialValue: DiscussionDetailModel(repo: repo, number: number, api: CollabEnv.api))
    }

    /// Opened from a list that already holds the discussion.
    init(repo: Repository, discussion: RepoDiscussion) {
        _model = State(initialValue: DiscussionDetailModel(
            repo: repo, number: discussion.number, initial: discussion, api: CollabEnv.api))
    }

    var body: some View {
        content
            .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .top)
            .background(fm.canvas)
            .navigationTitle(model.title)
            .navigationBarTitleDisplayMode(.inline)
            .task {
                await model.start()
            }
    }

    @ViewBuilder private var content: some View {
        if let discussion = model.discussion {
            ScrollView {
                VStack(alignment: .leading, spacing: 12) {
                    headerCard(discussion)
                    timelineCard(discussion)
                    if model.showsFederatedReplies {
                        FederatedRepliesCard(repo: model.repo, kind: "discussion", number: discussion.number)
                    }
                    replyCard
                }
                .padding(20)
            }
            .scrollDismissesKeyboard(.interactively)
        } else if let error = model.loadError {
            CollabStateScroll {
                CollabEmptyCard(message: error)
            }
        } else {
            CollabStateScroll {
                CollabLoadingCard()
            }
        }
    }

    private func headerCard(_ discussion: RepoDiscussion) -> some View {
        CollabInfoCard(title: discussion.title) {
            VStack(alignment: .leading, spacing: 0) {
                CollabChips(chips: model.chips)
                CollabBodyText(text: DiscussionDetail.body(discussion), repo: RepoRef(owner: model.repo.owner, name: model.repo.name))
                    .padding(.top, 16)
            }
        }
    }

    private func timelineCard(_ discussion: RepoDiscussion) -> some View {
        CollabInfoCard(title: "Timeline") {
            if discussion.events.isEmpty {
                Text(DiscussionDetail.timelineEmpty)
                    .fmText(.bodyMedium, color: fm.textPrimary)
            } else {
                VStack(spacing: 10) {
                    ForEach(discussion.events.indices, id: \.self) { (index: Int) in
                        eventRow(discussion.events[index], in: discussion)
                    }
                }
            }
        }
    }

    private func eventRow(_ event: DiscussionEvent, in discussion: RepoDiscussion) -> some View {
        let isAnswer: Bool = DiscussionDetail.isAnswer(event, in: discussion)
        return VStack(alignment: .leading, spacing: 4) {
            eventTile(event, isAnswer: isAnswer)
            if canMarkAnswer(discussion), event.type == "comment", !event.id.isEmpty {
                FmTextButton(
                    isAnswer ? DiscussionDetail.clearAnswer : DiscussionDetail.markAnswer,
                    icon: "task_alt", minHeight: 32,
                    onPressed: marking ? nil : { markAnswer(isAnswer ? "" : event.id) })
            }
        }
    }

    private func eventTile(_ event: DiscussionEvent, isAnswer: Bool) -> CollabTimelineTile {
        CollabTimelineTile(
            icon: isAnswer ? "task_alt" : "forum_outlined",
            tone: isAnswer ? .success : .tertiary,
            title: isAnswer ? DiscussionDetail.answerTitle : DiscussionDetail.eventTitle,
            author: event.displayAuthor,
            text: event.body,
            repo: RepoRef(owner: model.repo.owner, name: model.repo.name))
    }

    private func canMarkAnswer(_ discussion: RepoDiscussion) -> Bool {
        DiscussionDetail.canMarkAnswer(
            discussion, account: CollabEnv.account(), identityPublicKey: CollabEnv.identity?.publicKeyB64url,
            repoOwner: model.repo.owner)
    }

    private func markAnswer(_ target: String) {
        let model: DiscussionDetailModel = self.model
        Task {
            if !(await CollabEnv.requireAccount(DiscussionDetail.answerGuestAction)) {
                return
            }
            marking = true
            let notice: CollabNotice = await CollabEnv.writer(model.repo).markAnswer(
                model.number, target: target, sessionToken: CollabEnv.auth?.session?.sessionToken ?? "")
            marking = false
            if !notice.isError {
                model.showAnswer(target)
            }
            CollabMessenger.shared.show(notice)
        }
    }

    /// "Signed discussion reply".
    private var replyCard: some View {
        CollabInfoCard(title: DiscussionDetail.replyTitle) {
            VStack(alignment: .leading, spacing: 12) {
                Text(DiscussionDetail.replyCopy)
                    .fmText(.bodyMedium, color: fm.textPrimary)
                VStack(alignment: .leading, spacing: FmSpace.x2) {
                    CollabIdentityBar(verb: "Replying")
                    FmTextField(text: $model.reply, hint: DiscussionDetail.replyHint, multiline: true)
                        .accessibilityLabel(Text(DiscussionDetail.replyHint))
                }
                FmInboxPendingNote()
                FmFilledButton(DiscussionDetail.replyLabel, icon: "reply_outlined", onPressed: sending ? nil : { submit() })
                CollabNoticeLine()
            }
        }
    }

    private func submit() {
        if !model.hasReply {
            return
        }
        let model: DiscussionDetailModel = self.model
        Task {
            if !(await CollabEnv.requireAccount(DiscussionDetail.guestAction)) {
                return
            }
            sending = true
            let notice: CollabNotice? = await CollabEnv.writer(model.repo).reply(to: model.number, model.reply)
            sending = false
            CollabMessenger.shared.show(notice)
        }
    }
}
