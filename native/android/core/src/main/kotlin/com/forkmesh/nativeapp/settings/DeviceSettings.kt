package com.forkmesh.nativeapp.settings

import com.forkmesh.nativeapp.api.PrefStore

object DeviceSettings {
    const val HAPTICS_KEY = "device/haptics"
    const val SOUNDS_KEY = "device/sounds"
    const val HOME_HIDES_BAR_ICONS_KEY = "home/hideBarIcons"
    const val LEGACY_SOUNDS_KEY = "sounds.enabled"
    const val DICTATION_LOCALE_KEY = "voice/dictationLocale"
    const val LOCAL_AUTHOR_NAME_KEY = "repos/authorName"
    const val LOCAL_AUTHOR_EMAIL_KEY = "repos/authorEmail"
    const val LOCAL_DEFAULT_BRANCH_KEY = "repos/defaultBranch"
    const val FALLBACK_BRANCH = "main"

    fun hapticsEnabled(prefs: PrefStore): Boolean = prefs.getBool(HAPTICS_KEY) ?: true

    fun setHapticsEnabled(prefs: PrefStore, value: Boolean) = prefs.putBool(HAPTICS_KEY, value)

    fun homeHidesBarIcons(prefs: PrefStore): Boolean = prefs.getBool(HOME_HIDES_BAR_ICONS_KEY) ?: true

    fun setHomeHidesBarIcons(prefs: PrefStore, value: Boolean) = prefs.putBool(HOME_HIDES_BAR_ICONS_KEY, value)

    fun soundsEnabled(prefs: PrefStore): Boolean = prefs.getBool(SOUNDS_KEY) ?: true

    fun migrateLegacySounds(prefs: PrefStore) {
        if (!prefs.contains(LEGACY_SOUNDS_KEY)) return
        if (!prefs.contains(SOUNDS_KEY)) prefs.getBool(LEGACY_SOUNDS_KEY)?.let { prefs.putBool(SOUNDS_KEY, it) }
        prefs.remove(LEGACY_SOUNDS_KEY)
    }

    fun setSoundsEnabled(prefs: PrefStore, value: Boolean) = prefs.putBool(SOUNDS_KEY, value)

    fun dictationLocale(prefs: PrefStore): String = prefs.getString(DICTATION_LOCALE_KEY).orEmpty()

    fun setDictationLocale(prefs: PrefStore, identifier: String) {
        val cleaned = identifier.filter { it.isLetterOrDigit() || it == '-' || it == '_' }.take(35)
        if (cleaned.isEmpty()) prefs.remove(DICTATION_LOCALE_KEY) else prefs.putString(DICTATION_LOCALE_KEY, cleaned)
    }

    fun localAuthorName(prefs: PrefStore): String = prefs.getString(LOCAL_AUTHOR_NAME_KEY).orEmpty()

    fun localAuthorEmail(prefs: PrefStore): String = prefs.getString(LOCAL_AUTHOR_EMAIL_KEY).orEmpty()

    fun setLocalAuthorName(prefs: PrefStore, value: String) =
        prefs.putString(LOCAL_AUTHOR_NAME_KEY, value.filter { it != '\n' && it != '\r' && it != '<' && it != '>' }.take(64))

    fun setLocalAuthorEmail(prefs: PrefStore, value: String) =
        prefs.putString(LOCAL_AUTHOR_EMAIL_KEY, value.filter { !it.isWhitespace() && it != '<' && it != '>' }.take(254))

    fun localAuthorOverride(prefs: PrefStore): Pair<String, String>? {
        val name = localAuthorName(prefs).trim()
        val email = localAuthorEmail(prefs).trim()
        if (name.isEmpty() && email.isEmpty()) return null
        return name to email
    }

    fun localDefaultBranch(prefs: PrefStore): String {
        val stored = prefs.getString(LOCAL_DEFAULT_BRANCH_KEY).orEmpty().trim()
        return if (isValidBranchName(stored)) stored else FALLBACK_BRANCH
    }

    fun setLocalDefaultBranch(prefs: PrefStore, value: String): Boolean {
        val trimmed = value.trim()
        if (trimmed.isEmpty()) {
            prefs.remove(LOCAL_DEFAULT_BRANCH_KEY)
            return true
        }
        if (!isValidBranchName(trimmed)) return false
        prefs.putString(LOCAL_DEFAULT_BRANCH_KEY, trimmed)
        return true
    }

    fun isValidBranchName(name: String): Boolean {
        if (name.isEmpty() || name.length > 100 || name.startsWith("-") || name.startsWith("/") || name.endsWith("/") ||
            name.endsWith(".lock") || name.endsWith(".") || name.contains("..") || name.contains("//") || name.contains("@{")
        ) {
            return false
        }
        val banned = setOf(' ', '~', '^', ':', '?', '*', '[', '\\')
        return name.none { it in banned || it == '\n' || it == '\r' || (it.code < 128 && (it.code < 32 || it.code == 127)) }
    }

    fun isPlausibleEmail(email: String): Boolean {
        val parts = email.split('@')
        if (parts.size != 2 || parts[0].isEmpty() || !parts[1].contains('.') || parts[1].startsWith(".") || parts[1].endsWith(".")) return false
        return email.length <= 254 && email.none { it.isWhitespace() || it == '<' || it == '>' }
    }
}
