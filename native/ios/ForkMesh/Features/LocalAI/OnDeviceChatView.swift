// A streaming chat with the phone's own Gemma 4, with its speed in tokens per
// second, a stop button and clear. Binds to FMLocalAI's OnDeviceChatModel.
// New for the native app.

import FMBase
import FMLocalAI
import SwiftUI
import UIKit

@MainActor
struct OnDeviceChatView: View {
    @Environment(\.fmPalette) private var fm
    @Environment(\.scenePhase) private var scenePhase
    @State private var chat: OnDeviceChatModel

    init() {
        _chat = State(initialValue: OnDeviceChatModel(manager: LocalAIFeature.manager(), toolbox: LocalAIFeature.toolbox()))
    }

    init(manager: LocalModelManager) {
        _chat = State(initialValue: OnDeviceChatModel(manager: manager, toolbox: LocalAIFeature.toolbox()))
    }

    var body: some View {
        VStack(spacing: 0) {
            OnDeviceChatStatusBar(chat: chat)
            transcript
            if !chat.manager.isUsable {
                OnDeviceChatSetupNote(manager: chat.manager)
            }
            if let approval = chat.pendingApproval {
                OnDeviceChatApprovalBar(approval: approval) { allowed in
                    FmHaptics.tap()
                    chat.resolveApproval(allowed)
                }
            }
            OnDeviceChatComposer(chat: chat)
        }
        .background(fm.canvas)
        .navigationTitle("On-device chat")
        .navigationBarTitleDisplayMode(.inline)
        .toolbar {
            ToolbarItem(placement: .topBarTrailing) {
                Button("Clear") { chat.clear() }
                    .disabled(chat.entries.isEmpty)
            }
        }
        .onChange(of: scenePhase) { _, phase in
            // The GPU is off limits in the background.
            if phase != .active {
                chat.stop()
            }
        }
    }

    @ViewBuilder private var transcript: some View {
        if chat.entries.isEmpty {
            FmEmptyState(
                icon: "smart_toy_outlined",
                title: "Private, on this phone",
                message: "Gemma 4 runs on this phone. It reads your ForkMesh tasks, notes, issues, chat, pings and calendar, manages your CRM, drafts emails, searches the web and works with your agents. Deleting and messaging or stopping agents wait for your approval. The first message loads the model, which takes a few seconds."
            )
        } else {
            OnDeviceChatTranscript(chat: chat)
        }
    }
}

@MainActor
struct OnDeviceChatStatusBar: View {
    @Environment(\.fmPalette) private var fm
    let chat: OnDeviceChatModel

    var body: some View {
        HStack(spacing: FmSpace.x2) {
            FmIcon("speed_rounded", size: 16, color: fm.textTertiary)
            Text(chat.statusLine)
                .fmText(.bodySmall, color: fm.textSecondary, tabular: true)
                .lineLimit(1)
                .fmMessageTools(chat.statusLine, alignment: .center)
            Spacer(minLength: FmSpace.x2)
            if let stats = chat.lastStats, !chat.isGenerating {
                Text("\(stats.generatedTokens) tokens · prompt \(stats.promptTokens)")
                    .fmMono(color: fm.textTertiary)
                    .lineLimit(1)
            }
        }
        .padding(.horizontal, FmSpace.x4)
        .padding(.vertical, FmSpace.x2)
    }
}

@MainActor
struct OnDeviceChatTranscript: View {
    let chat: OnDeviceChatModel

    private var lastText: String {
        chat.entries.last?.text ?? ""
    }

    var body: some View {
        ScrollViewReader { proxy in
            ScrollView {
                LazyVStack(alignment: .leading, spacing: FmSpace.x3) {
                    ForEach(chat.entries) { entry in
                        if let email = entry.email {
                            OnDeviceEmailDraftCard(draft: email)
                                .id(entry.id)
                        } else if entry.isToolResult {
                            OnDeviceChatToolRow(entry: entry)
                                .id(entry.id)
                        } else if entry.isToolRequest && entry.text.dartTrimmed.isEmpty {
                            EmptyView()
                        } else {
                            OnDeviceChatBubble(entry: entry)
                                .id(entry.id)
                        }
                    }
                }
                .padding(FmSpace.x4)
            }
            .onChange(of: lastText) { _, _ in
                scrollToEnd(proxy)
            }
            .onChange(of: chat.entries.count) { _, _ in
                scrollToEnd(proxy)
            }
        }
    }

    private func scrollToEnd(_ proxy: ScrollViewProxy) {
        guard let last = chat.entries.last else { return }
        proxy.scrollTo(last.id, anchor: .bottom)
    }
}

struct OnDeviceChatBubble: View {
    @Environment(\.fmPalette) private var fm
    let entry: OnDeviceChatEntry

    private var isUser: Bool { entry.role == .user }

    private var fill: Color {
        if entry.isError { return fm.danger.opacity(0.10) }
        return isUser ? fm.accentSubtle : fm.bgRaised
    }

    private var ink: Color {
        entry.isError ? fm.danger : fm.textPrimary
    }

    private var shown: String {
        if entry.text.isEmpty && entry.isStreaming { return "…" }
        return entry.text
    }

    var body: some View {
        HStack(spacing: 0) {
            if isUser {
                Spacer(minLength: FmSpace.x7)
            }
            Text(shown)
                .fmText(.bodyLarge, color: ink)
                .fixedSize(horizontal: false, vertical: true)
                .padding(.horizontal, FmSpace.x3)
                .padding(.vertical, FmSpace.x2)
                .background(fill)
                .clipShape(FmRoundedShape(radius: FmRadius.lg))
                .fmMessageActions(text: entry.text)
            if entry.isError {
                FmMessageTools(entry.text, color: fm.danger)
            }
            if !isUser {
                Spacer(minLength: FmSpace.x7)
            }
        }
    }
}

struct OnDeviceChatToolRow: View {
    @Environment(\.fmPalette) private var fm
    let entry: OnDeviceChatEntry
    @State private var expanded = false

    var body: some View {
        VStack(alignment: .leading, spacing: FmSpace.x1) {
            Button {
                expanded.toggle()
            } label: {
                HStack(spacing: FmSpace.x2) {
                    FmIcon("search", size: 16, color: fm.textTertiary)
                    Text(OnDeviceChatTools.label(for: entry.toolName ?? "ForkMesh data"))
                        .fmText(.bodySmall, color: fm.textSecondary)
                    FmIcon(expanded ? "expand_less" : "expand_more", size: 16, color: fm.textTertiary)
                    Spacer(minLength: 0)
                }
            }
            .buttonStyle(.plain)
            if expanded {
                Text(entry.text)
                    .fmMono(color: fm.textTertiary)
                    .fixedSize(horizontal: false, vertical: true)
                    .textSelection(.enabled)
            }
        }
        .padding(.horizontal, FmSpace.x3)
    }
}

struct OnDeviceEmailDraftCard: View {
    @Environment(\.fmPalette) private var fm
    @Environment(\.openURL) private var openURL
    let draft: OnDeviceEmailDraft
    @State private var copied = false

    private var openAction: (() -> Void)? {
        guard let url = draft.mailtoURL else { return nil }
        return { openURL(url) }
    }

    var body: some View {
        VStack(alignment: .leading, spacing: FmSpace.x2) {
            HStack(spacing: FmSpace.x2) {
                FmIcon("edit_note_outlined", size: 18, color: fm.accent)
                Text("Email draft · not sent")
                    .fmText(.labelMedium, color: fm.textSecondary)
                Spacer(minLength: 0)
            }
            Text("To: \(draft.recipientName.isEmpty ? draft.to : "\(draft.recipientName) <\(draft.to)>")")
                .fmText(.bodySmall, color: fm.textSecondary)
            Text(draft.subject)
                .fmText(.titleSmall, color: fm.textPrimary)
            Text(draft.body)
                .fmText(.bodyMedium, color: fm.textPrimary)
                .fixedSize(horizontal: false, vertical: true)
                .textSelection(.enabled)
            HStack(spacing: FmSpace.x2) {
                FmToneButton(label: "Open in Mail", icon: "alternate_email", tone: .accent, onPressed: openAction)
                FmToneButton(label: copied ? "Copied" : "Copy", icon: "content_copy") {
                    UIPasteboard.general.string = draft.plainText
                    copied = true
                }
            }
        }
        .padding(FmSpace.x3)
        .background(fm.bgRaised)
        .clipShape(FmRoundedShape(radius: FmRadius.lg))
    }
}

struct OnDeviceChatApprovalBar: View {
    @Environment(\.fmPalette) private var fm
    let approval: OnDeviceToolApproval
    let decide: (Bool) -> Void

    var body: some View {
        VStack(alignment: .leading, spacing: FmSpace.x2) {
            HStack(alignment: .top, spacing: FmSpace.x2) {
                FmIcon("warning_amber", size: 18, color: fm.warning)
                Text(approval.summary)
                    .fmText(.bodyMedium, color: fm.textPrimary)
                    .fixedSize(horizontal: false, vertical: true)
                Spacer(minLength: 0)
                FmMessageTools(approval.summary)
            }
            HStack(spacing: FmSpace.x2) {
                FmToneButton(label: "Allow", icon: "check_rounded", tone: .accent, filled: true) { decide(true) }
                FmToneButton(label: "Decline", icon: "close_rounded", tone: .neutral) { decide(false) }
            }
        }
        .padding(.horizontal, FmSpace.x4)
        .padding(.vertical, FmSpace.x3)
        .shellGlass(RoundedRectangle(cornerRadius: FmRadius.lg, style: .continuous))
        .padding(.horizontal, FmSpace.x2)
        .padding(.top, FmSpace.x1)
    }
}

/// Why the chat cannot run yet, with the way to fix it.
@MainActor
struct OnDeviceChatSetupNote: View {
    @Environment(\.fmPalette) private var fm
    let manager: LocalModelManager

    private var text: String {
        if let reason = manager.engineAvailability.reason { return reason }
        if !manager.isEnabled { return "Turn on On-device AI and download a model first." }
        return "Download \(manager.selectedModel.displayName) first."
    }

    var body: some View {
        HStack(spacing: FmSpace.x3) {
            FmIcon("info_outline", size: 18, color: fm.textSecondary)
            Text(text)
                .fmText(.bodySmall, color: fm.textSecondary)
                .fixedSize(horizontal: false, vertical: true)
            Spacer(minLength: 0)
            if manager.engineAvailability.isAvailable {
                NavigationLink {
                    LocalAISettingsView(manager: manager)
                        .shellPageClearance()
                } label: {
                    Text("Set up")
                        .fmText(.labelMedium, color: fm.accent)
                }
            }
        }
        .padding(.horizontal, FmSpace.x4)
        .padding(.vertical, FmSpace.x3)
        .shellGlass(RoundedRectangle(cornerRadius: FmRadius.lg, style: .continuous))
        .padding(.horizontal, FmSpace.x2)
        .padding(.top, FmSpace.x1)
    }
}

@MainActor
struct OnDeviceChatComposer: View {
    @Environment(\.fmPalette) private var fm
    @Bindable var chat: OnDeviceChatModel

    var body: some View {
        FmBottomActionBar(padding: EdgeInsets.fmSymmetric(horizontal: FmSpace.x3, vertical: FmSpace.x2)) {
            HStack(alignment: .bottom, spacing: FmSpace.x2) {
                FmTextField(text: $chat.draft, hint: "Message \(chat.manager.selectedModel.displayName)", multiline: true)
                if chat.isGenerating {
                    FmIconButton(icon: "stop_circle_outlined", label: "Stop", size: 28, color: fm.danger) {
                        chat.stop()
                    }
                } else {
                    FmIconButton(icon: "send_rounded", label: "Send", size: 26, color: fm.accent, onPressed: sendAction)
                }
            }
        }
    }

    private var sendAction: (() -> Void)? {
        guard chat.canSend else { return nil }
        return {
            FmHaptics.tap()
            chat.send()
        }
    }
}
