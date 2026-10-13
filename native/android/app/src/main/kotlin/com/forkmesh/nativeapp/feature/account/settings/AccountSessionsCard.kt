package com.forkmesh.nativeapp.feature.account.settings

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.api.ApiService
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.feature.account.AuthService
import com.forkmesh.nativeapp.settings.AccountSignIn
import com.forkmesh.nativeapp.settings.AccountSignIns
import com.forkmesh.nativeapp.ui.kit.FmAlertDialog
import com.forkmesh.nativeapp.ui.kit.FmCircularProgressIndicator
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmInlineStatus
import com.forkmesh.nativeapp.ui.kit.FmMessenger
import com.forkmesh.nativeapp.ui.kit.FmStatus
import com.forkmesh.nativeapp.ui.kit.FmTextButton
import com.forkmesh.nativeapp.ui.kit.LocalFmMessenger
import com.forkmesh.nativeapp.ui.kit.rememberFmStatus
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmRadius
import com.forkmesh.nativeapp.ui.theme.FmSpace
import com.forkmesh.nativeapp.ui.theme.FmType
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

private suspend fun confirmRevoke(messenger: FmMessenger, title: String, body: String, action: String): Boolean =
    messenger.showDialog<Boolean> { done ->
        FmAlertDialog(
            onDismissRequest = { done(null) },
            title = title,
            containerColor = Fm.colors.bgOverlay,
            shape = RoundedCornerShape(FmRadius.lg),
            actions = {
                FmTextButton(onClick = { done(false) }) { Text("Cancel") }
                Button(onClick = { done(true) }, colors = ButtonDefaults.buttonColors(containerColor = Fm.colors.danger)) { Text(action) }
            },
            content = { Text(body) },
        )
    } == true

@Composable
internal fun AccountSessionsCard(token: String, auth: AuthService, signedOutStatus: FmStatus? = null) {
    val api = Services.get<ApiService>()
    val scope = rememberCoroutineScope()
    val messenger = LocalFmMessenger.current
    val revokeStatus = rememberFmStatus()
    val c = Fm.colors
    var sessions by remember(token) { mutableStateOf<List<AccountSignIn>?>(null) }
    var loading by remember(token) { mutableStateOf(false) }
    var error by remember(token) { mutableStateOf("") }
    var busyId by remember(token) { mutableStateOf("") }

    fun load() {
        if (api == null || loading) return
        loading = true
        error = ""
        scope.launch {
            try {
                sessions = withContext(Dispatchers.IO) { api.listSessions() }
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (_: Exception) {
                error = AccountSignIns.LOAD_FAILED
            } finally {
                loading = false
            }
        }
    }

    fun revoke(target: String, title: String, body: String, action: String) {
        if (api == null || busyId.isNotEmpty()) return
        scope.launch {
            if (!confirmRevoke(messenger, title, body, action)) return@launch
            busyId = target
            error = ""
            revokeStatus.clear()
            try {
                val currentRevoked = withContext(Dispatchers.IO) { api.revokeSession(target) }
                if (currentRevoked) {
                    auth.logout()
                    signedOutStatus?.show(AccountSignIns.SIGNED_OUT_HERE)
                    return@launch
                }
                sessions = AccountSignIns.remaining(sessions.orEmpty(), target)
                revokeStatus.show(AccountSignIns.revokedText(target))
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (_: Exception) {
                error = AccountSignIns.REVOKE_FAILED
            } finally {
                busyId = ""
            }
        }
    }

    LaunchedEffect(token) { load() }

    val list = sessions.orEmpty()
    val others = AccountSignIns.others(list)
    val secondary = Fm.text.bodySmall.copy(color = c.textSecondary)
    Column(Modifier.fillMaxWidth()) {
        Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
            Text(
                AccountSignIns.headline(sessions),
                modifier = Modifier.weight(1f),
                style = Fm.text.bodyMedium.copy(fontWeight = FmType.emphasis),
            )
            if (loading) {
                FmCircularProgressIndicator(Modifier.size(16.dp), strokeWidth = 2.dp)
            } else if (api != null) {
                FmIconButton(icon = "refresh", label = "Refresh sessions", onPressed = { load() }, iconSize = 18.dp)
            }
        }
        Spacer(Modifier.height(FmSpace.x2))
        Text(AccountSignIns.BLURB, style = secondary)
        if (error.isNotEmpty()) {
            Spacer(Modifier.height(FmSpace.x2))
            Text(error, style = Fm.text.bodySmall.copy(color = c.warning))
        }
        if (revokeStatus.message != null) {
            Spacer(Modifier.height(FmSpace.x2))
            FmInlineStatus(revokeStatus)
        }
        Spacer(Modifier.height(FmSpace.x3))
        if (list.isEmpty()) {
            Text(AccountSignIns.emptyText(loading, sessions != null), style = secondary)
        } else {
            Column(verticalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                for (session in list) {
                    SessionRow(
                        session = session,
                        busy = busyId == session.id || busyId == AccountSignIns.OTHERS,
                        enabled = busyId.isEmpty(),
                        onLogOut = {
                            revoke(session.id, AccountSignIns.confirmTitle(session), AccountSignIns.confirmBody(session), "Log out")
                        },
                    )
                }
            }
        }
        if (others > 0) {
            Spacer(Modifier.height(FmSpace.x3))
            FmTextButton(
                onClick = {
                    revoke(AccountSignIns.OTHERS, "Log out all other sessions?", AccountSignIns.confirmOthersBody(others), "Log out others")
                },
                enabled = busyId.isEmpty(),
                contentColor = c.danger,
            ) {
                FmIcon("logout", size = 18.dp, tint = c.danger)
                Spacer(Modifier.width(FmSpace.x2))
                Text("Log out all other sessions")
            }
        }
    }
}

@Composable
private fun SessionRow(session: AccountSignIn, busy: Boolean, enabled: Boolean, onLogOut: () -> Unit) {
    val c = Fm.colors
    val color = if (session.current) c.success else c.textSecondary
    val meta = session.meta()
    Row(
        Modifier
            .fillMaxWidth()
            .background(c.bgBase, RoundedCornerShape(FmRadius.md))
            .padding(FmSpace.x3),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Box(
            Modifier
                .size(36.dp)
                .background(color.copy(alpha = 28f / 255f), RoundedCornerShape(FmRadius.md)),
            contentAlignment = Alignment.Center,
        ) {
            FmIcon(session.icon, size = 20.dp, tint = color)
        }
        Spacer(Modifier.width(FmSpace.x3))
        Column(Modifier.weight(1f)) {
            Text(session.title, maxLines = 1, overflow = TextOverflow.Ellipsis, style = Fm.text.bodyMedium.copy(fontWeight = FmType.emphasis))
            if (meta.isNotEmpty()) {
                Spacer(Modifier.height(FmSpace.x1))
                Text(meta, maxLines = 1, overflow = TextOverflow.Ellipsis, style = Fm.text.labelSmall.copy(color = c.textTertiary))
            }
        }
        Spacer(Modifier.width(FmSpace.x2))
        if (busy) {
            FmCircularProgressIndicator(Modifier.size(16.dp), strokeWidth = 2.dp)
        } else {
            FmTextButton(onClick = onLogOut, enabled = enabled, contentColor = c.danger) { Text("Log out") }
        }
    }
}
