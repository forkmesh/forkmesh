package com.forkmesh.nativeapp.feature.account.settings

import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.account.hasRealSession
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.feature.account.AuthService
import com.forkmesh.nativeapp.feature.account.projects.ProjectsService
import com.forkmesh.nativeapp.feature.agents.AgentsFeature
import com.forkmesh.nativeapp.security.DeviceIdentity
import com.forkmesh.nativeapp.settings.IdentityBackupStore
import com.forkmesh.nativeapp.settings.QuickSetup
import com.forkmesh.nativeapp.settings.QuickSetupInput
import com.forkmesh.nativeapp.settings.QuickSetupStepId
import com.forkmesh.nativeapp.settings.SettingsCatalog
import com.forkmesh.nativeapp.settings.SettingsSectionId
import com.forkmesh.nativeapp.ui.kit.FmListRow
import com.forkmesh.nativeapp.ui.kit.FmTextButton
import com.forkmesh.nativeapp.ui.kit.FmTextField
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon

class SettingsHubScreen(private val focus: SettingsSectionId? = null) : Screen() {
    override val title: String get() = "Settings"

    private var query by mutableStateOf("")
    private var revision by mutableIntStateOf(0)

    private fun available(): Set<SettingsSectionId> {
        val all = SettingsSectionId.entries.toMutableSet()
        all.remove(SettingsSectionId.OnDeviceAI)
        if (!SettingsArea.signedIn) {
            all.removeAll(setOf(SettingsSectionId.SshKeys, SettingsSectionId.Mcp, SettingsSectionId.Data, SettingsSectionId.Nodes, SettingsSectionId.Sessions))
        }
        if (!SettingsArea.signedIn || !hasRealSession(Services.get<AuthService>()?.session?.value)) {
            all.remove(SettingsSectionId.InstalledApps)
        }
        return all
    }

    private fun quickSetupInput(context: android.content.Context): QuickSetupInput {
        val auth = Services.get<com.forkmesh.nativeapp.feature.account.AuthService>()
        val key = Services.get<DeviceIdentity>()?.publicKeyB64url.orEmpty()
        val backup = SettingsArea.prefs?.let { IdentityBackupStore(it).state }
        return QuickSetupInput(
            signedIn = auth?.session?.value != null,
            guest = auth?.isGuest?.value == true,
            projectName = Services.get<ProjectsService>()?.directory?.active?.label.orEmpty(),
            desktopsPaired = runCatching { AgentsFeature.localControl?.paired?.size ?: 0 }.getOrDefault(0),
            identityBackedUp = backup != null && !backup.needsBackup(key),
            pushEnabled = runCatching {
                val projectId = Services.get<ProjectsService>()?.directory?.active?.id.orEmpty()
                androidx.core.app.NotificationManagerCompat.from(context).areNotificationsEnabled() &&
                    com.forkmesh.nativeapp.feature.push.BackgroundLink.isEnabled(context, projectId)
            }.getOrDefault(false),
        )
    }

    @OptIn(ExperimentalFoundationApi::class)
    @Composable
    override fun Content(navigator: Navigator) {
        revision
        val input = quickSetupInput(androidx.compose.ui.platform.LocalContext.current)
        val steps = QuickSetup.steps(input)
        val key = Services.get<DeviceIdentity>()?.publicKeyB64url.orEmpty()
        val backupStore = SettingsArea.prefs?.let(::IdentityBackupStore)
        val nag = backupStore?.state?.showsNag(key, System.currentTimeMillis()) == true && SettingsArea.signedIn
        val sections = SettingsCatalog.visible(query, available())
        SettingsPage("Settings", navigator) {
            stickyHeader(contentType = "settings-search") {
                FmTextField(value = query, onValueChange = { query = it }, modifier = Modifier.fillMaxWidth().background(Fm.colors.canvas), hint = "Search settings")
            }
            if (nag && query.isBlank()) {
                item {
                    SettingsCard {
                        Text("Back up your identity key", style = Fm.text.titleMedium)
                        SettingsNote("If you lose this phone, a recovery kit is the only way to keep this device's key. It is encrypted with a passphrase you choose.")
                        Row {
                            FmTextButton(onClick = { navigator.push(IdentitySettingsScreen()) }) { Text("Make a recovery kit") }
                            FmTextButton(onClick = {
                                backupStore.snooze(System.currentTimeMillis())
                                revision++
                            }) { Text("Later") }
                        }
                    }
                }
            }
            if (query.isBlank() && (focus == SettingsSectionId.QuickSetup || QuickSetup.remaining(input) > 0)) {
                item {
                    SettingsCard("Quick setup") {
                        for (step in steps) {
                            Row(verticalAlignment = Alignment.CenterVertically) {
                                FmIcon(if (step.done) "check_circle" else "radio_button_unchecked", size = 20.dp, tint = if (step.done) Fm.colors.success else Fm.colors.textSecondary)
                                Spacer(Modifier.width(12.dp))
                                androidx.compose.foundation.layout.Column(Modifier.weight(1f)) {
                                    Text(step.title, style = Fm.text.bodyLarge)
                                    SettingsNote(step.detail)
                                }
                                if (!step.done) {
                                    FmTextButton(onClick = {
                                        when (step.id) {
                                            QuickSetupStepId.SignIn -> navigator.push(SettingsScreen())
                                            QuickSetupStepId.Project -> navigator.push(SettingsScreen())
                                            QuickSetupStepId.Desktop -> navigator.push(AgentsFeature.localDesktopScreen())
                                            QuickSetupStepId.Backup -> navigator.push(IdentitySettingsScreen())
                                            QuickSetupStepId.Notifications -> navigator.push(com.forkmesh.nativeapp.feature.account.AccountFeature.pingSettingsScreen())
                                        }
                                    }) { Text("Open") }
                                }
                            }
                        }
                    }
                }
            }
            items(sections, key = { it.id.raw }) { entry ->
                FmListRow(
                    entry.title,
                    subtitle = SettingsCatalog.matchedSettings(entry.id, query).joinToString(", ").ifEmpty { null },
                    leading = { FmIcon(entry.icon, size = 22.dp, tint = Fm.colors.textSecondary) },
                    onTap = { navigator.push(SettingsArea.screenFor(entry.id)) },
                )
            }
            if (sections.isEmpty()) item { SettingsNote("No settings match “$query”.") }
        }
    }
}
