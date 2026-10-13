import FMControl
import FMDesktopRemote
import SwiftUI

struct RemoteConfirmSheet: View {
    @Environment(\.dismiss) private var dismiss
    let title: String
    let confirmation: RemoteConfirmation
    var destructive = false
    let perform: (RemoteConfirmation, String) async throws -> String
    var onDone: (String) -> Void = { _ in }

    @State private var typed = ""
    @State private var running = false
    @State private var error = ""
    @State private var openedAt = Date()

    var body: some View {
        Form {
            Section {
                Text(confirmation.summary)
                    .fixedSize(horizontal: false, vertical: true)
            } header: {
                Text(title)
            }
            if confirmation.needsTypedConfirmation {
                Section {
                    TextField(confirmation.typeToConfirm, text: $typed)
                        .textInputAutocapitalization(.never)
                        .autocorrectionDisabled()
                        .font(.system(.body, design: .monospaced))
                } header: {
                    Text("Type \(confirmation.typeToConfirm) to confirm")
                } footer: {
                    Text("Face ID or your passcode confirms it is you before the desktop acts.")
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
                Button(role: destructive ? .destructive : nil) {
                    Task { await run() }
                } label: {
                    HStack {
                        Text(title)
                        Spacer()
                        if running { ProgressView() }
                    }
                }
                .disabled(running || !confirmation.accepts(typed: typed))
            } footer: {
                Text("The desktop runs this with its own keys; they never leave it. Every change is written to the desktop's log.")
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
        if Date().timeIntervalSince(openedAt) * 1000 > Double(confirmation.expiresInMs) {
            error = RemoteErrorText.describe(code: "confirmation_expired")
            return
        }
        if confirmation.needsDeviceAuthentication {
            let ok = await DesktopRemoteFeature.authenticateOwner(reason: title)
            if !ok {
                error = "Face ID or your passcode is needed for this."
                return
            }
        }
        running = true
        defer { running = false }
        do {
            let message = try await perform(confirmation, typed)
            onDone(message)
            dismiss()
        } catch {
            self.error = RemoteErrorText.describe(error)
        }
    }
}

struct PendingConfirmation: Identifiable {
    let title: String
    let confirmation: RemoteConfirmation
    let destructive: Bool

    var id: String { confirmation.id }
}
