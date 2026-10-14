// NewIssueScreen: the full-page composer for a repository's issue, or a
// sub-issue when `parentNumber` is positive. It sends the draft to the signed
// inbox itself and closes; the inline notice lands on the screen below
// (CollabMessenger). Push it (NavigationLink / navigationDestination) after
// `AccountFeature.requireAccount(action: "open an issue")`, as RepoDetailScreen
// does. State: FMCode's NewIssueModel.

import FMApi
import FMBase
import FMCode
import SwiftUI

struct NewIssueScreen: View {
    @Environment(\.fmPalette) private var fm
    @Environment(\.dismiss) private var dismiss
    @State private var model: NewIssueModel
    @State private var sending: Bool = false

    init(repo: Repository, parentNumber: Int = 0) {
        _model = State(initialValue: NewIssueModel(repo: repo, parentNumber: parentNumber))
    }

    var body: some View {
        form
            .background(fm.canvas)
            .safeAreaInset(edge: .bottom) {
                submitBar
            }
            .navigationBarTitleDisplayMode(.inline)
            .navigationBarBackButtonHidden(true)
            .toolbar {
                ToolbarItem(placement: .topBarLeading) {
                    FmIconButton(label: "Cancel", onPressed: { dismiss() }) {
                        FmIcon("close", size: 24, color: fm.textPrimary)
                    }
                }
                ToolbarItem(placement: .principal) {
                    titleBlock
                }
            }
    }

    private var titleBlock: some View {
        VStack(alignment: .leading, spacing: 0) {
            Text(model.pageTitle)
                .fmText(.titleMedium)
            Text(model.repo.fullName)
                .fmText(.bodySmall, color: fm.textTertiary, weight: FmType.emphasis)
                .lineLimit(1)
                .truncationMode(.tail)
        }
    }

    private var form: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: FmSpace.x3) {
                CollabIdentityBar(verb: "Posting")
                titleField
                FmTextField(text: $model.body, hint: NewIssueComposer.bodyHint, multiline: true)
                    .frame(minHeight: 140, alignment: .top)
                    .accessibilityLabel(Text(NewIssueComposer.bodyHint))
                FmSectionHeader(title: "Triage", icon: "tune_rounded")
                    .padding(.top, FmSpace.x3)
                triageFields
                FmInboxPendingNote()
                    .padding(.top, FmSpace.x1)
            }
            .padding(.horizontal, FmSpace.x4)
            .padding(.top, FmSpace.x3)
            .padding(.bottom, FmSpace.x5)
        }
        .scrollDismissesKeyboard(.interactively)
    }

    private var titleField: some View {
        VStack(alignment: .leading, spacing: FmSpace.x1) {
            FmTextField(text: $model.title, hint: NewIssueComposer.titleHint, autofocus: true)
                .textInputAutocapitalization(.sentences)
                .accessibilityLabel(Text(NewIssueComposer.titleHint))
            if let error = model.titleError {
                Text(error)
                    .fmText(.bodySmall, color: fm.danger)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .fmMessageTools(error)
                    .padding(.horizontal, FmSpace.x4)
            }
        }
    }

    private var triageFields: some View {
        VStack(alignment: .leading, spacing: FmSpace.x3) {
            FmField(label: "Labels") {
                helperField(text: $model.labels, helper: NewIssueComposer.labelsHelper)
            }
            HStack(alignment: .top, spacing: FmSpace.x3) {
                FmField(label: "Priority") {
                    FmTextField(text: $model.priority)
                        .keyboardType(.numberPad)
                }
                FmField(label: "Milestone") {
                    FmTextField(text: $model.milestone)
                }
            }
            FmField(label: "Assignees") {
                helperField(text: $model.assignees, helper: NewIssueComposer.assigneesHelper)
                    .textInputAutocapitalization(.never)
                    .autocorrectionDisabled(true)
            }
        }
    }

    private func helperField(text: Binding<String>, helper: String) -> some View {
        VStack(alignment: .leading, spacing: FmSpace.x1) {
            FmTextField(text: text)
            Text(helper)
                .fmText(.bodySmall, color: fm.textTertiary)
                .padding(.horizontal, FmSpace.x4)
        }
    }

    private var submitBar: some View {
        FmBottomActionBar(padding: EdgeInsets.fmLTRB(FmSpace.x4, FmSpace.x3, FmSpace.x4, FmSpace.x3)) {
            FmFilledButton(
                "Submit",
                icon: "send_rounded",
                background: fm.accent,
                foreground: Color.white,
                minHeight: 48,
                radius: FmRadius.lg,
                fillWidth: true,
                role: .titleSmall,
                onPressed: sending ? nil : { submit() })
                .accessibilityIdentifier("new-issue-submit")
        }
    }

    private func submit() {
        guard let draft = model.submit() else {
            return
        }
        sending = true
        let writer: CollabWriter = CollabEnv.writer(model.repo)
        Task {
            let notice: CollabNotice = await writer.submitIssue(draft)
            sending = false
            dismiss()
            CollabMessenger.shared.show(notice)
        }
    }
}
