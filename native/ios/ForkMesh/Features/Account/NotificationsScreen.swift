// The account inbox ("Pings") with its filters, swipe to delete, delete all, the tap that opens
// the ping's detail page, and "Send to agent". Logic: FMApi's
// NotificationsScreenModel and NotificationsLogic.

import FMApi
import FMBase
import SwiftUI

@MainActor
struct NotificationsScreen: View {
    @Environment(\.fmPalette) var fm

    @State var model: NotificationsScreenModel?
    @State var detailPing: ForkNotification?
    @State var agentTarget: PingAgentTarget?
    @State var pickRepos: PingRepoPick?
    @State var notice: FmNotice?
    @State var showPingSettings: Bool = false

    init() {
        if let service = Services.get(NotificationService.self) {
            let api: ApiService? = Services.get(ApiService.self)
            let screen: NotificationsScreenModel = api.map { NotificationsScreenModel(service: service, api: $0) }
                ?? NotificationsScreenModel(service: service)
            _model = State(initialValue: screen)
        } else {
            _model = State(initialValue: nil)
        }
    }

    var body: some View {
        Group {
            if let model = model {
                content(model)
            } else {
                FmEmptyState(icon: notificationLineIcon, title: NotificationsCopy.loadErrorTitle)
            }
        }
        .background(fm.bgBase.ignoresSafeArea())
        .navigationBarTitleDisplayMode(.inline)
    }

    private func content(_ model: NotificationsScreenModel) -> some View {
        VStack(spacing: 0) {
            NotificationFilterBar(model: model)
            if model.notice != nil {
                FmNoticeLine(model.notice, isError: model.noticeIsError, onDismiss: { model.notice = nil })
                    .padding(.horizontal, FmSpace.x4)
                    .padding(.bottom, FmSpace.x2)
                    .accessibilityIdentifier("notifications-notice")
            }
            if notice != nil {
                FmNoticeLine($notice)
                    .padding(.horizontal, FmSpace.x4)
                    .padding(.bottom, FmSpace.x2)
            }
            NotificationsBodyView(
                model: model,
                onOpen: { (n: ForkNotification) in detailPing = n },
                onSendToAgent: { (n: ForkNotification) in sendToAgent(n) })
        }
        .toolbar {
            ToolbarItem(placement: .principal) {
                VStack(spacing: FmSpace.x1) {
                    Text("Notifications").fmText(.titleMedium).lineLimit(1)
                    Text(model.unreadLabel).fmText(.bodySmall, color: fm.textTertiary, weight: FmType.emphasis)
                }
            }
            ToolbarItemGroup(placement: .topBarTrailing) {
                if model.canMarkAllRead {
                    Button("Mark all read") {
                        Task { await model.markAllRead() }
                    }
                }
                Button {
                    Task { await model.deleteAll() }
                } label: {
                    FmIcon(
                        "delete_sweep_outlined", size: 24, color: model.canDeleteAll ? fm.danger : fm.textDisabled,
                        label: NotificationsCopy.deleteAll)
                }
                .disabled(!model.canDeleteAll)
                .accessibilityIdentifier("notifications-delete-all")
                Button {
                    showPingSettings = true
                } label: {
                    FmIcon("settings_outlined", size: 22, color: fm.textPrimary, label: "Ping settings")
                }
                .accessibilityIdentifier("notifications-ping-settings")
                Menu {
                    Button(NotificationsCopy.refresh) {
                        Task { await model.refresh() }
                    }
                } label: {
                    FmIcon("more_vert", size: 24, color: fm.textPrimary, label: "More notification actions")
                }
            }
        }
        .task { await model.start() }
        .navigationDestination(isPresented: $showPingSettings) {
            PingSettingsScreen()
                .shellPageClearance()
        }
        .navigationDestination(item: $detailPing) { (ping: ForkNotification) in
            PingDetailScreen(model: model, ping: ping)
                .shellPageClearance()
        }
        .navigationDestination(item: $agentTarget) { (target: PingAgentTarget) in
            PingAgentHandoff.destination(target)
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
    }

    private func sendToAgent(_ notification: ForkNotification) {
        Task { @MainActor in
            switch await PingAgentHandoff.route(notification) {
            case .copied:
                notice = FmNotice(NotificationsCopy.pingCopied)
            case .open(let target):
                agentTarget = target
            case .pick(let pick):
                pickRepos = pick
            }
        }
    }
}

/// The repositories to choose from, named ones first.
struct PingRepoPick: Identifiable {
    let id: UUID = UUID()
    let candidates: [PingAgentCandidate]
    let prompt: String
}

// MARK: Filters

private struct NotificationFilterBar: View {
    @Environment(\.fmPalette) var fm
    @Bindable var model: NotificationsScreenModel

    var body: some View {
        ScrollView(.horizontal, showsIndicators: false) {
            HStack(spacing: FmSpace.x2) {
                ForEach(notificationFilters, id: \.self) { (filter: String) in
                    NotificationFilterChip(
                        label: filter,
                        selected: filter == model.filter,
                        onSelected: {
                            FmHaptics.selection()
                            model.filter = filter
                        })
                }
            }
            .padding(.horizontal, FmSpace.x4)
        }
        .frame(height: 56)
    }
}

/// A ChoiceChip without the checkmark.
private struct NotificationFilterChip: View {
    @Environment(\.fmPalette) var fm
    let label: String
    let selected: Bool
    let onSelected: () -> Void

    var body: some View {
        Button(action: onSelected) {
            Text(label)
                .font(FmFonts.ui(size: FmType.small, weight: selected ? FmType.emphasis : FmType.regular))
                .foregroundStyle(selected ? fm.textPrimary : fm.textSecondary)
                .padding(.horizontal, FmSpace.x3)
                .frame(minHeight: 32)
                .background(selected ? fm.accentSubtle : fm.bgRaised)
                .clipShape(FmRoundedShape(radius: FmRadius.sm))
                .overlay {
                    FmRoundedShape(radius: FmRadius.sm)
                        .strokeBorder(selected ? fm.accent.opacity(0.22) : fm.border, lineWidth: 1)
                }
        }
        .buttonStyle(FmPressStyle(highlight: fm.textPrimary.opacity(0.06), cornerRadius: FmRadius.sm))
        .accessibilityAddTraits(selected ? [.isSelected] : [])
    }
}

// MARK: Body

private struct NotificationsBodyView: View {
    @Environment(\.fmPalette) var fm
    let model: NotificationsScreenModel
    let onOpen: (ForkNotification) -> Void
    let onSendToAgent: (ForkNotification) -> Void

    var body: some View {
        let state: NotificationsBody = model.body
        FmSwap(stateKey: state.stateKey) {
            switch state {
            case .loading:
                FmSkeletonList(rows: 6, label: "Loading notifications")
            case .error(let message):
                NotificationsEmptyView(
                    icon: "cloud_off_outlined",
                    title: NotificationsCopy.loadErrorTitle,
                    message: message,
                    onRetry: { Task { await model.refresh() } })
            case .empty(let filter):
                let copy: NotificationsEmptyCopy = NotificationsEmptyCopy(filter: filter)
                NotificationsEmptyView(icon: copy.icon, title: copy.title, message: copy.message, onRetry: nil)
            case .items(_, let items):
                NotificationsList(model: model, items: items, onOpen: onOpen, onSendToAgent: onSendToAgent)
            }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }
}

private struct NotificationsList: View {
    @Environment(\.fmPalette) var fm
    let model: NotificationsScreenModel
    let items: [ForkNotification]
    let onOpen: (ForkNotification) -> Void
    let onSendToAgent: (ForkNotification) -> Void

    private var header: some View {
        Text(notificationsUpdatesLabel(items.count))
            .fmText(.titleSmall)
            .listRowInsets(EdgeInsets(top: FmSpace.x2, leading: FmSpace.x4, bottom: FmSpace.x2, trailing: FmSpace.x4))
            .listRowSeparator(.hidden)
            .listRowBackground(Color.clear)
    }

    private func row(_ notification: ForkNotification) -> some View {
        NotificationRow(
            notification: notification,
            onTap: { onOpen(notification) },
            onSendToAgent: { onSendToAgent(notification) })
            .id(model.rowKey(notification))
            .listRowInsets(EdgeInsets(top: 0, leading: FmSpace.x4, bottom: 0, trailing: FmSpace.x4))
            .listRowSeparator(.hidden)
            .listRowBackground(Color.clear)
            .swipeActions(edge: .trailing, allowsFullSwipe: true) {
                // A row without an id cannot be named to the Worker.
                if !notification.id.isEmpty {
                    Button(role: .destructive) {
                        Task { await model.delete(notification) }
                    } label: {
                        Text("Delete")
                    }
                }
            }
    }

    var body: some View {
        List {
            header
            ForEach(items) { (notification: ForkNotification) in
                row(notification)
            }
        }
        .listStyle(.plain)
        .scrollContentBackground(.hidden)
        .fmRefreshable { await model.refresh() }
    }
}

/// The whole message, never a teaser.
private struct NotificationRow: View {
    @Environment(\.fmPalette) var fm
    let notification: ForkNotification
    let onTap: () -> Void
    let onSendToAgent: () -> Void

    private var tone: Color {
        return notification.isUnread ? fm.accent : fm.textTertiary
    }

    private var titleLine: some View {
        HStack(alignment: .top, spacing: FmSpace.x2) {
            Text(notification.title)
                .font(FmFonts.ui(size: FmType.body, weight: notification.isUnread ? FmType.emphasis : FmType.regular))
                .foregroundStyle(fm.textPrimary)
                .frame(maxWidth: .infinity, alignment: .leading)
            let time: String = notificationTimeLabel(notification.ts)
            if !time.isEmpty {
                Text(time)
                    .fmText(.bodyMedium, color: fm.textTertiary, weight: FmType.medium, tabular: true)
            }
        }
    }

    private var pills: some View {
        HStack(spacing: FmSpace.x2) {
            FlowPills(labels: notificationPills(notification))
                .frame(maxWidth: .infinity, alignment: .leading)
            // The same paper airplane as the desktop Pings grid.
            FmIconButton(icon: "send_outlined", label: "Send to agent", size: 18, color: fm.accent, onPressed: onSendToAgent)
                .frame(width: 36, height: 36)
                .accessibilityIdentifier("ping-to-agent-\(notification.id)")
        }
    }

    var body: some View {
        Button(action: onTap) {
            HStack(alignment: .top, spacing: FmSpace.x3) {
                FmIcon(notificationIconForKind(notification.kind), size: 24, color: tone)
                    .frame(width: 52, height: 52)
                    .background(notification.isUnread ? fm.accentSubtle : fm.bgRaised)
                    .clipShape(Circle())
                VStack(alignment: .leading, spacing: FmSpace.x2) {
                    titleLine
                    pills
                    Text(notificationPreview(notification))
                        .fmText(.bodyMedium, color: fm.textSecondary, height: 1.45)
                    if let context = notificationContextLabel(notification) {
                        Text(context)
                            .fmText(.bodySmall, color: fm.accent, weight: FmType.emphasis)
                            .lineLimit(1)
                            .truncationMode(.tail)
                    }
                }
            }
            .padding(.vertical, FmSpace.x3)
            .frame(maxWidth: .infinity, alignment: .leading)
            .overlay(alignment: .bottom) {
                Rectangle().fill(fm.border).frame(height: 0.8)
            }
            .contentShape(Rectangle())
        }
        .buttonStyle(FmPressStyle(highlight: fm.textPrimary.opacity(0.04), cornerRadius: FmRadius.md))
    }
}

/// The small bordered pills of a ping (kind, repository, @actor), wrapping.
struct FlowPills: View {
    let labels: [String]

    var body: some View {
        ViewThatFits(in: .horizontal) {
            HStack(spacing: FmSpace.x2) {
                ForEach(labels, id: \.self) { (label: String) in
                    MiniPill(label: label)
                }
            }
            VStack(alignment: .leading, spacing: FmSpace.x1) {
                ForEach(labels, id: \.self) { (label: String) in
                    MiniPill(label: label)
                }
            }
        }
    }
}

struct MiniPill: View {
    @Environment(\.fmPalette) var fm
    let label: String

    var body: some View {
        Text(label)
            .font(FmFonts.ui(size: FmType.micro, weight: FmType.emphasis))
            .foregroundStyle(fm.textSecondary)
            .lineLimit(1)
            .padding(.horizontal, FmSpace.x2)
            .padding(.vertical, FmSpace.x1)
            .background(fm.bgRaised)
            .clipShape(FmRoundedShape(radius: FmRadius.sm))
            .overlay { FmRoundedShape(radius: FmRadius.sm).strokeBorder(fm.border, lineWidth: 1) }
    }
}

private struct NotificationsEmptyView: View {
    let icon: String
    let title: String
    let message: String
    let onRetry: (() -> Void)?

    var body: some View {
        VStack(spacing: FmSpace.x3) {
            FmEmptyState(icon: icon, title: title, message: message)
                .fixedSize(horizontal: false, vertical: true)
            if let onRetry = onRetry {
                FmTextButton("Retry", icon: "refresh", onPressed: onRetry)
            }
        }
        .padding(FmSpace.x5)
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }
}

// MARK: Sheets

struct PingRepoPickerSheet: View {
    @Environment(\.fmPalette) var fm
    let pick: PingRepoPick
    let runningRepo: String?
    let onPick: (PingAgentCandidate) -> Void

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            Text(NotificationsCopy.pickRepoTitle)
                .fmText(.titleMedium)
                .padding(FmSpace.x4)
            ForEach(pick.candidates) { (candidate: PingAgentCandidate) in
                let running: Bool = candidate.name == runningRepo
                FmListRow(
                    title: candidate.name,
                    subtitle: pingRepoSubtitle(candidate.summary, runningRepo: runningRepo),
                    leading: FmIcon("auto_awesome_outlined", size: 24, color: running ? fm.accent : fm.textTertiary).fmAny,
                    onTap: { onPick(candidate) },
                    showChevron: false)
            }
        }
        .padding(.bottom, FmSpace.x3)
        .fmPreGlassBackground(fm.bgRaised)
    }
}
