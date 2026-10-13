import FMApi
import FMBase
import SwiftUI

struct RunnerQueueCard: View {
    @Environment(\.fmPalette) private var fm
    let queue: AgentRunQueue

    var body: some View {
        TimelineView(.periodic(from: .now, by: 1)) { context in
            let now = Int(context.date.timeIntervalSince1970 * 1000)
            VStack(alignment: .leading, spacing: 6) {
                HStack(spacing: FmSpace.x2) {
                    Image(systemName: "hourglass")
                        .font(.system(size: 13, weight: .semibold))
                        .foregroundStyle(fm.textSecondary)
                    Text(queue.placeLine)
                        .fmText(.labelMedium)
                        .accessibilityIdentifier("agent-runner-queue-place")
                    Spacer(minLength: 0)
                }
                let label = queue.countdownLabel(now)
                if !label.isEmpty {
                    HStack(alignment: .firstTextBaseline, spacing: FmSpace.x2) {
                        Text(label)
                            .font(.caption)
                            .foregroundStyle(fm.textSecondary)
                        Text(AgentRunQueue.clock(queue.countdownMs(now)))
                            .font(.system(.title3, design: .monospaced).weight(.semibold))
                            .contentTransition(.numericText(countsDown: true))
                            .accessibilityIdentifier("agent-runner-queue-countdown")
                    }
                }
                let runner = queue.runnerLine(now)
                if !runner.isEmpty {
                    Text(runner)
                        .font(.caption)
                        .foregroundStyle(fm.textSecondary)
                        .fixedSize(horizontal: false, vertical: true)
                }
                let waited = queue.waitedLine(now)
                if !waited.isEmpty {
                    Text(waited)
                        .font(.caption.monospacedDigit())
                        .foregroundStyle(fm.textSecondary)
                }
            }
            .frame(maxWidth: .infinity, alignment: .leading)
            .padding(.horizontal, 12)
            .padding(.vertical, 8)
            .background(fm.bgOverlay, in: RoundedRectangle(cornerRadius: FmRadius.md))
            .accessibilityElement(children: .combine)
            .accessibilityIdentifier("agent-runner-queue")
        }
    }
}
