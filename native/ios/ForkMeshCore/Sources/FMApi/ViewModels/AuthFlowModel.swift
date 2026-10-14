// View model of the auth flow and its login screen, plus the onAuthenticated
// half of the app's auth home and the guest gate's `openSignIn`. The view
// (Features/Account/AuthFlowView) binds to it and lays the pages out.

import FMAccount
import FMBase
import Foundation
import Observation

@MainActor
@Observable
public final class AuthFlowModel {
    /// The page on screen.
    public var mode: AuthFlowStart

    // The login form.
    public var identifier = ""
    public var password = ""
    public var totp = ""
    public private(set) var loading = false
    public private(set) var form = LoginFormState()

    /// One-shot: set when the web app revoked this phone's node (or its bound
    /// session became invalid). AuthService arms the flag; the first login
    /// screen consumes it so it appears once.
    public private(set) var revokedBanner = false

    /// Pushed over the app (a guest choosing to sign in) rather than standing
    /// in front of it: Back leaves the flow instead of returning to Welcome.
    public let pushedOverApp: Bool
    /// `kDebugMode`: the empty-form design-preview shortcut exists only here.
    public let debugBuild: Bool

    @ObservationIgnored private let auth: AuthService?
    @ObservationIgnored private var bannerConsumed = false

    public init(
        auth: AuthService?,
        start: AuthFlowStart = .welcome,
        pushedOverApp: Bool = false,
        debugBuild: Bool = false
    ) {
        self.auth = auth
        self.mode = start
        self.pushedOverApp = pushedOverApp
        self.debugBuild = debugBuild
        if start == .login { consumeRevokedBanner() }
    }

    /// `_show`: switch pages. Opening the login page consumes the revoked
    /// banner, as mounting `_LoginScreen` does.
    public func show(_ page: AuthFlowStart) {
        mode = page
        if page == .login { consumeRevokedBanner() }
    }

    /// What Back does on login and signup: true means the caller dismisses the
    /// flow; otherwise the flow went back to Welcome.
    @discardableResult
    public func back() -> Bool {
        if authBackDismisses(pushedOverApp: pushedOverApp) { return true }
        show(.welcome)
        return false
    }

    private func consumeRevokedBanner() {
        if bannerConsumed { return }
        bannerConsumed = true
        if let auth, auth.revokedBannerPending {
            revokedBanner = true
            auth.clearRevokedBanner()
        }
    }

    public var relaySubtitle: String {
        guard let relays = auth?.settings.relays else {
            return loginRelaySubtitle(hasMultipleRelays: false, activeLabel: "")
        }
        return loginRelaySubtitle(hasMultipleRelays: relays.hasMultiple, activeLabel: relays.active.label)
    }

    public var buttonLabel: String { loginButtonLabel(loading: loading) }

    /// Whether Return in the password field submits (it moves on to the
    /// authenticator field while that shows).
    public var passwordSubmits: Bool { FMApi.passwordSubmits(showTotp: form.showTotp) }

    /// The secure signup page (`AuthService.signupUrl`).
    public var signupURL: URL? { auth?.signupUrl }

    public var forgotPasswordURL: URL? { auth?.forgotPasswordUrl }

    public func signupOpened() {
        show(.login)
        form = LoginFormState(hint: AuthCopy.signupReturnHint, bad: false, showTotp: form.showTotp)
    }

    /// `_submit`. Returns true when the flow is done and the caller should run
    /// `onAuthenticated` (a real login, or the debug design preview).
    @discardableResult
    public func submit() async -> Bool {
        switch loginSubmit(
            identifier: identifier, password: password, totp: totp, loading: loading, debugBuild: debugBuild)
        {
        case .busy:
            return false
        case .designPreview:
            return true
        case .invalid(let hint):
            form = form.invalid(hint)
            return false
        case .send(let cleanIdentifier, let cleanPassword, let code):
            guard let auth else {
                form = form.failed(AuthException("identity_unavailable", AuthCopy.networkError))
                return false
            }
            loading = true
            form = form.sending()
            defer { loading = false }
            do {
                _ = try await auth.login(identifier: cleanIdentifier, password: cleanPassword, totp: code)
                return true
            } catch {
                form = form.failed(error)
                return false
            }
        }
    }

    /// `onAuthenticated`, also when pushed over the app: a real login has
    /// already written the session; only the empty form's design preview
    /// reaches here without one, and creates the preview session.
    public func finishAuthenticated() {
        guard let auth else { return }
        let createPreview = pushedOverApp
            ? signInOverAppCreatesPreviewSession(isAuthenticated: auth.isAuthenticated, debugBuild: debugBuild)
            : gateCreatesPreviewSession(isAuthenticated: auth.isAuthenticated)
        if createPreview { auth.authenticatePreview() }
    }

    /// "Explore as guest" on the welcome page.
    public func continueAsGuest() {
        auth?.enterGuestMode()
    }
}
