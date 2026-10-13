package com.forkmesh.nativeapp.apps

import com.forkmesh.nativeapp.format.relativeTime
import com.forkmesh.nativeapp.json.jsonLong
import com.forkmesh.nativeapp.json.jsonMaps
import com.forkmesh.nativeapp.json.jsonText

data class AppOption(val text: String, val value: String)

sealed class AppElement {
    abstract val actionId: String

    data class Button(override val actionId: String, val text: String, val value: String, val style: String) : AppElement()

    data class Select(override val actionId: String, val placeholder: String, val options: List<AppOption>) : AppElement()
}

sealed class AppBlock {
    data class Section(val text: String, val fields: List<String>) : AppBlock()

    data class Context(val text: String) : AppBlock()

    data object Divider : AppBlock()

    data class Actions(val elements: List<AppElement>) : AppBlock()
}

object AppBlocks {
    const val MAX_BLOCKS = 20
    const val MAX_FIELDS = 10
    const val MAX_ELEMENTS = 5
    const val MAX_OPTIONS = 25

    private fun text(value: Any?): String {
        val raw = if (value is Map<*, *>) value["text"] else value
        return if (raw is String) raw.trim() else if (raw is Number) jsonText(raw) else ""
    }

    private fun element(raw: Map<String, Any?>): AppElement? {
        val actionId = jsonText(raw["action_id"]).trim()
        if (actionId.isEmpty() || actionId.length > 64) return null
        return when (jsonText(raw["type"])) {
            "button" -> {
                val label = text(raw["text"])
                if (label.isEmpty()) return null
                val style = jsonText(raw["style"])
                AppElement.Button(actionId, label, jsonText(raw["value"]), if (style == "primary" || style == "danger") style else "")
            }
            "static_select" -> {
                val options = jsonMaps(raw["options"]).mapNotNull { option ->
                    val label = text(option["text"])
                    val value = jsonText(option["value"])
                    if (label.isEmpty() || value.isEmpty()) null else AppOption(label, value)
                }.take(MAX_OPTIONS)
                if (options.isEmpty()) return null
                AppElement.Select(actionId, text(raw["placeholder"]).ifEmpty { "Choose" }, options)
            }
            else -> null
        }
    }

    fun parse(value: Any?): List<AppBlock> {
        val out = ArrayList<AppBlock>()
        for (raw in jsonMaps(value)) {
            if (out.size >= MAX_BLOCKS) break
            when (jsonText(raw["type"])) {
                "divider" -> out.add(AppBlock.Divider)
                "section" -> {
                    val body = text(raw["text"])
                    val fields = (raw["fields"] as? List<*>).orEmpty().map(::text).filter { it.isNotEmpty() }.take(MAX_FIELDS)
                    if (body.isNotEmpty() || fields.isNotEmpty()) out.add(AppBlock.Section(body, fields))
                }
                "context" -> {
                    val body = text(raw["text"])
                    if (body.isNotEmpty()) out.add(AppBlock.Context(body))
                }
                "actions" -> {
                    val seen = HashSet<String>()
                    val elements = jsonMaps(raw["elements"]).mapNotNull(::element).filter { seen.add(it.actionId) }.take(MAX_ELEMENTS)
                    if (elements.isNotEmpty()) out.add(AppBlock.Actions(elements))
                }
            }
        }
        return out
    }
}

data class AppScope(val scope: String, val label: String, val hint: String) {
    companion object {
        fun list(value: Any?): List<AppScope> = jsonMaps(value).map {
            AppScope(jsonText(it["scope"]), jsonText(it["label"]).ifEmpty { jsonText(it["scope"]) }, jsonText(it["hint"]))
        }.filter { it.scope.isNotEmpty() }
    }
}

data class AppCommandInfo(val command: String, val description: String, val usageHint: String)

data class AppInfo(
    val id: String,
    val clientId: String,
    val slug: String,
    val name: String,
    val botName: String,
    val description: String,
    val scopes: List<String>,
    val events: List<String>,
    val commands: List<AppCommandInfo>,
    val bot: Boolean,
    val developer: String,
    val organization: String,
    val listing: String,
    val createdAt: Long,
) {
    companion object {
        fun fromJson(json: Map<String, Any?>): AppInfo = AppInfo(
            id = jsonText(json["id"]),
            clientId = jsonText(json["clientId"]),
            slug = jsonText(json["slug"]),
            name = jsonText(json["name"]),
            botName = jsonText(json["botName"]),
            description = jsonText(json["description"]),
            scopes = (json["scopes"] as? List<*>).orEmpty().map(::jsonText),
            events = (json["events"] as? List<*>).orEmpty().map(::jsonText),
            commands = jsonMaps(json["commands"]).map {
                AppCommandInfo(jsonText(it["command"]), jsonText(it["description"]), jsonText(it["usageHint"]))
            },
            bot = json["bot"] == true,
            developer = jsonText(json["developer"]),
            organization = jsonText(json["organization"]),
            listing = jsonText(json["listing"]),
            createdAt = jsonLong(json["createdAt"]),
        )
    }
}

private fun map(value: Any?): Map<String, Any?> {
    @Suppress("UNCHECKED_CAST")
    return value as? Map<String, Any?> ?: emptyMap()
}

data class AppInstall(
    val installId: String,
    val app: AppInfo,
    val targetKind: String,
    val targetName: String,
    val scopes: List<AppScope>,
    val installedBy: String,
    val createdAt: Long,
    val lastUsedAt: Long,
    val canRevoke: Boolean,
) {
    val targetLabel: String
        get() = when (targetKind) {
            "org" -> if (targetName.isEmpty()) "Organization" else "Organization $targetName"
            else -> "Just me"
        }

    fun lastUsedText(nowMs: Long = System.currentTimeMillis()): String = AppsText.lastUsed(lastUsedAt, nowMs)

    companion object {
        fun fromJson(json: Map<String, Any?>): AppInstall {
            val target = map(json["target"])
            return AppInstall(
                installId = jsonText(json["installId"]),
                app = AppInfo.fromJson(map(json["app"])),
                targetKind = jsonText(target["kind"]),
                targetName = jsonText(target["name"]),
                scopes = AppScope.list(json["scopes"]),
                installedBy = jsonText(json["installedBy"]),
                createdAt = jsonLong(json["createdAt"]),
                lastUsedAt = jsonLong(json["lastUsedAt"]),
                canRevoke = json["canRevoke"] == true,
            )
        }
    }
}

data class AppInstallRequest(
    val id: String,
    val app: AppInfo,
    val scopes: List<AppScope>,
    val requester: String,
    val createdAt: Long,
) {
    companion object {
        fun fromJson(json: Map<String, Any?>): AppInstallRequest = AppInstallRequest(
            id = jsonText(json["id"]),
            app = AppInfo.fromJson(map(json["app"])),
            scopes = AppScope.list(json["scopes"]),
            requester = jsonText(json["requester"]),
            createdAt = jsonLong(json["createdAt"]),
        )
    }
}

data class AppOrgInstalls(
    val name: String,
    val role: String,
    val canManage: Boolean,
    val installs: List<AppInstall>,
    val requests: List<AppInstallRequest>,
    val policy: Map<String, Any?>?,
) {
    companion object {
        fun fromJson(json: Map<String, Any?>): AppOrgInstalls = AppOrgInstalls(
            name = jsonText(json["name"]),
            role = jsonText(json["role"]),
            canManage = json["canManage"] == true,
            installs = jsonMaps(json["installs"]).map(AppInstall::fromJson).filter { it.installId.isNotEmpty() },
            requests = jsonMaps(json["requests"]).map(AppInstallRequest::fromJson).filter { it.id.isNotEmpty() },
            policy = (json["policy"] as? Map<*, *>)?.let(::map),
        )
    }
}

data class InstalledApps(
    val account: String,
    val installs: List<AppInstall>,
    val organizations: List<AppOrgInstalls>,
) {
    val isEmpty: Boolean
        get() = installs.isEmpty() && organizations.all { it.installs.isEmpty() && it.requests.isEmpty() }

    companion object {
        fun fromJson(json: Map<String, Any?>): InstalledApps = InstalledApps(
            account = jsonText(json["account"]),
            installs = jsonMaps(json["installs"]).map(AppInstall::fromJson).filter { it.installId.isNotEmpty() },
            organizations = jsonMaps(json["organizations"]).map(AppOrgInstalls::fromJson).filter { it.name.isNotEmpty() },
        )
    }
}

data class AppCommand(
    val command: String,
    val description: String,
    val usageHint: String,
    val appName: String,
    val appSlug: String,
    val installId: String,
)

data class AppCommands(val commands: List<AppCommand>, val mentionable: List<String>) {
    companion object {
        fun fromJson(json: Map<String, Any?>): AppCommands = AppCommands(
            commands = jsonMaps(json["commands"]).map {
                val app = map(it["app"])
                AppCommand(
                    command = jsonText(it["command"]).lowercase(),
                    description = jsonText(it["description"]),
                    usageHint = jsonText(it["usageHint"]),
                    appName = jsonText(app["name"]),
                    appSlug = jsonText(app["slug"]),
                    installId = jsonText(it["installId"]),
                )
            }.filter { it.command.isNotEmpty() },
            mentionable = (json["mentionable"] as? List<*>).orEmpty().map { jsonText(it).lowercase() }.filter { it.isNotEmpty() },
        )
    }
}

data class AppReply(
    val ok: Boolean,
    val responseType: String = "",
    val text: String = "",
    val blocks: List<AppBlock> = emptyList(),
    val appMessageId: String = "",
    val appName: String = "",
    val botName: String = "",
    val appSlug: String = "",
    val error: String = "",
    val status: Int = 0,
) {
    val isEphemeral: Boolean get() = responseType == "ephemeral"

    val errorText: String get() = AppsText.errorText(error, appName)

    companion object {
        fun fromJson(status: Int, json: Map<String, Any?>): AppReply {
            val app = json["app"]
            val appMap = map(app)
            val ok = status in 200..299 && json["ok"] != false
            return AppReply(
                ok = ok,
                responseType = jsonText(json["responseType"]),
                text = jsonText(json["text"]),
                blocks = AppBlocks.parse(json["blocks"]),
                appMessageId = jsonText(json["appMessageId"]),
                appName = if (app is String) app else jsonText(appMap["name"]),
                botName = jsonText(appMap["botName"]),
                appSlug = jsonText(appMap["slug"]),
                error = if (ok) "" else jsonText(json["error"]).ifEmpty { if (status > 0) "http_$status" else "" },
                status = status,
            )
        }
    }
}

data class SlashCommand(val command: String, val arguments: String)

object AppsText {
    private val SLASH = Regex("^/([a-z][a-z0-9-]{1,31})(?:\\s+([\\s\\S]*))?$", RegexOption.IGNORE_CASE)
    private val MENTION = Regex("(^|[^\\w@/-])@([a-z][a-z0-9-]{1,30}[a-z0-9])(?![\\w-])", RegexOption.IGNORE_CASE)
    private val COMMAND_PREFIX = Regex("^/[a-z0-9-]*$", RegexOption.IGNORE_CASE)

    const val APPS_COMMAND = "apps"
    const val EPHEMERAL_NOTE = "Only you can see this."
    const val EMPTY_TITLE = "No apps installed"
    const val DIRECTORY_PATH = "/dashboard/apps"

    fun parseSlash(text: String): SlashCommand? {
        val match = SLASH.matchEntire(text.trim()) ?: return null
        return SlashCommand(match.groupValues[1].lowercase(), match.groupValues[2])
    }

    fun mentionsApp(text: String): Boolean = MENTION.containsMatchIn(text)

    fun mentionedSlugs(text: String): List<String> {
        val out = ArrayList<String>()
        for (match in MENTION.findAll(text)) {
            val slug = match.groupValues[2].lowercase()
            if (slug !in out) out.add(slug)
        }
        return out
    }

    fun shouldForward(text: String, mentionable: Collection<String>): Boolean {
        if (mentionable.isEmpty()) return false
        val slugs = mentionable.map { it.lowercase() }.toSet()
        return mentionedSlugs(text).any { it in slugs }
    }

    fun errorText(code: String, appName: String = ""): String = when (code) {
        "unknown_command" -> "No installed app has that command. Type /apps to see yours."
        "app_unavailable" -> if (appName.isEmpty()) "The app did not answer." else "$appName did not answer."
        "rate_limited" -> "Too many requests to that app; try again in a minute."
        "message_expired" -> "That message is too old to use."
        else -> "The app could not handle that (${code.ifEmpty { "error" }})."
    }

    fun commandList(commands: List<AppCommand>): String =
        if (commands.isEmpty()) {
            "You have no app commands. Install apps from /dashboard/apps."
        } else {
            "Your app commands: " + commands.joinToString(", ") { "/${it.command} (${it.appName})" }
        }

    fun lastUsed(lastUsedAt: Long, nowMs: Long = System.currentTimeMillis()): String =
        if (lastUsedAt <= 0) "Never used" else "Last used ${relativeTime(lastUsedAt, nowMs)}"

    const val REVOKE_BODY = "It loses access at once and every token it holds is revoked."

    fun revokeTitle(appName: String): String = "Revoke ${appName.ifEmpty { "this app" }}?"

    fun manageError(code: String): String = when (code) {
        "forbidden", "not_org_admin" -> "You do not have permission to do that."
        "not_found" -> "That app or request is no longer there."
        "invalid_session" -> "Sign in again to manage your apps."
        "" -> "Could not reach the server. Check your connection and try again."
        else -> "The server refused the change ($code)."
    }

    fun installedBy(name: String): String = if (name.isEmpty()) "" else "Installed by $name"

    fun isCommandPrefix(text: String): Boolean = COMMAND_PREFIX.matches(text)
}
