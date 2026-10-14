import FMBase
import FMTools
import SwiftUI

struct SpeechScreen: View {
    @Environment(\.fmPalette) private var fm
    @State private var model: SpeechPlaygroundModel?

    var body: some View {
        content
            .navigationTitle(SpeechPlaygroundModel.title)
            .navigationBarTitleDisplayMode(.inline)
            .background(fm.canvas)
            .onAppear {
                if model == nil {
                    model = SpeechPlaygroundModel(
                        engine: AppleSpeechEngine(), prefs: HealthFeature.prefs, language: AppleSpeechEngine.language)
                }
                model?.load()
            }
            .onDisappear { model?.stop() }
    }

    @ViewBuilder private var content: some View {
        if let model = model {
            SpeechContent(model: model)
        } else {
            Color.clear
        }
    }
}

private struct SpeechContent: View {
    @Environment(\.fmPalette) private var fm
    @Environment(\.scenePhase) private var scenePhase
    @Bindable var model: SpeechPlaygroundModel
    @State private var choosingVoice: Bool = false
    @State private var shared: ToolsSheet<URL>?
    @FocusState private var editing: Bool

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: FmSpace.x4) {
                textCard
                samples
                controls
                FmNoticeLine(model.notice?.message, isError: model.notice?.isError ?? false, onDismiss: { model.notice = nil })
                voiceCard
                tuningCard
                if model.showsVoiceHint {
                    hint(SpeechPlaygroundModel.noNaturalVoice, icon: "download_outlined")
                }
                if model.showsPersonalVoiceHint {
                    personalVoiceCard
                }
            }
            .padding(FmSpace.x4)
        }
        .scrollDismissesKeyboard(.interactively)
        .onChange(of: scenePhase) { (_: ScenePhase, phase: ScenePhase) in
            if phase == .active {
                model.load()
            }
        }
        .sheet(isPresented: $choosingVoice) {
            SpeechVoiceSheet(model: model)
        }
        .sheet(item: $shared) { (item: ToolsSheet<URL>) in
            AgentActivityView(items: [item.value], onDone: { shared = nil })
                .presentationDetents([.medium, .large])
        }
        .toolbar {
            ToolbarItemGroup(placement: .keyboard) {
                Spacer()
                Button("Done") { editing = false }
            }
        }
    }

    @ViewBuilder private var textBody: some View {
        if let highlight = model.highlight {
            Text(attributed(highlight))
                .fmText(.bodyLarge)
                .frame(maxWidth: .infinity, minHeight: 168, alignment: .topLeading)
                .padding(FmSpace.x3)
                .contentShape(Rectangle())
                .onTapGesture { model.stop() }
                .accessibilityIdentifier("speech-highlight")
        } else {
            ZStack(alignment: .topLeading) {
                if model.text.isEmpty {
                    Text(SpeechPlaygroundModel.placeholder)
                        .fmText(.bodyLarge, color: fm.textTertiary)
                        .padding(.horizontal, FmSpace.x3 + 5)
                        .padding(.vertical, FmSpace.x3 + 8)
                        .allowsHitTesting(false)
                }
                TextEditor(text: $model.text)
                    .focused($editing)
                    .fmText(.bodyLarge)
                    .scrollContentBackground(.hidden)
                    .tint(fm.accent)
                    .frame(minHeight: 168, maxHeight: 320)
                    .padding(FmSpace.x2)
                    .accessibilityIdentifier("speech-text")
            }
        }
    }

    private var textCard: some View {
        VStack(alignment: .leading, spacing: 0) {
            textBody
            HStack(spacing: FmSpace.x2) {
                Text(model.summary)
                    .fmText(.bodySmall, tabular: true)
                    .frame(maxWidth: .infinity, alignment: .leading)
                if !model.text.isEmpty {
                    FmTextButton("Clear", onPressed: { model.clear() })
                        .accessibilityIdentifier("speech-clear")
                }
            }
            .padding(.leading, FmSpace.x3)
            .padding(.trailing, FmSpace.x1)
        }
        .background(fm.bgRaised)
        .clipShape(FmRoundedShape(radius: FmRadius.lg))
        .overlay {
            FmRoundedShape(radius: FmRadius.lg)
                .strokeBorder(editing ? fm.accent : fm.border, lineWidth: editing ? 1.2 : 1)
        }
    }

    private func attributed(_ highlight: SpeechHighlight) -> AttributedString {
        var word: AttributedString = AttributedString(highlight.word)
        word.foregroundColor = fm.accent
        word.backgroundColor = fm.accentSubtle
        word.inlinePresentationIntent = .stronglyEmphasized
        var text: AttributedString = AttributedString(highlight.before)
        text.append(word)
        text.append(AttributedString(highlight.after))
        return text
    }

    private var samples: some View {
        ToolsFlowLayout(spacing: FmSpace.x2, lineSpacing: FmSpace.x2) {
            ForEach(SpeechPlaygroundModel.samples) { (sample: SpeechSample) in
                FmOutlinedButton(sample.title, minHeight: 34, onPressed: {
                    editing = false
                    model.useSample(sample)
                })
                .accessibilityIdentifier("speech-sample-\(sample.title.lowercased())")
            }
        }
    }

    private var playLabel: String {
        switch model.state {
        case .idle: return "Speak"
        case .speaking: return model.previewing == nil ? "Pause" : "Stop preview"
        case .paused: return "Resume"
        }
    }

    private var playIcon: String {
        model.state == .speaking ? "pause_rounded" : "play_arrow_rounded"
    }

    private var controls: some View {
        HStack(spacing: FmSpace.x2) {
            FmFilledButton(
                playLabel, icon: playIcon, background: fm.accent, foreground: .white, minHeight: 52,
                fillWidth: true, role: .titleSmall,
                onPressed: model.canSpeak || model.state != .idle ? {
                    editing = false
                    if model.previewing != nil {
                        model.stop()
                    } else {
                        model.toggle()
                    }
                } : nil
            )
            .accessibilityIdentifier("speech-play")
            if model.state != .idle {
                FmIconButton(label: "Stop", onPressed: { model.stop() }) {
                    Image(systemName: "stop.fill")
                        .font(.system(size: 20, weight: .semibold))
                        .foregroundStyle(fm.textPrimary)
                }
                .accessibilityIdentifier("speech-stop")
            }
            FmIconButton(label: "Save audio", onPressed: model.canSpeak && !model.rendering ? {
                editing = false
                Task {
                    if let url = await model.renderAudio() {
                        shared = ToolsSheet(url)
                    }
                }
            } : nil) {
                if model.rendering {
                    ProgressView()
                } else {
                    FmIcon("ios_share", size: 22, color: fm.textPrimary)
                }
            }
            .accessibilityIdentifier("speech-save")
        }
    }

    private var voiceCard: some View {
        FmCard(onTap: { choosingVoice = true }) {
            HStack(spacing: FmSpace.x3) {
                Image(systemName: "person.wave.2.fill")
                    .font(.system(size: 20))
                    .foregroundStyle(fm.accent)
                    .frame(width: 36, height: 36)
                    .background(fm.accentSubtle)
                    .clipShape(Circle())
                VStack(alignment: .leading, spacing: 2) {
                    Text("Voice")
                        .fmText(.labelSmall)
                    if let voice = model.voice {
                        HStack(spacing: FmSpace.x2) {
                            Text(voice.name)
                                .fmText(.titleSmall)
                                .lineLimit(1)
                            SpeechQualityBadge(quality: voice.quality)
                        }
                        Text(SpeechLanguage.name(voice.language))
                            .fmText(.bodySmall)
                    } else {
                        Text("No voices on this device")
                            .fmText(.bodyMedium, color: fm.textSecondary)
                    }
                }
                .frame(maxWidth: .infinity, alignment: .leading)
                Image(systemName: "chevron.right")
                    .font(.system(size: 14, weight: .semibold))
                    .foregroundStyle(fm.textTertiary)
            }
        }
        .accessibilityIdentifier("speech-voice")
    }

    private var tuningCard: some View {
        FmCard {
            VStack(alignment: .leading, spacing: FmSpace.x3) {
                HStack {
                    Text("Delivery")
                        .fmText(.labelSmall)
                        .frame(maxWidth: .infinity, alignment: .leading)
                    if model.speed != SpeechTuning.defaultSpeed || model.pitch != SpeechTuning.defaultPitch {
                        FmTextButton("Reset", onPressed: { model.resetTuning() })
                    }
                }
                slider(
                    "Speed", value: $model.speed, range: SpeechTuning.speeds,
                    caption: SpeechTuning.label(speed: model.speed), id: "speech-speed")
                slider(
                    "Pitch", value: $model.pitch, range: SpeechTuning.pitches,
                    caption: SpeechTuning.label(pitch: model.pitch), id: "speech-pitch")
            }
        }
    }

    private func slider(
        _ title: String, value: Binding<Double>, range: ClosedRange<Double>, caption: String, id: String
    ) -> some View {
        VStack(alignment: .leading, spacing: FmSpace.x1) {
            HStack {
                Text(title)
                    .fmText(.bodyMedium)
                Spacer()
                Text(caption)
                    .fmText(.bodySmall, tabular: true)
            }
            Slider(value: value, in: range, step: 0.05, onEditingChanged: { (sliding: Bool) in
                if !sliding {
                    model.tuningChanged()
                }
            })
            .tint(fm.accent)
            .accessibilityLabel(Text(title))
            .accessibilityValue(Text(caption))
            .accessibilityIdentifier(id)
        }
    }

    private func hint(_ text: String, icon: String) -> some View {
        FmCard {
            HStack(alignment: .top, spacing: FmSpace.x3) {
                FmIcon(icon, size: 20, color: fm.accent)
                Text(text)
                    .fmText(.bodyMedium, color: fm.textSecondary)
                    .frame(maxWidth: .infinity, alignment: .leading)
            }
        }
    }

    private var personalVoiceCard: some View {
        FmCard {
            VStack(alignment: .leading, spacing: FmSpace.x3) {
                HStack(alignment: .top, spacing: FmSpace.x3) {
                    Image(systemName: "person.crop.circle.badge.checkmark")
                        .font(.system(size: 20))
                        .foregroundStyle(fm.accent)
                    Text(SpeechPlaygroundModel.personalVoiceHint)
                        .fmText(.bodyMedium, color: fm.textSecondary)
                        .frame(maxWidth: .infinity, alignment: .leading)
                }
                if model.personalVoiceStatus == .notDetermined {
                    FmOutlinedButton("Use my Personal Voice", onPressed: {
                        Task { await model.requestPersonalVoice() }
                    })
                    .accessibilityIdentifier("speech-personal-voice")
                }
            }
        }
    }
}

enum SpeechLanguage {
    static func name(_ code: String) -> String {
        Locale.current.localizedString(forIdentifier: code) ?? code
    }
}

struct SpeechQualityBadge: View {
    @Environment(\.fmPalette) private var fm
    let quality: SpeechVoiceQuality

    var body: some View {
        switch quality {
        case .personal: FmStatusBadge(label: "Personal", color: fm.accent)
        case .premium: FmStatusBadge(label: "Premium", color: fm.success)
        case .enhanced: FmStatusBadge(label: "Enhanced", color: fm.warning)
        case .standard: EmptyView()
        }
    }
}

private struct SpeechVoiceSheet: View {
    @Environment(\.fmPalette) private var fm
    @Environment(\.dismiss) private var dismiss
    @Bindable var model: SpeechPlaygroundModel

    private var sections: [SpeechVoiceSection] {
        SpeechVoices.sections(model.voices, language: model.language, title: SpeechLanguage.name)
    }

    var body: some View {
        NavigationStack {
            List {
                if model.showsVoiceHint {
                    Section {
                        Text(SpeechPlaygroundModel.noNaturalVoice)
                            .font(.footnote)
                            .foregroundStyle(.secondary)
                    }
                }
                ForEach(sections) { (section: SpeechVoiceSection) in
                    Section(section.title) {
                        ForEach(section.voices) { (voice: SpeechVoice) in
                            row(voice)
                        }
                    }
                }
            }
            .navigationTitle("Voices")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button("Done") { dismiss() }
                }
            }
        }
        .onDisappear {
            if model.previewing != nil {
                model.stop()
            }
        }
    }

    private func row(_ voice: SpeechVoice) -> some View {
        let playing: Bool = model.previewing == voice.id && model.state != .idle
        return HStack(spacing: FmSpace.x3) {
            Button {
                model.select(voice)
            } label: {
                HStack(spacing: FmSpace.x2) {
                    Image(systemName: "checkmark")
                        .font(.system(size: 14, weight: .semibold))
                        .foregroundStyle(fm.accent)
                        .opacity(model.voiceId == voice.id ? 1 : 0)
                    Text(voice.name)
                        .foregroundStyle(voice.robotic ? fm.textSecondary : fm.textPrimary)
                        .lineLimit(1)
                    SpeechQualityBadge(quality: voice.quality)
                    Spacer(minLength: 0)
                }
                .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            .accessibilityAddTraits(model.voiceId == voice.id ? .isSelected : [])
            Button {
                model.preview(voice)
            } label: {
                Image(systemName: playing ? "stop.circle.fill" : "play.circle")
                    .font(.system(size: 24))
                    .foregroundStyle(fm.accent)
            }
            .buttonStyle(.plain)
            .accessibilityLabel(Text(playing ? "Stop preview" : "Preview \(voice.name)"))
        }
    }
}
