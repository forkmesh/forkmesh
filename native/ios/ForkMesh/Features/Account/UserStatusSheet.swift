// The Away toggle plus the custom status editor (an emoji from the quick picks or any one typed, a line
// of text, and when it should clear). Refusals show inside the sheet with the
// fields still filled in. Logic: FMApi's UserStatusSheetModel.

import FMApi
import FMBase
import SwiftUI

/// Present in a `.sheet`. Shows nothing useful without a signed-in
/// UserStatusService (`showUserStatusSheet` did nothing then).
@MainActor
struct UserStatusSheet: View {
    @Environment(\.dismiss) var dismiss
    private let service: UserStatusService?

    init() {
        service = Services.get(UserStatusService.self)
    }

    var body: some View {
        if let service = service, service.signedIn {
            UserStatusEditor(service: service, onClose: { dismiss() })
                .fmSheetStyle(fitsContent: false)
        } else {
            Color.clear
                .onAppear { dismiss() }
        }
    }
}

private struct UserStatusEditor: View {
    @Environment(\.fmPalette) var fm
    @State var model: UserStatusSheetModel
    @State var pickingCustom: Bool = false
    @State var customDraft: Date = Date()
    let onClose: () -> Void

    init(service: UserStatusService, onClose: @escaping () -> Void) {
        _model = State(initialValue: UserStatusSheetModel(service: service))
        self.onClose = onClose
    }

    private var header: some View {
        HStack(spacing: 0) {
            Text("Set a status")
                .fmText(.titleLarge, height: 1.1)
                .frame(maxWidth: .infinity, alignment: .leading)
            FmIconButton(label: "Close", onPressed: model.busy ? nil : onClose) {
                FmIcon("close", size: 24, color: fm.textPrimary)
                    .frame(width: 40, height: 40)
                    .shellGlass(Circle())
            }
            .accessibilityIdentifier("user-status-close")
        }
        .padding(EdgeInsets(top: FmSpace.x4, leading: FmSpace.x4, bottom: FmSpace.x2, trailing: FmSpace.x3))
    }

    private var awayToggle: some View {
        Toggle(
            isOn: Binding<Bool>(
                get: { model.away },
                set: { (away: Bool) in Task { await model.setAway(away) } })
        ) {
            VStack(alignment: .leading, spacing: 2) {
                Text("Away").fmText(.bodyLarge)
                Text("Show others you are not around").fmText(.bodySmall)
            }
        }
        .disabled(model.busy)
        .tint(fm.accent)
        .accessibilityIdentifier("user-status-away")
    }

    private var emojiPicker: some View {
        FmField(label: "Emoji") {
            ScrollView(.horizontal, showsIndicators: false) {
                HStack(spacing: FmSpace.x1) {
                    ForEach(statusQuickEmojis, id: \.self) { (emoji: String) in
                        UserStatusEmojiChoice(
                            emoji: emoji,
                            selected: model.isSelected(emoji),
                            onTap: model.busy ? nil : { model.pickEmoji(emoji) })
                    }
                    TextField("", text: $model.emoji, prompt: Text("Other"))
                        .multilineTextAlignment(.center)
                        .frame(width: 72)
                        .disabled(model.busy)
                        .onChange(of: model.emoji) { (_: String, _: String) in model.problem = "" }
                        .accessibilityIdentifier("user-status-emoji")
                }
            }
        }
    }

    private var textField: some View {
        FmField(label: "Status") {
            FmTextField(
                text: Binding<String>(
                    get: { model.text },
                    set: { (value: String) in model.text = String(value.prefix(UserStatus.textMax)) }),
                hint: "What's your status?",
                onSubmit: { save() })
                .textInputAutocapitalization(.sentences)
                .submitLabel(.done)
                .disabled(model.busy)
                .accessibilityIdentifier("user-status-text")
        }
    }

    private var clearAfterPicker: some View {
        FmField(label: "Clear after") {
            Menu {
                ForEach(ClearAfter.allCases, id: \.self) { (choice: ClearAfter) in
                    Button(choice.label) { choose(choice) }
                }
            } label: {
                HStack {
                    Text(model.clearAfter.label).fmText(.bodyLarge)
                    Spacer(minLength: 0)
                    FmIcon("arrow_drop_down", size: 24)
                }
                .padding(.vertical, FmSpace.x2)
            }
            .disabled(model.busy)
            .accessibilityIdentifier("user-status-clear-after")
        }
    }

    @ViewBuilder private var customTime: some View {
        if model.clearAfter == .custom {
            FmTextButton(model.customLabel, icon: "event_outlined", onPressed: model.busy ? nil : { openPicker() })
                .accessibilityIdentifier("user-status-custom-time")
        }
    }

    private var suggestions: some View {
        VStack(alignment: .leading, spacing: FmSpace.x1) {
            Text("Suggestions")
                .fmText(.bodySmall, weight: FmType.medium)
            ForEach(statusPresets, id: \.self) { (preset: StatusPreset) in
                UserStatusPresetRow(preset: preset, onTap: model.busy ? nil : { model.applyPreset(preset) })
            }
        }
    }

    @ViewBuilder private var problem: some View {
        if !model.problem.isEmpty {
            Text(model.problem)
                .fmText(.bodySmall, color: fm.danger)
                .accessibilityIdentifier("user-status-problem")
                .fmMessageTools(model.problem)
        }
    }

    private var footer: some View {
        FmBottomActionBar(padding: EdgeInsets(top: FmSpace.x3, leading: FmSpace.x4, bottom: FmSpace.x3, trailing: FmSpace.x4)) {
            HStack(spacing: FmSpace.x3) {
                FmOutlinedButton(
                    "Clear status",
                    minHeight: 48,
                    radius: FmRadius.lg,
                    fillWidth: true,
                    onPressed: model.canClear ? { clear() } : nil)
                    .accessibilityIdentifier("user-status-clear")
                FmFilledButton(
                    model.saveLabel,
                    background: fm.accent,
                    foreground: Color.white,
                    minHeight: 48,
                    radius: FmRadius.lg,
                    fillWidth: true,
                    role: .titleSmall,
                    onPressed: model.busy ? nil : { save() })
                    .accessibilityIdentifier("user-status-save")
            }
        }
    }

    private var customPicker: some View {
        NavigationStack {
            DatePicker(
                "Clear at",
                selection: $customDraft,
                in: model.pickerRange,
                displayedComponents: [.date, .hourAndMinute])
                .datePickerStyle(.graphical)
                .padding(FmSpace.x4)
                .toolbar {
                    ToolbarItem(placement: .cancellationAction) {
                        Button("Cancel") { pickingCustom = false }
                    }
                    ToolbarItem(placement: .confirmationAction) {
                        Button("Done") {
                            model.setCustom(customDraft)
                            pickingCustom = false
                        }
                    }
                }
        }
        .presentationDetents([.large])
    }

    var body: some View {
        VStack(spacing: 0) {
            header
            ScrollView {
                VStack(alignment: .leading, spacing: 0) {
                    awayToggle
                    emojiPicker.padding(.top, FmSpace.x3)
                    textField.padding(.top, FmSpace.x4)
                    clearAfterPicker.padding(.top, FmSpace.x2)
                    customTime
                    suggestions.padding(.top, FmSpace.x4)
                    problem.padding(.top, FmSpace.x3)
                }
                .padding(EdgeInsets(top: 0, leading: FmSpace.x4, bottom: FmSpace.x4, trailing: FmSpace.x4))
            }
            .scrollDismissesKeyboard(.interactively)
            footer
        }
        .fmPreGlassBackground(fm.bgRaised)
        .sheet(isPresented: $pickingCustom) { customPicker }
        .accessibilityIdentifier("user-status-sheet")
    }

    private func choose(_ choice: ClearAfter) {
        if choice == .custom {
            openPicker()
            return
        }
        model.chooseClearAfter(choice)
    }

    private func openPicker() {
        customDraft = model.pickerStart
        pickingCustom = true
    }

    private func save() {
        Task { @MainActor in
            if await model.save() {
                onClose()
            }
        }
    }

    private func clear() {
        Task { @MainActor in
            if await model.clear() {
                onClose()
            }
        }
    }
}

private struct UserStatusEmojiChoice: View {
    @Environment(\.fmPalette) var fm
    let emoji: String
    let selected: Bool
    let onTap: (() -> Void)?

    var body: some View {
        Button(action: { onTap?() }) {
            Text(verbatim: emoji)
                .font(Font.system(size: FmType.heading))
                .frame(width: 44, height: 44)
                .background(selected ? fm.accentSubtle : Color.clear)
                .clipShape(FmRoundedShape(radius: FmRadius.md))
        }
        .buttonStyle(FmPressStyle(highlight: fm.textPrimary.opacity(0.06), cornerRadius: FmRadius.md))
        .disabled(onTap == nil)
        .accessibilityAddTraits(selected ? [.isSelected] : [])
    }
}

private struct UserStatusPresetRow: View {
    @Environment(\.fmPalette) var fm
    let preset: StatusPreset
    let onTap: (() -> Void)?

    var body: some View {
        Button(action: { onTap?() }) {
            HStack(spacing: FmSpace.x3) {
                Text(verbatim: preset.emoji).font(Font.system(size: FmType.body))
                Text(preset.text)
                    .fmText(.bodyMedium)
                    .frame(maxWidth: .infinity, alignment: .leading)
                Text(preset.clearAfter.label)
                    .fmText(.bodySmall, color: fm.textTertiary)
            }
            .padding(.vertical, FmSpace.x2)
            .frame(minHeight: 44)
            .contentShape(Rectangle())
        }
        .buttonStyle(FmPressStyle(highlight: fm.textPrimary.opacity(0.06), cornerRadius: FmRadius.md))
        .disabled(onTap == nil)
    }
}
