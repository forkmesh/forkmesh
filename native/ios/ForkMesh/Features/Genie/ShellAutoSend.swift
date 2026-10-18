import FMAssistant
import FMBase
import FMPackages
import Foundation
import Observation

@MainActor
@Observable
final class ShellAutoSend {
    static let shared: ShellAutoSend = ShellAutoSend()
    nonisolated static let tries: Int = 5
    nonisolated static let idleCheck: Double = 60
    nonisolated static let soonest: Double = 30

    private(set) var on: Bool
    private(set) var sentToday: Int
    private(set) var sending: Bool = false
    private(set) var pausedFor: String = ""
    private(set) var waiting: Int = 0
    var review: GenieAutoSendReceipt?

    @ObservationIgnored private let pace: GenieAutoSendPace
    @ObservationIgnored private(set) var skipped: Set<String> = []
    @ObservationIgnored private var queue: [GenieAutoSendReceipt] = []
    @ObservationIgnored private let status: ShellMailStatus = ShellMailStatus.shared

    init(prefs: KeyValueStore? = Services.get(KeyValueStore.self)) {
        pace = GenieAutoSendPace(store: prefs)
        on = pace.on
        sentToday = pace.sentToday(now: Date())
    }

    func toggle() {
        setOn(!on)
        if on {
            FmHaptics.impact(.heavy)
            status.say("Auto-send is on: sending the first email now, then up to \(GenieAutoSendPace.perDay) a day through Zoho Mail, one about every \(Int((GenieAutoSendPace.every / 60).rounded())) minutes while the app is open. Long-press the picture to stop.")
        } else {
            FmHaptics.success()
            status.say("Auto-send is off. \(sentToday == 1 ? "1 email" : "\(sentToday) emails") sent today.")
        }
    }

    func setOn(_ value: Bool) {
        pace.setOn(value)
        on = value
        pausedFor = ""
        ShellSuggestions.shared.refreshNow()
        if value {
            Task { @MainActor [weak self] in await self?.tick(force: true) }
        }
    }

    func run() async {
        while !Task.isCancelled {
            await tick()
            let wait: Double = on ? max(ShellAutoSend.soonest, pace.wait(now: Date())) : ShellAutoSend.idleCheck
            try? await Task.sleep(for: .seconds(wait))
        }
    }

    func tick(now: Date = Date(), force: Bool = false) async {
        sentToday = pace.sentToday(now: now)
        guard on, !sending else { return }
        if force {
            guard sentToday < GenieAutoSendPace.perDay else {
                pause("Today's \(GenieAutoSendPace.perDay) emails are already sent. The next one goes out tomorrow.")
                return
            }
        } else {
            guard pace.due(now: now) else { return }
        }
        guard let toolbox: ForkMeshToolbox = LocalAIFeature.toolbox() else {
            if force { pause("This phone is not connected to ForkMesh. Sign in and try again.") }
            return
        }
        sending = true
        defer {
            sending = false
            status.finish()
        }
        for _ in 0..<ShellAutoSend.tries {
            guard on else { return }
            status.set(.finding)
            guard let pick: GenieOutreachPick = await toolbox.outreachBest(skip: skipped, only: .email, zohoOnly: true) else {
                pause("Nobody is left to email in a CRM with Zoho Mail connected.")
                return
            }
            let outcome: GenieAutoSendOutcome = await toolbox.outreachAutoSend(pick, writers: GenieFeature.outreachWriters()) {
                [status] (stage: GenieAutoSendStage) in status.set(stage)
            }
            switch outcome {
            case .sent(let receipt):
                skipped.insert(pick.contact.id)
                pace.sent(at: Date())
                sentToday = pace.sentToday(now: Date())
                pausedFor = ""
                show(receipt)
                ShellSuggestions.shared.refreshNow()
                return
            case .skip:
                skipped.insert(pick.contact.id)
            case .pause(let reason):
                pause(reason)
                return
            }
        }
        if force {
            pause("The next \(ShellAutoSend.tries) contacts were skipped: no email address, closed, or emailed recently.")
        } else {
            pace.held(at: Date())
        }
    }

    func countdown(now: Date) -> DateInterval? {
        sending ? nil : pace.countdown(now: now)
    }

    func next() {
        review = queue.isEmpty ? nil : queue.removeFirst()
        waiting = queue.count
    }

    func rate(_ receipt: GenieAutoSendReceipt, rating: Int, note: String) async -> String? {
        guard let client: PackagesClient = PackagesFeature.client else { return "Sign in to rate emails." }
        do {
            try await client.rateCrmAutoEmail(receipt.organization, emailId: receipt.id, rating: rating, note: note)
            return nil
        } catch {
            return PackagesError.describe(error)
        }
    }

    private func show(_ receipt: GenieAutoSendReceipt) {
        GenieSounds.shared.play(.whoosh)
        InboxLocalPings.report(surface: "genie", title: "Auto-sent to \(receipt.contactName)", message: "“\(receipt.subject)” through \(receipt.viaLabel)")
        if review == nil {
            review = receipt
        } else {
            queue.append(receipt)
            waiting = queue.count
        }
    }

    private func pause(_ reason: String) {
        pace.held(at: Date())
        guard pausedFor != reason else { return }
        pausedFor = reason
        FmHaptics.warning()
        status.say("Auto-send is waiting: \(reason)", isError: true)
    }
}
