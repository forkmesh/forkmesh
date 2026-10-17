import FMBase
import Foundation
import Observation

public enum ChatUnlockResult: Equatable, Sendable {
    case unlocked
    case failed
    case unavailable
}

@MainActor
@Observable
public final class ChatPrivacy {
    public static let coverKey = "chat/privacy/app-switcher-cover"
    public static let lockKey = "chat/privacy/lock"
    public static let unlockReason = "Unlock your chats"
    public static let lockedTitle = "Chats are locked"
    public static let unlockButton = "Unlock"
    public static let coverTitle = "Hide chats in the app switcher"
    public static let coverNote = "Covers ForkMesh with a plain screen whenever it leaves the foreground, so the app switcher shows no messages."
    public static let lockUnavailable = "Set a passcode for this iPhone in the Settings app to lock your chats."

    public static func lockTitle(_ method: String?) -> String { "Lock chats with \(method ?? "your passcode")" }

    public static func lockNote(_ method: String?) -> String {
        "After ForkMesh has been in the background, the Chat tab asks for \(either(method)) before it shows any messages."
    }

    public static func lockedMessage(_ method: String?) -> String {
        "Unlock with \(either(method)) to read your chats."
    }

    static func either(_ method: String?) -> String { method.map { "\($0) or your passcode" } ?? "your passcode" }

    public private(set) var coversAppSwitcher: Bool
    public private(set) var lockEnabled: Bool
    public private(set) var locked: Bool
    public private(set) var unlocking = false
    public private(set) var failed = false
    @ObservationIgnored private let store: KeyValueStore?

    public init(store: KeyValueStore?) {
        self.store = store
        coversAppSwitcher = store?.bool(forKey: ChatPrivacy.coverKey, or: true) ?? true
        let lock = store?.bool(forKey: ChatPrivacy.lockKey, or: false) ?? false
        lockEnabled = lock
        locked = lock
    }

    public var showsChat: Bool { !lockEnabled || !locked }

    public var shouldPromptAutomatically: Bool { lockEnabled && locked && !unlocking && !failed }

    public func setCoversAppSwitcher(_ on: Bool) {
        coversAppSwitcher = on
        store?.set(on, forKey: ChatPrivacy.coverKey)
    }

    public func setLockEnabled(_ on: Bool) {
        lockEnabled = on
        store?.set(on, forKey: ChatPrivacy.lockKey)
        if !on {
            locked = false
            failed = false
        }
    }

    public func appBackgrounded() {
        guard lockEnabled else { return }
        locked = true
        failed = false
    }

    public func unlock(manual: Bool, _ authenticate: @MainActor () async -> ChatUnlockResult) async {
        guard lockEnabled, locked, !unlocking, manual || !failed else { return }
        unlocking = true
        let result = await authenticate()
        unlocking = false
        switch result {
        case .unlocked, .unavailable:
            locked = false
            failed = false
        case .failed:
            failed = true
        }
    }
}

@MainActor
public final class DirectFocusReporter {
    public private(set) var reported: String?
    private let apply: @MainActor (String, Bool) -> Void

    public init(apply: @escaping @MainActor (String, Bool) -> Void) {
        self.apply = apply
    }

    public convenience init(hub: ChatHub) {
        self.init { [weak hub] conversation, on in hub?.setFocus(conversation, on) }
    }

    public static func focused(conversation: String, onScreen: Bool, tabHidden: Bool, active: Bool, locked: Bool) -> String? {
        guard onScreen, !tabHidden, active, !locked, DirectMessageFrames.isDirect(conversation) else { return nil }
        return conversation
    }

    public func update(conversation: String, onScreen: Bool, tabHidden: Bool, active: Bool, locked: Bool) {
        let next = DirectFocusReporter.focused(
            conversation: conversation, onScreen: onScreen, tabHidden: tabHidden, active: active, locked: locked)
        if next == reported { return }
        let previous = reported
        reported = next
        if let previous { apply(previous, false) }
        if let next { apply(next, true) }
    }

    public func clear() {
        guard let previous = reported else { return }
        reported = nil
        apply(previous, false)
    }
}
