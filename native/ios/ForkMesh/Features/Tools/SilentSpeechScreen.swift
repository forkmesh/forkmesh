import FMTools
import SwiftUI

struct SilentSpeechScreen: View {
    @Environment(\.fmPalette) private var fm
    @Bindable private var listener: SilentSpeechListener = SilentSpeechListener.shared
    @State private var newWord: String = ""
    @State private var newPhrase: String = ""

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: FmSpace.x3) {
                statusCard
                if let problem = listener.problem {
                    FmListRow(
                        title: problem,
                        leading: AnyView(FmIcon("error", color: fm.danger)),
                        onTap: settingsAction,
                        showChevron: listener.denied)
                }
                wordsSection
                trainSection
                checkSection
                decodeSection
                VStack(alignment: .leading, spacing: FmSpace.x1) {
                    FmSectionHeader(title: "How it works", icon: "info_outline")
                    Text(
                        "AirPods Pro, AirPods Max, AirPods (3rd generation and later) and Beats Fit Pro report how your head moves about 25 times a second. When you mouth a word, your jaw and face move your head a little, and each word moves it its own way. Silent Speech keeps your takes of each word and matches a new movement to the closest ones, letting for tempo, so it learns your words, not anyone else's."
                    )
                    .fmText(.bodySmall, color: fm.textSecondary)
                    Text("It is an experiment with a small vocabulary. Reseating the earbuds can change the signal; record a few fresh takes if it starts missing. Motion data stays on this phone.")
                        .fmText(.bodySmall, color: fm.textSecondary)
                }
            }
            .padding(FmSpace.x4)
        }
        .background(fm.canvas)
        .navigationTitle("Silent Speech")
        .navigationBarTitleDisplayMode(.inline)
        .onAppear { listener.start() }
        .onDisappear { listener.stop() }
    }

    private var statusCard: some View {
        FmGlassSurface(cornerRadius: FmRadius.lg) {
            VStack(alignment: .leading, spacing: FmSpace.x2) {
                HStack(spacing: FmSpace.x3) {
                    FmIcon("headphones", size: 28, color: listener.connected ? fm.success : fm.textTertiary)
                    VStack(alignment: .leading, spacing: 2) {
                        Text(listener.connected ? "Reading your earbuds" : "Put in your AirPods")
                            .fmText(.titleMedium)
                        Text(statusDetail)
                            .fmText(.bodySmall, color: fm.textSecondary)
                    }
                    Spacer(minLength: 0)
                }
                SilentLevelBar(level: listener.level, active: listener.hold != nil || listener.detecting)
                    .accessibilityLabel("Motion level")
                    .accessibilityValue("\(Int((min(listener.level, 2) / 2 * 100).rounded())) percent")
            }
            .padding(FmSpace.x3)
        }
    }

    private var statusDetail: String {
        guard listener.supported else { return "Head tracking is not available on this device" }
        guard listener.connected else { return "Needs AirPods or Beats with head tracking, playing from this phone" }
        if let rate = listener.rate { return "Head motion at \(Int(rate.rounded())) Hz" }
        return "Waiting for motion"
    }

    private var wordsSection: some View {
        VStack(alignment: .leading, spacing: FmSpace.x1) {
            FmSectionHeader(title: "Words", icon: "tune", count: listener.dataset.words.count)
            ForEach(listener.dataset.words, id: \.word) { (word: SilentWord) in
                FmListRow(
                    title: word.word,
                    subtitle: wordDetail(word),
                    leading: AnyView(FmIcon(listener.selectedWord == word.word ? "check_circle" : "touch_app", color: listener.selectedWord == word.word ? fm.accent : fm.textTertiary)),
                    trailing: AnyView(wordActions(word)),
                    onTap: { listener.selectedWord = word.word })
                    .accessibilityIdentifier("silent-speech-word-\(word.word)")
            }
            FmTextField(text: $newWord, hint: "New word")
                .accessibilityIdentifier("silent-speech-new-word")
            FmTextField(text: $newPhrase, hint: "Say instead (optional), for example: Could I have some water?")
            FmOutlinedButton("Add word", icon: "add", minHeight: 34, onPressed: addWord)
                .disabled(SilentDataset.clean(newWord).isEmpty)
                .accessibilityIdentifier("silent-speech-add-word")
        }
    }

    private func wordDetail(_ word: SilentWord) -> String {
        let takes: Int = listener.dataset.count(word.word)
        let count: String = takes == 1 ? "1 take" : "\(takes) takes"
        return word.phrase.isEmpty ? count : "\(count) · says “\(word.phrase)”"
    }

    private func wordActions(_ word: SilentWord) -> some View {
        HStack(spacing: FmSpace.x1) {
            if listener.dataset.count(word.word) > 0 {
                Button { listener.undoTake(word.word) } label: { FmIcon("undo", size: 20, color: fm.textSecondary) }
                    .buttonStyle(.plain)
                    .accessibilityLabel("Delete the last take of \(word.word)")
            }
            Button { listener.removeWord(word.word) } label: { FmIcon("delete_outline", size: 20, color: fm.textSecondary) }
                .buttonStyle(.plain)
                .accessibilityLabel("Remove \(word.word) and its takes")
        }
    }

    private func addWord() {
        if listener.addWord(newWord, phrase: newPhrase) {
            newWord = ""
            newPhrase = ""
        }
    }

    private var trainSection: some View {
        VStack(alignment: .leading, spacing: FmSpace.x1) {
            FmSectionHeader(title: "Teach", icon: "touch_app")
            readSection
            if let word = listener.selectedWord {
                SilentHoldButton(
                    label: listener.hold == .train(word) ? "Mouth “\(word)”, then let go" : "Hold and mouth “\(word)”",
                    active: listener.hold == .train(word),
                    enabled: listener.connected && listener.reading == nil,
                    onBegin: { listener.beginHold(.train(word)) },
                    onEnd: { listener.endHold() })
                    .accessibilityIdentifier("silent-speech-train")
                Text("Or teach one word at a time: keep your head still, hold the button, mouth the word without a sound, then let go.")
                    .fmText(.bodySmall, color: fm.textSecondary)
            } else {
                Text("Add a word to teach it.")
                    .fmText(.bodySmall, color: fm.textSecondary)
            }
        }
    }

    @ViewBuilder
    private var readSection: some View {
        if let reading = listener.reading {
            FmGlassSurface(cornerRadius: FmRadius.md) {
                VStack(alignment: .leading, spacing: FmSpace.x2) {
                    sentenceText(reading)
                        .fmText(.titleLarge, color: fm.textPrimary)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .accessibilityIdentifier("silent-speech-sentence")
                    Text(readingDetail(reading))
                        .fmText(.bodySmall, color: fm.textSecondary)
                }
                .padding(FmSpace.x3)
            }
            Text(listener.calibrating ? "Keep your head still for two seconds while it measures how still you are." : "Mouth each word without a sound, with a short stop between words. The highlight moves on as each word is heard.")
                .fmText(.bodySmall, color: fm.textSecondary)
            ScrollView(.horizontal, showsIndicators: false) {
                HStack(spacing: FmSpace.x2) {
                    FmOutlinedButton("Start again", icon: "refresh", minHeight: 34, onPressed: { listener.readAgain() })
                        .disabled(reading.position == 0)
                    FmOutlinedButton("Skip", icon: "skip_next", minHeight: 34, onPressed: { listener.skipSentence() })
                    if !listener.lastSentence.isEmpty {
                        FmOutlinedButton("Drop last sentence", icon: "undo", minHeight: 34, onPressed: { listener.dropLastSentence() })
                    }
                    FmOutlinedButton("Finish", icon: "stop", minHeight: 34, onPressed: { listener.finishReading() })
                        .accessibilityIdentifier("silent-speech-read-finish")
                }
            }
        } else {
            FmOutlinedButton("Read sentences for 5 minutes", icon: "play_arrow", minHeight: 44, onPressed: { listener.startReading() })
                .disabled(!listener.connected || listener.dataset.words.count < 2)
                .accessibilityIdentifier("silent-speech-read")
            Text(listener.readingSummary.map { "\($0) Check my words to see how well it tells them apart." } ?? "The best way to teach it: read short sentences made of your words for five minutes. Each word you mouth is saved as a take, so every word gets dozens of takes.")
                .fmText(.bodySmall, color: fm.textSecondary)
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
        let saved: String = reading.collected == 1 ? "1 take saved" : "\(reading.collected) takes saved"
        return "Sentence \(reading.sentencesRead + 1) · \(reading.clock) · \(saved)"
    }

    private var checkSection: some View {
        VStack(alignment: .leading, spacing: FmSpace.x1) {
            FmSectionHeader(title: "Check", icon: "check_circle")
            Text("Tests every take against all the others, then against 500 shuffles of the labels, to show whether your head motion really tells the words apart.")
                .fmText(.bodySmall, color: fm.textSecondary)
            ForEach(SilentChannel.allCases.filter { listener.hasAcceleration || $0 != .acceleration }, id: \.self) { (channel: SilentChannel) in
                Toggle(isOn: channelBinding(channel)) {
                    VStack(alignment: .leading, spacing: 2) {
                        Text(channel.label).fmText(.bodyMedium, color: fm.textPrimary)
                        Text(channel.detail).fmText(.bodySmall, color: fm.textSecondary)
                    }
                }
                .tint(fm.accent)
                .accessibilityIdentifier("silent-speech-channel-\(channel.rawValue)")
            }
            if let requirement = SilentEvaluator.requirement(listener.dataset) {
                Text(requirement).fmText(.bodySmall, color: fm.textSecondary)
            } else {
                FmOutlinedButton(listener.evaluating ? "Checking…" : "Check my words", icon: "check_circle", minHeight: 34, busy: listener.evaluating, onPressed: { listener.evaluate() })
                    .accessibilityIdentifier("silent-speech-evaluate")
            }
            if let result = listener.evaluation {
                Text(result.headline).fmText(.titleSmall, color: fm.textPrimary)
                Text(result.verdict).fmText(.bodySmall, color: result.significant ? fm.success : fm.textSecondary)
                    .accessibilityIdentifier("silent-speech-verdict")
                SilentConfusionGrid(evaluation: result)
            }
        }
    }

    private func channelBinding(_ channel: SilentChannel) -> Binding<Bool> {
        Binding<Bool>(
            get: { listener.channels.contains(channel) },
            set: { (on: Bool) in
                if on { listener.channels.insert(channel) } else { listener.channels.remove(channel) }
            })
    }

    private var decodeSection: some View {
        VStack(alignment: .leading, spacing: FmSpace.x1) {
            FmSectionHeader(title: "Speak silently", icon: "hearing")
            SilentHoldButton(
                label: listener.hold == .decode ? "Mouth a word, then let go" : "Hold and mouth a word",
                active: listener.hold == .decode,
                enabled: listener.connected && !listener.dataset.takes.isEmpty && listener.reading == nil,
                onBegin: { listener.beginHold(.decode) },
                onEnd: { listener.endHold() })
                .accessibilityIdentifier("silent-speech-decode")
            Toggle(isOn: $listener.autoDetect) {
                Text("Listen without holding").fmText(.bodyMedium, color: fm.textPrimary)
            }
            .tint(fm.accent)
            .accessibilityIdentifier("silent-speech-auto")
            Text(autoDetail).fmText(.bodySmall, color: fm.textSecondary)
            if listener.autoDetect {
                FmOutlinedButton(listener.calibrating ? "Keep still…" : "Measure stillness again", icon: "refresh", minHeight: 34, onPressed: { listener.calibrate() })
                    .disabled(listener.calibrating || !listener.connected)
            }
            Toggle(isOn: $listener.speakAloud) {
                Text("Say each word aloud").fmText(.bodyMedium, color: fm.textPrimary)
            }
            .tint(fm.accent)
            .accessibilityIdentifier("silent-speech-speak")
            Toggle(isOn: rejectBinding) {
                Text("Skip far-off matches").fmText(.bodyMedium, color: fm.textPrimary)
            }
            .tint(fm.accent)
            .disabled(listener.maxDistance == nil && listener.evaluation?.suggestedMaxDistance == nil)
            Text("Uses the limit found by Check my words, so a cough or a nod is not written down as a word.")
                .fmText(.bodySmall, color: fm.textSecondary)
            if let match = listener.lastMatch {
                Text(match.summary)
                    .fmText(.bodyMedium, color: match.accepted ? fm.textPrimary : fm.warning)
                    .accessibilityIdentifier("silent-speech-last")
            }
            FmGlassSurface(cornerRadius: FmRadius.md) {
                Text(listener.transcript.text.isEmpty ? "Words you mouth appear here." : listener.transcript.text)
                    .fmText(.bodyLarge, color: listener.transcript.text.isEmpty ? fm.textTertiary : fm.textPrimary)
                    .frame(maxWidth: .infinity, minHeight: 56, alignment: .topLeading)
                    .padding(FmSpace.x3)
                    .textSelection(.enabled)
                    .accessibilityIdentifier("silent-speech-transcript")
            }
            ScrollView(.horizontal, showsIndicators: false) {
                HStack(spacing: FmSpace.x2) {
                    FmOutlinedButton("Undo", icon: "undo", minHeight: 34, onPressed: { listener.undoWord() })
                    FmOutlinedButton("Copy", icon: "content_copy", minHeight: 34, onPressed: { listener.copyTranscript() })
                    FmOutlinedButton("Say it", icon: "volume_up_outlined", minHeight: 34, onPressed: { listener.speak(listener.transcript.words.map { listener.dataset.phrase($0) }.joined(separator: " ")) })
                    FmOutlinedButton("Clear", icon: "close", minHeight: 34, onPressed: { listener.clearTranscript() })
                }
            }
            .disabled(listener.transcript.words.isEmpty)
        }
    }

    private var autoDetail: String {
        guard listener.autoDetect else { return "Hold the button for each word, or turn this on to pick out words by themselves." }
        if listener.calibrating { return "Keep your head still for two seconds while it measures how still you are." }
        if listener.noise == nil { return "Waiting for your earbuds to measure stillness." }
        return listener.detecting ? "Hearing a word…" : "Listening for words. Keep your head still between them."
    }

    private var rejectBinding: Binding<Bool> {
        Binding<Bool>(
            get: { listener.maxDistance != nil },
            set: { (on: Bool) in
                if on { listener.maxDistance = listener.evaluation?.suggestedMaxDistance ?? listener.maxDistance } else { listener.resetLimit() }
            })
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

struct SilentHoldButton: View {
    @Environment(\.fmPalette) private var fm
    let label: String
    let active: Bool
    let enabled: Bool
    let onBegin: () -> Void
    let onEnd: () -> Void
    @State private var pressed: Bool = false

    var body: some View {
        Text(label)
            .fmText(.labelLarge, color: active ? fm.onPrimary : (enabled ? fm.accent : fm.textDisabled))
            .frame(maxWidth: .infinity, minHeight: 64)
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

struct SilentLevelBar: View {
    @Environment(\.fmPalette) private var fm
    let level: Double
    let active: Bool

    var body: some View {
        GeometryReader { (proxy: GeometryProxy) in
            ZStack(alignment: .leading) {
                Capsule().fill(fm.border)
                Capsule()
                    .fill(active ? fm.accent : fm.textTertiary)
                    .frame(width: proxy.size.width * min(max(level / 2, 0), 1))
            }
        }
        .frame(height: 6)
        .animation(.linear(duration: 0.1), value: level)
    }
}

struct SilentConfusionGrid: View {
    @Environment(\.fmPalette) private var fm
    let evaluation: SilentEvaluation

    var body: some View {
        VStack(alignment: .leading, spacing: 2) {
            Text("Rows: the word you mouthed. Columns: what it was taken for.")
                .fmText(.bodySmall, color: fm.textSecondary)
            ScrollView(.horizontal, showsIndicators: false) {
                Grid(horizontalSpacing: 2, verticalSpacing: 2) {
                    GridRow {
                        Text("").frame(width: 64)
                        ForEach(evaluation.words, id: \.self) { (word: String) in
                            Text(word).fmText(.labelSmall, color: fm.textSecondary).lineLimit(1).frame(width: 44)
                        }
                    }
                    ForEach(evaluation.words.indices, id: \.self) { (row: Int) in
                        GridRow {
                            Text(evaluation.words[row]).fmText(.labelSmall, color: fm.textSecondary).lineLimit(1).frame(width: 64, alignment: .leading)
                            ForEach(0..<evaluation.words.count, id: \.self) { (column: Int) in
                                let count: Int = evaluation.confusion[row][column]
                                Text(count == 0 ? "·" : "\(count)")
                                    .fmText(.labelMedium, color: count == 0 ? fm.textTertiary : (row == column ? fm.onPrimary : fm.textPrimary))
                                    .frame(width: 44, height: 28)
                                    .background(RoundedRectangle(cornerRadius: FmRadius.sm).fill(row == column && count > 0 ? fm.accent : fm.bgRaised))
                            }
                        }
                    }
                }
            }
            .accessibilityElement(children: .combine)
        }
    }
}
