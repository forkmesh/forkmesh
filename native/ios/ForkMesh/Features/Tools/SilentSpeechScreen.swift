import FMTools
import SwiftUI

struct SilentSpeechScreen: View {
    @Environment(\.fmPalette) private var fm
    @Bindable private var listener: SilentSpeechListener = SilentSpeechListener.shared
    @State private var entry: String = ""
    @State private var showWords: Bool = false

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: FmSpace.x3) {
                SilentMotionCard(listener: listener)
                if let problem = listener.problem {
                    FmListRow(
                        title: problem,
                        leading: AnyView(FmIcon("error", color: fm.danger)),
                        onTap: settingsAction,
                        showChevron: listener.denied)
                }
                teachSection
                wordsSection
                checkSection
                decodeSection
            }
            .padding(FmSpace.x3)
        }
        .background(fm.canvas)
        .navigationTitle("Silent Speech")
        .navigationBarTitleDisplayMode(.inline)
        .onAppear { listener.start() }
        .onDisappear { listener.stop() }
    }

    @ViewBuilder
    private var teachSection: some View {
        if let reading = listener.reading {
            FmGlassSurface(cornerRadius: FmRadius.md) {
                VStack(alignment: .leading, spacing: FmSpace.x2) {
                    sentenceText(reading)
                        .fmText(.titleLarge, color: fm.textPrimary)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .accessibilityIdentifier("silent-speech-sentence")
                    Text(listener.calibrating ? "Keep still for two seconds…" : readingDetail(reading))
                        .fmText(.bodySmall, color: fm.textSecondary)
                    if let notice = reading.notice {
                        Text(notice)
                            .fmText(.bodySmall, color: fm.warning)
                            .accessibilityIdentifier("silent-speech-reading-notice")
                    }
                    HStack(spacing: FmSpace.x2) {
                        FmToneButton(label: "Again", icon: "refresh", onPressed: reading.position == 0 ? nil : { listener.readAgain() })
                        FmToneButton(label: "Skip", icon: "skip_next", onPressed: { listener.skipSentence() })
                        if !listener.lastSentence.isEmpty {
                            FmToneButton(label: "Drop last", icon: "undo", onPressed: { listener.dropLastSentence() })
                        }
                        Spacer(minLength: 0)
                        FmToneButton(label: "Finish", icon: "stop", tone: .accent, onPressed: { listener.finishReading() })
                            .accessibilityIdentifier("silent-speech-read-finish")
                    }
                }
                .padding(FmSpace.x3)
            }
        } else {
            HStack(spacing: FmSpace.x3) {
                FmToneButton(label: "Read sentences · 5 min", icon: "play_arrow", tone: .accent, filled: true, height: 40, onPressed: listener.connected ? { listener.startReading() } : nil)
                    .accessibilityIdentifier("silent-speech-read")
                Text(listener.readingSummary ?? "Short stop between words, a longer pause after each sentence.")
                    .fmText(.bodySmall, color: fm.textSecondary)
                    .lineLimit(2)
            }
        }
    }

    private func sentenceText(_ reading: SilentReading) -> Text {
        reading.sentence.enumerated().reduce(Text(verbatim: "")) { (line: Text, item: (offset: Int, element: String)) -> Text in
            let color: Color = item.offset < reading.position ? fm.textTertiary : (item.offset == reading.position ? fm.accent : fm.textPrimary)
            let word: Text = Text(item.element).foregroundColor(color).fontWeight(item.offset == reading.position ? .bold : .regular)
            return item.offset == 0 ? word : line + Text(verbatim: " ") + word
        }
    }

    private func readingDetail(_ reading: SilentReading) -> String {
        let saved: String = reading.collected == 1 ? "1 take" : "\(reading.collected) takes"
        return "Sentence \(reading.sentencesRead + 1) · \(reading.clock) · \(saved) · pause to finish it"
    }

    private var wordsSection: some View {
        let counts: [String: Int] = listener.dataset.counts()
        let taught: Int = listener.dataset.words.filter { counts[$0.word, default: 0] > 0 }.count
        return VStack(alignment: .leading, spacing: FmSpace.x2) {
            Button { showWords.toggle() } label: {
                HStack(spacing: FmSpace.x2) {
                    Text("Words").fmText(.titleSmall, color: fm.textPrimary)
                    Text("\(taught) of \(listener.dataset.words.count) taught · \(listener.dataset.takes.count) takes")
                        .fmText(.bodySmall, color: fm.textSecondary)
                    Spacer(minLength: 0)
                    FmIcon(showWords ? "expand_less" : "expand_more", size: 20, color: fm.textSecondary)
                }
                .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            .accessibilityIdentifier("silent-speech-words")
            if showWords {
                ToolsFlowLayout(spacing: FmSpace.x1, lineSpacing: FmSpace.x1) {
                    ForEach(listener.dataset.words, id: \.word) { (word: SilentWord) in
                        SilentChip(label: "\(word.word) \(counts[word.word, default: 0])", on: listener.selectedWord == word.word) {
                            listener.selectedWord = word.word
                        }
                        .accessibilityIdentifier("silent-speech-word-\(word.word)")
                    }
                }
                if let word = listener.selectedWord {
                    HStack(spacing: FmSpace.x1) {
                        SilentHoldButton(
                            label: listener.hold == .train(word) ? "Mouth “\(word)”, let go" : "Hold, mouth “\(word)”",
                            active: listener.hold == .train(word),
                            enabled: listener.connected && listener.reading == nil,
                            height: 44,
                            onBegin: { listener.beginHold(.train(word)) },
                            onEnd: { listener.endHold() })
                            .accessibilityIdentifier("silent-speech-train")
                        FmIconButton(label: "Delete the last take of \(word)", onPressed: counts[word, default: 0] > 0 ? { listener.undoTake(word) } : nil) {
                            FmIcon("undo", size: 20, color: fm.textSecondary)
                        }
                        FmIconButton(label: "Remove \(word) and its takes", onPressed: { listener.removeWord(word) }) {
                            FmIcon("delete_outline", size: 20, color: fm.textSecondary)
                        }
                    }
                }
                HStack(spacing: FmSpace.x2) {
                    FmTextField(text: $entry, hint: "Add a word or your own sentence", onSubmit: addEntry)
                        .accessibilityIdentifier("silent-speech-new-word")
                    FmToneButton(label: "Add", icon: "add", height: 40, onPressed: SilentText.words(entry).isEmpty ? nil : addEntry)
                        .accessibilityIdentifier("silent-speech-add-word")
                }
            }
        }
    }

    private func addEntry() {
        if listener.addEntry(entry) { entry = "" }
    }

    private var checkSection: some View {
        VStack(alignment: .leading, spacing: FmSpace.x2) {
            HStack(spacing: FmSpace.x1) {
                ForEach(SilentChannel.allCases.filter { listener.hasAcceleration || $0 != .acceleration }, id: \.self) { (channel: SilentChannel) in
                    SilentChip(label: channel.label, on: listener.channels.contains(channel)) { toggle(channel) }
                    .accessibilityIdentifier("silent-speech-channel-\(channel.rawValue)")
                }
                Spacer(minLength: 0)
                FmToneButton(label: listener.diagnosing ? "Diagnosing…" : "Diagnose", icon: "tune", busy: listener.diagnosing, onPressed: { listener.diagnose() })
                    .accessibilityIdentifier("silent-speech-evaluate")
            }
            if let result = listener.evaluation {
                HStack(alignment: .top, spacing: FmSpace.x3) {
                    SilentConfusionMap(evaluation: result)
                        .frame(width: 96, height: 96)
                    VStack(alignment: .leading, spacing: 2) {
                        Text(result.headline).fmText(.labelMedium, color: fm.textPrimary)
                        Text(result.verdict).fmText(.bodySmall, color: result.significant ? fm.success : fm.textSecondary)
                            .accessibilityIdentifier("silent-speech-verdict")
                    }
                }
            }
            if let diagnosis = listener.diagnosis {
                ForEach(Array(diagnosis.findings.enumerated()), id: \.offset) { (item: (offset: Int, element: SilentFinding)) in
                    findingRow(item.element)
                }
            }
        }
    }

    private func findingRow(_ finding: SilentFinding) -> some View {
        HStack(alignment: .top, spacing: FmSpace.x2) {
            FmIcon(SilentFindingStyle.icon(finding.level), size: 18, color: SilentFindingStyle.color(finding.level, fm))
            VStack(alignment: .leading, spacing: 1) {
                Text(finding.title).fmText(.labelMedium, color: fm.textPrimary)
                Text(finding.detail).fmText(.bodySmall, color: fm.textSecondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
            Spacer(minLength: 0)
            if let fix = finding.fix {
                FmToneButton(label: fix.label, onPressed: { listener.applyFix(finding) })
            }
        }
        .accessibilityElement(children: .contain)
        .accessibilityIdentifier("silent-speech-finding")
    }

    private func toggle(_ channel: SilentChannel) {
        if listener.channels.contains(channel) {
            listener.channels.remove(channel)
        } else {
            listener.channels.insert(channel)
        }
    }

    private var decodeSection: some View {
        VStack(alignment: .leading, spacing: FmSpace.x2) {
            SilentHoldButton(
                label: listener.hold == .decode ? "Mouth words, then let go" : "Hold and mouth a word",
                active: listener.hold == .decode,
                enabled: listener.connected && !listener.dataset.takes.isEmpty && listener.reading == nil,
                height: 52,
                onBegin: { listener.beginHold(.decode) },
                onEnd: { listener.endHold() })
                .accessibilityIdentifier("silent-speech-decode")
            HStack(spacing: FmSpace.x1) {
                SilentChip(label: "Hands-free", on: listener.autoDetect) { listener.autoDetect.toggle() }
                    .accessibilityIdentifier("silent-speech-auto")
                SilentChip(label: "Speak", on: listener.speakAloud) { listener.speakAloud.toggle() }
                    .accessibilityIdentifier("silent-speech-speak")
                SilentChip(label: "Skip far-off", on: listener.maxDistance != nil) { toggleLimit() }
                    .disabled(listener.maxDistance == nil && listener.evaluation?.suggestedMaxDistance == nil)
                Spacer(minLength: 0)
                if listener.autoDetect {
                    FmIconButton(label: "Measure stillness again", onPressed: listener.calibrating || !listener.connected ? nil : { listener.calibrate() }) {
                        FmIcon("refresh", size: 20, color: fm.textSecondary)
                    }
                }
            }
            Text(statusLine)
                .fmText(.bodySmall, color: listener.lastMatch.map { $0.accepted ? fm.textPrimary : fm.warning } ?? fm.textSecondary)
                .accessibilityIdentifier("silent-speech-last")
            FmGlassSurface(cornerRadius: FmRadius.md) {
                VStack(alignment: .leading, spacing: FmSpace.x1) {
                    Text(listener.transcript.text.isEmpty ? "Words you mouth appear here." : listener.transcript.text)
                        .fmText(.bodyLarge, color: listener.transcript.text.isEmpty ? fm.textTertiary : fm.textPrimary)
                        .frame(maxWidth: .infinity, minHeight: 40, alignment: .topLeading)
                        .textSelection(.enabled)
                        .accessibilityIdentifier("silent-speech-transcript")
                    HStack(spacing: 0) {
                        Spacer(minLength: 0)
                        transcriptButton("undo", "Undo") { listener.undoWord() }
                        transcriptButton("content_copy", "Copy") { listener.copyTranscript() }
                        transcriptButton("volume_up_outlined", "Say it") { listener.speak(listener.transcript.words.map { listener.dataset.phrase($0) }.joined(separator: " ")) }
                        transcriptButton("close", "Clear") { listener.clearTranscript() }
                    }
                    .disabled(listener.transcript.words.isEmpty)
                }
                .padding(.horizontal, FmSpace.x3)
                .padding(.top, FmSpace.x2)
            }
        }
    }

    private func transcriptButton(_ icon: String, _ label: String, _ action: @escaping () -> Void) -> some View {
        FmIconButton(label: label, onPressed: action) {
            FmIcon(icon, size: 20, color: listener.transcript.words.isEmpty ? fm.textDisabled : fm.textSecondary)
        }
    }

    private var statusLine: String {
        if let match = listener.lastMatch { return match.summary }
        guard listener.autoDetect else { return "Hold for each word, or turn on Hands-free." }
        if listener.calibrating { return "Keep still for two seconds…" }
        if listener.noise == nil { return "Waiting to measure stillness." }
        return listener.detecting ? "Hearing a word…" : "Listening. Keep still between words."
    }

    private func toggleLimit() {
        if listener.maxDistance != nil { listener.resetLimit() } else { listener.maxDistance = listener.evaluation?.suggestedMaxDistance }
    }

    private var settingsAction: (() -> Void)? {
        guard listener.denied else { return nil }
        return { openSettings() }
    }

    private func openSettings() {
        guard let url = URL(string: UIApplication.openSettingsURLString) else { return }
        UIApplication.shared.open(url)
    }
}

struct SilentChip: View {
    @Environment(\.fmPalette) private var fm
    @Environment(\.isEnabled) private var enabled
    let label: String
    let on: Bool
    let action: () -> Void

    var body: some View {
        Button(action: action) {
            Text(label)
                .fmText(.labelMedium, color: on ? fm.accent : fm.textSecondary)
                .lineLimit(1)
                .padding(.horizontal, 10)
                .frame(height: 30)
                .background(Capsule().fill(on ? fm.accentSubtle : fm.bgRaised))
                .overlay(Capsule().stroke(on ? fm.accent.opacity(0.5) : fm.border, lineWidth: 1))
                .opacity(enabled ? 1 : 0.45)
        }
        .buttonStyle(.plain)
        .accessibilityAddTraits(on ? AccessibilityTraits.isSelected : AccessibilityTraits())
    }
}

struct SilentHoldButton: View {
    @Environment(\.fmPalette) private var fm
    let label: String
    let active: Bool
    let enabled: Bool
    var height: CGFloat = 52
    let onBegin: () -> Void
    let onEnd: () -> Void
    @State private var pressed: Bool = false

    var body: some View {
        Text(label)
            .fmText(.labelLarge, color: active ? fm.onPrimary : (enabled ? fm.accent : fm.textDisabled))
            .lineLimit(1)
            .frame(maxWidth: .infinity, minHeight: height)
            .background(RoundedRectangle(cornerRadius: FmRadius.lg).fill(active ? fm.accent : fm.accentSubtle))
            .contentShape(RoundedRectangle(cornerRadius: FmRadius.lg))
            .gesture(
                DragGesture(minimumDistance: 0)
                    .onChanged { _ in
                        guard enabled, !pressed else { return }
                        pressed = true
                        onBegin()
                    }
                    .onEnded { _ in
                        guard pressed else { return }
                        pressed = false
                        onEnd()
                    })
            .accessibilityElement(children: .ignore)
            .accessibilityLabel(label)
            .accessibilityAddTraits(.isButton)
            .accessibilityHint("Double-tap to start, mouth the word, then double-tap again")
            .accessibilityAction {
                guard enabled else { return }
                if active { onEnd() } else { onBegin() }
            }
    }
}

struct SilentMotionCard: View {
    @Environment(\.fmPalette) private var fm
    let listener: SilentSpeechListener

    var body: some View {
        let frames: [SilentScopeFrame] = listener.scope.frames
        let end: Double = listener.scope.latest ?? 0
        FmGlassSurface(cornerRadius: FmRadius.lg) {
            VStack(alignment: .leading, spacing: FmSpace.x1) {
                HStack(spacing: FmSpace.x2) {
                    FmIcon("headphones", size: 22, color: listener.connected ? fm.success : fm.textTertiary)
                    VStack(alignment: .leading, spacing: 0) {
                        Text(listener.connected ? "Reading your earbuds" : "Put in your AirPods").fmText(.titleSmall)
                        Text(statusDetail).fmText(.bodySmall, color: fm.textSecondary)
                    }
                    Spacer(minLength: 0)
                    FmIconButton(label: "Zero the head angle", onPressed: listener.connected ? { listener.recentre() } : nil) {
                        FmIcon("refresh", size: 20, color: fm.textSecondary)
                    }
                }
                SilentAxesStrip(title: "Turning", unit: "°/s", floor: 30, values: frames.map(\.rotation), times: frames.map(\.t), end: end, marks: listener.scope.marks)
                SilentAxesStrip(title: "Head angle", unit: "°", floor: 5, values: frames.map(\.angle), times: frames.map(\.t), end: end, marks: listener.scope.marks)
                if listener.hasAcceleration {
                    SilentAxesStrip(title: "Movement", unit: "g", floor: 0.05, values: frames.map { $0.acceleration ?? .zero }, times: frames.map(\.t), end: end, marks: listener.scope.marks)
                }
                HStack(spacing: FmSpace.x2) {
                    SilentEnergyStrip(energies: frames.map(\.energy), times: frames.map(\.t), end: end, marks: listener.scope.marks, noise: listener.noise, active: listener.hold != nil || listener.detecting)
                    SilentSpectrumView(spectrum: SilentScope.spectrum(frames.map(\.rotation)))
                }
            }
            .padding(FmSpace.x3)
        }
    }

    private var statusDetail: String {
        guard listener.supported else { return "Head tracking is not available on this device" }
        guard listener.connected else { return "AirPods or Beats with head tracking, playing from this phone" }
        if let rate = listener.rate {
            let heart: String = listener.pulse.map { " · heartbeat \($0.label)" } ?? ""
            return "\(Int(rate.rounded())) Hz · last \(Int(SilentScope.window)) seconds\(heart)"
        }
        return "Waiting for motion"
    }
}

enum SilentFindingStyle {
    static func icon(_ level: SilentFinding.Level) -> String {
        switch level {
        case .problem: return "error_outline"
        case .tip: return "lightbulb"
        case .good: return "check_circle_outline"
        }
    }

    static func color(_ level: SilentFinding.Level, _ fm: FmPalette) -> Color {
        switch level {
        case .problem: return fm.warning
        case .tip: return fm.textSecondary
        case .good: return fm.success
        }
    }
}

enum SilentAxis {
    static let names: [String] = ["X nod", "Y tilt", "Z turn"]

    static func colors(_ fm: FmPalette) -> [Color] {
        fm.dark
            ? [Color(fmARGB: 0xFF3987E5), Color(fmARGB: 0xFFD95926), Color(fmARGB: 0xFF199E70)]
            : [Color(fmARGB: 0xFF2A78D6), Color(fmARGB: 0xFFEB6834), Color(fmARGB: 0xFF1BAF7A)]
    }

    static func format(_ value: Double, _ unit: String) -> String {
        unit == "g" ? String(format: "%.2f", value) : "\(Int(value.rounded()))"
    }

    static func x(_ t: Double, _ end: Double, _ width: CGFloat) -> CGFloat {
        CGFloat((t - (end - SilentScope.window)) / SilentScope.window) * width
    }

    static func shade(_ context: inout GraphicsContext, _ size: CGSize, _ marks: [SilentMark], _ end: Double, _ fm: FmPalette, labels: Bool) {
        for mark in marks {
            let left: CGFloat = max(0, x(mark.start, end, size.width))
            let right: CGFloat = min(size.width, x(mark.end, end, size.width))
            guard right > left else { continue }
            context.fill(Path(CGRect(x: left, y: 0, width: right - left, height: size.height)), with: .color(fm.accent.opacity(0.10)))
            if labels && !mark.label.isEmpty {
                context.draw(Text(mark.label).font(.system(size: 9, weight: .semibold)).foregroundColor(fm.textSecondary), at: CGPoint(x: (left + right) / 2, y: 6))
            }
        }
    }
}

struct SilentAxesStrip: View {
    @Environment(\.fmPalette) private var fm
    let title: String
    let unit: String
    let floor: Double
    let values: [SilentVector]
    let times: [Double]
    let end: Double
    let marks: [SilentMark]

    var body: some View {
        let colors: [Color] = SilentAxis.colors(fm)
        let latest: SilentVector = values.last ?? .zero
        let parts: [Double] = [latest.x, latest.y, latest.z]
        let scale: Double = max(floor, values.reduce(0) { max($0, abs($1.x), abs($1.y), abs($1.z)) })
        VStack(alignment: .leading, spacing: 1) {
            HStack(spacing: FmSpace.x2) {
                Text("\(title) \(unit)").fmText(.labelSmall, color: fm.textSecondary)
                Spacer(minLength: 0)
                ForEach(0..<3, id: \.self) { (axis: Int) in
                    HStack(spacing: 3) {
                        Circle().fill(colors[axis]).frame(width: 6, height: 6)
                        Text("\(SilentAxis.names[axis]) \(SilentAxis.format(parts[axis], unit))")
                            .fmText(.labelSmall, color: fm.textPrimary)
                            .monospacedDigit()
                    }
                }
            }
            Canvas { (context: inout GraphicsContext, size: CGSize) in
                SilentAxis.shade(&context, size, marks, end, fm, labels: false)
                let mid: CGFloat = size.height / 2
                context.stroke(Path { $0.move(to: CGPoint(x: 0, y: mid)); $0.addLine(to: CGPoint(x: size.width, y: mid)) }, with: .color(fm.border), lineWidth: 1)
                for axis in 0..<3 {
                    var path: Path = Path()
                    for (i, v) in values.enumerated() {
                        let value: Double = axis == 0 ? v.x : (axis == 1 ? v.y : v.z)
                        let point: CGPoint = CGPoint(x: SilentAxis.x(times[i], end, size.width), y: mid - CGFloat(value / scale) * (mid - 2))
                        if i == 0 { path.move(to: point) } else { path.addLine(to: point) }
                    }
                    context.stroke(path, with: .color(colors[axis]), style: StrokeStyle(lineWidth: 1.5, lineCap: .round, lineJoin: .round))
                }
                context.draw(Text("±\(SilentAxis.format(scale, unit))").font(.system(size: 9)).foregroundColor(fm.textTertiary), at: CGPoint(x: size.width - 2, y: 2), anchor: .topTrailing)
            }
            .frame(height: 34)
        }
        .accessibilityElement(children: .ignore)
        .accessibilityLabel("\(title): " + (0..<3).map { "\(SilentAxis.names[$0]) \(SilentAxis.format(parts[$0], unit))" }.joined(separator: ", ") + " \(unit)")
    }
}

struct SilentEnergyStrip: View {
    @Environment(\.fmPalette) private var fm
    let energies: [Double]
    let times: [Double]
    let end: Double
    let marks: [SilentMark]
    let noise: SilentNoiseFloor?
    let active: Bool

    var body: some View {
        let top: Double = max(noise.map { $0.on * 2 } ?? 0.5, energies.max() ?? 0)
        VStack(alignment: .leading, spacing: 1) {
            Text(active ? "Energy · hearing" : "Energy · words heard").fmText(.labelSmall, color: active ? fm.accent : fm.textSecondary)
            Canvas { (context: inout GraphicsContext, size: CGSize) in
                SilentAxis.shade(&context, size, marks, end, fm, labels: true)
                func y(_ value: Double) -> CGFloat { size.height - CGFloat(min(value / top, 1)) * (size.height - 2) }
                if let noise {
                    for (level, alpha) in [(noise.on, 0.8), (noise.off, 0.4)] {
                        var line: Path = Path()
                        line.move(to: CGPoint(x: 0, y: y(level)))
                        line.addLine(to: CGPoint(x: size.width, y: y(level)))
                        context.stroke(line, with: .color(fm.textTertiary.opacity(alpha)), style: StrokeStyle(lineWidth: 1, dash: [3, 3]))
                    }
                }
                var area: Path = Path()
                area.move(to: CGPoint(x: times.first.map { SilentAxis.x($0, end, size.width) } ?? 0, y: size.height))
                for (i, e) in energies.enumerated() { area.addLine(to: CGPoint(x: SilentAxis.x(times[i], end, size.width), y: y(e))) }
                area.addLine(to: CGPoint(x: times.last.map { SilentAxis.x($0, end, size.width) } ?? 0, y: size.height))
                area.closeSubpath()
                context.fill(area, with: .color((active ? fm.accent : fm.textTertiary).opacity(0.35)))
            }
            .frame(height: 40)
        }
        .frame(maxWidth: .infinity)
        .accessibilityElement(children: .ignore)
        .accessibilityLabel(active ? "Motion energy, hearing a word" : "Motion energy")
    }
}

struct SilentSpectrumView: View {
    @Environment(\.fmPalette) private var fm
    let spectrum: SilentSpectrum

    var body: some View {
        let peak: String = spectrum.peakHz.map { String(format: "%.1f Hz", $0) } ?? "–"
        VStack(alignment: .leading, spacing: 1) {
            Text("Frequency · peak \(peak)").fmText(.labelSmall, color: fm.textSecondary).monospacedDigit()
            Canvas { (context: inout GraphicsContext, size: CGSize) in
                let bins: [Double] = Array(spectrum.power.dropFirst())
                let top: Double = max(bins.max() ?? 0, 1)
                let label: CGFloat = 10
                let height: CGFloat = size.height - label
                if !bins.isEmpty {
                    let step: CGFloat = size.width / CGFloat(bins.count)
                    for (k, p) in bins.enumerated() {
                        let h: CGFloat = max(1, CGFloat(p / top) * height)
                        let hz: Double = Double(k + 1) * spectrum.binHz
                        let isPeak: Bool = spectrum.peakHz.map { abs($0 - hz) < 1e-9 } ?? false
                        context.fill(Path(roundedRect: CGRect(x: CGFloat(k) * step + 0.5, y: height - h, width: max(1, step - 1), height: h), cornerRadius: 1), with: .color(isPeak ? fm.accent : fm.accent.opacity(0.45)))
                    }
                }
                for hz in [0.0, 5.0, 10.0] {
                    let x: CGFloat = CGFloat(hz / (spectrum.binHz * Double(max(spectrum.power.count - 1, 1)))) * size.width
                    context.draw(Text(hz == 10 ? "10 Hz" : "\(Int(hz))").font(.system(size: 8)).foregroundColor(fm.textTertiary), at: CGPoint(x: min(x, size.width), y: size.height), anchor: hz == 0 ? UnitPoint.bottomLeading : (hz == 10 ? UnitPoint.bottomTrailing : UnitPoint.bottom))
                }
            }
            .frame(height: 40)
        }
        .frame(maxWidth: .infinity)
        .accessibilityElement(children: .ignore)
        .accessibilityLabel("Head motion frequency, peak \(peak)")
    }
}

struct SilentConfusionMap: View {
    @Environment(\.fmPalette) private var fm
    let evaluation: SilentEvaluation

    var body: some View {
        Canvas { (context: inout GraphicsContext, size: CGSize) in
            let n: Int = evaluation.words.count
            guard n > 0 else { return }
            let cell: CGFloat = min(size.width, size.height) / CGFloat(n)
            let totals: [Int] = evaluation.confusion.map { $0.reduce(0, +) }
            for row in 0..<n {
                for column in 0..<n {
                    let count: Int = evaluation.confusion[row][column]
                    let share: Double = totals[row] > 0 ? Double(count) / Double(totals[row]) : 0
                    let color: Color = count == 0 ? fm.bgOverlay : (row == column ? fm.accent.opacity(0.25 + 0.75 * share) : fm.warning.opacity(0.25 + 0.75 * share))
                    let inset: CGFloat = n > 24 ? 0 : 0.5
                    context.fill(Path(CGRect(x: CGFloat(column) * cell + inset, y: CGFloat(row) * cell + inset, width: cell - 2 * inset, height: cell - 2 * inset)), with: .color(color))
                }
            }
        }
        .accessibilityElement(children: .ignore)
        .accessibilityLabel("Mix-up map: rows are the words you mouthed, columns what each was taken for")
    }
}
