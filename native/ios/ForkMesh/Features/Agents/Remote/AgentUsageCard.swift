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
                NavigationLink {
                    UsageScreen(usage: model)
                        .shellPageClearance()
                } label: {
                    HStack {
                        Text("Usage")
                            .fmText(.titleSmall)
                        Spacer()
                        if model.loading { ProgressView().controlSize(.small) }
                        Image(systemName: "chevron.right")
                            .font(.system(size: 13, weight: .semibold))
                            .foregroundStyle(fm.textTertiary)
                    }
                    .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
                .accessibilityHint(Text("Opens Usage: accounts, sign-in and new terminals"))
                .accessibilityIdentifier("agent-ops-usage-open")
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

    private var pace: String {
        [window.periodLabel(nowMs: nowMs), window.paceLabel(nowMs: nowMs)]
            .filter { !$0.isEmpty }
            .joined(separator: " · ")
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
            UsagePaceBar(
                usedPercent: window.usedPercent ?? 0, tint: tint,
                segments: window.elapsedFraction(nowMs: nowMs) == nil ? 1 : window.segments,
                elapsed: window.elapsedFraction(nowMs: nowMs))
            if !pace.isEmpty {
                Text(pace)
                    .fmText(.labelSmall, color: fm.textTertiary)
            }
        }
        .accessibilityElement(children: .combine)
    }
}

struct UsagePaceBar: View {
    @Environment(\.fmPalette) private var fm
    let usedPercent: Int
    let tint: Color
    let segments: Int
    let elapsed: Double?

    private let barHeight: CGFloat = 6
    private let gap: CGFloat = 2

    var body: some View {
        GeometryReader { geo in
            let width = geo.size.width
            let count = max(1, segments)
            let segmentWidth = max(0, (width - gap * CGFloat(count - 1)) / CGFloat(count))
            let filled = width * CGFloat(min(max(usedPercent, 0), 100)) / 100
            ZStack(alignment: .topLeading) {
                ZStack(alignment: .leading) {
                    Rectangle().fill(fm.bgOverlay)
                    Rectangle().fill(tint).frame(width: filled)
                }
                .frame(height: barHeight)
                .mask(
                    HStack(spacing: gap) {
                        ForEach(0..<count, id: \.self) { _ in
                            Rectangle().frame(width: segmentWidth, height: barHeight)
                        }
                    }
                )
                .clipShape(Capsule())
                .offset(y: 4)
                if let elapsed {
                    Rectangle()
                        .fill(fm.textPrimary)
                        .frame(width: 1.5, height: 3)
                        .offset(x: width * CGFloat(elapsed) - 0.75, y: 0)
                    Rectangle()
                        .fill(fm.textPrimary)
                        .frame(width: 1.5, height: 3)
                        .offset(x: width * CGFloat(elapsed) - 0.75, y: 4 + barHeight + 1)
                }
            }
        }
        .frame(height: barHeight + 8)
        .accessibilityHidden(true)
    }
}
