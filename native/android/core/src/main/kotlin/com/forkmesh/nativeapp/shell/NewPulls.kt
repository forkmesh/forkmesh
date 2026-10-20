package com.forkmesh.nativeapp.shell

import com.forkmesh.nativeapp.api.PrefStore
import com.forkmesh.nativeapp.model.PublishedPull
import com.forkmesh.nativeapp.navigation.RepoRef

enum class DeviceHeat { Calm, Warm, Critical }

class NewPullsMarker(private val prefs: PrefStore?) {
    private var memory: List<String> = emptyList()

    fun seen(slug: String): Int? = entries()[slug.lowercase()]

    fun newCount(slug: String, pulls: List<PublishedPull>): Int {
        val seen = seen(slug)
        if (seen == null) {
            record(slug, top(pulls))
            return 0
        }
        return pulls.count { it.number > seen && it.isOpen }
    }

    fun markSeen(slug: String, pulls: List<PublishedPull>) {
        record(slug, maxOf(top(pulls), seen(slug) ?: 0))
    }

    private fun lines(): List<String> {
        val stored = prefs?.getString(KEY) ?: return memory
        return stored.split('\n').filter { it.isNotEmpty() }
    }

    private fun entries(): Map<String, Int> {
        val out = LinkedHashMap<String, Int>()
        for (line in lines()) {
            val (slug, number) = parse(line) ?: continue
            if (slug !in out) out[slug] = number
        }
        return out
    }

    private fun record(slug: String, number: Int) {
        val key = slug.lowercase()
        if (key.isEmpty() || seen(key) == number) return
        val rest = lines().filter { parse(it)?.first != key }
        val next = (listOf("$number $key") + rest).take(LIMIT)
        memory = next
        prefs?.putString(KEY, next.joinToString("\n"))
    }

    private fun parse(line: String): Pair<String, Int>? {
        val space = line.indexOf(' ')
        if (space <= 0) return null
        val number = line.substring(0, space).toIntOrNull() ?: return null
        val slug = line.substring(space + 1)
        return if (slug.isEmpty()) null else slug to number
    }

    companion object {
        const val KEY = "shell/pulls-seen"
        const val LIMIT = 40
        const val CALM_POLL_MS = 20_000L
        const val WARM_POLL_MS = 300_000L

        fun top(pulls: List<PublishedPull>): Int = pulls.maxOfOrNull { it.number } ?: 0

        fun openCount(pulls: List<PublishedPull>): Int = pulls.count { it.isOpen }

        fun pollEvery(heat: DeviceHeat, lowPower: Boolean): Long? = when (heat) {
            DeviceHeat.Critical -> null
            DeviceHeat.Warm -> WARM_POLL_MS
            DeviceHeat.Calm -> if (lowPower) WARM_POLL_MS else CALM_POLL_MS
        }
    }
}

data class ShellPulls(
    val slug: String = "",
    val pulls: List<PublishedPull> = emptyList(),
    val revision: Int = 0,
    val newPulls: Int = 0,
) {
    fun adopted(repo: RepoRef, list: List<PublishedPull>, fresh: Int): ShellPulls {
        val key = repo.slug.lowercase()
        val changed = key != slug || list != pulls
        return copy(slug = key, pulls = list, revision = if (changed) revision + 1 else revision, newPulls = fresh)
    }

    fun holds(repo: RepoRef?): Boolean = repo != null && repo.slug.lowercase() == slug

    fun newBadge(current: RepoRef?): Int = if (holds(current)) newPulls else 0

    fun openCount(current: RepoRef?): Int? = if (holds(current)) NewPullsMarker.openCount(pulls) else null
}
