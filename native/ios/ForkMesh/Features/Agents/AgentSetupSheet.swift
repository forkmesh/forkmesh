// The agent screen's sheets: AgentSetupSheet (provider, key and model; Gemma 4
// on this iPhone is the third provider and takes no key, only a downloaded
// model), PullRequestSheet and AgentAttachFileSheet. Logic: FMAgents'
// AgentSetupModel and AttachFileModel.

import FMAgents
import FMBase
import FMCode
import FMLocalAI
import SwiftUI

struct AgentSetupSheet: View {
    @Environment(\.fmPalette) private var fm
    @Environment(\.dismiss) private var dismiss
    @State private var model: AgentSetupModel
    @State private var showLocalAI: Bool = false
    private let onSaved: (AgentCredentials?) -> Void

    /// `onSaved` receives what was stored, or nil when the key was removed.
    init(initial: AgentCredentials?, onSaved: @escaping (AgentCredentials?) -> Void) {
        _model = State(initialValue: AgentSetupModel(
            store: AgentsFeature.credentialStore(),
            initial: initial,
            localModels: { AgentsFeature.localModels() }
        ))
        self.onSaved = onSaved
    }

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 0) {
                Text("Agent model").fmText(.titleMedium)
                Text(model.kind.needsKey
                    ? "Runs on your own account. The key stays on this device."
                    : "Runs on this iPhone with Apple's MLX. Nothing leaves the device.")
                    .fmText(.bodySmall, color: fm.textSecondary)
                    .padding(.top, FmSpace.x1)
                providerTiles.padding(.top, FmSpace.x4)
                keyOrReadiness.padding(.top, FmSpace.x4)
                modelList.padding(.top, FmSpace.x3)
                if let problem = model.problem {
                    Text(problem)
                        .fmText(.bodySmall, color: fm.danger, height: 1.4)
                        .fixedSize(horizontal: false, vertical: true)
                        .fmMessageTools(problem)
                        .padding(.top, FmSpace.x3)
                }
                actions.padding(.top, FmSpace.x4)
            }
            .padding(FmSpace.x4)
        }
        .sheet(isPresented: $showLocalAI) {
            NavigationStack {
                LocalAISettingsView()
            }
        }
    }

    private var providerTiles: some View {
        HStack(spacing: FmSpace.x2) {
            ForEach(AgentProviderKind.allCases, id: \.self) { (kind: AgentProviderKind) in
                AgentProviderTile(label: kind == .onDevice ? kind.pickerLabel : kind.label, selected: model.kind == kind) {
                    model.selectKind(kind)
                }
            }
        }
    }

    @ViewBuilder private var keyOrReadiness: some View {
        if model.kind.needsKey {
            FmField(label: model.keyFieldLabel) {
                FmTextField(text: $model.key, secure: true, mono: true)
            }
        } else if let readiness = model.readiness {
            VStack(alignment: .leading, spacing: FmSpace.x2) {
                AgentNote(
                    text: readiness.message,
                    color: readiness.isReady ? fm.success : fm.warning,
                    icon: readiness.isReady ? "check_circle_outline" : "memory_outlined"
                )
                FmToneButton(label: "On-device AI settings", icon: "phone_iphone", tone: .accent) {
                    showLocalAI = true
                }
            }
        }
    }

    private var modelList: some View {
        FmField(label: "Model") {
            VStack(alignment: .leading, spacing: 0) {
                ForEach(model.models, id: \.id) { (option: AgentModelOption) in
                    AgentModelRow(option: option, selected: option.id == model.model) {
                        model.selectModel(option.id)
                    }
                }
            }
        }
    }

    private var actions: some View {
        HStack(spacing: FmSpace.x2) {
            if model.initial != nil {
                FmToneButton(label: model.initial?.kind.needsKey == false ? "Remove" : "Remove key", tone: .danger) {
                    model.remove()
                    onSaved(nil)
                    dismiss()
                }
            }
            Spacer(minLength: 0)
            FmToneButton(label: model.saveLabel, tone: .accent, filled: true, onPressed: model.busy ? nil : { Task { await save() } })
        }
    }

    private func save() async {
        if let saved = await model.save() {
            onSaved(saved)
            dismiss()
        }
    }
}

struct AgentProviderTile: View {
    @Environment(\.fmPalette) private var fm
    let label: String
    let selected: Bool
    let onTap: () -> Void

    var body: some View {
        Button(action: onTap) {
            Text(label)
                .fmText(.labelMedium, color: selected ? fm.accent : fm.textPrimary)
                .multilineTextAlignment(.center)
                .frame(maxWidth: .infinity, minHeight: 44)
                .padding(.horizontal, FmSpace.x1)
                .background(selected ? fm.accent.opacity(0.12) : fm.bgOverlay)
                .clipShape(FmRoundedShape(radius: FmRadius.md))
                .overlay(RoundedRectangle(cornerRadius: FmRadius.md).stroke(selected ? fm.accent : fm.border, lineWidth: 1))
        }
        .buttonStyle(.plain)
    }
}

struct AgentModelRow: View {
    @Environment(\.fmPalette) private var fm
    let option: AgentModelOption
    let selected: Bool
    let onTap: () -> Void

    var body: some View {
        Button(action: onTap) {
            HStack(spacing: FmSpace.x3) {
                FmIcon(selected ? "radio_button_checked" : "radio_button_off", size: 18, color: selected ? fm.accent : fm.textTertiary)
                Text(option.label).fmText(.labelMedium)
                Text(option.note)
                    .fmText(.bodySmall, color: fm.textTertiary)
                    .lineLimit(1)
                Spacer(minLength: 0)
            }
            .padding(.vertical, FmSpace.x2)
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
    }
}

// MARK: - Pull request

/// Confirms a prepared pull request, or explains precisely why the branch
/// cannot become one.
struct AgentPullRequestSheet: View {
    @Environment(\.fmPalette) private var fm
    @Environment(\.dismiss) private var dismiss
    let preparation: PullRequestPreparation
    let onConfirm: (String) -> Void
    @State private var title: String

    init(preparation: PullRequestPreparation, suggestedTitle: String, onConfirm: @escaping (String) -> Void) {
        self.preparation = preparation
        self.onConfirm = onConfirm
        _title = State(initialValue: suggestedTitle)
    }

    var body: some View {
        VStack(alignment: .leading, spacing: FmSpace.x2) {
            Text(preparation.isReady ? "Create pull request" : "Not ready for a pull request")
                .fmText(.titleMedium)
            if preparation.isReady {
                ready
            } else {
                blocked
            }
        }
        .padding(FmSpace.x4)
    }

    private var blocked: some View {
        VStack(alignment: .leading, spacing: FmSpace.x4) {
            Text(preparation.message)
                .fmText(.bodyMedium, color: fm.textSecondary, height: 1.45)
                .fixedSize(horizontal: false, vertical: true)
            HStack {
                Spacer(minLength: 0)
                FmToneButton(label: "Close") { dismiss() }
            }
        }
    }

    private var ready: some View {
        VStack(alignment: .leading, spacing: FmSpace.x2) {
            Text(preparation.summary).fmText(.labelMedium, color: fm.textSecondary)
            ForEach(Array(preparation.changedPaths.prefix(6)), id: \.self) { (path: String) in
                Text(path)
                    .fmMono(color: fm.textTertiary)
                    .lineLimit(1)
                    .truncationMode(.middle)
            }
            if preparation.changedPaths.count > 6 {
                Text("… and \(preparation.changedPaths.count - 6) more")
                    .fmText(.bodySmall, color: fm.textTertiary)
            }
            FmTextField(text: $title, hint: "Pull request title")
                .padding(.top, FmSpace.x2)
            HStack(spacing: FmSpace.x2) {
                Spacer(minLength: 0)
                FmToneButton(label: "Cancel") { dismiss() }
                FmToneButton(label: "Push & open", tone: .accent, filled: true) {
                    onConfirm(title.dartTrimmed)
                    dismiss()
                }
            }
            .padding(.top, FmSpace.x3)
        }
    }
}

// MARK: - Reference a file

/// Lists the working tree so a path can be picked instead of typed.
struct AgentAttachFileSheet: View {
    @Environment(\.fmPalette) private var fm
    @Environment(\.dismiss) private var dismiss
    @State private var model: AttachFileModel?
    private let onPick: (String) -> Void

    init(root: URL?, onPick: @escaping (String) -> Void) {
        _model = State(initialValue: root.map { AttachFileModel(root: $0) })
        self.onPick = onPick
    }

    var body: some View {
        VStack(alignment: .leading, spacing: FmSpace.x3) {
            Text("Reference a file").fmText(.titleMedium)
            if let model = model {
                AgentAttachFileList(model: model) { (path: String) in
                    onPick(path)
                    dismiss()
                }
            } else {
                Text("This working tree has no files yet.")
                    .fmText(.bodyMedium, color: fm.textTertiary)
            }
        }
        .padding(FmSpace.x4)
    }
}

struct AgentAttachFileList: View {
    @Environment(\.fmPalette) private var fm
    @Bindable var model: AttachFileModel
    let onPick: (String) -> Void

    var body: some View {
        VStack(alignment: .leading, spacing: FmSpace.x3) {
            FmTextField(text: $model.query, hint: "Filter by path…", autofocus: true)
            if model.isLoading {
                FmSkeletonList(rows: 4, hasLeading: false, label: "Searching files")
            } else if model.matches.isEmpty {
                Text(model.emptyText)
                    .fmText(.bodyMedium, color: fm.textTertiary)
                    .padding(.vertical, FmSpace.x5)
            } else {
                list
            }
            if model.truncated {
                Text(model.truncatedText)
                    .fmText(.bodySmall, color: fm.textTertiary)
            }
        }
        .task { await model.scan() }
    }

    private var list: some View {
        ScrollView {
            LazyVStack(alignment: .leading, spacing: 0) {
                ForEach(model.matches, id: \.self) { (path: String) in
                    Button {
                        onPick(path)
                    } label: {
                        HStack(spacing: FmSpace.x3) {
                            FmIcon("description_outlined", size: 18, color: fm.textTertiary)
                            Text(path)
                                .fmMono(size: FmType.small, color: fm.textPrimary)
                                .lineLimit(1)
                                .truncationMode(.middle)
                            Spacer(minLength: 0)
                        }
                        .padding(.vertical, FmSpace.x2)
                        .contentShape(Rectangle())
                    }
                    .buttonStyle(.plain)
                }
            }
        }
    }
}
