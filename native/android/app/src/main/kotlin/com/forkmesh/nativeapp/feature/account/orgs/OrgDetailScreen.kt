package com.forkmesh.nativeapp.feature.account.orgs

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.ListItem
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.account.MEMBER_MENU_REMOVE
import com.forkmesh.nativeapp.account.OrgMemberInput
import com.forkmesh.nativeapp.account.OrgMenuAction
import com.forkmesh.nativeapp.account.OrgMenuItem
import com.forkmesh.nativeapp.account.OrgRepoInput
import com.forkmesh.nativeapp.account.OrgTeamInput
import com.forkmesh.nativeapp.account.TEAM_ALREADY_FULL
import com.forkmesh.nativeapp.account.TEAM_MENU_DELETE
import com.forkmesh.nativeapp.account.defaultRepoNode
import com.forkmesh.nativeapp.account.linkedRepoServes
import com.forkmesh.nativeapp.account.linkedRepoTitle
import com.forkmesh.nativeapp.account.memberMenuAction
import com.forkmesh.nativeapp.account.memberMenuRoles
import com.forkmesh.nativeapp.account.orgDeleteTitle
import com.forkmesh.nativeapp.account.orgErrorText
import com.forkmesh.nativeapp.account.orgMemberInput
import com.forkmesh.nativeapp.account.orgRepoInput
import com.forkmesh.nativeapp.account.orgStats
import com.forkmesh.nativeapp.account.orgTeamInput
import com.forkmesh.nativeapp.account.teamAddCandidates
import com.forkmesh.nativeapp.account.teamEmptyMessage
import com.forkmesh.nativeapp.account.teamMemberCountLabel
import com.forkmesh.nativeapp.account.teamMenuAction
import com.forkmesh.nativeapp.account.teamMenuPermissions
import com.forkmesh.nativeapp.account.teamSubtitle
import com.forkmesh.nativeapp.api.ApiService
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.feature.account.AccountScreen
import com.forkmesh.nativeapp.feature.account.AuthService
import com.forkmesh.nativeapp.feature.account.HGap
import com.forkmesh.nativeapp.feature.account.VGap
import com.forkmesh.nativeapp.feature.people.openProfile
import com.forkmesh.nativeapp.model.ORG_ROLES
import com.forkmesh.nativeapp.model.ORG_TEAM_PERMISSIONS
import com.forkmesh.nativeapp.model.OrgMember
import com.forkmesh.nativeapp.model.OrgProfile
import com.forkmesh.nativeapp.model.OrgRepo
import com.forkmesh.nativeapp.model.OrgTeam
import com.forkmesh.nativeapp.ui.kit.FmAlertDialog
import com.forkmesh.nativeapp.ui.kit.FmCard
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenu
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenuItem
import com.forkmesh.nativeapp.ui.kit.FmEmptyState
import com.forkmesh.nativeapp.ui.kit.FmField
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmInlineStatus
import com.forkmesh.nativeapp.ui.kit.FmListTileDefaults
import com.forkmesh.nativeapp.ui.kit.FmPullToRefreshBox
import com.forkmesh.nativeapp.ui.kit.FmSectionHeader
import com.forkmesh.nativeapp.ui.kit.FmSkeletonList
import com.forkmesh.nativeapp.ui.kit.FmStatus
import com.forkmesh.nativeapp.ui.kit.FmStatusBadge
import com.forkmesh.nativeapp.ui.kit.FmTextButton
import com.forkmesh.nativeapp.ui.kit.FmTooltip
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
import kotlinx.coroutines.async
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

private data class OrgDetail(
    val profile: OrgProfile,
    val members: List<OrgMember>,
    val teams: List<OrgTeam>,
    val repos: List<OrgRepo>,
)

class OrgDetailScreen(private val orgName: String) : AccountScreen() {
    private var loading by mutableStateOf(true)
    private var error by mutableStateOf("")
    private var profile by mutableStateOf<OrgProfile?>(null)
    private var members by mutableStateOf<List<OrgMember>>(emptyList())
    private var teams by mutableStateOf<List<OrgTeam>>(emptyList())
    private var repos by mutableStateOf<List<OrgRepo>>(emptyList())
    private var started = false
    private var generation = 0
    private val actionStatus = FmStatus()

    override val title: String get() = profile?.title ?: orgName

    private fun load(api: ApiService) {
        val current = ++generation
        loading = true
        error = ""
        scope.launch {
            try {
                val detail = coroutineScope {
                    val p = async(Dispatchers.IO) { api.orgProfile(orgName) }
                    val m = async(Dispatchers.IO) { api.orgMembers(orgName) }
                    val t = async(Dispatchers.IO) { api.orgTeams(orgName) }
                    val r = async(Dispatchers.IO) { api.orgRepos(orgName) }
                    OrgDetail(p.await(), m.await(), t.await(), r.await())
                }
                if (current == generation) {
                    profile = detail.profile
                    members = detail.members
                    teams = detail.teams
                    repos = detail.repos
                    loading = false
                }
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (failure: Exception) {
                if (current == generation) {
                    loading = false
                    error = orgErrorText(failure)
                }
            }
        }
    }

    private fun runAction(api: ApiService, action: (ApiService) -> Unit) {
        actionStatus.clear()
        scope.launch {
            try {
                withContext(Dispatchers.IO) { action(api) }
                load(api)
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (failure: Exception) {
                actionStatus.error(orgErrorText(failure))
            }
        }
    }

    @OptIn(ExperimentalMaterial3Api::class)
    @Composable
    override fun Content(navigator: Navigator) {
        val api = Services.require<ApiService>()
        val messenger = LocalFmMessenger.current
        val c = Fm.colors
        LaunchedEffect(Unit) {
            if (!started) {
                started = true
                load(api)
            }
        }
        val current = profile
        val canManage = current?.canManage == true
        val act: ((ApiService) -> Unit) -> Unit = { action -> runAction(api, action) }

        val addMember: () -> Unit = {
            scope.launch {
                val input = messenger.showModalBottomSheet<OrgMemberInput>(
                    isScrollControlled = true,
                    containerColor = c.bgRaised,
                ) { close -> MemberPromptSheet(close) } ?: return@launch
                act { it.setOrgMember(orgName, input.name, role = input.role) }
            }
        }
        val addTeam: () -> Unit = {
            scope.launch {
                val input = messenger.showModalBottomSheet<OrgTeamInput>(
                    isScrollControlled = true,
                    containerColor = c.bgRaised,
                ) { close -> TeamPromptSheet(close) } ?: return@launch
                act { it.setOrgTeam(orgName, input.name, permission = input.permission) }
            }
        }
        val linkRepo: () -> Unit = {
            val account = defaultRepoNode(Services.get<AuthService>()?.session?.value)
            scope.launch {
                val input = messenger.showModalBottomSheet<OrgRepoInput>(
                    isScrollControlled = true,
                    containerColor = c.bgRaised,
                ) { close -> RepoPromptSheet(account, close) } ?: return@launch
                act { it.linkOrgRepo(orgName, input.repo, node = input.node) }
            }
        }
        val openTeam: (OrgTeam) -> Unit = { team ->
            navigator.push(TeamDetailScreen(orgName, team, members, canManage) { load(api) })
        }
        val confirmDelete: () -> Unit = {
            scope.launch {
                val confirmed = messenger.showDialog<Boolean> { done ->
                    FmAlertDialog(
                        onDismissRequest = { done(null) },
                        title = orgDeleteTitle(orgName),
                        containerColor = Fm.colors.bgOverlay,
                        actions = {
                            FmTextButton(onClick = { done(false) }) { Text("Cancel") }
                            Button(
                                onClick = { done(true) },
                                colors = ButtonDefaults.buttonColors(containerColor = Fm.colors.danger),
                            ) { Text("Delete") }
                        },
                        content = {
                            Text(
                                "This dissolves the organization, its members, teams, and repo links. " +
                                    "The linked repos themselves are not deleted. This cannot be undone.",
                            )
                        },
                    )
                }
                if (confirmed != true) return@launch
                actionStatus.clear()
                try {
                    withContext(Dispatchers.IO) { api.deleteOrg(orgName) }
                    if (navigator.top === this@OrgDetailScreen) navigator.pop()
                } catch (cancelled: CancellationException) {
                    throw cancelled
                } catch (failure: Exception) {
                    actionStatus.error(orgErrorText(failure))
                }
            }
        }

        Column(
            Modifier
                .fillMaxSize()
                .background(c.bgBase)
                .windowInsetsPadding(fmSafeAreaInsets()),
        ) {
            OrgScreenHeader(
                title = current?.title ?: orgName,
                subtitle = "/$orgName",
                onBack = { navigator.pop() },
                trailing = if (current?.isOwner == true) {
                    {
                        FmIconButton(
                            icon = "delete_outline",
                            label = "Delete organization",
                            onPressed = confirmDelete,
                            modifier = Modifier.testTag("org-delete-button"),
                            color = c.danger,
                        )
                    }
                } else {
                    null
                },
            )
            FmInlineStatus(actionStatus, Modifier.padding(horizontal = FmSpace.x4))
            Box(
                Modifier
                    .weight(1f)
                    .fillMaxWidth(),
            ) {
                when {
                    loading -> FmSkeletonList(rows = 5, label = "Loading organization")
                    current == null -> LazyColumn(Modifier.fillMaxSize()) {
                        item { VGap(FmSpace.x7) }
                        item { FmEmptyState(icon = "cloud_off_outlined", title = "Could not load organization", message = error) }
                        item {
                            Box(Modifier.fillMaxWidth(), contentAlignment = Alignment.Center) {
                                FmTextButton(onClick = { load(api) }) {
                                    FmIcon("refresh", size = 18.dp)
                                    HGap(8.dp)
                                    Text("Retry")
                                }
                            }
                        }
                    }
                    else -> FmPullToRefreshBox(
                        isRefreshing = false,
                        onRefresh = { load(api) },
                        modifier = Modifier.fillMaxSize(),
                    ) {
                        LazyColumn(Modifier.fillMaxSize(), contentPadding = PaddingValues(FmSpace.x4)) {
                            item { Overview(current) }
                            item {
                                Column(Modifier.fillMaxWidth()) {
                                    VGap(FmSpace.x5)
                                    OrgSection(title = "Members", items = members, onAdd = if (canManage) addMember else null) { member ->
                                        MemberRow(
                                            member = member,
                                            canManage = canManage,
                                            onChangeRole = { role -> act { it.setOrgMember(orgName, member.name, role = role) } },
                                            onRemove = { act { it.removeOrgMember(orgName, member.name) } },
                                        )
                                    }
                                }
                            }
                            item {
                                Column(Modifier.fillMaxWidth()) {
                                    VGap(FmSpace.x5)
                                    OrgSection(
                                        title = "Teams",
                                        items = teams,
                                        onAdd = if (canManage) addTeam else null,
                                        emptyLabel = "No teams yet",
                                    ) { team ->
                                        TeamRow(
                                            team = team,
                                            canManage = canManage,
                                            onOpen = { openTeam(team) },
                                            onChangePermission = { permission ->
                                                act { it.setOrgTeam(orgName, team.team, permission = permission) }
                                            },
                                            onDelete = { act { it.deleteOrgTeam(orgName, team.team) } },
                                        )
                                    }
                                }
                            }
                            item {
                                Column(Modifier.fillMaxWidth()) {
                                    VGap(FmSpace.x5)
                                    OrgSection(
                                        title = "Linked repos",
                                        items = repos,
                                        onAdd = if (canManage) linkRepo else null,
                                        emptyLabel = "No repos linked",
                                    ) { repo ->
                                        RepoRow(
                                            repo = repo,
                                            orgName = orgName,
                                            canManage = canManage,
                                            onUnlink = { act { it.unlinkOrgRepo(orgName, repo.repo) } },
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
}

internal class TeamDetailScreen(
    private val orgName: String,
    private val team: OrgTeam,
    private val orgMembers: List<OrgMember>,
    private val canManage: Boolean,
    private val onClosed: () -> Unit,
) : AccountScreen() {
    private var loading by mutableStateOf(true)
    private var error by mutableStateOf("")
    private var members by mutableStateOf<List<OrgMember>>(emptyList())
    private var started = false
    private var generation = 0
    private val actionStatus = FmStatus()

    override val title: String get() = team.team

    private fun load(api: ApiService) {
        val current = ++generation
        loading = true
        error = ""
        scope.launch {
            try {
                val loaded = withContext(Dispatchers.IO) { api.orgTeamMembers(orgName, team.team) }
                if (current == generation) {
                    members = loaded
                    loading = false
                }
            } catch (cancelled: CancellationException) {
                throw cancelled
            } catch (failure: Exception) {
                if (current == generation) {
                    loading = false
                    error = orgErrorText(failure)
                }
            }
        }
    }

    @Composable
    override fun Content(navigator: Navigator) {
        val api = Services.require<ApiService>()
        val messenger = LocalFmMessenger.current
        val c = Fm.colors
        LaunchedEffect(Unit) {
            if (!started) {
                started = true
                load(api)
            }
        }
        val act: ((ApiService) -> Unit) -> Unit = { action ->
            actionStatus.clear()
            scope.launch {
                try {
                    withContext(Dispatchers.IO) { action(api) }
                    load(api)
                } catch (cancelled: CancellationException) {
                    throw cancelled
                } catch (failure: Exception) {
                    actionStatus.error(orgErrorText(failure))
                }
            }
        }
        val addToTeam: () -> Unit = {
            val candidates = teamAddCandidates(orgMembers, members)
            if (candidates.isEmpty()) {
                actionStatus.show(TEAM_ALREADY_FULL)
            } else {
                scope.launch {
                    val picked = messenger.showModalBottomSheet<String>(containerColor = c.bgRaised) { close ->
                        PickMemberSheet(candidates, close)
                    } ?: return@launch
                    act { it.addOrgTeamMember(orgName, team.team, picked) }
                }
            }
        }

        Column(
            Modifier
                .fillMaxSize()
                .background(c.bgBase)
                .windowInsetsPadding(fmSafeAreaInsets()),
        ) {
            OrgScreenHeader(
                title = team.team,
                subtitle = teamSubtitle(team),
                onBack = { navigator.pop() },
                trailing = if (canManage) {
                    { FmIconButton(icon = "add", label = "Add member to team", onPressed = addToTeam, color = c.textPrimary) }
                } else {
                    null
                },
            )
            FmInlineStatus(actionStatus, Modifier.padding(horizontal = FmSpace.x4))
            Box(
                Modifier
                    .weight(1f)
                    .fillMaxWidth(),
            ) {
                when {
                    loading -> FmSkeletonList(rows = 4, label = "Loading members")
                    members.isEmpty() -> FmEmptyState(
                        icon = "group_outlined",
                        title = "No members on this team",
                        message = teamEmptyMessage(error, team),
                    )
                    else -> LazyColumn(
                        Modifier.fillMaxSize(),
                        contentPadding = PaddingValues(FmSpace.x4),
                        verticalArrangement = Arrangement.spacedBy(FmSpace.x2),
                    ) {
                        items(members) { member ->
                            FmCard(radius = FmRadius.lg) {
                                Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
                                    Text(
                                        member.name,
                                        modifier = Modifier.weight(1f).openProfile(member.name),
                                        style = Fm.text.bodyMedium.copy(fontWeight = FmType.emphasis),
                                    )
                                    if (canManage) {
                                        FmIconButton(
                                            icon = "close",
                                            label = "Remove from team",
                                            onPressed = { act { it.removeOrgTeamMember(orgName, team.team, member.name) } },
                                            color = c.textTertiary,
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

    override fun onDisposed() {
        super.onDisposed()
        onClosed()
    }
}

@Composable
private fun Overview(profile: OrgProfile) {
    val c = Fm.colors
    FmCard(modifier = Modifier.fillMaxWidth(), radius = FmRadius.lg) {
        Column(Modifier.fillMaxWidth()) {
            Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
                Text(profile.title, modifier = Modifier.weight(1f), style = Fm.text.titleMedium.copy(color = c.textPrimary))
                if (profile.viewerRole.isNotEmpty()) OrgRoleBadge(profile.viewerRole)
            }
            if (profile.description.isNotEmpty()) {
                VGap(FmSpace.x2)
                Text(
                    profile.description,
                    style = Fm.text.bodyMedium.copy(color = c.textSecondary, lineHeight = FmType.small * 1.35f),
                )
            }
            VGap(FmSpace.x3)
            Row(Modifier.fillMaxWidth()) {
                orgStats(profile).forEach { (label, value) ->
                    Column(Modifier.weight(1f)) {
                        Text("$value", style = Fm.text.titleLarge.copy(color = c.textPrimary))
                        Text(label, style = Fm.text.bodySmall.copy(color = c.textTertiary))
                    }
                }
            }
        }
    }
}

@Composable
private fun <T> OrgSection(
    title: String,
    items: List<T>,
    onAdd: (() -> Unit)?,
    emptyLabel: String? = null,
    row: @Composable (T) -> Unit,
) {
    val c = Fm.colors
    Column(Modifier.fillMaxWidth()) {
        FmSectionHeader(
            title = title,
            count = items.size,
            trailing = if (onAdd == null) {
                null
            } else {
                {
                    FmTooltip("Add") {
                        Box(
                            Modifier
                                .clip(CircleShape)
                                .clickable(role = Role.Button, onClick = onAdd)
                                .semantics { contentDescription = "Add" },
                        ) {
                            FmIcon("add_circle_outline", size = 20.dp, tint = c.accent)
                        }
                    }
                }
            },
        )
        VGap(FmSpace.x2)
        if (items.isEmpty()) {
            FmCard(modifier = Modifier.fillMaxWidth(), radius = FmRadius.lg) {
                Text(emptyLabel ?: "Nothing here yet", style = Fm.text.bodyMedium.copy(color = c.textTertiary))
            }
        } else {
            items.forEachIndexed { index, item ->
                row(item)
                if (index != items.lastIndex) VGap(FmSpace.x2)
            }
        }
    }
}

@Composable
private fun RowMenu(primary: List<OrgMenuItem>, secondary: OrgMenuItem, onSelected: (String) -> Unit) {
    val c = Fm.colors
    var expanded by remember { mutableStateOf(false) }
    Box {
        FmIconButton(icon = "more_vert", label = "Show menu", onPressed = { expanded = true }, color = c.textTertiary)
        FmDropdownMenu(expanded = expanded, onDismissRequest = { expanded = false }) {
            primary.forEach { item ->
                FmDropdownMenuItem(
                    text = item.label,
                    onClick = {
                        expanded = false
                        onSelected(item.value)
                    },
                    enabled = item.enabled,
                )
            }
            HorizontalDivider(Modifier.padding(vertical = 7.dp), color = c.border)
            FmDropdownMenuItem(
                text = secondary.label,
                onClick = {
                    expanded = false
                    onSelected(secondary.value)
                },
            )
        }
    }
}

@Composable
private fun MemberRow(member: OrgMember, canManage: Boolean, onChangeRole: (String) -> Unit, onRemove: () -> Unit) {
    FmCard(radius = FmRadius.lg) {
        Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
            Text(
                member.name,
                modifier = Modifier.weight(1f).openProfile(member.name),
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
                style = Fm.text.bodyMedium.copy(fontWeight = FmType.emphasis),
            )
            HGap(FmSpace.x2)
            OrgRoleBadge(member.role)
            if (canManage) {
                RowMenu(primary = memberMenuRoles(member), secondary = MEMBER_MENU_REMOVE) { value ->
                    when (val action = memberMenuAction(value)) {
                        OrgMenuAction.RemoveMember -> onRemove()
                        is OrgMenuAction.SetRole -> onChangeRole(action.role)
                        else -> Unit
                    }
                }
            }
        }
    }
}

@Composable
private fun TeamRow(
    team: OrgTeam,
    canManage: Boolean,
    onOpen: () -> Unit,
    onChangePermission: (String) -> Unit,
    onDelete: () -> Unit,
) {
    val c = Fm.colors
    FmCard(radius = FmRadius.lg, onTap = onOpen) {
        Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
            Column(Modifier.weight(1f)) {
                Text(
                    team.team,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                    style = Fm.text.bodyMedium.copy(fontWeight = FmType.emphasis),
                )
                VGap(FmSpace.x1)
                Text(teamMemberCountLabel(team.members), style = Fm.text.bodySmall.copy(color = c.textTertiary))
            }
            HGap(FmSpace.x2)
            FmStatusBadge(label = team.permission, color = c.textSecondary)
            if (canManage) {
                RowMenu(primary = teamMenuPermissions(team), secondary = TEAM_MENU_DELETE) { value ->
                    when (val action = teamMenuAction(value)) {
                        OrgMenuAction.DeleteTeam -> onDelete()
                        is OrgMenuAction.SetPermission -> onChangePermission(action.permission)
                        else -> Unit
                    }
                }
            }
        }
    }
}

@Composable
private fun RepoRow(repo: OrgRepo, orgName: String, canManage: Boolean, onUnlink: () -> Unit) {
    val c = Fm.colors
    FmCard(radius = FmRadius.lg) {
        Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
            Column(Modifier.weight(1f)) {
                Text(
                    linkedRepoTitle(orgName, repo),
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                    style = Fm.text.bodyMedium.copy(fontWeight = FmType.emphasis),
                )
                linkedRepoServes(repo)?.let { serves ->
                    VGap(FmSpace.x1)
                    Text(
                        serves,
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis,
                        style = Fm.text.bodySmall.copy(color = c.textTertiary),
                    )
                }
            }
            if (canManage) {
                FmIconButton(icon = "link_off", label = "Unlink repo", onPressed = onUnlink, color = c.textTertiary)
            }
        }
    }
}

@Composable
private fun PickMemberSheet(candidates: List<OrgMember>, close: (String?) -> Unit) {
    val c = Fm.colors
    Column(Modifier.fillMaxWidth()) {
        Text(
            "Add member to team",
            modifier = Modifier.padding(FmSpace.x4),
            style = Fm.text.titleMedium.copy(color = c.textPrimary),
        )
        Column(
            Modifier
                .weight(1f, fill = false)
                .verticalScroll(rememberScrollState()),
        ) {
            candidates.forEach { member ->
                ListItem(
                    headlineContent = { Text(member.name, color = c.textPrimary) },
                    trailingContent = { OrgRoleBadge(member.role) },
                    colors = FmListTileDefaults.colors(),
                    modifier = Modifier.clickable { close(member.name) },
                )
            }
        }
    }
}

@Composable
private fun PromptScaffold(title: String, helper: String, onSubmit: () -> Unit, content: @Composable ColumnScope.() -> Unit) {
    val c = Fm.colors
    Column(
        Modifier
            .fillMaxWidth()
            .padding(FmSpace.x4),
    ) {
        Text(title, style = Fm.text.titleLarge.copy(color = c.textPrimary))
        VGap(FmSpace.x2)
        Text(helper, style = Fm.text.bodySmall.copy(color = c.textSecondary, lineHeight = FmType.meta * 1.35f))
        VGap(FmSpace.x4)
        content()
        VGap(FmSpace.x2)
        Button(
            onClick = onSubmit,
            modifier = Modifier
                .fillMaxWidth()
                .testTag("org-prompt-submit"),
        ) {
            Text("Save")
        }
    }
}

@Composable
private fun MemberPromptSheet(close: (OrgMemberInput?) -> Unit) {
    var name by remember { mutableStateOf("") }
    var role by remember { mutableStateOf("member") }
    PromptScaffold(
        title = "Add or update member",
        helper = "Enter an existing account name. New members are notified.",
        onSubmit = { orgMemberInput(name, role)?.let { close(it) } },
    ) {
        OrgTextField(
            value = name,
            onValueChange = { name = it },
            label = "Account name",
            modifier = Modifier.testTag("org-member-field"),
            autofocus = true,
        )
        RoleDropdown(value = role, options = ORG_ROLES, label = "Role", onChanged = { role = it })
    }
}

@Composable
private fun TeamPromptSheet(close: (OrgTeamInput?) -> Unit) {
    var name by remember { mutableStateOf("") }
    var permission by remember { mutableStateOf("read") }
    PromptScaffold(
        title = "New or updated team",
        helper = "A team grants one permission over the org's linked repos.",
        onSubmit = { orgTeamInput(name, permission)?.let { close(it) } },
    ) {
        OrgTextField(
            value = name,
            onValueChange = { name = it },
            label = "Team name (lowercase, dashes)",
            modifier = Modifier.testTag("org-team-field"),
            autofocus = true,
        )
        RoleDropdown(value = permission, options = ORG_TEAM_PERMISSIONS, label = "Permission", onChanged = { permission = it })
    }
}

@Composable
private fun RepoPromptSheet(defaultNode: String, close: (OrgRepoInput?) -> Unit) {
    var repo by remember { mutableStateOf("") }
    var node by remember { mutableStateOf(defaultNode) }
    PromptScaffold(
        title = "Link a repo",
        helper = "Link one of your own node's published repos to serve it at /<org>/<repo>.",
        onSubmit = { orgRepoInput(repo, node)?.let { close(it) } },
    ) {
        OrgTextField(
            value = repo,
            onValueChange = { repo = it },
            label = "Repo name",
            modifier = Modifier.testTag("org-repo-field"),
            autofocus = true,
        )
        OrgTextField(value = node, onValueChange = { node = it }, label = "Your node (defaults to your account)")
    }
}

@Composable
private fun RoleDropdown(value: String, options: List<String>, label: String, onChanged: (String) -> Unit) {
    val c = Fm.colors
    var expanded by remember { mutableStateOf(false) }
    FmField(label = label, modifier = Modifier.padding(bottom = FmSpace.x3)) {
        Box {
            Row(
                Modifier
                    .fillMaxWidth()
                    .clip(RoundedCornerShape(FmRadius.md))
                    .background(c.bgBase)
                    .clickable(role = Role.Button) { expanded = true }
                    .semantics { contentDescription = label }
                    .padding(horizontal = FmSpace.x4, vertical = FmSpace.x3),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Text(value, modifier = Modifier.weight(1f), style = Fm.text.titleMedium)
                FmIcon("arrow_drop_down", tint = c.textSecondary)
            }
            FmDropdownMenu(expanded = expanded, onDismissRequest = { expanded = false }) {
                options.forEach { option ->
                    DropdownMenuItem(
                        text = { Text(option, style = Fm.text.titleMedium) },
                        onClick = {
                            expanded = false
                            onChanged(option)
                        },
                    )
                }
            }
        }
    }
}
