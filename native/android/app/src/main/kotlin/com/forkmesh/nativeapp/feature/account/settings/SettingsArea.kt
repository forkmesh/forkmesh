package com.forkmesh.nativeapp.feature.account.settings

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.LazyListScope
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import com.forkmesh.nativeapp.App
import com.forkmesh.nativeapp.api.ApiService
import com.forkmesh.nativeapp.api.PrefStore
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.feature.account.AuthService
import com.forkmesh.nativeapp.feature.navigation.NavigationFeature
import com.forkmesh.nativeapp.feature.tools.ToolsAppBar
import com.forkmesh.nativeapp.navigation.Route
import com.forkmesh.nativeapp.platform.AppearanceStore
import com.forkmesh.nativeapp.platform.ProjectLifecycle
import com.forkmesh.nativeapp.settings.AccountEndpointCalling
import com.forkmesh.nativeapp.settings.ApiAccountEndpoints
import com.forkmesh.nativeapp.settings.DeviceSettings
import com.forkmesh.nativeapp.settings.RecentAccountsStore
import com.forkmesh.nativeapp.settings.SettingsCatalog
import com.forkmesh.nativeapp.settings.SettingsSectionId
import com.forkmesh.nativeapp.ui.SoundCue
import com.forkmesh.nativeapp.ui.kit.FmCard
import com.forkmesh.nativeapp.ui.kit.FmSectionHeader
import com.forkmesh.nativeapp.ui.kit.FmSoundCues
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmRadius
import com.forkmesh.nativeapp.ui.theme.FmSpace
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch

object SettingsArea {
    private const val OWNER = "WS18"
    private val homeHidesBar = MutableStateFlow(true)

    val homeHidesBarIcons: StateFlow<Boolean> = homeHidesBar.asStateFlow()

    val prefs: PrefStore? get() = Services.get<PrefStore>()
    val api: ApiService? get() = Services.get<ApiService>()
    val endpoints: AccountEndpointCalling? get() = api?.let(::ApiAccountEndpoints)
    val account: String get() = Services.get<AuthService>()?.session?.value?.nodeName.orEmpty()
    val signedIn: Boolean get() = Services.get<AuthService>()?.session?.value != null && Services.get<AuthService>()?.isGuest?.value != true

    fun setHomeHidesBarIcons(on: Boolean) {
        prefs?.let { DeviceSettings.setHomeHidesBarIcons(it, on) }
        homeHidesBar.value = on
    }

    fun install(app: App) {
        prefs?.let { store ->
            DeviceSettings.migrateLegacySounds(store)
            AppearanceStore.hapticsEnabled = DeviceSettings.hapticsEnabled(store)
            homeHidesBar.value = DeviceSettings.homeHidesBarIcons(store)
        }
        NavigationFeature.register(Route.Kind.Settings, OWNER) { route -> screenFor(SettingsCatalog.section((route as? Route.Settings)?.section)) }
        NavigationFeature.register(Route.Kind.Profile, OWNER) { route -> AccountProfileScreen((route as Route.Profile).name) }
        val auth = Services.get<AuthService>()
        if (auth != null) {
            ProjectLifecycle.scope.launch {
                auth.session.collect { session ->
                    val store = prefs ?: return@collect
                    if (session != null && auth.isGuest.value != true && session.nodeName.isNotEmpty()) {
                        RecentAccountsStore(store).record(session.nodeName, session.email, System.currentTimeMillis())
                    }
                }
            }
        }
    }

    fun screenFor(section: SettingsSectionId?): Screen = when (section) {
        null, SettingsSectionId.QuickSetup -> SettingsHubScreen(section)
        SettingsSectionId.General -> GeneralSettingsScreen()
        SettingsSectionId.Repos -> ReposSettingsScreen()
        SettingsSectionId.Voice -> VoiceSettingsScreen()
        SettingsSectionId.Identity -> IdentitySettingsScreen()
        SettingsSectionId.SshKeys -> SshKeysScreen()
        SettingsSectionId.Mcp -> McpConnectorScreen()
        SettingsSectionId.Desktops -> DesktopsSettingsScreen()
        SettingsSectionId.Nodes -> LinkNodeScreen()
        SettingsSectionId.Data -> AccountDataScreen()
        SettingsSectionId.Security -> SecurityReportScreen()
        SettingsSectionId.Account -> AccountSwitchScreen()
        SettingsSectionId.Organizations -> com.forkmesh.nativeapp.feature.account.AccountFeature.orgsScreen()
        SettingsSectionId.InstalledApps -> com.forkmesh.nativeapp.feature.account.AccountFeature.installedAppsScreen()
        else -> SettingsScreen()
    }
}

@Composable
internal fun SettingsPage(title: String, navigator: Navigator, actions: @Composable androidx.compose.foundation.layout.RowScope.() -> Unit = {}, content: LazyListScope.() -> Unit) {
    Column(Modifier.fillMaxSize().background(Fm.colors.bgBase)) {
        ToolsAppBar(title, navigator, actions = actions)
        LazyColumn(Modifier.fillMaxSize(), contentPadding = PaddingValues(FmSpace.x4), verticalArrangement = Arrangement.spacedBy(FmSpace.x3), content = content)
    }
}

@Composable
internal fun SettingsCard(title: String? = null, content: @Composable ColumnScope.() -> Unit) {
    Column(Modifier.fillMaxWidth()) {
        if (title != null) {
            FmSectionHeader(title = title)
        }
        FmCard(modifier = Modifier.fillMaxWidth().padding(top = if (title != null) FmSpace.x2 else FmSpace.x0), radius = FmRadius.lg) {
            Column(Modifier.fillMaxWidth(), verticalArrangement = Arrangement.spacedBy(FmSpace.x2), content = content)
        }
    }
}

@Composable
internal fun SettingsNote(text: String, color: Color = Fm.colors.textSecondary) {
    Text(text, style = Fm.text.bodySmall.copy(color = color))
}

@Composable
internal fun SettingsSwitch(title: String, subtitle: String?, checked: Boolean, onChange: (Boolean) -> Unit) {
    Row(Modifier.fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
        Column(Modifier.weight(1f)) {
            Text(title, style = Fm.text.bodyLarge)
            if (subtitle != null) SettingsNote(subtitle)
        }
        Switch(checked = checked, onCheckedChange = {
            FmSoundCues.play(SoundCue.toggle(it))
            onChange(it)
        })
    }
}
