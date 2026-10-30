import FMChat
import Observation
import SwiftUI
import UIKit

@MainActor
@Observable
final class CallQualityMonitor {
    private(set) var history = CallQualityHistory()
    private(set) var diagnosis = CallDiagnosis.empty
    @ObservationIgnored var video = false
    @ObservationIgnored var sending = true
    @ObservationIgnored private var playout: Task<Void, Never>?

    func apply(_ event: CallQualityEvent) {
        history.apply(event)
        if event.live {
            rediagnose()
        } else if diagnosis != CallDiagnosis.empty {
            diagnosis = CallDiagnosis.empty
        }
        startPlayout()
    }

    func push(level: CallVoiceLevel) {
        history.push(level: level)
    }

    func queue(levels: [CallVoiceLevel]) {
        history.queue(levels)
        startPlayout()
    }

    func push(sample: CallQualitySample) {
        history.push(sample: sample)
        rediagnose()
    }

    func reset() {
        playout?.cancel()
        playout = nil
        history.reset()
        diagnosis = CallDiagnosis.empty
        CallDiagnosticsBook.shared.finish()
    }

    private func rediagnose() {
        let next = CallDiagnostics.diagnose(history.samples, video: video, sending: sending)
        diagnosis = next
        CallDiagnosticsBook.shared.refreshed(next)
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
    var sending = true
    var peerLabel = "Them"
    @AppStorage(CallQuality.openKey) private var open = false

    var body: some View {
        let history = monitor.history
        VStack(alignment: .leading, spacing: 6) {
            HStack(spacing: 6) {
                Button(action: { open.toggle() }) {
                    pill(history)
                }
                .buttonStyle(.plain)
                .accessibilityLabel(history.accessibilityLabel)
                .accessibilityHint(open ? "Hides the live charts" : "Shows live charts")
                .accessibilityIdentifier("chat-call-quality")
                if history.live {
                    CallDiagnosticsButton(monitor: monitor)
                }
            }
            if open {
                card(history)
                    .transition(.opacity)
            }
        }
        .environment(\.colorScheme, .dark)
        .onAppear {
            monitor.video = video
            monitor.sending = sending
        }
        .onChange(of: video) { (_: Bool, now: Bool) in monitor.video = now }
        .onChange(of: sending) { (_: Bool, now: Bool) in monitor.sending = now }
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

@MainActor
@Observable
final class CallDiagnosticsBook {
    static let shared = CallDiagnosticsBook()

    private(set) var memory = CallDiagnosticsMemory()
    @ObservationIgnored private var tracker = CallDiagnosticsTracker()

    private init() {
        memory = Self.load()
    }

    private static func load() -> CallDiagnosticsMemory {
        CallDiagnosticsMemory(json: UserDefaults.standard.string(forKey: CallDiagnostics.key))
    }

    private func save() {
        UserDefaults.standard.set(memory.json, forKey: CallDiagnostics.key)
    }

    func opened(_ diagnosis: CallDiagnosis) {
        memory = Self.load()
        tracker.opened(diagnosis, memory: &memory)
        save()
    }

    func refreshed(_ diagnosis: CallDiagnosis) {
        guard !tracker.pending.isEmpty else { return }
        if tracker.refreshed(diagnosis, memory: &memory) { save() }
    }

    func finish() {
        guard !tracker.pending.isEmpty else { return }
        memory = Self.load()
        if tracker.finished(memory: &memory) { save() }
    }

    func saveLesson(_ text: String) -> Bool {
        memory = Self.load()
        guard CallDiagnostics.rememberNote(&memory, text) else { return false }
        save()
        return true
    }

    static var environment: CallDiagnosticsEnvironment {
        var system = utsname()
        uname(&system)
        let model = withUnsafeBytes(of: &system.machine) { (raw: UnsafeRawBufferPointer) -> String in
            String(decoding: raw.prefix { $0 != 0 }, as: UTF8.self)
        }
        let version = Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? ""
        let device = UIDevice.current
        let agent = "ForkMesh iOS \(version) (\(model); \(device.systemName) \(device.systemVersion); native call screen)"
        let memory = (Double(ProcessInfo.processInfo.physicalMemory) / 1_073_741_824).rounded()
        return CallDiagnosticsEnvironment(
            userAgent: agent, cores: ProcessInfo.processInfo.activeProcessorCount, memory: memory, connection: "")
    }
}

struct OpenedCallDiagnosis: Identifiable {
    let id = UUID()
    let diagnosis: CallDiagnosis
}

struct CallDiagnosticsButton: View {
    let monitor: CallQualityMonitor
    @State private var shown: OpenedCallDiagnosis?

    var body: some View {
        let diagnosis = monitor.diagnosis
        Button(action: { open(diagnosis) }) {
            Circle()
                .fill(diagnosis.problem ? FmColors.dangerOnDark : Color.white.opacity(0.4))
                .frame(width: 8, height: 8)
                .frame(width: 26, height: 26)
                .background(.ultraThinMaterial, in: Circle())
                .overlay(
                    Circle().stroke(
                        diagnosis.severity == .bad ? FmColors.dangerOnDark : Color.white.opacity(0.12),
                        lineWidth: diagnosis.severity == .bad ? 1 : 0.5)
                )
                .frame(width: 44, height: 44)
                .contentShape(Circle())
        }
        .buttonStyle(.plain)
        .accessibilityLabel("Call diagnostics: \(diagnosis.summary)")
        .accessibilityHint("Shows diagnostics and how to fix them")
        .accessibilityIdentifier("chat-call-diagnostics")
        .sheet(item: $shown) { (opened: OpenedCallDiagnosis) in
            CallDiagnosticsSheet(diagnosis: opened.diagnosis)
                .presentationDetents([.medium, .large])
                .presentationDragIndicator(.visible)
        }
    }

    private func open(_ diagnosis: CallDiagnosis) {
        CallDiagnosticsBook.shared.opened(diagnosis)
        shown = OpenedCallDiagnosis(diagnosis: diagnosis)
    }
}

struct CallDiagnosticsSheet: View {
    let diagnosis: CallDiagnosis
    @Environment(\.dismiss) private var dismiss
    @State private var lesson = ""
    @State private var status = ""

    private var book: CallDiagnosticsBook { CallDiagnosticsBook.shared }

    var body: some View {
        let memory = book.memory
        let prompt = CallDiagnostics.prompt(diagnosis, memory, environment: CallDiagnosticsBook.environment)
        NavigationStack {
            ScrollView {
                VStack(alignment: .leading, spacing: FmSpace.x4) {
                    if diagnosis.issues.isEmpty {
                        let lines = CallDiagnostics.describe(diagnosis.metrics)
                        Text(lines.isEmpty ? "Measurements start once the call connects." : lines.joined(separator: ". "))
                            .font(.system(size: 15))
                    }
                    ForEach(Array(diagnosis.issues.enumerated()), id: \.element.id) {
                        (entry: (offset: Int, element: CallDiagnosisIssue)) in
                        issueView(entry.offset + 1, entry.element, memory: memory)
                    }
                    VStack(alignment: .leading, spacing: FmSpace.x2) {
                        Text(CallDiagnostics.promptLabel(diagnosis, memory))
                            .font(.system(size: 13, weight: .semibold))
                            .foregroundStyle(.secondary)
                        Text(prompt)
                            .font(.system(size: 12).monospaced())
                            .textSelection(.enabled)
                            .frame(maxWidth: .infinity, alignment: .leading)
                            .padding(FmSpace.x3)
                            .background(Color.secondary.opacity(0.12), in: RoundedRectangle(cornerRadius: 8))
                            .accessibilityIdentifier("chat-call-diagnostics-prompt")
                        Button {
                            UIPasteboard.general.string = prompt
                            status = "Copied. Paste it into your AI assistant."
                        } label: {
                            Label("Copy prompt", systemImage: "doc.on.doc")
                        }
                        .buttonStyle(.bordered)
                        .accessibilityIdentifier("chat-call-diagnostics-copy")
                    }
                    VStack(alignment: .leading, spacing: FmSpace.x2) {
                        TextField("Paste the LESSON line from the answer", text: $lesson, axis: .vertical)
                            .textFieldStyle(.roundedBorder)
                            .lineLimit(1...4)
                            .accessibilityLabel("Lesson from the answer")
                            .accessibilityIdentifier("chat-call-diagnostics-lesson")
                        Button("Save lesson", action: saveLesson)
                            .buttonStyle(.bordered)
                            .accessibilityIdentifier("chat-call-diagnostics-save")
                    }
                    if !status.isEmpty {
                        Text(status)
                            .font(.system(size: 13))
                            .foregroundStyle(.secondary)
                            .accessibilityAddTraits(.updatesFrequently)
                    }
                }
                .padding(FmSpace.x4)
            }
            .navigationTitle(diagnosis.heading)
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button("Done") { dismiss() }
                }
            }
        }
        .accessibilityIdentifier("chat-call-diagnostics-sheet")
    }

    private func issueView(_ number: Int, _ issue: CallDiagnosisIssue, memory: CallDiagnosticsMemory) -> some View {
        VStack(alignment: .leading, spacing: FmSpace.x2) {
            HStack(spacing: 6) {
                Circle()
                    .fill(issue.severity == .bad ? FmColors.danger : FmColors.warning)
                    .frame(width: 8, height: 8)
                    .accessibilityHidden(true)
                Text("\(number). \(issue.title)")
                    .font(.system(size: 15, weight: .semibold))
            }
            Text(issue.detail + " " + issue.cause)
                .font(.system(size: 14))
                .foregroundStyle(.secondary)
            ForEach(Array(CallDiagnostics.rankFixes(issue, memory).enumerated()), id: \.element.id) {
                (entry: (offset: Int, element: CallDiagnosisFix)) in
                HStack(alignment: .firstTextBaseline, spacing: 6) {
                    Text("\(entry.offset + 1).")
                        .font(.system(size: 14).monospacedDigit())
                        .foregroundStyle(.secondary)
                    Text(CallDiagnostics.fixLabel(entry.element))
                        .font(.system(size: 14))
                }
            }
        }
        .accessibilityElement(children: .combine)
    }

    private func saveLesson() {
        if book.saveLesson(lesson) {
            lesson = ""
            status = "Saved. The next prompt starts from it."
        } else {
            status = "Paste the LESSON line first."
        }
    }
}
