// The Calorie Counter package on the account-level Worker API. Install panel, then Today, Trends,
// Goals and Challenges. Photos come from the photo library (PhotosPicker), are resized when too
// large and sent once to the host AI model; barcodes are typed. State: FMTools' CaloriesModel and
// its sheet models.

import FMApi
import FMBase
import FMTools
import PhotosUI
import SwiftUI
import UIKit
import UniformTypeIdentifiers

/// A text prompt the screen is showing.
struct CaloriesPrompt: Identifiable {
    enum Kind {
        case photoHint
        case describe
        case barcode
        case invite(String)
    }

    let id: UUID = UUID()
    let kind: Kind
    let title: String
    let hint: String
    let action: String
    let multiline: Bool
}

struct CaloriesScreen: View {
    @Environment(\.fmPalette) private var fm
    @State private var model: CaloriesModel?

    init() {
        if let api = Services.get(ApiService.self) {
            let model: CaloriesModel = CaloriesModel(api: api)
            model.attachHealth(HealthFeature.bridge, prefs: HealthFeature.prefs)
            _model = State(initialValue: model)
        } else {
            _model = State(initialValue: nil)
        }
    }

    var body: some View {
        content
            .navigationTitle(CaloriesModel.title)
            .navigationBarTitleDisplayMode(.inline)
            .background(fm.bgBase)
    }

    @ViewBuilder private var content: some View {
        if let model = model {
            CaloriesContent(model: model)
                .task { await model.load() }
        } else {
            FmEmptyState(icon: "restaurant_outlined", title: CaloriesModel.unavailableTitle)
        }
    }
}

private struct CaloriesContent: View {
    @Environment(\.fmPalette) private var fm
    @Bindable var model: CaloriesModel
    @State private var prompt: CaloriesPrompt?
    @State private var promptText: String = ""
    @State private var photoPickerShown: Bool = false
    @State private var photoItem: PhotosPickerItem?
    @State private var pendingPhoto: String?
    @State private var entry: ToolsSheet<CaloriesEntryDraft>?
    @State private var suggestions: ToolsSheet<CaloriesSuggestionsModel>?
    @State private var search: ToolsSheet<CaloriesSearchModel>?
    @State private var portion: ToolsSheet<CaloriesPortionModel>?
    @State private var challenge: ToolsSheet<CaloriesChallengeDraft>?
    @State private var board: ToolsSheet<CaloriesBoard>?
    @State private var pickingDay: Bool = false
    @State private var weighing: Bool = false
    @State private var weightText: String = ""
    @State private var quickAdding: Bool = false
    @State private var onScreen: Bool = false
    @State private var promptToken: UUID?

    var body: some View {
        body1
            .onAppear {
                onScreen = true
                syncPromptTarget()
            }
            .onDisappear {
                onScreen = false
                syncPromptTarget()
            }
            .onChange(of: model.installed) { (_: Bool, _: Bool) in
                syncPromptTarget()
            }
            .photosPicker(isPresented: $photoPickerShown, selection: $photoItem, matching: .images)
            .onChange(of: photoItem) { (_: PhotosPickerItem?, item: PhotosPickerItem?) in
                if let item = item {
                    photoItem = nil
                    Task { await readPhoto(item) }
                }
            }
    }

    private var body1: some View {
        main
            .toolbar {
                ToolbarItem(placement: .primaryAction) {
                    FmIconButton(icon: "refresh", label: "Refresh", onPressed: { Task { await model.load() } })
                }
            }
            .fmDialog(isPresented: promptShown) { promptDialog }
            .fmDialog(isPresented: $weighing) { weighInDialog }
            .sheet(item: $entry) { (box: ToolsSheet<CaloriesEntryDraft>) in
                CaloriesEntrySheet(draft: box.value, onSave: {
                    entry = nil
                    Task { await model.saveEntry(box.value) }
                }, onDelete: {
                    entry = nil
                    if let existing = box.value.entry {
                        Task { await model.deleteEntry(existing) }
                    }
                })
            }
            .sheet(item: $suggestions) { (box: ToolsSheet<CaloriesSuggestionsModel>) in
                CaloriesSuggestionsSheet(model: box.value) {
                    suggestions = nil
                    Task { await model.addSuggestions(box.value) }
                }
            }
            .sheet(item: $search) { (box: ToolsSheet<CaloriesSearchModel>) in
                CaloriesSearchSheet(model: box.value) { (food: JSONObject) in
                    search = nil
                    if let next = model.portion(food, source: "search") {
                        portion = ToolsSheet(next)
                    }
                }
            }
            .sheet(item: $portion) { (box: ToolsSheet<CaloriesPortionModel>) in
                CaloriesPortionSheet(model: box.value) {
                    portion = nil
                    Task { await model.addPortion(box.value) }
                }
            }
            .sheet(item: $challenge) { (box: ToolsSheet<CaloriesChallengeDraft>) in
                CaloriesChallengeSheet(draft: box.value) {
                    challenge = nil
                    Task { await model.createChallenge(box.value) }
                }
            }
            .sheet(item: $board) { (box: ToolsSheet<CaloriesBoard>) in
                CaloriesBoardSheet(board: box.value) { (action: CaloriesBoardAction) in
                    board = nil
                    boardAction(action, box.value)
                }
            }
            .sheet(isPresented: $quickAdding) {
                CaloriesQuickAddSheet(model: model)
            }
            .sheet(isPresented: $pickingDay) {
                ToolsDateSheet(title: "Day", initial: model.dayDate, range: model.dayRange) { (date: Date) in
                    Task { await model.pickDay(date) }
                }
            }
    }

    @ViewBuilder private var main: some View {
        if model.showsSpinner {
            FmCircularProgressIndicator()
                .frame(maxWidth: .infinity, maxHeight: .infinity)
        } else if model.showsError {
            FmEmptyState(icon: "restaurant_outlined", title: CaloriesModel.unavailableTitle, message: model.error)
        } else if model.showsInstall {
            installPanel
        } else if model.overview != nil {
            VStack(spacing: 0) {
                FmSegmentTabBar(
                    tabs: CaloriesModel.tabs.map { (label: String) -> FmSegmentTab in FmSegmentTab(label: label) },
                    index: model.tab,
                    onChanged: { (index: Int) in model.tab = index })
                if model.notice != nil {
                    noticeLine
                        .padding(.horizontal, FmSpace.x4)
                        .padding(.top, FmSpace.x2)
                }
                tabContent
            }
        }
    }

    private var noticeLine: some View {
        FmNoticeLine(model.notice?.message, isError: model.notice?.isError ?? false, onDismiss: { model.notice = nil })
    }

    private var installPanel: some View {
        VStack(spacing: FmSpace.x3) {
            Text(CaloriesModel.installTitle)
                .fmText(.titleMedium)
                .multilineTextAlignment(.center)
            Text(CaloriesModel.installMessage)
                .fmText(.bodyMedium, color: fm.textSecondary)
                .multilineTextAlignment(.center)
            FmFilledButton(CaloriesModel.installButton, onPressed: { Task { await model.install() } })
                .padding(.top, FmSpace.x1)
                .accessibilityIdentifier("calories-install")
            noticeLine
        }
        .padding(FmSpace.x5)
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .accessibilityIdentifier("calories-install-panel")
    }

    @ViewBuilder private var tabContent: some View {
        switch model.tab {
        case 0:
            CaloriesTodayTab(model: model, onPickDay: { pickingDay = true }, onAction: todayAction)
        case 1:
            CaloriesTrendsTab(model: model)
        case 2:
            CaloriesGoalsTab(model: model, form: model.goalsModel())
                .id(model.goalsKey)
        default:
            CaloriesChallengesTab(model: model, onNew: { challenge = ToolsSheet(model.challengeDraft()) }, onOpen: openChallenge)
        }
    }

    // MARK: Today actions

    private func todayAction(_ action: String, _ existing: JSONObject?) {
        switch action {
        case "photo":
            if let refusal = model.photoUnavailable {
                model.notice = ToolsNotice(refusal, isError: true)
            } else {
                photoPickerShown = true
            }
        case "describe":
            ask(.describe, CaloriesModel.describePrompt.title, CaloriesModel.describePrompt.hint, multiline: true)
        case "barcode":
            ask(.barcode, CaloriesModel.barcodePrompt.title, CaloriesModel.barcodePrompt.hint, multiline: false)
        case "search":
            search = ToolsSheet(model.searchModel())
        case "weigh":
            weightText = model.weighInInitial
            weighing = true
        case "quick":
            quickAdding = true
        default:
            entry = ToolsSheet(model.entryDraft(existing))
        }
    }

    private func readPhoto(_ item: PhotosPickerItem) async {
        let ext: String = item.supportedContentTypes.first?.preferredFilenameExtension ?? ""
        guard let data = try? await item.loadTransferable(type: Data.self),
              let url = CaloriesPhotoEncoder.dataURL(data, fileExtension: ext) else {
            model.photoUnreadable()
            return
        }
        pendingPhoto = url
        ask(.photoHint, CaloriesModel.photoHintPrompt.title, CaloriesModel.photoHintPrompt.hint, multiline: false)
    }

    private func syncPromptTarget() {
        guard let targets = Services.get(ShellPromptTargets.self) else {
            return
        }
        if onScreen && model.installed {
            if promptToken == nil {
                let calories: CaloriesModel = model
                promptToken = targets.register(ShellPromptTarget(
                    id: "calories", label: "Log", icon: "restaurant_outlined", title: CaloriesQuick.title,
                    placeholder: CaloriesQuick.placeholder,
                    submit: { (text: String) async throws -> String in
                        try await calories.logTyped(text)
                    }))
            }
        } else if let token = promptToken {
            targets.unregister(token)
            promptToken = nil
        }
    }

    // MARK: Prompts

    private var promptShown: Binding<Bool> {
        Binding<Bool>(get: { prompt != nil }, set: { (shown: Bool) in
            if !shown {
                prompt = nil
            }
        })
    }

    private func ask(_ kind: CaloriesPrompt.Kind, _ title: String, _ hint: String, multiline: Bool, action: String = "OK") {
        promptText = ""
        prompt = CaloriesPrompt(kind: kind, title: title, hint: hint, action: action, multiline: multiline)
    }

    @ViewBuilder private var promptDialog: some View {
        if let current = prompt {
            ToolsPromptDialog(
                title: current.title, hint: current.hint, action: current.action, multiline: current.multiline,
                text: $promptText,
                onCancel: { prompt = nil },
                onSubmit: { (text: String) in
                    prompt = nil
                    answer(current.kind, text.trimmingCharacters(in: .whitespacesAndNewlines))
                })
                .keyboardType(keyboard(current.kind))
        }
    }

    private func keyboard(_ kind: CaloriesPrompt.Kind) -> UIKeyboardType {
        if case .barcode = kind {
            return .numberPad
        }
        return .default
    }

    private func answer(_ kind: CaloriesPrompt.Kind, _ text: String) {
        switch kind {
        case .photoHint:
            guard let photo = pendingPhoto else {
                return
            }
            pendingPhoto = nil
            Task {
                if let found = await model.analyzePhoto(dataURL: photo, hint: text) {
                    suggestions = ToolsSheet(found)
                }
            }
        case .describe:
            Task {
                if let found = await model.describeMeal(text) {
                    suggestions = ToolsSheet(found)
                }
            }
        case .barcode:
            if text.isEmpty {
                return
            }
            Task {
                if let product = await model.lookupBarcode(text) {
                    portion = ToolsSheet(product)
                }
            }
        case .invite(let id):
            Task { await model.invite(challengeId: id, members: CaloriesText.usernames(text)) }
        }
    }

    private var weighInDialog: some View {
        FmDialog(
            title: model.weighInTitle,
            content: {
                HStack(spacing: FmSpace.x2) {
                    FmTextField(text: $weightText, autofocus: true)
                        .keyboardType(.decimalPad)
                        .accessibilityIdentifier("calories-weight-field")
                    Text(model.weightUnit)
                        .fmText(.bodyMedium, color: fm.textSecondary)
                }
            },
            actions: {
                HStack(spacing: FmSpace.x2) {
                    FmTextButton("Remove", onPressed: {
                        weighing = false
                        Task { await model.removeWeighIn() }
                    })
                    Spacer(minLength: 0)
                    FmTextButton("Cancel", color: fm.textPrimary, onPressed: { weighing = false })
                    FmFilledButton("Save", onPressed: {
                        weighing = false
                        let text: String = weightText
                        Task { await model.saveWeighIn(text) }
                    })
                }
            })
    }

    // MARK: Challenges

    private func openChallenge(_ id: String) {
        Task {
            if let found = await model.openChallenge(id) {
                board = ToolsSheet(found)
            }
        }
    }

    private func boardAction(_ action: CaloriesBoardAction, _ current: CaloriesBoard) {
        if action == .invite {
            ask(.invite(current.id), "Invite friends", "maya, @sam", multiline: false, action: "Invite")
            return
        }
        Task { await model.boardAction(action, board: current) }
    }
}

/// Bounds a library photo to the Worker's data URL limit: small JPEG, PNG and WebP files travel
/// as they are; anything larger is redrawn at 800, 560 then 400 points wide as PNG.
enum CaloriesPhotoEncoder {
    static func dataURL(_ data: Data, fileExtension: String) -> String? {
        if let original = CaloriesPhoto.dataURL(bytes: data, fileExtension: fileExtension) {
            return original
        }
        guard let image = UIImage(data: data), image.size.width > 0 else {
            return nil
        }
        for width in CaloriesPhoto.widths {
            let scale: CGFloat = CGFloat(width) / image.size.width
            let size: CGSize = CGSize(width: CGFloat(width), height: (image.size.height * scale).rounded())
            let format: UIGraphicsImageRendererFormat = UIGraphicsImageRendererFormat()
            format.scale = 1
            let renderer: UIGraphicsImageRenderer = UIGraphicsImageRenderer(size: size, format: format)
            let png: Data = renderer.pngData { (_: UIGraphicsImageRendererContext) in
                image.draw(in: CGRect(origin: CGPoint.zero, size: size))
            }
            if let url = CaloriesPhoto.pngDataURL(png) {
                return url
            }
        }
        return nil
    }
}

extension CaloriesPhotoEncoder {
    static func receiptDataURL(_ data: Data) -> String? {
        guard let image = UIImage(data: data), image.size.width > 0, image.size.height > 0 else {
            return nil
        }
        let longest: CGFloat = max(image.size.width, image.size.height)
        for side in CaloriesPhoto.receiptSides {
            let scale: CGFloat = min(1, CGFloat(side) / longest)
            let size: CGSize = CGSize(width: (image.size.width * scale).rounded(), height: (image.size.height * scale).rounded())
            let format: UIGraphicsImageRendererFormat = UIGraphicsImageRendererFormat()
            format.scale = 1
            let renderer: UIGraphicsImageRenderer = UIGraphicsImageRenderer(size: size, format: format)
            let jpeg: Data = renderer.jpegData(withCompressionQuality: 0.8) { (_: UIGraphicsImageRendererContext) in
                image.draw(in: CGRect(origin: CGPoint.zero, size: size))
            }
            if let url = CaloriesPhoto.receiptDataURL(jpeg: jpeg) {
                return url
            }
        }
        return nil
    }
}

// MARK: - Tabs

private struct CaloriesTodayTab: View {
    @Environment(\.fmPalette) private var fm
    let model: CaloriesModel
    let onPickDay: () -> Void
    let onAction: (String, JSONObject?) -> Void

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: FmSpace.x3) {
                dayRow
                summaryCard
                statsCard
                actions
                FmSectionHeader(title: model.diaryHeader)
                    .padding(.top, FmSpace.x1)
                diary
            }
            .padding(FmSpace.x4)
        }
        .fmRefreshable { await model.load() }
    }

    private var dayRow: some View {
        HStack {
            FmOutlinedButton(model.dayLabel(model.day), icon: "calendar_today_outlined", onPressed: onPickDay)
                .accessibilityIdentifier("calories-day-button")
            Spacer()
            FmTextButton("Today", onPressed: { Task { await model.goToToday() } })
        }
    }

    private var summaryCard: some View {
        FmCard {
            HStack(spacing: FmSpace.x4) {
                ZStack {
                    CaloriesRingView(ring: model.ring, track: fm.border, accent: fm.accent, overColor: fm.warning)
                    VStack(spacing: 0) {
                        Text(model.caloriesText)
                            .font(Font.system(size: 24, weight: .semibold))
                            .foregroundStyle(fm.textPrimary)
                        Text(model.goalLine)
                            .font(Font.system(size: 11))
                            .foregroundStyle(fm.textSecondary)
                    }
                }
                .frame(width: 120, height: 120)
                .accessibilityIdentifier("calories-ring")
                VStack(alignment: .leading, spacing: FmSpace.x2) {
                    ForEach(model.macros, id: \.label) { (macro: CaloriesMacro) in
                        CaloriesMacroRow(macro: macro)
                    }
                    Text(model.remainingLine)
                        .font(Font.system(size: 12))
                        .foregroundStyle(fm.textSecondary)
                }
            }
        }
    }

    private var statsCard: some View {
        FmCard {
            HStack(alignment: .top, spacing: FmSpace.x3) {
                CaloriesStat(label: "Streaks", value: model.streaksText)
                CaloriesStat(label: "Weight", value: model.weightStat)
            }
        }
    }

    private var actions: some View {
        ToolsFlowLayout(spacing: FmSpace.x2, lineSpacing: FmSpace.x2) {
            FmFilledButton("Photo", icon: "photo_camera_outlined", onPressed: { onAction("photo", nil) })
                .accessibilityIdentifier("calories-photo")
            FmOutlinedButton("Quick add", icon: "bolt_outlined", onPressed: { onAction("quick", nil) })
                .accessibilityIdentifier("calories-quick-add")
            FmOutlinedButton("Describe", icon: "auto_awesome_outlined", onPressed: { onAction("describe", nil) })
            FmOutlinedButton("Barcode", icon: "qr_code_scanner_outlined", onPressed: { onAction("barcode", nil) })
                .accessibilityIdentifier("calories-barcode")
            FmOutlinedButton("Search", icon: "search", onPressed: { onAction("search", nil) })
            FmOutlinedButton("Add", icon: "add", onPressed: { onAction("add", nil) })
                .accessibilityIdentifier("calories-add")
            FmOutlinedButton("Weigh in", icon: "monitor_weight_outlined", onPressed: { onAction("weigh", nil) })
        }
    }

    @ViewBuilder private var diary: some View {
        let rows: [CaloriesEntryRow] = model.entryRows
        if rows.isEmpty {
            Text(CaloriesModel.emptyDiary)
                .fmText(.bodyMedium, color: fm.textSecondary)
                .multilineTextAlignment(.center)
                .frame(maxWidth: .infinity)
                .padding(FmSpace.x4)
        }
        ForEach(rows) { (row: CaloriesEntryRow) in
            Button(action: { onAction("edit", row.entry) }) {
                HStack(spacing: FmSpace.x3) {
                    VStack(alignment: .leading, spacing: 2) {
                        Text(row.title)
                            .fmText(.bodyLarge)
                        Text(row.subtitle)
                            .fmText(.bodySmall, color: fm.textSecondary)
                    }
                    .frame(maxWidth: .infinity, alignment: .leading)
                    Text(row.trailing)
                        .fmText(.bodyMedium)
                }
                .padding(.vertical, FmSpace.x2)
                .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            .accessibilityIdentifier("calories-entry-\(row.id)")
        }
    }
}

private struct CaloriesMacroRow: View {
    @Environment(\.fmPalette) private var fm
    let macro: CaloriesMacro

    var body: some View {
        VStack(alignment: .leading, spacing: 3) {
            HStack {
                Text(macro.label)
                    .font(Font.system(size: 12))
                    .foregroundStyle(fm.textSecondary)
                Spacer()
                Text(macro.amounts)
                    .font(Font.system(size: 12))
                    .foregroundStyle(fm.textPrimary)
            }
            CaloriesBar(ratio: macro.ratio)
        }
    }
}

/// A 6pt rounded progress bar.
struct CaloriesBar: View {
    @Environment(\.fmPalette) private var fm
    let ratio: Double

    var body: some View {
        GeometryReader { (proxy: GeometryProxy) in
            ZStack(alignment: .leading) {
                Capsule().fill(fm.border)
                Capsule().fill(fm.accent)
                    .frame(width: proxy.size.width * CGFloat(min(max(ratio, 0), 1)))
            }
        }
        .frame(height: 6)
    }
}

private struct CaloriesStat: View {
    @Environment(\.fmPalette) private var fm
    let label: String
    let value: String

    var body: some View {
        VStack(alignment: .leading, spacing: FmSpace.x1) {
            Text(label.uppercased())
                .font(Font.system(size: 11, weight: .semibold))
                .foregroundStyle(fm.textSecondary)
            Text(value)
                .fmText(.bodyMedium)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
    }
}

private struct CaloriesTrendsTab: View {
    @Environment(\.fmPalette) private var fm
    let model: CaloriesModel

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: FmSpace.x2) {
                FmSectionHeader(title: "Calories · last 30 days")
                FmCard {
                    CaloriesDayChartView(
                        days: model.days, goal: model.goal, accent: fm.accent, over: fm.warning, goalColor: fm.warning,
                        grid: fm.border, ink: fm.textSecondary)
                        .frame(height: 200)
                        .accessibilityIdentifier("calories-day-chart")
                }
                Text(model.trendsNote)
                    .font(Font.system(size: 12))
                    .foregroundStyle(fm.textSecondary)
                    .padding(.vertical, FmSpace.x2)
                FmSectionHeader(title: "Weight · last 180 days")
                FmCard {
                    weightChart
                        .frame(height: 180)
                }
                FmSectionHeader(title: "Day by day")
                    .padding(.top, FmSpace.x3)
                dayRows
            }
            .padding(FmSpace.x4)
        }
    }

    @ViewBuilder private var weightChart: some View {
        if model.weights.isEmpty {
            Text("No weigh-ins yet. Use Weigh in on the Today tab.")
                .fmText(.bodyMedium, color: fm.textSecondary)
                .frame(maxWidth: .infinity, maxHeight: .infinity)
        } else {
            CaloriesWeightChartView(
                points: model.weights, target: model.targetWeightKg, imperial: model.imperial, line: fm.success,
                goalColor: fm.warning, grid: fm.border, ink: fm.textSecondary, surface: fm.bgRaised)
        }
    }

    private var dayRows: some View {
        ForEach(Array(model.dayRows.enumerated()), id: \.offset) { (pair: (offset: Int, element: (label: String, macros: String, calories: String))) in
            HStack {
                VStack(alignment: .leading, spacing: 2) {
                    Text(pair.element.label)
                        .fmText(.bodyMedium)
                    Text(pair.element.macros)
                        .fmText(.bodySmall, color: fm.textSecondary)
                }
                .frame(maxWidth: .infinity, alignment: .leading)
                Text(pair.element.calories)
                    .fmText(.bodyMedium)
            }
            .padding(.vertical, FmSpace.x1)
        }
    }
}

private struct CaloriesChallengesTab: View {
    @Environment(\.fmPalette) private var fm
    let model: CaloriesModel
    let onNew: () -> Void
    let onOpen: (String) -> Void

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: FmSpace.x3) {
                Text(CaloriesModel.challengesIntro)
                    .fmText(.bodyMedium, color: fm.textSecondary)
                FmFilledButton("New challenge", icon: "emoji_events_outlined", onPressed: onNew)
                    .accessibilityIdentifier("calories-new-challenge")
                if model.challengeRows.isEmpty {
                    Text(CaloriesModel.noChallenges)
                        .fmText(.bodyMedium, color: fm.textSecondary)
                        .multilineTextAlignment(.center)
                        .frame(maxWidth: .infinity)
                        .padding(FmSpace.x4)
                }
                ForEach(model.challengeRows) { (row: CaloriesChallengeRow) in
                    card(row)
                }
            }
            .padding(FmSpace.x4)
        }
    }

    private func card(_ row: CaloriesChallengeRow) -> some View {
        FmCard(onTap: { onOpen(row.id) }) {
            VStack(alignment: .leading, spacing: FmSpace.x1) {
                HStack {
                    Text(row.title)
                        .font(Font.system(size: FmType.body, weight: .semibold))
                        .foregroundStyle(fm.textPrimary)
                        .frame(maxWidth: .infinity, alignment: .leading)
                    FmStatusBadge(label: row.status, color: row.active ? fm.success : fm.textSecondary)
                }
                Text(row.kind)
                    .font(Font.system(size: 12))
                    .foregroundStyle(fm.textSecondary)
                Text(row.dates)
                    .font(Font.system(size: 12))
                    .foregroundStyle(fm.textSecondary)
                if row.invited {
                    Text("You are invited")
                        .font(Font.system(size: 12, weight: .semibold))
                        .foregroundStyle(fm.accent)
                }
            }
        }
        .accessibilityIdentifier("calories-challenge-\(row.id)")
    }
}
