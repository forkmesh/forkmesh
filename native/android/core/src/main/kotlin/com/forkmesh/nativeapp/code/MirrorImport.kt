package com.forkmesh.nativeapp.code

import com.forkmesh.nativeapp.api.ApiJsonResult
import com.forkmesh.nativeapp.api.ApiService
import com.forkmesh.nativeapp.json.jsonText

sealed class MirrorImportOutcome {
    data class Problem(val message: String) : MirrorImportOutcome()

    data class Status(val message: String) : MirrorImportOutcome()

    data class Done(val notice: String) : MirrorImportOutcome()
}

object MirrorImport {
    val providers: List<String> = listOf("github.com", "gitlab.com", "codeberg.org", "gitea.com")

    const val TITLE = "Mirror from GitHub/GitLab/Codeberg/Gitea"
    const val URL_LABEL = "Repository URL"
    const val URL_HINT = "https://github.com/owner/repository"
    const val TOKEN_LABEL = "Provider token (private repositories only)"
    const val TOKEN_HELPER = "Used for this request only; never stored."
    const val DISCLAIMER =
        "Imports metadata only — issues, branches, releases and the like. ForkMesh does not own or control the " +
            "source repository, and the entry is not a mirror until one is published and verified."
    const val SIGN_IN = "Sign in to import a repository."
    const val READING = "Reading repository metadata…"
    const val CONFIRM = "Import"

    private val SCHEME = Regex("^([A-Za-z][A-Za-z0-9+.-]*):")
    private val HOST_CHARS = Regex("^[A-Za-z0-9._~%!$&'()*+,;=-]*$")

    fun validateSourceUrl(value: String): String? {
        val raw = value.trim()
        if (raw.isEmpty()) return "Paste a repository URL."
        val withScheme = if (raw.contains("://")) raw else "https://$raw"
        val uri = parseUri(withScheme)
        if (uri == null || uri.scheme != "https" || uri.userInfo.isNotEmpty()) {
            return "Use an https:// repository URL."
        }
        val host = uri.host.lowercase().replaceFirst(Regex("^www\\."), "")
        if (host !in providers) {
            return "Only ${providers.joinToString(", ")} repositories can be imported."
        }
        if (uri.pathSegments.size < 2) return "Include the owner and the repository name."
        return null
    }

    private class Parts(val scheme: String, val userInfo: String, val host: String, val pathSegments: List<String>)

    private fun parseUri(text: String): Parts? {
        val schemeMatch = SCHEME.find(text)
        val scheme = schemeMatch?.groupValues?.get(1)?.lowercase() ?: ""
        var rest = if (schemeMatch != null) text.substring(schemeMatch.value.length) else text
        var userInfo = ""
        var host = ""
        if (rest.startsWith("//")) {
            rest = rest.substring(2)
            val authorityEnd = rest.indexOfFirst { it == '/' || it == '?' || it == '#' }.let { if (it < 0) rest.length else it }
            val authority = rest.substring(0, authorityEnd)
            rest = rest.substring(authorityEnd)
            val at = authority.lastIndexOf('@')
            if (at >= 0) userInfo = authority.substring(0, at)
            val hostPort = authority.substring(at + 1)
            host = if (hostPort.startsWith("[")) hostPort.substringBefore(']') + "]" else hostPort.substringBefore(':')
            if (!hostPort.startsWith("[") && !HOST_CHARS.matches(host)) return null
        }
        val path = rest.substringBefore('#').substringBefore('?')
        return Parts(scheme, userInfo, host, path.split('/').filter { it.isNotEmpty() })
    }

    fun explain(answer: ApiJsonResult): String = when (answer.error) {
        "invalid_session" -> "Your session expired. Sign in again."
        "target_owner_forbidden" -> "You cannot import into that owner. Pick an account you administer."
        "source_url_required", "https_provider_url_required", "invalid_provider_url", "invalid_repository_path" ->
            "That does not look like a canonical repository URL."
        "github_owner_and_repository_required" -> "Use the full https://github.com/owner/repository URL."
        "unsupported_provider" -> "Only GitHub, GitLab, Codeberg, and Gitea repositories can be imported."
        "provider_request_failed" -> "The provider did not answer. Try again in a moment."
        "rate_limited" -> "The provider is rate-limiting; try again later."
        "" -> "Import failed (HTTP ${answer.statusCode})."
        else -> "Import failed (${answer.error})."
    }

    fun successNotice(body: Map<String, Any?>): String {
        val created = body["created"] == true
        val notice = jsonText(body["notice"])
        return when {
            notice.isNotEmpty() -> notice
            created -> "Imported the repository metadata. It is listed as an external entry until a ForkMesh mirror is published."
            else -> "Refreshed the existing external listing."
        }
    }

    fun submit(api: ApiService, sourceUrl: String, providerToken: String, accountName: String?): MirrorImportOutcome {
        validateSourceUrl(sourceUrl)?.let { return MirrorImportOutcome.Problem(it) }
        if (accountName.isNullOrEmpty()) return MirrorImportOutcome.Status(SIGN_IN)
        return try {
            val answer = api.createRepositoryImport(
                sourceUrl = sourceUrl.trim(),
                targetOwner = accountName,
                providerToken = providerToken.trim(),
            )
            if (answer.ok) MirrorImportOutcome.Done(successNotice(answer.body)) else MirrorImportOutcome.Status(explain(answer))
        } catch (error: Exception) {
            MirrorImportOutcome.Status("Import failed: $error")
        }
    }
}
