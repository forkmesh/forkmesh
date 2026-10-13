package com.forkmesh.nativeapp.settings

import java.text.Normalizer

enum class SettingsSectionId(val raw: String) {
    QuickSetup("quickSetup"),
    Profile("profile"),
    Membership("membership"),
    General("general"),
    Repos("repos"),
    Agents("agents"),
    Voice("voice"),
    Identity("identity"),
    SshKeys("sshKeys"),
    Mcp("mcp"),
    Organizations("organizations"),
    InstalledApps("installedApps"),
    Device("device"),
    Desktops("desktops"),
    Nodes("nodes"),
    Sessions("sessions"),
    Notifications("notifications"),
    Relay("relay"),
    Imports("imports"),
    Data("data"),
    Security("security"),
    Account("account"),
    Sponsored("sponsored"),
    OnDeviceAI("onDeviceAI"),
    Legal("legal"),
}

data class SettingsSectionEntry(val id: SettingsSectionId, val title: String, val icon: String, val keywords: List<String>)

object SettingsCatalog {
    val entries: List<SettingsSectionEntry> = listOf(
        SettingsSectionEntry(SettingsSectionId.QuickSetup, "Quick setup", "task_alt", listOf("start", "onboarding", "first run", "pair")),
        SettingsSectionEntry(SettingsSectionId.Profile, "Profile", "person_outline", listOf("name", "display name", "status", "away", "avatar", "node id")),
        SettingsSectionEntry(SettingsSectionId.Membership, "Membership", "favorite_outline", listOf("patreon", "plan", "tier", "member")),
        SettingsSectionEntry(SettingsSectionId.General, "General", "tune", listOf("appearance", "theme", "dark", "light", "accent", "color", "haptics", "vibration", "vibrate", "sounds", "audio", "mute", "silent", "noise", "volume", "beep", "click")),
        SettingsSectionEntry(SettingsSectionId.Repos, "Repositories on this phone", "folder_outlined", listOf("commit", "author", "email", "branch", "local", "clone")),
        SettingsSectionEntry(SettingsSectionId.Agents, "Agents", "smart_toy_outlined", listOf("provider", "model", "claude", "codex", "desktop", "default")),
        SettingsSectionEntry(SettingsSectionId.Voice, "Voice", "tune_rounded", listOf("dictation", "speech", "language", "microphone")),
        SettingsSectionEntry(SettingsSectionId.Identity, "Identity and recovery", "fingerprint", listOf("key", "backup", "recovery kit", "ed25519", "wallet", "public address", "qr", "trust list")),
        SettingsSectionEntry(SettingsSectionId.SshKeys, "SSH keys", "key_outlined", listOf("ssh", "public key", "git", "gateway")),
        SettingsSectionEntry(SettingsSectionId.Mcp, "MCP connector", "hub_outlined", listOf("mcp", "api token", "token", "claude code", "codex", "bot", "remote")),
        SettingsSectionEntry(SettingsSectionId.Organizations, "Organizations", "apartment_outlined", listOf("org", "team", "members")),
        SettingsSectionEntry(SettingsSectionId.InstalledApps, "Installed apps", "extension_outlined", listOf("apps", "integrations", "bots", "slash commands", "revoke", "app directory")),
        SettingsSectionEntry(SettingsSectionId.Device, "This device", "phone_android", listOf("serve as node", "phone")),
        SettingsSectionEntry(SettingsSectionId.Desktops, "Desktops", "desktop_mac_outlined", listOf("pair", "paired", "desktop", "rename", "forget", "backups")),
        SettingsSectionEntry(SettingsSectionId.Nodes, "Nodes on this account", "dns_outlined", listOf("node", "link", "link node", "scan", "qr", "devices")),
        SettingsSectionEntry(SettingsSectionId.Sessions, "Where you're signed in", "devices_other", listOf("sessions", "sign out", "revoke")),
        SettingsSectionEntry(SettingsSectionId.Notifications, "Notifications", "notifications_none_rounded", listOf("push", "pings", "email", "quiet hours", "background")),
        SettingsSectionEntry(SettingsSectionId.Relay, "Relay", "cloud_outlined", listOf("server", "relay", "room", "passphrase", "network")),
        SettingsSectionEntry(SettingsSectionId.Imports, "Import tokens", "download_outlined", listOf("github", "gitlab", "import")),
        SettingsSectionEntry(SettingsSectionId.Data, "Data", "storage_outlined", listOf("export", "download", "cache", "clear", "backup")),
        SettingsSectionEntry(SettingsSectionId.Security, "Report a vulnerability", "verified_user_outlined", listOf("security", "vulnerability", "report", "bug bounty")),
        SettingsSectionEntry(SettingsSectionId.Account, "Account", "person_outline_rounded", listOf("sign out", "log out", "switch account", "delete account", "guest")),
        SettingsSectionEntry(SettingsSectionId.Sponsored, "Sponsored messages", "info_outline", listOf("ads", "sponsored")),
        SettingsSectionEntry(SettingsSectionId.OnDeviceAI, "On-device AI", "memory_outlined", listOf("gemma", "local model", "ai")),
        SettingsSectionEntry(SettingsSectionId.Legal, "Legal", "description_outlined", listOf("terms", "privacy")),
    )

    fun entry(id: SettingsSectionId): SettingsSectionEntry =
        entries.firstOrNull { it.id == id } ?: SettingsSectionEntry(id, id.raw, "settings_outlined", emptyList())

    fun fold(text: String): String =
        Normalizer.normalize(text, Normalizer.Form.NFD).replace(Regex("\\p{Mn}+"), "").lowercase().trim()

    fun matches(entry: SettingsSectionEntry, query: String): Boolean {
        val words = fold(query).split(Regex("\\s+")).filter { it.isNotEmpty() }
        if (words.isEmpty()) return true
        val haystack = (listOf(entry.title) + entry.keywords).joinToString(" | ") { fold(it) }
        return words.all { haystack.contains(it) }
    }

    fun visible(query: String, available: Set<SettingsSectionId>): List<SettingsSectionEntry> =
        entries.filter { it.id in available && matches(it, query) }

    private val settings: List<Pair<SettingsSectionId, List<String>>> = listOf(
        SettingsSectionId.General to listOf("Appearance", "theme", "dark", "light", "accent", "color"),
        SettingsSectionId.General to listOf("Haptics", "vibration", "vibrate"),
        SettingsSectionId.General to listOf("Sounds", "audio", "mute", "silent", "noise", "volume", "beep", "click"),
    )

    fun matchedSettings(id: SettingsSectionId, query: String): List<String> {
        val words = fold(query).split(Regex("\\s+")).filter { it.isNotEmpty() }
        if (words.isEmpty()) return emptyList()
        return settings.filter { (section, names) ->
            section == id && names.joinToString(" | ") { fold(it) }.let { haystack -> words.all { haystack.contains(it) } }
        }.map { it.second.first() }
    }

    private val aliases = mapOf(
        "ssh" to SettingsSectionId.SshKeys, "keys" to SettingsSectionId.SshKeys, "appearance" to SettingsSectionId.General,
        "theme" to SettingsSectionId.General, "sounds" to SettingsSectionId.General, "haptics" to SettingsSectionId.General, "wallet" to SettingsSectionId.Identity, "recovery" to SettingsSectionId.Identity,
        "backup" to SettingsSectionId.Identity, "export" to SettingsSectionId.Data, "vulnerability" to SettingsSectionId.Security,
        "phone" to SettingsSectionId.Desktops, "devices" to SettingsSectionId.Nodes, "linknode" to SettingsSectionId.Nodes,
        "apitoken" to SettingsSectionId.Mcp, "token" to SettingsSectionId.Mcp, "orgs" to SettingsSectionId.Organizations,
        "apps" to SettingsSectionId.InstalledApps,
        "pings" to SettingsSectionId.Notifications, "push" to SettingsSectionId.Notifications, "setup" to SettingsSectionId.QuickSetup,
        "quicksetup" to SettingsSectionId.QuickSetup,
    )

    fun section(named: String?): SettingsSectionId? {
        if (named.isNullOrEmpty()) return null
        val folded = fold(named).replace("-", "").replace("_", "")
        SettingsSectionId.entries.firstOrNull { it.raw.lowercase() == folded }?.let { return it }
        return aliases[folded]
    }
}

data class QuickSetupInput(
    val signedIn: Boolean,
    val guest: Boolean = false,
    val projectName: String = "",
    val desktopsPaired: Int = 0,
    val identityBackedUp: Boolean = false,
    val pushEnabled: Boolean = false,
)

enum class QuickSetupStepId { SignIn, Project, Desktop, Backup, Notifications }

data class QuickSetupStep(val id: QuickSetupStepId, val title: String, val detail: String, val done: Boolean)

object QuickSetup {
    fun steps(input: QuickSetupInput): List<QuickSetupStep> {
        val account = input.signedIn && !input.guest
        return listOf(
            QuickSetupStep(
                QuickSetupStepId.SignIn, "Sign in",
                if (account) "Signed in." else if (input.guest) "You are exploring as a guest." else "Sign in or create an account.",
                account,
            ),
            QuickSetupStep(
                QuickSetupStepId.Project, "Pick a project",
                if (input.projectName.isEmpty()) "Choose which ForkMesh instance this app talks to." else "Using ${input.projectName}.",
                input.projectName.isNotEmpty(),
            ),
            QuickSetupStep(
                QuickSetupStepId.Desktop, "Pair a desktop",
                if (input.desktopsPaired > 0) "${input.desktopsPaired} desktop${if (input.desktopsPaired == 1) "" else "s"} paired."
                else "Drive your desktop's agents from this phone.",
                input.desktopsPaired > 0,
            ),
            QuickSetupStep(
                QuickSetupStepId.Backup, "Back up your identity key",
                if (input.identityBackedUp) "A recovery kit exists for this key." else "Save an encrypted recovery kit to Files.",
                input.identityBackedUp,
            ),
            QuickSetupStep(
                QuickSetupStepId.Notifications, "Turn on notifications",
                if (input.pushEnabled) "Notifications are on." else "Get pings, approvals and review requests.",
                input.pushEnabled,
            ),
        )
    }

    fun remaining(input: QuickSetupInput): Int = steps(input).count { !it.done }

    fun next(input: QuickSetupInput): QuickSetupStep? = steps(input).firstOrNull { !it.done }
}
