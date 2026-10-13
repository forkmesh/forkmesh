package com.forkmesh.nativeapp.feature.account.orgs

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Button
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.account.NewOrgName
import com.forkmesh.nativeapp.account.canUseOrgs
import com.forkmesh.nativeapp.account.newOrgName
import com.forkmesh.nativeapp.account.orgErrorText
import com.forkmesh.nativeapp.account.orgsListStateKey
import com.forkmesh.nativeapp.api.ApiService
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.feature.account.AccountFilledButton
import com.forkmesh.nativeapp.feature.account.AccountScreen
import com.forkmesh.nativeapp.feature.account.AuthService
import com.forkmesh.nativeapp.feature.account.HGap
import com.forkmesh.nativeapp.feature.account.VGap
import com.forkmesh.nativeapp.model.OrgSummary
import com.forkmesh.nativeapp.ui.kit.FmCard
import com.forkmesh.nativeapp.ui.kit.FmCircularProgressIndicator
import com.forkmesh.nativeapp.ui.kit.FmEmptyState
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmPullToRefreshBox
import com.forkmesh.nativeapp.ui.kit.FmSkeletonList
import com.forkmesh.nativeapp.ui.kit.FmSwap
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

class OrgsScreen : AccountScreen() {
    private var orgs by mutableStateOf<List<OrgSummary>>(emptyList())
    private var loading by mutableStateOf(false)
    private var failure by mutableStateOf<Throwable?>(null)
    private var started = false
    private var generation = 0

    override val title: String get() = "Organizations"

    private fun reload(api: ApiService) {
        val current = ++generation
        loading = true
        failure = null
        scope.launch {
            try {
                val loaded = withContext(Dispatchers.IO) { api.myOrgs() }
                if (current == generation) orgs = loaded
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (error: Exception) {
                if (current == generation) failure = error
            } finally {
                if (current == generation) loading = false
            }
        }
    }

    @OptIn(ExperimentalMaterial3Api::class)
    @Composable
    override fun Content(navigator: Navigator) {
        val api = Services.require<ApiService>()
        val auth = Services.require<AuthService>()
        val messenger = LocalFmMessenger.current
        val c = Fm.colors
        val session by auth.session.collectAsState()
        val signedIn = canUseOrgs(session)
        LaunchedEffect(Unit) {
            if (!started) {
                started = true
                if (signedIn) reload(api)
            }
        }
        val openOrg: (String) -> Unit = { name -> navigator.push(OrgDetailScreen(name)) }
        val createOrg: () -> Unit = {
            scope.launch {
                val created = messenger.showModalBottomSheet<OrgSummary>(
                    isScrollControlled = true,
                    containerColor = c.bgRaised,
                ) { close -> CreateOrgSheet(api, close) } ?: return@launch
                reload(api)
                openOrg(created.name)
            }
        }

        Column(
            Modifier
                .fillMaxSize()
                .background(c.bgBase)
                .windowInsetsPadding(fmSafeAreaInsets()),
        ) {
            OrgScreenHeader(
                title = "Organizations",
                subtitle = "Shared /<org>/<repo> namespaces",
                onBack = { navigator.pop() },
                trailing = if (signedIn) {
                    {
                        FmIconButton(
                            icon = "add",
                            label = "New organization",
                            onPressed = createOrg,
                            modifier = Modifier.testTag("org-create-button"),
                            color = c.textPrimary,
                        )
                    }
                } else {
                    null
                },
            )
            Box(
                Modifier
                    .weight(1f)
                    .fillMaxWidth(),
            ) {
                if (!signedIn) {
                    FmEmptyState(
                        icon = "apartment_outlined",
                        title = "Sign in to manage organizations",
                        message = "Organizations are tied to your Worker account. Sign in, then create " +
                            "or manage orgs and the repos they serve.",
                    )
                } else {
                    FmPullToRefreshBox(
                        isRefreshing = false,
                        onRefresh = { reload(api) },
                        modifier = Modifier.fillMaxSize(),
                    ) {
                        FmSwap(
                            stateKey = orgsListStateKey(loading, failure != null, orgs),
                            modifier = Modifier.fillMaxSize(),
                        ) { key ->
                            when {
                                key == "loading" -> FmSkeletonList(rows = 5, label = "Loading organizations")
                                key == "error" -> OrgsMessage(
                                    icon = "cloud_off_outlined",
                                    title = "Could not load organizations",
                                    message = orgErrorText(failure),
                                ) {
                                    FmTextButton(onClick = { reload(api) }) {
                                        FmIcon("refresh", size = 18.dp)
                                        HGap(8.dp)
                                        Text("Retry")
                                    }
                                }
                                key == "empty" -> OrgsMessage(
                                    icon = "apartment_outlined",
                                    title = "No organizations yet",
                                    message = "Create one to serve your repos at /<org>/<repo> and share " +
                                        "management with teams.",
                                ) {
                                    AccountFilledButton(label = "New organization", icon = "add", onClick = createOrg)
                                }
                                else -> LazyColumn(
                                    Modifier.fillMaxSize(),
                                    contentPadding = PaddingValues(FmSpace.x4),
                                    verticalArrangement = Arrangement.spacedBy(FmSpace.x2),
                                ) {
                                    items(orgs) { org -> OrgRow(org) { openOrg(org.name) } }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}

@Composable
private fun OrgsMessage(icon: String, title: String, message: String, action: @Composable () -> Unit) {
    LazyColumn(Modifier.fillMaxSize()) {
        item { VGap(FmSpace.x7) }
        item { FmEmptyState(icon = icon, title = title, message = message) }
        item {
            Box(Modifier.fillMaxWidth(), contentAlignment = Alignment.Center) { action() }
        }
    }
}

@Composable
private fun OrgRow(org: OrgSummary, onTap: () -> Unit) {
    val c = Fm.colors
    FmCard(radius = FmRadius.lg, onTap = onTap) {
        Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
            Box(
                Modifier
                    .size(42.dp)
                    .background(c.accentSubtle, RoundedCornerShape(FmRadius.md)),
                contentAlignment = Alignment.Center,
            ) {
                FmIcon("apartment_outlined", tint = c.accent)
            }
            HGap(FmSpace.x3)
            Text(
                org.name,
                modifier = Modifier.weight(1f),
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
                style = Fm.text.bodyMedium.copy(fontWeight = FmType.emphasis),
            )
            HGap(FmSpace.x2)
            OrgRoleBadge(org.role)
            HGap(FmSpace.x2)
            FmIcon("chevron_right", tint = c.textDisabled)
        }
    }
}

@Composable
private fun CreateOrgSheet(api: ApiService, close: (OrgSummary?) -> Unit) {
    val c = Fm.colors
    val scope = rememberCoroutineScope()
    var name by remember { mutableStateOf("") }
    var displayName by remember { mutableStateOf("") }
    var description by remember { mutableStateOf("") }
    var busy by remember { mutableStateOf(false) }
    var error by remember { mutableStateOf("") }

    fun submit() {
        when (val checked = newOrgName(name)) {
            is NewOrgName.Invalid -> error = checked.message
            is NewOrgName.Valid -> {
                busy = true
                error = ""
                scope.launch {
                    try {
                        val created = withContext(Dispatchers.IO) {
                            api.createOrg(name = checked.name, displayName = displayName, description = description)
                        }
                        close(created)
                    } catch (cancelled: CancellationException) {
                        throw cancelled
                    } catch (failure: Exception) {
                        busy = false
                        error = orgErrorText(failure)
                    }
                }
            }
        }
    }

    Column(
        Modifier
            .fillMaxWidth()
            .padding(FmSpace.x4),
    ) {
        Text("New organization", style = Fm.text.titleLarge.copy(color = c.textPrimary))
        VGap(FmSpace.x2)
        Text(
            "The name shares the /<name>/<repo> namespace with accounts, so it must be free. " +
                "You become its first owner.",
            style = Fm.text.bodySmall.copy(color = c.textSecondary, lineHeight = FmType.meta * 1.35f),
        )
        VGap(FmSpace.x4)
        OrgTextField(
            value = name,
            onValueChange = { name = it },
            label = "Name (lowercase, dashes)",
            modifier = Modifier.testTag("org-name-field"),
            autofocus = true,
        )
        OrgTextField(value = displayName, onValueChange = { displayName = it }, label = "Display name (optional)")
        OrgTextField(value = description, onValueChange = { description = it }, label = "Description (optional)")
        if (error.isNotEmpty()) {
            VGap(FmSpace.x1)
            Text(error, style = Fm.text.bodyMedium.copy(color = c.danger))
        }
        VGap(FmSpace.x3)
        Button(
            onClick = { submit() },
            enabled = !busy,
            modifier = Modifier
                .fillMaxWidth()
                .testTag("org-create-submit"),
        ) {
            if (busy) {
                FmCircularProgressIndicator(Modifier.size(18.dp), strokeWidth = 2.dp)
            } else {
                Text("Create organization")
            }
        }
    }
}
