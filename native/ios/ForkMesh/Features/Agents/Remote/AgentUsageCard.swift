import FMAgents
import SwiftUI

struct AgentUsageCard: View {
    @Environment(\.fmPalette) private var fm
    @State private var model: AgentUsageModel

    init(client: AgentOpsClient) {
        _model = State(initialValue: AgentUsageModel(client: client))
    }

    var body: some View {
        FmCard(radius: FmRadius.lg) {
            VStack(alignment: .leading, spacing: FmSpace.x3) {
                HStack {
                    Text("Usage")
                        .fmText(.titleSmall)
                    Spacer()
                    if model.loading { ProgressView().controlSize(.small) }
                }
                content
            }
        }
        .task { await load() }
    }

    @ViewBuilder private var content: some View {
        if let summary = model.summary {
            ForEach(summary.providers) { provider in
                UsageProviderBlock(provider: provider, nowMs: model.nowMs)
            }
            if let total = summary.totalSpendUsd {
                Text("API spend this month: \(UsageFormat.usd(total))")
                    .fmText(.bodySmall, color: fm.textSecondary)
            }
        } else if model.unsupported {
            Text("Update ForkMesh on the desktop to see its usage here.")
                .fmText(.bodySmall, color: fm.textSecondary)
        } else if !model.errorText.isEmpty {
            Text(model.errorText)
                .fmText(.bodySmall, color: fm.danger)
                .fmMessageTools(model.errorText)
        }
    }

    private func load() async {
        await model.refresh()
        await AgentOpsFeature.scheduleUsageReminders(model.reminders, nowMs: model.nowMs)
    }
}

private struct UsageProviderBlock: View {
    @Environment(\.fmPalette) private var fm
    let provider: UsageProviderMeter
    let nowMs: Int64

    var body: some View {
        VStack(alignment: .leading, spacing: FmSpace.x2) {
            HStack {
                Text(provider.label)
                    .fmText(.labelLarge)
                Spacer()
                if let account = provider.activeAccount, provider.accounts.count > 1 {
                    Text(account.label)
                        .fmText(.labelSmall, color: fm.textSecondary)
                }
            }
            ForEach(provider.windows) { window in
                UsageMeterRow(window: window, nowMs: nowMs)
            }
            if provider.credits != nil || provider.id == UsageCreditsRow.provider {
                UsageCreditsRow(credits: provider.credits)
            }
        }
    }
}

struct UsageCreditsRow: View {
    static let provider: String = "claude-code"

    @Environment(\.fmPalette) private var fm
    let credits: UsageCredits?

    private var tint: Color {
        switch credits?.state ?? .unknown {
        case .exhausted: return fm.danger
        case .high: return fm.warning
        case .ok: return fm.accent
        case .unknown: return fm.textTertiary
        }
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            HStack {
                Text("Usage credits")
                    .fmText(.bodySmall)
                Spacer()
                Text(credits?.caption ?? "Off, or not reported by the desktop")
                    .fmText(.labelSmall, color: fm.textSecondary)
            }
            ProgressView(value: Double(credits?.usedPercent ?? 0), total: 100)
                .tint(tint)
        }
        .accessibilityElement(children: .combine)
        .accessibilityIdentifier("usage-credits-row")
    }
}

struct UsageMeterRow: View {
    @Environment(\.fmPalette) private var fm
    let window: UsageWindowMeter
    let nowMs: Int64

    private var tint: Color {
        switch window.state {
        case .exhausted: return fm.danger
        case .high: return fm.warning
        case .ok: return fm.success
        case .unknown: return fm.textTertiary
        }
    }

    private var caption: String {
        let used = window.usedPercent.map { "\($0)% used" } ?? "No reading yet"
        let reset = window.resetLabel(nowMs: nowMs)
        return reset.isEmpty ? used : "\(used) · \(reset)"
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            HStack {
                Text(window.label)
                    .fmText(.bodySmall)
                Spacer()
                Text(caption)
                    .fmText(.labelSmall, color: fm.textSecondary)
            }
            ProgressView(value: Double(window.usedPercent ?? 0), total: 100)
                .tint(tint)
        }
        .accessibilityElement(children: .combine)
    }
}
