import FMApi
import FMBase
import FMCode
import SwiftUI
import UIKit

@MainActor
struct PingDetailScreen: View {
    @Environment(\.fmPalette) var fm
    @Environment(\.openURL) var openURL
    @Environment(\.dismiss) var dismiss

    let model: NotificationsScreenModel
    let ping: ForkNotification

    @State var openedRepo: NotificationDeepLink?
    @State var agentTarget: PingAgentTarget?
    @State var pickRepos: PingRepoPick?
    @State var showPingSettings: Bool = false
    @State var confirmDelete: Bool = false
    @State var deleting: Bool = false
    @State var openNotice: FmNotice?
    @State var actionNotice: FmNotice?

    private var current: ForkNotification {
        model.current(ping)
    }

    var body: some View {
        let notification: ForkNotification = current
        let target: NotificationTapTarget = model.target(notification)
        ScrollView {
            VStack(alignment: .leading, spacing: FmSpace.x5) {
                PingDetailHero(notification: notification)
                if let label = pingOpenLabel(target) {
                    FmFilledButton(
                        label,
                        icon: "open_in_new",
                        radius: FmRadius.md,
                        fillWidth: true,
                        onPressed: { open(target) })
                        .accessibilityIdentifier("ping-detail-open")
                    if openNotice != nil {
                        FmNoticeLine($openNotice)
                    }
                }
                actions(notification)
                details(notification)
            }
            .padding(FmSpace.x4)
        }
        .background(fm.canvas.ignoresSafeArea())
        .navigationTitle(PingDetailCopy.title)
        .navigationBarTitleDisplayMode(.inline)
        .toolbar {
            ToolbarItemGroup(placement: .topBarTrailing) {
                ShareLink(item: pingAgentPrompt(notification), subject: Text(notification.title)) {
                    FmIcon("ios_share", size: 22, color: fm.textPrimary, label: "Share ping")
                }
                if !notification.id.isEmpty {
                    Button {
                        confirmDelete = true
                    } label: {
                        FmIcon("delete_outline", size: 22, color: fm.danger, label: PingDetailCopy.deleteLabel)
                    }
                    .disabled(deleting)
                    .accessibilityIdentifier("ping-detail-delete")
                }
            }
        }
        .task { await model.markRead(ping) }
        .navigationDestination(item: $openedRepo) { (link: NotificationDeepLink) in
            RepoDetailScreen(repo: link.repo, initialTab: link.initialTab, initialTarget: link)
                .shellPageClearance()
        }
        .navigationDestination(item: $agentTarget) { (target: PingAgentTarget) in
            PingAgentHandoff.destination(target)
                .shellPageClearance()
        }
        .navigationDestination(isPresented: $showPingSettings) {
            PingSettingsScreen()
                .shellPageClearance()
        }
        .sheet(item: $pickRepos) { (pick: PingRepoPick) in
            PingRepoPickerSheet(
                pick: pick,
                runningRepo: PingAgentHandoff.runningRepo,
                onPick: { (candidate: PingAgentCandidate) in
                    pickRepos = nil
                    agentTarget = PingAgentTarget(candidate: candidate, prompt: pick.prompt)
                })
                .fmSheetStyle(fitsContent: true)
        }
        .fmDialog(isPresented: $confirmDelete) {
            FmConfirmDialog(
                title: PingDetailCopy.deleteTitle,
                message: PingDetailCopy.deleteBody,
                confirmLabel: "Delete",
                destructive: true,
                onCancel: { confirmDelete = false },
                onConfirm: {
                    confirmDelete = false
                    delete(notification)
                })
        }
    }

    private func actions(_ notification: ForkNotification) -> some View {
        VStack(alignment: .leading, spacing: FmSpace.x2) {
            FmSectionHeader(title: "Actions")
            FmCard(padding: EdgeInsets()) {
                VStack(spacing: 0) {
                    FmListRow(
                        title: "Send to agent",
                        subtitle: "Open the coding agent with this ping in its composer",
                        leading: FmIconTile(icon: "send_outlined", tint: fm.accent).fmAny,
                        onTap: { sendToAgent(notification) },
                        showChevron: false)
                        .accessibilityIdentifier("ping-detail-to-agent")
                    FmRowDivider()
                    FmListRow(
                        title: "Copy text",
                        leading: FmIconTile(icon: "copy_outlined").fmAny,
                        onTap: {
                            UIPasteboard.general.string = pingAgentPrompt(notification)
                            actionNotice = FmNotice("Copied to clipboard")
                        },
                        showChevron: false)
                    if !notification.href.dartTrimmed.isEmpty {
                        FmRowDivider()
                        FmListRow(
                            title: "Copy link",
                            leading: FmIconTile(icon: "link_rounded").fmAny,
                            onTap: {
                                if case .web(let url) = model.target(notification) {
                                    UIPasteboard.general.string = url.absoluteString
                                } else {
                                    UIPasteboard.general.string = notification.href.dartTrimmed
                                }
                                actionNotice = FmNotice("Link copied")
                            },
                            showChevron: false)
                    }
                    FmRowDivider()
                    FmListRow(
                        title: "Ping settings",
                        leading: FmIconTile(icon: "tune_rounded").fmAny,
                        onTap: { showPingSettings = true })
                    if !notification.id.isEmpty {
                        FmRowDivider()
                        FmListRow(
                            title: PingDetailCopy.deleteLabel,
                            leading: FmIconTile(icon: "delete_outline", tint: fm.danger).fmAny,
                            onTap: { confirmDelete = true },
                            showChevron: false,
                            titleColor: fm.danger)
                    }
                }
            }
            if actionNotice != nil {
                FmNoticeLine($actionNotice)
                    .accessibilityIdentifier("ping-detail-notice")
            }
        }
    }

    private func details(_ notification: ForkNotification) -> some View {
        let now: Int = Int(Date().timeIntervalSince1970 * 1000)
        return VStack(alignment: .leading, spacing: FmSpace.x2) {
            FmSectionHeader(title: "Details")
            FmCard {
                VStack(alignment: .leading, spacing: 0) {
                    ForEach(Array(pingDetailFacts(notification, now: now).enumerated()), id: \.offset) { entry in
                        FmKeyValueRow(label: entry.element.label, value: entry.element.value)
                            .textSelection(.enabled)
                    }
                }
            }
        }
    }

    private func open(_ target: NotificationTapTarget) {
        switch target {
        case .repo(let link):
            openedRepo = link
        case .web(let url):
            if let attention = Services.get(RunnerAttentionModel.self), attention.openHref(ping.href) {
                return
            }
            openURL(url) { (accepted: Bool) in
                if !accepted {
                    openNotice = FmNotice.error("Could not open \(url.absoluteString)")
                }
            }
        case .context:
            break
        }
    }

    private func sendToAgent(_ notification: ForkNotification) {
        Task { @MainActor in
            switch await PingAgentHandoff.route(notification) {
            case .copied:
                actionNotice = FmNotice(NotificationsCopy.pingCopied)
            case .open(let target):
                agentTarget = target
            case .pick(let pick):
                pickRepos = pick
            }
        }
    }

    private func delete(_ notification: ForkNotification) {
        if deleting { return }
        deleting = true
        Task { @MainActor in
            let failure: String? = await model.deletePing(notification)
            deleting = false
            if let failure = failure {
                actionNotice = FmNotice.error(failure)
            } else {
                dismiss()
            }
        }
    }
}

private struct PingDetailHero: View {
    @Environment(\.fmPalette) var fm
    let notification: ForkNotification

    var body: some View {
        VStack(alignment: .leading, spacing: FmSpace.x3) {
            HStack(alignment: .center, spacing: FmSpace.x3) {
                FmIcon(notificationIconForKind(notification.kind), size: 24, color: fm.accent)
                    .frame(width: 52, height: 52)
                    .background(notification.isUnread ? fm.accentSubtle : fm.bgRaised)
                    .clipShape(Circle())
                FlowPills(labels: notificationPills(notification))
                    .frame(maxWidth: .infinity, alignment: .leading)
            }
            Text(notification.title)
                .fmText(.titleLarge)
                .textSelection(.enabled)
            let text: String = notification.body.trimmingCharacters(in: .whitespacesAndNewlines)
            if !text.isEmpty {
                LinkedText(text, repo: referenceRepo(href: notification.href), commits: false)
                    .fmText(.bodyLarge, color: fm.textSecondary, height: 1.5)
                    .textSelection(.enabled)
            }
        }
    }
}
