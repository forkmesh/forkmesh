import FMApi
import FMBase
import FMTools
import SwiftUI

struct SugarBreakScreen: View {
    @Environment(\.fmPalette) private var fm
    @State private var model: SugarBreakModel?

    init() {
        if let api = Services.get(ApiService.self) {
            _model = State(initialValue: SugarBreakModel(
                api: SugarBreakAPI(api: api), health: HealthFeature.bridge, prefs: HealthFeature.prefs))
        } else {
            _model = State(initialValue: nil)
        }
    }

    var body: some View {
        content
            .navigationTitle(SugarBreakModel.title)
            .navigationBarTitleDisplayMode(.inline)
            .background(fm.canvas)
    }

    @ViewBuilder private var content: some View {
        if let model = model {
            SugarBreakContent(model: model)
                .task { await model.load() }
        } else {
            FmEmptyState(icon: "no_food_outlined", title: SugarBreakModel.unavailableTitle)
        }
    }
}

private struct SugarBreakContent: View {
    @Environment(\.fmPalette) private var fm
    @Environment(\.scenePhase) private var scenePhase
    @Bindable var model: SugarBreakModel
    @State private var confirmEnd: Bool = false
    @State private var confirmUninstall: Bool = false
    @State private var deleting: String?
    @State private var contract: ToolsSheet<SugarContract>?

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
            .sheet(item: $contract) { (box: ToolsSheet<SugarContract>) in
                SugarContractSheet(contract: box.value)
            }
            .fmDialog(isPresented: $confirmEnd) {
                FmConfirmDialog(
                    title: "End this sugar break early?",
                    message: "Your days so far stay in your history. You can start a new break any time.",
                    confirmLabel: "End break", destructive: true,
                    onCancel: { confirmEnd = false },
                    onConfirm: {
                        confirmEnd = false
                        FmHaptics.commit()
                        Task { await model.endEarly() }
                    })
            }
            .fmDialog(isPresented: deletingBinding) {
                FmConfirmDialog(
                    title: "Delete this sugar break?",
                    message: "The break and every +1 in it are deleted from your account.",
                    confirmLabel: "Delete", destructive: true,
                    onCancel: { deleting = nil },
                    onConfirm: {
                        let id: String = deleting ?? ""
                        deleting = nil
                        Task { await model.deleteBreak(id) }
                    })
            }
            .fmDialog(isPresented: $confirmUninstall) {
                FmConfirmDialog(
                    title: "Uninstall Sugar Break?",
                    message: "Your breaks and every +1 are deleted from the web dashboard, the desktop app, and this app. Apple Health is not changed.",
                    confirmLabel: "Uninstall", destructive: true,
                    onCancel: { confirmUninstall = false },
                    onConfirm: {
                        confirmUninstall = false
                        Task { if await model.uninstall() { await PackagesFeature.removeFromAccount("sugar-break") } }
                    })
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
        if model.showsSpinner {
            FmCircularProgressIndicator()
                .frame(maxWidth: .infinity, maxHeight: .infinity)
        } else if model.showsError {
            FmEmptyState(icon: "no_food_outlined", title: SugarBreakModel.unavailableTitle, message: model.error)
        } else if model.showsInstall {
            installPanel
        } else if model.overview != nil {
            dashboard
        }
    }

    private var installPanel: some View {
        VStack(spacing: FmSpace.x3) {
            Text(SugarBreakModel.installTitle)
                .fmText(.titleMedium)
                .multilineTextAlignment(.center)
            Text(SugarBreakModel.installMessage)
                .fmText(.bodyMedium, color: fm.textSecondary)
                .multilineTextAlignment(.center)
            FmFilledButton(SugarBreakModel.installButton, onPressed: { Task { await model.install() } })
                .padding(.top, FmSpace.x1)
                .accessibilityIdentifier("sugar-break-install")
            noticeLine
        }
        .padding(FmSpace.x5)
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }

    private var dashboard: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: FmSpace.x3) {
                if model.isActive {
                    progressCard
                    skipButton
                    todayCard
                    statsCard
                } else {
                    if model.currentBreak != nil {
                        resultCard
                    }
                    startCard
                }
                noticeLine
                FmOutlinedButton(
                    "Send a no-sugar contract", icon: "ios_share", fillWidth: true,
                    onPressed: { contract = ToolsSheet(model.contract()) })
                    .accessibilityIdentifier("sugar-break-contract")
                if !model.days.isEmpty {
                    FmSectionHeader(title: "Days")
                    SugarDayGrid(days: model.days)
                    SugarDayLegend()
                }
                healthCard
                if !model.history.isEmpty {
                    FmSectionHeader(title: "Past breaks")
                    history
                }
                HStack(spacing: FmSpace.x2) {
                    if model.isActive {
                        FmOutlinedButton("End break early", icon: "stop_circle_outlined", onPressed: { confirmEnd = true })
                            .disabled(model.busy)
                            .accessibilityIdentifier("sugar-break-end")
                    }
                    Spacer(minLength: 0)
                    FmTextButton("Uninstall", color: fm.danger, onPressed: { confirmUninstall = true })
                }
            }
            .padding(FmSpace.x4)
        }
        .fmRefreshable { await model.load() }
    }

    private var noticeLine: some View {
        FmNoticeLine(model.notice?.message, isError: model.notice?.isError ?? false, onDismiss: { model.notice = nil })
    }

    private var progressCard: some View {
        FmCard {
            HStack(spacing: FmSpace.x4) {
                ZStack {
                    Circle()
                        .stroke(fm.border, lineWidth: 10)
                    Circle()
                        .trim(from: 0, to: model.progressRatio)
                        .stroke(fm.success, style: StrokeStyle(lineWidth: 10, lineCap: .round))
                        .rotationEffect(Angle.degrees(-90))
                        .animation(.easeOut(duration: 0.3), value: model.progressRatio)
                    VStack(spacing: 0) {
                        Text(String(model.sugarFreeDays))
                            .font(Font.system(size: 28, weight: .semibold))
                            .foregroundStyle(fm.textPrimary)
                            .contentTransition(.numericText())
                        Text("of \(model.lengthDays) days")
                            .font(Font.system(size: 11))
                            .foregroundStyle(fm.textSecondary)
                    }
                }
                .frame(width: 112, height: 112)
                .accessibilityElement(children: .combine)
                .accessibilityLabel(model.progressLine)
                .accessibilityIdentifier("sugar-break-ring")
                VStack(alignment: .leading, spacing: FmSpace.x2) {
                    Text(model.headline)
                        .fmText(.titleLarge)
                        .accessibilityIdentifier("sugar-break-headline")
                    Text(model.progressLine)
                        .fmText(.bodyMedium)
                    Text(model.remainingLine)
                        .fmText(.bodySmall, color: fm.textSecondary)
                }
                .frame(maxWidth: .infinity, alignment: .leading)
            }
        }
    }

    private var skipButton: some View {
        Button(action: {
            FmHaptics.commit()
            Task { await model.skip() }
        }) {
            VStack(spacing: FmSpace.x1) {
                Text("+1")
                    .font(Font.system(size: 44, weight: .bold, design: .rounded))
                Text("I skipped sugar")
                    .font(Font.system(size: 17, weight: .semibold))
            }
            .foregroundStyle(fm.onPrimary)
            .frame(maxWidth: .infinity, minHeight: 132)
            .background(fm.success)
            .clipShape(RoundedRectangle(cornerRadius: FmRadius.lg, style: .continuous))
        }
        .buttonStyle(.plain)
        .accessibilityLabel("Plus one, I skipped sugar")
        .accessibilityIdentifier("sugar-break-skip")
    }

    private var todayCard: some View {
        FmCard {
            VStack(alignment: .leading, spacing: FmSpace.x2) {
                HStack(spacing: FmSpace.x2) {
                    Text("Today")
                        .fmText(.titleMedium)
                    Spacer(minLength: 0)
                    Text("+\(model.todaySkips)")
                        .fmText(.titleMedium, color: fm.success)
                        .contentTransition(.numericText())
                        .accessibilityIdentifier("sugar-break-today-count")
                }
                Text(model.todayLine)
                    .fmText(.bodyMedium, color: fm.textSecondary)
                if !model.healthTodayLine.isEmpty {
                    Text(model.healthTodayLine)
                        .fmText(.bodySmall, color: fm.warning)
                        .accessibilityIdentifier("sugar-break-health-today")
                        .fmMessageTools(model.healthTodayLine)
                }
                FmOutlinedButton(
                    model.suggestsSlip ? "Log that I had sugar" : "I had sugar", icon: "add",
                    foreground: fm.danger, onPressed: {
                        FmHaptics.tap()
                        Task { await model.slip() }
                    })
                    .disabled(model.busy)
                    .accessibilityIdentifier("sugar-break-slip")
                ForEach(model.events) { (row: SugarBreakEventRow) in
                    HStack(spacing: FmSpace.x3) {
                        Circle()
                            .fill(row.slip ? fm.danger : fm.success)
                            .frame(width: 8, height: 8)
                        Text(row.title)
                            .fmText(.bodyMedium)
                        Spacer(minLength: 0)
                        Text(row.time)
                            .fmText(.bodySmall, color: fm.textSecondary)
                        FmIconButton(icon: "undo", label: "Undo", onPressed: {
                            Task { await model.undo(row.id) }
                        })
                    }
                    .contextMenu {
                        Button(role: .destructive) {
                            Task { await model.undo(row.id) }
                        } label: {
                            SwiftUI.Label("Undo", systemImage: "arrow.uturn.backward")
                        }
                    }
                    .accessibilityIdentifier("sugar-break-event-\(row.id)")
                }
            }
        }
    }

    private var statsCard: some View {
        FmCard {
            LazyVGrid(columns: [GridItem(.flexible()), GridItem(.flexible()), GridItem(.flexible())], alignment: .leading,
                      spacing: FmSpace.x3) {
                ForEach(model.stats) { (stat: SugarBreakStat) in
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
    }

    private var resultCard: some View {
        FmCard {
            VStack(alignment: .leading, spacing: FmSpace.x2) {
                Text(model.headline)
                    .fmText(.titleMedium)
                Text(model.remainingLine)
                    .fmText(.bodySmall, color: fm.textSecondary)
                Text(model.resultLine)
                    .fmText(.bodyMedium)
            }
        }
    }

    private var startCard: some View {
        FmCard {
            VStack(alignment: .leading, spacing: FmSpace.x3) {
                Text(SugarBreakModel.startTitle)
                    .fmText(.titleMedium)
                Text(SugarBreakModel.startMessage)
                    .fmText(.bodyMedium, color: fm.textSecondary)
                Picker("Length", selection: $model.selectedLength) {
                    ForEach(model.lengths) { (length: SugarBreakLength) in
                        Text(length.label).tag(length.days)
                    }
                }
                .pickerStyle(.menu)
                .tint(fm.accent)
                .accessibilityIdentifier("sugar-break-length")
                FmFilledButton(
                    "Start sugar break", icon: "play_arrow", background: fm.success, minHeight: 52, fillWidth: true,
                    onPressed: {
                        FmHaptics.commit()
                        Task { await model.startBreak() }
                    })
                    .disabled(model.busy)
                    .accessibilityIdentifier("sugar-break-start")
            }
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
                    Text("Show sugar from Apple Health")
                        .fmText(.bodyLarge)
                }
                .tint(fm.accent)
                .disabled(!model.healthAvailable)
                .accessibilityIdentifier("sugar-break-health-toggle")
                Text(model.healthLine)
                    .fmText(.bodySmall, color: fm.textSecondary)
                AppleHealthDisclosure(tool: .sugarBreak)
            }
        }
    }

    private var history: some View {
        VStack(alignment: .leading, spacing: FmSpace.x2) {
            ForEach(model.history) { (row: SugarBreakHistoryRow) in
                HStack(spacing: FmSpace.x3) {
                    VStack(alignment: .leading, spacing: 2) {
                        HStack(spacing: FmSpace.x2) {
                            Text(row.title)
                                .fmText(.bodyLarge)
                            Text(row.status)
                                .fmText(.labelSmall, color: fm.textSecondary)
                        }
                        Text(row.detail)
                            .fmText(.bodySmall, color: fm.textSecondary)
                    }
                    .frame(maxWidth: .infinity, alignment: .leading)
                    FmIconButton(icon: "delete_outline", label: "Delete", onPressed: { deleting = row.id })
                }
                .padding(.vertical, FmSpace.x1)
                .accessibilityIdentifier("sugar-break-history-\(row.id)")
            }
        }
    }
}

struct SugarDayGrid: View {
    @Environment(\.fmPalette) private var fm
    let days: [SugarBreakDay]

    private let columns: [GridItem] = Array(repeating: GridItem(.flexible(), spacing: 6), count: 7)

    var body: some View {
        LazyVGrid(columns: columns, spacing: 6) {
            ForEach(days) { (day: SugarBreakDay) in
                cell(day)
            }
        }
        .accessibilityIdentifier("sugar-break-days")
    }

    private func fill(_ status: SugarDayStatus) -> Color {
        switch status {
        case .sugarFree: return fm.success
        case .slip: return fm.danger
        case .open: return fm.border.opacity(0.6)
        case .upcoming: return Color.clear
        }
    }

    private func ink(_ status: SugarDayStatus) -> Color {
        switch status {
        case .sugarFree, .slip: return fm.onPrimary
        case .open: return fm.textPrimary
        case .upcoming: return fm.textTertiary
        }
    }

    private func cell(_ day: SugarBreakDay) -> some View {
        VStack(spacing: 1) {
            Text(String(day.number))
                .font(Font.system(size: 13, weight: day.isToday ? .bold : .medium))
            if let grams = day.healthGrams {
                Text(SugarBreakText.grams(grams))
                    .font(Font.system(size: 9))
                    .lineLimit(1)
                    .minimumScaleFactor(0.7)
            }
        }
        .foregroundStyle(ink(day.status))
        .frame(maxWidth: .infinity, minHeight: 40)
        .background(fill(day.status))
        .clipShape(RoundedRectangle(cornerRadius: 8, style: .continuous))
        .overlay {
            RoundedRectangle(cornerRadius: 8, style: .continuous)
                .strokeBorder(day.isToday ? fm.accent : (day.status == .upcoming ? fm.border : Color.clear),
                              lineWidth: day.isToday ? 2 : 1)
        }
        .accessibilityElement(children: .ignore)
        .accessibilityLabel(day.caption)
    }
}

struct SugarDayLegend: View {
    @Environment(\.fmPalette) private var fm

    var body: some View {
        HStack(spacing: FmSpace.x3) {
            item(fm.success, "Sugar-free")
            item(fm.danger, "Had sugar")
            item(fm.border.opacity(0.6), "Nothing logged")
        }
        .accessibilityElement(children: .combine)
    }

    private func item(_ color: Color, _ label: String) -> some View {
        HStack(spacing: FmSpace.x1) {
            RoundedRectangle(cornerRadius: 3, style: .continuous)
                .fill(color)
                .frame(width: 10, height: 10)
            Text(label)
                .fmText(.bodySmall, color: fm.textSecondary)
        }
    }
}
