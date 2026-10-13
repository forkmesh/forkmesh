import ActivityKit
import FMApi
import FMPlanning
import FMSurfaces
import SwiftUI
import WidgetKit

struct TimerLiveActivity: Widget {
    var body: some WidgetConfiguration {
        ActivityConfiguration(for: PlanningTimerAttributes.self) { context in
            HStack(spacing: 12) {
                Image(systemName: context.state.kind.systemImage)
                    .font(.title2)
                    .foregroundStyle(.orange)
                VStack(alignment: .leading, spacing: 2) {
                    Text(context.state.label)
                        .font(.subheadline.weight(.semibold))
                        .lineLimit(1)
                    Text(context.state.paused ? "Paused" : context.state.kind.label)
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
                Spacer(minLength: 4)
                TimerClockText(timer: context.state)
                    .font(.title.weight(.bold))
                    .monospacedDigit()
                    .multilineTextAlignment(.trailing)
                    .frame(maxWidth: 140, alignment: .trailing)
            }
            .padding(14)
            .activityBackgroundTint(Color.black.opacity(0.35))
            .widgetURL(WidgetRoute.url(.timers))
        } dynamicIsland: { context in
            DynamicIsland {
                DynamicIslandExpandedRegion(.leading) {
                    Label(context.state.label, systemImage: context.state.kind.systemImage)
                        .font(.caption.weight(.semibold))
                        .lineLimit(1)
                }
                DynamicIslandExpandedRegion(.trailing) {
                    TimerClockText(timer: context.state)
                        .font(.title3.weight(.bold))
                        .monospacedDigit()
                        .frame(maxWidth: 110, alignment: .trailing)
                }
            } compactLeading: {
                Image(systemName: context.state.kind.systemImage)
                    .foregroundStyle(.orange)
            } compactTrailing: {
                TimerClockText(timer: context.state)
                    .monospacedDigit()
                    .frame(maxWidth: 56)
            } minimal: {
                Image(systemName: context.state.kind.systemImage)
                    .foregroundStyle(.orange)
            }
            .widgetURL(WidgetRoute.url(.timers))
            .keylineTint(.orange)
        }
    }
}
