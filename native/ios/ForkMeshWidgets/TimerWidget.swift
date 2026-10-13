import FMApi
import FMPlanning
import FMSurfaces
import SwiftUI
import WidgetKit

struct TimerWidget: Widget {
    var body: some WidgetConfiguration {
        StaticConfiguration(kind: SurfaceSection.timer.widgetKind, provider: SurfaceProvider()) { entry in
            TimerWidgetView(timer: entry.snapshot?.timer)
                .containerBackground(.fill.tertiary, for: .widget)
                .widgetURL(WidgetRoute.url(.timers))
        }
        .configurationDisplayName("Timer")
        .description("Your running ForkMesh timer.")
        .supportedFamilies([.systemSmall, .accessoryCircular, .accessoryRectangular, .accessoryInline])
    }
}

struct TimerClockText: View {
    let timer: TimerActivitySnapshot

    var body: some View {
        if timer.paused {
            Text(timer.pausedText)
        } else if let ends = timer.endsAt, ends > Date() {
            Text(timerInterval: Date()...ends, countsDown: true)
        } else if let from = timer.countsFrom {
            Text(from, style: .timer)
        } else {
            Text("Done")
        }
    }
}

struct TimerWidgetView: View {
    @Environment(\.widgetFamily) private var family
    let timer: TimerActivitySnapshot?

    var body: some View {
        switch family {
        case .accessoryInline:
            if let timer {
                Label {
                    TimerClockText(timer: timer)
                } icon: {
                    Image(systemName: timer.kind.systemImage)
                }
            } else {
                Label("No timer", systemImage: "timer")
            }
        case .accessoryCircular:
            ZStack {
                AccessoryWidgetBackground()
                if let timer {
                    VStack(spacing: 0) {
                        Image(systemName: timer.kind.systemImage)
                            .font(.caption2)
                        TimerClockText(timer: timer)
                            .font(.caption2.weight(.semibold))
                            .monospacedDigit()
                            .multilineTextAlignment(.center)
                            .minimumScaleFactor(0.6)
                    }
                    .padding(2)
                } else {
                    Image(systemName: "timer")
                }
            }
        case .accessoryRectangular:
            VStack(alignment: .leading, spacing: 1) {
                if let timer {
                    Label(timer.label, systemImage: timer.kind.systemImage)
                        .font(.caption.weight(.semibold))
                        .lineLimit(1)
                    TimerClockText(timer: timer)
                        .font(.title3.weight(.bold))
                        .monospacedDigit()
                } else {
                    Label("No timer running", systemImage: "timer")
                        .font(.caption)
                }
            }
            .frame(maxWidth: .infinity, alignment: .leading)
        default:
            VStack(alignment: .leading, spacing: 6) {
                if let timer {
                    Label(timer.kind.label, systemImage: timer.kind.systemImage)
                        .font(.caption.weight(.semibold))
                        .foregroundStyle(.orange)
                    Spacer(minLength: 0)
                    Text(timer.label)
                        .font(.footnote.weight(.semibold))
                        .lineLimit(2)
                    TimerClockText(timer: timer)
                        .font(.title2.weight(.bold))
                        .monospacedDigit()
                } else {
                    Label("Timer", systemImage: "timer")
                        .font(.caption.weight(.semibold))
                        .foregroundStyle(.orange)
                    Spacer(minLength: 0)
                    Text("No timer running")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
        }
    }
}
