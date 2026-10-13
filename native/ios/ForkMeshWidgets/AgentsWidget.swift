import AppIntents
import FMApi
import FMSurfaces
import SwiftUI
import WidgetKit

struct AgentsWidget: Widget {
    var body: some WidgetConfiguration {
        StaticConfiguration(kind: SurfaceSection.agents.widgetKind, provider: SurfaceProvider()) { entry in
            AgentsWidgetView(entry: entry)
                .containerBackground(.fill.tertiary, for: .widget)
        }
        .configurationDisplayName("Agents")
        .description("Runs that need you and runs in progress.")
        .supportedFamilies([
            .systemSmall, .systemMedium, .accessoryCircular, .accessoryRectangular, .accessoryInline,
        ])
    }
}

struct AgentsWidgetView: View {
    @Environment(\.widgetFamily) private var family
    let entry: SurfaceEntry

    var body: some View {
        Group {
            switch family {
            case .accessoryInline:
                inline
            case .accessoryCircular:
                circular
            case .accessoryRectangular:
                rectangular
            case .systemMedium:
                medium
            default:
                small
            }
        }
        .widgetURL(WidgetRoute.url(tapRoute))
    }

    private var surface: AgentsSurface { entry.agents }

    private var tapRoute: Route {
        surface.needsYou > 0 ? .agents(.needsYou) : .agents(.running)
    }

    private var inline: some View {
        Label(entry.signedIn ? surface.summaryLine : "ForkMesh", systemImage: "cpu")
    }

    private var circular: some View {
        ZStack {
            AccessoryWidgetBackground()
            VStack(spacing: 0) {
                Image(systemName: surface.needsYou > 0 ? "hand.raised.fill" : "cpu")
                    .font(.caption)
                Text("\(surface.needsYou > 0 ? surface.needsYou : surface.running)")
                    .font(.title3.weight(.bold))
                    .monospacedDigit()
            }
        }
        .accessibilityLabel(surface.summaryLine)
    }

    private var rectangular: some View {
        VStack(alignment: .leading, spacing: 1) {
            Label("Agents", systemImage: "cpu")
                .font(.caption.weight(.semibold))
            Text(entry.signedIn ? surface.summaryLine : "Sign in to ForkMesh")
                .font(.caption)
            if let latest = surface.latest {
                Text(latest.title)
                    .font(.caption2)
                    .foregroundStyle(.secondary)
                    .lineLimit(1)
                    .privacySensitive()
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
    }

    @ViewBuilder private var small: some View {
        if !entry.signedIn {
            SignedOutWidgetView()
        } else {
            VStack(alignment: .leading, spacing: 6) {
                HStack(alignment: .top) {
                    CountBadge(value: surface.needsYou, label: "Needs you", systemImage: "hand.raised.fill", tint: .orange)
                    Spacer(minLength: 4)
                    CountBadge(value: surface.running, label: "Running", systemImage: "bolt.fill", tint: .blue)
                }
                Spacer(minLength: 0)
                if let latest = surface.latest {
                    Text(latest.title)
                        .font(.caption.weight(.semibold))
                        .lineLimit(2)
                        .privacySensitive()
                    Text(latest.state.label)
                        .font(.caption2)
                        .foregroundStyle(latest.state.tint)
                } else {
                    Text("No agents running")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
            }
        }
    }

    @ViewBuilder private var medium: some View {
        if !entry.signedIn {
            SignedOutWidgetView()
        } else {
            VStack(alignment: .leading, spacing: 6) {
                HStack {
                    Label("Agents", systemImage: "cpu")
                        .font(.caption.weight(.semibold))
                    Spacer()
                    Text(surface.summaryLine)
                        .font(.caption2)
                        .foregroundStyle(.secondary)
                }
                if surface.runs.isEmpty {
                    Spacer(minLength: 0)
                    Text("No agent runs yet. Start one from the app.")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                    Spacer(minLength: 0)
                } else {
                    ForEach(surface.runs.prefix(3)) { run in
                        AgentWidgetRow(run: run)
                    }
                    Spacer(minLength: 0)
                }
            }
        }
    }
}

struct AgentWidgetRow: View {
    let run: AgentRunCard

    var body: some View {
        HStack(spacing: 8) {
            Button(intent: OpenSurfaceRouteIntent(route: run.route)) {
                HStack(spacing: 8) {
                    Image(systemName: run.state.systemImage)
                        .foregroundStyle(run.state.tint)
                        .frame(width: 16)
                    VStack(alignment: .leading, spacing: 0) {
                        Text(run.title)
                            .font(.caption.weight(.semibold))
                            .lineLimit(1)
                            .privacySensitive()
                        Text(run.detail.isEmpty ? "\(run.state.label) · \(run.repo)" : run.detail)
                            .font(.caption2)
                            .foregroundStyle(.secondary)
                            .lineLimit(1)
                            .privacySensitive()
                    }
                    Spacer(minLength: 0)
                }
                .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            if run.state.isActive && run.canStop {
                Button(intent: StopAgentRunIntent(runId: run.id)) {
                    Image(systemName: "stop.fill")
                        .font(.caption)
                }
                .buttonStyle(.bordered)
                .tint(.red)
                .accessibilityLabel("Stop \(run.title)")
            }
        }
    }
}
