package com.forkmesh.nativeapp.genie

import com.forkmesh.nativeapp.api.PrefStore
import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.market.CrmContact
import com.forkmesh.nativeapp.market.CrmFlow
import com.forkmesh.nativeapp.market.CrmOrganizationData
import java.net.URI
import java.net.URLEncoder
import java.security.MessageDigest
import java.util.concurrent.Callable
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit

enum class GenieOutreachChannel { Email, Mastodon }

data class GenieMessage(val role: String, val content: String) {
    companion object {
        fun system(text: String) = GenieMessage("system", text)
        fun user(text: String) = GenieMessage("user", text)
        fun assistant(text: String) = GenieMessage("assistant", text)
    }
}

data class GenieMastodonHandle(val user: String, val host: String) {
    val mention: String get() = "@$user@$host"

    val lookupUrl: String get() = "https://$host/api/v1/accounts/lookup?acct=" + URLEncoder.encode(user, "UTF-8")

    companion object {
        fun of(raw: String): GenieMastodonHandle? {
            var text = raw.trim()
            val scheme = text.substringBefore("://", "").lowercase()
            if (scheme == "https" || scheme == "http") {
                val uri = runCatching { URI(text) }.getOrNull() ?: return null
                val host = uri.host.orEmpty()
                if (host.isEmpty()) return null
                val path = uri.path.orEmpty().split('/').firstOrNull { it.isNotEmpty() }.orEmpty()
                if (!path.startsWith("@") || path.length < 2) return null
                text = "$path@$host"
            }
            if (text.startsWith("@")) text = text.drop(1)
            val parts = text.split("@")
            if (parts.size != 2 || parts[0].isEmpty() || !parts[1].contains('.')) return null
            if (text.any { it.isWhitespace() || it == '/' }) return null
            return GenieMastodonHandle(parts[0], parts[1].lowercase())
        }
    }
}

data class GenieOutreachPick(
    val organization: String,
    val contact: CrmContact,
    val flowName: String,
    val channel: GenieOutreachChannel,
    val sender: String,
    val investor: Boolean = false,
    val zohoRegion: String = "com",
    val cc: List<String> = emptyList(),
) {
    val needsDeck: Boolean get() = investor && channel == GenieOutreachChannel.Email

    val firstName: String
        get() {
            val name = contact.name.trim()
            return name.split(' ').firstOrNull { it.isNotEmpty() } ?: name
        }

    val handle: GenieMastodonHandle? get() = GenieMastodonHandle.of(contact.mastodon)

    val images: List<String> get() = GenieOutreach.images(contact)

    val caption: String
        get() = if (channel == GenieOutreachChannel.Email) "Email ${contact.displayName}?" else "Toot to ${contact.displayName}?"
}

data class GenieOutreachDraft(
    val channel: GenieOutreachChannel,
    val to: String,
    val subject: String = "",
    val body: String,
    val byModel: Boolean,
    val writer: String = "",
    val deck: String = "",
    val zohoRegion: String = "com",
    val cc: List<String> = emptyList(),
) {
    val mailtoUrl: String? get() = if (channel == GenieOutreachChannel.Email) GenieMail.mailto(to, subject, body, cc) else null

    val zohoUrl: String? get() = if (channel == GenieOutreachChannel.Email) GenieMail.zohoCompose(zohoRegion, to, subject, body, cc) else null

    val shareText: String
        get() {
            if (channel != GenieOutreachChannel.Email) return body
            val copies = if (cc.isEmpty()) "" else "Cc: ${cc.joinToString(", ")}\n"
            return "${copies}Subject: $subject\n\n$body"
        }
}

class GenieOutreachWriter(val name: String, val timeoutSeconds: Double, val run: (List<GenieMessage>) -> String)

object GenieMail {
    val zohoHosts: Map<String, String> = mapOf(
        "com" to "https://mail.zoho.com", "eu" to "https://mail.zoho.eu", "in" to "https://mail.zoho.in",
        "com.au" to "https://mail.zoho.com.au", "jp" to "https://mail.zoho.jp", "ca" to "https://mail.zohocloud.ca",
    )

    private fun query(text: String): String = URLEncoder.encode(text, "UTF-8").replace("+", "%20")

    fun mailto(to: String, subject: String = "", body: String = "", cc: List<String> = emptyList()): String {
        val copies = cc.filter { !it.equals(to, ignoreCase = true) }.map(::query)
        val carbon = if (copies.isEmpty()) "" else "cc=${copies.joinToString(",")}&"
        return "mailto:${query(to).replace("%40", "@")}?${carbon}subject=${query(subject)}&body=${query(body)}"
    }

    fun zohoCompose(region: String, to: String, subject: String = "", body: String = "", cc: List<String> = emptyList()): String {
        val ct = URLEncoder.encode(mailto(to, subject, body, cc), "UTF-8").replace("+", "%20").replace("*", "%2A").replace("%7E", "~")
        val host = zohoHosts[region] ?: zohoHosts.getValue("com")
        return "$host/zm/comp.do?ct=$ct"
    }
}

object GenieOutreach {
    val closedStages: Set<String> = setOf(
        "passed", "rejected", "closed", "inactive", "lost", "closed-lost", "declined", "churned", "not-interested", "disqualified",
    )
    const val TOOT_LIMIT = 500
    const val EMAIL_WORDS = 140
    const val INVESTOR_WORDS = 180
    const val TIMEOUT = 25.0
    const val CLOUD_TIMEOUT = 90.0
    const val DESKTOP_TIMEOUT = 300.0
    const val TEMPERATURE = 0.4
    val investorFlowIds: Set<String> = setOf("investors", "investment")
    const val PITCH =
        "ForkMesh (https://forkmesh.com) lets developers collaborate on, mirror and preserve source-code repositories across independent hosts, without relying on any single hosting provider. Git, issues, pull requests, chat and coding agents come together in one app on desktop, web, iPhone and Android."
    val investorFacts: List<String> = listOf(
        "ForkMesh (https://forkmesh.com) turns Claude Code and Codex into attributable contributors on a local-first Git platform teams control. Tagline: Code with agents. Own the forge.",
        "Problem: coding is moving from autocomplete to delegated agent work, but tasks, transcripts, branches and reviews are scattered, accountability for agent changes is shallow, and code, identity and collaboration stay rented from a central platform.",
        "Product: an issue or prompt starts a Claude Code or Codex session in its own worktree and branch; sessions run in parallel and can be streamed, steered, stopped and resumed; the patch and session metadata (model, cost, tool use) become a signed pull request that a human reviews and merges. Desktop app and web.",
        "Proof (Git history and live network, 13 September 2026): ForkMesh builds ForkMesh. 7,416 agent branches merged into main, 51,457 commits in 90 days, 24 human and agent contributors, 63 community pull requests, 54 repositories on the live network.",
        "Open source and peer-to-peer: repositories mirror across independent nodes; the protocol is MIT-licensed; any community incentives are optional, non-custodial and never guaranteed.",
        "Customers: 20 to 200 person software teams using several coding agents on private code; buyer is the CTO, VP Engineering or security lead. Land one agent workflow, then expand.",
        "Business model: Community (free, open source), Agent team (subscription plus usage), Enterprise (annual contract: policy, RBAC, audit, self-hosted and federated). \$25K blended ACV hypothesis.",
        "Why now: 180M+ developers on GitHub, 4.7M paid Copilot subscribers growing 75% a year; teams use several agents and need provenance, policy and trusted merges.",
        "Competition: hosted AI forges (Copilot, GitLab Duo) keep the code and control plane; coding agents (Claude, Codex, Cursor) are not a shared system of record; Radicle is peer-to-peer Git without agent orchestration. ForkMesh owns the intersection.",
        "Team: Jett Vale, CEO and founder, built and exited a prior company; Md. Kaif, CTO and co-founder, distributed systems and Git.",
        "The round: raising \$1.0M for an 18-month plan to reach 20 paid organizations, \$500K ARR and Series A readiness by Q4 2027. Funds: 55% product and engineering, 20% infrastructure and security, 15% go-to-market, 10% legal, ops and contingency.",
        "Contact: founders@forkmesh.com.",
    )

    fun stageKey(stage: String): String = stage.trim().lowercase().replace(" ", "-").replace("_", "-")

    fun closed(contact: CrmContact, flows: List<CrmFlow>): Boolean {
        val stage = stageKey(contact.stage)
        if (stage in closedStages) return true
        val flow = flows.firstOrNull { it.id == contact.flow } ?: return false
        if (flow.stages.size <= 1) return false
        return stageKey(flow.stages.last()) == stage
    }

    fun emailAddress(contact: CrmContact): String? {
        val email = contact.email.trim()
        val parts = email.split("@")
        if (parts.size != 2 || parts[0].isEmpty() || !parts[1].contains('.') || email.any { it.isWhitespace() }) return null
        return email
    }

    fun channel(contact: CrmContact): GenieOutreachChannel? = when {
        emailAddress(contact) != null -> GenieOutreachChannel.Email
        GenieMastodonHandle.of(contact.mastodon) != null -> GenieOutreachChannel.Mastodon
        else -> null
    }

    fun reached(contact: CrmContact, data: CrmOrganizationData): Boolean {
        if (contact.lastContactedAt > 0) return true
        return data.activity.any { it.contactId == contact.id && !it.incoming && it.status != "draft" }
    }

    fun investor(contact: CrmContact, flows: List<CrmFlow>): Boolean {
        if (contact.flow in investorFlowIds) return true
        val flow = flows.firstOrNull { it.id == contact.flow } ?: return false
        if (flow.icon == "investors") return true
        val name = flow.name.lowercase()
        return listOf("investor", "angel", "vc", "fund", "capital").any { name.contains(it) }
    }

    fun candidates(data: GenieCrmOrganization, skip: Set<String> = emptySet()): List<CrmContact> =
        data.data.contacts.filter { contact ->
            contact.id !in skip && channel(contact) != null && !reached(contact, data.data) && !closed(contact, data.data.flows)
        }

    fun pick(
        organizations: List<GenieCrmOrganization>,
        skip: Set<String> = emptySet(),
        sender: String = "",
        random: () -> Double = { Math.random() },
    ): GenieOutreachPick? {
        val pool = organizations.flatMap { data -> candidates(data, skip).map { data to it } }
        if (pool.isEmpty()) return null
        val index = (random() * pool.size).toInt().coerceIn(0, pool.size - 1)
        val (data, contact) = pool[index]
        return offer(contact, data, sender)
    }

    fun best(
        organizations: List<GenieCrmOrganization>,
        skip: Set<String> = emptySet(),
        sender: String = "",
        only: GenieOutreachChannel? = null,
    ): GenieOutreachPick? {
        val pool = organizations.flatMap { data ->
            candidates(data, skip).filter { only == null || channel(it) == only }.map { data to it }
        }
        val chosen = pool.sortedWith { a, b -> if (ranksAhead(a.second, b.second)) -1 else if (ranksAhead(b.second, a.second)) 1 else 0 }
            .firstOrNull() ?: return null
        return offer(chosen.second, chosen.first, sender)
    }

    fun ranksAhead(a: CrmContact, b: CrmContact): Boolean {
        if (a.value != b.value) return a.value > b.value
        val aNext = a.nextStep.trim().isNotEmpty()
        val bNext = b.nextStep.trim().isNotEmpty()
        if (aNext != bNext) return aNext
        val aEmail = channel(a) == GenieOutreachChannel.Email
        val bEmail = channel(b) == GenieOutreachChannel.Email
        if (aEmail != bEmail) return aEmail
        val aAdded = if (a.createdAt > 0) a.createdAt else Long.MAX_VALUE
        val bAdded = if (b.createdAt > 0) b.createdAt else Long.MAX_VALUE
        if (aAdded != bAdded) return aAdded < bAdded
        return a.id < b.id
    }

    fun offer(contact: CrmContact, organization: GenieCrmOrganization, sender: String): GenieOutreachPick? {
        val data = organization.data
        val channel = channel(contact) ?: return null
        val from = data.settings.fromName.trim()
        return GenieOutreachPick(
            organization = data.organization,
            contact = contact,
            flowName = data.flow(contact.flow).name,
            channel = channel,
            sender = from.ifEmpty { sender },
            investor = investor(contact, data.flows),
            zohoRegion = data.settings.zoho.region,
            cc = if (channel == GenieOutreachChannel.Email) organization.memberEmails.filter { !it.equals(contact.email, ignoreCase = true) } else emptyList(),
        )
    }

    fun images(contact: CrmContact): List<String> {
        val urls = mutableListOf<String>()
        for (key in listOf("photo", "avatar", "image", "picture", "photoUrl", "avatarUrl")) {
            val match = contact.fields.entries.firstOrNull { it.key.equals(key, ignoreCase = true) }?.value.orEmpty().trim()
            if (match.lowercase().startsWith("https://")) urls.add(match)
        }
        val github = githubUser(contact.github)
        if (github.isNotEmpty()) urls.add("https://github.com/$github.png?size=160")
        emailAddress(contact)?.let { email ->
            val hash = MessageDigest.getInstance("SHA-256").digest(email.lowercase().toByteArray()).joinToString("") { "%02x".format(it) }
            urls.add("https://gravatar.com/avatar/$hash?s=160&d=404")
        }
        return urls
    }

    fun githubUser(raw: String): String {
        var text = raw.trim()
        val at = text.lowercase().indexOf("github.com/")
        if (at >= 0) text = text.substring(at + "github.com/".length)
        if (text.startsWith("@")) text = text.drop(1)
        val user = text.split('/').firstOrNull { it.isNotEmpty() }.orEmpty()
        if (user.isEmpty() || !user.all { it.isLetterOrDigit() || it == '-' }) return ""
        return user
    }

    fun angle(sheet: String, flowName: String): String {
        val group = if (sheet.isEmpty() || sheet == "No pipeline") flowName else sheet
        val audience = "$group $flowName".lowercase()
        fun mentions(vararg words: String) = words.any { audience.contains(it) }
        val quoted = "“$group”"
        return when {
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
    }

    fun describe(pick: GenieOutreachPick): String {
        val contact = pick.contact
        val lines = mutableListOf("Name: ${contact.displayName}")
        if (contact.title.isNotEmpty()) lines.add("Role: ${contact.title}")
        if (contact.company.isNotEmpty()) lines.add("Company: ${contact.company}")
        for ((label, value) in listOf(
            "Website" to contact.website, "Location" to contact.location, "GitHub" to contact.github,
            "Mastodon" to contact.mastodon, "Next step" to contact.nextStep,
        )) {
            if (value.isNotEmpty()) lines.add("$label: $value")
        }
        if (contact.tags.isNotEmpty()) lines.add("Tags: " + contact.tags.joinToString(", "))
        val notes = contact.notes.trim()
        if (notes.isNotEmpty()) lines.add("Notes: " + if (notes.length > 400) notes.take(399) + "…" else notes)
        if (pick.flowName.isNotEmpty()) lines.add("Pipeline: ${pick.flowName}, stage ${contact.stage}")
        return lines.joinToString("\n")
    }

    fun messages(pick: GenieOutreachPick, deck: String = ""): List<GenieMessage> {
        val angle = angle("", pick.flowName)
        val signer = pick.sender.ifEmpty { "the ForkMesh team" }
        val pitching = pick.investor && pick.channel == GenieOutreachChannel.Email
        var system = "You write first messages from the ForkMesh team to people who have never heard from us. $PITCH"
        if (pitching) system += "\n\nAbout ForkMesh, from its investor deck:\n" + investorFacts.joinToString("\n") { "- $it" } + "\n\n"
        system += " Write as $signer. Be warm, plain and specific to who they are and their work."
        system += " Work only from the facts given here: never invent traction, numbers, dates, names, quotes, links or shared history, and do not claim to know their work beyond what their record says. Leave out anything you are not given."
        system += " No placeholders in square brackets and no markdown."
        var user = "Who they are:\n" + describe(pick) + "\n\n"
        when (pick.channel) {
            GenieOutreachChannel.Email -> {
                if (pitching) {
                    user += "Write a short first email (at most $INVESTOR_WORDS words) to this investor about ForkMesh."
                    if (angle.isNotEmpty()) user += " $angle"
                    user += " Make one clear ask: a 20-minute call."
                } else {
                    user += "Write a short first email (at most $EMAIL_WORDS words) about how ForkMesh fits them."
                    if (angle.isNotEmpty()) user += " $angle"
                }
                if (deck.isNotEmpty()) {
                    user += "\n\nTheir personal deck link: $deck\nIt is unique to them and every open and page read is recorded against their record, so put it in the email exactly as written, on its own line, once. Do not shorten, rewrite or add anything to it.\n\n"
                }
                user += " Reply with exactly: a first line starting \"Subject:\", a blank line, then the body, signed by $signer."
            }
            GenieOutreachChannel.Mastodon -> {
                val mention = pick.handle?.mention ?: pick.contact.mastodon
                user += "Write one Mastodon post (under 450 characters) to them about ForkMesh."
                if (angle.isNotEmpty()) user += " $angle"
                user += " Start with their handle $mention, use at most two hashtags, and reply with only the post."
            }
        }
        return listOf(GenieMessage.system(system), GenieMessage.user(user))
    }

    fun parse(text: String, pick: GenieOutreachPick, deck: String = "", writer: String = ""): GenieOutreachDraft? {
        var body = text.replace("**", "").trim()
        if (body.startsWith("```")) {
            body = body.split("\n").drop(1).filter { !it.startsWith("```") }.joinToString("\n").trim()
        }
        val placeholder = body.contains("[") && body.contains("]")
        if (body.isEmpty() || placeholder) return null
        return when (pick.channel) {
            GenieOutreachChannel.Email -> {
                val to = emailAddress(pick.contact) ?: return null
                val lines = body.split("\n")
                val index = lines.indexOfFirst { it.trim().lowercase().startsWith("subject:") }
                if (index < 0) return null
                val line = lines[index]
                val subject = line.substring(line.indexOf(':') + 1).trim()
                val rest = lines.drop(index + 1).joinToString("\n").trim()
                if (subject.isEmpty() || rest.length < 40) return null
                GenieOutreachDraft(
                    GenieOutreachChannel.Email, to, subject, withDeck(rest, deck), byModel = true, writer = writer, deck = deck,
                    zohoRegion = pick.zohoRegion, cc = pick.cc,
                )
            }
            GenieOutreachChannel.Mastodon -> {
                val handle = pick.handle ?: return null
                var post = body.trim { it.isWhitespace() || it == '"' || it == '“' || it == '”' }
                if (!post.lowercase().contains("@" + handle.user.lowercase())) post = handle.mention + " " + post
                if (post.length < 40) return null
                GenieOutreachDraft(GenieOutreachChannel.Mastodon, handle.mention, body = clipToot(post), byModel = true, writer = writer)
            }
        }
    }

    fun withDeck(body: String, deck: String): String {
        if (deck.isEmpty() || body.contains(deck)) return body
        val paragraphs = body.split("\n\n").toMutableList()
        val line = "Our deck, made for you: $deck"
        if (paragraphs.size > 1) paragraphs.add(paragraphs.size - 1, line) else paragraphs.add(line)
        return paragraphs.joinToString("\n\n")
    }

    private val unsupportedPatterns = listOf(Regex("https?://[^\\s<>\"')\\]]+"), Regex("[0-9]+(?:[.,][0-9]+)*"))

    fun unsupported(text: String, sources: String): List<String> {
        val found = mutableListOf<String>()
        for (pattern in unsupportedPatterns) {
            for (match in pattern.findAll(text)) {
                val token = match.value.trim('.', ',', ';', ':', '!', '?')
                if (token.isNotEmpty() && !sources.contains(token) && token !in found) found.add(token)
            }
        }
        return found
    }

    fun clipToot(text: String): String {
        if (text.length <= TOOT_LIMIT) return text
        val cut = text.take(TOOT_LIMIT - 1)
        val words = cut.split(" ").dropLast(1).joinToString(" ")
        return words.ifEmpty { cut } + "…"
    }

    fun fallback(pick: GenieOutreachPick, deck: String = ""): GenieOutreachDraft {
        val contact = pick.contact
        val hello = if (pick.firstName.isEmpty()) "Hi there" else "Hi ${pick.firstName}"
        val about = when {
            contact.company.isNotEmpty() -> "your work at ${contact.company}"
            contact.title.isNotEmpty() -> "your work as ${contact.title}"
            else -> "your work"
        }
        val signer = pick.sender.ifEmpty { "The ForkMesh team" }
        val to = emailAddress(contact) ?: contact.email
        val named = contact.company.ifEmpty { contact.displayName }
        if (pick.channel == GenieOutreachChannel.Mastodon) {
            val mention = pick.handle?.mention ?: contact.mastodon
            val post = "$mention $hello! I think ForkMesh could fit $about: repositories mirrored and preserved across independent hosts, with issues, pull requests and chat in one app. Have a look at https://forkmesh.com #git #opensource"
            return GenieOutreachDraft(GenieOutreachChannel.Mastodon, mention, body = clipToot(post), byModel = false)
        }
        if (pick.investor && deck.isNotEmpty()) {
            val body = "$hello,\n\nForkMesh (https://forkmesh.com) turns Claude Code and Codex into attributable contributors on a local-first Git platform that teams control. We are raising our round now, and I would value your view.\n\nOur deck, made for you:\n$deck\n\nWould you be open to a 20-minute call?\n\nBest,\n$signer"
            return GenieOutreachDraft(
                GenieOutreachChannel.Email, to, "ForkMesh deck for $named", body, byModel = false, deck = deck, zohoRegion = pick.zohoRegion, cc = pick.cc,
            )
        }
        val body = "$hello,\n\nI came across $about and thought ForkMesh might be useful to you. It lets developers collaborate on, mirror and preserve their repositories across independent hosts, so no single provider can take their code away, with issues, pull requests, chat and coding agents in one app.\n\nWould you be open to a quick look or a short call? You can try it at https://forkmesh.com.\n\nBest,\n$signer"
        return GenieOutreachDraft(
            GenieOutreachChannel.Email, to, "ForkMesh for $named", withDeck(body, deck), byModel = false, deck = deck, zohoRegion = pick.zohoRegion, cc = pick.cc,
        )
    }

    fun write(pick: GenieOutreachPick, writers: List<GenieOutreachWriter>, deck: String = ""): GenieOutreachDraft {
        val messages = messages(pick, deck)
        val sources = messages.joinToString("\n") { it.content }
        for (writer in writers) {
            val text = attempt(writer, messages) ?: continue
            val draft = parse(text, pick, deck, writer.name) ?: continue
            if (unsupported(draft.subject + "\n" + draft.body, sources).isNotEmpty()) continue
            return draft
        }
        return fallback(pick, deck)
    }

    private val attempts = Executors.newCachedThreadPool { runnable -> Thread(runnable, "genie-writer").apply { isDaemon = true } }

    fun attempt(writer: GenieOutreachWriter, messages: List<GenieMessage>): String? {
        val future = attempts.submit(Callable { writer.run(messages) })
        return try {
            future.get((maxOf(writer.timeoutSeconds, 0.1) * 1000).toLong(), TimeUnit.MILLISECONDS)
        } catch (_: Exception) {
            future.cancel(true)
            null
        }
    }
}

class GenieOutreachLog(private val store: PrefStore?) {
    companion object {
        const val OFF_KEY = "genie/outreach_off"
        const val SHOWN_KEY = "genie/outreach_shown"
        const val DRAFTED_KEY = "genie/outreach_drafted"
        const val LAST_KEY = "genie/outreach_last"
        const val EVERY = 2 * 60 * 60.0
        const val FIRST_AFTER = 3 * 60.0
        const val REST_AFTER_SHOWN = 7 * 24 * 60 * 60.0
        const val MOST = 300

        fun strings(store: PrefStore?, key: String): List<String> =
            (runCatching { Json.parse(store?.getString(key) ?: "[]") }.getOrNull() as? List<*>).orEmpty().map { it.toString() }

        fun putStrings(store: PrefStore?, key: String, values: List<String>) {
            store?.putString(key, Json.stringify(values))
        }
    }

    var off: Boolean = store?.getBool(OFF_KEY) ?: false
        private set
    var last: Double = store?.getDouble(LAST_KEY) ?: 0.0
        private set
    private var shown: Map<String, Double> = strings(store, SHOWN_KEY).mapNotNull { entry ->
        val parts = entry.split(" ")
        val at = parts.getOrNull(1)?.toDoubleOrNull()
        if (parts.size == 2 && at != null) parts[0] to at else null
    }.toMap()
    private var drafted: List<String> = strings(store, DRAFTED_KEY)

    fun setOff(value: Boolean) {
        off = value
        store?.putBool(OFF_KEY, value)
    }

    fun due(now: Double, activeSince: Double): Boolean = !off && now - activeSince >= FIRST_AFTER && now - last >= EVERY

    fun skip(now: Double): Set<String> = shown.filterValues { now - it < REST_AFTER_SHOWN }.keys + drafted

    fun showed(contactId: String, now: Double) {
        last = now
        val kept = (shown + (contactId to now)).entries.sortedByDescending { it.value }.take(MOST)
        shown = kept.associate { it.key to it.value }
        store?.putDouble(LAST_KEY, now)
        putStrings(store, SHOWN_KEY, kept.map { "${it.key} ${it.value.toLong()}" })
    }

    val draftedContacts: Set<String> get() = drafted.toSet()

    fun drafted(contactId: String) {
        drafted = (drafted.filter { it != contactId } + contactId).takeLast(MOST)
        putStrings(store, DRAFTED_KEY, drafted)
    }

    fun checked(now: Double) {
        last = now
        store?.putDouble(LAST_KEY, now)
    }
}

object GenieDesktopWriter {
    fun name(desktop: String): String = if (desktop.isBlank()) "Claude Code or Codex on your desktop" else "Claude Code or Codex on ${desktop.trim()}"

    fun make(desktop: String, request: (String, Map<String, Any?>, Long) -> Map<String, Any?>): GenieOutreachWriter =
        GenieOutreachWriter(name(desktop), GenieOutreach.DESKTOP_TIMEOUT) { messages ->
            val prompt = messages.joinToString("\n\n") { it.content }
            val answer = request("agents.write", mapOf("prompt" to prompt), (GenieOutreach.DESKTOP_TIMEOUT * 1000).toLong())
            (answer["text"] as? String).orEmpty()
        }
}
