package com.forkmesh.nativeapp.feature.tools.vault

import android.app.assist.AssistStructure
import android.os.Build
import android.text.InputType
import android.view.View
import android.view.autofill.AutofillId
import androidx.annotation.RequiresApi

@RequiresApi(Build.VERSION_CODES.O)
class VaultLoginForm private constructor(
    val packageName: String,
    val usernameId: AutofillId?,
    val passwordId: AutofillId?,
    val username: String,
    val password: String,
    private val webDomain: String,
    private val webScheme: String,
) {
    val fillable: Boolean get() = usernameId != null || passwordId != null

    val trustedHost: String get() = trustedHostFor(packageName, webDomain, webScheme)

    val inBrowser: Boolean get() = packageName in BROWSERS

    private class Field(val id: AutofillId, val text: String, val password: Boolean, val username: Boolean)

    companion object {
        fun from(structure: AssistStructure): VaultLoginForm {
            val fields = mutableListOf<Field>()
            var domain = ""
            var scheme = ""
            fun visit(node: AssistStructure.ViewNode) {
                node.webDomain?.takeIf { it.isNotBlank() }?.let { domain = it }
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
                    node.webScheme?.takeIf { it.isNotBlank() }?.let { scheme = it }
                }
                val id = node.autofillId
                if (id != null && node.autofillType == View.AUTOFILL_TYPE_TEXT && node.visibility == View.VISIBLE) {
                    val hints = node.autofillHints.orEmpty().map { it.lowercase() }
                    val html = node.htmlInfo?.attributes.orEmpty().associate { it.first.lowercase() to it.second.orEmpty().lowercase() }
                    val named = listOfNotNull(node.idEntry, node.hint, html["name"], html["id"], html["autocomplete"])
                        .joinToString(" ").lowercase()
                    val variation = node.inputType and InputType.TYPE_MASK_VARIATION
                    val textClass = node.inputType and InputType.TYPE_MASK_CLASS == InputType.TYPE_CLASS_TEXT
                    val isPassword = hints.any { "password" in it } || html["type"] == "password" ||
                        (textClass && (variation == InputType.TYPE_TEXT_VARIATION_PASSWORD ||
                            variation == InputType.TYPE_TEXT_VARIATION_WEB_PASSWORD))
                    val isUsername = !isPassword && (
                        hints.any { it == View.AUTOFILL_HINT_USERNAME.lowercase() || it == View.AUTOFILL_HINT_EMAIL_ADDRESS.lowercase() } ||
                            html["type"] == "email" ||
                            (textClass && (variation == InputType.TYPE_TEXT_VARIATION_EMAIL_ADDRESS ||
                                variation == InputType.TYPE_TEXT_VARIATION_WEB_EMAIL_ADDRESS)) ||
                            USERNAME_WORDS.any { it in named })
                    if (isPassword || isUsername || textClass || html["type"] == "text") {
                        fields += Field(id, node.autofillValue?.takeIf { it.isText }?.textValue?.toString().orEmpty(), isPassword, isUsername)
                    }
                }
                for (index in 0 until node.childCount) visit(node.getChildAt(index))
            }
            for (index in 0 until structure.windowNodeCount) visit(structure.getWindowNodeAt(index).rootViewNode)

            val password = fields.firstOrNull { it.password }
            val username = fields.firstOrNull { it.username }
                ?: password?.let { found -> fields.take(fields.indexOf(found)).lastOrNull { !it.password } }
            return VaultLoginForm(
                packageName = structure.activityComponent.packageName,
                usernameId = username?.id,
                passwordId = password?.id,
                username = username?.text.orEmpty(),
                password = password?.text.orEmpty(),
                webDomain = domain,
                webScheme = scheme.lowercase(),
            )
        }

        fun trustedHostFor(packageName: String, webDomain: String, webScheme: String): String {
            if (webDomain.isBlank() || packageName !in BROWSERS) return ""
            if (webScheme.isNotEmpty() && webScheme.lowercase() != "https") return ""
            return webDomain.trim().lowercase().removePrefix("www.")
        }

        private val USERNAME_WORDS = listOf("user", "login", "email", "e-mail", "identifier", "account")

        private val BROWSERS = setOf(
            "com.android.chrome", "com.chrome.beta", "com.chrome.dev", "com.chrome.canary",
            "org.chromium.chrome", "com.google.android.apps.chrome",
            "org.mozilla.firefox", "org.mozilla.firefox_beta", "org.mozilla.fenix", "org.mozilla.focus",
            "org.mozilla.fennec_fdroid", "com.microsoft.emmx", "com.brave.browser", "com.brave.browser_beta",
            "com.sec.android.app.sbrowser", "com.sec.android.app.sbrowser.beta", "com.opera.browser",
            "com.opera.mini.native", "com.vivaldi.browser", "com.duckduckgo.mobile.android",
            "com.kiwibrowser.browser", "org.torproject.torbrowser", "com.ecosia.android",
            "org.bromite.bromite", "app.vanadium.browser", "org.cromite.cromite",
        )
    }
}
