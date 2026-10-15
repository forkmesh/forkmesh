import ActivityKit
import AppIntents
import FMApi
import FMPlanning
import FMSurfaces
import SwiftUI
import WidgetKit

typealias SummaryState = ActivitySummaryAttributes.ContentState

struct SummaryLiveActivity: Widget {
    var body: some WidgetConfiguration {
        ActivityConfiguration(for: ActivitySummaryAttributes.self) { context in
            SummaryLockScreenView(state: context.state)
                .padding(14)
                .activityBackgroundTint(Color.black.opacity(0.35))
                .widgetURL(WidgetRoute.url(context.state.route))
        } dynamicIsland: { context in
            let state = context.state
            return DynamicIsland {
                DynamicIslandExpandedRegion(.leading) {
                    SummaryBadge(state: state)
                        .font(.footnote.weight(.semibold))
                }
                DynamicIslandExpandedRegion(.center) {
                    Text(state.summaryLine)
                        .font(.footnote.weight(.semibold))
                        .lineLimit(1)
                }
                DynamicIslandExpandedRegion(.bottom) {
                    SummaryRows(state: state, agentLimit: 2, shortcutLimit: 1)
                }
            } compactLeading: {
                SummaryBadge(state: state)
            } compactTrailing: {
                SummaryCompactTrailing(state: state)
            } minimal: {
                Image(systemName: state.systemImage)
                    .foregroundStyle(state.tint)
            }
            .widgetURL(WidgetRoute.url(state.route))
            .keylineTint(state.tint)
        }
    }
}

extension ActivitySummaryAttributes.ContentState {
    var tint: Color {
        if activeAgents > 0 { return leadState.tint }
        if shortcutsRunning > 0 { return .blue }
        if timer != nil { return .orange }
        return AgentRunState.completed.tint
    }
}

struct SummaryBadge: View {
    let state: SummaryState

    var body: some View {
        HStack(spacing: 3) {
            Image(systemName: state.systemImage)
            if state.compactCount > 1 {
                Text("\(state.compactCount)")
                    .monospacedDigit()
            }
        }
        .foregroundStyle(state.tint)
        .accessibilityLabel(state.summaryLine)
    }
}

struct SummaryCompactTrailing: View {
    let state: SummaryState

    var body: some View {
        if let shortcut = state.leadShortcut, let start = shortcut.startedAt, let end = shortcut.expectedEnd {
            ProgressView(timerInterval: start...end, countsDown: false) {
                EmptyView()
            } currentValueLabel: {
                EmptyView()
            }
            .progressViewStyle(.circular)
            .tint(.blue)
            .frame(width: 18, height: 18)
        } else if let timer = state.timer, state.activeAgents == 0 {
            TimerClockText(timer: timer)
                .monospacedDigit()
                .frame(maxWidth: 56)
        } else if let started = state.lead?.startedAt ?? state.leadShortcut?.startedAt {
            Text(started, style: .timer)
                .monospacedDigit()
                .frame(maxWidth: 56)
        } else {
            Image(systemName: state.leadState.systemImage)
                .foregroundStyle(state.tint)
        }
    }
}

struct SummaryRows: View {
    let state: SummaryState
    let agentLimit: Int
    let shortcutLimit: Int
    var showsDetail = false

    var body: some View {
        let agents = Array(state.agents.prefix(agentLimit))
        let shortcuts = Array(state.shortcuts.prefix(shortcutLimit))
        VStack(alignment: .leading, spacing: 6) {
            ForEach(agents) { agent in
                SummaryAgentRow(agent: agent, showsDetail: showsDetail && agent.state == .needsApproval)
            }
            if state.activeAgents > agents.count {
                SummaryMoreRow(count: state.activeAgents - agents.count, noun: "agent")
            }
            ForEach(shortcuts) { shortcut in
                SummaryShortcutRow(shortcut: shortcut)
            }
            if state.shortcutsRunning > shortcuts.count {
                SummaryMoreRow(count: state.shortcutsRunning - shortcuts.count, noun: "shortcut")
            }
            if let timer = state.timer {
                SummaryTimerRow(timer: timer)
            }
            SummaryButtons(state: state)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
    }
}

struct SummaryAgentRow: View {
    let agent: ActivitySummaryAttributes.Agent
    let showsDetail: Bool

    var body: some View {
        Link(destination: WidgetRoute.url(agent.route)) {
            VStack(alignment: .leading, spacing: 1) {
                HStack(spacing: 6) {
                    Image(systemName: agent.state.systemImage)
                        .foregroundStyle(agent.state.tint)
                        .frame(width: 16)
                    Text(agent.title)
                        .lineLimit(1)
                    Spacer(minLength: 4)
                    if agent.state == .running, let started = agent.startedAt {
                        Text(started, style: .timer)
                            .monospacedDigit()
                            .foregroundStyle(.secondary)
                            .multilineTextAlignment(.trailing)
                            .frame(maxWidth: 60, alignment: .trailing)
                    } else {
                        Text(agent.state.label)
                            .foregroundStyle(agent.state.tint)
                    }
                }
                .font(.caption.weight(.semibold))
                if showsDetail && !agent.detail.isEmpty {
                    Text(agent.detail)
                        .font(.caption2)
                        .foregroundStyle(.secondary)
                        .lineLimit(2)
                        .padding(.leading, 22)
                }
            }
        }
    }
}

struct SummaryShortcutRow: View {
    let shortcut: ActivitySummaryAttributes.Shortcut

    var body: some View {
        Link(destination: WidgetRoute.url(shortcut.route)) {
            VStack(alignment: .leading, spacing: 3) {
                HStack(spacing: 6) {
                    Image(systemName: "bolt.horizontal.fill")
                        .foregroundStyle(.blue)
                        .frame(width: 16)
                    Text(shortcut.title)
                        .lineLimit(1)
                    Spacer(minLength: 4)
                    if let started = shortcut.startedAt {
                        Text(started, style: .timer)
                            .monospacedDigit()
                            .foregroundStyle(.secondary)
                            .multilineTextAlignment(.trailing)
                            .frame(maxWidth: 60, alignment: .trailing)
                    } else {
                        Text("Running")
                            .foregroundStyle(.secondary)
                    }
                }
                .font(.caption.weight(.semibold))
                if let start = shortcut.startedAt, let end = shortcut.expectedEnd {
                    ProgressView(timerInterval: start...end, countsDown: false) {
                        EmptyView()
                    } currentValueLabel: {
                        EmptyView()
                    }
                    .tint(.blue)
                    .padding(.leading, 22)
                }
            }
        }
    }
}

struct SummaryTimerRow: View {
    let timer: TimerActivitySnapshot

    var body: some View {
        Link(destination: WidgetRoute.url(timer.route)) {
            HStack(spacing: 6) {
                Image(systemName: timer.kind.systemImage)
                    .foregroundStyle(.orange)
                    .frame(width: 16)
                Text(timer.label)
                    .lineLimit(1)
                Spacer(minLength: 4)
                TimerClockText(timer: timer)
                    .monospacedDigit()
                    .multilineTextAlignment(.trailing)
                    .frame(maxWidth: 80, alignment: .trailing)
            }
            .font(.caption.weight(.semibold))
        }
    }
}

struct SummaryMoreRow: View {
    let count: Int
    let noun: String

    var body: some View {
        Text("+\(count) more \(noun)\(count == 1 ? "" : "s")")
            .font(.caption2)
            .foregroundStyle(.secondary)
            .padding(.leading, 22)
    }
}

struct SummaryButtons: View {
    let state: SummaryState

    var body: some View {
        if let approval = state.pendingApproval {
            Button(intent: ApproveAgentRequestIntent(approvalId: approval.approvalId)) {
                Label("Approve", systemImage: "checkmark")
                    .font(.caption.weight(.semibold))
            }
            .buttonStyle(.bordered)
            .tint(.green)
        } else if let agent = state.stoppable {
            Button(intent: StopAgentRunIntent(runId: agent.id)) {
                Label("Stop", systemImage: "stop.fill")
                    .font(.caption.weight(.semibold))
            }
            .buttonStyle(.bordered)
            .tint(.red)
        }
    }
}

struct SummaryLockScreenView: View {
    let state: SummaryState

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack(spacing: 8) {
                Image(systemName: state.systemImage)
                    .font(.subheadline.weight(.semibold))
                    .foregroundStyle(state.tint)
                Text(state.summaryLine)
                    .font(.subheadline.weight(.semibold))
                    .lineLimit(1)
                Spacer(minLength: 4)
                if state.unread > 0 {
                    Label("\(state.unread)", systemImage: "tray.full")
                        .font(.caption.weight(.semibold))
                        .foregroundStyle(.secondary)
                        .accessibilityLabel("\(state.unread) unread")
                }
            }
            SummaryRows(
                state: state, agentLimit: ActivitySummaryAttributes.maxAgents,
                shortcutLimit: ActivitySummaryAttributes.maxShortcuts, showsDetail: true)
        }
    }
}
