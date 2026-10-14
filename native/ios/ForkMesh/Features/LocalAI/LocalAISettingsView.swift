// Settings → On-device AI: the switch, the Gemma 4 models (download progress,
// delete, load), and what they cost in storage and memory. Binds to
// FMLocalAI's LocalModelManager. New for the native app.

import FMBase
import FMLocalAI
import SwiftUI
import UIKit

@MainActor
struct LocalAISettingsView: View {
    @Environment(\.fmPalette) private var fm
    @State private var manager: LocalModelManager

    init() {
        _manager = State(initialValue: LocalAIFeature.manager())
    }

    init(manager: LocalModelManager) {
        _manager = State(initialValue: manager)
    }

    private var anyDownloading: Bool {
        manager.catalog.contains { manager.state(for: $0).isDownloading }
    }

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: FmSpace.x4) {
                OnDeviceChatEntryCard(manager: manager)
                LocalAIIntroCard(manager: manager)
                FmSectionHeader(title: "Models", icon: "memory_outlined")
                ForEach(manager.catalog) { model in
                    LocalModelCard(manager: manager, model: model)
                }
                FmSectionHeader(title: "This device", icon: "phone_iphone")
                LocalAIDeviceCard(manager: manager)
            }
            .padding(FmSpace.x4)
        }
        .background(fm.canvas)
        .navigationTitle("On-device AI")
        .navigationBarTitleDisplayMode(.inline)
        .onAppear {
            manager.updateDevice(LocalAIFeature.currentDevice())
            manager.refresh()
        }
        .onChange(of: anyDownloading) { _, downloading in
            // A download stops when the phone locks; keep the screen on while
            // this screen shows one.
            UIApplication.shared.isIdleTimerDisabled = downloading
        }
        .onDisappear {
            UIApplication.shared.isIdleTimerDisabled = false
        }
    }
}

@MainActor
struct OnDeviceChatEntryCard: View {
    @Environment(\.fmPalette) private var fm
    let manager: LocalModelManager

    var body: some View {
        let access: OnDeviceChatAccess = manager.chatAccess
        VStack(alignment: .leading, spacing: FmSpace.x3) {
            if access.isEnabled {
                NavigationLink {
                    OnDeviceChatView(manager: manager)
                        .shellPageClearance()
                } label: {
                    row(access, enabled: true)
                }
                .buttonStyle(.plain)
                .accessibilityIdentifier("localai-chat-open")
            } else {
                row(access, enabled: false)
                    .accessibilityElement(children: .combine)
                    .accessibilityAddTraits(.isButton)
                    .accessibilityHint(Text(access.reason ?? ""))
                    .accessibilityIdentifier("localai-chat-open")
            }
            if let reason = access.reason {
                let stuck: Bool = !access.isEnabled && access.action == nil
                LocalAINotice(
                    icon: stuck ? "warning_amber_outlined" : "info_outline",
                    text: reason,
                    color: stuck ? fm.warning : fm.textSecondary
                )
            }
            if let progress = access.progress {
                FmLinearProgressIndicator(value: progress)
            }
            if let action = access.action {
                HStack(spacing: FmSpace.x2) {
                    FmToneButton(
                        label: action.label,
                        icon: icon(for: action),
                        tone: action == .cancelDownload ? .neutral : .accent,
                        filled: action != .cancelDownload
                    ) {
                        perform(action)
                    }
                    .accessibilityIdentifier("localai-chat-fix")
                    Spacer(minLength: 0)
                }
            }
        }
        .padding(FmSpace.x4)
        .background(fm.bgRaised)
        .clipShape(FmRoundedShape(radius: FmRadius.md))
    }

    private func row(_ access: OnDeviceChatAccess, enabled: Bool) -> some View {
        HStack(spacing: FmSpace.x3) {
            FmIcon("chat_bubble_outline_rounded", size: 20, color: enabled ? fm.accent : fm.textTertiary)
            Text(access.title)
                .fmText(.labelLarge, color: enabled ? fm.accent : fm.textTertiary)
            Spacer(minLength: 0)
            FmIcon("chevron_right", size: 20, color: enabled ? fm.textTertiary : fm.textTertiary.opacity(0.4))
        }
        .contentShape(Rectangle())
    }

    private func icon(for action: OnDeviceChatAccess.Action) -> String {
        switch action {
        case .turnOn: return "play_arrow_rounded"
        case .download, .resumeDownload: return "download_outlined"
        case .cancelDownload: return "close"
        }
    }

    private func perform(_ action: OnDeviceChatAccess.Action) {
        if action != .cancelDownload {
            manager.updateDevice(LocalAIFeature.currentDevice())
        }
        manager.performChatAccess(action)
    }
}

/// The switch and what it means.
@MainActor
struct LocalAIIntroCard: View {
    @Environment(\.fmPalette) private var fm
    let manager: LocalModelManager

    private var enabled: Binding<Bool> {
        Binding<Bool>(
            get: { manager.isEnabled },
            set: { manager.setEnabled($0) }
        )
    }

    var body: some View {
        FmCard {
            VStack(alignment: .leading, spacing: FmSpace.x3) {
                Toggle(isOn: enabled) {
                    Text("On-device AI")
                        .fmText(.titleSmall)
                }
                Text("Runs Google's Gemma 4 on this phone with Apple's MLX. What you type never leaves the device, and it works offline once a model is downloaded.")
                    .fmText(.bodyMedium, color: fm.textSecondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
        }
    }
}

/// One catalog model: size, fit, state and its actions.
@MainActor
struct LocalModelCard: View {
    @Environment(\.fmPalette) private var fm
    let manager: LocalModelManager
    let model: LocalAIModel
    @State private var confirmDelete = false

    private var state: LocalModelState { manager.state(for: model) }
    private var fit: LocalModelFit { manager.fit(for: model) }
    private var isSelected: Bool { manager.selectedModelId == model.id }

    var body: some View {
        FmCard {
            VStack(alignment: .leading, spacing: FmSpace.x3) {
                header
                Text(model.summary)
                    .fmText(.bodyMedium, color: fm.textSecondary)
                    .fixedSize(horizontal: false, vertical: true)
                Text(facts)
                    .fmMono(color: fm.textTertiary)
                if let note = fit.message {
                    LocalAINotice(icon: fit.canRun ? "info_outline" : "error_outline", text: note, color: fit.canRun ? fm.textSecondary : fm.danger)
                }
                LocalModelStateView(manager: manager, model: model)
                actions
            }
        }
        .confirmationDialog("Delete \(model.displayName)?", isPresented: $confirmDelete, titleVisibility: .visible) {
            Button("Delete", role: .destructive) {
                Task { await manager.delete(model) }
            }
            Button("Cancel", role: .cancel) {}
        } message: {
            Text("This frees \(LocalAIFormat.bytes(manager.diskUsage[model.id] ?? 0)). Downloading it again takes a while.")
        }
    }

    private var facts: String {
        "\(LocalAIFormat.bytes(model.approxDownloadBytes)) download · about \(LocalAIFormat.bytes(model.approxMemoryBytes)) in memory · \(model.contextLength / 1024)K context"
    }

    private var header: some View {
        HStack(spacing: FmSpace.x2) {
            Button(action: { manager.select(model) }) {
                FmIcon(isSelected ? "radio_button_checked" : "radio_button_unchecked", size: 22, color: isSelected ? fm.accent : fm.textTertiary)
            }
            .buttonStyle(.plain)
            .accessibilityLabel(Text(isSelected ? "Selected" : "Use \(model.displayName)"))
            Text(model.displayName)
                .fmText(.titleSmall)
            if manager.recommendedModel?.id == model.id {
                FmStatusBadge(label: "Recommended", color: fm.success)
            }
            Spacer(minLength: 0)
        }
    }

    @ViewBuilder private var actions: some View {
        HStack(spacing: FmSpace.x2) {
            switch state {
            case .notDownloaded:
                downloadButton
            case .failed:
                if manager.isDownloaded(model) {
                    FmToneButton(label: "Load", icon: "play_arrow_rounded", tone: .accent, filled: true, onPressed: loadAction)
                    deleteButton
                } else {
                    downloadButton
                }
            case .downloading:
                FmToneButton(label: "Cancel", icon: "close", tone: .neutral) {
                    manager.cancelDownload(model)
                }
            case .downloaded:
                FmToneButton(label: "Load", icon: "play_arrow_rounded", tone: .accent, filled: true, onPressed: loadAction)
                deleteButton
            case .loading:
                FmCircularProgressIndicator(size: 20, strokeWidth: 2)
            case .ready:
                FmToneButton(label: "Unload", icon: "pause_rounded", tone: .neutral) {
                    Task { await manager.unload() }
                }
                deleteButton
            }
            Spacer(minLength: 0)
        }
    }

    private var loadAction: (() -> Void)? {
        guard manager.engineAvailability.isAvailable else { return nil }
        return {
            manager.updateDevice(LocalAIFeature.currentDevice())
            Task { await manager.load(model) }
        }
    }

    @ViewBuilder private var downloadButton: some View {
        let resumable: Int64 = manager.resumableBytes[model.id] ?? 0
        let label: String = resumable > 0 ? "Resume download" : "Download"
        let action: (() -> Void)? = manager.canDownload(model) ? { manager.download(model) } : nil
        FmToneButton(label: label, icon: "download_outlined", tone: .accent, filled: true, onPressed: action)
        if resumable > 0 {
            deleteButton
        }
    }

    private var deleteButton: some View {
        FmToneButton(label: "Delete", icon: "delete_outline", tone: .danger) {
            confirmDelete = true
        }
    }
}

/// Progress, or the state in words.
@MainActor
struct LocalModelStateView: View {
    @Environment(\.fmPalette) private var fm
    let manager: LocalModelManager
    let model: LocalAIModel

    var body: some View {
        switch manager.state(for: model) {
        case .notDownloaded:
            notDownloaded
        case .downloading(let progress, let bytes):
            downloading(progress: progress, bytes: bytes)
        case .downloaded:
            line("Downloaded · \(LocalAIFormat.bytes(manager.diskUsage[model.id] ?? 0)) on this phone", color: fm.textSecondary)
        case .loading:
            line("Loading into memory…", color: fm.textSecondary)
        case .ready:
            line("Ready · in memory", color: fm.success)
        case .failed(let message):
            LocalAINotice(icon: "error_outline", text: message, color: fm.danger)
        }
    }

    @ViewBuilder private var notDownloaded: some View {
        let resumable: Int64 = manager.resumableBytes[model.id] ?? 0
        if resumable > 0 {
            line("\(LocalAIFormat.bytes(resumable)) downloaded so far; it resumes where it stopped.", color: fm.textSecondary)
        } else {
            line("Not downloaded. Use Wi-Fi: it is a large download.", color: fm.textTertiary)
        }
    }

    private func downloading(progress: Double, bytes: Int64) -> some View {
        let total: Int64 = manager.downloadProgress[model.id]?.totalBytes ?? model.approxDownloadBytes
        let file: String = manager.downloadProgress[model.id]?.currentFile ?? ""
        return VStack(alignment: .leading, spacing: FmSpace.x2) {
            FmLinearProgressIndicator(value: progress)
            HStack {
                Text("\(LocalAIFormat.percent(progress)) · \(LocalAIFormat.bytes(bytes)) of \(LocalAIFormat.bytes(total))")
                    .fmText(.bodySmall, tabular: true)
                Spacer(minLength: FmSpace.x2)
                Text(file)
                    .fmMono(color: fm.textTertiary)
                    .lineLimit(1)
                    .truncationMode(.middle)
            }
            Text("Keep ForkMesh open until it finishes; if it stops, it resumes where it left off.")
                .fmText(.bodySmall, color: fm.textTertiary)
                .fixedSize(horizontal: false, vertical: true)
        }
    }

    private func line(_ text: String, color: Color) -> some View {
        Text(text)
            .fmText(.bodySmall, color: color)
            .fixedSize(horizontal: false, vertical: true)
            .frame(maxWidth: .infinity, alignment: .leading)
            .fmMessageTools(text)
    }
}

/// Memory and storage, and which model suits this device.
@MainActor
struct LocalAIDeviceCard: View {
    @Environment(\.fmPalette) private var fm
    let manager: LocalModelManager

    private var recommendation: String {
        if let reason = manager.engineAvailability.reason { return reason }
        if let model = manager.recommendedModel { return model.displayName }
        return "None: this device has too little memory for Gemma 4."
    }

    var body: some View {
        FmCard {
            VStack(alignment: .leading, spacing: 0) {
                FmKeyValueRow(label: "Memory", value: LocalAIFormat.memory(manager.device.physicalMemory))
                if let available = manager.device.availableMemory {
                    FmKeyValueRow(label: "Free now", value: LocalAIFormat.bytes(Int64(clamping: available)))
                }
                FmKeyValueRow(label: "Models", value: LocalAIFormat.bytes(manager.totalDiskUsage))
                if let free = manager.availableDiskBytes {
                    FmKeyValueRow(label: "Storage free", value: LocalAIFormat.bytes(free))
                }
                FmKeyValueRow(label: "Suggested", value: recommendation)
            }
        }
    }
}

/// An icon and a sentence, tinted.
struct LocalAINotice: View {
    let icon: String
    let text: String
    let color: Color

    var body: some View {
        HStack(alignment: .top, spacing: FmSpace.x2) {
            FmIcon(icon, size: 16, color: color)
            Text(text)
                .fmText(.bodySmall, color: color)
                .fixedSize(horizontal: false, vertical: true)
            Spacer(minLength: 0)
            FmMessageTools(text)
        }
    }
}
