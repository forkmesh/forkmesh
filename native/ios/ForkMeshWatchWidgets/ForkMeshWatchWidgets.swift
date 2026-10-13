// The watch's complications and Smart Stack widget (docs/native/PARITY.md,
// 4.3). They read the last `ComplicationSummary` the watch app left in the
// keychain group the two share; the app reloads them when the
// phone sends a new one. Nothing here talks to the network or the phone.
//
//   circular     running agents, with the top agent's context as a ring
//   rectangular  the top agent's model face, status and title
//   inline       "2 agents running"
//   corner       the running count, labelled
//
// In the Smart Stack the same widget rises on its own while a run is active
// or has just finished (`TimelineEntryRelevance`). A tap opens that agent's
// live view.

import FMBase
import FMWatchWire
import SwiftUI
import WidgetKit

struct AgentsEntry: TimelineEntry {
    let date: Date
    let summary: ComplicationSummary
    let relevance: TimelineEntryRelevance?
}

struct AgentsProvider: TimelineProvider {
    private var stored: ComplicationSummary {
        WatchGroup.load(from: WatchGroup.shared()) ?? ComplicationSummary()
    }

    func placeholder(in context: Context) -> AgentsEntry {
        AgentsEntry(
            date: Date(),
            summary: ComplicationSummary(running: 2, active: 2, total: 3, topTitle: "Fix the flaky test",
                                         topStatus: .running, topFace: "Opus", topProgress: 0.4),
            relevance: nil)
    }

    func getSnapshot(in context: Context, completion: @escaping (AgentsEntry) -> Void) {
        completion(context.isPreview ? placeholder(in: context) : entry(stored, at: Date()))
    }

    func getTimeline(in context: Context, completion: @escaping (Timeline<AgentsEntry>) -> Void) {
        let now = Date()
        let summary = stored
        var entries = [entry(summary, at: now)]
        // A finished run's relevance lapses after a quarter of an hour; one
        // more entry lets the Smart Stack drop it without a reload.
        if summary.running == 0 && summary.lastFinishedAtMs > 0 {
            let lapse = Date(timeIntervalSince1970: Double(summary.lastFinishedAtMs) / 1000 + 15 * 60)
            if lapse > now { entries.append(entry(summary, at: lapse)) }
        }
        completion(Timeline(entries: entries, policy: .never))
    }

    private func entry(_ summary: ComplicationSummary, at date: Date) -> AgentsEntry {
        let score = summary.relevance(nowMs: Int64(date.timeIntervalSince1970 * 1000))
        return AgentsEntry(
            date: date, summary: summary,
            relevance: TimelineEntryRelevance(score: Float(score), duration: summary.running > 0 ? 3600 : 15 * 60))
    }
}

struct AgentsWidgetView: View {
    @Environment(\.widgetFamily) private var family
    let entry: AgentsEntry

    private var summary: ComplicationSummary { entry.summary }
    private var tint: Color { Color(hue: summary.topHue, saturation: 0.6, brightness: 0.95) }

    var body: some View {
        content
            .widgetURL(summary.topKey.isEmpty ? nil : WatchGroup.agentURL(summary.topKey))
            .containerBackground(for: .widget) { Color.clear }
    }

    @ViewBuilder
    private var content: some View {
        switch family {
        case .accessoryCircular:
            Gauge(value: summary.topProgress) {
                Image(systemName: "sparkles")
            } currentValueLabel: {
                Text("\(summary.running)")
            }
            .gaugeStyle(.accessoryCircularCapacity)
            .tint(tint)
            .accessibilityLabel(summary.inlineText)
        case .accessoryRectangular:
            VStack(alignment: .leading, spacing: 1) {
                HStack(spacing: 4) {
                    Text(summary.topFace.isEmpty ? "ForkMesh" : summary.topFace)
                        .font(.headline)
                        .foregroundStyle(tint)
                    Text(summary.topTitle.isEmpty ? "" : summary.topStatus.label)
                        .font(.caption2)
                }
                Text(summary.topTitle.isEmpty ? "No agent work yet" : summary.topTitle)
                    .font(.caption)
                    .lineLimit(2)
                Text(summary.inlineText)
                    .font(.caption2)
                    .foregroundStyle(.secondary)
            }
            .frame(maxWidth: .infinity, alignment: .leading)
        case .accessoryInline:
            Label(summary.inlineText, systemImage: "sparkles")
        case .accessoryCorner:
            Text("\(summary.running)")
                .font(.title2.bold())
                .widgetLabel {
                    Text(summary.running == 1 ? "agent" : "agents")
                }
        default:
            Text(summary.inlineText)
        }
    }
}

struct AgentsWidget: Widget {
    var body: some WidgetConfiguration {
        StaticConfiguration(kind: WatchGroup.widgetKind, provider: AgentsProvider()) { (entry: AgentsEntry) in
            AgentsWidgetView(entry: entry)
        }
        .configurationDisplayName("ForkMesh agents")
        .description("Your running agents and the one most worth a look.")
        .supportedFamilies([.accessoryCircular, .accessoryRectangular, .accessoryInline, .accessoryCorner])
    }
}

@main
struct ForkMeshWatchWidgets: WidgetBundle {
    var body: some Widget {
        AgentsWidget()
    }
}
