import FMBase
import Foundation

public enum ProjectLegacyMarker {
    public static let key = "packages/legacy-project"
    public static let legacy = "legacy"
    public static let fresh = "new"

    public static let projectEvidence: [String] = [
        "profile/name", "server/room", "profile/solana", "nodeSync/applied", "node/catalogNodeId",
        "agent/branch-records-v1", "localControl/pairedDesktops",
    ]

    public static let sharedEvidence: [String] = [
        "packages/account/state", "packages/account/catalog", "packages/status-page/monitors",
        "packages/design-system/on", "packages/tour/on", "shell/bar-layout", "shell/menu-layout", "shell/bar-uses",
        "shell/bar-recent", "shell/bar-sort", "appearance/themeMode", "appearance/accent", "bugReport/anchor",
    ]

    public static func stored(_ prefs: KeyValueStore, scope: ProjectScope) -> Bool? {
        switch prefs.string(forKey: scope.key(key)) {
        case legacy: return true
        case fresh: return false
        default: return nil
        }
    }

    public static func evidence(prefs: KeyValueStore, secure: SecureStore, scope: ProjectScope) -> Bool {
        if projectEvidence.contains(where: prefs.contains) { return true }
        if scope.isLegacy && sharedEvidence.contains(where: prefs.contains) { return true }
        let session = secure.read(AuthService.sessionKeyFor(scope.projectId)) ?? ""
        if !scope.projectId.isEmpty && !session.isEmpty { return true }
        if prefs.bool(forKey: AuthService.guestModeKeyFor(scope.projectId)) == true { return true }
        if scope.isLegacy {
            if !(secure.read(AuthService.sessionKey) ?? "").isEmpty { return true }
            if prefs.bool(forKey: AuthService.guestModeKey) == true { return true }
        }
        return false
    }

    public static func markNew(prefs: KeyValueStore, scope: ProjectScope) {
        if scope.isLegacy || stored(prefs, scope: scope) != nil { return }
        prefs.set(fresh, forKey: scope.key(key))
    }

    @discardableResult
    public static func settle(prefs: KeyValueStore, secure: SecureStore, scope: ProjectScope) -> Bool {
        if let known = stored(prefs, scope: scope) { return known }
        let used = evidence(prefs: prefs, secure: secure, scope: scope)
        prefs.set(used ? legacy : fresh, forKey: scope.key(key))
        return used
    }
}
