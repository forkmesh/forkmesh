package com.forkmesh.nativeapp.settings

import java.net.URLEncoder
import kotlin.math.PI
import kotlin.math.asin
import kotlin.math.cos
import kotlin.math.floor
import kotlin.math.ln
import kotlin.math.min
import kotlin.math.sin

enum class PeopleRing(val id: String, val title: String, val blurb: String) {
    BLAZING("blazing", "Blazing", "Active today and deep in the work"),
    GLOWING("glowing", "Glowing", "Active in the last five days"),
    KINDLING("kindling", "Kindling", "Joined in the last two weeks"),
    WARM("warm", "Warm", "Active this month, or has contributed"),
    EMBERS("embers", "Embers", "Quiet for a while"),
}

data class PeopleMember(
    val name: String,
    val avatar: String = "",
    val createdAt: Long = 0,
    val activityBucket: String = "",
    val totalActiveMs: Long = 0,
    val totalSteps: Long = 0,
    val officeVisits: Int = 0,
    val contributions: Int = 0,
    val nodeCount: Int = 0,
    val emailVerified: Boolean = false,
    val hasPayoutAddress: Boolean = false,
    val statusText: String = "",
    val statusEmoji: String = "",
) {
    val id: String get() = name.lowercase()

    companion object {
        fun parse(map: Map<String, Any?>): PeopleMember? {
            val name = map.text("name").trim()
            if (name.isEmpty() || map.text("kind", "user") != "user") return null
            val status = map.obj("userStatus")
            return PeopleMember(
                name = name,
                avatar = PeopleAvatar.base64(map.text("avatarThumb")),
                createdAt = maxOf(0L, map.long("createdAt")),
                activityBucket = map.text("activityBucket"),
                totalActiveMs = maxOf(0L, map.long("totalActiveMs")),
                totalSteps = maxOf(0L, map.long("totalSteps")),
                officeVisits = maxOf(0, map.int("officeVisits")),
                contributions = listOf("issues", "pulls", "commits", "discussions").sumOf { maxOf(0, map.int(it)) },
                nodeCount = map.list("nodes").size,
                emailVerified = map.flag("emailVerified"),
                hasPayoutAddress = map.text("solana").isNotBlank(),
                statusText = status.text("text").take(80),
                statusEmoji = status.text("emoji").take(8),
            )
        }
    }
}

object PeopleAvatar {
    fun base64(value: String): String {
        if (!value.startsWith("data:image/")) return ""
        val comma = value.indexOf(',')
        if (comma < 0 || !value.substring(0, comma).endsWith(";base64")) return ""
        return value.substring(comma + 1)
    }
}

object PeopleRings {
    const val NEW_WINDOW_MS: Long = 14L * 24 * 60 * 60 * 1000
    const val BLAZING_SCORE = 70
    private const val HOUR: Long = 60L * 60 * 1000

    private val recency = mapOf(
        "hour" to 48, "5h" to 46, "24h" to 42, "3d" to 36, "5d" to 32, "10d" to 25, "30d" to 18, "60d" to 11, "120d" to 5,
    )

    private fun tier(value: Long, tiers: List<Pair<Long, Int>>): Int = tiers.firstOrNull { value >= it.first }?.second ?: 0

    fun score(member: PeopleMember): Int {
        val depth = tier(member.totalActiveMs, listOf(40 * HOUR to 22, 12 * HOUR to 17, 4 * HOUR to 12, HOUR to 7, 10L * 60 * 1000 to 3)) +
            tier(member.totalSteps, listOf(100_000L to 13, 25_000L to 10, 5_000L to 7, 500L to 4, 1L to 1)) +
            tier(member.officeVisits.toLong(), listOf(20L to 6, 5L to 4, 1L to 2)) +
            min(14, floor(ln((member.contributions + 1).toDouble()) / ln(2.0) * 3.5).toInt())
        val readiness = (if (member.emailVerified) 4 else 0) + (if (member.hasPayoutAddress) 4 else 0) + min(7, member.nodeCount * 3)
        return (recency[member.activityBucket] ?: 0).plus(depth).plus(readiness).coerceIn(0, 100)
    }

    fun ring(member: PeopleMember, now: Long): PeopleRing {
        val bucket = member.activityBucket
        val today = bucket in setOf("hour", "5h", "24h")
        if (today && score(member) >= BLAZING_SCORE) return PeopleRing.BLAZING
        if (today || bucket in setOf("3d", "5d")) return PeopleRing.GLOWING
        if (member.createdAt > 0 && now - member.createdAt < NEW_WINDOW_MS) return PeopleRing.KINDLING
        if (bucket in setOf("10d", "30d") || member.contributions > 0 || member.totalSteps > 0 || member.officeVisits > 0) {
            return PeopleRing.WARM
        }
        return PeopleRing.EMBERS
    }

    fun lastActive(bucket: String): String = when (bucket) {
        "hour" -> "Active in the last hour"
        "5h" -> "Active in the last few hours"
        "24h" -> "Active today"
        "3d" -> "Active in the last 3 days"
        "5d" -> "Active in the last 5 days"
        "10d" -> "Active in the last 10 days"
        "30d" -> "Active this month"
        "60d" -> "Active in the last 2 months"
        "120d" -> "Active in the last 4 months"
        else -> "Not seen recently"
    }
}

data class PeopleRanked(val member: PeopleMember, val ring: PeopleRing, val score: Int)

data class PeopleSection(val ring: PeopleRing, val people: List<PeopleRanked>)

object PeopleDirectory {
    private val order = compareByDescending<PeopleRanked> { it.score }
        .thenByDescending { it.member.totalActiveMs }
        .thenBy { it.member.id }

    fun sections(members: List<PeopleMember>, now: Long): List<PeopleSection> {
        val ranked = members.map { PeopleRanked(it, PeopleRings.ring(it, now), PeopleRings.score(it)) }
        return PeopleRing.entries.map { ring -> PeopleSection(ring, ranked.filter { it.ring == ring }.sortedWith(order)) }
    }

    fun filter(sections: List<PeopleSection>, query: String): List<PeopleSection> {
        val needle = query.trim().lowercase().trim('@')
        if (needle.isEmpty()) return sections
        return sections.map { section -> section.copy(people = section.people.filter { it.member.id.contains(needle) }) }
    }
}

data class PeopleSeat(val ring: PeopleRing, val x: Double, val y: Double, val person: PeopleRanked?, val overflow: Int)

object PeopleRingLayout {
    fun radius(ring: Int, size: Double): Double {
        val half = size / 2
        val inner = half * 0.26
        return inner + (half * 0.9 - inner) / (PeopleRing.entries.size - 1) * ring
    }

    fun labelGap(ring: Int, size: Double, seat: Double, label: Double): Double =
        asin(min(1.0, (label / 2 + seat / 2 + 2) / radius(ring, size)))

    fun capacity(ring: Int, size: Double, seat: Double, label: Double): Int {
        val arc = 2 * PI - 2 * labelGap(ring, size, seat, label)
        return maxOf(2, floor(arc * radius(ring, size) / (seat * 1.15)).toInt())
    }

    fun seats(sections: List<PeopleSection>, size: Double, seat: Double, label: Double): List<PeopleSeat> {
        val out = mutableListOf<PeopleSeat>()
        for (section in sections) {
            if (section.people.isEmpty()) continue
            val index = section.ring.ordinal
            val cap = capacity(index, size, seat, label)
            val overflow = if (section.people.size > cap) section.people.size - (cap - 1) else 0
            val shown = if (overflow > 0) section.people.take(cap - 1) else section.people
            val total = shown.size + if (overflow > 0) 1 else 0
            val r = radius(index, size)
            val arc = 2 * PI - 2 * labelGap(index, size, seat, label)
            val spread = min(arc, total * seat * 1.15 / r)
            val start = PI / 2 - spread / 2
            for (slot in 0 until total) {
                val angle = start + spread * (slot + 0.5) / total
                val person = shown.getOrNull(slot)
                out += PeopleSeat(section.ring, size / 2 + r * cos(angle), size / 2 + r * sin(angle), person, if (person == null) overflow else 0)
            }
        }
        return out
    }
}

data class PeopleProfile(
    val name: String,
    val exists: Boolean,
    val isPrivate: Boolean,
    val avatar: String,
    val bio: String,
    val location: String,
    val createdAt: Long,
    val followers: Int,
    val following: Int,
    val isFollowing: Boolean,
    val statusText: String,
    val statusEmoji: String,
) {
    companion object {
        fun parse(map: Map<String, Any?>, requested: String): PeopleProfile {
            val social = map.obj("social")
            val status = map.obj("userStatus")
            fun pick(key: String): Int = maxOf(0, if (social.containsKey(key)) social.int(key) else map.int(key))
            return PeopleProfile(
                name = map.text("name").ifEmpty { requested },
                exists = map["exists"] != false,
                isPrivate = map.flag("profilePrivate"),
                avatar = map.text("avatarPng"),
                bio = map.text("profileBio").take(500),
                location = map.text("profileLocation").take(120),
                createdAt = map.long("createdAt"),
                followers = pick("followers"),
                following = pick("following"),
                isFollowing = if (social.containsKey("isFollowing")) social.flag("isFollowing") else map.flag("isFollowing"),
                statusText = status.text("text").take(80),
                statusEmoji = status.text("emoji").take(8),
            )
        }
    }
}

class PeopleModel(
    private val client: AccountEndpointCalling,
    private val viewer: () -> String,
    private val clock: () -> Long = { System.currentTimeMillis() },
) {
    @Volatile var members: List<PeopleMember> = emptyList()
        private set
    @Volatile var loaded = false
        private set
    @Volatile var error = ""
        private set

    val viewerName: String get() = viewer().lowercase()

    fun sections(): List<PeopleSection> = PeopleDirectory.sections(members, clock())

    fun visible(query: String, focus: PeopleRing?): List<PeopleSection> =
        PeopleDirectory.filter(sections(), query).filter { focus == null || it.ring == focus }

    fun load() {
        try {
            val map = client.accountCall("GET", DIRECTORY_PATH, null)
            val seen = HashSet<String>()
            members = map.maps("users").mapNotNull { PeopleMember.parse(it) }.filter { seen.add(it.id) }
            loaded = true
            error = ""
        } catch (failure: Exception) {
            error = AccountErrorText.text(failure, "Could not load people.")
        }
    }

    companion object {
        const val DIRECTORY_PATH = "/api/accounts/users?view=world"
    }
}

class PeopleProfileModel(name: String, viewer: String, private val client: AccountEndpointCalling) {
    val name: String = name.lowercase()
    private val viewer: String = viewer.lowercase()

    @Volatile var profile: PeopleProfile? = null
        private set
    @Volatile var busy = false
        private set
    @Volatile var error = ""
        private set
    @Volatile var actionError = ""
        private set

    val isSelf: Boolean get() = viewer.isNotEmpty() && viewer == name
    val signedIn: Boolean get() = viewer.isNotEmpty()

    val path: String
        get() = if (signedIn) "/api/accounts/$name?viewer=${URLEncoder.encode(viewer, "UTF-8")}" else "/api/accounts/$name"

    fun load() {
        if (!NodeLink.isNodeName(name)) {
            error = "That is not a ForkMesh name."
            return
        }
        try {
            profile = PeopleProfile.parse(client.accountCall("GET", path, null), name)
            error = ""
        } catch (failure: Exception) {
            error = AccountErrorText.text(failure, "Could not load @$name.")
        }
    }

    fun toggleFollow() {
        val current = profile ?: return
        if (!signedIn || isSelf || busy) return
        busy = true
        val follow = !current.isFollowing
        try {
            val map = client.accountCall(if (follow) "POST" else "DELETE", "/api/accounts/$name/follow", emptyMap())
            val count = if (map.containsKey("followers")) map.int("followers") else current.followers + if (follow) 1 else -1
            val state = if (map.containsKey("isFollowing")) map.flag("isFollowing") else follow
            profile = current.copy(isFollowing = state, followers = maxOf(0, count))
            actionError = ""
        } catch (failure: Exception) {
            actionError = followError(failure, follow)
        } finally {
            busy = false
        }
    }

    companion object {
        fun followError(failure: Exception, follow: Boolean): String {
            if (failure is AccountEndpointException) {
                if (failure.code == "cannot_follow_self") return "You cannot follow yourself."
                if (failure.status == 403 || failure.code in setOf("email_not_verified", "account_pending_verification")) return "Verify your email to follow people."
            }
            return AccountErrorText.text(failure, if (follow) "Could not follow." else "Could not unfollow.")
        }
    }
}
