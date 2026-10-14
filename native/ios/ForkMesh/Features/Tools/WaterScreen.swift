import FMApi
import FMBase
import FMTools
import SwiftUI

struct WaterScreen: View {
    @Environment(\.fmPalette) private var fm
    @State private var model: WaterModel?

    init() {
        if let api = Services.get(ApiService.self) {
            _model = State(initialValue: WaterModel(
                api: WaterAPI(api: api), health: HealthFeature.bridge, prefs: HealthFeature.prefs))
        } else {
            _model = State(initialValue: nil)
        }
    }

    var body: some View {
        content
            .navigationTitle(WaterModel.title)
            .navigationBarTitleDisplayMode(.inline)
            .background(fm.canvas)
    }

    @ViewBuilder private var content: some View {
        if let model = model {
            WaterContent(model: model)
                .task { await model.load() }
        } else {
            FmEmptyState(icon: "water_drop_outlined", title: WaterModel.unavailableTitle)
        }
    }
}

private struct WaterContent: View {
    @Environment(\.fmPalette) private var fm
    @Bindable var model: WaterModel
    @State private var adding: Bool = false
    @State private var amountText: String = ""
    @State private var drink: String = "water"
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
            .fmDialog(isPresented: $adding) { addDialog }
            .fmDialog(isPresented: $editingGoal) { goalDialog }
            .fmDialog(isPresented: $confirmUninstall) {
                FmConfirmDialog(
                    title: "Uninstall Water Intake?",
                    message: "Your drinks log is deleted from the web dashboard, the desktop app, and this app. Water already written to Apple Health stays there.",
                    confirmLabel: "Uninstall", destructive: true,
                    onCancel: { confirmUninstall = false },
                    onConfirm: {
                        confirmUninstall = false
                        Task { if await model.uninstall() { await PackagesFeature.removeFromAccount("water") } }
                    })
            }
    }

    @ViewBuilder private var main: some View {
        if model.showsSpinner {
            FmCircularProgressIndicator()
                .frame(maxWidth: .infinity, maxHeight: .infinity)
        } else if model.showsError {
            FmEmptyState(icon: "water_drop_outlined", title: WaterModel.unavailableTitle, message: model.error)
        } else if model.showsInstall {
            installPanel
        } else if model.overview != nil {
            dashboard
        }
    }

    private var installPanel: some View {
        VStack(spacing: FmSpace.x3) {
            Text(WaterModel.installTitle)
                .fmText(.titleMedium)
                .multilineTextAlignment(.center)
            Text(WaterModel.installMessage)
                .fmText(.bodyMedium, color: fm.textSecondary)
                .multilineTextAlignment(.center)
            FmFilledButton(WaterModel.installButton, onPressed: { Task { await model.install() } })
                .padding(.top, FmSpace.x1)
                .accessibilityIdentifier("water-install")
            noticeLine
        }
        .padding(FmSpace.x5)
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }

    private var dashboard: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: FmSpace.x3) {
                todayCard
                ToolsFlowLayout(spacing: FmSpace.x2, lineSpacing: FmSpace.x2) {
                    ForEach(model.presets) { (preset: WaterPreset) in
                        FmFilledButton("+ \(preset.label)", icon: "water_drop_outlined", onPressed: {
                            Task { await model.add(milliliters: preset.milliliters) }
                        })
                        .disabled(model.busy)
                        .accessibilityIdentifier("water-preset-\(preset.milliliters)")
                    }
                    FmOutlinedButton("Other drink", icon: "add", onPressed: {
                        amountText = ""
                        drink = "water"
                        adding = true
                    })
                }
                noticeLine
                healthCard
                FmSectionHeader(title: "Today")
                entries
                FmSectionHeader(title: "Last 30 days")
                WaterBarsView(bars: model.bars, accent: fm.accent, met: fm.success, ink: fm.textSecondary)
                    .frame(height: 140)
                Text(model.summaryLine)
                    .fmText(.bodySmall, color: fm.textSecondary)
                HStack(spacing: FmSpace.x2) {
                    FmOutlinedButton("Goal", icon: "flag_outlined", onPressed: {
                        goalText = String(model.goalMl)
                        goalUnits = model.unitsPreference
                        editingGoal = true
                    })
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

    private var todayCard: some View {
        FmCard {
            HStack(spacing: FmSpace.x4) {
                WaterGlassView(ratio: model.ratio, fill: model.goalMet ? fm.success : fm.accent, outline: fm.border)
                    .frame(width: 84, height: 116)
                    .accessibilityHidden(true)
                VStack(alignment: .leading, spacing: FmSpace.x2) {
                    Text(model.totalText)
                        .font(Font.system(size: 28, weight: .semibold))
                        .foregroundStyle(fm.textPrimary)
                        .contentTransition(.numericText())
                        .accessibilityIdentifier("water-total")
                    Text(model.goalLine)
                        .fmText(.bodyMedium, color: fm.textSecondary)
                }
                .frame(maxWidth: .infinity, alignment: .leading)
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
                    Text("Share with Apple Health")
                        .fmText(.bodyLarge)
                }
                .tint(fm.accent)
                .disabled(!model.healthAvailable)
                .accessibilityIdentifier("water-health-toggle")
                Text(model.healthLine)
                    .fmText(.bodySmall, color: fm.textSecondary)
                AppleHealthDisclosure(tool: .water)
            }
        }
    }

    @ViewBuilder private var entries: some View {
        let rows: [WaterEntryRow] = model.rows
        if rows.isEmpty {
            Text(WaterModel.emptyLog)
                .fmText(.bodyMedium, color: fm.textSecondary)
                .frame(maxWidth: .infinity)
                .padding(FmSpace.x3)
        }
        ForEach(rows) { (row: WaterEntryRow) in
            HStack(spacing: FmSpace.x3) {
                VStack(alignment: .leading, spacing: 2) {
                    Text(row.title)
                        .fmText(.bodyLarge)
                    if !row.subtitle.isEmpty {
                        Text(row.subtitle)
                            .fmText(.bodySmall, color: fm.textSecondary)
                    }
                }
                .frame(maxWidth: .infinity, alignment: .leading)
                Text(row.amount)
                    .fmText(.bodyMedium)
                FmIconButton(icon: "delete_outline", label: "Delete", onPressed: {
                    Task { await model.delete(row.id) }
                })
            }
            .padding(.vertical, FmSpace.x1)
            .accessibilityIdentifier("water-entry-\(row.id)")
        }
    }

    private var addDialog: some View {
        FmDialog(
            title: "Log a drink",
            content: {
                VStack(alignment: .leading, spacing: FmSpace.x2) {
                    HStack(spacing: FmSpace.x2) {
                        FmTextField(text: $amountText, hint: model.amountHint, autofocus: true)
                            .keyboardType(.decimalPad)
                        Text(model.unit)
                            .fmText(.bodyMedium, color: fm.textSecondary)
                    }
                    Picker("Drink", selection: $drink) {
                        ForEach(WaterModel.drinks, id: \.self) { (value: String) in
                            Text(WaterText.drinkLabel(value)).tag(value)
                        }
                    }
                    .pickerStyle(.menu)
                }
            },
            actions: {
                HStack(spacing: FmSpace.x2) {
                    Spacer(minLength: 0)
                    FmTextButton("Cancel", color: fm.textPrimary, onPressed: { adding = false })
                    FmFilledButton("Log", onPressed: {
                        adding = false
                        let text: String = amountText
                        let chosen: String = drink
                        Task { await model.addTyped(text, drink: chosen) }
                    })
                }
            })
    }

    private var goalDialog: some View {
        FmDialog(
            title: "Daily water goal (ml)",
            content: {
                VStack(alignment: .leading, spacing: FmSpace.x2) {
                    FmTextField(text: $goalText, hint: "2000", autofocus: true)
                        .keyboardType(.numberPad)
                    Picker("Units", selection: $goalUnits) {
                        Text("Automatic").tag("auto")
                        Text("ml").tag("metric")
                        Text("fl oz").tag("imperial")
                        Text("cups").tag("cups")
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

struct WaterGlassView: View {
    let ratio: Double
    let fill: Color
    let outline: Color

    var body: some View {
        Canvas { (context: inout GraphicsContext, size: CGSize) in
            var glass: Path = Path()
            glass.move(to: CGPoint(x: 4, y: 4))
            glass.addLine(to: CGPoint(x: size.width - 4, y: 4))
            glass.addLine(to: CGPoint(x: size.width - 12, y: size.height - 4))
            glass.addLine(to: CGPoint(x: 12, y: size.height - 4))
            glass.closeSubpath()
            let level: CGFloat = 4 + (size.height - 8) * CGFloat(1 - min(1, max(0, ratio)))
            context.drawLayer { (layer: inout GraphicsContext) in
                layer.clip(to: glass)
                layer.fill(Path(CGRect(x: 0, y: level, width: size.width, height: size.height - level)), with: .color(fill.opacity(0.85)))
            }
            context.stroke(glass, with: .color(outline), style: StrokeStyle(lineWidth: 3, lineJoin: .round))
        }
    }
}

struct WaterBarsView: View {
    let bars: [WaterBar]
    let accent: Color
    let met: Color
    let ink: Color

    var body: some View {
        Canvas { (context: inout GraphicsContext, size: CGSize) in
            guard !bars.isEmpty else { return }
            let plotHeight: CGFloat = size.height - 16
            let slot: CGFloat = size.width / CGFloat(bars.count)
            let width: CGFloat = max(2, min(12, slot - 3))
            for (index, bar) in bars.enumerated() {
                let height: CGFloat = bar.ratio > 0 ? max(2, plotHeight * CGFloat(min(1, bar.ratio))) : 0
                let x: CGFloat = CGFloat(index) * slot + (slot - width) / 2
                context.fill(
                    Path(roundedRect: CGRect(x: x, y: plotHeight - height, width: width, height: height), cornerRadius: 2),
                    with: .color(bar.metGoal ? met : accent))
                if !bar.label.isEmpty {
                    let text: GraphicsContext.ResolvedText = context.resolve(
                        Text(bar.label).font(Font.system(size: 9)).foregroundColor(ink))
                    context.draw(text, at: CGPoint(x: x + width / 2, y: size.height), anchor: .bottom)
                }
            }
        }
        .accessibilityElement()
        .accessibilityLabel(bars.suffix(7).map(\.caption).joined(separator: ", "))
    }
}
