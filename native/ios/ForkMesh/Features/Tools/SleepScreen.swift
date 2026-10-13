import FMApi
import FMBase
import FMTools
import SwiftUI

struct SleepScreen: View {
    @Environment(\.fmPalette) private var fm
    @State private var model: SleepModel?

    init() {
        if let api = Services.get(ApiService.self) {
            _model = State(initialValue: SleepModel(
                api: SleepAPI(api: api), bridge: HealthFeature.bridge, prefs: HealthFeature.prefs))
        } else {
            _model = State(initialValue: nil)
        }
    }

    var body: some View {
        content
            .navigationTitle(SleepModel.title)
            .navigationBarTitleDisplayMode(.inline)
            .background(fm.bgBase)
    }

    @ViewBuilder private var content: some View {
        if let model = model {
            SleepContent(model: model)
                .task { await model.load() }
        } else {
            FmEmptyState(icon: "dark_mode_outlined", title: SleepModel.unavailableTitle)
        }
    }
}

private struct SleepContent: View {
    @Environment(\.fmPalette) private var fm
    @Environment(\.scenePhase) private var scenePhase
    @Bindable var model: SleepModel
    @State private var adding: Bool = false
    @State private var addNight: Date = Date()
    @State private var addBed: Date = SleepClock.date("23:00")
    @State private var addWake: Date = SleepClock.date("07:00")
    @State private var addAwake: String = ""
    @State private var editingGoal: Bool = false
    @State private var goalText: String = ""
    @State private var goalBed: Date = SleepClock.date("23:00")
    @State private var goalWake: Date = SleepClock.date("07:00")
    @State private var confirmUninstall: Bool = false

    var body: some View {
        main
            .toolbar {
                ToolbarItem(placement: .primaryAction) {
                    FmIconButton(icon: "refresh", label: "Refresh", onPressed: { Task { await model.load() } })
                }
            }
            .onChange(of: scenePhase) { (_: ScenePhase, phase: ScenePhase) in
                if phase == .active {
                    Task { await model.load() }
                }
            }
            .onChange(of: model.trackingEnabled) { (_: Bool, _: Bool) in
                HealthFeature.sleepEnabledChanged()
            }
            .fmDialog(isPresented: $adding) { addDialog }
            .fmDialog(isPresented: $editingGoal) { goalDialog }
            .fmDialog(isPresented: $confirmUninstall) {
                FmConfirmDialog(
                    title: "Uninstall Sleep?",
                    message: "Your nights are deleted from the web dashboard, the desktop app, and this app. Apple Health keeps its own copy of your sleep.",
                    confirmLabel: "Uninstall", destructive: true,
                    onCancel: { confirmUninstall = false },
                    onConfirm: {
                        confirmUninstall = false
                        Task { if await model.uninstall() { await PackagesFeature.removeFromAccount("sleep") } }
                    })
            }
    }

    @ViewBuilder private var main: some View {
        if model.showsSpinner {
            FmCircularProgressIndicator()
                .frame(maxWidth: .infinity, maxHeight: .infinity)
        } else if model.showsError {
            FmEmptyState(icon: "dark_mode_outlined", title: SleepModel.unavailableTitle, message: model.error)
        } else if model.showsInstall {
            installPanel
        } else if model.overview != nil {
            dashboard
        }
    }

    private var installPanel: some View {
        VStack(spacing: FmSpace.x3) {
            Text(SleepModel.installTitle)
                .fmText(.titleMedium)
                .multilineTextAlignment(.center)
            Text(SleepModel.installMessage)
                .fmText(.bodyMedium, color: fm.textSecondary)
                .multilineTextAlignment(.center)
            FmFilledButton(SleepModel.installButton, onPressed: { Task { await model.install() } })
                .padding(.top, FmSpace.x1)
                .accessibilityIdentifier("sleep-install")
            noticeLine
        }
        .padding(FmSpace.x5)
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }

    private var dashboard: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: FmSpace.x3) {
                lastNightCard
                trackingCard
                noticeLine
                FmCard {
                    LazyVGrid(columns: [GridItem(.flexible()), GridItem(.flexible())], alignment: .leading, spacing: FmSpace.x3) {
                        ForEach(model.stats) { (stat: SleepStat) in
                            VStack(alignment: .leading, spacing: FmSpace.x1) {
                                Text(stat.label.uppercased())
                                    .font(Font.system(size: 11, weight: .semibold))
                                    .foregroundStyle(fm.textSecondary)
                                Text(stat.value)
                                    .fmText(.bodyMedium)
                            }
                        }
                    }
                }
                FmSectionHeader(title: "Last 30 nights")
                SleepBarsView(bars: model.bars, goalRatio: model.goalRatio, accent: fm.accent, met: fm.success,
                              goalColor: fm.warning, ink: fm.textSecondary)
                    .frame(height: 160)
                    .accessibilityIdentifier("sleep-chart")
                HStack(spacing: FmSpace.x2) {
                    FmOutlinedButton("Add night", icon: "add", onPressed: {
                        addNight = Date()
                        addBed = SleepClock.date(model.bedtime)
                        addWake = SleepClock.date(model.wakeTime)
                        addAwake = ""
                        adding = true
                    })
                    FmOutlinedButton("Goal", icon: "flag_outlined", onPressed: {
                        goalText = DartCore.toStringAsFixed(Double(model.goal) / 60, 2)
                        goalBed = SleepClock.date(model.bedtime)
                        goalWake = SleepClock.date(model.wakeTime)
                        editingGoal = true
                    })
                    Spacer(minLength: 0)
                    FmTextButton("Uninstall", color: fm.danger, onPressed: { confirmUninstall = true })
                }
            }
            .padding(FmSpace.x4)
        }
        .fmRefreshable { await model.syncNow() }
    }

    private var noticeLine: some View {
        FmNoticeLine(model.notice?.message, isError: model.notice?.isError ?? false, onDismiss: { model.notice = nil })
    }

    private var lastNightCard: some View {
        FmCard {
            HStack(spacing: FmSpace.x4) {
                ZStack {
                    Circle()
                        .stroke(fm.border, lineWidth: 10)
                    Circle()
                        .trim(from: 0, to: model.ringRatio)
                        .stroke(model.goalMet ? fm.success : fm.accent, style: StrokeStyle(lineWidth: 10, lineCap: .round))
                        .rotationEffect(Angle.degrees(-90))
                    VStack(spacing: 0) {
                        Text(model.durationText)
                            .font(Font.system(size: 20, weight: .semibold))
                            .foregroundStyle(fm.textPrimary)
                            .minimumScaleFactor(0.6)
                            .lineLimit(1)
                        Text(model.goalLine)
                            .font(Font.system(size: 11))
                            .foregroundStyle(fm.textSecondary)
                    }
                    .padding(.horizontal, FmSpace.x3)
                }
                .frame(width: 128, height: 128)
                .accessibilityElement(children: .combine)
                .accessibilityIdentifier("sleep-ring")
                VStack(alignment: .leading, spacing: FmSpace.x2) {
                    Text("Last night")
                        .fmText(.titleMedium)
                    Text(model.detailLine)
                        .fmText(.bodyMedium, color: fm.textSecondary)
                }
                .frame(maxWidth: .infinity, alignment: .leading)
            }
        }
    }

    private var trackingCard: some View {
        FmCard {
            VStack(alignment: .leading, spacing: FmSpace.x2) {
                Toggle(isOn: Binding<Bool>(
                    get: { model.trackingEnabled },
                    set: { (on: Bool) in
                        if on {
                            Task { await model.enableTracking() }
                        } else {
                            model.disableTracking()
                        }
                    })) {
                    Text("Track sleep with my phone")
                        .fmText(.bodyLarge)
                }
                .tint(fm.accent)
                .disabled(!model.trackingAvailable)
                .accessibilityIdentifier("sleep-tracking-toggle")
                Text(model.sourceLine)
                    .fmText(.bodySmall, color: fm.textSecondary)
                if model.trackingEnabled && model.healthAvailable {
                    Toggle(isOn: Binding<Bool>(
                        get: { model.writesHealth },
                        set: { (on: Bool) in Task { await model.setWritesHealth(on) } })) {
                        Text("Save detected sleep to Apple Health")
                            .fmText(.bodyMedium)
                    }
                    .tint(fm.accent)
                    .accessibilityIdentifier("sleep-health-write-toggle")
                }
                if model.trackingEnabled {
                    FmTextButton(model.syncing ? "Syncing…" : "Sync now", onPressed: {
                        Task { await model.syncNow() }
                    })
                    .disabled(model.syncing)
                }
                if model.healthAvailable {
                    AppleHealthDisclosure(tool: .sleep)
                }
            }
        }
    }

    private var addDialog: some View {
        FmDialog(
            title: "Add a night",
            message: "For a night your phone was not by your bed. It replaces what the phone or Apple Health recorded for that night.",
            content: {
                VStack(alignment: .leading, spacing: FmSpace.x2) {
                    DatePicker("Woke up on", selection: $addNight, in: ...Date(), displayedComponents: .date)
                    DatePicker("Went to bed", selection: $addBed, displayedComponents: .hourAndMinute)
                    DatePicker("Woke up", selection: $addWake, displayedComponents: .hourAndMinute)
                    FmTextField(text: $addAwake, hint: "Minutes awake in bed (optional)")
                        .keyboardType(.numberPad)
                }
            },
            actions: {
                HStack(spacing: FmSpace.x2) {
                    FmTextButton("Remove added", onPressed: {
                        adding = false
                        let night: String = CaloriesText.isoDay(addNight)
                        Task { await model.removeAddedNight(night) }
                    })
                    Spacer(minLength: 0)
                    FmTextButton("Cancel", color: fm.textPrimary, onPressed: { adding = false })
                    FmFilledButton("Save", onPressed: {
                        adding = false
                        let night: String = CaloriesText.isoDay(addNight)
                        let awake: Int = CaloriesText.int(addAwake)
                        guard let span = model.span(night: night, bed: addBed, wake: addWake) else { return }
                        Task { await model.addNight(night: night, bedAt: span.bedAt, wakeAt: span.wakeAt, awakeMinutes: awake) }
                    })
                }
            })
    }

    private var goalDialog: some View {
        FmDialog(
            title: "Sleep goal and schedule",
            message: "Your phone looks for its longest still stretch around this schedule.",
            content: {
                VStack(alignment: .leading, spacing: FmSpace.x2) {
                    FmTextField(text: $goalText, hint: "Hours of sleep, e.g. 8", autofocus: true)
                        .keyboardType(.decimalPad)
                    DatePicker("Bedtime", selection: $goalBed, displayedComponents: .hourAndMinute)
                    DatePicker("Wake time", selection: $goalWake, displayedComponents: .hourAndMinute)
                }
            },
            actions: {
                HStack(spacing: FmSpace.x2) {
                    Spacer(minLength: 0)
                    FmTextButton("Cancel", color: fm.textPrimary, onPressed: { editingGoal = false })
                    FmFilledButton("Save", onPressed: {
                        editingGoal = false
                        let hours: Double = CaloriesText.num(goalText.replacingOccurrences(of: ",", with: "."))
                        let minutes: Int = Int((hours * 60).rounded())
                        let bedtime: String = SleepClock.text(goalBed)
                        let wakeTime: String = SleepClock.text(goalWake)
                        Task { await model.saveGoal(minutes: minutes, bedtime: bedtime, wakeTime: wakeTime) }
                    })
                }
            })
    }
}

enum SleepClock {
    static func date(_ clock: String) -> Date {
        let minutes: Int = SleepSchedule.minutes(clock) ?? SleepSchedule.defaultBedtime
        let midnight: Date = Calendar.current.startOfDay(for: Date())
        return Calendar.current.date(byAdding: .minute, value: minutes, to: midnight) ?? midnight
    }

    static func text(_ date: Date) -> String {
        let parts: DateComponents = Calendar.current.dateComponents([.hour, .minute], from: date)
        return CaloriesText.clock(hour: parts.hour ?? 0, minute: parts.minute ?? 0)
    }
}

struct SleepBarsView: View {
    let bars: [SleepBar]
    let goalRatio: Double
    let accent: Color
    let met: Color
    let goalColor: Color
    let ink: Color

    var body: some View {
        Canvas { (context: inout GraphicsContext, size: CGSize) in
            guard !bars.isEmpty else { return }
            let plotHeight: CGFloat = size.height - 16
            let slot: CGFloat = size.width / CGFloat(bars.count)
            let width: CGFloat = max(2, min(12, slot - 3))
            for (index, bar) in bars.enumerated() {
                let height: CGFloat = bar.minutes > 0 ? max(2, plotHeight * CGFloat(min(1, bar.ratio))) : 0
                let x: CGFloat = CGFloat(index) * slot + (slot - width) / 2
                let rect: CGRect = CGRect(x: x, y: plotHeight - height, width: width, height: height)
                context.fill(Path(roundedRect: rect, cornerRadius: 2), with: .color(bar.metGoal ? met : accent))
                if !bar.label.isEmpty {
                    let text: GraphicsContext.ResolvedText = context.resolve(
                        Text(bar.label).font(Font.system(size: 9)).foregroundColor(ink))
                    context.draw(text, at: CGPoint(x: x + width / 2, y: size.height), anchor: .bottom)
                }
            }
            if goalRatio > 0 {
                let y: CGFloat = plotHeight - plotHeight * CGFloat(min(1, goalRatio))
                var line: Path = Path()
                line.move(to: CGPoint(x: 0, y: y))
                line.addLine(to: CGPoint(x: size.width, y: y))
                context.stroke(line, with: .color(goalColor), style: StrokeStyle(lineWidth: 1.5, dash: [5, 4]))
            }
        }
        .accessibilityElement()
        .accessibilityLabel(bars.suffix(7).map(\.caption).joined(separator: ", "))
    }
}
