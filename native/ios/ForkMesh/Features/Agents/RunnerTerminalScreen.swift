import FMAgents
import FMApi
import FMBase
import SwiftUI
import UIKit

struct RunnerTerminalRequest: Hashable, Identifiable {
    let runner: String
    let provider: String
    let accountLabel: String
    let openId: String

    var id: String { "\(runner):\(provider)" }
}

struct RunnerTerminalScreen: View {
    @Environment(\.fmPalette) private var fm
    @Environment(\.dismiss) private var dismiss
    let request: RunnerTerminalRequest
    @State private var terminal: RunnerTerminal?
    @State private var input: String = ""
    @State private var notice: FmNotice?

    init(request: RunnerTerminalRequest) {
        self.request = request
        _terminal = State(initialValue: RunnerTerminalScreen.makeTerminal(request))
    }

    @MainActor
    static func makeTerminal(_ request: RunnerTerminalRequest, attach: Bool = true) -> RunnerTerminal? {
        guard let api = Services.get(ApiService.self), api.hasSession else { return nil }
        return RunnerTerminal(
            api: api, runner: request.runner, provider: request.provider, accountLabel: request.accountLabel,
            openId: attach ? request.openId : "")
    }

    private var title: String { "\(RunnerSignIn.providerLong(request.provider)) on \(request.runner)" }

    var body: some View {
        NavigationStack {
            content
                .background(fm.bgBase)
                .navigationBarTitleDisplayMode(.inline)
                .toolbar { toolbarItems }
        }
        .onDisappear {
            let current: RunnerTerminal? = terminal
            Task { await current?.close() }
        }
    }

    @ToolbarContentBuilder private var toolbarItems: some ToolbarContent {
        ToolbarItem(placement: .principal) {
            VStack(spacing: 0) {
                Text(title).fmText(.titleSmall).lineLimit(1)
                if !request.accountLabel.isEmpty {
                    Text("as \(request.accountLabel)").fmText(.bodySmall, color: fm.textTertiary).lineLimit(1)
                }
            }
        }
        ToolbarItem(placement: .cancellationAction) {
            Button("Close") { dismiss() }
                .accessibilityHint(Text("Ends the terminal on the runner"))
        }
        ToolbarItem(placement: .topBarTrailing) {
            if let terminal = terminal {
                Button {
                    UIPasteboard.general.string = terminal.screen.allRows.map { $0.text }.joined(separator: "\n").dartTrimmed
                    notice = FmNotice("Terminal text copied")
                } label: {
                    FmIcon("copy_all_outlined", size: 22, color: fm.textSecondary)
                }
                .accessibilityLabel(Text("Copy terminal text"))
            }
        }
    }

    @ViewBuilder private var content: some View {
        if let terminal = terminal {
            VStack(spacing: 0) {
                TerminalStatusLine(terminal: terminal, onRestart: restartable(terminal) ? { restart() } : nil, notice: $notice)
                TerminalGridView(terminal: terminal)
                    .id(ObjectIdentifier(terminal))
                TerminalKeyBar(terminal: terminal, onPaste: {
                    if let text = UIPasteboard.general.string, !text.isEmpty {
                        terminal.paste(text)
                    }
                })
                inputLine(terminal)
            }
            .accessibilityIdentifier("runner-terminal")
        } else {
            FmEmptyState(
                icon: "lock_outline",
                title: "Sign in to ForkMesh first",
                message: "A runner opens a terminal only for the account that added it."
            )
        }
    }

    private func restartable(_ terminal: RunnerTerminal) -> Bool {
        terminal.statusLevel == .error || (!terminal.open && !terminal.session.id.isEmpty)
    }

    private func restart() {
        let old: RunnerTerminal? = terminal
        terminal = RunnerTerminalScreen.makeTerminal(request, attach: false)
        Task { await old?.close() }
    }

    private func inputLine(_ terminal: RunnerTerminal) -> some View {
        HStack(spacing: FmSpace.x2) {
            TextField("Type, then send (sends Enter)", text: $input)
                .textInputAutocapitalization(.never)
                .autocorrectionDisabled(true)
                .fmMono(size: FmType.small, color: fm.textPrimary)
                .submitLabel(.send)
                .onSubmit { submit(terminal) }
                .accessibilityIdentifier("runner-terminal-input")
            Button {
                submit(terminal)
            } label: {
                FmIcon("keyboard_return", size: 22, color: fm.accent)
                    .frame(width: 44, height: 44)
            }
            .buttonStyle(.plain)
            .disabled(terminal.closed)
            .accessibilityLabel(Text("Send with Enter"))
        }
        .padding(EdgeInsets.fmLTRB(FmSpace.x3, 0, FmSpace.x3, FmSpace.x3))
    }

    private func submit(_ terminal: RunnerTerminal) {
        let text: String = input
        input = ""
        terminal.submit(text)
    }
}
