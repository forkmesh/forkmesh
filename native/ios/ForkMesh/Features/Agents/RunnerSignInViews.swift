import FMApi
import FMBase
import SwiftUI
import UIKit

struct RunnerAttentionBanner: View {
    @Environment(\.fmPalette) private var fm
    let model: RunnerAttentionModel
    var compact: Bool = false

    private var tint: Color { model.anyExpired ? fm.danger : fm.warning }

    var body: some View {
        if model.visible {
            content
                .accessibilityIdentifier("runner-signin-banner")
        }
    }

    private var content: some View {
        VStack(alignment: .leading, spacing: FmSpace.x2) {
            if compact, let first = model.items.first {
                HStack(spacing: FmSpace.x2) {
                    Image(systemName: "exclamationmark.triangle.fill")
                        .foregroundStyle(tint)
                    Text(model.title)
                        .fmText(.labelMedium, color: tint)
                        .lineLimit(1)
                    Spacer(minLength: FmSpace.x2)
                    FmToneButton(
                        label: model.items.count == 1 ? first.buttonLabel : "Sign in",
                        tone: model.anyExpired ? .danger : .warning, filled: true, height: 28,
                        onPressed: { model.open(first) })
                        .accessibilityIdentifier("runner-signin-chip-button")
                }
            } else {
                HStack(spacing: FmSpace.x2) {
                    Image(systemName: "exclamationmark.triangle.fill")
                        .foregroundStyle(tint)
                    Text(model.title)
                        .fmText(.titleSmall, color: tint)
                        .fmMessageTools(model.title)
                }
                ForEach(model.items) { (item: RunnerAttentionItem) in
                    row(item)
                }
            }
        }
        .padding(compact ? EdgeInsets(top: 6, leading: 12, bottom: 6, trailing: 8) : EdgeInsets.fmAll(FmSpace.x3))
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(RoundedRectangle(cornerRadius: FmRadius.md).fill(tint.opacity(0.12)))
        .overlay(RoundedRectangle(cornerRadius: FmRadius.md).stroke(tint.opacity(0.45), lineWidth: 1))
    }

    private func row(_ item: RunnerAttentionItem) -> some View {
        let color: Color = item.isExpired ? fm.danger : fm.warning
        return HStack(alignment: .center, spacing: FmSpace.x2) {
            VStack(alignment: .leading, spacing: 2) {
                if model.items.count > 1 {
                    Text(item.title).fmText(.labelMedium, color: color)
                }
                Text(item.detail()).fmText(.bodySmall, color: fm.textSecondary)
            }
            Spacer(minLength: FmSpace.x2)
            FmToneButton(
                label: item.buttonLabel, tone: item.isExpired ? .danger : .warning, filled: true,
                onPressed: { model.open(item) })
                .accessibilityIdentifier("runner-signin-\(item.id)")
        }
    }
}

struct RunnerSignInSheet: View {
    @Environment(\.fmPalette) private var fm
    @Environment(\.openURL) private var openURL
    @Environment(\.dismiss) private var dismiss
    @State private var model: RunnerLoginModel

    init(model: RunnerLoginModel) {
        _model = State(initialValue: model)
    }

    var body: some View {
        NavigationStack {
            ScrollView {
                VStack(alignment: .leading, spacing: FmSpace.x3) {
                    Text(model.status.isEmpty ? "Starting the sign-in…" : model.status)
                        .fmText(.bodyMedium, color: model.failed ? fm.danger : fm.textSecondary)
                        .accessibilityIdentifier("runner-signin-status")
                        .fmMessageTools(model.status)
                    if let fallback = model.fallbackURL {
                        FmFilledButton(
                            "Open Runners on the website", icon: "open_in_new", fillWidth: true,
                            onPressed: { openURL(fallback) })
                    }
                    ForEach(model.urls, id: \.self) { (url: URL) in
                        FmFilledButton(
                            "Open \(url.host ?? "sign-in page")", icon: "open_in_new", fillWidth: true,
                            onPressed: { openURL(url) })
                    }
                    if !model.text.isEmpty {
                        Text(model.text)
                            .font(.system(size: 12, design: .monospaced))
                            .foregroundStyle(fm.textPrimary)
                            .textSelection(.enabled)
                            .frame(maxWidth: .infinity, alignment: .leading)
                            .padding(FmSpace.x2)
                            .background(RoundedRectangle(cornerRadius: FmRadius.sm).fill(fm.bgRaised))
                    }
                    if model.open {
                        codeField
                    }
                }
                .padding(FmSpace.x4)
            }
            .fmPreGlassBackground(fm.bgBase)
            .navigationTitle(model.request.title)
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button(model.open ? "Cancel sign-in" : "Done") {
                        Task {
                            await model.close()
                            dismiss()
                        }
                    }
                }
            }
        }
        .task { await model.run() }
        .onDisappear {
            let current: RunnerLoginModel = model
            Task { await current.close() }
        }
    }

    private var sendAction: (() -> Void)? {
        guard model.canSubmit else { return nil }
        let current: RunnerLoginModel = model
        return { Task { await current.submitCode() } }
    }

    private var codeField: some View {
        VStack(alignment: .leading, spacing: FmSpace.x2) {
            Text("Paste the code from the sign-in page").fmText(.labelMedium)
            HStack(spacing: FmSpace.x2) {
                TextField("Code", text: $model.code)
                    .textInputAutocapitalization(.never)
                    .autocorrectionDisabled()
                    .textFieldStyle(.roundedBorder)
                    .onSubmit { Task { await model.submitCode() } }
                    .accessibilityIdentifier("runner-signin-code")
                FmToneButton(
                    label: "Send", tone: .accent, filled: true,
                    onPressed: sendAction)
            }
            if !model.inputError.isEmpty {
                Text(model.inputError).fmText(.bodySmall, color: fm.danger)
                    .fmMessageTools(model.inputError)
            }
        }
    }
}

struct RunnerSignInPresenter: ViewModifier {
    @Environment(\.openURL) private var openURL
    @Environment(\.scenePhase) private var scenePhase
    let model: RunnerAttentionModel?

    func body(content: Content) -> some View {
        if let model = model {
            content
                .sheet(item: Binding(get: { model.request }, set: { model.request = $0 })) {
                    (request: RunnerSignInRequest) in
                    RunnerSignInSheet(model: model.loginModel(for: request))
                }
                .onChange(of: model.fallbackURL) { (_: URL?, next: URL?) in
                    if let next = next {
                        model.fallbackURL = nil
                        openURL(next)
                    }
                }
                .onChange(of: scenePhase) { (_: ScenePhase, phase: ScenePhase) in
                    if phase == .active {
                        Task { await model.refresh() }
                    }
                }
                .task { await model.refresh() }
        } else {
            content
        }
    }
}

extension View {
    func runnerSignInPresenter(_ model: RunnerAttentionModel?) -> some View {
        modifier(RunnerSignInPresenter(model: model))
    }
}
