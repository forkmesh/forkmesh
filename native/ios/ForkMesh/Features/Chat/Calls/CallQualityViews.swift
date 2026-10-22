import FMChat
import Observation
import SwiftUI

@MainActor
@Observable
final class CallQualityMonitor {
    private(set) var history = CallQualityHistory()
    @ObservationIgnored private var playout: Task<Void, Never>?

    func apply(_ event: CallQualityEvent) {
        history.apply(event)
        startPlayout()
    }

    func push(level: CallVoiceLevel) {
        history.push(level: level)
    }

    func push(sample: CallQualitySample) {
        history.push(sample: sample)
    }

    func reset() {
        playout?.cancel()
        playout = nil
        history.reset()
    }

    private func startPlayout() {
        guard playout == nil else { return }
        playout = Task { @MainActor [weak self] in
            while !Task.isCancelled {
                guard let self, self.history.advance() else { break }
                try? await Task.sleep(nanoseconds: UInt64(CallQuality.levelEvery * 1_000_000_000))
            }
            if !Task.isCancelled { self?.playout = nil }
        }
    }
}

enum CallQualityColors {
    static let you = FmColors.accentOnDark
    static let them = FmColors.offline
    static let line = Color(white: 0.9)
    static let grid = FmColors.offline.opacity(0.35)
    static let faint = Color.white.opacity(0.25)

    static func grade(_ grade: CallQualityGrade) -> Color {
        switch grade {
        case .good: return FmColors.successOnDark
        case .fair: return FmColors.warningOnDark
        case .poor: return FmColors.dangerOnDark
        case .unknown: return faint
        }
    }
}

struct CallSignalBars: View {
    let bars: Int
    let grade: CallQualityGrade

    var body: some View {
        HStack(alignment: .bottom, spacing: 1.5) {
            ForEach(0..<4, id: \.self) { (index: Int) in
                RoundedRectangle(cornerRadius: 0.75)
                    .fill(index < bars ? CallQualityColors.grade(grade) : CallQualityColors.faint)
                    .frame(width: 3, height: 4 + CGFloat(index) * 3)
            }
        }
        .frame(height: 13, alignment: .bottom)
        .accessibilityHidden(true)
    }
}

struct CallVoiceWave: View {
    let levels: [CallVoiceLevel]
    let count: Int

    var body: some View {
        Canvas { (context: inout GraphicsContext, size: CGSize) in
            let middle = size.height / 2
            let slot = size.width / CGFloat(max(1, count))
            let bar = max(1, slot - 1)
            let recent = levels.suffix(count)
            let offset = count - recent.count
            var mine = Path()
            var theirs = Path()
            for (index, entry) in recent.enumerated() {
                let x = CGFloat(offset + index) * slot
                let up = max(0.5, CGFloat(entry.mic) * (middle - 1))
                mine.addRect(CGRect(x: x, y: middle - 0.5 - up, width: bar, height: up))
                if let peer = entry.peer {
                    let down = max(0.5, CGFloat(peer) * (middle - 1))
                    theirs.addRect(CGRect(x: x, y: middle + 0.5, width: bar, height: down))
                }
            }
            context.fill(Path(CGRect(x: 0, y: middle - 0.5, width: size.width, height: 1)), with: .color(CallQualityColors.grid))
            context.fill(mine, with: .color(CallQualityColors.you))
            context.fill(theirs, with: .color(CallQualityColors.them))
        }
        .accessibilityHidden(true)
    }
}

struct CallSparkline: View {
    let values: [Double?]
    var floor: Double = 0
    var marks: [Color?] = []

    var body: some View {
        Canvas { (context: inout GraphicsContext, size: CGSize) in
            let count = CallQuality.statsSamples
            let slot = size.width / CGFloat(max(1, count - 1))
            let offset = count - values.count
            context.fill(Path(CGRect(x: 0, y: size.height - 1, width: size.width, height: 1)), with: .color(CallQualityColors.grid))
            for (index, mark) in marks.enumerated() {
                guard let mark else { continue }
                let x = CGFloat(offset + index) * slot - 1
                context.fill(Path(CGRect(x: x, y: 0, width: 2, height: size.height - 1)), with: .color(mark))
            }
            let known = values.compactMap { $0 }
            guard let peak = known.max() else { return }
            let top = max(floor, peak) * 1.15
            let scale = top > 0 ? top : 1
            var line = Path()
            var drawing = false
            for (index, value) in values.enumerated() {
                guard let value else {
                    drawing = false
                    continue
                }
                let point = CGPoint(
                    x: CGFloat(offset + index) * slot, y: size.height - 2 - CGFloat(value / scale) * (size.height - 4))
                if drawing { line.addLine(to: point) } else { line.move(to: point) }
                drawing = true
            }
            context.stroke(
                line, with: .color(CallQualityColors.line),
                style: StrokeStyle(lineWidth: 1.5, lineCap: .round, lineJoin: .round))
        }
    }
}

struct CallQualityWidget: View {
    let monitor: CallQualityMonitor
    var video = false
    var peerLabel = "Them"
    @AppStorage(CallQuality.openKey) private var open = false

    var body: some View {
        let history = monitor.history
        VStack(alignment: .leading, spacing: 6) {
            Button(action: { open.toggle() }) {
                pill(history)
            }
            .buttonStyle(.plain)
            .accessibilityLabel(history.accessibilityLabel)
            .accessibilityHint(open ? "Hides the live charts" : "Shows live charts")
            .accessibilityIdentifier("chat-call-quality")
            if open {
                card(history)
                    .transition(.opacity)
            }
        }
        .environment(\.colorScheme, .dark)
    }

    private func pill(_ history: CallQualityHistory) -> some View {
        HStack(spacing: 6) {
            if history.live {
                CallSignalBars(bars: history.bars, grade: history.grade)
            }
            CallVoiceWave(levels: history.levels, count: CallQuality.pillSamples)
                .frame(width: 40, height: 16)
            let text = history.pillText
            if !text.isEmpty {
                Text(text)
                    .font(.system(size: 11, weight: .semibold).monospacedDigit())
                    .foregroundStyle(.white)
                    .lineLimit(1)
            }
        }
        .padding(.horizontal, 8)
        .frame(height: 26)
        .background(.ultraThinMaterial, in: Capsule())
        .overlay(Capsule().stroke(Color.white.opacity(0.12), lineWidth: 0.5))
        .contentShape(Capsule())
    }

    private func card(_ history: CallQualityHistory) -> some View {
        let latest = history.latest
        let network = CallQuality.describeNetwork(latest)
        return VStack(alignment: .leading, spacing: 6) {
            Text(history.heading)
                .font(.system(size: 13, weight: .semibold))
                .foregroundStyle(.white)
                .accessibilityAddTraits(.isHeader)
            row(
                "Voice", value: CallQuality.voiceValue(hasPeer: history.hasPeer, peerLabel: peerLabel),
                chartLabel: "Voice levels over the last five seconds"
            ) {
                CallVoiceWave(levels: history.levels, count: CallQuality.levelSamples)
            }
            if history.live {
                row("Network", value: network, chartLabel: "Round-trip time over the last minute; ticks mark packet loss") {
                    CallSparkline(
                        values: history.samples.map(\.rttMs), floor: 100,
                        marks: history.samples.map { CallQuality.lossMark($0.lossPct).map(CallQualityColors.grade) })
                }
                if video {
                    row(
                        "Video in", value: CallQuality.describeVideo(latest, .incoming).ifEmpty("No video"),
                        chartLabel: "Incoming bitrate over the last minute"
                    ) {
                        CallSparkline(values: history.samples.map(\.recvKbps), floor: 200)
                    }
                    row(
                        "Video out", value: CallQuality.describeVideo(latest, .outgoing).ifEmpty("Camera off"),
                        chartLabel: "Outgoing bitrate over the last minute"
                    ) {
                        CallSparkline(values: history.samples.map(\.sendKbps), floor: 200)
                    }
                }
            }
        }
        .padding(10)
        .background(.ultraThinMaterial, in: RoundedRectangle(cornerRadius: 12))
        .overlay(RoundedRectangle(cornerRadius: 12).stroke(Color.white.opacity(0.12), lineWidth: 0.5))
        .accessibilityElement(children: .contain)
        .accessibilityIdentifier("chat-call-quality-card")
    }

    private func row<Chart: View>(
        _ label: String, value: String, chartLabel: String, @ViewBuilder chart: () -> Chart
    ) -> some View {
        HStack(spacing: 8) {
            Text(label)
                .font(.system(size: 11, weight: .medium))
                .foregroundStyle(Color.white.opacity(0.7))
                .frame(width: 54, alignment: .leading)
            chart()
                .frame(width: 120, height: 22)
            Text(value)
                .font(.system(size: 11).monospacedDigit())
                .foregroundStyle(.white)
                .lineLimit(2)
                .minimumScaleFactor(0.8)
                .frame(maxWidth: 150, alignment: .leading)
                .fixedSize(horizontal: false, vertical: true)
        }
        .accessibilityElement(children: .ignore)
        .accessibilityLabel("\(label): \(value). \(chartLabel)")
    }
}

private extension String {
    func ifEmpty(_ fallback: String) -> String { isEmpty ? fallback : self }
}
