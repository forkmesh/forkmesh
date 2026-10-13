package com.forkmesh.nativeapp.collab

import com.forkmesh.nativeapp.ci.RepoRef
import com.forkmesh.nativeapp.code.CodeMapNode
import com.forkmesh.nativeapp.json.firstStr
import com.forkmesh.nativeapp.json.int
import com.forkmesh.nativeapp.json.maps
import com.forkmesh.nativeapp.json.obj
import com.forkmesh.nativeapp.json.optBool
import com.forkmesh.nativeapp.json.str
import com.forkmesh.nativeapp.model.RepoTree
import com.forkmesh.nativeapp.navigation.RepoRef as NavRepoRef
import com.forkmesh.nativeapp.navigation.RouteParser
import java.io.ByteArrayOutputStream
import java.net.URI
import java.time.ZoneOffset
import java.time.ZonedDateTime
import java.util.zip.CRC32

enum class ReferenceKind { Issue, Pull, Commit, Url }

data class Reference(
    val kind: ReferenceKind,
    val label: String,
    val repo: RepoRef?,
    val number: Int,
    val sha: String,
    val url: String,
) {
    fun link(current: RepoRef): String {
        val target = repo ?: current
        return when (kind) {
            ReferenceKind.Issue -> "forkmesh://${target.owner}/${target.name}/issues/$number"
            ReferenceKind.Pull -> "forkmesh://${target.owner}/${target.name}/pulls/$number"
            ReferenceKind.Commit -> "forkmesh://${target.owner}/${target.name}/commit/$sha"
            ReferenceKind.Url -> url
        }
    }
}

sealed class ReferenceSegment {
    data class Text(val text: String) : ReferenceSegment()
    data class Ref(val reference: Reference) : ReferenceSegment()
}

data class ReferenceRun(val text: String, val url: String?)

object ReferenceLinks {
    val CHAT_REPO = RepoRef("forkmesh", "forkmesh")

    fun repoOf(slug: String?): RepoRef? = NavRepoRef.parse(slug?.trim())?.let { RepoRef(it.owner, it.name) }

    fun repoFor(href: String = "", slug: String = ""): RepoRef =
        repoOf(slug) ?: RouteParser.parse(if (href.startsWith("/")) "https://forkmesh.com$href" else href)?.stack?.lastOrNull()?.repoRef?.let { RepoRef(it.owner, it.name) } ?: CHAT_REPO
    private val TRAILING = setOf('.', ',', ';', ':', '!', '?')
    private val SLUG_PART = Regex("^[A-Za-z0-9._-]{1,100}$")

    private fun isBoundary(chars: String, index: Int): Boolean {
        if (index < 0 || index >= chars.length) return true
        val c = chars[index]
        return !c.isLetterOrDigit() && c != '_'
    }

    private fun isHex(c: Char): Boolean = c in '0'..'9' || c in 'a'..'f' || c in 'A'..'F'

    private fun digits(chars: String, start: Int): Int {
        var end = start
        while (end < chars.length && chars[end] in '0'..'9') end++
        return end
    }

    private fun hasPrefix(chars: String, at: Int, prefix: String, ignoreCase: Boolean): Boolean =
        at + prefix.length <= chars.length && chars.regionMatches(at, prefix, 0, prefix.length, ignoreCase)

    private fun repoPrefix(chars: String, hash: Int): Pair<RepoRef, Int>? {
        var start = hash
        var slash = -1
        while (start > 0) {
            val c = chars[start - 1]
            if (c == '/') {
                if (slash >= 0) break
                slash = start - 1
            } else if (!(c.isLetterOrDigit() || c == '-' || c == '_' || c == '.')) {
                break
            }
            start--
        }
        if (slash <= start || slash >= hash - 1 || !isBoundary(chars, start - 1)) return null
        val owner = chars.substring(start, slash)
        val name = chars.substring(slash + 1, hash)
        if (!SLUG_PART.matches(owner) || !SLUG_PART.matches(name) || owner.startsWith(".") || name.startsWith(".")) return null
        return RepoRef(owner, name) to start
    }

    fun segments(text: String, local: Boolean = true, commits: Boolean = true): List<ReferenceSegment> {
        val out = ArrayList<ReferenceSegment>()
        val plain = StringBuilder()
        var fenced = false
        var lineStart = true
        var i = 0
        fun flush() {
            if (plain.isNotEmpty()) {
                out.add(ReferenceSegment.Text(plain.toString()))
                plain.setLength(0)
            }
        }
        fun emit(reference: Reference) {
            flush()
            out.add(ReferenceSegment.Ref(reference))
        }
        while (i < text.length) {
            val c = text[i]
            if (lineStart && hasPrefix(text, i, "```", false)) {
                fenced = !fenced
                plain.append("```")
                i += 3
                lineStart = false
                continue
            }
            if (c == '\n') {
                plain.append(c)
                i++
                lineStart = true
                continue
            }
            lineStart = false
            if (c == '`') {
                var ticks = 0
                while (i + ticks < text.length && text[i + ticks] == '`') ticks++
                val delimiter = "`".repeat(ticks)
                val close = text.indexOf(delimiter, i + ticks)
                if (close >= 0) {
                    plain.append(text, i, close + ticks)
                    i = close + ticks
                } else {
                    plain.append(delimiter)
                    i += ticks
                }
                continue
            }
            if (fenced) {
                plain.append(c)
                i++
                continue
            }
            if (hasPrefix(text, i, "https://", false) || hasPrefix(text, i, "http://", false) || hasPrefix(text, i, "forkmesh://", false)) {
                var end = i
                while (end < text.length && !text[end].isWhitespace()) end++
                var linkEnd = end
                while (linkEnd > i && text[linkEnd - 1] in TRAILING) linkEnd--
                val url = text.substring(i, linkEnd)
                emit(Reference(ReferenceKind.Url, url, null, 0, "", url))
                plain.append(text, linkEnd, end)
                i = end
                continue
            }
            if (local && isBoundary(text, i - 1)) {
                var matched = false
                for ((prefix, kind) in listOf(
                    "pull request #" to ReferenceKind.Pull,
                    "pull #" to ReferenceKind.Pull,
                    "PR #" to ReferenceKind.Pull,
                    "issue #" to ReferenceKind.Issue,
                )) {
                    if (!hasPrefix(text, i, prefix, true)) continue
                    val start = i + prefix.length
                    val end = digits(text, start)
                    val number = if (end > start) text.substring(start, end).toIntOrNull() else null
                    if (number == null) continue
                    emit(Reference(kind, text.substring(i, end), null, number, "", ""))
                    i = end
                    matched = true
                    break
                }
                if (matched) continue
            }
            if (c == '#') {
                val end = digits(text, i + 1)
                val number = if (end > i + 1) text.substring(i + 1, end).toIntOrNull() else null
                if (number != null) {
                    val prefix = repoPrefix(text, i)
                    if (prefix != null && plain.length >= i - prefix.second) {
                        plain.setLength(plain.length - (i - prefix.second))
                        emit(Reference(ReferenceKind.Issue, text.substring(prefix.second, end), prefix.first, number, "", ""))
                        i = end
                        continue
                    }
                    if (local && isBoundary(text, i - 1)) {
                        emit(Reference(ReferenceKind.Issue, text.substring(i, end), null, number, "", ""))
                        i = end
                        continue
                    }
                }
            }
            if (commits && local && isHex(c) && isBoundary(text, i - 1)) {
                var end = i
                while (end < text.length && isHex(text[end])) end++
                val length = end - i
                if (length in 7..40 && isBoundary(text, end)) {
                    val sha = text.substring(i, end)
                    emit(Reference(ReferenceKind.Commit, sha, null, 0, sha, ""))
                    i = end
                    continue
                }
            }
            plain.append(c)
            i++
        }
        flush()
        return out
    }

    fun references(text: String): List<Reference> = segments(text).mapNotNull { (it as? ReferenceSegment.Ref)?.reference }

    fun runs(text: String, repo: RepoRef?, commits: Boolean = true): List<ReferenceRun> {
        val out = ArrayList<ReferenceRun>()
        fun add(text: String, url: String?) {
            val last = out.lastOrNull()
            if (url == null && last != null && last.url == null) {
                out[out.size - 1] = ReferenceRun(last.text + text, null)
            } else {
                out.add(ReferenceRun(text, url))
            }
        }
        for (segment in segments(text, local = repo != null, commits = commits)) {
            when (segment) {
                is ReferenceSegment.Text -> add(segment.text, null)
                is ReferenceSegment.Ref -> {
                    val ref = segment.reference
                    val target = ref.repo ?: repo
                    when {
                        ref.kind == ReferenceKind.Url -> add(ref.label, ref.url)
                        target != null -> add(ref.label, ref.link(target))
                        else -> add(ref.label, null)
                    }
                }
            }
        }
        return out
    }

    fun markdown(text: String, repo: RepoRef): String = segments(text).joinToString("") { segment ->
        when (segment) {
            is ReferenceSegment.Text -> segment.text
            is ReferenceSegment.Ref -> {
                val ref = segment.reference
                if (ref.kind == ReferenceKind.Url && ref.label.startsWith("http")) ref.label else "[${ref.label}](${ref.link(repo)})"
            }
        }
    }
}

class ZipStoreWriter(now: ZonedDateTime = ZonedDateTime.now(ZoneOffset.UTC)) {
    private class Central(val name: ByteArray, val crc: Long, val size: Int, val offset: Int)

    private val data = ByteArrayOutputStream()
    private val central = ArrayList<Central>()
    private val dosTime: Int
    private val dosDate: Int

    init {
        val utc = now.withZoneSameInstant(ZoneOffset.UTC)
        val year = maxOf(1980, utc.year)
        dosTime = ((utc.hour shl 11) or (utc.minute shl 5) or (utc.second / 2)) and 0xffff
        dosDate = (((year - 1980) shl 9) or (utc.monthValue shl 5) or utc.dayOfMonth) and 0xffff
    }

    val count: Int get() = central.size
    val size: Int get() = data.size()

    private fun ByteArrayOutputStream.u16(value: Int) {
        write(value and 0xff)
        write((value ushr 8) and 0xff)
    }

    private fun ByteArrayOutputStream.u32(value: Long) {
        for (shift in 0 until 32 step 8) write(((value ushr shift) and 0xff).toInt())
    }

    fun add(path: String, bytes: ByteArray): Boolean {
        val clean = safeName(path) ?: return false
        if (central.size >= MAX_ENTRIES || data.size() + bytes.size + 256 >= MAX_BYTES) return false
        val name = clean.toByteArray(Charsets.UTF_8)
        val crc = CRC32().also { it.update(bytes) }.value
        val offset = data.size()
        data.u32(0x04034B50)
        data.u16(20)
        data.u16(0x0800)
        data.u16(0)
        data.u16(dosTime)
        data.u16(dosDate)
        data.u32(crc)
        data.u32(bytes.size.toLong())
        data.u32(bytes.size.toLong())
        data.u16(name.size)
        data.u16(0)
        data.write(name)
        data.write(bytes)
        central.add(Central(name, crc, bytes.size, offset))
        return true
    }

    fun finish(): ByteArray {
        val start = data.size()
        val directory = ByteArrayOutputStream()
        for (entry in central) {
            directory.u32(0x02014B50)
            directory.u16(20)
            directory.u16(20)
            directory.u16(0x0800)
            directory.u16(0)
            directory.u16(dosTime)
            directory.u16(dosDate)
            directory.u32(entry.crc)
            directory.u32(entry.size.toLong())
            directory.u32(entry.size.toLong())
            directory.u16(entry.name.size)
            directory.u16(0)
            directory.u16(0)
            directory.u16(0)
            directory.u16(0)
            directory.u32(0)
            directory.u32(entry.offset.toLong())
            directory.write(entry.name)
        }
        val end = ByteArrayOutputStream()
        end.u32(0x06054B50)
        end.u16(0)
        end.u16(0)
        end.u16(central.size)
        end.u16(central.size)
        end.u32(directory.size().toLong())
        end.u32(start.toLong())
        end.u16(0)
        val out = ByteArrayOutputStream()
        data.writeTo(out)
        directory.writeTo(out)
        end.writeTo(out)
        return out.toByteArray()
    }

    companion object {
        const val MAX_BYTES = 64 * 1024 * 1024
        const val MAX_ENTRIES = 5000

        fun safeName(path: String): String? {
            val parts = path.split("/")
            if (parts.isEmpty() || parts.any { it == ".." || it == "." || it.isEmpty() }) return null
            return parts.joinToString("/")
        }
    }
}

interface RepoArchiveReader {
    fun tree(owner: String, name: String, path: String, ref: String): RepoTree
    fun rawBytes(owner: String, name: String, path: String, ref: String, maxBytes: Int): ByteArray
}

data class RepoArchiveResult(val data: ByteArray, val files: Int, val skipped: List<String>, val fileName: String)

class RepoArchiveBuilder(private val reader: RepoArchiveReader) {
    fun build(
        owner: String,
        name: String,
        ref: String,
        cancelled: () -> Boolean = { false },
        onProgress: ((Int, Int) -> Unit)? = null,
    ): RepoArchiveResult {
        val writer = ZipStoreWriter()
        val skipped = ArrayList<String>()
        val queue = ArrayDeque<String>()
        queue.add("")
        var visited = 0
        val root = if (ref.isEmpty()) name else "$name-${ref.replace('/', '-')}"
        while (queue.isNotEmpty()) {
            val dir = queue.removeFirst()
            visited++
            if (visited > MAX_DIRECTORIES) {
                skipped.add(dir)
                continue
            }
            if (cancelled()) throw CollabException("Cancelled.")
            val listing = reader.tree(owner, name, dir, ref)
            for (entry in listing.entries.sortedBy { it.path }) {
                if (entry.isDirectory) {
                    queue.add(entry.path)
                    continue
                }
                if (entry.size > MAX_FILE_BYTES) {
                    skipped.add(entry.path)
                    continue
                }
                val bytes = try {
                    reader.rawBytes(owner, name, entry.path, ref, MAX_FILE_BYTES)
                } catch (_: Exception) {
                    null
                }
                if (bytes == null || !writer.add("$root/${entry.path}", bytes)) {
                    skipped.add(entry.path)
                    continue
                }
                onProgress?.invoke(writer.count, writer.size)
            }
        }
        val files = writer.count
        return RepoArchiveResult(writer.finish(), files, skipped, fileName(name, ref))
    }

    companion object {
        const val MAX_FILE_BYTES = 8 * 1024 * 1024
        const val MAX_DIRECTORIES = 2000

        fun fileName(repo: String, ref: String): String = "$repo${if (ref.isEmpty()) "" else "-" + ref.replace('/', '-')}.zip"
    }
}

data class RepoAbout(
    val description: String = "",
    val website: String = "",
    val logoUrl: String = "",
    val bannerUrl: String = "",
    val isPrivate: Boolean = false,
    val federate: Boolean = true,
    val broadcastEvents: Boolean = true,
    val acceptComments: Boolean = true,
    val fediverseHandle: String = "",
    val followers: Int = 0,
) {
    companion object {
        fun fromJson(j: Map<String, Any?>): RepoAbout {
            val fedi = j.obj("fediverse")
            val settings = fedi.obj("settings")
            return RepoAbout(
                description = j.str("description"),
                website = j.str("website"),
                logoUrl = j.str("logoUrl"),
                bannerUrl = j.str("bannerUrl"),
                isPrivate = j["isPrivate"] == true,
                federate = settings.optBool("federate") ?: true,
                broadcastEvents = settings.optBool("broadcastEvents") ?: true,
                acceptComments = settings.optBool("acceptComments") ?: true,
                fediverseHandle = fedi.str("handle"),
                followers = fedi.int("followers"),
            )
        }
    }
}

data class RepoForkRow(val owner: String, val name: String) {
    companion object {
        fun fromJson(j: Map<String, Any?>): RepoForkRow? {
            val owner = j.firstStr("owner", "node")
            val name = j.firstStr("name", "repo")
            if (owner.isEmpty() || name.isEmpty()) return null
            return RepoForkRow(owner, name)
        }
    }
}

data class RepoForkOutcome(val created: Boolean, val queued: Boolean, val owner: String, val name: String) {
    val message: String
        get() = when {
            created -> "Fork created${if (owner.isEmpty()) "" else " as $owner/$name"}."
            queued -> "Fork recorded. Your managed mirror creates it the next time it is online."
            else -> "You already have a fork${if (owner.isEmpty()) "" else ": $owner/$name"}."
        }

    companion object {
        fun fromJson(j: Map<String, Any?>): RepoForkOutcome {
            val fork = j.obj("fork")
            return RepoForkOutcome(j["created"] == true, j["queued"] == true, fork.firstStr("owner", "node"), fork.firstStr("name", "repo"))
        }
    }
}

enum class RepoSettingsSection(val title: String) {
    General("General"), Agents("Agents"), Automation("Automation"), Coves("Coves"), Permissions("Permissions"), Danger("Danger zone");

    companion object {
        fun route(value: String?): RepoSettingsSection = entries.firstOrNull { it.name.lowercase() == value } ?: General
    }
}

object RepoSettingsRules {
    const val MAX_DESCRIPTION = 240
    const val MAX_WEBSITE = 240

    fun validateWebsite(text: String): String? {
        val value = text.trim()
        if (value.isEmpty()) return null
        val url = try {
            URI(value)
        } catch (_: Exception) {
            null
        }
        val scheme = url?.scheme?.lowercase()
        if (value.length > MAX_WEBSITE || url == null || (scheme != "https" && scheme != "http") || url.host == null) {
            return "Enter a full http(s) address."
        }
        return null
    }

    fun aboutPatch(old: RepoAbout, new: RepoAbout): Map<String, Any?> {
        val body = LinkedHashMap<String, Any?>()
        if (new.description != old.description) body["description"] = new.description.take(MAX_DESCRIPTION)
        if (new.website.trim() != old.website.trim()) body["website"] = new.website.trim()
        if (new.federate != old.federate || new.broadcastEvents != old.broadcastEvents || new.acceptComments != old.acceptComments) {
            body["fediverse"] = linkedMapOf(
                "federate" to new.federate,
                "broadcastEvents" to new.broadcastEvents,
                "acceptComments" to new.acceptComments,
            )
        }
        return body
    }

    fun canEdit(repoOwner: String, account: String?, organizationOwner: Boolean): Boolean {
        if (account.isNullOrEmpty()) return false
        return organizationOwner || account.lowercase() == repoOwner.lowercase()
    }

    fun confirmsName(typed: String, repo: String): Boolean = repo.isNotEmpty() && typed.trim() == repo
}

class RepoSettingsClient(
    private val call: CollabJsonCall,
    private val owner: String,
    private val name: String,
    private val sessionToken: () -> String,
) {
    private val base: String get() = "/api/repo/$owner/$name"

    private fun authed(body: Map<String, Any?>): Map<String, Any?> {
        val out = LinkedHashMap(body)
        val token = sessionToken()
        if (token.isNotEmpty() && token != "cookie") out["sessionToken"] = token
        return out
    }

    fun about(): RepoAbout = RepoAbout.fromJson(CollabHttp.json(call, "GET", "$base/about"))

    fun saveAbout(old: RepoAbout, new: RepoAbout): Boolean {
        val patch = RepoSettingsRules.aboutPatch(old, new)
        if (patch.isEmpty()) return false
        CollabHttp.json(call, "POST", "$base/about", body = authed(patch))
        return true
    }

    fun forks(): Pair<Int, List<RepoForkRow>> {
        val reply = CollabHttp.json(call, "GET", "$base/forks")
        return reply.int("count") to reply.maps("forks").take(200).mapNotNull(RepoForkRow::fromJson)
    }

    fun fork(targetNode: String = ""): RepoForkOutcome {
        val body = LinkedHashMap<String, Any?>()
        if (targetNode.isNotBlank()) body["targetNode"] = targetNode.trim().lowercase()
        return RepoForkOutcome.fromJson(CollabHttp.json(call, "POST", "$base/forks", body = authed(body)))
    }

    fun sizes(ref: String = "", depth: Int = 0): CodeMapNode {
        val query = ArrayList<Pair<String, String>>()
        if (ref.isNotEmpty()) query.add("ref" to ref)
        if (depth > 0) query.add("depth" to "$depth")
        val reply = CollabHttp.json(call, "GET", "$base/sizes", query)
        @Suppress("UNCHECKED_CAST")
        val root = (reply["tree"] as? Map<String, Any?>) ?: (reply["root"] as? Map<String, Any?>) ?: reply
        return CodeMapNode.fromJson(root)
    }

    fun history(ref: String): List<Map<String, Any?>> {
        val reply = CollabHttp.json(call, "GET", "$base/history", if (ref.isEmpty()) emptyList() else listOf("ref" to ref))
        return reply.maps("commits")
    }
}
