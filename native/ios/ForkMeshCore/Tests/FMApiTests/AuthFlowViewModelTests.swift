// The logic of the auth flow, the guest gate and guest settings (native/android
// AuthFlowLogicTest.kt, GuestGateLogicTest.kt), and drives AuthFlowModel
// against a real AuthService over FakeHTTPClient.

import FMAccount
import FMBase
import FMCrypto
import FMNet
import XCTest
@testable import FMApi

@MainActor
private struct AuthHarness {
    let prefs = InMemoryKeyValueStore([:])
    let secure = InMemorySecureStore([:])
    let settings: SettingsService

    init() {
        settings = SettingsService.create(prefs: prefs, secureStore: secure)
    }

    func auth(_ http: HTTPClient) throws -> AuthService {
        let identity = try Identity.loadOrCreate(secureStore: secure, prefs: prefs)
        return AuthService.create(settings: settings, identity: identity, http: http)
    }
}

private func accountPayload() -> JSONObject {
    [
        "nodeName": "alice", "email": "alice@example.com", "status": "active",
        "sessionToken": "t-1", "sessionKind": "account", "capabilities": ["browse"],
    ]
}

final class AuthFlowViewModelTests: XCTestCase {
    func testLegalLinksPointAtPublicForkmeshDocuments() {
        XCTAssertEqual(LegalLinks.terms.absoluteString, "https://forkmesh.com/terms")
        XCTAssertEqual(LegalLinks.privacy.absoluteString, "https://forkmesh.com/privacy")
    }

    func testAPartiallyFilledFormNeverAuthenticates() {
        XCTAssertEqual(
            loginSubmit(identifier: "demo@example.com", password: "", totp: "", loading: false, debugBuild: true),
            .invalid("Enter your email or username and password."))
        XCTAssertEqual(
            loginSubmit(identifier: "  ", password: "correct-horse", totp: "", loading: false, debugBuild: true),
            .invalid("Enter your email or username and password."))
    }

    func testACompleteFormSendsTheTrimmedIdentifier() {
        XCTAssertEqual(
            loginSubmit(
                identifier: " demo@example.com ", password: " correct-horse ", totp: " 123456 ", loading: false,
                debugBuild: false),
            .send(identifier: "demo@example.com", password: " correct-horse ", totp: " 123456 "))
    }

    func testEmptyFormTakesTheDebugDesignPreviewShortcut() {
        XCTAssertEqual(loginSubmit(identifier: "", password: "", totp: "", loading: false, debugBuild: true), .designPreview)
        XCTAssertEqual(
            loginSubmit(identifier: "", password: "", totp: "", loading: false, debugBuild: false),
            .invalid("Enter your email or username and password."))
    }

    func testContinueIsIgnoredWhileARequestRuns() {
        XCTAssertEqual(loginSubmit(identifier: "a", password: "b", totp: "", loading: true, debugBuild: true), .busy)
        XCTAssertEqual(loginButtonLabel(loading: true), "Logging in...")
        XCTAssertEqual(loginButtonLabel(loading: false), "Continue")
    }

    func testBadTotpRevealsTheAuthenticatorFieldWithoutAnErrorColour() {
        let state = LoginFormState().failed(AuthException("bad_totp", AuthService.messageFor("bad_totp")))
        XCTAssertEqual(state.hint, "Enter your authenticator code.")
        XCTAssertTrue(state.showTotp)
        XCTAssertFalse(state.bad)
        XCTAssertFalse(passwordSubmits(showTotp: state.showTotp))

        // The field stays once shown; a later refusal is an error.
        let refused = state.sending().failed(
            AuthException("invalid_credentials", AuthService.messageFor("invalid_credentials")))
        XCTAssertTrue(refused.showTotp)
        XCTAssertTrue(refused.bad)
        XCTAssertEqual(refused.hint, "Incorrect email, username, or password.")
    }

    func testANonAuthFailureIsANetworkError() {
        let state = LoginFormState().failed(HTTPError.transport("socket closed"))
        XCTAssertEqual(state.hint, "Network error - please try again.")
        XCTAssertTrue(state.bad)
        XCTAssertFalse(state.showTotp)
        XCTAssertTrue(passwordSubmits(showTotp: state.showTotp))
        XCTAssertEqual(state.sending(), LoginFormState())
    }

    func testOnlyTheEmptyFormPathCreatesAPreviewSession() {
        XCTAssertFalse(gateCreatesPreviewSession(isAuthenticated: true))
        XCTAssertTrue(gateCreatesPreviewSession(isAuthenticated: false))
        XCTAssertTrue(signInOverAppCreatesPreviewSession(isAuthenticated: false, debugBuild: true))
        XCTAssertFalse(signInOverAppCreatesPreviewSession(isAuthenticated: false, debugBuild: false))
        XCTAssertFalse(signInOverAppCreatesPreviewSession(isAuthenticated: true, debugBuild: true))
    }

    func testBackReturnsToWelcomeUnlessPushedOverTheApp() {
        XCTAssertTrue(authBackDismisses(pushedOverApp: true))
        XCTAssertFalse(authBackDismisses(pushedOverApp: false))
    }

    func testRelaySubtitleNamesTheRelayOnlyWhenThereAreSeveral() {
        XCTAssertEqual(loginRelaySubtitle(hasMultipleRelays: false, activeLabel: "x"), "Continue with your ForkMesh account.")
        XCTAssertEqual(
            loginRelaySubtitle(hasMultipleRelays: true, activeLabel: "Home relay"),
            "Signing in to Home relay. Your account on your other relays is untouched.")
    }

    // MARK: guest_gate

    func testAHarnessWithoutAuthBehavesAsSignedIn() {
        XCTAssertFalse(isGuestMode(authAvailable: false, isGuest: true))
        XCTAssertTrue(isGuestMode(authAvailable: true, isGuest: true))
        XCTAssertFalse(isGuestMode(authAvailable: true, isGuest: false))
    }

    @MainActor
    func testLetsASignedInUserStraightThrough() async {
        var sheets = 0
        let allowed = await requireAccount(
            guest: false,
            showSheet: { sheets += 1; return nil },
            openSignIn: { _ in XCTFail("no sign-in for a signed-in user") },
            isAuthenticated: { true })
        XCTAssertTrue(allowed)
        XCTAssertEqual(sheets, 0)
    }

    @MainActor
    func testAsksAGuestToSignInAndBlocksTheWrite() async {
        var sheets = 0
        var signIns = 0
        let allowed = await requireAccount(
            guest: true,
            showSheet: { sheets += 1; return nil },  // "Keep exploring"
            openSignIn: { _ in signIns += 1 },
            isAuthenticated: { false })
        XCTAssertFalse(allowed)
        XCTAssertEqual(sheets, 1)
        XCTAssertEqual(signIns, 0)
    }

    @MainActor
    func testLogInOpensTheLoginPageAndBackReturnsWithoutTheWrite() async {
        var opened: [AuthFlowStart] = []
        let allowed = await requireAccount(
            guest: true, showSheet: { .login }, openSignIn: { opened.append($0) }, isAuthenticated: { false })
        XCTAssertFalse(allowed)
        XCTAssertEqual(opened, [.login])
    }

    @MainActor
    func testSigningInFromTheSheetLetsTheWriteContinue() async {
        var signedIn = false
        let allowed = await requireAccount(
            guest: true, showSheet: { .signup }, openSignIn: { _ in signedIn = true }, isAuthenticated: { signedIn })
        XCTAssertTrue(allowed)
    }

    // MARK: AuthFlowModel

    @MainActor
    func testTheModelLogsInAndReportsDone() async throws {
        let h = AuthHarness()
        let http = FakeHTTPClient { _ in HTTPResponse(status: 200, json: .object(accountPayload())) }
        let auth = try h.auth(http)
        let model = AuthFlowModel(auth: auth)
        XCTAssertEqual(model.mode, .welcome)
        model.show(.login)
        model.identifier = " alice "
        model.password = "pw"
        let done = await model.submit()
        XCTAssertTrue(done)
        XCTAssertTrue(auth.isAuthenticated)
        XCTAssertFalse(model.loading)
        XCTAssertEqual(model.form, LoginFormState())
        XCTAssertEqual(bodyText(http.lastRequest).contains("\"alice\""), true)
        model.finishAuthenticated()
        XCTAssertNotEqual(auth.session?.sessionKind, "preview")
    }

    @MainActor
    func testTheModelShowsTheRefusalAndTheAuthenticatorField() async throws {
        let h = AuthHarness()
        let http = FakeHTTPClient { _ in HTTPResponse(status: 401, json: ["error": "bad_totp"]) }
        let model = AuthFlowModel(auth: try h.auth(http), start: .login)
        model.identifier = "alice"
        model.password = "pw"
        let done = await model.submit()
        XCTAssertFalse(done)
        XCTAssertTrue(model.form.showTotp)
        XCTAssertFalse(model.form.bad)
        XCTAssertEqual(model.form.hint, "Enter your authenticator code.")
        XCTAssertFalse(model.passwordSubmits)
    }

    @MainActor
    func testTheModelValidatesBeforeSending() async throws {
        let h = AuthHarness()
        let http = FakeHTTPClient()
        let model = AuthFlowModel(auth: try h.auth(http), start: .login)
        model.identifier = "alice"
        let done = await model.submit()
        XCTAssertFalse(done)
        XCTAssertEqual(model.form.hint, AuthCopy.missingCredentials)
        XCTAssertTrue(model.form.bad)
        XCTAssertTrue(http.requests.isEmpty)
    }

    @MainActor
    func testTheDesignPreviewCreatesThePreviewSessionOnlyInDebug() async throws {
        let h = AuthHarness()
        let auth = try h.auth(FakeHTTPClient())
        let model = AuthFlowModel(auth: auth, start: .login, debugBuild: true)
        let done = await model.submit()
        XCTAssertTrue(done)
        model.finishAuthenticated()
        XCTAssertEqual(auth.session?.sessionKind, "preview")
    }

    @MainActor
    func testBackDismissesAFlowPushedOverTheApp() throws {
        let h = AuthHarness()
        let pushed = AuthFlowModel(auth: try h.auth(FakeHTTPClient()), start: .login, pushedOverApp: true)
        XCTAssertTrue(pushed.back())
        XCTAssertEqual(pushed.mode, .login)
        let front = AuthFlowModel(auth: try h.auth(FakeHTTPClient()), start: .signup)
        XCTAssertFalse(front.back())
        XCTAssertEqual(front.mode, .welcome)
    }

    @MainActor
    func testTheRevokedBannerShowsOnceOnTheLoginPage() throws {
        let h = AuthHarness()
        h.prefs.set(true, forKey: AuthService.revokedBannerKeyFor(h.settings.relays.activeId))
        let auth = try h.auth(FakeHTTPClient())
        XCTAssertTrue(auth.revokedBannerPending)
        let model = AuthFlowModel(auth: auth)
        XCTAssertFalse(model.revokedBanner)
        model.show(.login)
        XCTAssertTrue(model.revokedBanner)
        XCTAssertFalse(auth.revokedBannerPending)
    }

    @MainActor
    func testGuestModeFromWelcome() throws {
        let h = AuthHarness()
        let auth = try h.auth(FakeHTTPClient())
        AuthFlowModel(auth: auth).continueAsGuest()
        XCTAssertTrue(auth.isGuest)
        XCTAssertTrue(auth.hasAppAccess)
    }
}

private func bodyText(_ request: HTTPRequest?) -> String {
    request?.bodyText ?? ""
}
