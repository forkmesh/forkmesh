import FMApi
import FMSurfaces
import SwiftUI
import WidgetKit

struct OverviewWidget: Widget {
    var body: some WidgetConfiguration {
        StaticConfiguration(kind: SurfaceSection.overview.widgetKind, provider: SurfaceProvider()) { entry in
            OverviewWidgetView(entry: entry)
                .containerBackground(.fill.tertiary, for: .widget)
        }
        .configurationDisplayName("Overview")
        .description("Agents in total and running, open issues, pull requests, tasks and unread pings.")
        .supportedFamilies([.systemSmall, .systemMedium, .accessoryRectangular, .accessoryInline])
    }
}

struct OverviewStat: Identifiable {
    let id: String
    let label: String
    let value: String
    let systemImage: String
    let tint: Color
    let route: Route
}

struct OverviewWidgetView: View {
    @Environment(\.widgetFamily) private var family
    let entry: SurfaceEntry

    private var agents: AgentsSurface { entry.agents }
    private var work: WorkSurface { entry.work }
    private var pings: Int { entry.inbox.unread }

    private var stats: [OverviewStat] {
        [
            OverviewStat(
                id: "agents", label: "Agents", value: "\(agents.total)", systemImage: "cpu", tint: .primary,
                route: .agents(.all)),
            OverviewStat(
                id: "running", label: "Running", value: "\(agents.running)", systemImage: "bolt.fill", tint: .blue,
                route: .agents(.running)),
            OverviewStat(
                id: "issues", label: "Issues", value: WorkSurface.label(work.openIssues),
                systemImage: "smallcircle.filled.circle", tint: .green, route: .repos(.mine)),
            OverviewStat(
                id: "pulls", label: "PRs", value: WorkSurface.label(work.openPulls),
                systemImage: "arrow.triangle.pull", tint: .purple, route: .repos(.mine)),
            OverviewStat(
                id: "tasks", label: "Tasks", value: WorkSurface.label(work.openTasks, more: work.moreTasks),
                systemImage: "checklist", tint: .teal, route: .tasks(filter: nil)),
            OverviewStat(
                id: "pings", label: "Pings", value: "\(pings)", systemImage: "bell.fill",
                tint: pings > 0 ? .orange : .secondary, route: .inbox(.pings)),
        ]
    }

    private var inlineLine: String {
        guard entry.signedIn else { return "ForkMesh" }
        return "\(agents.running)/\(agents.total) agents · \(pings) pings"
    }

    var body: some View {
        Group {
            switch family {
            case .accessoryInline:
                Label(inlineLine, systemImage: "square.grid.2x2")
            case .accessoryRectangular:
                rectangular
            case .systemMedium:
                medium
            default:
                small
            }
        }
        .widgetURL(WidgetRoute.url(agents.needsYou > 0 ? .agents(.needsYou) : .agents(.all)))
    }

    @ViewBuilder private var rectangular: some View {
        if !entry.signedIn {
            Text("Sign in to ForkMesh")
                .font(.caption)
        } else {
            VStack(alignment: .leading, spacing: 1) {
                Label("\(agents.running) running of \(agents.total)", systemImage: "cpu")
                    .font(.caption.weight(.semibold))
                Text("\(WorkSurface.label(work.openIssues)) issues · \(WorkSurface.label(work.openPulls)) PRs")
                    .font(.caption2)
                Text("\(WorkSurface.label(work.openTasks, more: work.moreTasks)) tasks · \(pings) pings")
                    .font(.caption2)
            }
            .monospacedDigit()
            .frame(maxWidth: .infinity, alignment: .leading)
        }
    }

    @ViewBuilder private var small: some View {
        if !entry.signedIn {
            SignedOutWidgetView()
        } else {
            VStack(alignment: .leading, spacing: 6) {
                header
                Grid(alignment: .leading, horizontalSpacing: 8, verticalSpacing: 4) {
                    ForEach(0..<3, id: \.self) { row in
                        GridRow {
                            ForEach(stats[(row * 2)..<(row * 2 + 2)]) { stat in
                                OverviewCompactStat(stat: stat)
                            }
                        }
                    }
                }
                Spacer(minLength: 0)
            }
        }
    }

    @ViewBuilder private var medium: some View {
        if !entry.signedIn {
            SignedOutWidgetView()
        } else {
            VStack(alignment: .leading, spacing: 6) {
                header
                Grid(alignment: .leading, horizontalSpacing: 8, verticalSpacing: 6) {
                    ForEach(0..<2, id: \.self) { row in
                        GridRow {
                            ForEach(stats[(row * 3)..<(row * 3 + 3)]) { stat in
                                if let url = WidgetRoute.url(stat.route) {
                                    Link(destination: url) {
                                        OverviewTile(stat: stat)
                                    }
                                } else {
                                    OverviewTile(stat: stat)
                                }
                            }
                        }
                    }
                }
                Spacer(minLength: 0)
            }
        }
    }

    private var header: some View {
        HStack {
            Label("ForkMesh", systemImage: "square.grid.2x2")
                .font(.caption.weight(.semibold))
            Spacer(minLength: 4)
            if agents.needsYou > 0 {
                Label("\(agents.needsYou)", systemImage: "hand.raised.fill")
                    .font(.caption2.weight(.semibold))
                    .foregroundStyle(.orange)
                    .accessibilityLabel("\(agents.needsYou) need you")
            }
        }
    }
}

struct OverviewTile: View {
    let stat: OverviewStat

    var body: some View {
        VStack(alignment: .leading, spacing: 2) {
            Label {
                Text(stat.label)
            } icon: {
                Image(systemName: stat.systemImage)
            }
            .font(.caption2.weight(.semibold))
            .foregroundStyle(stat.tint)
            Text(stat.value)
                .font(.title2.weight(.bold))
                .monospacedDigit()
                .contentTransition(.numericText())
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .accessibilityElement(children: .combine)
    }
}

struct OverviewCompactStat: View {
    let stat: OverviewStat

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            Text(stat.value)
                .font(.headline.weight(.bold))
                .monospacedDigit()
                .contentTransition(.numericText())
            Label {
                Text(stat.label)
            } icon: {
                Image(systemName: stat.systemImage)
            }
            .font(.caption2)
            .foregroundStyle(stat.tint)
            .lineLimit(1)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .accessibilityElement(children: .combine)
    }
}
