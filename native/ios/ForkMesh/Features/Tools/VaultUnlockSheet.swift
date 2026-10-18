import FMVault
import SwiftUI

struct VaultUnlockSheet: View {
    let vault: VaultService
    let purpose: String
    let onUnlocked: () -> Void
    let onCancel: () -> Void
    @State private var password = ""
    @State private var problem = ""
    @State private var busy = false

    var body: some View {
        NavigationStack {
            Form {
                Section {
                    SecureField(VaultCopy.masterPasswordLabel, text: $password)
                        .textContentType(nil)
                        .textInputAutocapitalization(.never)
                        .autocorrectionDisabled(true)
                        .onSubmit(unlock)
                        .accessibilityIdentifier("vault-prompt-password")
                } header: {
                    Text("\(purpose) Type your master password to unlock it.")
                } footer: {
                    if !problem.isEmpty { Text(problem).foregroundStyle(.red) }
                }
                Section {
                    Button(busy ? "Unlocking…" : VaultCopy.unlock, action: unlock)
                        .disabled(busy || password.isEmpty)
                        .accessibilityIdentifier("vault-prompt-unlock")
                    if vault.quickUnlockEnabled {
                        Button(VaultCopy.unlockWithDevice, action: unlockWithDevice)
                            .disabled(busy)
                    }
                }
            }
            .navigationTitle("Unlock Password Vault")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) { Button("Cancel", action: onCancel).disabled(busy) }
            }
        }
        .interactiveDismissDisabled(busy)
    }

    private func unlock() {
        guard !busy, !password.isEmpty else { return }
        let typed = password
        password = ""
        busy = true
        problem = ""
        Task {
            defer { busy = false }
            do {
                try await vault.unlock(typed)
                onUnlocked()
            } catch {
                problem = VaultCopy.errorText(error)
            }
        }
    }

    private func unlockWithDevice() {
        guard !busy else { return }
        busy = true
        problem = ""
        Task {
            defer { busy = false }
            do {
                if try await vault.unlockWithDevice() { onUnlocked() }
            } catch {
                problem = VaultCopy.errorText(error)
            }
        }
    }
}
