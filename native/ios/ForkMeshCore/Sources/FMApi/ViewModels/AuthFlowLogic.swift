// The pure logic of the auth flow (login validation, the design-preview path,
// the TOTP field and its hints, Back), the app's auth home and the decision
// half of the guest gate (isGuestMode, requireAccount). Follows native/android
// core/account/AuthFlowLogic.kt and GuestGateLogic.kt.

import FMAccount
import FMBase
import Foundation

/// The page the auth flow opens on (`AuthFlowStart`).
public enum AuthFlowStart: String, Sendable, CaseIterable {
    case welcome
    case login
    case signup
}

/// The auth flow's fixed sentences.
public enum AuthCopy {
    public static let missingCredentials = "Enter your email or username and password."
    public static let networkError = "Network error - please try again."
    public static let revokedDetail =
        "Your local repositories were kept. Sign in again to register this phone as a new node."
    public static let welcomeHeadline = "Build open, sync easy"
    public static let welcomeBody =
        "Private relay chat, repository mirrors, and signed collaboration from your phone."
    public static let loginTitle = "Welcome back"
    public static let signupTitle = "Create your account"
    public static let signupSubtitle =
        "For spam protection, new accounts are created only on the secure ForkMesh website."
    public static let signupBody =
        "The signup page issues a short-lived, one-use browser check. After you confirm your email, return here and log in."
    public static let signupReturnHint = "When your email is confirmed, log in here with your new account."
    public static let forgotPassword = "Forgot password?"
}

/// What Continue on the login form does (`_LoginScreenState._submit`).
public enum LoginSubmit: Equatable, Sendable {
    /// A request is already running: ignore the tap.
    case busy
    /// Debug builds only: a completely empty form calls onAuthenticated without
    /// a request (the design-preview path). Release builds fall through to the
    /// validation, so an empty form can never bypass real authentication.
    case designPreview
    /// Something is missing; show the hint as an error.
    case invalid(String)
    /// Log in with the trimmed identifier; the password and code go as typed.
    case send(identifier: String, password: String, totp: String)
}

public func loginSubmit(
    identifier: String,
    password: String,
    totp: String,
    loading: Bool,
    debugBuild: Bool
) -> LoginSubmit {
    if loading { return .busy }
    let cleanIdentifier = identifier.dartTrimmed
    if cleanIdentifier.isEmpty && password.isEmpty && debugBuild { return .designPreview }
    if cleanIdentifier.isEmpty || password.isEmpty { return .invalid(AuthCopy.missingCredentials) }
    return .send(identifier: cleanIdentifier, password: password, totp: totp)
}

/// The login form's hint line (success or danger colour) and whether the
/// authenticator field shows.
public struct LoginFormState: Equatable, Sendable {
    public var hint: String
    public var bad: Bool
    public var showTotp: Bool

    public init(hint: String = "", bad: Bool = false, showTotp: Bool = false) {
        self.hint = hint
        self.bad = bad
        self.showTotp = showTotp
    }

    public func invalid(_ message: String) -> LoginFormState {
        LoginFormState(hint: message, bad: true, showTotp: showTotp)
    }

    /// A request starts: the hint clears, the authenticator field stays.
    public func sending() -> LoginFormState {
        LoginFormState(hint: "", bad: false, showTotp: showTotp)
    }

    /// A refused or failed login. An `AuthException` shows its sentence;
    /// `bad_totp` reveals the authenticator field and is not drawn as an error.
    /// Anything else is a network error.
    public func failed(_ error: Error) -> LoginFormState {
        if let auth = error as? AuthException {
            return LoginFormState(
                hint: auth.message,
                bad: auth.code != "bad_totp",
                showTotp: auth.code == "bad_totp" || showTotp)
        }
        return LoginFormState(hint: AuthCopy.networkError, bad: true, showTotp: showTotp)
    }
}

public func loginButtonLabel(loading: Bool) -> String { loading ? "Logging in..." : "Continue" }

/// The password field submits the form unless the authenticator field follows it.
public func passwordSubmits(showTotp: Bool) -> Bool { !showTotp }

/// The header's Back on login and signup: a flow pushed over the app (a guest
/// choosing to sign in) is dismissed; the flow in front of the app returns to
/// the welcome screen.
public func authBackDismisses(pushedOverApp: Bool) -> Bool { pushedOverApp }

/// After onAuthenticated a real login has already written
/// the session, so only the empty-form preview path reaches here without one
/// and creates the preview session.
public func gateCreatesPreviewSession(isAuthenticated: Bool) -> Bool { !isAuthenticated }

/// The same for sign-in pushed over the app, and only in debug builds.
public func signInOverAppCreatesPreviewSession(isAuthenticated: Bool, debugBuild: Bool) -> Bool {
    !isAuthenticated && debugBuild
}

/// `_relaySubtitle`: what the login form says it is signing into. With one
/// relay the generic line; with several, the active relay's name, so someone
/// with two accounts can tell which one is being asked for.
public func loginRelaySubtitle(hasMultipleRelays: Bool, activeLabel: String) -> String {
    if !hasMultipleRelays { return "Continue with your ForkMesh account." }
    return "Signing in to \(activeLabel). Your account on your other relays is untouched."
}

// MARK: - Guest gate

/// `isGuestMode`: true while exploring without an account. Without an auth
/// service (a harness) the app behaves as signed in, as it did before guest
/// mode existed.
public func isGuestMode(authAvailable: Bool, isGuest: Bool) -> Bool { authAvailable && isGuest }

/// `requireAccount`: whether the caller may go ahead with a write.
///
/// Anyone who is not a guest passes straight through. A guest sees the sign-in
/// sheet: `showSheet` resolves to the chosen start page (`.login` or
/// `.signup`) or nil for "Keep exploring". A choice opens sign-in over the
/// app, and `openSignIn` returns once that flow closes. The write goes ahead
/// only when the user is signed in by then.
@MainActor
public func requireAccount(
    guest: Bool,
    showSheet: @MainActor () async -> AuthFlowStart?,
    openSignIn: @MainActor (AuthFlowStart) async -> Void,
    isAuthenticated: @MainActor () -> Bool
) async -> Bool {
    if !guest { return true }
    if let choice = await showSheet() {
        await openSignIn(choice)
    }
    return isAuthenticated()
}
