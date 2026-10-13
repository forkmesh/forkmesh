import ActivityKit
import AppIntents
import FMApi
import FMSurfaces
import SwiftUI
import WidgetKit

struct AgentRunLiveActivity: Widget {
    var body: some WidgetConfiguration {
        ActivityConfiguration(for: AgentRunAttributes.self) { context in
            AgentRunLockScreenView(attributes: context.attributes, state: context.state)
                .padding(14)
                .activityBackgroundTint(Color.black.opacity(0.35))
                .widgetURL(WidgetRoute.url(context.state.approvalRoute ?? context.attributes.route))
        } dynamicIsland: { context in
            let state = context.state.state
            return DynamicIsland {
                DynamicIslandExpandedRegion(.leading) {
                    AgentFace(state: state, provider: context.state.provider)
                }
                DynamicIslandExpandedRegion(.trailing) {
                    AgentElapsed(state: context.state)
                        .font(.caption.weight(.semibold))
                        .monospacedDigit()
                        .frame(maxWidth: 70, alignment: .trailing)
                }
                DynamicIslandExpandedRegion(.center) {
                    Text(context.state.title ?? context.attributes.title)
                        .font(.footnote.weight(.semibold))
                        .lineLimit(1)
                }
                DynamicIslandExpandedRegion(.bottom) {
                    VStack(alignment: .leading, spacing: 6) {
                        Text(context.state.detail ?? state.label)
                            .font(.caption)
                            .foregroundStyle(.secondary)
                            .lineLimit(2)
                        AgentRunButtons(sessionId: context.attributes.sessionId, state: context.state)
                    }
                    .frame(maxWidth: .infinity, alignment: .leading)
                }
            } compactLeading: {
                AgentFace(state: state, provider: context.state.provider)
            } compactTrailing: {
                Image(systemName: state.systemImage)
                    .foregroundStyle(state.tint)
            } minimal: {
                Image(systemName: state == .needsApproval ? "hand.raised.fill" : "cpu")
                    .foregroundStyle(state.tint)
            }
            .widgetURL(WidgetRoute.url(context.state.approvalRoute ?? context.attributes.route))
            .keylineTint(state.tint)
        }
    }
}

struct AgentFace: View {
    let state: AgentRunState
    let provider: String?

    var body: some View {
        ZStack {
            Circle()
                .fill(state.tint.opacity(0.25))
            Text(initial)
                .font(.caption.weight(.bold))
                .foregroundStyle(state.tint)
        }
        .frame(width: 24, height: 24)
        .accessibilityLabel(provider ?? "Agent")
    }

    private var initial: String {
        guard let first = provider?.trimmingCharacters(in: .whitespaces).first else { return "A" }
        return String(first).uppercased()
    }
}

struct AgentElapsed: View {
    let state: AgentRunAttributes.ContentState

    var body: some View {
        if state.state.isActive, let started = state.startedDate {
            Text(started, style: .timer)
        } else {
            Text(state.state.label)
        }
    }
}

struct AgentRunButtons: View {
    let sessionId: String
    let state: AgentRunAttributes.ContentState

    var body: some View {
        HStack(spacing: 8) {
            if state.needsApproval, let approvalId = state.approvalId, !approvalId.isEmpty {
                Button(intent: ApproveAgentRequestIntent(approvalId: approvalId)) {
                    Label("Approve", systemImage: "checkmark")
                        .font(.caption.weight(.semibold))
                }
                .tint(.green)
            }
            if state.state.isActive {
                Button(intent: StopAgentRunIntent(runId: sessionId)) {
                    Label("Stop", systemImage: "stop.fill")
                        .font(.caption.weight(.semibold))
                }
                .tint(.red)
            }
        }
        .buttonStyle(.bordered)
    }
}

struct AgentRunLockScreenView: View {
    let attributes: AgentRunAttributes
    let state: AgentRunAttributes.ContentState

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack(spacing: 10) {
                AgentFace(state: state.state, provider: state.provider)
                VStack(alignment: .leading, spacing: 1) {
                    Text(state.title ?? attributes.title)
                        .font(.subheadline.weight(.semibold))
                        .lineLimit(1)
                    Text(state.repo ?? attributes.repo)
                        .font(.caption)
                        .foregroundStyle(.secondary)
                        .lineLimit(1)
                }
                Spacer(minLength: 4)
                VStack(alignment: .trailing, spacing: 1) {
                    Label(state.state.label, systemImage: state.state.systemImage)
                        .font(.caption.weight(.semibold))
                        .foregroundStyle(state.state.tint)
                    AgentElapsed(state: state)
                        .font(.caption2)
                        .monospacedDigit()
                        .foregroundStyle(.secondary)
                }
            }
            if let detail = state.detail, !detail.isEmpty {
                Text(detail)
                    .font(.caption)
                    .lineLimit(2)
            }
            if let progress = state.progress {
                ProgressView(value: progress)
                    .tint(state.state.tint)
            }
            AgentRunButtons(sessionId: attributes.sessionId, state: state)
        }
    }
}
