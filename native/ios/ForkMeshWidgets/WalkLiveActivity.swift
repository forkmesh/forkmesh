import ActivityKit
import FMApi
import FMSurfaces
import SwiftUI
import WidgetKit

struct WalkLiveActivity: Widget {
    var body: some WidgetConfiguration {
        ActivityConfiguration(for: WalkActivityAttributes.self) { context in
            HStack(spacing: 12) {
                Image(systemName: "figure.walk")
                    .font(.title2)
                    .foregroundStyle(.green)
                VStack(alignment: .leading, spacing: 2) {
                    Text(context.state.ended ? "Walk ended" : "Walking")
                        .font(.subheadline.weight(.semibold))
                        .lineLimit(1)
                    HStack(spacing: 4) {
                        WalkClockText(attributes: context.attributes, state: context.state)
                        Text("· \(context.state.distance)")
                    }
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .lineLimit(1)
                }
                Spacer(minLength: 4)
                VStack(alignment: .trailing, spacing: 2) {
                    Text(context.state.steps, format: .number)
                        .font(.title.weight(.bold))
                        .monospacedDigit()
                        .contentTransition(.numericText())
                    Text("\(context.state.todaySteps.formatted()) today")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                        .lineLimit(1)
                }
            }
            .padding(14)
            .activityBackgroundTint(Color.black.opacity(0.35))
            .widgetURL(WidgetRoute.url(context.attributes.route))
        } dynamicIsland: { context in
            DynamicIsland {
                DynamicIslandExpandedRegion(.leading) {
                    Label(context.state.ended ? "Walk ended" : "Walking", systemImage: "figure.walk")
                        .font(.caption.weight(.semibold))
                        .foregroundStyle(.green)
                        .lineLimit(1)
                }
                DynamicIslandExpandedRegion(.trailing) {
                    WalkClockText(attributes: context.attributes, state: context.state)
                        .font(.caption)
                        .monospacedDigit()
                        .frame(maxWidth: 80, alignment: .trailing)
                }
                DynamicIslandExpandedRegion(.bottom) {
                    HStack(alignment: .firstTextBaseline) {
                        Text(context.state.steps, format: .number)
                            .font(.title.weight(.bold))
                            .monospacedDigit()
                            .contentTransition(.numericText())
                        Text("steps")
                            .font(.subheadline)
                            .foregroundStyle(.secondary)
                        Spacer(minLength: 4)
                        VStack(alignment: .trailing, spacing: 2) {
                            Text(context.state.distance)
                            Text("\(context.state.todaySteps.formatted()) today")
                        }
                        .font(.caption)
                        .foregroundStyle(.secondary)
                    }
                }
            } compactLeading: {
                Image(systemName: "figure.walk")
                    .foregroundStyle(.green)
            } compactTrailing: {
                Text(context.state.steps, format: .number)
                    .monospacedDigit()
                    .frame(maxWidth: 64)
            } minimal: {
                Image(systemName: "figure.walk")
                    .foregroundStyle(.green)
            }
            .widgetURL(WidgetRoute.url(context.attributes.route))
            .keylineTint(.green)
        }
    }
}

struct WalkClockText: View {
    let attributes: WalkActivityAttributes
    let state: WalkActivityAttributes.ContentState

    var body: some View {
        if let end = state.endedAt, end > attributes.startedAt {
            Text(timerInterval: attributes.startedAt...end, pauseTime: end, countsDown: false)
        } else {
            Text(timerInterval: attributes.startedAt...Date.distantFuture, countsDown: false)
        }
    }
}
