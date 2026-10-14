import AVFoundation
import FMApi
import FMBase
import FMTools
import SwiftUI
import UIKit

struct StretchScreen: View {
    @Environment(\.fmPalette) private var fm
    @State private var model: StretchModel?

    init() {
        if let api = Services.get(ApiService.self) {
            _model = State(initialValue: StretchModel(
                api: StretchAPI(api: api), health: HealthFeature.bridge, prefs: HealthFeature.prefs))
        } else {
            _model = State(initialValue: nil)
        }
    }

    var body: some View {
        content
            .navigationTitle(StretchModel.title)
            .navigationBarTitleDisplayMode(.inline)
            .background(fm.canvas)
    }

    @ViewBuilder private var content: some View {
        if let model = model {
            StretchContent(model: model)
                .task { await model.load() }
        } else {
            FmEmptyState(icon: "self_improvement", title: StretchModel.unavailableTitle)
        }
    }
}

@MainActor
final class StretchVoice: StretchSpeaker {
    private let synthesizer: AVSpeechSynthesizer = AVSpeechSynthesizer()
    private let prefs: KeyValueStore
    private var chosenVoice: String?

    init(prefs: KeyValueStore) {
        self.prefs = prefs
    }

    private var voiceId: String {
        if let chosenVoice { return chosenVoice }
        let language: String = AppleSpeechEngine.language
        var picked: String = prefs.string(forKey: SpeechPrefs.voice) ?? ""
        if picked.isEmpty {
            picked = SpeechVoices.best(AppleSpeechEngine().voices().filter { !$0.novelty }, language: language)?.id
                ?? AVSpeechSynthesisVoice(language: language)?.identifier ?? ""
        }
        chosenVoice = picked
        return picked
    }

    private var rate: Float {
        SpeechTuning.utteranceRate(speed: prefs.double(forKey: SpeechPrefs.speed) ?? SpeechTuning.defaultSpeed)
    }

    func say(_ text: String, interrupt: Bool) {
        guard !text.isEmpty else { return }
        if interrupt && (synthesizer.isSpeaking || synthesizer.isPaused) {
            synthesizer.stopSpeaking(at: .immediate)
        }
        PromptDictation.yieldOverhearing()
        let session: AVAudioSession = AVAudioSession.sharedInstance()
        try? session.setCategory(.playback, mode: .spokenAudio, options: [.duckOthers])
        try? session.setActive(true)
        synthesizer.speak(AppleSpeechEngine.utterance(text, voiceId: voiceId, rate: rate, pitch: 1))
    }

    func pause() {
        synthesizer.pauseSpeaking(at: .word)
    }

    func resume() {
        synthesizer.continueSpeaking()
    }

    func stop() {
        synthesizer.stopSpeaking(at: .immediate)
        guard !PromptDictation.isListening else { return }
        try? AVAudioSession.sharedInstance().setActive(false, options: .notifyOthersOnDeactivation)
    }
}

private struct StretchContent: View {
    @Environment(\.fmPalette) private var fm
    @Environment(\.scenePhase) private var scenePhase
    @Bindable var model: StretchModel
    @State private var voice: StretchVoice?
    @State private var confirmUninstall: Bool = false
    @State private var deleting: String?
    @State private var idleBefore: Bool?

    var body: some View {
        main
            .toolbar {
                if !model.inSession {
                    ToolbarItem(placement: .primaryAction) {
                        FmIconButton(icon: "refresh", label: "Refresh", onPressed: { Task { await model.load() } })
                    }
                }
            }
            .onAppear {
                let speaker: StretchVoice = voice ?? StretchVoice(prefs: HealthFeature.prefs)
                voice = speaker
                model.speaker = speaker
            }
            .onDisappear {
                if model.run?.finished == false {
                    Task { await model.stop() }
                }
                voice?.stop()
                keepAwake(false)
            }
            .onChange(of: running) { (_: Bool, now: Bool) in
                keepAwake(now)
            }
            .onChange(of: scenePhase) { (_: ScenePhase, phase: ScenePhase) in
                if phase == .active {
                    if !model.inSession {
                        Task { await model.load() }
                    }
                } else if running {
                    model.pause()
                }
            }
            .task(id: model.inSession) {
                while model.inSession && !Task.isCancelled {
                    try? await Task.sleep(nanoseconds: 100_000_000)
                    await model.tick()
                }
            }
            .fmDialog(isPresented: deletingBinding) {
                FmConfirmDialog(
                    title: "Remove this session?",
                    message: "It is removed from your account on every device. A workout already saved to Apple Health stays there.",
                    confirmLabel: "Remove", destructive: true,
                    onCancel: { deleting = nil },
                    onConfirm: {
                        let id: String = deleting ?? ""
                        deleting = nil
                        Task { await model.deleteSession(id) }
                    })
            }
            .fmDialog(isPresented: $confirmUninstall) {
                FmConfirmDialog(
                    title: "Uninstall Stretch Break?",
                    message: "Your sessions and settings are deleted from the web dashboard, the desktop app, and this app. Workouts saved to Apple Health stay there.",
                    confirmLabel: "Uninstall", destructive: true,
                    onCancel: { confirmUninstall = false },
                    onConfirm: {
                        confirmUninstall = false
                        Task { if await model.uninstall() { await PackagesFeature.removeFromAccount("stretch") } }
                    })
            }
    }

    private var running: Bool { model.run.map { !$0.finished } ?? false }

    private func keepAwake(_ on: Bool) {
        let application: UIApplication = UIApplication.shared
        if on {
            if idleBefore == nil {
                idleBefore = application.isIdleTimerDisabled
            }
            application.isIdleTimerDisabled = true
        } else if let before = idleBefore {
            application.isIdleTimerDisabled = before
            idleBefore = nil
        }
    }

    private var deletingBinding: Binding<Bool> {
        Binding<Bool>(
            get: { deleting != nil },
            set: { (shown: Bool) in
                if !shown { deleting = nil }
            })
    }

    @ViewBuilder private var main: some View {
        if let run = model.run, let library = model.library {
            StretchSessionView(model: model, run: run, library: library)
        } else if model.showsSpinner {
            FmCircularProgressIndicator()
                .frame(maxWidth: .infinity, maxHeight: .infinity)
        } else if model.showsError {
            FmEmptyState(icon: "self_improvement", title: StretchModel.unavailableTitle, message: model.error)
        } else if model.showsInstall {
            installPanel
        } else if model.overview != nil {
            dashboard
        }
    }

    private var installPanel: some View {
        VStack(spacing: FmSpace.x3) {
            if let library = model.library {
                StretchFigureCanvas(library: library, step: nil, seconds: 0)
                    .frame(width: 120, height: 160)
                    .accessibilityHidden(true)
            }
            Text(StretchModel.installTitle)
                .fmText(.titleMedium)
                .multilineTextAlignment(.center)
            Text(StretchModel.installMessage)
                .fmText(.bodyMedium, color: fm.textSecondary)
                .multilineTextAlignment(.center)
            FmFilledButton(StretchModel.installButton, onPressed: { Task { await model.install() } })
                .disabled(model.busy)
                .padding(.top, FmSpace.x1)
                .accessibilityIdentifier("stretch-install")
            noticeLine
        }
        .padding(FmSpace.x5)
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }

    private var noticeLine: some View {
        FmNoticeLine(model.notice?.message, isError: model.notice?.isError ?? false, onDismiss: { model.notice = nil })
    }

    private var dashboard: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: FmSpace.x3) {
                statsCard
                FmSectionHeader(title: "Routine")
                ForEach(model.routines) { (routine: StretchRoutine) in
                    routineCard(routine)
                }
                if let routine = model.chosenRoutine {
                    FmFilledButton(
                        "Start \(routine.title.lowercased())", icon: "play_arrow", background: fm.success, minHeight: 52,
                        fillWidth: true,
                        onPressed: {
                            FmHaptics.commit()
                            Task { await model.start() }
                        })
                        .disabled(routine.steps.isEmpty || model.busy)
                        .accessibilityIdentifier("stretch-start")
                    FmSectionHeader(title: "Steps", count: routine.steps.count)
                    stepList(routine)
                }
                noticeLine
                settingsCard
                healthCard
                if !model.recent.isEmpty {
                    FmSectionHeader(title: "Recent sessions")
                    recentList
                }
                Text(StretchModel.disclaimer)
                    .fmText(.bodySmall, color: fm.textTertiary)
                HStack {
                    Spacer(minLength: 0)
                    FmTextButton("Uninstall", color: fm.danger, onPressed: { confirmUninstall = true })
                }
            }
            .padding(FmSpace.x4)
        }
        .fmRefreshable { await model.load() }
    }

    private var statsCard: some View {
        FmCard {
            LazyVGrid(columns: [GridItem(.flexible()), GridItem(.flexible())], alignment: .leading, spacing: FmSpace.x3) {
                ForEach(model.stats) { (stat: StretchStat) in
                    VStack(alignment: .leading, spacing: FmSpace.x1) {
                        Text(stat.label.uppercased())
                            .font(Font.system(size: 11, weight: .semibold))
                            .foregroundStyle(fm.textSecondary)
                        Text(stat.value)
                            .fmText(.bodyMedium)
                    }
                    .accessibilityElement(children: .combine)
                }
            }
        }
        .accessibilityIdentifier("stretch-stats")
    }

    private func routineCard(_ routine: StretchRoutine) -> some View {
        let selected: Bool = model.chosenRoutine?.id == routine.id
        return Button(action: {
            FmHaptics.tap()
            model.selectedRoutine = routine.id
        }) {
            HStack(alignment: .top, spacing: FmSpace.x3) {
                Image(systemName: selected ? "checkmark.circle.fill" : "circle")
                    .font(Font.system(size: 20))
                    .foregroundStyle(selected ? fm.accent : fm.textTertiary)
                VStack(alignment: .leading, spacing: FmSpace.x1) {
                    HStack {
                        Text(routine.title)
                            .fmText(.titleSmall)
                        Spacer(minLength: 0)
                        Text(routine.durationLabel)
                            .fmText(.bodySmall, color: fm.textSecondary)
                    }
                    Text(routine.summary)
                        .fmText(.bodySmall, color: fm.textSecondary)
                        .multilineTextAlignment(.leading)
                }
            }
            .padding(FmSpace.x3)
            .frame(maxWidth: .infinity, alignment: .leading)
            .background(selected ? fm.accentSubtle : fm.bgRaised)
            .clipShape(RoundedRectangle(cornerRadius: FmRadius.lg, style: .continuous))
            .overlay {
                RoundedRectangle(cornerRadius: FmRadius.lg, style: .continuous)
                    .strokeBorder(selected ? fm.accent : fm.border, lineWidth: selected ? 2 : 1)
            }
        }
        .buttonStyle(.plain)
        .accessibilityAddTraits(selected ? .isSelected : [])
        .accessibilityIdentifier("stretch-routine-\(routine.id)")
    }

    private func stepList(_ routine: StretchRoutine) -> some View {
        FmCard {
            VStack(alignment: .leading, spacing: FmSpace.x2) {
                ForEach(Array(routine.steps.enumerated()), id: \.offset) { (item: (offset: Int, element: StretchStep)) in
                    HStack(spacing: FmSpace.x3) {
                        Text(String(item.offset + 1))
                            .fmText(.bodySmall, color: fm.textTertiary)
                            .frame(width: 22, alignment: .trailing)
                        Text(item.element.title)
                            .fmText(.bodyMedium)
                        Spacer(minLength: 0)
                        Text(StretchText.duration(item.element.seconds))
                            .fmText(.bodySmall, color: fm.textSecondary)
                    }
                    .accessibilityElement(children: .combine)
                }
            }
        }
    }

    private var settingsCard: some View {
        FmCard {
            Toggle(isOn: Binding<Bool>(
                get: { model.voice },
                set: { (on: Bool) in Task { await model.setVoice(on) } })) {
                VStack(alignment: .leading, spacing: 2) {
                    Text("Speak the cues")
                        .fmText(.bodyLarge)
                    Text("Uses this iPhone's own voice. Nothing is sent to a speech service.")
                        .fmText(.bodySmall, color: fm.textSecondary)
                }
            }
            .tint(fm.accent)
            .accessibilityIdentifier("stretch-voice-toggle")
        }
    }

    private var healthCard: some View {
        FmCard {
            VStack(alignment: .leading, spacing: FmSpace.x2) {
                Toggle(isOn: Binding<Bool>(
                    get: { model.healthEnabled },
                    set: { (on: Bool) in
                        if on {
                            Task { await model.enableHealth() }
                        } else {
                            model.disableHealth()
                        }
                    })) {
                    Text("Save to Apple Health")
                        .fmText(.bodyLarge)
                }
                .tint(fm.accent)
                .disabled(!model.healthAvailable)
                .accessibilityIdentifier("stretch-health-toggle")
                Text(model.healthLine)
                    .fmText(.bodySmall, color: fm.textSecondary)
                    .accessibilityIdentifier("stretch-health-line")
                AppleHealthDisclosure(tool: .stretch)
            }
        }
    }

    private var recentList: some View {
        VStack(alignment: .leading, spacing: FmSpace.x2) {
            ForEach(model.recent) { (row: StretchRecentRow) in
                HStack(spacing: FmSpace.x3) {
                    Circle()
                        .fill(row.completed ? fm.success : fm.warning)
                        .frame(width: 8, height: 8)
                    VStack(alignment: .leading, spacing: 2) {
                        HStack(spacing: FmSpace.x2) {
                            Text(row.title)
                                .fmText(.bodyLarge)
                            if row.health {
                                Image(systemName: "heart.fill")
                                    .font(Font.system(size: 11))
                                    .foregroundStyle(fm.danger)
                                    .accessibilityLabel("In Apple Health")
                            }
                        }
                        Text(row.detail)
                            .fmText(.bodySmall, color: fm.textSecondary)
                        Text(row.when)
                            .fmText(.bodySmall, color: fm.textTertiary)
                    }
                    .frame(maxWidth: .infinity, alignment: .leading)
                    FmIconButton(icon: "delete_outline", label: "Remove", onPressed: { deleting = row.id })
                }
                .padding(.vertical, FmSpace.x1)
                .accessibilityIdentifier("stretch-session-\(row.id)")
            }
        }
    }
}

private struct StretchSessionView: View {
    @Environment(\.fmPalette) private var fm
    @Bindable var model: StretchModel
    let run: StretchRun
    let library: StretchLibrary

    var body: some View {
        if run.finished {
            finished
        } else {
            active
        }
    }

    private var active: some View {
        VStack(spacing: FmSpace.x3) {
            VStack(alignment: .leading, spacing: FmSpace.x1) {
                HStack {
                    Text(run.routine.title)
                        .fmText(.titleSmall)
                    Spacer(minLength: 0)
                    Text("Step \(min(run.index + 1, run.steps.count)) of \(run.steps.count)")
                        .fmText(.bodySmall, color: fm.textSecondary)
                }
                ProgressView(value: run.progress)
                    .tint(fm.accent)
                    .accessibilityLabel("Overall progress")
                    .accessibilityValue("\(Int((run.progress * 100).rounded())) percent")
            }
            ZStack(alignment: .topTrailing) {
                TimelineView(.animation(paused: run.paused)) { (context: TimelineViewDefaultContext) in
                    StretchFigureCanvas(library: library, step: run.step, seconds: model.figureSeconds(at: context.date))
                }
                .frame(maxWidth: .infinity, maxHeight: .infinity)
                .accessibilityHidden(true)
                Text(StretchText.countdown(run.stepRemaining))
                    .font(Font.system(size: 34, weight: .semibold, design: .rounded).monospacedDigit())
                    .foregroundStyle(run.paused ? fm.textTertiary : fm.textPrimary)
                    .contentTransition(.numericText())
                    .accessibilityLabel("\(run.stepRemaining) seconds left in this step")
                    .accessibilityIdentifier("stretch-countdown")
            }
            .frame(minHeight: 220)
            VStack(alignment: .leading, spacing: FmSpace.x1) {
                Text(run.step?.title ?? "")
                    .fmText(.titleLarge)
                    .accessibilityIdentifier("stretch-step-title")
                Text(run.step?.say ?? "")
                    .fmText(.bodyMedium, color: fm.textSecondary)
                    .fixedSize(horizontal: false, vertical: true)
                if let next = run.nextStep {
                    Text("Next: \(next.title)")
                        .fmText(.bodySmall, color: fm.textTertiary)
                        .padding(.top, FmSpace.x1)
                }
            }
            .frame(maxWidth: .infinity, alignment: .leading)
            controls
        }
        .padding(FmSpace.x4)
    }

    private var controls: some View {
        HStack(spacing: FmSpace.x2) {
            FmIconButton(icon: "skip_previous", label: "Back", onPressed: { model.back() })
                .accessibilityIdentifier("stretch-back")
            Spacer(minLength: 0)
            Button(action: {
                FmHaptics.tap()
                model.togglePause()
            }) {
                FmIcon(run.paused ? "play_arrow" : "pause", size: 34, color: fm.onPrimary)
                    .frame(width: 72, height: 72)
                    .background(fm.accent)
                    .clipShape(Circle())
            }
            .buttonStyle(.plain)
            .accessibilityLabel(run.paused ? "Resume" : "Pause")
            .accessibilityIdentifier("stretch-pause")
            Spacer(minLength: 0)
            FmIconButton(icon: "skip_next", label: "Skip", onPressed: { Task { await model.skip() } })
                .accessibilityIdentifier("stretch-skip")
            FmIconButton(
                icon: model.voice ? "volume_up_outlined" : "volume_off_outlined",
                label: model.voice ? "Turn the voice off" : "Turn the voice on",
                onPressed: { Task { await model.setVoice(!model.voice) } })
                .accessibilityIdentifier("stretch-voice")
            FmIconButton(icon: "stop_circle_outlined", label: "Stop", color: fm.danger, onPressed: {
                FmHaptics.commit()
                Task { await model.stop() }
            })
            .accessibilityIdentifier("stretch-stop")
        }
    }

    private var finished: some View {
        VStack(spacing: FmSpace.x3) {
            StretchFigureCanvas(library: library, step: nil, seconds: 0)
                .frame(width: 140, height: 185)
                .accessibilityHidden(true)
            Text(run.completed ? "Well done" : "Session stopped")
                .fmText(.titleLarge)
            Text("\(run.routine.title) · \(StretchText.duration(run.loggedSeconds)) · \(run.stepsDone) of \(run.steps.count) steps")
                .fmText(.bodyMedium, color: fm.textSecondary)
                .multilineTextAlignment(.center)
            if model.busy {
                FmCircularProgressIndicator()
            } else if !model.logNote.isEmpty {
                Text(model.logNote)
                    .fmText(.bodyMedium)
                    .multilineTextAlignment(.center)
                    .accessibilityIdentifier("stretch-log-note")
            }
            FmNoticeLine(model.notice?.message, isError: model.notice?.isError ?? false, onDismiss: { model.notice = nil })
            FmFilledButton("Done", minHeight: 48, fillWidth: true, onPressed: {
                model.close()
                Task { await model.load() }
            })
            .disabled(model.busy)
            .accessibilityIdentifier("stretch-done")
        }
        .padding(FmSpace.x5)
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }
}

struct StretchFigureCanvas: View {
    @Environment(\.fmPalette) private var fm
    let library: StretchLibrary
    let step: StretchStep?
    let seconds: Double

    var body: some View {
        Canvas { (context: inout GraphicsContext, size: CGSize) in
            let figure: StretchFigure = library.figure
            guard figure.width > 0, figure.height > 0 else { return }
            let scale: Double = min(size.width / figure.width, size.height / figure.height)
            let dx: Double = (size.width - figure.width * scale) / 2
            let dy: Double = (size.height - figure.height * scale) / 2
            func point(_ p: StretchPoint) -> CGPoint { CGPoint(x: dx + p.x * scale, y: dy + p.y * scale) }
            let shape: StretchBody = StretchMath.body(library, step: step, seconds: seconds)
            let ink: Color = fm.accent
            for segment in shape.segments {
                var path: Path = Path()
                path.move(to: point(segment.from))
                path.addLine(to: point(segment.to))
                context.stroke(path, with: .color(ink), style: StrokeStyle(lineWidth: segment.width * scale, lineCap: .round))
            }
            let head: CGPoint = point(shape.head)
            let radius: Double = shape.headRadius * scale
            context.fill(
                Path(ellipseIn: CGRect(x: head.x - radius, y: head.y - radius, width: radius * 2, height: radius * 2)),
                with: .color(ink))
            let eye: Double = shape.eyeRadius * scale
            for spot in shape.eyes {
                let centre: CGPoint = point(spot)
                context.fill(
                    Path(ellipseIn: CGRect(x: centre.x - eye, y: centre.y - eye, width: eye * 2, height: eye * 2)),
                    with: .color(fm.bgBase))
            }
        }
    }
}
