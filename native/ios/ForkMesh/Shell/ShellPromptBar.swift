// The shell's composer. The bottom bar's prompt button opens it over the page
// with the keyboard already up, and Send does the whole job: the run starts at
// once (QuickAgentLauncher decides the repository, where it runs and the model;
// the context line says so and opens the picker). Chat has its own composer.
//
// Speed: the composer is built once with the shell and stays mounted, hidden,
// so opening it is a state flip plus `becomeFirstResponder` on a UITextView
// that already exists, in the same run-loop turn as the tap. Nothing animates
// before the keyboard is asked for.

import FMAccount
import FMAgents
import FMApi
import FMBase
import FMCode
import FMControl
import Observation
import SwiftUI
import UIKit

/// The composer's state, shared by the shell and the overlay. Created by
/// ComposerInstaller (AppServices+Composer.swift).
@MainActor
@Observable
final class ShellComposer {
    var isOpen: Bool = false
    var attachments: [PromptAttachment] = []
    var busy: Bool = false
    /// What stands between the last send and a run, with its one-tap fix.
    var fix: QuickLaunchFix?
    /// The sentence for `fix`, or why the last send failed.
    var message: String?
    let launcher: QuickAgentLauncher
    @ObservationIgnored let focus: ComposerFocus = ComposerFocus()
    @ObservationIgnored private var screenRepos: [(UUID, String)] = []

    var screenRepo: String? { screenRepos.last?.1 }

    func enter(repo key: String) -> UUID {
        let token: UUID = UUID()
        screenRepos.append((token, key))
        return token
    }

    func leave(_ token: UUID) {
        screenRepos.removeAll { $0.0 == token }
    }

    init(launcher: QuickAgentLauncher) {
        self.launcher = launcher
    }

    /// Open with the keyboard up. The field exists already, so this is
    /// synchronous; the overlay retries once on its own change in case the
    /// view was not in a window yet.
    func open() {
        isOpen = true
        focus.focus()
        let launcher: QuickAgentLauncher = self.launcher
        Task { await launcher.prepare() }
    }

    func close() {
        focus.blur()
        isOpen = false
    }

    func clearProblem() {
        fix = nil
        message = nil
    }
}

/// The composer over the page: a dimmed backdrop (tap to close) and the card
/// above the keyboard.
struct ShellComposerOverlay: View {
    @Environment(\.fmPalette) var fm
    @Bindable var model: HomeShellModel
    @Bindable var composer: ShellComposer
    let onSubmit: () -> Void
    let onFix: (QuickLaunchFix) -> Void
    let onPickContext: () -> Void
    let onDiagnostics: () -> Void

    private var target: ShellPromptTarget? { model.promptTarget }
    private var startsAgent: Bool { target == nil }
    private var launcher: QuickAgentLauncher { composer.launcher }
    private var backdropOpacity: Double { fm.dark ? 0.28 : 0.12 }

    var body: some View {
        ZStack(alignment: .bottom) {
            Color.black.opacity(composer.isOpen ? backdropOpacity : 0)
                .ignoresSafeArea()
                .contentShape(Rectangle())
                .onTapGesture { composer.close() }
                .accessibilityLabel(Text("Close the composer"))
                .accessibilityAddTraits(.isButton)
            PromptComposer(
                text: $model.prompt,
                hint: model.promptHint,
                attachments: startsAgent ? $composer.attachments : nil,
                modelLabel: startsAgent ? launcher.chip.label : nil,
                modelOptions: launcher.modelChoices.map { PromptModelOption(id: $0.id, label: $0.label, note: $0.note) },
                selectedModelId: launcher.chip.id,
                onSelectModel: { (id: String) in
                    if let choice = launcher.modelChoices.first(where: { $0.id == id }) {
                        launcher.choose(model: choice)
                    }
                },
                modeOptions: modeOptions,
                selectedModeId: launcher.mode.rawValue,
                onSelectMode: { (id: String) in
                    if let mode = AgentPermissionMode(label: id) {
                        launcher.choose(mode: mode)
                    }
                },
                busy: composer.busy,
                onSend: onSubmit,
                offersFix: startsAgent,
                isOpen: composer.isOpen,
                listensOnOpen: true,
                submitOnReturn: target != nil,
                slashCommands: slashCommands,
                extraControls: model.targets.active != nil ? AnyView(modePill) : nil,
                header: AnyView(header),
                extraAttachAction: PromptAttachAction(
                    label: "Send diagnostics", systemImage: "stethoscope", run: onDiagnostics),
                uploadsFiles: startsAgent,
                focus: composer.focus,
                accessibilityPrefix: "shell-prompt",
                onSendFrame: startsAgent ? { (frame: CGRect) in
                    if composer.isOpen {
                        ShellLaunchFlight.shared.source = frame
                    }
                } : nil,
                onClose: { composer.close() })
                .onGeometryChange(for: CGRect.self, of: { $0.frame(in: .global) }) { (frame: CGRect) in
                    if composer.isOpen && startsAgent {
                        ShellLaunchFlight.shared.card = frame
                    }
                }
                .padding(.horizontal, FmSpace.x3)
                .padding(.bottom, FmSpace.x2)
        }
        .opacity(composer.isOpen ? 1 : 0)
        .allowsHitTesting(composer.isOpen)
        .accessibilityHidden(!composer.isOpen)
        .onChange(of: composer.isOpen) { (_: Bool, open: Bool) in
            if open && !composer.focus.isFocused {
                composer.focus.focus()
            }
        }
        .accessibilityAction(.escape) { composer.close() }
    }

    private var modeOptions: [PromptModeOption] {
        guard startsAgent, launcher.takesMode else { return [] }
        return AgentPermissionMode.allCases.map { (mode: AgentPermissionMode) in
            PromptModeOption(id: mode.rawValue, label: mode.label, note: mode.note, systemImage: mode.systemImage)
        }
    }

    private var slashCommands: [PromptSlashCommand] {
        let diagnostics = PromptSlashCommand(
            name: "diagnostics", description: "Add the app's performance report for an agent to fix", run: onDiagnostics)
        if target != nil {
            return [
                PromptSlashCommand(name: "agent", description: "Start an agent instead") {
                    model.targets.bypassed = true
                },
                diagnostics,
            ]
        }
        return [
            PromptSlashCommand(name: "where", description: "Choose the repository and where it runs", run: onPickContext),
            diagnostics,
        ]
    }

    /// The page's own target or Agent, and it flips.
    private var modePill: some View {
        Button {
            FmHaptics.selection()
            model.togglePromptMode()
            composer.clearProblem()
        } label: {
            HStack(spacing: FmSpace.x1) {
                Image(systemName: target != nil ? "fork.knife" : "sparkles")
                    .font(.subheadline.weight(.semibold))
                Text(target?.label ?? "Agent")
                    .fmText(.labelLarge, color: fm.accent)
            }
            .foregroundStyle(fm.accent)
            .padding(.horizontal, FmSpace.x3)
            .frame(minHeight: 44)
            .background(Capsule().fill(fm.accentSubtle))
            .contentShape(Capsule())
        }
        .buttonStyle(.plain)
        .accessibilityLabel(Text(model.promptModeLabel))
        .accessibilityIdentifier("shell-prompt-mode")
    }

    private var runOptions: some View {
        HStack(spacing: FmSpace.x2) {
            runOptionToggle(
                title: "YOLO", systemImage: "bolt.fill", on: launcher.runOptions.yolo,
                enabled: launcher.runOptions.worktree || !launcher.takesNoWorktree,
                hint: "When the run finishes, merge it into the default branch, push, and delete the agent, its branch and worktree",
                identifier: "shell-prompt-yolo"
            ) {
                launcher.toggleYolo()
            }
            if launcher.takesNoWorktree {
                runOptionToggle(
                    title: "No worktree", systemImage: "folder.badge.minus", on: !launcher.runOptions.worktree,
                    enabled: true,
                    hint: "The next run works straight in the repository folder on the desktop, with no branch or worktree",
                    identifier: "shell-prompt-no-worktree"
                ) {
                    launcher.toggleWorktree()
                }
            }
            Spacer(minLength: 0)
        }
    }

    private func runOptionToggle(
        title: String, systemImage: String, on: Bool, enabled: Bool, hint: String, identifier: String,
        action: @escaping () -> Void
    ) -> some View {
        let active: Bool = on && enabled
        return Button {
            FmHaptics.selection()
            action()
        } label: {
            HStack(spacing: 3) {
                Image(systemName: systemImage)
                    .font(.caption2.weight(.semibold))
                Text(title)
                    .fmText(.labelSmall, color: active ? fm.accent : fm.textTertiary)
            }
            .foregroundStyle(active ? fm.accent : fm.textTertiary)
            .padding(.horizontal, FmSpace.x2)
            .frame(minHeight: 22)
            .background(Capsule().fill(active ? fm.accentSubtle : Color.clear))
            .overlay(Capsule().strokeBorder(active ? Color.clear : fm.textTertiary.opacity(0.35), lineWidth: 1))
            .contentShape(Capsule())
            .opacity(enabled ? 1 : 0.5)
        }
        .buttonStyle(.plain)
        .accessibilityLabel(Text(title))
        .accessibilityValue(Text(on ? "On" : "Off"))
        .accessibilityHint(Text(hint))
        .accessibilityAddTraits(on ? [.isSelected] : [])
        .accessibilityIdentifier(identifier)
    }

    private var assigneeMenu: some View {
        Menu {
            ForEach(launcher.taskAssignees, id: \.self) { (name: String) in
                Button {
                    FmHaptics.selection()
                    launcher.choose(assignee: name)
                } label: {
                    if name == launcher.taskAssignee {
                        Label(launcher.taskAssigneeLabel(name), systemImage: "checkmark")
                    } else {
                        Text(launcher.taskAssigneeLabel(name))
                    }
                }
            }
        } label: {
            HStack(spacing: FmSpace.x1) {
                Image(systemName: "person.crop.circle")
                    .font(.footnote)
                Text("Assign to \(launcher.taskAssigneeLabel(launcher.taskAssignee))")
                    .fmText(.bodySmall, color: fm.accent)
                    .lineLimit(1)
                Image(systemName: "chevron.down")
                    .font(.caption2.weight(.semibold))
            }
            .foregroundStyle(fm.accent)
            .frame(minHeight: 32)
            .contentShape(Rectangle())
        }
        .task { if launcher.taskPeople == nil { await launcher.loadTaskPeople() } }
        .accessibilityLabel(Text("Assign the task to \(launcher.taskAssigneeLabel(launcher.taskAssignee))"))
        .accessibilityIdentifier("shell-prompt-assignee")
    }

    /// The run's context ("forkmesh/forkmesh · mirror35"), tappable; and when
    /// the last send could not run, why and the one tap that fixes it.
    @ViewBuilder private var header: some View {
        VStack(alignment: .leading, spacing: FmSpace.x2) {
            if let target = target {
                Text(target.title)
                    .fmText(.bodySmall, color: fm.textSecondary)
                    .lineLimit(1)
            } else {
                if launcher.takesRunOptions {
                    runOptions
                }
                if launcher.action == .task {
                    assigneeMenu
                }
                Button {
                    FmHaptics.tap()
                    onPickContext()
                } label: {
                    HStack(spacing: FmSpace.x1) {
                        Image(systemName: "folder")
                            .font(.footnote)
                        Text(launcher.contextLine)
                            .fmText(.bodySmall, color: fm.textSecondary)
                            .lineLimit(1)
                            .truncationMode(.middle)
                        Image(systemName: "chevron.right")
                            .font(.caption2.weight(.semibold))
                    }
                    .foregroundStyle(fm.textSecondary)
                    .frame(minHeight: 32)
                    .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
                .accessibilityLabel(Text(launcher.action == nil ? "Runs in \(launcher.contextLine)" : launcher.contextLine))
                .accessibilityHint(Text("Choose the repository, where it runs, and the model"))
                .accessibilityIdentifier("shell-prompt-context")
            }
            if let message = composer.message {
                HStack(alignment: .firstTextBaseline, spacing: FmSpace.x2) {
                    Text(message)
                        .fmText(.bodySmall, color: composer.fix == nil ? fm.danger : fm.textPrimary)
                        .fixedSize(horizontal: false, vertical: true)
                        .frame(maxWidth: .infinity, alignment: .leading)
                    FmMessageTools(message)
                    if let fix = composer.fix {
                        Button {
                            FmHaptics.tap()
                            onFix(fix)
                        } label: {
                            Text(fix.action)
                                .fmText(.labelLarge, color: fm.accent)
                                .multilineTextAlignment(.trailing)
                        }
                        .buttonStyle(.plain)
                        .accessibilityIdentifier("shell-prompt-fix")
                    }
                }
            }
        }
    }
}

/// The context picker: repository, where it runs, and the model. Every choice
/// applies at once and is remembered per repository; nothing here blocks a
/// send.
struct QuickLaunchPickerSheet: View {
    @Environment(\.fmPalette) var fm
    @Environment(\.dismiss) var dismiss
    let launcher: QuickAgentLauncher

    var body: some View {
        NavigationStack {
            List {
                if launcher.action == .task {
                    Section("Assign to") {
                        if launcher.loadingTaskPeople && launcher.taskPeople == nil {
                            Text("Loading members…")
                                .fmText(.bodySmall, color: fm.textSecondary)
                        }
                        ForEach(launcher.taskAssignees, id: \.self) { (name: String) in
                            row(title: launcher.taskAssigneeLabel(name), detail: name.isEmpty ? "On the General board" : "",
                                selected: name == launcher.taskAssignee) {
                                launcher.choose(assignee: name)
                            }
                        }
                    }
                }
                if launcher.action == nil {
                    Section("Runs on") {
                        if launcher.destinations.isEmpty {
                            Text(emptyDestinations)
                                .fmText(.bodySmall, color: fm.textSecondary)
                        }
                        ForEach(launcher.destinations, id: \.self) { (destination: QuickLaunchDestination) in
                            row(
                                title: title(destination), detail: launcher.detail(destination),
                                selected: destination == launcher.destination
                            ) {
                                launcher.choose(destination)
                            }
                        }
                    }
                }
                Section("Model") {
                    ForEach(launcher.modelChoices) { (choice: AgentModelChoice) in
                        row(title: choice.label, detail: choice.note, selected: choice == launcher.chip) {
                            launcher.choose(model: choice)
                        }
                    }
                }
                if launcher.takesMode {
                    Section("Mode") {
                        ForEach(AgentPermissionMode.allCases) { (mode: AgentPermissionMode) in
                            row(title: mode.label, detail: mode.note, selected: mode == launcher.mode) {
                                launcher.choose(mode: mode)
                            }
                        }
                    }
                }
                if launcher.offersBaseBranch {
                    Section("Base branch") {
                        ForEach(launcher.baseBranches, id: \.self) { (name: String) in
                            row(
                                title: name, detail: name == launcher.form.defaultBranch ? "Default branch" : "",
                                selected: name == launcher.baseBranch
                            ) {
                                launcher.choose(baseBranch: name)
                            }
                        }
                    }
                }
                if launcher.action == nil || launcher.action?.needsRepository == true {
                    Section("Repository") {
                        if (launcher.form.targets ?? []).isEmpty {
                            Text(launcher.preparing || launcher.form.targets == nil
                                ? "Loading repositories…" : "No repositories could be loaded. Check the connection and open this again.")
                                .fmText(.bodySmall, color: fm.textSecondary)
                                .fmMessageTools(launcher.preparing || launcher.form.targets == nil
                                    ? nil : "No repositories could be loaded. Check the connection and open this again.")
                        }
                        ForEach(launcher.form.targets ?? []) { (target: AgentStartTarget) in
                            row(title: target.label, detail: target.detail, selected: target == launcher.repository) {
                                Task { await launcher.choose(target) }
                            }
                        }
                    }
                }
            }
            .navigationTitle(launcher.action == nil ? "Where it runs" : "Send as")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button("Done") { dismiss() }
                }
            }
        }
        .presentationDetents([.medium, .large])
        .task { await launcher.prepare() }
    }

    private var emptyDestinations: String {
        if launcher.repository == nil {
            return launcher.preparing ? "Finding a repository…" : "Choose a repository below."
        }
        return launcher.form.plan?.loading ?? false ? "Checking…" : "Nothing can run this repository yet."
    }

    private func title(_ destination: QuickLaunchDestination) -> String {
        switch destination {
        case .node(let name): return name
        case .device: return "This iPhone"
        case .desktop(let name): return "\(name) (desktop)"
        }
    }

    private func row(title: String, detail: String, selected: Bool, action: @escaping () -> Void) -> some View {
        Button {
            FmHaptics.selection()
            action()
        } label: {
            HStack {
                VStack(alignment: .leading, spacing: 2) {
                    Text(title).fmText(.bodyMedium)
                    if !detail.isEmpty {
                        Text(detail).fmText(.bodySmall, color: fm.textSecondary)
                    }
                }
                Spacer(minLength: FmSpace.x2)
                if selected {
                    Image(systemName: "checkmark")
                        .foregroundStyle(fm.accent)
                        .accessibilityHidden(true)
                }
            }
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .accessibilityAddTraits(selected ? [.isSelected] : [])
    }
}

/// `FocusManager.instance.primaryFocus?.unfocus()`: drops the keyboard
/// wherever it is, before a page change or a sheet.
enum ShellKeyboard {
    @MainActor
    static func dismiss() {
        UIApplication.shared.sendAction(#selector(UIResponder.resignFirstResponder), to: nil, from: nil, for: nil)
    }
}
