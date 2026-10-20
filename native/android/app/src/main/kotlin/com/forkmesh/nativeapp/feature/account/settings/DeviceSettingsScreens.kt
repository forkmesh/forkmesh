package com.forkmesh.nativeapp.feature.account.settings

import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.input.KeyboardType
import com.forkmesh.nativeapp.platform.AppearanceStore
import com.forkmesh.nativeapp.settings.DeviceSettings
import com.forkmesh.nativeapp.ui.SoundCuePrefs
import com.forkmesh.nativeapp.ui.kit.FmListRow
import com.forkmesh.nativeapp.ui.kit.FmTextButton
import com.forkmesh.nativeapp.ui.kit.FmTextField
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import java.util.Locale

class GeneralSettingsScreen : Screen() {
    override val title: String get() = "General"

    @Composable
    override fun Content(navigator: Navigator) {
        val prefs = SettingsArea.prefs
        var haptics by remember { mutableStateOf(prefs?.let(DeviceSettings::hapticsEnabled) ?: true) }
        var sounds by remember { mutableStateOf(SoundCuePrefs.enabled(prefs)) }
        val hideBarIcons by SettingsArea.homeHidesBarIcons.collectAsState()
        SettingsPage("General", navigator) {
            item { SettingsCard("Appearance") { AppearanceCard(navigator) } }
            item {
                SettingsCard("Home screen") {
                    SettingsSwitch("Hide icons in the bottom bar", "Icons already in the bottom bar leave Home so each one shows once.", hideBarIcons) {
                        SettingsArea.setHomeHidesBarIcons(it)
                    }
                }
            }
            item { SettingsCard("Chat privacy") { com.forkmesh.nativeapp.feature.chat.ChatPrivacySettings() } }
            item {
                SettingsCard("Feedback") {
                    SettingsSwitch("Haptics", "Small vibrations when you switch tabs, send and confirm.", haptics) {
                        haptics = it
                        prefs?.let { store -> DeviceSettings.setHapticsEnabled(store, it) }
                        AppearanceStore.hapticsEnabled = it
                    }
                    SettingsSwitch("Sounds", "Soft notes when you tap, switch tabs, send, finish or delete. On when the app opens, even in silent mode; switch them off here.", sounds) {
                        sounds = it
                        prefs?.let { store -> SoundCuePrefs.setEnabled(store, it) }
                    }
                }
            }
            item { SettingsCard("Genie") { com.forkmesh.nativeapp.feature.genie.GenieSpiderSwitchRow() } }
            item {
                SettingsCard("Suggestion mode") {
                    com.forkmesh.nativeapp.feature.genie.SuggestionModeRow()
                    com.forkmesh.nativeapp.feature.genie.SuggestionModeNote()
                    com.forkmesh.nativeapp.feature.pocket.PocketModeRow(onClose = {})
                }
            }
        }
    }
}

class ReposSettingsScreen : Screen() {
    override val title: String get() = "Repositories on this phone"

    @Composable
    override fun Content(navigator: Navigator) {
        val prefs = SettingsArea.prefs
        var name by remember { mutableStateOf(prefs?.let(DeviceSettings::localAuthorName).orEmpty()) }
        var email by remember { mutableStateOf(prefs?.let(DeviceSettings::localAuthorEmail).orEmpty()) }
        var branch by remember { mutableStateOf(prefs?.let(DeviceSettings::localDefaultBranch).orEmpty()) }
        var branchError by remember { mutableStateOf<String?>(null) }
        SettingsPage("Repositories on this phone", navigator) {
            item {
                SettingsCard("Commit author") {
                    SettingsNote("Leave both empty to sign commits with your account name.")
                    FmTextField(name, {
                        name = it
                        prefs?.let { store -> DeviceSettings.setLocalAuthorName(store, it) }
                    }, Modifier.fillMaxWidth(), hint = "Name")
                    FmTextField(
                        email,
                        {
                            email = it
                            prefs?.let { store -> DeviceSettings.setLocalAuthorEmail(store, it) }
                        },
                        Modifier.fillMaxWidth(),
                        hint = "Email",
                        errorText = if (email.isNotEmpty() && !DeviceSettings.isPlausibleEmail(email)) "That does not look like an email address." else null,
                        keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Email),
                    )
                }
            }
            item {
                SettingsCard("Default branch") {
                    SettingsNote("New repositories on this phone start on this branch. Leave it empty for main.")
                    FmTextField(
                        branch,
                        {
                            branch = it
                            val saved = prefs?.let { store -> DeviceSettings.setLocalDefaultBranch(store, it) } ?: true
                            branchError = if (saved) null else "Use a valid Git branch name, like main or trunk."
                        },
                        Modifier.fillMaxWidth(),
                        hint = "main",
                        errorText = branchError,
                    )
                }
            }
        }
    }
}

class VoiceSettingsScreen : Screen() {
    override val title: String get() = "Voice"

    @Composable
    override fun Content(navigator: Navigator) {
        val prefs = SettingsArea.prefs
        var locale by remember { mutableStateOf(prefs?.let(DeviceSettings::dictationLocale).orEmpty()) }
        val choices = listOf("", "en-US", "en-GB", "de-DE", "es-ES", "fr-FR", "it-IT", "ja-JP", "ko-KR", "nl-NL", "pt-BR", "zh-CN")
        fun choose(tag: String) {
            locale = tag
            prefs?.let { DeviceSettings.setDictationLocale(it, tag) }
        }
        SettingsPage("Voice", navigator) {
            item {
                SettingsCard("Dictation language") {
                    SettingsNote("Dictation listens only while /dictate is on in the composer and keeps nothing on the phone.")
                    for (tag in choices) {
                        val label = if (tag.isEmpty()) "Same as the phone" else Locale.forLanguageTag(tag).displayName
                        FmListRow(
                            label,
                            trailing = if (locale == tag) ({ Text("✓", style = Fm.text.bodyLarge.copy(color = Fm.colors.accent)) }) else null,
                            onTap = { choose(tag) },
                            showChevron = false,
                        )
                    }
                    FmTextButton(onClick = { choose("") }) { Text("Reset") }
                }
            }
        }
    }
}
