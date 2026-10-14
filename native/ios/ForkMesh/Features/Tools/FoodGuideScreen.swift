import FMApi
import FMBase
import FMTools
import SwiftUI

struct FoodGuideScreen: View {
    @Environment(\.fmPalette) private var fm
    @State private var model: FoodGuideModel

    init() {
        let api: FoodGuideAPI? = Services.get(ApiService.self).map { (api: ApiService) -> FoodGuideAPI in FoodGuideAPI(api: api) }
        _model = State(initialValue: FoodGuideModel(api: api, health: HealthFeature.bridge, prefs: HealthFeature.prefs))
    }

    var body: some View {
        FoodGuideContent(model: model)
            .task { await model.load() }
            .navigationTitle(FoodGuideModel.title)
            .navigationBarTitleDisplayMode(.inline)
            .background(fm.canvas)
    }
}

private struct FoodGuideContent: View {
    @Environment(\.fmPalette) private var fm
    @Environment(\.scenePhase) private var scenePhase
    @Bindable var model: FoodGuideModel

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
    }

    @ViewBuilder private var main: some View {
        if !model.loaded {
            FmCircularProgressIndicator()
                .frame(maxWidth: .infinity, maxHeight: .infinity)
        } else {
            ScrollView {
                VStack(alignment: .leading, spacing: FmSpace.x3) {
                    Text(FoodGuideModel.intro)
                        .fmText(.bodyMedium, color: fm.textSecondary)
                    noticeLine
                    budgetCard
                    needsCard
                    ForEach(model.plans) { (plan: FoodMealPlan) in
                        FoodMealSection(model: model, plan: plan)
                    }
                    dietCard
                    Text(FoodGuideModel.disclaimer)
                        .fmText(.bodySmall, color: fm.textTertiary)
                        .fixedSize(horizontal: false, vertical: true)
                }
                .padding(FmSpace.x4)
            }
            .fmRefreshable { await model.load() }
        }
    }

    private var noticeLine: some View {
        FmNoticeLine(model.notice?.message, isError: model.notice?.isError ?? false, onDismiss: { model.notice = nil })
    }

    private var budgetCard: some View {
        FmCard {
            VStack(alignment: .leading, spacing: FmSpace.x2) {
                Text("Today's calorie budget")
                    .fmText(.titleMedium)
                Text(model.budget.line)
                    .fmText(.bodyMedium)
                    .accessibilityIdentifier("food-guide-budget")
                Text(model.budget.sourceLine)
                    .fmText(.bodySmall, color: fm.textSecondary)
                    .fixedSize(horizontal: false, vertical: true)
                FmHairline()
                HStack(spacing: FmSpace.x2) {
                    Text("Goal")
                        .fmText(.bodyLarge)
                    Spacer(minLength: 0)
                    Picker("Goal", selection: Binding<FoodGoal>(get: { model.goal }, set: { (goal: FoodGoal) in model.setGoal(goal) })) {
                        ForEach(FoodGoal.allCases) { (goal: FoodGoal) in
                            Text(goal.label).tag(goal)
                        }
                    }
                    .pickerStyle(.menu)
                    .tint(fm.accent)
                    .accessibilityIdentifier("food-guide-goal")
                }
                Text(model.goal.line)
                    .fmText(.bodySmall, color: fm.textSecondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
        }
    }

    private var needsCard: some View {
        FmCard {
            VStack(alignment: .leading, spacing: FmSpace.x2) {
                Text("What your diet needs")
                    .fmText(.titleMedium)
                Text(model.needsLine)
                    .fmText(.bodySmall, color: fm.textSecondary)
                    .fixedSize(horizontal: false, vertical: true)
                ForEach(model.gaps) { (need: FoodNeed) in
                    HStack(alignment: .firstTextBaseline, spacing: FmSpace.x2) {
                        Image(systemName: need.isLow ? "arrow.down.circle.fill" : "arrow.up.circle.fill")
                            .foregroundStyle(fm.warning)
                            .accessibilityHidden(true)
                        VStack(alignment: .leading, spacing: 2) {
                            Text(need.title)
                                .fmText(.bodyLarge)
                            Text(need.line)
                                .fmText(.bodySmall, color: fm.textSecondary)
                                .fixedSize(horizontal: false, vertical: true)
                        }
                    }
                    .accessibilityElement(children: .combine)
                    .accessibilityIdentifier("food-guide-need-\(need.id)")
                }
                Toggle(isOn: Binding<Bool>(
                    get: { model.healthEnabled },
                    set: { (on: Bool) in
                        if on {
                            Task { await model.enableHealth() }
                        } else {
                            model.disableHealth()
                        }
                    })) {
                    Text("Use my nutrition from Apple Health")
                        .fmText(.bodyLarge)
                }
                .tint(fm.accent)
                .disabled(!model.healthAvailable)
                .accessibilityIdentifier("food-guide-health-toggle")
                AppleHealthDisclosure(tool: .foodGuide)
            }
        }
    }

    private var dietCard: some View {
        FmCard {
            VStack(alignment: .leading, spacing: FmSpace.x2) {
                Text("Leave out")
                    .fmText(.titleMedium)
                LazyVGrid(columns: [GridItem(.adaptive(minimum: 150), spacing: FmSpace.x2)], alignment: .leading, spacing: FmSpace.x2) {
                    ForEach(FoodDiet.allCases) { (diet: FoodDiet) in
                        FoodDietChip(label: diet.label, on: model.diets.contains(diet), onTap: { model.toggleDiet(diet) })
                            .accessibilityIdentifier("food-guide-diet-\(diet.id)")
                    }
                }
            }
        }
    }
}

private struct FoodDietChip: View {
    @Environment(\.fmPalette) private var fm
    let label: String
    let on: Bool
    let onTap: () -> Void

    var body: some View {
        Button(action: {
            FmHaptics.tap()
            onTap()
        }) {
            HStack(spacing: FmSpace.x1) {
                Image(systemName: on ? "checkmark.circle.fill" : "circle")
                Text(label)
                    .lineLimit(1)
                    .minimumScaleFactor(0.8)
            }
            .font(Font.system(size: 14, weight: .medium))
            .foregroundStyle(on ? fm.onPrimary : fm.textPrimary)
            .padding(.horizontal, FmSpace.x3)
            .frame(maxWidth: .infinity, minHeight: 44, alignment: .leading)
            .background(Capsule().fill(on ? fm.accent : fm.bgRaised))
            .overlay(Capsule().stroke(on ? Color.clear : fm.border, lineWidth: 1))
        }
        .buttonStyle(.plain)
        .accessibilityAddTraits(on ? .isSelected : [])
    }
}

private struct FoodMealSection: View {
    @Environment(\.fmPalette) private var fm
    @Bindable var model: FoodGuideModel
    let plan: FoodMealPlan

    var body: some View {
        VStack(alignment: .leading, spacing: FmSpace.x2) {
            FmSectionHeader(title: plan.title, trailing: AnyView(
                Text(plan.line)
                    .fmText(.bodySmall, color: fm.textSecondary)
                    .lineLimit(2)
                    .multilineTextAlignment(.trailing)))
            let picks: [FoodSuggestion] = model.suggestions(for: plan.meal)
            if picks.isEmpty {
                Text("No \(plan.meal.rawValue) ideas match what you left out. Turn one off below to see more.")
                    .fmText(.bodySmall, color: fm.textSecondary)
            }
            ForEach(picks) { (suggestion: FoodSuggestion) in
                FoodSuggestionCard(suggestion: suggestion)
            }
            if !picks.isEmpty {
                FmTextButton("Show other \(plan.meal.rawValue) ideas", icon: "refresh", onPressed: {
                    FmHaptics.tap()
                    model.moreIdeas(plan.meal)
                })
                .accessibilityIdentifier("food-guide-more-\(plan.meal.rawValue)")
            }
        }
        .padding(.top, FmSpace.x2)
        .accessibilityIdentifier("food-guide-meal-\(plan.meal.rawValue)")
    }
}

private struct FoodSuggestionCard: View {
    @Environment(\.fmPalette) private var fm
    @Environment(\.openURL) private var openURL
    let suggestion: FoodSuggestion

    var body: some View {
        FmCard {
            VStack(alignment: .leading, spacing: FmSpace.x2) {
                Text(suggestion.recipe.title)
                    .fmText(.titleSmall)
                    .fixedSize(horizontal: false, vertical: true)
                Text(suggestion.metaLine)
                    .fmText(.bodySmall, color: fm.textSecondary)
                Text(suggestion.nutrientLine)
                    .fmText(.bodySmall)
                ForEach(suggestion.reasons, id: \.self) { (reason: String) in
                    HStack(alignment: .firstTextBaseline, spacing: FmSpace.x2) {
                        Image(systemName: "checkmark.circle.fill")
                            .foregroundStyle(fm.success)
                            .accessibilityHidden(true)
                        Text(reason)
                            .fmText(.bodySmall)
                            .fixedSize(horizontal: false, vertical: true)
                    }
                }
                HStack(alignment: .firstTextBaseline, spacing: FmSpace.x2) {
                    Image(systemName: "cart")
                        .foregroundStyle(fm.textSecondary)
                        .accessibilityHidden(true)
                    Text(suggestion.shoppingLine)
                        .fmText(.bodySmall, color: fm.textSecondary)
                        .fixedSize(horizontal: false, vertical: true)
                }
                if let url = URL(string: suggestion.recipe.url) {
                    FmTextButton("Open recipe", icon: "open_in_new", onPressed: { openURL(url) })
                        .accessibilityIdentifier("food-guide-open-\(suggestion.id)")
                }
            }
        }
        .accessibilityIdentifier("food-guide-suggestion-\(suggestion.id)")
    }
}
