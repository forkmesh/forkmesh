import AppIntents
import Foundation

enum ListeningSwitch: String, AppEnum {
    case on
    case off

    static let typeDisplayRepresentation: TypeDisplayRepresentation = "Listening"
    static let caseDisplayRepresentations: [ListeningSwitch: DisplayRepresentation] = [
        .on: DisplayRepresentation(title: "Start", synonyms: ["Turn on", "Switch on", "Begin", "Resume"]),
        .off: DisplayRepresentation(title: "Stop", synonyms: ["Turn off", "Switch off", "End", "Pause"]),
    ]
}

struct ListeningIntent: AppIntent {
    static let title: LocalizedStringResource = "Turn listening on or off"
    static let description = IntentDescription(
        "Starts or stops ForkMesh's always-on speech recognition, so you can talk to the Genie. Leave Listening empty to switch it.")
    static let openAppWhenRun: Bool = true
    static let readySteps = 32

    @Parameter(title: "Listening")
    var change: ListeningSwitch?

    init() {}

    @MainActor
    func perform() async throws -> some IntentResult & ProvidesDialog {
        let hub = GenieHub.shared
        var waited = 0
        while !hub.attached, waited < Self.readySteps {
            try? await Task.sleep(nanoseconds: 250_000_000)
            waited += 1
        }
        let on = change.map { $0 == .on } ?? !hub.alwaysListening
        if on {
            if PromptDictation.refused {
                throw AgentVoiceError("Allow the microphone and speech recognition for ForkMesh in Settings, then try again.")
            }
            if hub.alwaysListening && hub.listensNow {
                return .result(dialog: "ForkMesh is already listening.")
            }
        } else if !hub.alwaysListening && !hub.listensNow {
            return .result(dialog: "ForkMesh is not listening.")
        }
        let returning = on && hub.spiderHidden
        hub.setAlwaysListening(on)
        if returning {
            return .result(dialog: "The Genie is back and listening.")
        }
        return .result(dialog: on ? "ForkMesh is listening." : "ForkMesh stopped listening.")
    }
}
