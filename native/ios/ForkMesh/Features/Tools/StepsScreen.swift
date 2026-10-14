import FMApi
import FMBase
import FMTools
import SwiftUI

struct StepsScreen: View {
    @Environment(\.fmPalette) private var fm
    @State private var model: StepsModel?
    @State private var heart: HeartTapModel

    init() {
        _heart = State(initialValue: HeartTapModel(bridge: HealthFeature.bridge))
        if let api = Services.get(ApiService.self) {
            _model = State(initialValue: StepsModel(
                api: StepsAPI(api: api), health: HealthFeature.bridge, prefs: HealthFeature.prefs))
        } else {
            _model = State(initialValue: nil)
        }
    }

    var body: some View {
        content
            .navigationTitle(StepsModel.title)
            .navigationBarTitleDisplayMode(.inline)
            .background(fm.canvas)
    }

    @ViewBuilder private var content: some View {
        if let model = model {
            StepsContent(model: model, heart: heart)
                .task { await model.load() }
        } else {
            FmEmptyState(icon: "directions_walk_outlined", title: StepsModel.unavailableTitle)
        }
    }
}

private struct StepsContent: View {
    @Environment(\.fmPalette) private var fm
    @Environment(\.scenePhase) private var scenePhase
    @Bindable var model: StepsModel
    let heart: HeartTapModel
    @State private var adding: Bool = false
    @State private var addDay: Date = Date()
    @State private var addSteps: String = ""
    @State private var addDistance: String = ""
    @State private var editingGoal: Bool = false
    @State private var goalText: String = ""
    @State private var goalUnits: String = "auto"
    @State private var confirmUninstall: Bool = false

    var body: some View {
        main
            .toolbar {
                ToolbarItem(placement: .primaryAction) {
                    FmIconButton(icon: "refresh", label: "Refresh", onPressed: { Task { await model.load() } })
                }
            }
            .onAppear { model.startLive() }
            .onDisappear { Task { await model.leave() } }
            .onChange(of: scenePhase) { (_: ScenePhase, phase: ScenePhase) in
                if phase == .active {
                    model.startLive()
                    Task { await model.load() }
                } else if phase == .background {
                    model.stopLive()
                }
            }
            .onChange(of: model.healthEnabled) { (_: Bool, enabled: Bool) in
                HealthFeature.stepsEnabledChanged()
                if enabled { model.startLive() }
            }
            .fmDialog(isPresented: $adding) { addDialog }
            .fmDialog(isPresented: $editingGoal) { goalDialog }
            .fmDialog(isPresented: $confirmUninstall) {
                FmConfirmDialog(
                    title: "Uninstall Pedometer?",
                    message: "Your step history is deleted from the web dashboard, the desktop app, and this app. Apple Health keeps its own copy of your steps.",
                    confirmLabel: "Uninstall", destructive: true,
                    onCancel: { confirmUninstall = false },
                    onConfirm: {
                        confirmUninstall = false
                        Task { if await model.uninstall() { await PackagesFeature.removeFromAccount("steps") } }
                    })
            }
    }

    @ViewBuilder private var main: some View {
        if model.showsSpinner {
            FmCircularProgressIndicator()
                .frame(maxWidth: .infinity, maxHeight: .infinity)
        } else if model.showsError {
            FmEmptyState(icon: "directions_walk_outlined", title: StepsModel.unavailableTitle, message: model.error)
        } else if model.showsInstall {
            installPanel
        } else if model.overview != nil {
            dashboard
        }
    }

    private var installPanel: some View {
        VStack(spacing: FmSpace.x3) {
            Text(StepsModel.installTitle)
                .fmText(.titleMedium)
                .multilineTextAlignment(.center)
            Text(StepsModel.installMessage)
                .fmText(.bodyMedium, color: fm.textSecondary)
                .multilineTextAlignment(.center)
            FmFilledButton(StepsModel.installButton, onPressed: { Task { await model.install() } })
                .padding(.top, FmSpace.x1)
                .accessibilityIdentifier("steps-install")
            noticeLine
        }
        .padding(FmSpace.x5)
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }

    private var dashboard: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: FmSpace.x3) {
                todayCard
                walkCard
                healthCard
                noticeLine
                heartCard
                FmCard {
                    LazyVGrid(columns: [GridItem(.flexible()), GridItem(.flexible())], alignment: .leading, spacing: FmSpace.x3) {
                        ForEach(model.stats) { (stat: StepsStat) in
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
                FmSectionHeader(title: "Last 30 days")
                StepsBarsView(bars: model.bars, goalRatio: model.goalRatio, accent: fm.accent, met: fm.success,
                              goalColor: fm.warning, ink: fm.textSecondary)
                    .frame(height: 160)
                    .accessibilityIdentifier("steps-chart")
                HStack(spacing: FmSpace.x2) {
                    FmOutlinedButton("Add steps", icon: "add", onPressed: {
                        addDay = Date()
                        addSteps = ""
                        addDistance = ""
                        adding = true
                    })
                    FmOutlinedButton("Goal", icon: "flag_outlined", onPressed: {
                        goalText = String(model.goal)
                        goalUnits = model.unitsPreference
                        editingGoal = true
                    })
                    Spacer(minLength: 0)
                    FmTextButton("Uninstall", color: fm.danger, onPressed: { confirmUninstall = true })
                }
            }
            .padding(FmSpace.x4)
        }
        .fmRefreshable { await model.syncNow(days: 7) }
    }

    private var todayCard: some View {
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
                        Text(model.stepsText)
                            .font(Font.system(size: 24, weight: .semibold))
                            .foregroundStyle(fm.textPrimary)
                            .contentTransition(.numericText())
                        Text(model.goalLine)
                            .font(Font.system(size: 11))
                            .foregroundStyle(fm.textSecondary)
                    }
                }
                .frame(width: 128, height: 128)
                .accessibilityElement(children: .combine)
                .accessibilityIdentifier("steps-ring")
                VStack(alignment: .leading, spacing: FmSpace.x2) {
                    Text("Today")
                        .fmText(.titleMedium)
                    Text(model.detailLine)
                        .fmText(.bodyMedium, color: fm.textSecondary)
                }
                .frame(maxWidth: .infinity, alignment: .leading)
            }
        }
    }

    private var walkCard: some View {
        FmCard {
            VStack(alignment: .leading, spacing: FmSpace.x2) {
                StepsWalkScene(cadence: model.cadence, running: model.running, figure: model.goalMet ? fm.success : fm.accent,
                               scenery: fm.textTertiary, ground: fm.border)
                    .frame(height: 96)
                Text(model.paceLine)
                    .fmText(.bodyMedium, color: fm.textSecondary)
                    .contentTransition(.numericText())
            }
        }
        .accessibilityElement(children: .ignore)
        .accessibilityLabel(model.paceLine)
        .accessibilityIdentifier("steps-walk")
    }

    private var noticeLine: some View {
        FmNoticeLine(model.notice?.message, isError: model.notice?.isError ?? false, onDismiss: { model.notice = nil })
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
                    Text("Count steps with Apple Health")
                        .fmText(.bodyLarge)
                }
                .tint(fm.accent)
                .disabled(!model.healthAvailable)
                .accessibilityIdentifier("steps-health-toggle")
                Text(model.sourceLine)
                    .fmText(.bodySmall, color: fm.textSecondary)
                if model.healthEnabled {
                    FmTextButton(model.syncing ? "Syncing…" : "Sync now", onPressed: {
                        Task { await model.syncNow(days: 30) }
                    })
                    .disabled(model.syncing)
                }
                AppleHealthDisclosure(tool: .steps)
            }
        }
    }

    private var heartCard: some View {
        FmCard {
            VStack(alignment: .leading, spacing: FmSpace.x3) {
                HStack(spacing: FmSpace.x4) {
                    Button(action: {
                        FmHaptics.tap()
                        heart.tap()
                    }) {
                        Image(systemName: "heart.fill")
                            .font(Font.system(size: 34, weight: .semibold))
                            .foregroundStyle(Color.white)
                            .frame(width: 88, height: 88)
                            .background(Circle().fill(fm.danger))
                            .contentShape(Circle())
                    }
                    .buttonStyle(.plain)
                    .disabled(heart.saving)
                    .accessibilityLabel(HeartTapModel.tapLabel)
                    .accessibilityValue(heart.bpm.map { "\($0) BPM" } ?? "")
                    .accessibilityIdentifier("steps-heart-tap")
                    VStack(alignment: .leading, spacing: FmSpace.x2) {
                        Text(HeartTapModel.title)
                            .fmText(.titleMedium)
                        Text(heart.line)
                            .fmText(.bodyMedium, color: fm.textSecondary)
                            .contentTransition(.numericText())
                            .fixedSize(horizontal: false, vertical: true)
                    }
                    .frame(maxWidth: .infinity, alignment: .leading)
                }
                HStack(spacing: FmSpace.x2) {
                    FmFilledButton(
                        heart.saving ? "Saving…" : HeartTapModel.saveLabel, icon: "favorite_outline",
                        onPressed: heart.canSave ? { Task { await heart.save() } } : nil)
                    .accessibilityIdentifier("steps-heart-save")
                    Spacer(minLength: 0)
                    if heart.canReset {
                        FmTextButton("Reset", onPressed: { heart.reset() })
                    }
                }
                FmNoticeLine(heart.notice?.message, isError: heart.notice?.isError ?? false, onDismiss: { heart.notice = nil })
            }
        }
    }

    private var addDialog: some View {
        FmDialog(
            title: "Add steps",
            message: "For a walk your phone missed. These count on top of Apple Health's total for the day.",
            content: {
                VStack(alignment: .leading, spacing: FmSpace.x2) {
                    DatePicker("Day", selection: $addDay, in: ...Date(), displayedComponents: .date)
                    FmTextField(text: $addSteps, hint: "Steps", autofocus: true)
                        .keyboardType(.numberPad)
                    FmTextField(text: $addDistance, hint: "Distance (\(model.distanceUnit), optional)")
                        .keyboardType(.decimalPad)
                }
            },
            actions: {
                HStack(spacing: FmSpace.x2) {
                    FmTextButton("Remove added", onPressed: {
                        adding = false
                        let day: String = CaloriesText.isoDay(addDay)
                        Task { await model.removeAddedSteps(day: day) }
                    })
                    Spacer(minLength: 0)
                    FmTextButton("Cancel", color: fm.textPrimary, onPressed: { adding = false })
                    FmFilledButton("Save", onPressed: {
                        adding = false
                        let day: String = CaloriesText.isoDay(addDay)
                        let steps: Int = CaloriesText.int(addSteps)
                        let distance: Double = CaloriesText.num(addDistance.replacingOccurrences(of: ",", with: "."))
                        Task { await model.addSteps(day: day, steps: steps, distance: distance) }
                    })
                }
            })
    }

    private var goalDialog: some View {
        FmDialog(
            title: "Daily step goal",
            content: {
                VStack(alignment: .leading, spacing: FmSpace.x2) {
                    FmTextField(text: $goalText, hint: "10000", autofocus: true)
                        .keyboardType(.numberPad)
                    Picker("Distance in", selection: $goalUnits) {
                        Text("Automatic").tag("auto")
                        Text("Kilometres").tag("metric")
                        Text("Miles").tag("imperial")
                    }
                    .pickerStyle(.segmented)
                }
            },
            actions: {
                HStack(spacing: FmSpace.x2) {
                    Spacer(minLength: 0)
                    FmTextButton("Cancel", color: fm.textPrimary, onPressed: { editingGoal = false })
                    FmFilledButton("Save", onPressed: {
                        editingGoal = false
                        let goal: Int = CaloriesText.int(goalText)
                        let units: String = goalUnits
                        Task { await model.saveGoal(goal, units: units) }
                    })
                }
            })
    }
}

struct StepsBarsView: View {
    let bars: [StepsBar]
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
                let height: CGFloat = bar.steps > 0 ? max(2, plotHeight * CGFloat(min(1, bar.ratio))) : 0
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

final class StepsWalkClock {
    var phase: Double = 0
    var scroll: Double = 0
    var speed: Double = 0
    var run: Double = 0
    var last: Date?

    func advance(to date: Date, cadence: Double, running: Bool, legLength: Double) {
        let dt: Double = last.map { min(0.1, max(0, date.timeIntervalSince($0))) } ?? 0
        last = date
        speed += (cadence - speed) * min(1, dt * 3)
        if speed < 0.08 && cadence == 0 { speed = 0 }
        let target: Double = running ? 1 : StepsWalkPose.run(speed)
        run += (target - run) * min(1, dt * 3)
        if speed == 0 { run = 0 }
        phase = (phase + dt * speed * Double.pi).truncatingRemainder(dividingBy: 2 * Double.pi)
        scroll += dt * 2 * legLength * StepsWalkPose.swing(speed, run: run) * speed
    }
}

struct StepsWalkPose {
    let hip: CGPoint
    let shoulder: CGPoint
    let head: CGPoint
    let legs: [(knee: CGPoint, foot: CGPoint)]
    let arms: [(elbow: CGPoint, hand: CGPoint)]

    static let thigh: Double = 19
    static let shin: Double = 19
    static let torso: Double = 25
    static let upperArm: Double = 13
    static let forearm: Double = 12
    static let headRadius: Double = 7.5

    static func stride(_ speed: Double) -> Double { min(1, speed / 1.4) }
    static func run(_ speed: Double) -> Double { min(1, max(0, (speed - 2.1) / 0.5)) }
    static func swing(_ speed: Double, run: Double) -> Double { stride(speed) * (0.42 + 0.18 * run) }

    init(x: Double, groundY: Double, phase: Double, speed: Double, run: Double) {
        let stride: Double = Self.stride(speed)
        let swing: Double = Self.swing(speed, run: run)
        let knee: Double = stride * (0.55 + 0.75 * run)
        let armSwing: Double = stride * (0.35 + 0.45 * run)
        let elbow: Double = 0.25 + stride * (0.2 + 1.0 * run)
        let lean: Double = 0.04 * stride + 0.16 * run
        let legLength: Double = Self.thigh + Self.shin
        let flight: Double = run * 3 * abs(cos(phase))
        let hipY: Double = groundY - legLength * cos(swing * sin(phase)) - flight
        let hip: CGPoint = CGPoint(x: x, y: hipY)
        func along(_ from: CGPoint, _ angle: Double, _ length: Double) -> CGPoint {
            CGPoint(x: from.x + length * sin(angle), y: from.y + length * cos(angle))
        }
        let shoulder: CGPoint = CGPoint(x: x + Self.torso * sin(lean), y: hipY - Self.torso * cos(lean))
        self.hip = hip
        self.shoulder = shoulder
        head = CGPoint(
            x: shoulder.x + (Self.headRadius + 3) * sin(lean), y: shoulder.y - (Self.headRadius + 3) * cos(lean))
        legs = [1.0, -1.0].map { (side: Double) -> (knee: CGPoint, foot: CGPoint) in
            let angle: Double = side * swing * sin(phase)
            let bend: Double = knee * max(0, side * cos(phase))
            let kneePoint: CGPoint = along(hip, angle, Self.thigh)
            return (kneePoint, along(kneePoint, angle - bend, Self.shin))
        }
        arms = [1.0, -1.0].map { (side: Double) -> (elbow: CGPoint, hand: CGPoint) in
            let angle: Double = -side * armSwing * sin(phase)
            let elbowPoint: CGPoint = along(shoulder, angle, Self.upperArm)
            return (elbowPoint, along(elbowPoint, angle + elbow, Self.forearm))
        }
    }
}

struct StepsWalkScene: View {
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    let cadence: Double
    let running: Bool
    let figure: Color
    let scenery: Color
    let ground: Color
    var figureX: Double = 0.38
    @State private var clock: StepsWalkClock = StepsWalkClock()
    @State private var settling: Bool = true

    var body: some View {
        TimelineView(.animation(minimumInterval: 1.0 / 60, paused: reduceMotion || (cadence == 0 && !settling))) {
            (timeline: TimelineViewDefaultContext) in
            Canvas { (context: inout GraphicsContext, size: CGSize) in
                clock.advance(to: timeline.date, cadence: reduceMotion ? 0 : cadence, running: running,
                              legLength: StepsWalkPose.thigh + StepsWalkPose.shin)
                draw(&context, size: size)
            }
        }
        .task(id: cadence == 0) {
            guard cadence == 0 else { return }
            settling = true
            try? await Task.sleep(nanoseconds: 1_500_000_000)
            if !Task.isCancelled { settling = false }
        }
        .accessibilityHidden(true)
    }

    private func draw(_ context: inout GraphicsContext, size: CGSize) {
        let groundY: Double = size.height - 12
        let scroll: Double = clock.scroll
        var hills: Path = Path()
        hills.move(to: CGPoint(x: 0, y: groundY))
        for step in 0...Int(size.width / 4) {
            let x: Double = Double(step) * 4
            let world: Double = x + scroll * 0.15
            let y: Double = groundY - 16 - 9 * sin(world / 53) - 6 * sin(world / 23 + 1.3)
            hills.addLine(to: CGPoint(x: x, y: y))
        }
        hills.addLine(to: CGPoint(x: size.width, y: groundY))
        hills.closeSubpath()
        context.fill(hills, with: .color(ground.opacity(0.45)))
        let spacing: Double = 74
        let treeShift: Double = scroll * 0.55
        let firstTree: Int = Int(floor((treeShift - 20) / spacing))
        for index in firstTree...(firstTree + Int(size.width / spacing) + 2) {
            let variant: Int = ((index % 7) + 7) % 7
            guard variant != 3 else { continue }
            let x: Double = Double(index) * spacing - treeShift + Double((variant * 17) % 29)
            let height: Double = 20 + Double((variant * 11) % 13)
            var trunk: Path = Path()
            trunk.move(to: CGPoint(x: x, y: groundY))
            trunk.addLine(to: CGPoint(x: x, y: groundY - height))
            context.stroke(trunk, with: .color(scenery), style: StrokeStyle(lineWidth: 2.5, lineCap: .round))
            let crown: Double = 7 + Double(variant % 3) * 2
            context.fill(
                Path(ellipseIn: CGRect(x: x - crown, y: groundY - height - crown * 1.4, width: crown * 2, height: crown * 2.2)),
                with: .color(scenery))
        }
        var line: Path = Path()
        line.move(to: CGPoint(x: 0, y: groundY))
        line.addLine(to: CGPoint(x: size.width, y: groundY))
        context.stroke(line, with: .color(ground), style: StrokeStyle(lineWidth: 2))
        let dash: Double = 22
        var dashes: Path = Path()
        var x: Double = -scroll.truncatingRemainder(dividingBy: dash)
        while x < size.width {
            dashes.move(to: CGPoint(x: x, y: groundY + 6))
            dashes.addLine(to: CGPoint(x: x + 8, y: groundY + 6))
            x += dash
        }
        context.stroke(dashes, with: .color(ground), style: StrokeStyle(lineWidth: 2, lineCap: .round))
        let pose: StepsWalkPose = StepsWalkPose(
            x: size.width * figureX, groundY: groundY - 1, phase: clock.phase, speed: clock.speed, run: clock.run)
        let limb: StrokeStyle = StrokeStyle(lineWidth: 5, lineCap: .round, lineJoin: .round)
        func stroke(_ points: [CGPoint], _ color: Color) {
            var path: Path = Path()
            path.addLines(points)
            context.stroke(path, with: .color(color), style: limb)
        }
        let far: Color = figure.opacity(0.5)
        stroke([pose.hip, pose.legs[1].knee, pose.legs[1].foot], far)
        stroke([pose.shoulder, pose.arms[1].elbow, pose.arms[1].hand], far)
        stroke([pose.hip, pose.legs[0].knee, pose.legs[0].foot], figure)
        stroke([pose.hip, pose.shoulder], figure)
        let radius: Double = StepsWalkPose.headRadius
        context.fill(
            Path(ellipseIn: CGRect(x: pose.head.x - radius, y: pose.head.y - radius, width: radius * 2, height: radius * 2)),
            with: .color(figure))
        stroke([pose.shoulder, pose.arms[0].elbow, pose.arms[0].hand], figure)
    }
}

struct StepsWalkBadge: View {
    @Environment(\.fmPalette) private var fm
    let cadence: Double
    let running: Bool
    let size: CGFloat
    var figure: Color? = nil

    private static let canvas: CGFloat = 104

    var body: some View {
        StepsWalkScene(cadence: cadence, running: running, figure: figure ?? fm.accent,
                       scenery: fm.textTertiary, ground: fm.border, figureX: 0.5)
            .frame(width: Self.canvas, height: Self.canvas)
            .scaleEffect(size / Self.canvas)
            .frame(width: size, height: size)
            .background(Circle().fill(fm.accent.opacity(0.12)))
            .clipShape(Circle())
    }
}

struct StepsWalkChip: View {
    private let walk: WalkActivityController = WalkActivityController.shared

    var body: some View {
        if walk.stepping, PackageGateSync.isInstalled("steps") {
            Button { SurfaceRouter.open(.steps) } label: {
                StepsWalkBadge(cadence: walk.cadence, running: walk.running, size: 34)
                    .padding(5)
                    .shellGlass(Circle())
                    .contentShape(Circle())
            }
            .buttonStyle(ShellBarButtonStyle())
            .transition(.scale.combined(with: .opacity))
            .accessibilityElement(children: .ignore)
            .accessibilityLabel(Text("\(walk.gaitText), \(walk.stepsPerMinute) steps a minute"))
            .accessibilityHint(Text("Opens the Pedometer"))
            .accessibilityAddTraits(.isButton)
            .accessibilityIdentifier("top-walking")
        }
    }
}
