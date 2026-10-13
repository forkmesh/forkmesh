// The watch's end of the WatchConnectivity link (docs/native/PARITY.md, 4.2).
// It holds the phone's latest `WatchSnapshot`, sends `WatchCommand`s back,
// leaves the complication summary in the keychain group for the widgets, and
// hands every new beat to `WatchFeedback` (sound and haptics).

import FMBase
import FMWatchWire
import Foundation
import Observation
import WatchConnectivity
import WidgetKit

@MainActor
@Observable
final class WatchStore: NSObject {
    static let shared = WatchStore()

    private(set) var snapshot = WatchSnapshot(signedIn: true)
    private(set) var hasSnapshot = false
    private(set) var reachable = false
    /// Commands in flight, by agent key (their buttons show progress).
    private(set) var busy: Set<String> = []
    /// The last reply worth showing, cleared by the view that shows it.
    var notice: String?
    /// When each agent's newest beat arrived here, for the field's flash.
    private(set) var beatArrivedAt: [String: Date] = [:]
    private(set) var reviewed: [String] = UserDefaults.standard.stringArray(forKey: WatchStore.reviewedKey) ?? []

    let feedback = WatchFeedback()

    @ObservationIgnored private var watchedKey: String?
    @ObservationIgnored private let session: WCSession? = WCSession.isSupported() ? WCSession.default : nil
    @ObservationIgnored private let group = WatchGroup.shared()

    override private init() {
        super.init()
    }

    func activate() {
        guard let session, session.delegate == nil else { return }
        session.delegate = self
        session.activate()
    }

    func connected(within seconds: Double) async -> Bool {
        activate()
        guard let session else { return false }
        var waited: Double = 0
        while !(session.activationState == .activated && session.isReachable), waited < seconds {
            try? await Task.sleep(nanoseconds: 250_000_000)
            waited += 0.25
        }
        return session.activationState == .activated && session.isReachable
    }

    func frame(_ key: String) -> AgentFrame? { snapshot.frame(key) }

    var frames: [AgentFrame] { AgentFrame.ordered(snapshot.frames) }

    // MARK: Commands

    /// Sends a command and waits for the phone's answer. Refresh and the
    /// watched agent fall back to a queued transfer when the phone is out of
    /// reach; controls need it now, so they say why they could not run.
    @discardableResult
    func send(_ command: WatchCommand) async -> WatchReply {
        guard let session, session.activationState == .activated else {
            return .refused("Your iPhone is not connected.")
        }
        guard let envelope = try? WatchWire.envelope(.command, command) else {
            return .refused("That could not be sent.")
        }
        if !session.isReachable {
            switch command {
            case .refresh, .watch, .listen:
                session.transferUserInfo(envelope)
                return .done()
            default:
                return .refused("Your iPhone is out of reach. Bring it closer and try again.")
            }
        }
        let key = command.key
        if let key { busy.insert(key) }
        defer {
            if let key { busy.remove(key) }
        }
        let reply: WatchReply = await withCheckedContinuation { (continuation: CheckedContinuation<WatchReply, Never>) in
            session.sendMessage(envelope, replyHandler: { (answer: [String: Any]) in
                let reply = (try? WatchWire.open(.reply, answer, as: WatchReply.self))
                    ?? .refused("Update ForkMesh on your iPhone to use this.")
                continuation.resume(returning: reply)
            }, errorHandler: { (error: Error) in
                continuation.resume(returning: .refused("Your iPhone did not answer: \(error.localizedDescription)"))
            })
        }
        return reply
    }

    /// Runs a control from a button: the reply's sentence becomes a notice,
    /// and a failure buzzes.
    func perform(_ command: WatchCommand) {
        Task {
            let reply = await send(command)
            if !reply.message.isEmpty { notice = reply.message }
            feedback.acknowledge(reply)
        }
    }

    /// The live view opened (or closed): the phone reads this agent first.
    func setWatched(_ key: String?) {
        if watchedKey == key { return }
        watchedKey = key
        Task { await send(.watch(key: key)) }
    }

    func setListening(_ on: Bool) {
        feedback.setListening(on)
        Task { await send(.listen(on: on)) }
    }

    func diff(_ key: String) async -> WatchDiff {
        guard let session, session.activationState == .activated else {
            return .refused(key, "Your iPhone is not connected.")
        }
        guard session.isReachable else {
            return .refused(key, "Your iPhone is out of reach. Bring it closer and try again.")
        }
        guard let envelope = try? WatchWire.envelope(.command, WatchCommand.diff(key: key)) else {
            return .refused(key, "That could not be sent.")
        }
        return await withCheckedContinuation { (continuation: CheckedContinuation<WatchDiff, Never>) in
            session.sendMessage(envelope, replyHandler: { (answer: [String: Any]) in
                if let diff = try? WatchWire.open(.diff, answer, as: WatchDiff.self) {
                    continuation.resume(returning: diff)
                    return
                }
                let reply = try? WatchWire.open(.reply, answer, as: WatchReply.self)
                let message = reply?.message ?? ""
                continuation.resume(
                    returning: .refused(key, message.isEmpty ? "Update ForkMesh on your iPhone to see changes here." : message))
            }, errorHandler: { (error: Error) in
                continuation.resume(returning: .refused(key, "Your iPhone did not answer: \(error.localizedDescription)"))
            })
        }
    }

    private static let reviewedKey = "fm.watch.reviewed"
    private static let maxReviewed = 600

    func isReviewed(_ file: WatchDiffFile, in diff: WatchDiff) -> Bool {
        reviewed.contains(WatchStore.reviewMark(file, in: diff))
    }

    func setReviewed(_ file: WatchDiffFile, in diff: WatchDiff, _ on: Bool) {
        let mark = WatchStore.reviewMark(file, in: diff)
        reviewed.removeAll { $0 == mark }
        if on { reviewed.append(mark) }
        if reviewed.count > WatchStore.maxReviewed {
            reviewed.removeFirst(reviewed.count - WatchStore.maxReviewed)
        }
        UserDefaults.standard.set(reviewed, forKey: WatchStore.reviewedKey)
    }

    private static func reviewMark(_ file: WatchDiffFile, in diff: WatchDiff) -> String {
        "\(diff.key)|\(diff.branch)|\(file.reviewMark)"
    }

    // MARK: Snapshots

    fileprivate func receive(_ next: WatchSnapshot) {
        let previous = snapshot
        snapshot = next
        hasSnapshot = true
        let now = Date()
        for frame in next.frames {
            let old = previous.frame(frame.key)
            let fresh = WatchWire.newBeats(previous: old, next: frame)
            if !fresh.isEmpty { beatArrivedAt[frame.key] = now }
            let isWatched = frame.key == watchedKey
            feedback.beats(fresh, of: frame, watched: isWatched)
            if let old, old.status != frame.status {
                feedback.statusChanged(frame, from: old.status, watched: isWatched)
            }
        }
        saveSummary(next.summary)
    }

    fileprivate func receiveSummary(_ summary: ComplicationSummary) {
        saveSummary(summary)
    }

    fileprivate func setReachable(_ value: Bool) {
        reachable = value
    }

    private func saveSummary(_ summary: ComplicationSummary) {
        let old = WatchGroup.load(from: group)
        WatchGroup.save(summary, to: group)
        var comparable = summary
        comparable.generatedAtMs = old?.generatedAtMs ?? 0
        if comparable != old {
            WidgetCenter.shared.reloadTimelines(ofKind: WatchGroup.widgetKind)
        }
    }
}

extension WatchStore: WCSessionDelegate {
    nonisolated func session(
        _ session: WCSession, activationDidCompleteWith activationState: WCSessionActivationState, error: Error?
    ) {
        let context = session.receivedApplicationContext
        let reachable = session.isReachable
        let snapshot = try? WatchWire.open(.snapshot, context, as: WatchSnapshot.self)
        Task { @MainActor in
            let store = WatchStore.shared
            store.setReachable(reachable)
            if let snapshot { store.receive(snapshot) }
            await store.send(.refresh)
        }
    }

    nonisolated func sessionReachabilityDidChange(_ session: WCSession) {
        let reachable = session.isReachable
        Task { @MainActor in
            WatchStore.shared.setReachable(reachable)
            if reachable { await WatchStore.shared.send(.refresh) }
        }
    }

    nonisolated func session(_ session: WCSession, didReceiveApplicationContext applicationContext: [String: Any]) {
        guard let snapshot = try? WatchWire.open(.snapshot, applicationContext, as: WatchSnapshot.self) else { return }
        Task { @MainActor in WatchStore.shared.receive(snapshot) }
    }

    nonisolated func session(_ session: WCSession, didReceiveMessage message: [String: Any]) {
        guard let snapshot = try? WatchWire.open(.snapshot, message, as: WatchSnapshot.self) else { return }
        Task { @MainActor in WatchStore.shared.receive(snapshot) }
    }

    nonisolated func session(_ session: WCSession, didReceiveUserInfo userInfo: [String: Any] = [:]) {
        guard let summary = try? WatchWire.open(.summary, userInfo, as: ComplicationSummary.self) else { return }
        Task { @MainActor in WatchStore.shared.receiveSummary(summary) }
    }
}
