package com.forkmesh.nativeapp.feature.account.projects

import android.graphics.Bitmap
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.PickVisualMediaRequest
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.platform.LocalContext
import androidx.compose.foundation.Image
import androidx.compose.ui.draw.clip
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.sp
import com.forkmesh.nativeapp.R
import com.forkmesh.nativeapp.ui.kit.fmTapSounds
import com.forkmesh.nativeapp.ui.theme.FmColors
import androidx.compose.foundation.clickable
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.activity.compose.BackHandler
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.core.tween
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.slideInHorizontally
import androidx.compose.animation.slideOutHorizontally
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.material3.Button
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.MaterialTheme
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
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.feature.account.AuthService
import com.forkmesh.nativeapp.ui.navigation.AppNavigation
import com.forkmesh.nativeapp.account.projects.LegalLinks
import com.forkmesh.nativeapp.account.projects.ProbeResult
import com.forkmesh.nativeapp.account.projects.Project
import com.forkmesh.nativeapp.account.projects.ProjectCatalogEntry
import com.forkmesh.nativeapp.account.projects.ProjectDefaults
import com.forkmesh.nativeapp.account.projects.SwitchPhase
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.feature.account.AccountFilledButton
import com.forkmesh.nativeapp.feature.account.AccountLabeledField
import com.forkmesh.nativeapp.feature.account.AccountOutlinedButton
import com.forkmesh.nativeapp.feature.account.AccountScreen
import com.forkmesh.nativeapp.feature.code.CodeFeature
import com.forkmesh.nativeapp.platform.AppLifecycle
import com.forkmesh.nativeapp.ui.kit.FmAlertDialog
import com.forkmesh.nativeapp.ui.kit.FmCard
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmInlineStatus
import com.forkmesh.nativeapp.ui.kit.FmStatus
import com.forkmesh.nativeapp.ui.kit.FmTextButton
import com.forkmesh.nativeapp.ui.kit.LocalFmMessenger
import com.forkmesh.nativeapp.ui.kit.fmSafeAreaInsets
import com.forkmesh.nativeapp.ui.kit.rememberFmStatus
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmRadius
import com.forkmesh.nativeapp.ui.theme.FmSpace
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

object ProjectActions {
    val status = FmStatus()

    fun switchTo(projectId: String, reopenDrawerOnError: Boolean = false) {
        ProjectDrawer.close()
        status.clear()
        val projects = Services.get<ProjectsService>() ?: return
        AppLifecycle.scope.launch {
            val result = projects.switchTo(projectId)
            if (!result.ok) {
                if (result.error.isNotEmpty()) {
                    status.error(result.error)
                    if (reopenDrawerOnError) ProjectDrawer.open()
                }
                return@launch
            }
            val project = result.project ?: return@launch
            if (project.hasInstance && projects.awaitConnection(project.id) == false) status.show(projects.switcher.offlineNote(project))
        }
    }

    fun homepage(): String = Services.get<ProjectsService>()?.active?.value?.homepageUri ?: ProjectDefaults.PROD_HOMEPAGE_URL

    fun termsUrl(): String = LegalLinks.termsFor(homepage())

    fun privacyUrl(): String = LegalLinks.privacyFor(homepage())

    fun membershipUrl(): String = LegalLinks.membershipFor(homepage())

    fun desktopUrl(): String = LegalLinks.desktopFor(homepage())
}

@Composable
fun ProjectIcon(project: Project, size: Dp = 32.dp) {
    val shape = RoundedCornerShape(size * 0.25f)
    val projects = Services.get<ProjectsService>()
    val cacheDir = LocalContext.current.cacheDir
    LaunchedEffect(project.id, project.logo, project.serverUrl) {
        if (projects != null) ProjectImages.load(project, projects.logos, cacheDir)
    }
    val image = ProjectImages.image(project)
    if (image != null) {
        Image(
            bitmap = image,
            contentDescription = null,
            contentScale = ContentScale.Fit,
            modifier = Modifier.size(size).clip(shape).background(Fm.colors.bgRaised),
        )
    } else if (ProjectDefaults.isFixed(project.id)) {
        Image(
            painter = painterResource(R.drawable.fm_logo),
            contentDescription = null,
            contentScale = ContentScale.Fit,
            modifier = Modifier.size(size).clip(shape).background(Color.Black),
        )
    } else {
        Box(
            Modifier.size(size).clip(shape).background(FmColors.senderColor(project.label)),
            contentAlignment = Alignment.Center,
        ) {
            Text(
                project.label.firstOrNull()?.uppercase() ?: "?",
                style = Fm.text.titleMedium.copy(fontSize = (size.value * 0.5f).sp),
                color = Color.White,
            )
        }
    }
}

@Composable
fun ProjectSwitcherTitle(
    modifier: Modifier = Modifier,
    fallback: String = "ForkMesh",
    icon: Boolean = false,
    subtitle: String? = null,
    onOpen: () -> Unit = {},
    iconOnly: Boolean = false,
    onLongPress: (() -> Unit)? = null,
) {
    val projects = Services.get<ProjectsService>()
    if (projects == null && iconOnly) {
        val held = if (onLongPress == null) modifier else modifier.combinedClickable(onClick = {}, onLongClick = onLongPress, onLongClickLabel = "Go Home")
        Image(painterResource(R.drawable.fm_logo), contentDescription = "ForkMesh", modifier = held.size(40.dp))
        return
    }
    if (projects == null) {
        Text(fallback, style = MaterialTheme.typography.titleLarge, color = Fm.colors.textPrimary, modifier = modifier)
        return
    }
    val active by projects.active.collectAsState()
    Row(
        modifier
            .testTag("home-project-switcher")
            .combinedClickable(
                onClickLabel = "Switch project or add a new one",
                role = Role.Button,
                onLongClick = onLongPress,
                onLongClickLabel = onLongPress?.let { "Go Home" },
            ) {
                onOpen()
                ProjectDrawer.open()
            },
        verticalAlignment = Alignment.CenterVertically,
    ) {
        if (iconOnly) {
            ProjectIcon(active, size = 40.dp)
            return@Row
        }
        if (icon) {
            ProjectIcon(active, size = 36.dp)
            Spacer(Modifier.width(FmSpace.x3))
        }
        Column {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(
                    active.label,
                    style = MaterialTheme.typography.titleLarge,
                    color = Fm.colors.textPrimary,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                    modifier = Modifier.weight(1f, fill = false),
                )
                FmIcon("expand_more", size = 20.dp, tint = Fm.colors.textSecondary)
            }
            if (subtitle != null) {
                Text(subtitle, style = MaterialTheme.typography.bodySmall, color = Fm.colors.textTertiary)
            }
        }
    }
}

object ProjectDrawer {
    const val WIDTH_FRACTION = 0.75f
    const val ANIMATION_MS = 250
    private val openState = mutableStateOf(false)

    val isOpen: Boolean get() = openState.value

    fun open() {
        openState.value = true
    }

    fun close() {
        openState.value = false
    }
}

@Composable
fun ProjectDrawerHost() {
    val projects = Services.get<ProjectsService>() ?: return
    val open = ProjectDrawer.isOpen
    Box(Modifier.fillMaxSize()) {
        AnimatedVisibility(
            visible = open,
            enter = fadeIn(tween(ProjectDrawer.ANIMATION_MS)),
            exit = fadeOut(tween(ProjectDrawer.ANIMATION_MS)),
        ) {
            Box(
                Modifier
                    .fillMaxSize()
                    .background(Color(0x80000000))
                    .testTag("project-drawer-scrim")
                    .clickable(
                        interactionSource = remember { MutableInteractionSource() },
                        indication = null,
                        onClickLabel = "Close projects",
                        onClick = { ProjectDrawer.close() },
                    ),
            )
        }
        AnimatedVisibility(
            visible = open,
            enter = slideInHorizontally(tween(ProjectDrawer.ANIMATION_MS)) { -it },
            exit = slideOutHorizontally(tween(ProjectDrawer.ANIMATION_MS)) { -it },
        ) {
            Box(
                Modifier
                    .fillMaxHeight()
                    .fillMaxWidth(ProjectDrawer.WIDTH_FRACTION)
                    .background(Fm.colors.bgRaised)
                    .testTag("project-drawer"),
            ) {
                BackHandler(enabled = open) { ProjectDrawer.close() }
                ProjectDrawerContent(projects)
            }
        }
    }
}

@Composable
private fun ProjectDrawerContent(projects: ProjectsService) {
    val c = Fm.colors
    val list by projects.projects.collectAsState()
    val active by projects.active.collectAsState()
    val messenger = LocalFmMessenger.current
    val scope = rememberCoroutineScope()
    val auth = Services.get<AuthService>()
    val canManage = auth?.hasAppAccess?.collectAsState()?.value ?: true
    val accounts = remember(list, ProjectDrawer.isOpen) {
        list.associate { project ->
            val account = projects.sessionFor(project.id)?.let { it.nodeName.ifEmpty { it.email } }.orEmpty()
            project.id to when {
                account.isNotEmpty() -> "Signed in as $account"
                projects.isSignedInOn(project.id) -> "Guest"
                else -> "Not signed in"
            }
        }
    }
    Column(Modifier.fillMaxSize().windowInsetsPadding(fmSafeAreaInsets())) {
        Text(
            "Projects",
            style = MaterialTheme.typography.titleLarge,
            color = c.textPrimary,
            modifier = Modifier.padding(start = FmSpace.x4, end = FmSpace.x4, top = FmSpace.x4, bottom = FmSpace.x2),
        )
        FmInlineStatus(ProjectActions.status, Modifier.padding(start = FmSpace.x4, end = FmSpace.x2, bottom = FmSpace.x2))
        LazyColumn(Modifier.weight(1f).fillMaxWidth()) {
            items(list, key = { it.id }) { project ->
                val isActive = project.id == active.id
                Row(
                    Modifier
                        .fillMaxWidth()
                        .background(if (isActive) c.bgOverlay else Color.Transparent)
                        .clickable(onClickLabel = if (isActive) "Close projects" else "Switch to ${project.label}", role = Role.Button) {
                            if (isActive) ProjectDrawer.close() else ProjectActions.switchTo(project.id, reopenDrawerOnError = true)
                        }
                        .padding(horizontal = FmSpace.x4, vertical = FmSpace.x3),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    ProjectIcon(project, size = 36.dp)
                    Spacer(Modifier.width(FmSpace.x3))
                    Column(Modifier.weight(1f)) {
                        Text(project.label, style = Fm.text.bodyMedium, color = c.textPrimary, maxLines = 1, overflow = TextOverflow.Ellipsis)
                        Text(project.subtitle, style = Fm.text.bodySmall.copy(color = c.textTertiary), maxLines = 1, overflow = TextOverflow.Ellipsis)
                        Text(accounts[project.id].orEmpty(), style = Fm.text.bodySmall.copy(color = c.textSecondary), maxLines = 1, overflow = TextOverflow.Ellipsis)
                    }
                    if (isActive) FmIcon("check", size = 20.dp, tint = c.accent, contentDescription = "Active project")
                }
            }
        }
        HorizontalDivider(thickness = 1.dp, color = c.border)
        ProjectDrawerAction("add", "Add project") {
            ProjectDrawer.close()
            scope.launch {
                val added = messenger.showDialog<Project> { done -> AddProjectDialog(projects, done) } ?: return@launch
                ProjectActions.switchTo(added.id, reopenDrawerOnError = true)
            }
        }
        ProjectDrawerAction("group_outlined", "Invite people to ${active.label}") {
            ProjectDrawer.close()
            val project = active
            scope.launch {
                messenger.showDialog<Unit> { done -> InviteToProjectDialog(projects, project, done) }
            }
        }
        if (canManage) {
            ProjectDrawerAction("settings", "Manage projects") {
                ProjectDrawer.close()
                AppNavigation.navigator.push(ProjectsScreen())
            }
        }
        Spacer(Modifier.height(FmSpace.x2))
    }
}

@Composable
private fun ProjectDrawerAction(icon: String, label: String, onClick: () -> Unit) {
    Row(
        Modifier
            .fillMaxWidth()
            .clickable(role = Role.Button, onClick = onClick)
            .padding(horizontal = FmSpace.x4, vertical = FmSpace.x3),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        FmIcon(icon, size = 20.dp, tint = Fm.colors.textSecondary)
        Spacer(Modifier.width(FmSpace.x3))
        Text(label, style = Fm.text.bodyMedium, color = Fm.colors.textPrimary)
    }
}

@Composable
fun ProjectSwitchOverlay() {
    val projects = Services.get<ProjectsService>() ?: return
    val phase by projects.phase.collectAsState()
    val status by projects.statusLine.collectAsState()
    val busy = phase == SwitchPhase.leaving || phase == SwitchPhase.clearing || phase == SwitchPhase.adopting || phase == SwitchPhase.joining
    if (!busy) return
    Box(
        Modifier
            .fillMaxSize()
            .background(Color(0x99000000))
            .clickable(enabled = true, onClick = {}),
        contentAlignment = Alignment.Center,
    ) {
        FmCard(radius = FmRadius.lg) {
            Column(Modifier.padding(FmSpace.x5), horizontalAlignment = Alignment.CenterHorizontally) {
                com.forkmesh.nativeapp.ui.kit.FmCircularProgressIndicator()
                Spacer(Modifier.height(FmSpace.x3))
                Text(status, style = Fm.text.bodyMedium)
            }
        }
    }
}

@Composable
fun ProjectsCard(navigator: Navigator) {
    val projects = Services.get<ProjectsService>() ?: return
    val list by projects.projects.collectAsState()
    val active by projects.active.collectAsState()
    val messenger = LocalFmMessenger.current
    val context = androidx.compose.ui.platform.LocalContext.current
    val scope = rememberCoroutineScope()
    val cardStatus = rememberFmStatus()
    val c = Fm.colors
    var logoTarget by remember { mutableStateOf("") }
    val pickLogo = rememberLauncherForActivityResult(ActivityResultContracts.PickVisualMedia()) { uri ->
        val target = logoTarget
        logoTarget = ""
        if (uri == null || target.isEmpty()) return@rememberLauncherForActivityResult
        cardStatus.clear()
        scope.launch {
            val saved = withContext(Dispatchers.IO) {
                val shrunk = ProjectLogoStore.read(context, uri)?.let { ProjectLogoStore.shrink(it) } ?: return@withContext null
                projects.logos.save(ProjectLogoStore.png(shrunk), target)?.let { it to shrunk }
            }
            if (saved == null) {
                cardStatus.error("That image could not be used as a logo.")
                return@launch
            }
            ProjectImages.remember(saved.first, saved.second)
            projects.setLogo(target, saved.first)
        }
    }
    Column(Modifier.fillMaxWidth()) {
        list.forEachIndexed { index, project ->
            if (index > 0) HorizontalDivider(thickness = 1.dp, color = c.border)
            ProjectRow(
                project = project,
                active = project.id == active.id,
                account = projects.sessionFor(project.id)?.let { it.nodeName.ifEmpty { it.email } },
                onSwitch = {
                    cardStatus.clear()
                    ProjectActions.switchTo(project.id)
                },
                onTest = {
                    cardStatus.clear()
                    scope.launch {
                        val result = projects.testConnection(project)
                        cardStatus.report(result.message, !result.ok)
                    }
                },
                onChooseLogo = {
                    logoTarget = project.id
                    pickLogo.launch(PickVisualMediaRequest(ActivityResultContracts.PickVisualMedia.ImageOnly))
                },
                onRemoveLogo = if (project.logo.isEmpty()) null else { { projects.setLogo(project.id, "") } },
                onEdit = if (project.builtIn) null else {
                    {
                        scope.launch {
                            val label = messenger.showPromptDialog(
                                title = "Rename project",
                                hint = project.subtitle,
                                initialValue = project.label,
                                confirmLabel = "Save",
                            ) ?: return@launch
                            projects.update(project.id, label = label)
                        }
                    }
                },
                onRemove = if (project.builtIn) null else {
                    {
                        scope.launch {
                            val confirmed = messenger.showConfirmDialog(
                                title = "Remove ${project.label}?",
                                body = if (project.hasInstance) {
                                    "This signs you out of ${project.id} on this phone and erases what this phone stored for it. Your account on that instance is not touched."
                                } else {
                                    "This erases what this phone stored for ${project.label}, including its sign-in and keys."
                                },
                                confirmLabel = "Remove",
                                destructive = true,
                            )
                            if (!confirmed) return@launch
                            if (!UserPresence.confirm(context, messenger, "Confirm it's you to remove ${project.label}.")) return@launch
                            val result = projects.remove(project.id)
                            if (!result.ok && result.error.isNotEmpty()) cardStatus.error(result.error)
                        }
                    }
                },
            )
        }
        HorizontalDivider(thickness = 1.dp, color = c.border)
        FlowRow(
            Modifier.padding(FmSpace.x4),
            horizontalArrangement = Arrangement.spacedBy(FmSpace.x3),
            verticalArrangement = Arrangement.spacedBy(FmSpace.x2),
        ) {
            AccountFilledButton(
                label = "Add project",
                icon = "add",
                onClick = {
                    scope.launch {
                        val added = messenger.showDialog<Project> { done -> AddProjectDialog(projects, done) } ?: return@launch
                        cardStatus.show("${added.label} added. Switch to it to sign in there.")
                    }
                },
            )
            AccountOutlinedButton(label = "All networks", icon = "hub", onClick = { navigator.push(NetworksScreen()) })
        }
        FmInlineStatus(ProjectActions.status, Modifier.padding(start = FmSpace.x4, end = FmSpace.x2, bottom = FmSpace.x2))
        FmInlineStatus(cardStatus, Modifier.padding(start = FmSpace.x4, end = FmSpace.x2, bottom = FmSpace.x2))
    }
}

@Composable
private fun ProjectRow(
    project: Project,
    active: Boolean,
    account: String?,
    onSwitch: () -> Unit,
    onTest: () -> Unit,
    onChooseLogo: () -> Unit,
    onRemoveLogo: (() -> Unit)?,
    onEdit: (() -> Unit)?,
    onRemove: (() -> Unit)?,
) {
    val c = Fm.colors
    var menu by remember { mutableStateOf(false) }
    Row(
        Modifier
            .fillMaxWidth()
            .clickable(enabled = !active, onClickLabel = "Switch to ${project.label}", role = Role.Button, onClick = onSwitch)
            .padding(horizontal = FmSpace.x4, vertical = FmSpace.x3),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        ProjectIcon(project, size = 32.dp)
        Spacer(Modifier.width(FmSpace.x3))
        Column(Modifier.weight(1f)) {
            Text(if (active) "${project.label} · active" else project.label, style = Fm.text.bodyMedium, maxLines = 1, overflow = TextOverflow.Ellipsis)
            Text(project.subtitle, style = Fm.text.bodySmall.copy(color = c.textTertiary), maxLines = 1, overflow = TextOverflow.Ellipsis)
            Text(
                if (account != null) "Signed in as $account" else "Not signed in",
                style = Fm.text.bodySmall.copy(color = if (account != null) c.textSecondary else c.textTertiary),
            )
        }
        Box {
            FmIconButton(icon = "more_vert", label = "Project actions", onPressed = { menu = true })
            DropdownMenu(expanded = menu, onDismissRequest = { menu = false }, modifier = Modifier.fmTapSounds()) {
                if (project.hasInstance) DropdownMenuItem(text = { Text("Test connection") }, onClick = { menu = false; onTest() })
                if (onEdit != null) DropdownMenuItem(text = { Text("Rename") }, onClick = { menu = false; onEdit() })
                DropdownMenuItem(
                    text = { Text(if (project.logo.isEmpty()) "Choose logo…" else "Change logo…") },
                    onClick = { menu = false; onChooseLogo() },
                )
                if (onRemoveLogo != null) DropdownMenuItem(text = { Text("Remove logo") }, onClick = { menu = false; onRemoveLogo() })
                if (onRemove != null) {
                    DropdownMenuItem(text = { Text("Remove", color = c.danger) }, onClick = { menu = false; onRemove() })
                }
            }
        }
    }
}

@Composable
private fun AddProjectDialog(projects: ProjectsService, done: (Project?) -> Unit) {
    val c = Fm.colors
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    var name by remember { mutableStateOf("") }
    var host by remember { mutableStateOf("") }
    var world by remember { mutableStateOf("") }
    var checking by remember { mutableStateOf(false) }
    var result by remember { mutableStateOf<ProbeResult?>(null) }
    var logo by remember { mutableStateOf<Bitmap?>(null) }
    var logoError by remember { mutableStateOf("") }
    val pickLogo = rememberLauncherForActivityResult(ActivityResultContracts.PickVisualMedia()) { uri ->
        if (uri == null) return@rememberLauncherForActivityResult
        scope.launch {
            val shrunk = withContext(Dispatchers.IO) { ProjectLogoStore.read(context, uri)?.let { ProjectLogoStore.shrink(it) } }
            logo = shrunk
            logoError = if (shrunk == null) "That image could not be read. Try a PNG or JPEG." else ""
        }
    }
    val hasHost = host.isNotBlank()
    val verified = result?.takeIf { it.ok }?.project
    val canCreate = !checking && if (hasHost) verified != null else name.isNotBlank()

    fun check() {
        if (checking || !hasHost) return
        checking = true
        scope.launch {
            result = projects.check(host, name, world)
            checking = false
        }
    }

    fun create() {
        scope.launch {
            val picked = logo
            val logoName = if (picked == null) {
                ""
            } else {
                withContext(Dispatchers.IO) { projects.logos.save(ProjectLogoStore.png(picked), name.ifBlank { host }) }.orEmpty()
            }
            val created = if (hasHost) {
                verified?.let { project -> projects.add(if (name.isBlank()) project else project.copy(label = name.trim())) }
            } else {
                projects.addNamed(name, logoName)
            }
            if (created == null) {
                if (logoName.isNotEmpty()) projects.logos.delete(logoName)
                return@launch
            }
            if (picked != null && logoName.isNotEmpty()) {
                ProjectImages.remember(logoName, picked)
                if (hasHost) projects.setLogo(created.id, logoName)
            } else if (hasHost) {
                projects.adoptInstanceLogo(created.id, result?.logoUrl.orEmpty(), result?.branded == true)
            }
            done(projects.directory.byId(created.id) ?: created)
        }
    }

    FmAlertDialog(
        onDismissRequest = { done(null) },
        title = "New project",
        containerColor = c.bgOverlay,
        shape = RoundedCornerShape(FmRadius.lg),
        actions = {
            FmTextButton(onClick = { done(null) }) { Text("Cancel") }
            if (hasHost && verified == null) {
                Button(onClick = { check() }, enabled = !checking) { Text(if (checking) "Checking…" else "Check instance") }
            } else {
                Button(onClick = { create() }, enabled = canCreate, modifier = Modifier.testTag("add-project-create")) { Text("Create") }
            }
        },
        content = {
            Text(
                "A project keeps its own sign-in, keys and data on this phone. A name is all it needs; attach an instance if it lives on one. Without one it is not linked to any instance and shows no repositories until you choose one.",
                style = Fm.text.bodySmall.copy(color = c.textSecondary),
            )
            Spacer(Modifier.height(FmSpace.x3))
            Row(verticalAlignment = Alignment.CenterVertically, modifier = Modifier.padding(bottom = FmSpace.x3)) {
                val picked = logo
                if (picked != null) {
                    Image(
                        bitmap = picked.asImageBitmap(),
                        contentDescription = null,
                        contentScale = ContentScale.Fit,
                        modifier = Modifier.size(48.dp).clip(RoundedCornerShape(12.dp)).background(c.bgRaised),
                    )
                } else {
                    ProjectIcon(Project(id = "preview", label = name.trim().ifEmpty { "?" }, serverUrl = "", hasInstance = false), size = 48.dp)
                }
                Spacer(Modifier.width(FmSpace.x3))
                FmTextButton(
                    onClick = { pickLogo.launch(PickVisualMediaRequest(ActivityResultContracts.PickVisualMedia.ImageOnly)) },
                    modifier = Modifier.testTag("add-project-logo"),
                ) { Text(if (picked == null) "Choose logo…" else "Change logo") }
                if (picked != null) FmTextButton(onClick = { logo = null }) { Text("Remove") }
            }
            if (logoError.isNotEmpty()) Text(logoError, style = Fm.text.bodySmall.copy(color = c.danger))
            AccountLabeledField(label = "Name", value = name, onValueChange = { name = it }, hint = "My project")
            AccountLabeledField(label = "Instance (optional)", value = host, onValueChange = { host = it; result = null }, hint = "relay.example.com")
            if (hasHost) {
                AccountLabeledField(label = "World host (optional)", value = world, onValueChange = { world = it; result = null }, hint = "world.example.com")
            }
            result?.let { probe ->
                Text(probe.message, style = Fm.text.bodySmall.copy(color = if (probe.ok) c.success else c.danger))
                if (probe.ok && probe.missingOptional.isNotEmpty()) {
                    Text(
                        "Not on this instance: " + probe.missingOptional.joinToString(", ") { it.label },
                        style = Fm.text.bodySmall.copy(color = c.textTertiary),
                    )
                }
            }
        },
    )
}

class ProjectsScreen : AccountScreen() {
    override val title: String get() = "Projects"

    @Composable
    override fun Content(navigator: Navigator) {
        val c = Fm.colors
        Column(
            Modifier
                .fillMaxSize()
                .background(c.bgBase)
                .windowInsetsPadding(fmSafeAreaInsets()),
        ) {
            Row(Modifier.fillMaxWidth().padding(FmSpace.x2), verticalAlignment = Alignment.CenterVertically) {
                FmIconButton(icon = "arrow_back", label = "Back", onPressed = { navigator.pop() })
                Text("Projects", style = MaterialTheme.typography.titleLarge, modifier = Modifier.weight(1f))
            }
            LazyColumn(Modifier.fillMaxSize().padding(horizontal = FmSpace.x4)) {
                item {
                    FmCard(modifier = Modifier.fillMaxWidth().padding(vertical = FmSpace.x2), radius = FmRadius.lg) {
                        ProjectsCard(navigator)
                    }
                }
            }
        }
    }
}

class NetworksScreen : AccountScreen() {
    private var entries by mutableStateOf<List<ProjectCatalogEntry>>(emptyList())
    private var started = false

    override val title: String get() = "All networks"

    private fun refresh(projects: ProjectsService) {
        scope.launch { projects.refreshCatalog { update -> scope.launch { entries = update } } }
    }

    @Composable
    override fun Content(navigator: Navigator) {
        val projects = Services.get<ProjectsService>() ?: return
        val messenger = LocalFmMessenger.current
        val c = Fm.colors
        LaunchedEffect(Unit) {
            if (!started) {
                started = true
                refresh(projects)
            }
        }
        Column(
            Modifier
                .fillMaxSize()
                .background(c.bgBase)
                .windowInsetsPadding(fmSafeAreaInsets()),
        ) {
            Row(Modifier.fillMaxWidth().padding(FmSpace.x2), verticalAlignment = Alignment.CenterVertically) {
                FmIconButton(icon = "arrow_back", label = "Back", onPressed = { navigator.pop() })
                Text("All networks", style = MaterialTheme.typography.titleLarge, modifier = Modifier.weight(1f))
                FmIconButton(icon = "refresh", label = "Refresh", onPressed = { refresh(projects) })
            }
            FmInlineStatus(ProjectActions.status, Modifier.padding(horizontal = FmSpace.x4))
            LazyColumn(Modifier.fillMaxSize().padding(horizontal = FmSpace.x4)) {
                for (entry in entries) {
                    item(key = "h-" + entry.project.id) {
                        Column(Modifier.fillMaxWidth().padding(top = FmSpace.x4, bottom = FmSpace.x2)) {
                            Text(
                                entry.project.label + if (entry.active) " · this project" else "",
                                style = Fm.text.bodyMedium,
                            )
                            Text(
                                when {
                                    !entry.signedIn -> "Not signed in. Switch to ${entry.project.label} to sign in."
                                    entry.loading -> "Loading…"
                                    entry.error.isNotEmpty() -> entry.error
                                    else -> "${entry.repositories.size} repositories"
                                },
                                style = Fm.text.bodySmall.copy(color = if (entry.error.isNotEmpty()) c.danger else c.textTertiary),
                            )
                            if (!entry.signedIn && !entry.active) {
                                FmTextButton(onClick = { ProjectActions.switchTo(entry.project.id) }) { Text("Switch and sign in") }
                            }
                        }
                    }
                    items(entry.repositories, key = { "r-" + entry.project.id + "/" + it.owner + "/" + it.name }) { repo ->
                        FmCard(
                            modifier = Modifier.fillMaxWidth().padding(vertical = FmSpace.x1),
                            radius = FmRadius.lg,
                            onTap = {
                                if (entry.active) {
                                    navigator.push(CodeFeature.repoDetailScreen(repo))
                                } else {
                                    scope.launch {
                                        val go = messenger.showConfirmDialog(
                                            title = "Open on ${entry.project.label}?",
                                            body = "${repo.owner}/${repo.name} lives on ${entry.project.label}. Switch projects to open it.",
                                            confirmLabel = "Switch",
                                        )
                                        if (go) ProjectActions.switchTo(entry.project.id)
                                    }
                                }
                            },
                        ) {
                            Column(Modifier.padding(FmSpace.x3)) {
                                Text("${repo.displayOwner}/${repo.name}", style = Fm.text.bodyMedium, maxLines = 1, overflow = TextOverflow.Ellipsis)
                                if (repo.description.isNotEmpty()) {
                                    Text(repo.description, style = Fm.text.bodySmall.copy(color = c.textSecondary), maxLines = 2, overflow = TextOverflow.Ellipsis)
                                }
                            }
                        }
                    }
                }
                item { Spacer(Modifier.size(FmSpace.x6)) }
            }
        }
    }
}

@Composable
fun ProjectChip(modifier: Modifier = Modifier) {
    val projects = Services.get<ProjectsService>() ?: return
    val active by projects.active.collectAsState()
    val c = Fm.colors
    Row(
        modifier
            .background(c.bgOverlay, RoundedCornerShape(FmRadius.lg))
            .clickable(onClickLabel = "Switch project", role = Role.Button) { ProjectDrawer.open() }
            .padding(horizontal = FmSpace.x3, vertical = FmSpace.x2),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        FmIcon("public", size = 16.dp, tint = c.textSecondary)
        Spacer(Modifier.width(FmSpace.x2))
        Text(active.label, style = Fm.text.bodySmall.copy(color = c.textSecondary))
        FmIcon("expand_more", size = 16.dp, tint = c.textSecondary)
    }
}

@Composable
fun rememberProjectLinks(): Triple<String, String, String> {
    val projects = Services.get<ProjectsService>()
    val active = projects?.active?.collectAsState()?.value
    val homepage = active?.homepageUri ?: ProjectDefaults.PROD_HOMEPAGE_URL
    return Triple(LegalLinks.termsFor(homepage), LegalLinks.privacyFor(homepage), LegalLinks.membershipFor(homepage))
}
