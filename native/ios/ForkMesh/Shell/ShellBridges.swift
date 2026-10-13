// The shell's seams to the other feature areas that the fixed entry points
// (seams: ChatScreen, AgentStartCard...) do not cover: the prompt bar's
// hand-off to the new-agent form.

import FMAgents
import FMApi
import FMBase
import Foundation
import Observation

/// The prompt bar's text on its way into the new-agent form. The shell offers
/// it before presenting `AgentStartCard()`; the card takes it once (to fill
/// its prompt field) and calls `started()` when a run was launched, which
/// clears the prompt bar, reloads the Agents list and opens Agents.
@MainActor
@Observable
final class AgentPromptHandoff {
    private(set) var pendingPrompt: String?
    /// Photos travelling with the prompt: into the form, or into the
    /// on-device agent a quick send opened.
    @ObservationIgnored private(set) var pendingImages: [PromptAttachment] = []
    @ObservationIgnored private(set) var pendingCredentials: AgentCredentials?
    @ObservationIgnored private var onStarted: (() -> Void)?

    func offer(prompt: String, images: [PromptAttachment] = [], onStarted: @escaping () -> Void) {
        pendingPrompt = prompt
        pendingImages = images
        self.onStarted = onStarted
    }

    /// Photos for the next on-device agent that auto-starts.
    func offer(images: [PromptAttachment], credentials: AgentCredentials? = nil) {
        pendingImages = images
        pendingCredentials = credentials
    }

    func takeCredentials() -> AgentCredentials? {
        let credentials: AgentCredentials? = pendingCredentials
        pendingCredentials = nil
        return credentials
    }

    /// The photos, once.
    func takeImages() -> [PromptAttachment] {
        let images: [PromptAttachment] = pendingImages
        pendingImages = []
        return images
    }

    /// The prompt, once; nil when the form was opened some other way.
    func takePrompt() -> String? {
        let prompt: String? = pendingPrompt
        pendingPrompt = nil
        return prompt
    }

    /// A run started from the form.
    func started() {
        let handler: (() -> Void)? = onStarted
        onStarted = nil
        pendingPrompt = nil
        pendingImages = []
        handler?()
    }

    /// The form closed without starting anything.
    func abandoned() {
        onStarted = nil
        pendingPrompt = nil
        pendingImages = []
    }
}
