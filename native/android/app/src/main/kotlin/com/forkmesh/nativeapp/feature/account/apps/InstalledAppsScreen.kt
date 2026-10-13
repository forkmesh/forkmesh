package com.forkmesh.nativeapp.feature.account.apps

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.LazyListScope
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.account.hasRealSession
import com.forkmesh.nativeapp.api.ApiService
import com.forkmesh.nativeapp.apps.AppInfo
import com.forkmesh.nativeapp.apps.AppInstall
import com.forkmesh.nativeapp.apps.AppInstallRequest
import com.forkmesh.nativeapp.apps.AppOrgInstalls
import com.forkmesh.nativeapp.apps.AppScope
import com.forkmesh.nativeapp.apps.AppsApi
import com.forkmesh.nativeapp.apps.AppsText
import com.forkmesh.nativeapp.apps.InstalledApps
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.feature.account.AccountFilledButton
import com.forkmesh.nativeapp.feature.account.AccountOutlinedButton
import com.forkmesh.nativeapp.feature.account.AccountScreen
import com.forkmesh.nativeapp.feature.account.AuthService
import com.forkmesh.nativeapp.feature.account.HGap
import com.forkmesh.nativeapp.feature.account.VGap
import com.forkmesh.nativeapp.feature.account.orgs.OrgRoleBadge
import com.forkmesh.nativeapp.feature.account.orgs.OrgScreenHeader
import com.forkmesh.nativeapp.feature.navigation.NavigationFeature
import com.forkmesh.nativeapp.model.OrgApiException
import com.forkmesh.nativeapp.ui.kit.FmCard
import com.forkmesh.nativeapp.ui.kit.FmEmptyState
import com.forkmesh.nativeapp.ui.kit.FmInlineStatus
import com.forkmesh.nativeapp.ui.kit.FmPullToRefreshBox
import com.forkmesh.nativeapp.ui.kit.FmSectionHeader
import com.forkmesh.nativeapp.ui.kit.FmSkeletonList
import com.forkmesh.nativeapp.ui.kit.FmStatus
import com.forkmesh.nativeapp.ui.kit.FmStatusBadge
import com.forkmesh.nativeapp.ui.kit.FmTextButton
import com.forkmesh.nativeapp.ui.kit.LocalFmMessenger
import com.forkmesh.nativeapp.ui.kit.fmSafeAreaInsets
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmRadius
import com.forkmesh.nativeapp.ui.theme.FmSpace
import com.forkmesh.nativeapp.ui.theme.FmType
import kotlin.coroutines.cancellation.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

class InstalledAppsScreen : AccountScreen() {
    private var installed by mutableStateOf<InstalledApps?>(null)
    private var loading by mutableStateOf(false)
    private var failure by mutableStateOf<Throwable?>(null)
    private var busy by mutableStateOf(emptySet<String>())
    private val actionStatus = FmStatus()
    private var started = false
    private var generation = 0

    override val title: String get() = "Installed apps"

    private fun reload(apps: AppsApi) {
        val current = ++generation
        loading = true
        failure = null
        scope.launch {
            try {
                val loaded = withContext(Dispatchers.IO) { apps.installed() }
                if (current == generation) installed = loaded
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (error: Exception) {
                if (current == generation) failure = error
            } finally {
                if (current == generation) loading = false
            }
        }
    }

    private fun failureText(error: Throwable?): String = AppsText.manageError((error as? OrgApiException)?.code.orEmpty())

    @OptIn(ExperimentalMaterial3Api::class)
    @Composable
    override fun Content(navigator: Navigator) {
        val api = Services.require<ApiService>()
        val apps = remember(api) { AppsApi(api) }
        val auth = Services.require<AuthService>()
        val messenger = LocalFmMessenger.current
        val context = LocalContext.current
        val c = Fm.colors
        val session by auth.session.collectAsState()
        val guest by auth.isGuest.collectAsState()
        val signedIn = hasRealSession(session) && !guest
        LaunchedEffect(signedIn) {
            if (signedIn && !started) {
                started = true
                reload(apps)
            }
        }

        fun run(key: String, action: () -> Unit, done: String) {
            if (key in busy) return
            busy = busy + key
            actionStatus.clear()
            scope.launch {
                try {
                    withContext(Dispatchers.IO) { action() }
                    if (done.isNotEmpty()) actionStatus.show(done)
                    reload(apps)
                } catch (cancelled: CancellationException) {
                    throw cancelled
                } catch (error: Exception) {
                    actionStatus.error(failureText(error))
                } finally {
                    busy = busy - key
                }
            }
        }

        val revoke: (AppInstall) -> Unit = { install ->
            scope.launch {
                val confirmed = messenger.showConfirmDialog(
                    title = AppsText.revokeTitle(install.app.name),
                    body = AppsText.REVOKE_BODY,
                    confirmLabel = "Revoke",
                    destructive = true,
                )
                if (confirmed) run(install.installId, { apps.revoke(install.installId) }, "${install.app.name.ifEmpty { "The app" }} was revoked.")
            }
        }
        val decide: (AppOrgInstalls, AppInstallRequest, Boolean) -> Unit = { org, request, approve ->
            val verb = if (approve) "approved" else "denied"
            run(request.id, { apps.decide(org.name, request.id, approve) }, "${request.app.name.ifEmpty { "The app" }} was $verb for ${org.name}.")
        }
        val browse: () -> Unit = { NavigationFeature.openInBrowser(context, api.gitOrigin + AppsText.DIRECTORY_PATH) }

        Column(
            Modifier
                .fillMaxSize()
                .background(c.bgBase)
                .windowInsetsPadding(fmSafeAreaInsets()),
        ) {
            OrgScreenHeader(
                title = "Installed apps",
                subtitle = "Apps that act for you or your organizations",
                onBack = { navigator.pop() },
            )
            Box(
                Modifier
                    .weight(1f)
                    .fillMaxWidth(),
            ) {
                val data = installed
                when {
                    !signedIn -> FmEmptyState(
                        icon = "extension",
                        title = "Sign in to manage apps",
                        message = "Installed apps belong to your ForkMesh account. Sign in to see and revoke them.",
                    )
                    data == null && loading -> FmSkeletonList(rows = 4, label = "Loading apps")
                    data == null -> LazyColumn(Modifier.fillMaxSize()) {
                        item { VGap(FmSpace.x7) }
                        item { FmEmptyState(icon = "cloud_off_outlined", title = "Could not load your apps", message = failureText(failure)) }
                        item {
                            Box(Modifier.fillMaxWidth(), contentAlignment = Alignment.Center) {
                                FmTextButton(onClick = { reload(apps) }) {
                                    FmIcon("refresh", size = 18.dp)
                                    HGap(8.dp)
                                    Text("Retry")
                                }
                            }
                        }
                    }
                    else -> FmPullToRefreshBox(
                        isRefreshing = loading,
                        onRefresh = { reload(apps) },
                        modifier = Modifier.fillMaxSize(),
                    ) {
                        LazyColumn(
                            Modifier
                                .fillMaxSize()
                                .testTag("installed-apps-list"),
                            contentPadding = PaddingValues(FmSpace.x4),
                            verticalArrangement = Arrangement.spacedBy(FmSpace.x2),
                        ) {
                            if (actionStatus.message != null) {
                                item(key = "action-status") { FmInlineStatus(actionStatus) }
                            }
                            if (data.isEmpty) {
                                item {
                                    FmEmptyState(
                                        icon = "extension",
                                        title = AppsText.EMPTY_TITLE,
                                        message = "Apps you install from the App Directory show up here, with what they can do.",
                                    )
                                }
                            } else {
                                section("Just me")
                                if (data.installs.isEmpty()) {
                                    item { Note("No apps are installed just for you.") }
                                } else {
                                    items(data.installs, key = { "me-" + it.installId }) { install ->
                                        InstallCard(install, busy = install.installId in busy, onRevoke = { revoke(install) })
                                    }
                                }
                                for (org in data.organizations) {
                                    item(key = "org-" + org.name) {
                                        FmSectionHeader(
                                            title = org.name,
                                            modifier = Modifier.padding(top = FmSpace.x3, bottom = FmSpace.x1),
                                            icon = "apartment_outlined",
                                            trailing = { OrgRoleBadge(org.role) },
                                        )
                                    }
                                    if (org.installs.isEmpty()) {
                                        item { Note("No apps are installed for ${org.name}.") }
                                    } else {
                                        items(org.installs, key = { org.name + "-" + it.installId }) { install ->
                                            InstallCard(install, busy = install.installId in busy, onRevoke = { revoke(install) })
                                        }
                                    }
                                    if (org.canManage && org.requests.isNotEmpty()) {
                                        section("Requests", key = "requests-" + org.name)
                                        items(org.requests, key = { org.name + "-request-" + it.id }) { request ->
                                            RequestCard(
                                                request,
                                                busy = request.id in busy,
                                                onApprove = { decide(org, request, true) },
                                                onDeny = { decide(org, request, false) },
                                            )
                                        }
                                    }
                                }
                            }
                            item {
                                Box(Modifier.fillMaxWidth().padding(top = FmSpace.x4), contentAlignment = Alignment.Center) {
                                    AccountOutlinedButton(
                                        label = "Browse the App Directory",
                                        onClick = browse,
                                        icon = "open_in_new",
                                        modifier = Modifier.testTag("installed-apps-directory"),
                                    )
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}

private fun LazyListScope.section(title: String, key: String = "section-$title") {
    item(key = key) {
        FmSectionHeader(title = title, modifier = Modifier.padding(top = FmSpace.x3, bottom = FmSpace.x1))
    }
}

@Composable
private fun Note(text: String) {
    Text(text, modifier = Modifier.padding(vertical = FmSpace.x1), style = Fm.text.bodySmall.copy(color = Fm.colors.textTertiary))
}

@Composable
private fun AppHeading(app: AppInfo, trailing: (@Composable () -> Unit)? = null) {
    val c = Fm.colors
    Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
        Box(
            Modifier
                .size(40.dp)
                .background(c.accentSubtle, RoundedCornerShape(FmRadius.md)),
            contentAlignment = Alignment.Center,
        ) {
            FmIcon("extension", tint = c.accent)
        }
        HGap(FmSpace.x3)
        Column(Modifier.weight(1f)) {
            Text(
                app.name.ifEmpty { app.slug.ifEmpty { "App" } },
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
                style = Fm.text.bodyMedium.copy(fontWeight = FmType.emphasis, color = c.textPrimary),
            )
            if (app.developer.isNotEmpty() || app.organization.isNotEmpty()) {
                Text(
                    "by " + app.organization.ifEmpty { app.developer },
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                    style = Fm.text.bodySmall.copy(color = c.textTertiary),
                )
            }
        }
        if (trailing != null) {
            HGap(FmSpace.x2)
            trailing()
        }
    }
    if (app.description.isNotEmpty()) {
        VGap(FmSpace.x2)
        Text(app.description, style = Fm.text.bodySmall.copy(color = c.textSecondary))
    }
}

@OptIn(ExperimentalLayoutApi::class)
@Composable
private fun ScopeChips(scopes: List<AppScope>) {
    if (scopes.isEmpty()) return
    VGap(FmSpace.x2)
    FlowRow(horizontalArrangement = Arrangement.spacedBy(FmSpace.x1), verticalArrangement = Arrangement.spacedBy(FmSpace.x1)) {
        for (scope in scopes) FmStatusBadge(label = scope.label, color = Fm.colors.textSecondary)
    }
}

@Composable
private fun InstallCard(install: AppInstall, busy: Boolean, onRevoke: () -> Unit) {
    val c = Fm.colors
    FmCard(modifier = Modifier.fillMaxWidth(), radius = FmRadius.lg) {
        Column(Modifier.fillMaxWidth()) {
            AppHeading(install.app)
            ScopeChips(install.scopes)
            VGap(FmSpace.x2)
            val details = listOf(install.targetLabel, AppsText.installedBy(install.installedBy), install.lastUsedText()).filter { it.isNotEmpty() }
            Text(details.joinToString(" · "), style = Fm.text.bodySmall.copy(color = c.textTertiary))
            if (install.canRevoke) {
                VGap(FmSpace.x2)
                Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.End) {
                    AccountOutlinedButton(
                        label = "Revoke",
                        onClick = if (busy) null else onRevoke,
                        icon = "block",
                        contentColor = c.danger,
                        borderColor = c.danger.copy(alpha = 0.4f),
                    )
                }
            }
        }
    }
}

@Composable
private fun RequestCard(request: AppInstallRequest, busy: Boolean, onApprove: () -> Unit, onDeny: () -> Unit) {
    val c = Fm.colors
    FmCard(modifier = Modifier.fillMaxWidth(), radius = FmRadius.lg) {
        Column(Modifier.fillMaxWidth()) {
            AppHeading(request.app)
            ScopeChips(request.scopes)
            if (request.requester.isNotEmpty()) {
                VGap(FmSpace.x2)
                Text("Requested by ${request.requester}", style = Fm.text.bodySmall.copy(color = c.textTertiary))
            }
            VGap(FmSpace.x2)
            Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(FmSpace.x2, Alignment.End)) {
                AccountOutlinedButton(label = "Deny", onClick = if (busy) null else onDeny)
                AccountFilledButton(
                    label = "Approve",
                    icon = "check",
                    onClick = onApprove,
                    enabled = !busy,
                    colors = ButtonDefaults.buttonColors(containerColor = c.accent),
                )
            }
        }
    }
}
