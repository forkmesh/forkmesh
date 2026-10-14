// An issue and what only it uses: the parent links, the sub-issues card, the
// timeline with its mentions, the quick actions, the bounty funding card and
// the issue actions (the bottom sheet and its comment prompt). The state is
// FMCode's IssueDetailModel.

import FMApi
import FMBase
import FMCode
import SwiftUI

struct IssueDetailScreen: View {
    @Environment(\.fmPalette) private var fm
    @State private var model: IssueDetailModel
    @State private var showActions: Bool = false
    @State private var showComment: Bool = false
    @State private var commentText: String = ""
    @State private var composingSubIssue: Bool = false

    /// Opened by number (a deep link or a notification): the issue is read
    /// from the published list.
    init(repo: Repository, number: Int) {
        _model = State(initialValue: IssueDetailModel(repo: repo, number: number, api: CollabEnv.api))
    }

    /// Opened from a list that already holds the issue.
    init(repo: Repository, issue: Issue) {
        _model = State(initialValue: IssueDetailModel(repo: repo, number: issue.number, initial: issue, api: CollabEnv.api))
    }

    private var repo: Repository { model.repo }

    var body: some View {
        if let pull = model.pull {
            PullDetailScreen(repo: repo, pull: pull)
        } else {
            issueBody
        }
    }

    private var issueBody: some View {
        content
            .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .top)
            .background(fm.canvas)
            .navigationTitle(model.title)
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .topBarTrailing) {
                    if let issue = model.issue {
                        IssueToolsMenu(repo: RepoRef(owner: repo.owner, name: repo.name), issue: issue, onChanged: {})
                    }
                }
            }
            .task {
                await model.start()
            }
            .sheet(isPresented: $showActions) {
                actionsSheet
            }
            .navigationDestination(isPresented: $composingSubIssue) {
                NewIssueScreen(repo: repo, parentNumber: model.number)
                    .shellPageClearance()
            }
            .fmDialog(isPresented: $showComment) {
                commentPrompt
            }
    }

    @ViewBuilder private var content: some View {
        if let issue = model.issue {
            ScrollView {
                VStack(alignment: .leading, spacing: 12) {
                    CollabNoticeLine()
                    headerCard(issue)
                    IssueChecklistCard(repo: RepoRef(owner: repo.owner, name: repo.name), issue: issue)
                    subIssuesCard
                    timelineCard(issue)
                    if model.showsFederatedReplies {
                        FederatedRepliesCard(repo: repo, kind: "issue", number: issue.number)
                    }
                    BountyFundingCard(model: model, issue: issue)
                }
                .padding(20)
            }
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

    // MARK: Cards

    private func headerCard(_ issue: Issue) -> some View {
        CollabInfoCard(title: issue.title) {
            VStack(alignment: .leading, spacing: 0) {
                if !model.parents.isEmpty {
                    parentLinks
                        .padding(.bottom, 12)
                }
                CollabChips(chips: model.chips)
                CollabBodyText(text: model.body, repo: RepoRef(owner: repo.owner, name: repo.name))
                    .padding(.top, 16)
            }
        }
    }

    /// "Sub-issue of #N title · Open".
    private var parentLinks: some View {
        CollabWrap(spacing: FmSpace.x2, runSpacing: 6) {
            Text("Sub-issue of")
                .fmText(.bodySmall, color: fm.textSecondary)
                .padding(.vertical, 6)
            ForEach(model.parents, id: \.self) { (number: Int) in
                parentChip(number)
            }
        }
        .accessibilityIdentifier("issue-parent-links")
    }

    @ViewBuilder private func parentChip(_ number: Int) -> some View {
        if let parent = model.find(number) {
            NavigationLink {
                IssueDetailScreen(repo: repo, issue: parent)
                    .shellPageClearance()
            } label: {
                parentFace(number, parent)
            }
            .buttonStyle(.plain)
        } else {
            parentFace(number, nil)
        }
    }

    private func parentFace(_ number: Int, _ parent: Issue?) -> some View {
        HStack(spacing: FmSpace.x1) {
            FmIcon("account_tree_outlined", size: 16, color: fm.textSecondary)
            Text(issueParentLinkLabel(number, parent))
                .fmText(.labelLarge, color: parent == nil ? fm.textTertiary : fm.textPrimary)
                .lineLimit(1)
                .truncationMode(.tail)
        }
        .padding(.horizontal, FmSpace.x3)
        .frame(height: 32)
        .overlay {
            FmRoundedShape(radius: FmRadius.md).strokeBorder(fm.border, lineWidth: 1)
        }
    }

    private var subIssuesCard: some View {
        CollabInfoCard(title: model.subIssuesTitle) {
            VStack(alignment: .leading, spacing: 0) {
                if model.children.isEmpty {
                    Text(SubIssues.empty)
                        .fmText(.bodyMedium, color: fm.textPrimary)
                } else {
                    ForEach(model.children, id: \.self) { (number: Int) in
                        subIssueRow(number)
                    }
                }
                FmOutlinedButton("Add sub-issue", icon: "add", onPressed: { addSubIssue() })
                    .accessibilityIdentifier("issue-add-sub-issue")
                    .padding(.top, 10)
            }
        }
    }

    @ViewBuilder private func subIssueRow(_ number: Int) -> some View {
        if let child = model.find(number) {
            NavigationLink {
                IssueDetailScreen(repo: repo, issue: child)
                    .shellPageClearance()
            } label: {
                subIssueFace(number, child)
            }
            .buttonStyle(.plain)
            .accessibilityIdentifier("sub-issue-\(number)")
        } else {
            subIssueFace(number, nil)
        }
    }

    private func subIssueFace(_ number: Int, _ child: Issue?) -> some View {
        VStack(alignment: .leading, spacing: 0) {
            Text(SubIssues.rowTitle(number, child))
                .fmText(.bodyMedium, color: fm.textPrimary, weight: FmType.emphasis)
            Text(SubIssues.details(child))
                .fmText(.bodySmall, color: fm.textSecondary)
                .padding(.top, 2)
            if let child = child {
                FmLinearProgressIndicator(value: SubIssues.progress(child))
                    .frame(height: 3)
                    .clipped()
                    .padding(.top, 6)
            }
        }
        .padding(.vertical, FmSpace.x2)
        .frame(maxWidth: .infinity, alignment: .leading)
        .contentShape(Rectangle())
    }

    private func timelineCard(_ issue: Issue) -> some View {
        CollabInfoCard(title: "Timeline") {
            VStack(alignment: .leading, spacing: 0) {
                timeline
                Text(CollabCopy.threadSubscriptionNote)
                    .fmText(.bodyMedium, color: fm.textPrimary)
                    .padding(.top, 10)
                quickActions(issue)
                    .padding(.top, 14)
            }
        }
    }

    /// Signed events and derived mentions by time.
    @ViewBuilder private var timeline: some View {
        let entries: [IssueTimelineEntry] = model.timeline
        if entries.isEmpty {
            Text(IssueTimeline.empty)
                .fmText(.bodyMedium, color: fm.textPrimary)
        } else {
            VStack(spacing: 10) {
                ForEach(entries.indices, id: \.self) { (index: Int) in
                    timelineEntry(entries[index])
                }
            }
        }
    }

    @ViewBuilder private func timelineEntry(_ entry: IssueTimelineEntry) -> some View {
        switch entry {
        case .event(let event):
            CollabTimelineTile(
                icon: IssueTimeline.icon(event),
                tone: IssueTimeline.tone(event),
                title: IssueTimeline.title(event),
                author: event.displayAuthor,
                text: event.body,
                repo: RepoRef(owner: repo.owner, name: repo.name))
        case .mention(let mention):
            mentionTile(mention)
        }
    }

    @ViewBuilder private func mentionTile(_ mention: IssueMention) -> some View {
        if let source = model.find(mention.sourceNumber) {
            NavigationLink {
                IssueDetailScreen(repo: repo, issue: source)
                    .shellPageClearance()
            } label: {
                mentionFace(mention)
            }
            .buttonStyle(.plain)
            .accessibilityIdentifier("issue-mention-\(mention.sourceNumber)")
        } else {
            mentionFace(mention)
        }
    }

    private func mentionFace(_ mention: IssueMention) -> CollabTimelineTile {
        CollabTimelineTile(
            icon: "link",
            tone: .tertiary,
            title: IssueTimeline.mentionTitle(mention),
            author: mention.authorName,
            text: "")
    }

    private func quickActions(_ issue: Issue) -> some View {
        CollabWrap(spacing: 10, runSpacing: 10) {
            FmFilledButton(IssueActions.quickActionsLabel, icon: "add_comment_outlined", onPressed: { openActions() })
            ThreadSubscriptionButton(repo: repo, source: "issue", number: issue.number)
            FmInboxPendingNote()
        }
    }

    // MARK: Actions

    private func addSubIssue() {
        Task {
            if await CollabEnv.requireAccount(NewIssueComposer.guestAction) {
                composingSubIssue = true
            }
        }
    }

    private func openActions() {
        Task {
            if await CollabEnv.requireAccount(IssueActions.guestAction) {
                showActions = true
            }
        }
    }

    /// showIssueActions' bottom sheet.
    @ViewBuilder private var actionsSheet: some View {
        if let issue = model.issue {
            VStack(alignment: .leading, spacing: 0) {
                CollabSheetHeader(title: IssueActions.header(issue), subtitle: issue.body)
                    .padding(.bottom, FmSpace.x2)
                CollabSheetRow(icon: "how_to_vote_outlined", label: "Vote") {
                    showActions = false
                    send { writer in await writer.vote(issue) }
                }
                CollabSheetRow(icon: "chat_bubble_outline", label: "Comment") {
                    showActions = false
                    commentText = ""
                    showComment = true
                }
                CollabSheetRow(icon: IssueActions.statusIcon(issue), label: IssueActions.statusLabel(issue)) {
                    showActions = false
                    send { writer in await writer.toggleStatus(issue) }
                }
            }
            .padding(.bottom, FmSpace.x4)
            .fmSheetStyle(fitsContent: true)
        }
    }

    private var commentPrompt: some View {
        CollabPromptDialog(
            text: $commentText,
            title: model.issue.map(IssueActions.commentTitle) ?? "",
            multiline: true,
            onCancel: { showComment = false },
            onSubmit: {
                showComment = false
                guard let issue = model.issue else {
                    return
                }
                let text: String = commentText
                send { writer in await writer.comment(on: issue, text) }
            })
    }

    /// The write, then its inline notice.
    private func send(_ write: @escaping (CollabWriter) async -> CollabNotice?) {
        let writer: CollabWriter = CollabEnv.writer(repo)
        Task {
            let notice: CollabNotice? = await write(writer)
            CollabMessenger.shared.show(notice)
        }
    }
}

/// Migration-only status of a legacy bounty.
private struct BountyFundingCard: View {
    @Environment(\.fmPalette) private var fm

    let model: IssueDetailModel
    let issue: Issue

    var body: some View {
        CollabInfoCard(title: "Bounty funding") {
            VStack(alignment: .leading, spacing: 0) {
                pledge
                Text(IssueDetail.statusLine(issue))
                    .fmText(.bodyMedium, color: fm.textPrimary)
                    .padding(.top, FmSpace.x2)
                if !issue.bountyProgressLabel.isEmpty {
                    Text(issue.bountyProgressLabel)
                        .fmText(.bodyMedium, color: fm.textPrimary)
                        .padding(.top, FmSpace.x2)
                }
                copyRows
                Text(IssueDetail.nonCustodialCopy)
                    .fmText(.bodySmall, color: fm.textSecondary, height: 1.35)
                    .padding(.top, 12)
                buttons
                    .padding(.top, 12)
                Text(IssueDetail.fundingGateCopy)
                    .fmText(.bodySmall, color: fm.textTertiary)
                    .padding(.top, 10)
                if !model.bountyMessage.isEmpty {
                    Text(model.bountyMessage)
                        .fmText(.bodySmall, color: model.bountyMessageIsError ? fm.danger : fm.textSecondary)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .fmMessageTools(model.bountyMessage)
                        .padding(.top, 10)
                }
            }
        }
    }

    @ViewBuilder private var pledge: some View {
        if let text = IssueDetail.pledgeText(issue) {
            Text(text)
                .fmText(.bodyMedium, color: fm.textPrimary, weight: FmType.emphasis)
        } else {
            Text(IssueDetail.noPledge)
                .fmText(.bodyMedium, color: fm.textSecondary)
        }
    }

    private var copyRows: some View {
        let rows: [BountyCopyRow] = model.bountyRows
        return VStack(alignment: .leading, spacing: FmSpace.x2) {
            ForEach(rows.indices, id: \.self) { (index: Int) in
                CollabCopyableValue(row: rows[index])
            }
        }
        .padding(.top, rows.isEmpty ? 0 : 12)
    }

    private var buttons: some View {
        CollabWrap(spacing: 10, runSpacing: 10) {
            FmOutlinedButton("Refresh bounty status", icon: "refresh", onPressed: refreshAction)
                .accessibilityIdentifier("issue-bounty-refresh-status")
            FmFilledButton("Custodial funding disabled", icon: "account_balance_wallet_outlined", onPressed: nil)
                .accessibilityIdentifier("issue-bounty-prepare-deposit")
        }
    }

    /// Disabled while a refresh is in flight.
    private var refreshAction: (() -> Void)? {
        if model.bountyBusy {
            return nil
        }
        return { refresh() }
    }

    private func refresh() {
        let model: IssueDetailModel = self.model
        Task {
            await model.refreshBounty()
        }
    }
}
