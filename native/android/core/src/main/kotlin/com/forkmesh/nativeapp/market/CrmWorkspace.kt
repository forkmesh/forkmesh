package com.forkmesh.nativeapp.market

import com.forkmesh.nativeapp.json.JsonMap
import com.forkmesh.nativeapp.json.bool
import com.forkmesh.nativeapp.json.int
import com.forkmesh.nativeapp.json.jsonLong
import com.forkmesh.nativeapp.json.jsonText
import com.forkmesh.nativeapp.json.list
import com.forkmesh.nativeapp.json.long
import com.forkmesh.nativeapp.json.maps
import com.forkmesh.nativeapp.json.optBool
import com.forkmesh.nativeapp.json.str
import com.forkmesh.nativeapp.json.strings
import java.net.URLEncoder

object CrmText {
    private val BLOCKS = setOf("p", "br", "div", "li", "tr", "h1", "h2", "h3", "h4", "blockquote", "ul", "ol", "table")

    fun strippingTags(html: String): String {
        if (html.isEmpty()) return ""
        val out = StringBuilder()
        var tag: StringBuilder? = null
        for (character in html) {
            when {
                character == '<' -> tag = StringBuilder()
                character == '>' && tag != null -> {
                    val word = tag.toString().lowercase().trim('/', ' ').takeWhile { it.isLetterOrDigit() }
                    out.append(if (word in BLOCKS) '\n' else ' ')
                    tag = null
                }
                tag != null -> tag.append(character)
                else -> out.append(character)
            }
        }
        var text = out.toString()
        listOf("&nbsp;" to " ", "&amp;" to "&", "&lt;" to "<", "&gt;" to ">", "&quot;" to "\"", "&#39;" to "'").forEach { (entity, plain) ->
            text = text.replace(entity, plain)
        }
        return text.lines().map { it.trim() }.filter { it.isNotEmpty() }.joinToString("\n")
    }

    fun domain(email: String): String = email.substringAfterLast('@', "").lowercase()

    fun slug(value: String): String {
        val out = StringBuilder()
        var dash = false
        for (character in value.lowercase()) {
            if (character in 'a'..'z' || character in '0'..'9') {
                out.append(character)
                dash = false
            } else if (!dash && out.isNotEmpty()) {
                out.append('-')
                dash = true
            }
        }
        val trimmed = out.toString().trimEnd('-').take(40).trimEnd('-')
        return if (trimmed.isNotEmpty() && trimmed[0] in 'a'..'z') trimmed else ""
    }

    fun profileUrl(field: String, value: String): String? {
        val raw = value.trim()
        if (raw.isEmpty()) return null
        if (raw.lowercase().startsWith("https://") || raw.lowercase().startsWith("http://")) return raw
        val handle = raw.removePrefix("@")
        return when (field) {
            "website" -> "https://$raw"
            "linkedin" -> if ("linkedin.com" in raw) "https://$raw" else "https://www.linkedin.com/in/$handle"
            "github" -> "https://github.com/$handle"
            "twitter" -> "https://x.com/$handle"
            "bluesky" -> "https://bsky.app/profile/$handle"
            "mastodon" -> handle.split('@').takeIf { it.size == 2 }?.let { "https://${it[1]}/@${it[0]}" }
            else -> null
        }
    }

    fun money(value: Long): String = when {
        value >= 1_000_000 -> String.format("%.1fM", value / 1_000_000.0).replace(".0M", "M")
        value >= 10_000 -> "${value / 1000}k"
        value >= 1000 -> String.format("%.1fk", value / 1000.0).replace(".0k", "k")
        else -> value.toString()
    }

    fun duration(seconds: Int): String = when {
        seconds < 60 -> "${seconds}s"
        seconds < 3600 -> "${seconds / 60}m ${seconds % 60}s"
        else -> "${seconds / 3600}h ${(seconds % 3600) / 60}m"
    }

    fun isDeckToken(value: String): Boolean = value.length in 10..40 && value.all { it in 'a'..'z' || it in '0'..'9' }

    fun encode(value: String): String = URLEncoder.encode(value, "UTF-8").replace("+", "%20")
}

object CrmColumnCatalog {
    val BUILT_IN: List<Pair<String, String>> = listOf(
        "name" to "Name", "email" to "Email", "company" to "Company", "title" to "Title", "phone" to "Phone",
        "twitter" to "Twitter", "mastodon" to "Mastodon", "bluesky" to "Bluesky", "linkedin" to "LinkedIn",
        "github" to "GitHub", "website" to "Website", "location" to "Location", "flow" to "Flow", "stage" to "Stage",
        "value" to "Deal value", "nextStep" to "Next step", "owner" to "Owner", "source" to "Source",
        "tags" to "Tags", "notes" to "Notes", "lastContactedAt" to "Last contacted",
    )

    val DELETABLE: Set<String> = BUILT_IN.map { it.first }.toSet() - setOf("name", "email", "flow", "stage", "lastContactedAt")

    val IMPORT_TARGETS: List<Pair<String, String>> =
        BUILT_IN + listOf("first_name" to "First name", "last_name" to "Last name", "custom" to "Its own column", "" to "Skip")

    fun isBuiltIn(field: String): Boolean = BUILT_IN.any { it.first == field }

    fun editorColumns(columns: List<CrmColumn>, removed: List<String>): List<CrmColumn> {
        val result = columns.filter { it.field !in removed || it.custom }.toMutableList()
        val known = result.map { it.field }.toSet()
        for ((field, label) in BUILT_IN) {
            if (field !in known && field !in removed) result.add(CrmColumn(field, label, custom = false, hidden = true))
        }
        return result
    }

    fun label(field: String): String =
        BUILT_IN.firstOrNull { it.first == field }?.second ?: field.split('_').joinToString(" ") { part -> part.replaceFirstChar { it.uppercase() } }
}

data class CrmColumn(val field: String, val label: String, val custom: Boolean, val hidden: Boolean) {
    val json: Map<String, Any?>
        get() = linkedMapOf("field" to this.field, "label" to label, "custom" to custom, "hidden" to hidden)

    companion object {
        fun fromJson(json: JsonMap): CrmColumn {
            val field = json.str("field")
            return CrmColumn(field, json.str("label", CrmColumnCatalog.label(field)), json.optBool("custom") ?: !CrmColumnCatalog.isBuiltIn(field), json.bool("hidden"))
        }
    }
}

data class CrmView(
    val id: String,
    val name: String,
    val columns: List<CrmColumn> = emptyList(),
    val visibility: String = "organization",
    val sharedWith: List<String> = emptyList(),
    val shareAccess: String = "view",
    val source: String = "manual",
    val fileName: String = "",
    val flow: String = "",
    val owner: String = "",
    val canManage: Boolean = false,
    val canEdit: Boolean = false,
    val organization: String = "",
    val stages: List<String> = emptyList(),
) {
    val isPipeline: Boolean get() = flow.isNotEmpty()
    val isPrivate: Boolean get() = visibility == "private"
    val visibleColumns: List<CrmColumn> get() = columns.filter { !it.hidden }
    val summaryColumns: List<CrmColumn>
        get() = visibleColumns.filter { it.field !in setOf("name", "email", "company", "title", "flow", "stage", "value", "nextStep", "tags", "notes") }

    companion object {
        fun fromJson(json: JsonMap) = CrmView(
            id = json.str("id"), name = json.str("name", "Untitled sheet"),
            columns = json.maps("columns").map(CrmColumn::fromJson).filter { it.field.isNotEmpty() },
            visibility = json.str("visibility", "organization"), sharedWith = json.strings("sharedWith"),
            shareAccess = json.str("shareAccess", "view"), source = json.str("source", "manual"), fileName = json.str("fileName"),
            flow = json.str("flow"), owner = json.str("owner"), canManage = json.bool("canManage"), canEdit = json.bool("canEdit"),
            organization = json.str("organization"), stages = json.strings("stages"),
        )
    }
}

sealed class CrmTab(val id: String) {
    data class Sheet(val viewId: String) : CrmTab("list:$viewId")
    data class Pipeline(val flowId: String) : CrmTab("pipeline:$flowId")
    data object NoPipeline : CrmTab("no-pipeline")
    data object Activity : CrmTab("activity")
    data object Mail : CrmTab("mail")
    data object Outreach : CrmTab("outreach")
    data object Team : CrmTab("team")
    data object Decks : CrmTab("decks")
    data object Shared : CrmTab("shared")

    val isContacts: Boolean get() = this is Sheet || this is Pipeline || this is NoPipeline
}

data class CrmTabItem(val tab: CrmTab, val title: String, val icon: String, val count: Int?, val view: CrmView? = null, val flow: CrmFlow? = null)

object CrmFlowIcon {
    val ALL = listOf("pipeline", "investors", "hiring", "partners", "customers", "sales", "support", "projects", "events")

    fun symbol(icon: String): String = when (icon) {
        "investors" -> "payments"
        "hiring" -> "person_add"
        "partners" -> "handshake"
        "customers" -> "groups"
        "sales" -> "shopping_cart"
        "support" -> "support"
        "projects" -> "folder"
        "events" -> "event"
        else -> "filter_alt"
    }

    fun title(icon: String): String = if (icon == "pipeline") "Funnel" else CrmStageLabel.title(icon)
}

val CrmOrganizationData.pipelineLists: Map<String, List<String>>
    get() = views.filter { it.isPipeline }.groupBy({ it.flow }, { it.id })

fun CrmOrganizationData.shows(contact: CrmContact, view: CrmView): Boolean {
    if (!view.isPipeline) return view.id in contact.views
    if (contact.flow != view.flow) return false
    val lists = pipelineLists[view.flow] ?: listOf(view.id)
    return (lists.firstOrNull { it in contact.views } ?: lists.first()) == view.id
}

val CrmOrganizationData.strayContacts: List<CrmContact>
    get() {
        val live = flows.map { it.id }.toSet()
        return contacts.filter { it.flow !in live }
    }

val CrmOrganizationData.listViews: List<CrmView> get() = views.filter { !it.isPipeline }

val CrmOrganizationData.pipelineValue: Long get() = contacts.sumOf { it.value }

val CrmOrganizationData.tags: List<String> get() = contacts.flatMap { it.tags }.distinct().sorted()

val CrmOrganizationData.owners: List<String>
    get() {
        val seen = HashSet<String>()
        return (members + contacts.map { it.owner }).filter { it.isNotEmpty() && seen.add(it.lowercase()) }.sorted()
    }

fun CrmOrganizationData.contactById(id: String): CrmContact? = contacts.firstOrNull { it.id == id }

fun CrmOrganizationData.contactByEmail(email: String): CrmContact? {
    val needle = email.trim().lowercase()
    return if (needle.isEmpty()) null else contacts.firstOrNull { it.email.lowercase() == needle }
}

fun CrmOrganizationData.decksFor(contactId: String): List<CrmDeckLink> = decks.filter { it.contactId == contactId }

fun CrmOrganizationData.contactsFor(tab: CrmTab): List<CrmContact> = when (tab) {
    is CrmTab.Sheet -> views.firstOrNull { it.id == tab.viewId }?.let { view -> contacts.filter { shows(it, view) } }.orEmpty()
    is CrmTab.Pipeline -> contacts.filter { it.flow == tab.flowId }
    CrmTab.NoPipeline -> strayContacts
    else -> emptyList()
}

fun CrmOrganizationData.flowFor(tab: CrmTab): CrmFlow? = when (tab) {
    is CrmTab.Sheet -> views.firstOrNull { it.id == tab.viewId }?.takeIf { it.isPipeline }?.let { view -> flows.firstOrNull { it.id == view.flow } }
    is CrmTab.Pipeline -> flows.firstOrNull { it.id == tab.flowId }
    else -> null
}

fun CrmOrganizationData.viewFor(tab: CrmTab): CrmView? = (tab as? CrmTab.Sheet)?.let { list -> views.firstOrNull { it.id == list.viewId } }

val CrmOrganizationData.contactTabs: List<CrmTabItem>
    get() {
        val items = mutableListOf<CrmTabItem>()
        val flowIds = flows.map { it.id }.toSet()
        for (view in views) {
            if (view.isPipeline && view.flow !in flowIds) continue
            val flow = if (view.isPipeline) flows.firstOrNull { it.id == view.flow } else null
            val tab = CrmTab.Sheet(view.id)
            val icon = flow?.let { CrmFlowIcon.symbol(it.icon) } ?: if (view.isPrivate) "lock" else "list"
            items.add(CrmTabItem(tab, view.name, icon, contactsFor(tab).size, view, flow))
        }
        val bound = views.filter { it.isPipeline }.map { it.flow }.toSet()
        for (flow in flows) {
            if (flow.id in bound) continue
            val tab = CrmTab.Pipeline(flow.id)
            items.add(CrmTabItem(tab, flow.name, CrmFlowIcon.symbol(flow.icon), contactsFor(tab).size, flow = flow))
        }
        val stray = strayContacts.size
        if (stray > 0) items.add(CrmTabItem(CrmTab.NoPipeline, "No pipeline", "help_outline", stray))
        return items
    }

val CrmOrganizationData.workspaceTabs: List<CrmTabItem>
    get() {
        val items = mutableListOf(
            CrmTabItem(CrmTab.Activity, "Activity", "history", null),
            CrmTabItem(CrmTab.Mail, "Mail", "inbox", mail.size.takeIf { it > 0 }),
            CrmTabItem(CrmTab.Outreach, "Outreach", "send", null),
            CrmTabItem(CrmTab.Team, "Team", "leaderboard", null),
            CrmTabItem(CrmTab.Decks, "Decks", "slideshow", decks.size.takeIf { it > 0 }),
        )
        if (sharedViews.isNotEmpty()) items.add(CrmTabItem(CrmTab.Shared, "Shared with me", "group", sharedViews.size))
        return items
    }

fun CrmOrganizationData.tabForFlow(flowId: String): CrmTab? = contactTabs.firstOrNull { it.flow?.id == flowId }?.tab

fun CrmOrganizationData.customFieldKeys(): List<String> {
    val keys = LinkedHashSet<String>()
    views.forEach { view -> view.columns.filter { it.custom }.forEach { keys.add(it.field) } }
    contacts.forEach { contact -> contact.fields.keys.sorted().forEach { keys.add(it) } }
    return keys.toList()
}

fun CrmOrganizationData.duplicatesOf(contact: CrmContact): List<CrmContact> {
    val email = contact.email.lowercase()
    val name = contact.name.lowercase().trim()
    val domain = CrmText.domain(contact.email)
    return contacts.filter { other ->
        other.id != contact.id && (
            (email.isNotEmpty() && other.email.lowercase() == email) ||
                (name.isNotEmpty() && other.name.lowercase().trim() == name) ||
                (domain.isNotEmpty() && name.isNotEmpty() && CrmText.domain(other.email) == domain &&
                    other.name.lowercase().split(' ').last() == name.split(' ').last())
            )
    }
}

enum class CrmSort(val title: String) {
    STAGE("Stage"), NAME("Name"), VALUE("Deal value"), UPDATED("Recently updated"), LAST_CONTACTED("Least recently contacted"), CREATED("Newest"),
}

data class CrmFilter(
    val query: String = "",
    val stage: String = "",
    val owner: String = "",
    val tag: String = "",
    val sort: CrmSort = CrmSort.STAGE,
) {
    val isActive: Boolean get() = stage.isNotEmpty() || owner.isNotEmpty() || tag.isNotEmpty()

    fun apply(contacts: List<CrmContact>, flow: CrmFlow?): List<CrmContact> {
        val rows = contacts.filter { contact ->
            contact.matches(query) && (stage.isEmpty() || contact.stage == stage) &&
                (owner.isEmpty() || contact.owner.equals(owner, ignoreCase = true)) && (tag.isEmpty() || tag in contact.tags)
        }
        return when (sort) {
            CrmSort.STAGE -> {
                val order = flow?.stages.orEmpty().withIndex().associate { it.value to it.index }
                rows.sortedWith(compareBy<CrmContact> { order[it.stage] ?: Int.MAX_VALUE }.thenByDescending { it.updatedAt })
            }
            CrmSort.NAME -> rows.sortedBy { it.displayName.lowercase() }
            CrmSort.VALUE -> rows.sortedWith(compareByDescending<CrmContact> { it.value }.thenByDescending { it.updatedAt })
            CrmSort.UPDATED -> rows.sortedByDescending { it.updatedAt }
            CrmSort.LAST_CONTACTED -> rows.sortedWith(compareBy<CrmContact> { it.lastContactedAt }.thenByDescending { it.updatedAt })
            CrmSort.CREATED -> rows.sortedByDescending { it.createdAt }
        }
    }
}

data class CrmStageStat(val stage: String, val count: Int, val value: Long)

fun CrmFlow.stats(contacts: List<CrmContact>): List<CrmStageStat> {
    val stats = stages.map { stage ->
        val rows = contacts.filter { it.stage == stage }
        CrmStageStat(stage, rows.size, rows.sumOf { it.value })
    }.toMutableList()
    val stray = contacts.filter { it.stage !in stages }
    if (stray.isNotEmpty()) stats.add(CrmStageStat("other", stray.size, stray.sumOf { it.value }))
    return stats
}

fun CrmFlow.next(after: String): String? = stages.indexOf(after).takeIf { it >= 0 && it + 1 < stages.size }?.let { stages[it + 1] }

fun CrmFlow.progress(stage: String): Double {
    val index = stages.indexOf(stage)
    return if (index < 0 || stages.size < 2) 0.0 else index.toDouble() / (stages.size - 1)
}

val CrmFlow.json: Map<String, Any?>
    get() = linkedMapOf("id" to id, "name" to name, "icon" to icon, "stages" to stages)

data class CrmFlowDraft(val id: String = "", val name: String = "", val icon: String = "pipeline", val stages: List<String> = listOf("Lead", "Qualified", "Won"), val isNew: Boolean = true) {
    val flow: CrmFlow?
        get() {
            val slug = if (isNew) CrmText.slug(name) else id
            if (slug.isEmpty()) return null
            val slugs = stages.map(CrmText::slug).filter { it.isNotEmpty() }.distinct()
            val title = name.trim()
            return CrmFlow(slug, title.ifEmpty { CrmStageLabel.title(slug) }, icon, slugs.ifEmpty { listOf("lead") })
        }

    companion object {
        fun of(flow: CrmFlow) = CrmFlowDraft(flow.id, flow.name, flow.icon, flow.stages.map(CrmStageLabel::title), isNew = false)

        fun settingsBody(flows: List<CrmFlow>): Map<String, Any?> = mapOf("flows" to flows.take(12).map { it.json })
    }
}

data class CrmMailMessage(
    val id: String,
    val box: String = "Inbox",
    val subject: String = "",
    val summary: String = "",
    val sender: String = "",
    val from: String = "",
    val to: String = "",
    val hasAttachment: Boolean = false,
    val providerUrl: String = "",
    val receivedAt: Long = 0,
) {
    val who: String get() = sender.ifEmpty { from }
    val address: String get() = from.substringAfterLast('<', from).substringBefore('>')

    fun matches(query: String): Boolean = PackagesText.matches(query, subject, summary, sender, from, to)

    companion object {
        fun fromJson(json: JsonMap) = CrmMailMessage(
            json.str("id"), json.str("box", "Inbox"), json.str("subject"), json.str("summary"), json.str("sender"), json.str("from"),
            json.str("to"), json.bool("hasAttachment"), json.str("providerUrl"), json.long("receivedAt"),
        )
    }
}

data class CrmZohoStatus(val connected: Boolean, val region: String, val lastSyncAt: Long, val oneClick: Boolean)

data class CrmSettings(
    val providerName: String,
    val providerUrlTemplate: String,
    val fromName: String,
    val fromEmail: String,
    val mailProvider: String,
    val mailBoxes: List<String>,
    val zoho: CrmZohoStatus,
    val mailtrapConfigured: Boolean,
    val updatedBy: String,
    val updatedAt: Long,
    val removedColumns: List<String> = emptyList(),
) {
    fun composeUrl(to: String, subject: String = "", body: String = ""): String =
        if (mailProvider == "gmail") {
            "https://mail.google.com/mail/?view=cm&fs=1&to=${CrmText.encode(to)}&su=${CrmText.encode(subject)}&body=${CrmText.encode(body)}"
        } else {
            "https://mail.zoho.com/zm/#compose?to=${CrmText.encode(to)}&subject=${CrmText.encode(subject)}&body=${CrmText.encode(body)}"
        }

    companion object {
        val DEFAULT_BOXES = listOf("Inbox", "Follow-up", "Important", "Archive")

        fun fromJson(json: JsonMap): CrmSettings {
            val boxes = json.list("mailBoxes").map { if (it is Map<*, *>) it.asMap().str("name") else jsonText(it) }.filter { it.isNotEmpty() }
            val zoho = json["zoho"].asMap()
            return CrmSettings(
                json.str("providerName"), json.str("providerUrlTemplate"), json.str("fromName"), json.str("fromEmail"),
                json.str("mailProvider", "zoho"), boxes.ifEmpty { DEFAULT_BOXES },
                CrmZohoStatus(zoho.bool("connected"), zoho.str("region", "com"), zoho.long("lastSyncAt"), zoho.bool("oneClick")),
                json.bool("mailtrapConfigured"), json.str("updatedBy"), json.long("updatedAt"), json.strings("removedColumns"),
            )
        }
    }
}

data class CrmSettingsEdit(
    val fromName: String,
    val fromEmail: String,
    val mailProvider: String,
    val providerName: String,
    val providerUrlTemplate: String,
    val mailBoxes: String,
) {
    val body: Map<String, Any?>
        get() {
            val seen = HashSet<String>()
            val boxes = mailBoxes.split(',').map { it.trim() }.filter { it.isNotEmpty() && seen.add(it.lowercase()) }
            return linkedMapOf(
                "fromName" to fromName.trim(), "fromEmail" to fromEmail.trim(), "mailProvider" to mailProvider,
                "providerName" to providerName.trim(), "providerUrlTemplate" to providerUrlTemplate.trim(),
                "mailBoxes" to boxes.ifEmpty { CrmSettings.DEFAULT_BOXES },
            )
        }

    companion object {
        fun of(settings: CrmSettings) = CrmSettingsEdit(
            settings.fromName, settings.fromEmail, settings.mailProvider, settings.providerName, settings.providerUrlTemplate,
            settings.mailBoxes.joinToString(", "),
        )
    }
}

data class CrmOutreachTemplate(val key: String, val label: String, val subject: String, val body: String) {
    fun filled(contact: CrmContact?): Pair<String, String> {
        if (contact == null) return subject to body
        val first = contact.name.split(' ').firstOrNull().orEmpty()
        fun fill(text: String) = text.replace("{name}", contact.name).replace("{firstName}", first).replace("{company}", contact.company)
        return fill(subject) to fill(body)
    }
}

data class CrmOutreachSend(val ts: Long, val sender: String, val ok: Boolean, val to: String, val subject: String, val error: String)

data class CrmOutreachMember(val name: String, val addedBy: String, val addedAt: Long)

data class CrmOutreach(
    val allowed: Boolean,
    val canManage: Boolean,
    val configured: Boolean,
    val fromEmail: String,
    val fromName: String,
    val dailyLimit: Int,
    val sentToday: Int,
    val templates: List<CrmOutreachTemplate>,
    val recent: List<CrmOutreachSend>,
    val members: List<CrmOutreachMember>,
) {
    val remaining: Int get() = maxOf(0, dailyLimit - sentToday)

    companion object {
        fun memberOf(json: JsonMap) = CrmOutreachMember(json.str("name"), json.str("addedBy"), json.long("addedAt"))

        fun fromJson(json: JsonMap) = CrmOutreach(
            json.bool("allowed"), json.bool("canManage"), json.bool("configured"), json.str("fromEmail"), json.str("fromName"),
            json.int("dailyLimit", 50), json.int("sentToday"),
            json.maps("templates").map { CrmOutreachTemplate(it.str("key"), it.str("label", it.str("key")), it.str("subject"), it.str("body")) }
                .filter { it.key.isNotEmpty() },
            json.maps("recent").map { CrmOutreachSend(it.long("ts"), it.str("sender"), it.bool("ok"), it.str("to"), it.str("subject"), it.str("error")) },
            json.maps("members").map(::memberOf).filter { it.name.isNotEmpty() },
        )
    }
}

data class CrmDeckLink(
    val token: String,
    val url: String = "",
    val contactId: String = "",
    val createdAt: Long = 0,
    val createdBy: String = "",
    val active: Boolean = true,
    val views: Int = 0,
    val seconds: Int = 0,
    val lastViewedAt: Long = 0,
    val pagesViewed: Int = 0,
    val pageCount: Int = 0,
) {
    val pageShare: Double get() = if (pageCount > 0) minOf(1.0, pagesViewed.toDouble() / pageCount) else 0.0

    companion object {
        fun fromJson(json: JsonMap) = CrmDeckLink(
            json.str("token"), json.str("url"), json.str("contactId"), json.long("createdAt"), json.str("createdBy"),
            json.optBool("active") ?: (json.long("revokedAt") == 0L), json.int("views"), json.int("seconds"), json.long("lastViewedAt"),
            json.int("pagesViewed"), json.int("pageCount"),
        )
    }
}

data class CrmDeckPage(val page: Int, val title: String, val seconds: Int, val viewers: Int)

data class CrmDeckSession(val startedAt: Long, val seconds: Int, val pages: Int, val country: String, val agent: String)

data class CrmDeckDetail(val link: CrmDeckLink, val pages: List<CrmDeckPage>, val sessions: List<CrmDeckSession>) {
    val longestPage: Int get() = pages.maxOfOrNull { it.seconds } ?: 0

    companion object {
        fun fromJson(json: JsonMap) = CrmDeckDetail(
            CrmDeckLink.fromJson(json),
            json.maps("pages").mapIndexed { index, page -> CrmDeckPage(page.int("page", index + 1), page.str("title"), page.int("seconds"), page.int("viewers")) },
            json.maps("sessions").map { CrmDeckSession(it.long("startedAt"), it.int("seconds"), it.int("pages"), it.str("country"), it.str("agent")) },
        )
    }
}

data class CrmDayCounts(val emails: Int, val outreach: Int, val contacts: Int) {
    val total: Int get() = emails + outreach + contacts

    operator fun plus(other: CrmDayCounts) = CrmDayCounts(emails + other.emails, outreach + other.outreach, contacts + other.contacts)

    companion object {
        val ZERO = CrmDayCounts(0, 0, 0)
    }
}

data class CrmTeamMember(val name: String, val emails: List<Int>, val outreach: List<Int>, val contacts: List<Int>) {
    val total: Int get() = emails.sum() + outreach.sum() + contacts.sum()

    fun day(index: Int) = CrmDayCounts(emails.getOrElse(index) { 0 }, outreach.getOrElse(index) { 0 }, contacts.getOrElse(index) { 0 })

    fun sum(range: IntRange): CrmDayCounts = range.fold(CrmDayCounts.ZERO) { total, index -> total + day(index) }
}

data class CrmTeamActivity(val days: List<String>, val members: List<CrmTeamMember>) {
    fun totals(index: Int): CrmDayCounts = members.fold(CrmDayCounts.ZERO) { total, member -> total + member.day(index) }

    val busiestDay: Int get() = days.indices.maxOfOrNull { totals(it).total } ?: 0

    companion object {
        private fun ints(value: List<Any?>) = value.map { jsonLong(it).toInt() }

        fun fromJson(json: JsonMap) = CrmTeamActivity(
            json.strings("days"),
            json.maps("members").map { CrmTeamMember(it.str("name"), ints(it.list("emails")), ints(it.list("outreach")), ints(it.list("contacts"))) }
                .filter { it.name.isNotEmpty() },
        )
    }
}

data class CrmListActivityEntry(val at: Long, val actor: String, val action: String, val text: String, val count: Int)

data class CrmImportColumn(val index: Int, val header: String, val field: String, val samples: List<String>)

data class CrmImportPreview(val delimiter: String, val columns: List<CrmImportColumn>) {
    companion object {
        fun fromJson(json: JsonMap) = CrmImportPreview(
            json.str("delimiter"),
            json.maps("columns").map { CrmImportColumn(it.int("index"), it.str("header"), it.str("field"), it.strings("samples")) },
        )
    }
}

data class CrmImportResult(val created: Int = 0, val updated: Int = 0, val skipped: Int = 0, val errors: List<String> = emptyList(), val viewId: String = "") {
    val sentence: String
        get() {
            val parts = mutableListOf<String>()
            if (created > 0) parts.add("$created added")
            if (updated > 0) parts.add("$updated updated")
            if (skipped > 0) parts.add("$skipped skipped")
            return if (parts.isEmpty()) "Nothing to import." else parts.joinToString(", ") + "."
        }

    companion object {
        fun fromJson(json: JsonMap) = CrmImportResult(
            json.int("created"), json.int("updated"), json.int("skipped"),
            json.maps("errors").map { it.str("message", "Row ${it.int("row")}: ${it.str("error")}") }, json.str("viewId"),
        )
    }
}

data class CrmEmailDraft(
    val to: String = "",
    val subject: String = "",
    val body: String = "",
    val template: String = "blank",
    val contactId: String = "",
    val noteDraft: Boolean = false,
) {
    val isValid: Boolean get() = '@' in to && subject.isNotBlank() && body.isNotBlank()

    val outreachBody: Map<String, Any?>
        get() = linkedMapOf("to" to to.trim(), "subject" to subject.trim(), "body" to body, "template" to template)

    val sendBody: Map<String, Any?>
        get() = linkedMapOf<String, Any?>("to" to to.trim(), "subject" to subject.trim(), "text" to body).also {
            if (PackagesText.isHexId(contactId)) it["contactId"] = contactId
        }

    companion object {
        fun of(contact: CrmContact?) = CrmEmailDraft(to = contact?.email.orEmpty(), contactId = contact?.id.orEmpty())
    }
}

sealed class CrmBulkAction {
    data class Stage(val stage: String) : CrmBulkAction()
    data class Flow(val flow: String) : CrmBulkAction()
    data class AddList(val viewId: String) : CrmBulkAction()
    data class RemoveList(val viewId: String) : CrmBulkAction()
    data class Owner(val owner: String) : CrmBulkAction()
    data class Contacted(val at: Long) : CrmBulkAction()

    fun edit(contact: CrmContact, flows: List<CrmFlow>): Map<String, Any?> {
        val body = linkedMapOf<String, Any?>("id" to contact.id)
        when (this) {
            is Stage -> body["stage"] = stage
            is Flow -> {
                val target = flows.firstOrNull { it.id == flow }
                body["flow"] = flow
                body["stage"] = target?.let { if (contact.stage in it.stages) contact.stage else it.stages.firstOrNull() ?: "lead" } ?: contact.stage
            }
            is AddList -> body["views"] = listOf(viewId)
            is RemoveList -> body["removeViews"] = listOf(viewId)
            is Owner -> body["owner"] = owner
            is Contacted -> body["lastContactedAt"] = at
        }
        return body
    }
}

object CrmPrompt {
    fun outreach(contact: CrmContact, sheet: String, flowName: String): String {
        val group = if (sheet.isEmpty() || sheet == "No pipeline") flowName else sheet
        val audience = "$group $flowName".lowercase()
        fun mentions(vararg words: String) = words.any { it in audience }
        val about = mutableListOf<String>()
        if (contact.mastodon.isNotEmpty()) {
            val profile = CrmText.profileUrl("mastodon", contact.mastodon).orEmpty()
            about.add("Mastodon: ${contact.mastodon}" + if (profile.isEmpty() || profile == contact.mastodon) "" else " ($profile)")
        } else {
            about.add("no Mastodon handle saved, so look it up")
        }
        if (contact.title.isNotEmpty() && contact.company.isNotEmpty()) about.add("${contact.title} at ${contact.company}")
        else if (contact.title.isNotEmpty() || contact.company.isNotEmpty()) about.add(contact.title + contact.company)
        listOf(contact.website, contact.location).filter { it.isNotEmpty() }.forEach { about.add(it) }
        val quoted = "“$group”"
        val angle = when {
            mentions("journalist", "press", "media", "reporter", "writer", "editor", "podcast", "newsletter", "blog") ->
                "They are on my $quoted sheet as a journalist, so pitch a story: why decentralized code hosting matters now and what is new about ForkMesh, and offer an interview or a demo. No sales talk."
            mentions("investor", "angel", "vc", "fund", "capital") ->
                "They are on my $quoted sheet as an investor, so lead with the opportunity ForkMesh opens and ask for a short call; state only facts you were given or looked up, and never invent traction, numbers or names."
            mentions("partner", "integration", "alliance") ->
                "They are on my $quoted sheet as a potential partner, so show how ForkMesh fits what they build and suggest exploring working together."
            mentions("customer", "lead", "user", "developer", "maintainer", "community", "open source") ->
                "They are on my $quoted sheet as a potential user, so give one concrete way ForkMesh helps their projects and invite them to try it."
            group.isNotEmpty() -> "They are on my $quoted sheet, so pitch it the way that suits that group."
            else -> ""
        }
        return listOf(
            "Tell me about ForkMesh (https://forkmesh.com).",
            "Then tell me about ${contact.displayName} (${about.joinToString("; ")}).",
            "Then write a short toot (under 500 characters) I can send them on Mastodon about ForkMesh." +
                (if (angle.isEmpty()) "" else " $angle") +
                " Mention them by their handle, make it specific to their work, and use at most two hashtags.",
        ).joinToString("\n\n")
    }

    fun contactLink(organization: String, contactId: String) = "https://app.forkmesh.com/dashboard/crm?org=$organization&contact=$contactId"

    fun listLink(organization: String, viewId: String) = "https://app.forkmesh.com/dashboard/crm?org=$organization&list=$viewId"
}

object CrmCsv {
    data class Split(val header: String, val parts: List<String>, val rows: Int)

    fun split(csv: String, rowsPerPart: Int = 100): Split {
        val records = mutableListOf<String>()
        val current = StringBuilder()
        var quoted = false
        for (character in csv) {
            if (character == '"') quoted = !quoted
            if (character == '\n' && !quoted) {
                if (current.isNotBlank()) records.add(current.toString())
                current.clear()
            } else if (character != '\r' || quoted) {
                current.append(character)
            }
        }
        if (current.isNotBlank()) records.add(current.toString())
        val header = records.firstOrNull() ?: return Split("", emptyList(), 0)
        val rows = records.drop(1)
        val parts = rows.chunked(maxOf(1, rowsPerPart)).map { (listOf(header) + it).joinToString("\n") }
        return Split(header, parts.ifEmpty { listOf(header) }, rows.size)
    }

    fun preview(csv: String, rows: Int = 20): String = split(csv, rows).parts.firstOrNull().orEmpty()
}
