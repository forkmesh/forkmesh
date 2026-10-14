package com.forkmesh.nativeapp.account

import com.forkmesh.nativeapp.model.auth.AuthException
import com.forkmesh.nativeapp.model.auth.authErrorMessage
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class AuthFlowLogicTest {
    fun testLegalLinksPointAtPublicForkmeshDocuments() {
        assertEq("https://forkmesh.com/terms", LegalLinks.TERMS)
        assertEq("https://forkmesh.com/privacy", LegalLinks.PRIVACY)
    }

    fun testMembershipLinkOpensThePlansOnTheWebsite() {
        assertEq("https://forkmesh.com/#membership", LegalLinks.MEMBERSHIP)
    }

    fun testAfterSignupTheLoginFormSaysToLogInWithoutAnError() {
        val form = LoginFormState(showTotp = true).afterSignup()
        assertEq(AuthCopy.SIGNUP_RETURN_HINT, form.hint)
        expect(!form.bad)
        expect(form.showTotp)
    }

    fun testAPartiallyFilledFormNeverAuthenticates() {
        val missingPassword = loginSubmit("demo@example.com", "", "", loading = false, debugBuild = true)
        assertEq(LoginSubmit.Invalid("Enter your email or username and password."), missingPassword)
        val missingIdentifier = loginSubmit("  ", "correct-horse", "", loading = false, debugBuild = true)
        assertEq(LoginSubmit.Invalid("Enter your email or username and password."), missingIdentifier)
    }

    fun testACompleteFormSendsTheTrimmedIdentifier() {
        assertEq(
            LoginSubmit.Send("demo@example.com", " correct-horse ", " 123456 "),
            loginSubmit(" demo@example.com ", " correct-horse ", " 123456 ", loading = false, debugBuild = false),
        )
    }

    fun testEmptyFormTakesTheDebugDesignPreviewShortcut() {
        assertEq(LoginSubmit.DesignPreview, loginSubmit("", "", "", loading = false, debugBuild = true))
        assertEq(
            LoginSubmit.Invalid("Enter your email or username and password."),
            loginSubmit("", "", "", loading = false, debugBuild = false),
        )
    }

    fun testContinueIsIgnoredWhileARequestRuns() {
        assertEq(LoginSubmit.Busy, loginSubmit("a", "b", "", loading = true, debugBuild = true))
        assertEq("Logging in...", loginButtonLabel(loading = true))
        assertEq("Continue", loginButtonLabel(loading = false))
    }

    fun testBadTotpRevealsTheAuthenticatorFieldWithoutAnErrorColour() {
        val state = LoginFormState().failed(AuthException("bad_totp", authErrorMessage("bad_totp")))
        assertEq("Enter your authenticator code.", state.hint)
        expect(state.showTotp)
        expect(!state.bad)
        expect(!passwordSubmits(state.showTotp))

        val refused = state.sending().failed(AuthException("invalid_credentials", authErrorMessage("invalid_credentials")))
        expect(refused.showTotp)
        expect(refused.bad)
        assertEq("Incorrect email, username, or password.", refused.hint)
    }

    fun testANonAuthFailureIsANetworkError() {
        val state = LoginFormState().failed(IllegalStateException("socket closed"))
        assertEq("Network error - please try again.", state.hint)
        expect(state.bad)
        expect(!state.showTotp)
        expect(passwordSubmits(state.showTotp))
        assertEq(LoginFormState(), state.sending().copy(hint = "", bad = false))
    }

    fun testOnlyTheEmptyFormPathCreatesAPreviewSession() {
        expect(!gateCreatesPreviewSession(isAuthenticated = true))
        expect(gateCreatesPreviewSession(isAuthenticated = false))
        expect(signInOverAppCreatesPreviewSession(isAuthenticated = false, debugBuild = true))
        expect(!signInOverAppCreatesPreviewSession(isAuthenticated = false, debugBuild = false))
        expect(!signInOverAppCreatesPreviewSession(isAuthenticated = true, debugBuild = true))
    }

    fun testBackReturnsToWelcomeUnlessPushedOverTheApp() {
        expect(authBackDismisses(pushedOverApp = true))
        expect(!authBackDismisses(pushedOverApp = false))
    }
}
