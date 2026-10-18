package com.forkmesh.nativeapp.codeinsight

import java.text.NumberFormat
import java.util.Locale

enum class InsightHistoryDepth(val id: String, val commits: Int, val label: String, val shortLabel: String) {
    FilesOnly("files", 1, "Files only", "files only"),
    Recent("300", 300, "Last 300 commits", "300 commits"),
    Extended("2000", 2000, "Last 2,000 commits", "2,000 commits"),
    ;

    val options: InsightOptions
        get() = if (this == FilesOnly) InsightOptions(history = false) else InsightOptions(maxCommits = commits)

    companion object {
        const val PREF_KEY = "codeInsight/historyDepth"
        val DEFAULT = Recent

        fun fromId(id: String?): InsightHistoryDepth = entries.firstOrNull { it.id == id } ?: DEFAULT
    }
}

sealed class InsightTarget {
    abstract val key: String
    abstract val title: String

    data class Local(val name: String, val publishedAs: String? = null) : InsightTarget() {
        override val key: String get() = "local:$name"
        override val title: String get() = name
    }

    data class Account(val owner: String, val name: String) : InsightTarget() {
        override val key: String get() = "repo:$slug"
        override val title: String get() = name
        val slug: String get() = "$owner/$name"
    }

    companion object {
        const val LAST_KEY = "codeInsight/lastTarget"

        fun parse(key: String?): InsightTarget? {
            val text = key ?: return null
            if (text.startsWith("local:")) {
                val name = text.removePrefix("local:")
                return if (name.isEmpty()) null else Local(name)
            }
            if (text.startsWith("repo:")) {
                val parts = text.removePrefix("repo:").split('/')
                if (parts.size != 2 || parts[0].isEmpty() || parts[1].isEmpty()) return null
                return Account(parts[0], parts[1])
            }
            return null
        }
    }
}

object InsightLabels {
    const val ON_THIS_PHONE = "On this phone"

    fun sourceLabel(target: InsightTarget, depth: InsightHistoryDepth): String = when (target) {
        is InsightTarget.Local -> ON_THIS_PHONE
        is InsightTarget.Account -> "Analysis copy · ${depth.shortLabel}"
    }

    fun fileCount(count: Int): String {
        val text = NumberFormat.getIntegerInstance(Locale.US).format(count)
        return if (count == 1) "$text file" else "$text files"
    }

    fun statusLine(source: String, files: Int, summary: String): String =
        listOf(source, fileCount(files), summary).filter { it.isNotEmpty() }.joinToString(" · ")

    fun copyFolder(owner: String, name: String): String? {
        if (!isSafeSegment(owner) || !isSafeSegment(name)) return null
        return "$owner/$name"
    }

    private fun isSafeSegment(value: String): Boolean =
        value.isNotEmpty() && value.length <= 100 && value != "." && value != ".." && value.all { it.isLetterOrDigit() || it == '-' || it == '_' || it == '.' }
}

class InsightSnapshotCache<K>(private val ttlMillis: Long = TTL_MILLIS, private val clock: () -> Long = System::currentTimeMillis) {
    private class Entry(val snapshot: InsightSnapshot, val at: Long)

    private val entries = HashMap<K, Entry>()

    @Synchronized
    fun fresh(key: K): InsightSnapshot? {
        val entry = entries[key] ?: return null
        if (clock() - entry.at > ttlMillis) return null
        return entry.snapshot
    }

    @Synchronized
    fun latest(key: K): InsightSnapshot? = entries[key]?.snapshot

    @Synchronized
    fun put(key: K, snapshot: InsightSnapshot) {
        entries[key] = Entry(snapshot, clock())
    }

    @Synchronized
    fun invalidate(key: K) {
        entries.remove(key)
    }

    companion object {
        const val TTL_MILLIS = 10 * 60 * 1000L
    }
}
