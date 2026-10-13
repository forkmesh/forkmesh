// WS7: the confirmation every host change goes through. The desktop wrote the summary and issued
// a one-use token that expires in a minute; destroy also needs the host's name typed back, and
// the desktop checks it again.

import FMControl
import FMHosts
import SwiftUI

struct HostActionSheet: View {
    @Environment(\.dismiss) private var dismiss
    @Bindable var store: HostsStore
    let confirmation: HostConfirmation
    var plan = ""
    var region = ""
    var onTerminal: ((String) -> Void)?

    @State private var typed = ""
    @State private var running = false
    @State private var error = ""
    @State private var openedAt = Date()

    var body: some View {
        Form {
            Section {
                Text(confirmation.summary)
                    .font(.body)
                    .fixedSize(horizontal: false, vertical: true)
            } header: {
                Text(confirmation.action.title)
            }
            if confirmation.needsTypedName {
                Section {
                    TextField(confirmation.typeToConfirm, text: $typed)
                        .textInputAutocapitalization(.never)
                        .autocorrectionDisabled()
                        .font(.system(.body, design: .monospaced))
                } header: {
                    Text("Type \(confirmation.typeToConfirm) to confirm")
                }
            }
            if !error.isEmpty {
                Section {
                    Label(error, systemImage: "exclamationmark.triangle")
                        .foregroundStyle(.red)
                        .font(.footnote)
                        .fmMessageTools(error)
                }
            }
            Section {
                Button(role: confirmation.action.isDestructive ? .destructive : nil) {
                    Task { await run() }
                } label: {
                    HStack {
                        Text(confirmation.action.title)
                        Spacer()
                        if running { ProgressView() }
                    }
                }
                .disabled(running || !confirmation.accepts(typed: typed))
            } footer: {
                Text(confirmation.action.opensTerminal
                    ? "The desktop opens the connection with its own key, which never leaves it. Opening and closing the session are written to the desktop's log."
                    : "The desktop runs this with its own keys; they never leave it. Every change is written to the desktop's log.")
            }
        }
        .navigationTitle("Confirm")
        .navigationBarTitleDisplayMode(.inline)
        .toolbar {
            ToolbarItem(placement: .cancellationAction) {
                Button("Cancel") { dismiss() }
            }
        }
    }

    private func run() async {
        // A token older than its lifetime would only be refused; say so here.
        if Date().timeIntervalSince(openedAt) * 1000 > Double(confirmation.expiresInMs) {
            error = ControlErrorText.describe(code: "confirmation_expired")
            return
        }
        running = true
        defer { running = false }
        do {
            if confirmation.action.opensTerminal {
                let terminal = try await store.openTerminal(confirmation)
                dismiss()
                onTerminal?(terminal)
                return
            }
            try await store.run(confirmation, typedName: typed, plan: plan, region: region)
            dismiss()
        } catch {
            self.error = ControlErrorText.describe(error)
        }
    }
}
