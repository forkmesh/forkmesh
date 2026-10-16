// The desktop on this network, driven from the phone (pair, agents, files), LocalAgentScreen
// (one desktop agent), and LocalDesktopChip. Also the switch that shares this phone's on-device model with the paired
// desktop (LocalControlService.serveLocalAIKey, off by default).

import FMAccount
import FMApi
import FMBase
import FMChat
import FMControl
import SwiftUI
import UniformTypeIdentifiers
#if canImport(UIKit)
import UIKit
#endif

private func toneColor(_ tone: LocalDesktopTone, _ fm: FmPalette) -> Color {
    switch tone {
    case .success: return fm.success
    case .warning: return fm.warning
    case .secondary: return fm.textSecondary
    case .danger: return fm.danger
    case .accent: return fm.accent
    }
}

/// A compact "desktop on this network" indicator for a header: nothing when no desktop is in
/// reach.
struct LocalDesktopChip: View {
    @Environment(\.fmPalette) private var fm
    @State private var control: LocalControlService? = Services.get(LocalControlService.self)

    var body: some View {
        if let control = control, let chip = LocalDesktopText.chip(control) {
            NavigationLink {
                LocalDesktopScreen()
                    .shellPageClearance()
            } label: {
                HStack(spacing: FmSpace.x1) {
                    Circle().fill(toneColor(chip.tone, fm)).frame(width: 10, height: 10)
                    Text(chip.label).fmText(.labelMedium).lineLimit(1)
                }
                .padding(.horizontal, FmSpace.x3)
                .padding(.vertical, FmSpace.x1)
                .background(fm.bgOverlay)
                .clipShape(Capsule())
            }
            .buttonStyle(.plain)
        }
    }
}

struct LocalDesktopScreen: View {
    @State private var control: LocalControlService? = Services.get(LocalControlService.self)

    init() {}

    var body: some View {
        if let control = control {
            LocalDesktopBody(control: control)
        } else {
            FmEmptyState(icon: "wifi_off", title: "Local control is not available")
        }
    }
}

private struct LocalDesktopBody: View {
    @Environment(\.fmPalette) private var fm
    let control: LocalControlService

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: FmSpace.x4) {
                ConnectionCard(control: control)
                if control.connected {
                    ShareAICard(control: control)
                    NewAgentCard(control: control)
                    AgentsCard(control: control)
                    FilesCard(control: control)
                } else {
                    HostsCard(control: control)
                }
            }
            .padding(FmSpace.x4)
        }
        .background(fm.canvas)
        .fmRefreshable {
            if control.connected { await control.refreshAgents() }
        }
        .navigationTitle(control.connected ? control.desktopName : "Desktop on this network")
        .toolbar {
            if control.connected {
                ToolbarItem(placement: .topBarTrailing) {
                    Button(action: { Task { await control.refreshAgents() } }) {
                        FmIcon("refresh", color: fm.textPrimary)
                    }
                    .accessibilityLabel("Refresh")
                }
            }
        }
    }
}

private struct ConnectionCard: View {
    @Environment(\.fmPalette) private var fm
    let control: LocalControlService

    var body: some View {
        let card: LocalDesktopCard = LocalDesktopText.card(control)
        FmCard(radius: FmRadius.lg) {
            VStack(alignment: .leading, spacing: 0) {
                HStack(spacing: FmSpace.x3) {
                    FmIcon(card.icon, color: toneColor(card.tone, fm))
                    Text(card.title).fmText(.titleMedium)
                }
                if !card.detail.isEmpty {
                    Text(card.detail)
                        .fmText(.bodySmall, color: fm.textSecondary, height: 1.35)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .fmMessageTools("\(card.title). \(card.detail)")
                        .padding(.top, FmSpace.x2)
                }
                if control.status == .awaitingApproval, let code = LocalDesktopText.spacedCode(control.pairingCode) {
                    Text(code)
                        .fmMono(size: 34, weight: .semibold)
                        .tracking(4)
                        .frame(maxWidth: .infinity)
                        .padding(.top, FmSpace.x3)
                }
                if control.connected, let agents = control.agents {
                    Text("\(agents.running) running · \(agents.sessions.count) sessions")
                        .fmText(.bodySmall, color: fm.textSecondary)
                        .padding(.top, FmSpace.x2)
                }
                if control.status != .idle {
                    buttons.padding(.top, FmSpace.x3)
                }
            }
        }
    }

    private var buttons: some View {
        HStack(spacing: FmSpace.x2) {
            if control.status == .failed, let host = control.host {
                FmOutlinedButton("Try again", onPressed: { Task { await control.connect(host) } })
            }
            if control.status != .failed {
                FmOutlinedButton(control.connected ? "Disconnect" : "Cancel", onPressed: {
                    Task { await control.disconnect() }
                })
            }
            if control.isPaired(control.desktopKey) {
                FmTextButton("Forget this desktop", onPressed: {
                    let key: String = control.desktopKey
                    Task { await control.forget(key) }
                })
            }
        }
    }
}

private struct HostsCard: View {
    @Environment(\.fmPalette) private var fm
    let control: LocalControlService

    var body: some View {
        let hosts: [MeshControlHost] = control.hosts
        let blocked: Bool = Services.get(LocalMeshService.self)?.localNetworkBlocked ?? false
        FmCard(radius: FmRadius.lg) {
            VStack(alignment: .leading, spacing: FmSpace.x2) {
                Text("DESKTOPS ON THIS NETWORK").fmText(.labelSmall, color: fm.textTertiary)
                if hosts.isEmpty {
                    Text(LocalDesktopText.hostsEmpty(blocked: blocked))
                        .fmText(.bodySmall, color: fm.textSecondary, height: 1.35)
                }
                ForEach(hosts) { (host: MeshControlHost) in
                    HStack(spacing: FmSpace.x3) {
                        FmIcon("desktop_windows_outlined", color: fm.textSecondary)
                        VStack(alignment: .leading, spacing: 2) {
                            Text(host.label).fmText(.titleSmall)
                            Text(LocalDesktopText.hostSubtitle(host)).fmText(.bodySmall, color: fm.textSecondary)
                        }
                        .frame(maxWidth: .infinity, alignment: .leading)
                        FmFilledButton(
                            control.isPaired(host.nodeId) ? "Connect" : "Pair",
                            onPressed: control.status == .connecting ? nil : { Task { await control.connect(host) } })
                    }
                    .padding(.vertical, FmSpace.x1)
                }
            }
        }
    }
}

/// New: lets the paired desktop run prompts on this phone's own model.
private struct ShareAICard: View {
    @Environment(\.fmPalette) private var fm
    let control: LocalControlService

    var body: some View {
        FmCard(radius: FmRadius.lg) {
            VStack(alignment: .leading, spacing: FmSpace.x2) {
                Toggle(isOn: Binding(get: { control.servesLocalAI }, set: { control.setServesLocalAI($0) })) {
                    Text(LocalDesktopText.aiToggleTitle).fmText(.titleSmall)
                }
                .tint(fm.accent)
                Text(LocalDesktopText.aiToggleDetail)
                    .fmText(.bodySmall, color: fm.textSecondary, height: 1.35)
                if !control.localAIAvailable {
                    Text("Turn on On-device AI in Settings and download a model first.")
                        .fmText(.bodySmall, color: fm.warning)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .fmMessageTools("Turn on On-device AI in Settings and download a model first.")
                } else if control.aiRequestsServed > 0 {
                    Text("\(control.aiRequestsServed) \(control.aiRequestsServed == 1 ? "request" : "requests") answered")
                        .fmText(.bodySmall, color: fm.textTertiary)
                }
            }
        }
    }
}

private struct NewAgentCard: View {
    @Environment(\.fmPalette) private var fm
    let control: LocalControlService
    @State private var model: LocalNewAgentModel?

    var body: some View {
        FmCard(radius: FmRadius.lg) {
            content
        }
        .onAppear {
            if model == nil { model = LocalNewAgentModel(control: control) }
        }
    }

    @ViewBuilder private var content: some View {
        if let agents = control.agents, let model = model {
            if agents.repositories.isEmpty {
                Text("The desktop has no repository checked out, so it has nowhere to run a new agent.")
                    .fmText(.bodySmall, color: fm.textSecondary)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .fmMessageTools("The desktop has no repository checked out, so it has nowhere to run a new agent.")
            } else {
                NewAgentForm(model: model, agents: agents)
            }
        } else {
            FmLinearProgressIndicator()
        }
    }
}

private struct NewAgentForm: View {
    @Environment(\.fmPalette) private var fm
    @Bindable var model: LocalNewAgentModel
    let agents: LocalAgentsSnapshot

    var body: some View {
        VStack(alignment: .leading, spacing: FmSpace.x2) {
            Text("NEW AGENT ON THE DESKTOP").fmText(.labelSmall, color: fm.textTertiary)
            Picker("Repository", selection: repoBinding) {
                ForEach(model.repoLabels, id: \.self) { (label: String) in
                    Text(label).tag(label)
                }
            }
            if !agents.providers.isEmpty {
                Picker("Agent", selection: providerBinding) {
                    ForEach(agents.providers) { (provider: LocalAgentProvider) in
                        Text(provider.label).tag(provider.id)
                    }
                }
            }
            TextField("What should the agent do?", text: $model.task, axis: .vertical)
                .lineLimit(2...6)
                .padding(FmSpace.x3)
                .background(fm.bgBase)
                .clipShape(FmRoundedShape(radius: FmRadius.md))
                .onChange(of: model.task) { _, text in
                    if text.count > 8000 { model.task = String(text.prefix(8000)) }
                }
            HStack {
                Spacer()
                FmFilledButton(model.busy ? "Starting…" : "Start on desktop", icon: "play_arrow", onPressed: model.busy ? nil : {
                    Task { await model.start() }
                })
            }
            FmNoticeLine(model.notice, isError: model.noticeIsError)
        }
    }

    private var repoBinding: Binding<String> {
        Binding(get: { model.selectedRepo ?? "" }, set: { model.repo = $0 })
    }

    private var providerBinding: Binding<String> {
        Binding(get: { model.selectedProvider ?? "" }, set: { model.provider = $0 })
    }
}

private struct AgentsCard: View {
    @Environment(\.fmPalette) private var fm
    let control: LocalControlService

    var body: some View {
        let sessions: [LocalAgentRow] = control.agents?.sessions ?? []
        FmCard(padding: EdgeInsets.fmSymmetric(vertical: FmSpace.x3), radius: FmRadius.lg) {
            VStack(alignment: .leading, spacing: 0) {
                HStack {
                    Text("AGENTS ON THE DESKTOP")
                        .fmText(.labelSmall, color: fm.textTertiary)
                        .frame(maxWidth: .infinity, alignment: .leading)
                    if !sessions.isEmpty {
                        NavigationLink {
                            AgentWatchScreen.forLocal(control, sessions)
                                .shellPageClearance()
                        } label: {
                            AgentWatchEntryLabel(
                                text: sessions.contains { $0.session.isActive } ? "Watch & listen" : "Replay & listen")
                        }
                        .buttonStyle(.plain)
                        .accessibilityIdentifier("local-agents-watch")
                    }
                }
                .padding(.horizontal, FmSpace.x4)
                if control.agents == nil || (control.agentsLoading && sessions.isEmpty) {
                    FmLinearProgressIndicator().padding(FmSpace.x4)
                } else if sessions.isEmpty {
                    Text("No agent sessions on the desktop yet.")
                        .fmText(.bodySmall, color: fm.textSecondary)
                        .padding(FmSpace.x4)
                }
                ForEach(sessions) { (row: LocalAgentRow) in
                    NavigationLink {
                        LocalAgentScreen(sessionId: row.session.id)
                            .shellPageClearance()
                    } label: {
                        agentRow(row)
                    }
                    .buttonStyle(.plain)
                }
            }
        }
    }

    private func agentRow(_ row: LocalAgentRow) -> some View {
        HStack(spacing: FmSpace.x3) {
            FmIcon(row.session.isActive ? "play_circle_outline" : "check_circle_outline",
                   color: row.session.isActive ? fm.accent : fm.textTertiary)
            VStack(alignment: .leading, spacing: 2) {
                Text(row.title).fmText(.titleSmall).lineLimit(2)
                Text(LocalDesktopText.agentSubtitle(row)).fmText(.bodySmall, color: fm.textSecondary).lineLimit(1)
            }
            .frame(maxWidth: .infinity, alignment: .leading)
        }
        .padding(.horizontal, FmSpace.x4)
        .padding(.vertical, FmSpace.x2)
        .contentShape(Rectangle())
    }
}

private struct FilesCard: View {
    @Environment(\.fmPalette) private var fm
    let control: LocalControlService
    @State private var notice: FmNotice?
    @State private var picking: Bool = false
    @State private var preview: LocalReceivedFile?

    var body: some View {
        FmCard(radius: FmRadius.lg) {
            VStack(alignment: .leading, spacing: FmSpace.x2) {
                Text("FILES").fmText(.labelSmall, color: fm.textTertiary)
                Text(LocalDesktopText.filesIntro).fmText(.bodySmall, color: fm.textSecondary, height: 1.35)
                if let progress = control.uploadProgress {
                    Text("Sending \(control.uploadName)…").fmText(.bodyMedium)
                    FmLinearProgressIndicator(value: progress)
                }
                FmOutlinedButton("Send a file to the desktop", icon: "upload_file",
                                 onPressed: control.uploadProgress != nil ? nil : { picking = true })
                if notice != nil {
                    FmNoticeLine($notice)
                }
                ForEach(control.receivedFiles) { (file: LocalReceivedFile) in
                    receivedRow(file)
                }
            }
        }
        .fileImporter(isPresented: $picking, allowedContentTypes: [UTType.item]) { (result: Result<URL, Error>) in
            if case .success(let url) = result { send(url) }
        }
        .sheet(item: $preview) { (file: LocalReceivedFile) in
            ReceivedImagePreview(path: file.path)
        }
    }

    private func receivedRow(_ file: LocalReceivedFile) -> some View {
        Button(action: {
            if LocalDesktopText.isImage(file.name) { preview = file }
        }) {
            HStack(spacing: FmSpace.x3) {
                FmIcon("download_done", color: fm.textSecondary)
                VStack(alignment: .leading, spacing: 2) {
                    Text(file.name).fmText(.titleSmall).lineLimit(1)
                    Text(LocalDesktopText.sizeLabel(file.size)).fmText(.bodySmall, color: fm.textSecondary)
                }
                .frame(maxWidth: .infinity, alignment: .leading)
            }
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
    }

    private func send(_ url: URL) {
        Task {
            let scoped: Bool = url.startAccessingSecurityScopedResource()
            defer {
                if scoped { url.stopAccessingSecurityScopedResource() }
            }
            do {
                let saved: String = try await control.sendFile(url)
                notice = FmNotice("Sent \(saved) to \(control.desktopName).")
            } catch {
                notice = FmNotice.error(LocalDesktopText.errorText(error))
            }
        }
    }
}

private struct ReceivedImagePreview: View {
    let path: String

    var body: some View {
        #if canImport(UIKit)
        if let image = UIImage(contentsOfFile: path) {
            ScrollView([.horizontal, .vertical]) {
                Image(uiImage: image).resizable().scaledToFit()
            }
        } else {
            FmEmptyState(icon: "broken_image", title: "This file cannot be shown")
        }
        #else
        FmEmptyState(icon: "broken_image", title: "This file cannot be shown")
        #endif
    }
}

/// One of the desktop's agent sessions: its transcript, a box to message it, and Stop / Continue.
struct LocalAgentScreen: View {
    @State private var model: LocalAgentModel?

    init(sessionId: Int) {
        let control: LocalControlService? = Services.get(LocalControlService.self)
        _model = State(initialValue: control.map { LocalAgentModel(control: $0, sessionId: sessionId) })
    }

    var body: some View {
        if let model = model {
            LocalAgentBody(model: model)
        } else {
            FmEmptyState(icon: "wifi_off", title: "Local control is not available")
        }
    }
}

private struct LocalAgentBody: View {
    @Environment(\.fmPalette) private var fm
    @Bindable var model: LocalAgentModel
    @State private var statusNotice: FmNotice?
    @State private var sendNotice: FmNotice?
    @State private var attachmentLoader: AgentAttachmentLoader = AgentAttachmentsSupport.loader()

    var body: some View {
        VStack(spacing: 0) {
            if let row = model.row {
                statusLine(row).padding(.horizontal, FmSpace.x4).padding(.top, FmSpace.x3)
                if statusNotice != nil {
                    FmNoticeLine($statusNotice)
                        .padding(.horizontal, FmSpace.x4)
                }
                if !row.session.lastError.isEmpty {
                    Text(row.session.lastError)
                        .fmText(.bodySmall, color: fm.danger)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .fmMessageTools(row.session.lastError)
                        .padding(.horizontal, FmSpace.x4)
                        .padding(.top, FmSpace.x2)
                }
            }
            transcript.padding(.top, FmSpace.x2)
            if sendNotice != nil {
                FmNoticeLine($sendNotice)
                    .padding(.horizontal, FmSpace.x4)
            }
            promptBar
        }
        .background(fm.canvas)
        .navigationTitle(model.title)
        .toolbar {
            if let row = model.row, let control = Services.get(LocalControlService.self) {
                ToolbarItem(placement: .topBarTrailing) {
                    NavigationLink {
                        AgentWatchScreen(
                            targets: [localWatchTarget(control, row)],
                            title: row.session.isActive ? "Watch agent" : "Replay agent",
                            trigger: .localControl(control),
                            debounce: 0.4)
                            .shellPageClearance()
                    } label: {
                        Image(systemName: "waveform")
                    }
                    .accessibilityLabel(Text("Watch, hear and feel it"))
                    .accessibilityIdentifier("local-agent-watch")
                }
            }
        }
        .onAppear { model.start() }
        .onDisappear { model.stop() }
    }

    private func statusLine(_ row: LocalAgentRow) -> some View {
        HStack(spacing: FmSpace.x2) {
            Text(model.subtitle)
                .fmText(.bodySmall, color: fm.textSecondary)
                .frame(maxWidth: .infinity, alignment: .leading)
            if row.stoppable {
                FmOutlinedButton("Stop", onPressed: model.busy ? nil : { run { await model.stopAgent() } })
            } else if row.canContinue {
                FmOutlinedButton("Continue", onPressed: model.busy ? nil : { run { await model.continueAgent() } })
            }
        }
    }

    @ViewBuilder private var transcript: some View {
        if model.loading {
            FmSkeletonList(rows: 6, hasLeading: false)
        } else {
            ScrollViewReader { (proxy: ScrollViewProxy) in
                ScrollView {
                    if !model.attachments.isEmpty {
                        AgentAttachmentStrip(refs: model.attachments, loader: attachmentLoader)
                            .padding(.horizontal, FmSpace.x4)
                            .padding(.top, FmSpace.x3)
                    }
                    AgentTranscriptBlocksView(blocks: AgentTranscriptFormat.blocks(model.transcriptText))
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .padding(FmSpace.x4)
                    Color.clear.frame(height: 1).id("end")
                }
                .fmRefreshable {
                    await Services.get(LocalControlService.self)?.refreshAgents()
                    await model.reload()
                }
                .onChange(of: model.transcript) { _, _ in proxy.scrollTo("end", anchor: .bottom) }
            }
        }
    }

    private var promptBar: some View {
        HStack(spacing: FmSpace.x2) {
            TextField("Message this agent", text: $model.prompt, axis: .vertical)
                .lineLimit(1...4)
                .padding(FmSpace.x3)
                .shellGlass(RoundedRectangle(cornerRadius: FmRadius.lg * 2, style: .continuous))
            Button(action: {
                Task {
                    if let message = await model.send() { sendNotice = FmNotice(message) }
                }
            }) {
                FmIcon("send", size: 18, color: Color.white)
                    .frame(width: 44, height: 44)
                    .background(fm.accent)
                    .clipShape(Circle())
            }
            .buttonStyle(.plain)
            .disabled(model.busy)
            .accessibilityLabel("Send")
        }
        .padding(.horizontal, FmSpace.x3)
        .padding(.top, FmSpace.x2)
        .padding(.bottom, FmSpace.x3)
    }

    private func run(_ action: @escaping () async -> String) {
        Task {
            let message: String = await action()
            statusNotice = FmNotice(message)
        }
    }
}
