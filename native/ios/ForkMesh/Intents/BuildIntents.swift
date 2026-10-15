import AppIntents
import FMTeammates
import Foundation

struct StartIOSBuildIntent: AppIntent {
    static let title: LocalizedStringResource = "Start a new iOS build"
    static let description = IntentDescription(
        "Builds main ad hoc on the Office MacBook and emails the install link to the iOS testers.")
    static let authenticationPolicy: IntentAuthenticationPolicy = .requiresAuthentication

    static let question = "Start a new ad hoc iOS build of main on the Office MacBook? The testers get the install link by email."
    static let started = "Started a new iOS build on the Office MacBook. The testers get the install link when it finishes."
    static let alreadyRunning = "An iOS build is already running on the Office MacBook."

    init() {}

    @MainActor
    func perform() async throws -> some IntentResult & ProvidesDialog {
        _ = try await AgentVoiceSource.center()
        guard let shortcuts = TeammatesFeature.shortcuts else {
            throw AgentVoiceError("Pair this phone with your desktop first.")
        }
        let shortcut = BuildMacShortcut.iosAdHoc
        await shortcuts.loadRun()
        if shortcuts.isRunning(repo: BuildMacShortcut.repo, file: shortcut.file) {
            return .result(dialog: IntentDialog(stringLiteral: Self.alreadyRunning))
        }
        let confirmation: TeamConfirmation
        do {
            confirmation = try await shortcuts.prepare(repo: BuildMacShortcut.repo, entry: shortcut.entry)
        } catch {
            throw AgentVoiceError(TeamErrorText.describe(error))
        }
        let preparedAt = Date()
        try await requestConfirmation(result: .result(dialog: IntentDialog(stringLiteral: Self.question)))
        if Date().timeIntervalSince(preparedAt) * 1000 > Double(confirmation.expiresInMs) {
            throw AgentVoiceError(TeamErrorText.describe(code: "confirmation_expired"))
        }
        do {
            try await shortcuts.run(confirmation)
        } catch {
            throw AgentVoiceError(TeamErrorText.describe(error))
        }
        return .result(dialog: IntentDialog(stringLiteral: Self.started))
    }
}
