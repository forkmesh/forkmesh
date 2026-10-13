// The Calorie Counter's sheets (entry, suggestions, search, portion, challenge, board) and its
// Goals form. State: FMTools' CaloriesEntryDraft, CaloriesSuggestionsModel, CaloriesSearchModel,
// CaloriesPortionModel, CaloriesGoalsModel, CaloriesChallengeDraft and CaloriesBoard.

import FMApi
import FMBase
import FMTools
import PhotosUI
import SwiftUI

/// A title over a scrolling column.
private struct CaloriesSheetFrame<Content: View>: View {
    let title: String
    let content: Content

    init(title: String, @ViewBuilder content: () -> Content) {
        self.title = title
        self.content = content()
    }

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: FmSpace.x3) {
                Text(title)
                    .fmText(.titleMedium)
                content
            }
            .padding(FmSpace.x4)
        }
        .fmSheetStyle(fitsContent: false)
    }
}

/// The meal picker every sheet shares.
private struct CaloriesMealPicker: View {
    @Binding var meal: String

    var body: some View {
        FmField(label: "Meal") {
            Picker("", selection: $meal) {
                ForEach(CaloriesText.meals, id: \.self) { (value: String) in
                    Text(CaloriesText.cap(value)).tag(value)
                }
            }
            .pickerStyle(.segmented)
            .labelsHidden()
        }
    }
}

struct CaloriesEntrySheet: View {
    @Environment(\.fmPalette) private var fm
    @Environment(\.dismiss) private var dismiss
    @Bindable var draft: CaloriesEntryDraft
    let onSave: () -> Void
    let onDelete: () -> Void

    var body: some View {
        CaloriesSheetFrame(title: draft.sheetTitle) {
            ToolsLabeledField(label: "Food", text: $draft.name)
                .accessibilityIdentifier("calories-entry-name")
            ToolsLabeledField(label: "Portion (1 bowl, 150 g…)", text: $draft.portion)
            CaloriesMealPicker(meal: $draft.meal)
            HStack(spacing: FmSpace.x2) {
                ToolsLabeledField(label: "kcal", text: $draft.calories)
                    .keyboardType(.numberPad)
                    .accessibilityIdentifier("calories-entry-kcal")
                ToolsLabeledField(label: "Protein g", text: $draft.protein)
                    .keyboardType(.decimalPad)
                ToolsLabeledField(label: "Carbs g", text: $draft.carbs)
                    .keyboardType(.decimalPad)
                ToolsLabeledField(label: "Fat g", text: $draft.fat)
                    .keyboardType(.decimalPad)
            }
            ToolsLabeledField(label: "Notes", text: $draft.notes, multiline: true)
            HStack(spacing: FmSpace.x2) {
                if draft.editing {
                    FmTextButton("Delete", color: fm.danger, onPressed: onDelete)
                }
                Spacer()
                FmTextButton("Cancel", onPressed: { dismiss() })
                FmFilledButton("Save", onPressed: {
                    if draft.result() != nil {
                        onSave()
                    }
                })
                .accessibilityIdentifier("calories-entry-save")
            }
        }
    }
}

struct CaloriesSuggestionsSheet: View {
    @Environment(\.fmPalette) private var fm
    @Bindable var model: CaloriesSuggestionsModel
    let onAdd: () -> Void

    var body: some View {
        CaloriesSheetFrame(title: model.sheetTitle) {
            Text(model.intro)
                .font(Font.system(size: 12))
                .foregroundStyle(fm.textSecondary)
            ForEach(Array(model.items.indices), id: \.self) { (index: Int) in
                row(index)
            }
            CaloriesMealPicker(meal: $model.meal)
            FmFilledButton(CaloriesSuggestionsModel.addButton, fillWidth: true, onPressed: onAdd)
        }
    }

    private func row(_ index: Int) -> some View {
        HStack(spacing: FmSpace.x2) {
            Toggle("", isOn: $model.chosen[index])
                .labelsHidden()
                .toggleStyle(.switch)
            VStack(alignment: .leading, spacing: 2) {
                Text(model.name(index))
                    .fmText(.bodyMedium)
                Text(model.detail(index))
                    .font(Font.system(size: 11))
                    .foregroundStyle(fm.textSecondary)
            }
            .frame(maxWidth: .infinity, alignment: .leading)
            HStack(spacing: 2) {
                TextField("", text: $model.kcal[index])
                    .keyboardType(.numberPad)
                    .multilineTextAlignment(.trailing)
                    .frame(width: 48)
                Text("kcal")
                    .font(Font.system(size: 11))
                    .foregroundStyle(fm.textSecondary)
            }
        }
    }
}

struct CaloriesSearchSheet: View {
    @Environment(\.fmPalette) private var fm
    @Bindable var model: CaloriesSearchModel
    let onPick: (JSONObject) -> Void

    var body: some View {
        CaloriesSheetFrame(title: CaloriesSearchModel.title) {
            HStack(spacing: FmSpace.x2) {
                FmTextField(text: $model.query, hint: CaloriesSearchModel.hint, autofocus: true, onSubmit: submit)
                    .submitLabel(.search)
                    .onChange(of: model.query) { (_: String, _: String) in model.queryChanged() }
                FmIconButton(icon: "search", label: "Search", onPressed: submit)
            }
            Text(model.status)
                .font(Font.system(size: 12))
                .foregroundStyle(fm.textSecondary)
                .frame(maxWidth: .infinity, alignment: .leading)
                .fmMessageTools(model.status)
            ForEach(Array(model.results.enumerated()), id: \.offset) { (pair: (offset: Int, element: JSONObject)) in
                Button(action: { onPick(pair.element) }) {
                    VStack(alignment: .leading, spacing: 2) {
                        Text(model.title(pair.element))
                            .fmText(.bodyLarge)
                        let subtitle: String = model.subtitle(pair.element)
                        if !subtitle.isEmpty {
                            Text(subtitle)
                                .fmText(.bodySmall, color: fm.textSecondary)
                        }
                    }
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .padding(.vertical, FmSpace.x2)
                    .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
            }
        }
    }

    private func submit() {
        if let food = model.submitted() { onPick(food) }
    }
}

struct CaloriesQuickAddSheet: View {
    @Environment(\.fmPalette) private var fm
    @Environment(\.dismiss) private var dismiss
    @Bindable var model: CaloriesModel
    @State private var receiptPickerShown: Bool = false
    @State private var receiptItem: PhotosPickerItem?

    var body: some View {
        CaloriesSheetFrame(title: model.quickHeader) {
            CaloriesMealPicker(meal: $model.quickMeal)
            FmTextField(text: $model.quickFilter, hint: "Filter foods")
                .autocorrectionDisabled(true)
                .accessibilityIdentifier("calories-quick-filter")
            HStack(spacing: FmSpace.x2) {
                FmOutlinedButton(
                    model.readingReceipt ? "Reading…" : "Read receipt", icon: "receipt_long_outlined",
                    onPressed: model.readingReceipt ? nil : { pickReceipt() })
                    .accessibilityIdentifier("calories-read-receipt")
                Spacer(minLength: 0)
                FmTextButton("Done", onPressed: { dismiss() })
            }
            Text(CaloriesQuick.receiptHelp)
                .font(Font.system(size: 12))
                .foregroundStyle(fm.textSecondary)
            FmNoticeLine(model.notice?.message, isError: model.notice?.isError ?? false, onDismiss: { model.notice = nil })
            ForEach(model.quickSections) { (section: CaloriesQuickSection) in
                sectionView(section)
            }
        }
        .photosPicker(isPresented: $receiptPickerShown, selection: $receiptItem, matching: .images)
        .onChange(of: receiptItem) { (_: PhotosPickerItem?, item: PhotosPickerItem?) in
            if let item = item {
                receiptItem = nil
                Task { await readReceipt(item) }
            }
        }
    }

    private func sectionView(_ section: CaloriesQuickSection) -> some View {
        VStack(alignment: .leading, spacing: 0) {
            FmSectionHeader(title: section.title, count: section.foods.count)
                .padding(.top, FmSpace.x2)
            if section.foods.isEmpty {
                Text(section.empty)
                    .font(Font.system(size: 12))
                    .foregroundStyle(fm.textSecondary)
                    .padding(.vertical, FmSpace.x1)
            }
            ForEach(section.foods) { (food: CaloriesQuickFood) in
                row(food, kind: section.kind)
            }
        }
    }

    private func row(_ food: CaloriesQuickFood, kind: CaloriesQuickKind) -> some View {
        HStack(spacing: FmSpace.x1) {
            Button(action: { Task { await model.quickAdd(food) } }) {
                VStack(alignment: .leading, spacing: 2) {
                    Text(food.name)
                        .fmText(.bodyLarge)
                    Text(food.meta)
                        .fmText(.bodySmall, color: fm.textSecondary)
                }
                .frame(maxWidth: .infinity, alignment: .leading)
                .padding(.vertical, FmSpace.x2)
                .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            .accessibilityLabel(Text("Log \(food.name), \(food.meta)"))
            .accessibilityIdentifier("calories-quick-\(kind.rawValue)-\(food.id)")
            side(food, kind: kind)
        }
    }

    @ViewBuilder
    private func side(_ food: CaloriesQuickFood, kind: CaloriesQuickKind) -> some View {
        if kind == .mine {
            FmIconButton(icon: "close", label: "Remove \(food.name) from My foods", size: 20, onPressed: {
                Task { await model.removeMyFood(food) }
            })
        } else if model.isMyFood(food) {
            FmIconButton(icon: "star", label: "\(food.name) is in My foods", size: 20, color: fm.accent, onPressed: nil)
        } else {
            FmIconButton(icon: "star_border", label: "Save \(food.name) to My foods", size: 20, onPressed: {
                Task { await model.starFood(food) }
            })
        }
    }

    private func pickReceipt() {
        if let refusal = model.receiptUnavailable {
            model.notice = ToolsNotice(refusal, isError: true)
        } else {
            receiptPickerShown = true
        }
    }

    private func readReceipt(_ item: PhotosPickerItem) async {
        guard let data = try? await item.loadTransferable(type: Data.self),
              let url = CaloriesPhotoEncoder.receiptDataURL(data) else {
            model.receiptUnreadable()
            return
        }
        await model.readReceipt(dataURL: url)
    }
}

struct CaloriesPortionSheet: View {
    @Environment(\.fmPalette) private var fm
    @Bindable var model: CaloriesPortionModel
    let onAdd: () -> Void

    var body: some View {
        CaloriesSheetFrame(title: model.sheetTitle) {
            Text(model.subtitle)
                .font(Font.system(size: 12))
                .foregroundStyle(fm.textSecondary)
            FmField(label: "Basis") {
                Picker("", selection: $model.basis) {
                    ForEach(model.bases, id: \.value) { (basis: (value: String, label: String)) in
                        Text(basis.label).tag(basis.value)
                    }
                }
                .pickerStyle(.menu)
                .labelsHidden()
            }
            ToolsLabeledField(label: "Amount", text: $model.amount)
                .keyboardType(.decimalPad)
            CaloriesMealPicker(meal: $model.meal)
            Text(model.summary)
                .fmText(.bodyMedium)
                .padding(.vertical, FmSpace.x2)
            FmFilledButton("Add to diary", fillWidth: true, onPressed: onAdd)
        }
    }
}

struct CaloriesChallengeSheet: View {
    @Environment(\.fmPalette) private var fm
    @Bindable var draft: CaloriesChallengeDraft
    let onSave: () -> Void
    @State private var picking: Bool?

    var body: some View {
        CaloriesSheetFrame(title: "New challenge") {
            ToolsLabeledField(label: "Title", text: $draft.title)
                .accessibilityIdentifier("calories-challenge-title")
            FmField(label: "Kind") {
                Picker("", selection: $draft.kind) {
                    ForEach(CaloriesText.kinds, id: \.value) { (kind: (value: String, label: String)) in
                        Text(kind.label).tag(kind.value)
                    }
                }
                .pickerStyle(.menu)
                .labelsHidden()
            }
            HStack(spacing: FmSpace.x2) {
                FmOutlinedButton(draft.startsLabel, fillWidth: true, onPressed: { picking = true })
                FmOutlinedButton(draft.endsLabel, fillWidth: true, onPressed: { picking = false })
            }
            ToolsLabeledField(label: "Description", text: $draft.description, multiline: true)
            ToolsLabeledField(label: "Invite friends", text: $draft.invite, hint: "ForkMesh usernames, e.g. maya, @sam")
                .textInputAutocapitalization(.never)
                .autocorrectionDisabled(true)
                .accessibilityIdentifier("calories-challenge-invite")
            FmFilledButton("Save challenge", fillWidth: true, onPressed: {
                if draft.result() != nil {
                    onSave()
                }
            })
            .accessibilityIdentifier("calories-challenge-save")
        }
        .sheet(isPresented: pickerShown) {
            ToolsDateSheet(
                title: picking == true ? "Starts" : "Ends",
                initial: picking == true ? draft.startDate : draft.endDate,
                range: dateRange
            ) { (date: Date) in
                if picking == true {
                    draft.setStart(date)
                } else {
                    draft.setEnd(date)
                }
                picking = nil
            }
        }
    }

    private var pickerShown: Binding<Bool> {
        Binding<Bool>(get: { picking != nil }, set: { (shown: Bool) in
            if !shown {
                picking = nil
            }
        })
    }

    /// 2020-01-01 to 400 days from now.
    private var dateRange: ClosedRange<Date> {
        var components: DateComponents = DateComponents()
        components.year = 2020
        components.month = 1
        components.day = 1
        let first: Date = Calendar.current.date(from: components) ?? Date(timeIntervalSince1970: 0)
        return first...Date().addingTimeInterval(400 * 86400)
    }
}

struct CaloriesBoardSheet: View {
    @Environment(\.fmPalette) private var fm
    let board: CaloriesBoard
    let onAction: (CaloriesBoardAction) -> Void

    var body: some View {
        CaloriesSheetFrame(title: board.title) {
            Text(board.subtitle)
                .font(Font.system(size: 12))
                .foregroundStyle(fm.textSecondary)
            if !board.description.isEmpty {
                Text(board.description)
                    .fmText(.bodyMedium)
            }
            ToolsFlowLayout(spacing: FmSpace.x2, lineSpacing: FmSpace.x2) {
                ForEach(board.actions, id: \.self) { (action: CaloriesBoardAction) in
                    button(action)
                }
            }
            if board.members.isEmpty {
                Text(CaloriesBoard.nobody)
                    .fmText(.bodyMedium, color: fm.textSecondary)
            }
            ForEach(board.members) { (member: CaloriesBoard.Member) in
                memberRow(member)
            }
            if let pending = board.pending {
                Text(pending)
                    .font(Font.system(size: 12))
                    .foregroundStyle(fm.textSecondary)
            }
        }
    }

    @ViewBuilder
    private func button(_ action: CaloriesBoardAction) -> some View {
        switch action {
        case .accept:
            FmFilledButton(action.label, onPressed: { onAction(action) })
                .accessibilityIdentifier("calories-board-accept")
        case .delete:
            FmTextButton(action.label, color: fm.danger, onPressed: { onAction(action) })
        default:
            FmOutlinedButton(action.label, onPressed: { onAction(action) })
                .accessibilityIdentifier("calories-board-\(action.rawValue)")
        }
    }

    private func memberRow(_ member: CaloriesBoard.Member) -> some View {
        HStack(spacing: FmSpace.x2) {
            Text(member.rank)
                .font(Font.system(size: FmType.body, weight: .semibold))
                .foregroundStyle(fm.textPrimary)
                .frame(width: 24, alignment: .leading)
            VStack(alignment: .leading, spacing: 3) {
                Text(member.name)
                    .fmText(.bodyMedium)
                CaloriesBar(ratio: member.progress)
            }
            .frame(maxWidth: .infinity, alignment: .leading)
            Text(member.detail)
                .font(Font.system(size: 12))
                .foregroundStyle(fm.textSecondary)
        }
    }
}

/// Daily goals, units, measurements, reminders and the USDA key. Rebuilt (by `.id`)
/// whenever the loaded profile changes.
struct CaloriesGoalsTab: View {
    @Environment(\.fmPalette) private var fm
    let model: CaloriesModel
    @State private var form: CaloriesGoalsModel
    @State private var editingReminder: UUID?
    @State private var addingReminder: Bool = false

    init(model: CaloriesModel, form: CaloriesGoalsModel) {
        self.model = model
        _form = State(initialValue: form)
    }

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: FmSpace.x3) {
                goals
                reminders
                    .padding(.top, FmSpace.x3)
                FmFilledButton("Save goals and reminders", fillWidth: true, onPressed: {
                    Task { await model.saveGoals(form) }
                })
                .accessibilityIdentifier("calories-save-goals")
                appleHealth
                    .padding(.top, FmSpace.x4)
                usda
                    .padding(.top, FmSpace.x4)
            }
            .padding(FmSpace.x4)
        }
        .sheet(isPresented: timeShown) {
            ToolsDateSheet(title: "Reminder", initial: initialTime, timeOnly: true) { (date: Date) in
                pickedTime(date)
            }
        }
    }

    private var appleHealth: some View {
        VStack(alignment: .leading, spacing: FmSpace.x2) {
            FmSectionHeader(title: "Apple Health")
            Toggle(isOn: Binding<Bool>(
                get: { model.healthEnabled },
                set: { (on: Bool) in
                    if on {
                        Task { await model.enableHealth() }
                    } else {
                        model.disableHealth()
                    }
                })) {
                Text("Write meals to Apple Health")
                    .fmText(.bodyLarge)
            }
            .tint(fm.accent)
            .disabled(!model.healthAvailable)
            .accessibilityIdentifier("calories-health-toggle")
            Text(model.healthLine)
                .fmText(.bodySmall, color: fm.textSecondary)
            AppleHealthDisclosure(tool: .calories)
        }
    }

    private var goals: some View {
        VStack(alignment: .leading, spacing: FmSpace.x3) {
            FmSectionHeader(title: "Daily goals")
            numberField("Calories", $form.calories, suffix: "kcal")
            HStack(spacing: FmSpace.x2) {
                numberField("Protein", $form.protein, suffix: "g")
                numberField("Carbs", $form.carbs, suffix: "g")
                numberField("Fat", $form.fat, suffix: "g")
            }
            FmField(label: "Units") {
                Picker("", selection: Binding<String>(get: { form.unitsPreference }, set: { (value: String) in form.setUnits(value) })) {
                    ForEach(form.unitChoices, id: \.value) { (choice: (value: String, label: String)) in
                        Text(choice.label).tag(choice.value)
                    }
                }
                .pickerStyle(.menu)
                .labelsHidden()
            }
            HStack(spacing: FmSpace.x2) {
                numberField("Target weight", $form.target, suffix: form.weightUnit)
                numberField("Starting weight", $form.start, suffix: form.weightUnit)
            }
            numberField("Height", $form.height, suffix: form.heightUnit)
            if let bmi = form.bmiLine {
                Text(bmi)
                    .font(Font.system(size: 12))
                    .foregroundStyle(fm.textSecondary)
            }
        }
    }

    private func numberField(_ label: String, _ text: Binding<String>, suffix: String) -> some View {
        FmField(label: "\(label) (\(suffix))") {
            FmTextField(text: text)
                .keyboardType(.decimalPad)
        }
    }

    private var reminders: some View {
        VStack(alignment: .leading, spacing: FmSpace.x2) {
            FmSectionHeader(title: "Reminders", trailing: FmTextButton("Add", onPressed: form.canAddReminder ? {
                addingReminder = true
            } : nil).fmAny)
            Text(CaloriesGoalsModel.remindersNote)
                .font(Font.system(size: 12))
                .foregroundStyle(fm.textSecondary)
            ForEach($form.reminders) { (reminder: Binding<CaloriesReminder>) in
                reminderRow(reminder)
            }
        }
    }

    private func reminderRow(_ reminder: Binding<CaloriesReminder>) -> some View {
        let id: UUID = reminder.wrappedValue.id
        return VStack(alignment: .leading, spacing: FmSpace.x1) {
            HStack(spacing: FmSpace.x2) {
                FmTextButton(reminder.wrappedValue.time, onPressed: { editingReminder = id })
                FmTextField(text: reminder.label, hint: "Log lunch")
                FmIconButton(icon: "close", label: "Remove reminder", onPressed: { form.removeReminder(id) })
            }
            HStack(spacing: FmSpace.x2) {
                Toggle(isOn: reminder.onlyIfUnlogged) {
                    Text("Only if nothing logged")
                        .font(Font.system(size: 12))
                        .foregroundStyle(fm.textSecondary)
                }
                Toggle("", isOn: reminder.enabled)
                    .labelsHidden()
            }
        }
    }

    private var usda: some View {
        VStack(alignment: .leading, spacing: FmSpace.x2) {
            FmSectionHeader(title: "Food databases")
            Text(form.usdaLine)
                .font(Font.system(size: 12))
                .foregroundStyle(fm.textSecondary)
            FmField(label: "USDA FoodData Central API key") {
                FmTextField(text: $form.usdaKey, secure: true)
            }
            FmOutlinedButton("Save key", onPressed: {
                Task { await model.saveUsdaKey(form) }
            })
        }
    }

    // MARK: Time picking

    private var timeShown: Binding<Bool> {
        Binding<Bool>(get: { addingReminder || editingReminder != nil }, set: { (shown: Bool) in
            if !shown {
                addingReminder = false
                editingReminder = nil
            }
        })
    }

    private var initialTime: Date {
        var hour: Int = 12
        var minute: Int = 30
        if let id = editingReminder, let current = form.reminders.first(where: { $0.id == id }) {
            let parsed: (hour: Int, minute: Int) = CaloriesText.parseClock(current.time)
            hour = parsed.hour
            minute = parsed.minute
        }
        return Calendar.current.date(bySettingHour: hour, minute: minute, second: 0, of: Date()) ?? Date()
    }

    private func pickedTime(_ date: Date) {
        let parts: DateComponents = Calendar.current.dateComponents([.hour, .minute], from: date)
        let hour: Int = parts.hour ?? 12
        let minute: Int = parts.minute ?? 0
        if let id = editingReminder {
            form.setReminderTime(id, hour: hour, minute: minute)
        } else {
            form.addReminder(hour: hour, minute: minute)
        }
        addingReminder = false
        editingReminder = nil
    }
}
