package com.forkmesh.nativeapp.feature.code.collab

import android.content.ActivityNotFoundException
import android.content.ClipData
import android.content.Context
import android.content.Intent
import android.net.Uri
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.Stable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.ui.platform.ClipEntry
import androidx.compose.ui.platform.LocalClipboard
import com.forkmesh.nativeapp.api.ApiService
import com.forkmesh.nativeapp.api.InboxService
import com.forkmesh.nativeapp.collab.dartErrorText
import com.forkmesh.nativeapp.collab.runFailureText
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.feature.account.AccountFeature
import com.forkmesh.nativeapp.feature.account.AuthService
import com.forkmesh.nativeapp.model.auth.AuthSession
import com.forkmesh.nativeapp.platform.AppLifecycle
import com.forkmesh.nativeapp.security.DeviceIdentity
import com.forkmesh.nativeapp.ui.kit.FmMessenger
import com.forkmesh.nativeapp.ui.kit.FmStatus
import com.forkmesh.nativeapp.ui.kit.LocalFmMessenger
import com.forkmesh.nativeapp.ui.kit.rememberFmStatus
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmPalette
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

@Stable
internal class CollabEnv(
    val messenger: FmMessenger,
    val navigator: Navigator,
    val palette: FmPalette,
    val status: FmStatus,
    private val requireAccountGate: suspend (String) -> Boolean,
) {
    val api: ApiService get() = Services.require()

    val inbox: InboxService get() = Services.require()

    val auth: AuthService? get() = Services.get()

    val identity: DeviceIdentity? get() = Services.get()

    suspend fun requireAccount(action: String): Boolean = requireAccountGate(action)

    fun launch(block: suspend CollabEnv.() -> Unit): Job = AppLifecycle.scope.launch { block() }

    suspend fun run(okMessage: String, status: FmStatus = this.status, action: () -> Unit) {
        try {
            withContext(Dispatchers.IO) { action() }
            status.show(okMessage)
        } catch (error: CancellationException) {
            throw error
        } catch (error: Exception) {
            status.error(runFailureText(error))
        }
    }
}

@Composable
internal fun rememberCollabEnv(navigator: Navigator): CollabEnv {
    val messenger = LocalFmMessenger.current
    val requireAccount = AccountFeature.rememberRequireAccount()
    val palette = Fm.colors
    val status = rememberFmStatus()
    return remember(messenger, navigator, palette, status, requireAccount) {
        CollabEnv(messenger, navigator, palette, status, requireAccount)
    }
}

@Composable
internal fun rememberSession(): AuthSession? {
    val flow: StateFlow<AuthSession?> = remember { Services.get<AuthService>()?.session ?: MutableStateFlow(null) }
    val session by flow.collectAsState()
    return session
}

@Composable
internal fun rememberGuestMode(): Boolean {
    val flow: StateFlow<Boolean> = remember { Services.get<AuthService>()?.isGuest ?: MutableStateFlow(false) }
    val guest by flow.collectAsState()
    return guest
}

internal sealed interface Loadable<out T> {
    data object Loading : Loadable<Nothing>

    data class Failed(val message: String) : Loadable<Nothing>

    data class Ready<T>(val value: T) : Loadable<T>
}

@Composable
internal fun <T> rememberLoad(vararg keys: Any?, load: () -> T): Loadable<T> {
    val state = remember(*keys) { mutableStateOf<Loadable<T>>(Loadable.Loading) }
    val latest by rememberUpdatedState(load)
    LaunchedEffect(*keys) {
        state.value = try {
            Loadable.Ready(withContext(Dispatchers.IO) { latest() })
        } catch (error: CancellationException) {
            throw error
        } catch (error: Exception) {
            Loadable.Failed(dartErrorText(error))
        }
    }
    return state.value
}

@Composable
internal fun rememberCopyText(status: FmStatus? = null): (label: String, value: String) -> Unit {
    val clipboard = LocalClipboard.current
    val scope = rememberCoroutineScope()
    return remember(clipboard, scope, status) {
        { label, value ->
            scope.launch {
                clipboard.setClipEntry(ClipEntry(ClipData.newPlainText(label, value)))
                status?.show("$label copied")
            }
        }
    }
}

internal fun openExternalUrl(context: Context, url: String) {
    try {
        context.startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(url)).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK))
    } catch (_: ActivityNotFoundException) {
    }
}
