// Node activity, network stats, funds received, the relay's activity log and the one ForkMesh Ads
// slot. State: FMTools' ActivityScreenModel; the relay log and node sync arrive through ToolsHooks.

import FMApi
import FMBase
import FMTools
import SwiftUI

struct ActivityDestination: View {
    var body: some View {
        ActivityScreen()
            .navigationTitle("Activity")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItemGroup(placement: .topBarTrailing) {
                    NavigationLink(value: Route.network(section: nil)) {
                        Image(systemName: "point.3.connected.trianglepath.dotted").accessibilityLabel("Network and nodes")
                    }
                    NavigationLink(value: Route.status) {
                        Image(systemName: "waveform.path.ecg").accessibilityLabel("Status")
                    }
                }
            }
    }
}

struct ActivityScreen: View {
    @Environment(\.fmPalette) private var fm
    @State private var model: ActivityScreenModel?

    init() {
        if let api = Services.get(ApiService.self) {
            _model = State(initialValue: ActivityScreenModel(api: api, hooks: Services.get(ToolsHooks.self)))
        } else {
            _model = State(initialValue: nil)
        }
    }

    var body: some View {
        if let model = model {
            ActivityContent(model: model)
                .task { await model.reload() }
        } else {
            FmEmptyState(icon: "bolt_outlined", title: "Activity is unavailable")
        }
    }
}

private struct ActivityContent: View {
    @Environment(\.fmPalette) private var fm
    @Bindable var model: ActivityScreenModel

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 0) {
                if let node = model.node {
                    NodeActivitySection(words: node, onSync: { model.syncNodeNow() })
                        .padding(.bottom, FmSpace.x5)
                }
                FmSectionHeader(title: "Network")
                    .padding(.bottom, FmSpace.x2)
                statsSection
                    .padding(.bottom, FmSpace.x5)
                fundsSection
                    .padding(.bottom, FmSpace.x5)
                ActivityLogSection(model: model)
                SponsoredAdCard(margin: EdgeInsets(top: FmSpace.x5, leading: 0, bottom: 0, trailing: 0))
            }
            .padding(FmSpace.x4)
        }
        .background(fm.bgBase)
        .fmRefreshable { await model.reload() }
    }

    @ViewBuilder private var statsSection: some View {
        switch model.stats {
        case .loading:
            FmSkeletonList(rows: 1, hasLeading: false, label: "Loading network activity")
                .frame(height: 96)
        case .failed(let message):
            ActivityErrorState(title: "Could not load network activity", message: message) {
                Task { await model.reload() }
            }
        case .loaded(let stats):
            HStack(spacing: FmSpace.x3) {
                ActivityStatCard(label: "Nodes online", value: "\(stats.nodesOnline)")
                ActivityStatCard(label: "Hosts online", value: "\(stats.hostsOnline)")
                ActivityStatCard(label: "Repositories", value: "\(stats.repos)")
            }
        }
    }

    @ViewBuilder private var fundsSection: some View {
        switch model.leaderboards {
        case .loading:
            FmSkeletonList(rows: 1, hasLeading: false, label: "Loading funds received")
                .frame(height: 96)
        case .failed(let message):
            ActivityErrorState(title: "Could not load funds received", message: message) {
                Task { await model.reload() }
            }
        case .loaded(let boards):
            FundsReceivedSection(boards: boards)
        }
    }
}

/// "Node activity": this phone's own node status (only for a mobile-node session).
private struct NodeActivitySection: View {
    @Environment(\.fmPalette) private var fm
    let words: NodeActivityPresentation
    let onSync: () -> Void

    var body: some View {
        VStack(alignment: .leading, spacing: FmSpace.x2) {
            FmSectionHeader(
                title: NodeActivityPresentation.sectionTitle, icon: "hub_outlined",
                trailing: FmStatusBadge(label: words.badge, color: words.badgeTone.color(fm)).fmAny)
            FmCard(color: fm.bgRaised, radius: FmRadius.lg) {
                card
            }
        }
    }

    private var card: some View {
        VStack(alignment: .leading, spacing: 0) {
            Text(words.title)
                .fmText(.titleSmall)
            Text(words.body)
                .fmText(.bodySmall, color: fm.textSecondary)
                .padding(.top, FmSpace.x1)
            ActivityTokens(labels: words.tokens)
                .padding(.top, FmSpace.x3)
            if !words.lastError.isEmpty {
                Text(words.lastError)
                    .fmText(.bodySmall, color: fm.danger)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .fmMessageTools(words.lastError)
                    .padding(.top, FmSpace.x2)
            }
            if let detail = words.needsDesktopDetail {
                FmNeedsDesktopNote(detail: detail)
                    .padding(.top, FmSpace.x3)
            }
            applier
            FmOutlinedButton(words.syncLabel, icon: words.syncing ? nil : "sync", onPressed: words.syncing ? nil : onSync)
                .accessibilityHint(Text(NodeActivityPresentation.syncTooltip))
                .padding(.top, FmSpace.x3)
        }
    }

    @ViewBuilder private var applier: some View {
        if !words.applierTokens.isEmpty {
            ActivityTokens(labels: words.applierTokens)
                .padding(.top, FmSpace.x3)
            if let note = words.appliedNote {
                Text(note)
                    .fmText(.bodySmall, color: fm.textTertiary)
                    .padding(.top, FmSpace.x2)
            }
            ForEach(words.leftReasonLines, id: \.self) { (line: String) in
                Text(line)
                    .fmText(.bodySmall, color: fm.textTertiary)
                    .padding(.top, FmSpace.x1)
            }
        }
    }
}

/// Small bordered labels that wrap onto several lines.
private struct ActivityTokens: View {
    @Environment(\.fmPalette) private var fm
    let labels: [String]

    var body: some View {
        ToolsFlowLayout(spacing: FmSpace.x2, lineSpacing: FmSpace.x1) {
            ForEach(Array(labels.enumerated()), id: \.offset) { (pair: (offset: Int, element: String)) in
                ActivityToken(label: pair.element)
            }
        }
    }
}

private struct ActivityToken: View {
    @Environment(\.fmPalette) private var fm
    let label: String

    var body: some View {
        Text(label)
            .font(Font.system(size: FmType.micro, weight: FmType.emphasis))
            .foregroundStyle(fm.textSecondary)
            .padding(.horizontal, FmSpace.x2)
            .padding(.vertical, FmSpace.x1)
            .background(fm.bgRaised)
            .clipShape(FmRoundedShape(radius: FmRadius.sm))
            .overlay { FmRoundedShape(radius: FmRadius.sm).strokeBorder(fm.border, lineWidth: 1) }
    }
}

private struct ActivityErrorState: View {
    @Environment(\.fmPalette) private var fm
    let title: String
    let message: String
    let onRetry: () -> Void

    var body: some View {
        FmCard(color: fm.bgRaised, radius: FmRadius.lg) {
            VStack(spacing: FmSpace.x2) {
                FmIcon("error_outline", size: 32, color: fm.danger)
                Text(title)
                    .fmText(.titleSmall)
                    .multilineTextAlignment(.center)
                Text(message)
                    .fmText(.bodyMedium, color: fm.textSecondary)
                    .multilineTextAlignment(.center)
                FmMessageTools(message)
                FmTextButton("Retry", icon: "refresh", onPressed: onRetry)
                    .accessibilityHint(Text("Retry network activity"))
            }
            .frame(maxWidth: .infinity)
        }
    }
}

private struct ActivityStatCard: View {
    @Environment(\.fmPalette) private var fm
    let label: String
    let value: String

    var body: some View {
        FmCard(padding: EdgeInsets.fmAll(FmSpace.x3), radius: FmRadius.lg) {
            VStack(alignment: .leading, spacing: FmSpace.x2) {
                Text(value)
                    .fmText(.displaySmall)
                    .lineLimit(1)
                    .minimumScaleFactor(0.6)
                Text(label)
                    .fmText(.bodySmall, color: fm.textSecondary)
                    .lineLimit(2)
            }
        }
    }
}

private struct FundsReceivedSection: View {
    @Environment(\.fmPalette) private var fm
    let boards: NetworkLeaderboards

    var body: some View {
        VStack(alignment: .leading, spacing: FmSpace.x2) {
            FmSectionHeader(title: "Funds received")
            FmCard(color: fm.bgRaised, radius: FmRadius.lg) {
                VStack(alignment: .leading, spacing: FmSpace.x3) {
                    Text(ActivityScreenModel.fundsNote)
                        .fmText(.bodySmall, color: fm.textSecondary)
                    groups
                }
            }
        }
    }

    @ViewBuilder private var groups: some View {
        if boards.isEmpty {
            FmEmptyState(
                icon: "leaderboard_outlined", title: "No funds received yet",
                message: "Public funds-received totals appear here after the mainnode reports them.")
        } else {
            let groups: [(title: String, rows: [(name: String, amount: String)])] = ActivityScreenModel.fundsGroups(boards)
            ForEach(Array(groups.enumerated()), id: \.offset) { (pair: (offset: Int, element: (title: String, rows: [(name: String, amount: String)]))) in
                FundsGroup(title: pair.element.title, rows: pair.element.rows)
            }
        }
    }
}

private struct FundsGroup: View {
    @Environment(\.fmPalette) private var fm
    let title: String
    let rows: [(name: String, amount: String)]

    var body: some View {
        VStack(alignment: .leading, spacing: FmSpace.x1) {
            Text(title)
                .font(Font.system(size: FmType.micro, weight: FmType.emphasis))
                .tracking(0.5)
                .foregroundStyle(fm.textTertiary)
                .padding(.bottom, FmSpace.x1)
            ForEach(Array(rows.enumerated()), id: \.offset) { (pair: (offset: Int, element: (name: String, amount: String))) in
                HStack(spacing: FmSpace.x3) {
                    Text(pair.element.name)
                        .fmText(.labelMedium)
                        .lineLimit(1)
                        .frame(maxWidth: .infinity, alignment: .leading)
                    Text(pair.element.amount)
                        .fmText(.bodySmall, color: fm.textSecondary, weight: FmType.emphasis)
                        .lineLimit(1)
                }
            }
        }
    }
}

private struct ActivityLogSection: View {
    @Environment(\.fmPalette) private var fm
    @Bindable var model: ActivityScreenModel

    var body: some View {
        VStack(alignment: .leading, spacing: FmSpace.x2) {
            FmSectionHeader(
                title: "Activity log",
                trailing: Text(model.logCountLabel).fmText(.bodySmall, color: fm.textTertiary, weight: FmType.emphasis).fmAny)
            FmCard(color: fm.bgRaised, radius: FmRadius.lg) {
                content
            }
        }
    }

    @ViewBuilder private var content: some View {
        let lines: [ActivityLogLine] = model.visibleLogs
        if lines.isEmpty {
            FmEmptyState(icon: "history", title: "No activity yet",
                         message: "Connection events and network updates will appear here.")
        } else {
            VStack(alignment: .leading, spacing: FmSpace.x2) {
                ForEach(lines) { (line: ActivityLogLine) in
                    ActivityLogRow(line: line)
                }
                if model.showsLogToggle {
                    FmTextButton(model.logToggleLabel, icon: model.logToggleIcon, onPressed: { model.showAllLogs.toggle() })
                        .padding(.top, FmSpace.x1)
                }
            }
        }
    }
}

private struct ActivityLogRow: View {
    @Environment(\.fmPalette) private var fm
    let line: ActivityLogLine

    var body: some View {
        HStack(spacing: FmSpace.x2) {
            FmIcon(line.icon, size: 16, color: line.tone.color(fm))
            Text(line.timestamp)
                .fmMono(size: FmType.micro, weight: FmType.emphasis, color: fm.textTertiary)
                .frame(width: 80, alignment: .leading)
            Text(line.body)
                .fmText(.bodySmall, color: fm.textSecondary)
                .lineLimit(2)
                .frame(maxWidth: .infinity, alignment: .leading)
        }
        .padding(.horizontal, FmSpace.x3)
        .padding(.vertical, FmSpace.x2)
        .background(fm.bgBase)
        .clipShape(FmRoundedShape(radius: FmRadius.md))
        .overlay { FmRoundedShape(radius: FmRadius.md).strokeBorder(fm.border, lineWidth: 1) }
    }
}
