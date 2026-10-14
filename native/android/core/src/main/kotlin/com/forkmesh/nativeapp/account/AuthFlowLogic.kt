package com.forkmesh.nativeapp.account

import com.forkmesh.nativeapp.model.auth.AuthException

enum class AuthFlowStart { Welcome, Login, Signup }

object LegalLinks {
    const val TERMS = "https://forkmesh.com/terms"
    const val PRIVACY = "https://forkmesh.com/privacy"

    const val MEMBERSHIP = "https://forkmesh.com/#membership"

    const val AD_NETWORK = "https://forkmesh.com/docs/operations/ad-network/"
}

object AuthCopy {
    const val MISSING_CREDENTIALS = "Enter your email or username and password."
    const val NETWORK_ERROR = "Network error - please try again."
    const val REVOKED_DETAIL =
        "Your local repositories were kept. Sign in again to register this phone as a new node."
    const val SIGNUP_RETURN_HINT = "When your email is confirmed, log in here with your new account."
    const val FORGOT_PASSWORD = "Forgot password?"
}

sealed class LoginSubmit {
    data object Busy : LoginSubmit()

    data object DesignPreview : LoginSubmit()

    data class Invalid(val hint: String) : LoginSubmit()

    data class Send(val identifier: String, val password: String, val totp: String) : LoginSubmit()
}

fun loginSubmit(
    identifier: String,
    password: String,
    totp: String,
    loading: Boolean,
    debugBuild: Boolean,
): LoginSubmit {
    if (loading) return LoginSubmit.Busy
    val cleanIdentifier = identifier.trim()
    if (cleanIdentifier.isEmpty() && password.isEmpty() && debugBuild) return LoginSubmit.DesignPreview
    if (cleanIdentifier.isEmpty() || password.isEmpty()) return LoginSubmit.Invalid(AuthCopy.MISSING_CREDENTIALS)
    return LoginSubmit.Send(cleanIdentifier, password, totp)
}

data class LoginFormState(
    val hint: String = "",
    val bad: Boolean = false,
    val showTotp: Boolean = false,
) {
    fun invalid(message: String): LoginFormState = copy(hint = message, bad = true)

    fun sending(): LoginFormState = copy(hint = "", bad = false)

    fun afterSignup(): LoginFormState = copy(hint = AuthCopy.SIGNUP_RETURN_HINT, bad = false)

    fun failed(error: Throwable): LoginFormState = if (error is AuthException) {
        copy(hint = error.message, bad = error.code != "bad_totp", showTotp = error.code == "bad_totp" || showTotp)
    } else {
        copy(hint = AuthCopy.NETWORK_ERROR, bad = true)
    }
}

fun loginButtonLabel(loading: Boolean): String = if (loading) "Logging in..." else "Continue"

fun passwordSubmits(showTotp: Boolean): Boolean = !showTotp

fun authBackDismisses(pushedOverApp: Boolean): Boolean = pushedOverApp

fun gateCreatesPreviewSession(isAuthenticated: Boolean): Boolean = !isAuthenticated

fun signInOverAppCreatesPreviewSession(isAuthenticated: Boolean, debugBuild: Boolean): Boolean =
    !isAuthenticated && debugBuild
