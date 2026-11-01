package com.forkmesh.nativeapp.feature.mastodon

import android.content.Intent
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateListOf
import androidx.compose.runtime.mutableStateMapOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import com.forkmesh.nativeapp.App
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.feature.navigation.NavigationFeature
import com.forkmesh.nativeapp.mastodon.MastodonClient
import com.forkmesh.nativeapp.mastodon.MastodonException
import com.forkmesh.nativeapp.mastodon.MastodonInstanceLimits
import com.forkmesh.nativeapp.mastodon.Mastodon
import com.forkmesh.nativeapp.mastodon.MastodonSession
import com.forkmesh.nativeapp.mastodon.MastodonSignIn
import com.forkmesh.nativeapp.mastodon.MastodonStatus
import com.forkmesh.nativeapp.mastodon.MastodonStore
import com.forkmesh.nativeapp.mastodon.MastodonUnauthorized
import com.forkmesh.nativeapp.navigation.Route
import com.forkmesh.nativeapp.platform.SecureStore
import com.forkmesh.nativeapp.security.SecretStore
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.MainScope
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

object MastodonFeature {
    private const val OWNER = "WS20"

    private var app: App? = null
    private val scope = MainScope()

    fun install(app: App) {
        this.app = app
        NavigationFeature.register(Route.Kind.Mastodon, OWNER) { MastodonScreen() }
    }

    fun store(): MastodonStore? {
        val secrets = Services.get<SecretStore>() ?: app?.let(::SecureStore) ?: return null
        return MastodonStore(secrets)
    }

    fun onIntent(navigator: com.forkmesh.nativeapp.ui.navigation.Navigator, intent: Intent?): Boolean {
        if (intent?.action != Intent.ACTION_VIEW) return false
        val data = intent.data ?: return false
        if (!data.scheme.equals("forkmesh", ignoreCase = true) || !data.host.equals("mastodon", ignoreCase = true) || data.path != "/callback") return false
        intent.action = null
        val callback = data.toString()
        scope.launch {
            val error = data.getQueryParameter("error")
            if (error != null) {
                MastodonState.signInFailed(data.getQueryParameter("error_description")?.takeIf { it.isNotBlank() } ?: "The server did not finish the sign-in.")
            } else {
                MastodonState.finish(callback)
            }
            NavigationFeature.open(Route.Mastodon)
        }
        return true
    }
}

enum class MastodonToggle { Favourite, Boost, Bookmark }

object MastodonState {
    var session by mutableStateOf<MastodonSession?>(null)
        private set
    var loaded by mutableStateOf(false)
        private set
    var server by mutableStateOf("")
    var notice by mutableStateOf("")
    var noticeIsError by mutableStateOf(false)
    var busy by mutableStateOf(false)
    var limits by mutableStateOf(MastodonInstanceLimits())
    var generation by mutableStateOf(0)
        private set
    val patched = mutableStateMapOf<String, MastodonStatus>()
    val removed = mutableStateListOf<String>()
    val revealed = mutableStateMapOf<String, Boolean>()

    fun load() {
        if (loaded) return
        val store = MastodonFeature.store()
        session = store?.session
        if (server.isEmpty()) server = session?.server?.removePrefix("https://") ?: store?.pending?.server?.removePrefix("https://").orEmpty()
        loaded = true
    }

    fun current(status: MastodonStatus): MastodonStatus = patched[status.id] ?: status

    fun client(): MastodonClient? = session?.let { MastodonClient(it.server, it.token) }

    fun show(message: String, error: Boolean = false) {
        notice = message
        noticeIsError = error
    }

    fun clearNotice() {
        notice = ""
        noticeIsError = false
    }

    fun signInFailed(message: String) {
        busy = false
        show(message, error = true)
    }

    private fun signIn(): MastodonSignIn? = MastodonFeature.store()?.let { MastodonSignIn(it) }

    suspend fun start(): String? = attempt("Could not start the sign-in.") { signIn()?.start(server) }

    suspend fun startCode(): String? = attempt("Could not start the sign-in.") { signIn()?.start(server, Mastodon.OOB) }

    suspend fun finish(input: String) {
        attempt("Could not finish the sign-in.") { signIn()?.finish(input) }?.let(::signedIn)
    }

    suspend fun withToken(token: String) {
        attempt("Could not sign in with that token.") { signIn()?.withToken(server, token) }?.let(::signedIn)
    }

    private fun signedIn(value: MastodonSession) {
        session = value
        server = value.server.removePrefix("https://")
        limits = MastodonInstanceLimits()
        patched.clear()
        removed.clear()
        generation++
        clearNotice()
    }

    suspend fun signOut() {
        busy = true
        withContext(Dispatchers.IO) { runCatching { signIn()?.signOut() } }
        busy = false
        session = null
        patched.clear()
        removed.clear()
        generation++
        show("Signed out of Mastodon.")
    }

    fun revoked() {
        val current = session ?: return
        MastodonFeature.store()?.session = null
        session = null
        server = current.server.removePrefix("https://")
        patched.clear()
        removed.clear()
        generation++
        show(Mastodon.revokedNotice(current.server), error = true)
    }

    suspend fun refreshLimits() {
        val client = client() ?: return
        limits = withContext(Dispatchers.IO) { runCatching { client.instance() }.getOrNull() } ?: return
    }

    suspend fun toggle(status: MastodonStatus, kind: MastodonToggle): String? {
        val client = client() ?: return null
        val before = current(status)
        val after = when (kind) {
            MastodonToggle.Favourite -> before.copy(
                favourited = !before.favourited,
                favouritesCount = (before.favouritesCount + if (before.favourited) -1 else 1).coerceAtLeast(0),
            )
            MastodonToggle.Boost -> before.copy(
                reblogged = !before.reblogged,
                reblogsCount = (before.reblogsCount + if (before.reblogged) -1 else 1).coerceAtLeast(0),
            )
            MastodonToggle.Bookmark -> before.copy(bookmarked = !before.bookmarked)
        }
        patched[status.id] = after
        return try {
            val updated = withContext(Dispatchers.IO) {
                when (kind) {
                    MastodonToggle.Favourite -> client.favourite(status.id, after.favourited)
                    MastodonToggle.Boost -> client.reblog(status.id, after.reblogged)
                    MastodonToggle.Bookmark -> client.bookmark(status.id, after.bookmarked)
                }
            }
            patched[status.id] = updated
            null
        } catch (failure: CancellationException) {
            patched[status.id] = before
            throw failure
        } catch (_: MastodonUnauthorized) {
            revoked()
            null
        } catch (failure: Exception) {
            patched[status.id] = before
            "Could not update the post: ${describe(failure)}"
        }
    }

    suspend fun delete(status: MastodonStatus): String? {
        val client = client() ?: return null
        return try {
            withContext(Dispatchers.IO) { client.delete(status.id) }
            removed += status.id
            null
        } catch (failure: CancellationException) {
            throw failure
        } catch (_: MastodonUnauthorized) {
            revoked()
            null
        } catch (failure: Exception) {
            "Could not delete the post: ${describe(failure)}"
        }
    }

    fun describe(failure: Throwable): String = when (failure) {
        is MastodonUnauthorized -> "Mastodon refused the sign-in."
        is MastodonException -> failure.message.orEmpty()
        else -> failure.message?.takeIf { it.isNotBlank() } ?: "Something went wrong."
    }

    private suspend fun <T> attempt(fallback: String, block: () -> T?): T? {
        busy = true
        clearNotice()
        return try {
            withContext(Dispatchers.IO) { block() } ?: run {
                show(fallback, error = true)
                null
            }
        } catch (failure: CancellationException) {
            throw failure
        } catch (failure: Exception) {
            show(if (failure is MastodonException) failure.message.orEmpty() else fallback, error = true)
            null
        } finally {
            busy = false
        }
    }
}
