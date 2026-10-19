package com.forkmesh.nativeapp.feature.runners

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.feature.agents.RunnerAccountsFeature
import com.forkmesh.nativeapp.feature.agents.RunnerAttentionFeature
import com.forkmesh.nativeapp.feature.code.DetailScaffold
import com.forkmesh.nativeapp.feature.navigation.NavigationFeature
import com.forkmesh.nativeapp.runners.RunnerAction
import com.forkmesh.nativeapp.runners.RunnerAttention
import com.forkmesh.nativeapp.runners.RunnerAuth
import com.forkmesh.nativeapp.runners.RunnerList
import com.forkmesh.nativeapp.runners.RunnerListResult
import com.forkmesh.nativeapp.runners.RunnerProviderState
import com.forkmesh.nativeapp.runners.RunnerRow
import com.forkmesh.nativeapp.runners.RunnerSignInTarget
import com.forkmesh.nativeapp.runners.RunnerStatus
import com.forkmesh.nativeapp.ui.kit.FmCard
import com.forkmesh.nativeapp.ui.kit.FmCircularProgressIndicator
import com.forkmesh.nativeapp.ui.kit.FmConfirmDialog
import com.forkmesh.nativeapp.ui.kit.FmEmptyState
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmPullToRefreshBox
import com.forkmesh.nativeapp.ui.kit.FmStatusBadge
import com.forkmesh.nativeapp.ui.kit.FmTone
import com.forkmesh.nativeapp.ui.kit.FmToneButton
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmRadius
import com.forkmesh.nativeapp.ui.theme.FmSpace
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

internal class RunnersScreen : Screen() {
    override val title: String get() = "Runners"
    private var rows by mutableStateOf<List<RunnerRow>>(emptyList())
    private var loaded by mutableStateOf(false)
    private var signedOut by mutableStateOf(false)
    private var loading by mutableStateOf(false)
    private var refreshing by mutableStateOf(false)
    private var error by mutableStateOf("")
    private var notice by mutableStateOf("")
    private var noticeError by mutableStateOf(false)
    private var busy by mutableStateOf<String?>(null)
    private var reloadAgain = false
    private var confirmRevoke by mutableStateOf<String?>(null)

    @Composable
    override fun Content(navigator: Navigator) {
        val scope = rememberCoroutineScope()
        val context = LocalContext.current

        fun reload(pulled: Boolean) {
            if (loading) {
                reloadAgain = true
                return
            }
            val api = RunnerAttentionFeature.api()
            if (api == null) {
                signedOut = true
                loaded = true
                return
            }
            loading = true
            refreshing = pulled
            scope.launch {
                try {
                    val result = withContext(Dispatchers.IO) { RunnerList.load(api) }
                    when (result) {
                        is RunnerListResult.SignedOut -> {
                            signedOut = true
                            rows = emptyList()
                            error = ""
                        }
                        is RunnerListResult.Loaded -> {
                            signedOut = false
                            rows = result.rows
                            error = ""
                        }
                        is RunnerListResult.Failed -> error = result.message
                    }
                    loaded = true
                } finally {
                    loading = false
                    refreshing = false
                }
                if (reloadAgain) {
                    reloadAgain = false
                    reload(false)
                }
            }
        }

        fun change(action: RunnerAction, name: String) {
            val api = RunnerAttentionFeature.api() ?: return
            if (busy != null) return
            busy = name
            scope.launch {
                try {
                    val outcome = withContext(Dispatchers.IO) { RunnerList.change(api, action, name) }
                    notice = outcome.message
                    noticeError = !outcome.ok
                    val status = outcome.status
                    if (outcome.ok && status != null) {
                        rows = RunnerList.sorted(rows.map { if (it.name == name) it.copy(status = status) else it })
                    }
                } finally {
                    busy = null
                }
                reload(false)
            }
        }

        fun addRunner() {
            val api = RunnerAttentionFeature.api() ?: return
            NavigationFeature.openInBrowser(context, api.appOrigin + RunnerAttention.DASHBOARD_PATH)
        }

        fun signIn(runner: String, provider: String) {
            RunnerAttentionFeature.open(navigator, RunnerSignInTarget(runner, provider)) { url ->
                NavigationFeature.openInBrowser(context, url)
            }
        }

        LaunchedEffect(Unit) { reload(false) }

        DetailScaffold(
            title,
            navigator,
            actions = {
                if (!signedOut) {
                    FmIconButton("add", "Add runner", onPressed = { addRunner() }, color = Fm.colors.textPrimary)
                }
                FmIconButton("refresh", "Refresh", onPressed = if (loading) null else ({ reload(false) }), color = Fm.colors.textPrimary)
            },
        ) {
            FmPullToRefreshBox(isRefreshing = refreshing, onRefresh = { reload(true) }, modifier = Modifier.fillMaxSize()) {
                LazyColumn(
                    Modifier.fillMaxSize().testTag("runners-list"),
                    contentPadding = PaddingValues(FmSpace.x4),
                    verticalArrangement = Arrangement.spacedBy(FmSpace.x2),
                ) {
                    if (error.isNotEmpty()) item(key = "error") { Line(error, Fm.colors.danger) }
                    if (notice.isNotEmpty()) item(key = "notice") { Line(notice, if (noticeError) Fm.colors.danger else Fm.colors.textSecondary) }
                    when {
                        !loaded -> item(key = "loading") {
                            Box(Modifier.fillMaxWidth().padding(FmSpace.x6), contentAlignment = Alignment.Center) { FmCircularProgressIndicator() }
                        }
                        signedOut -> item(key = "signed-out") {
                            FmEmptyState("dns", "Runners", message = "Sign in to your ForkMesh account to manage your runners.")
                        }
                        rows.isEmpty() && error.isNotEmpty() -> item(key = "failed") {
                            FmEmptyState("cloud_off", "Couldn't load runners", message = "Pull down or tap Refresh to try again.")
                        }
                        rows.isEmpty() -> item(key = "empty") { Empty { addRunner() } }
                        else -> {
                            item(key = "intro") {
                                Text(
                                    "Runners you registered from the desktop app or a phone. A paused runner finishes its jobs and takes no new ones.",
                                    style = Fm.text.bodySmall.copy(color = Fm.colors.textSecondary),
                                )
                            }
                            items(rows, key = { it.name }) { row ->
                                RunnerCard(
                                    row = row,
                                    busy = busy == row.name,
                                    enabled = busy == null,
                                    onSignIn = { provider -> signIn(row.name, provider) },
                                    onAction = { action ->
                                        when (action) {
                                            RunnerAction.Accounts -> RunnerAccountsFeature.open(navigator)
                                            RunnerAction.Revoke -> confirmRevoke = row.name
                                            RunnerAction.Pause, RunnerAction.Resume -> change(action, row.name)
                                            RunnerAction.SignIn -> row.signInProviders.firstOrNull()?.let { signIn(row.name, it) }
                                        }
                                    },
                                )
                            }
                        }
                    }
                }
            }
        }

        confirmRevoke?.let { name ->
            FmConfirmDialog(
                title = RunnerList.revokeTitle(name),
                body = RunnerList.revokeMessage(name),
                confirmLabel = RunnerAction.Revoke.label,
                destructive = true,
                busy = busy == name,
                onConfirm = {
                    confirmRevoke = null
                    change(RunnerAction.Revoke, name)
                },
                onDismissRequest = { confirmRevoke = null },
            )
        }
    }
}

@Composable
private fun Line(text: String, color: Color) {
    Text(text, style = Fm.text.bodySmall.copy(color = color), modifier = Modifier.padding(bottom = FmSpace.x1))
}

@Composable
private fun Empty(onAdd: () -> Unit) {
    Column(
        Modifier.fillMaxWidth().padding(vertical = FmSpace.x6),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.spacedBy(FmSpace.x3),
    ) {
        FmEmptyState(
            "dns",
            "No runners yet",
            message = "Add a runner from the web dashboard; it shows here once it registers.",
            modifier = Modifier.fillMaxWidth(),
        )
        FmToneButton("Add runner", onPressed = onAdd, icon = "add", tone = FmTone.Accent, filled = true)
    }
}

@OptIn(ExperimentalLayoutApi::class)
@Composable
private fun RunnerCard(
    row: RunnerRow,
    busy: Boolean,
    enabled: Boolean,
    onSignIn: (String) -> Unit,
    onAction: (RunnerAction) -> Unit,
) {
    val c = Fm.colors
    val now = System.currentTimeMillis()
    val online = row.online(now)
    FmCard(Modifier.fillMaxWidth().testTag("runner-${row.name}"), radius = FmRadius.lg) {
        Column(verticalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Box(
                    Modifier
                        .size(10.dp)
                        .background(if (online && !row.revoked) c.success else c.textDisabled, CircleShape)
                        .semantics { contentDescription = if (online && !row.revoked) "Online" else "Offline" },
                )
                Spacer(Modifier.width(FmSpace.x3))
                Column(Modifier.weight(1f)) {
                    Text(
                        row.name,
                        style = Fm.mono(color = if (row.revoked) c.textSecondary else c.textPrimary),
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis,
                    )
                    Text(
                        listOf(RunnerList.seenText(row.lastSeenAtMs, now), RunnerList.summary(row)).filter { it.isNotEmpty() }.joinToString(" · "),
                        style = Fm.text.bodySmall.copy(color = c.textSecondary),
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis,
                    )
                }
                val label = RunnerList.statusLabel(row.status)
                if (label.isNotEmpty()) {
                    Spacer(Modifier.width(FmSpace.x2))
                    FmStatusBadge(label, if (row.status == RunnerStatus.Revoked) c.danger else c.textSecondary)
                }
                if (busy) {
                    Spacer(Modifier.width(FmSpace.x2))
                    FmCircularProgressIndicator(Modifier.size(18.dp), strokeWidth = 2.dp)
                }
            }
            if (!row.revoked) {
                for (provider in row.providers) ProviderLine(provider, enabled && provider.binaryFound) { onSignIn(provider.provider) }
            }
            val actions = row.actions.filter { it != RunnerAction.SignIn }
            if (actions.isNotEmpty()) {
                FlowRow(horizontalArrangement = Arrangement.spacedBy(FmSpace.x2), verticalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                    for (action in actions) {
                        FmToneButton(
                            action.label,
                            onPressed = if (enabled || action == RunnerAction.Accounts) ({ onAction(action) }) else null,
                            modifier = Modifier.testTag("runner-${row.name}-${action.raw}"),
                            icon = when (action) {
                                RunnerAction.Accounts -> "manage_accounts"
                                RunnerAction.Pause -> "pause"
                                RunnerAction.Resume -> "play_arrow"
                                RunnerAction.Revoke -> "block"
                                RunnerAction.SignIn -> "login"
                            },
                            tone = if (action == RunnerAction.Revoke) FmTone.Danger else FmTone.Neutral,
                        )
                    }
                }
            }
        }
    }
}

@Composable
private fun ProviderLine(state: RunnerProviderState, canSignIn: Boolean, onSignIn: () -> Unit) {
    val c = Fm.colors
    val tint = when (state.auth) {
        RunnerAuth.Expired -> c.danger
        RunnerAuth.Expiring -> c.warning
        RunnerAuth.Ok, RunnerAuth.SignedOut, RunnerAuth.NotInstalled -> c.textSecondary
    }
    Row(verticalAlignment = Alignment.CenterVertically) {
        Column(Modifier.weight(1f)) {
            Text(
                RunnerAttention.providerLong(state.provider),
                style = Fm.text.labelMedium.copy(color = c.textPrimary, fontWeight = FontWeight.SemiBold),
            )
            Text(
                RunnerList.providerText(state),
                style = Fm.text.bodySmall.copy(color = tint),
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
        }
        if (state.binaryFound) {
            FmToneButton(
                RunnerAction.SignIn.label,
                onPressed = if (canSignIn) onSignIn else null,
                icon = "login",
                tone = FmTone.Accent,
            )
        }
    }
}
