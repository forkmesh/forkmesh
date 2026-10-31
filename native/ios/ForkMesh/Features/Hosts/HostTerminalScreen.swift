import FMAgents
import FMBase
import FMChat
import FMControl
import SwiftUI
import UIKit

struct HostTerminalTarget: Hashable, Identifiable {
    let id: String
}

struct HostTerminalScreen: View {
    @Environment(\.fmPalette) private var fm
    let host: String
    @State private var terminal: DesktopAgentTerminal
    @State private var input: String = ""
    @State private var ctrl: Bool = false
    @State private var notice: FmNotice?
    @State private var confirmEnd: Bool = false

    init(hub: DesktopControlHub, host: String, terminal id: String) {
        self.host = host
        _terminal = State(initialValue: DesktopAgentTerminal(channel: hub, terminal: id))
    }

    var body: some View {
        VStack(spacing: 0) {
            TerminalStatusLine(terminal: terminal, onRestart: nil, notice: $notice)
            TerminalGridView(terminal: terminal)
                .id(ObjectIdentifier(terminal))
            TerminalKeyBar(terminal: terminal, ctrl: $ctrl, onPaste: {
                if let text = UIPasteboard.general.string, !text.isEmpty {
                    terminal.paste(text)
                }
            })
            inputLine
        }
        .background(fm.canvas)
        .navigationTitle("SSH to \(host)")
        .navigationBarTitleDisplayMode(.inline)
        .toolbar { toolbarItems }
        .confirmationDialog("Close the SSH session to \(host)?", isPresented: $confirmEnd, titleVisibility: .visible) {
            Button("Close", role: .destructive) { end() }
        } message: {
            Text("The desktop ends the connection and anything still running in that shell. Leaving this screen instead keeps the session open on the desktop.")
        }
        .onAppear { terminal.reattach() }
        .onDisappear { terminal.detach() }
    }

    @ToolbarContentBuilder private var toolbarItems: some ToolbarContent {
        ToolbarItem(placement: .principal) {
            VStack(spacing: 0) {
                Text("SSH to \(host)").fmText(.titleSmall).lineLimit(1)
                Text("through the desktop").fmText(.bodySmall, color: fm.textTertiary).lineLimit(1)
            }
        }
        ToolbarItemGroup(placement: .topBarTrailing) {
            Button {
                UIPasteboard.general.string = terminal.screen.allRows.map { $0.text }.joined(separator: "\n").dartTrimmed
                notice = FmNotice("Terminal text copied")
            } label: {
                FmIcon("copy_all_outlined", size: 22, color: fm.textSecondary)
            }
            .accessibilityLabel(Text("Copy terminal text"))
            if !terminal.closed && !terminal.terminalId.isEmpty {
                Button(role: .destructive) {
                    confirmEnd = true
                } label: {
                    if terminal.ending {
                        ProgressView()
                    } else {
                        Text("Close")
                    }
                }
                .disabled(terminal.ending)
                .accessibilityIdentifier("host-terminal-close")
            }
        }
    }

    private var inputLine: some View {
        HStack(spacing: FmSpace.x2) {
            TextField("Type a command, then send (sends Enter)", text: $input)
                .textInputAutocapitalization(.never)
                .autocorrectionDisabled(true)
                .fmMono(size: FmType.small, color: fm.textPrimary)
                .submitLabel(.send)
                .onSubmit { submitLine() }
                .terminalChords(text: $input, ctrl: $ctrl, terminal: terminal)
                .accessibilityIdentifier("host-terminal-input")
            Button {
                submitLine()
            } label: {
                FmIcon("keyboard_return", size: 22, color: fm.accent)
                    .frame(width: 44, height: 44)
            }
            .buttonStyle(.plain)
            .accessibilityLabel(Text("Send with Enter"))
        }
        .padding(EdgeInsets.fmLTRB(FmSpace.x3, 0, FmSpace.x3, FmSpace.x3))
    }

    private func submitLine() {
        let text: String = input
        input = ""
        terminal.paste(text)
        if text.isEmpty {
            terminal.key(.enter)
        } else {
            let current = terminal
            Task {
                try? await Task.sleep(nanoseconds: 120_000_000)
                if !current.closed {
                    current.key(.enter)
                }
            }
        }
    }

    private func end() {
        let current = terminal
        Task {
            await current.end()
            if current.closed {
                notice = FmNotice("SSH session closed")
            }
        }
    }
}
