package com.forkmesh.nativeapp.files

import com.forkmesh.nativeapp.json.JsonMap
import com.forkmesh.nativeapp.json.bool
import com.forkmesh.nativeapp.json.str
import java.io.File
import java.io.InputStream
import java.io.OutputStream
import java.nio.ByteBuffer
import java.nio.charset.CharacterCodingException
import java.nio.charset.CodingErrorAction
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

enum class FileKind { Folder, Text, Image, Pdf, Other }

enum class FileSort(val label: String) { Name("Name"), Date("Date"), Size("Size") }

data class SmbAddress(val host: String, val port: Int, val share: String, val path: String, val error: String = "") {
    val valid: Boolean get() = error.isEmpty()
}

data class SavedShare(
    val id: String,
    val host: String,
    val share: String,
    val user: String = "",
    val guest: Boolean = false,
    val remember: Boolean = false,
    val port: Int = 445,
) {
    val title: String get() = "$share on $host"

    val url: String get() = FilesModel.smbUrl(host, share, "", port)

    fun toJson(): Map<String, Any?> = mapOf(
        "id" to id, "host" to host, "share" to share, "user" to user, "guest" to guest, "remember" to remember, "port" to port.toLong(),
    )

    companion object {
        fun fromJson(json: JsonMap): SavedShare? {
            val host = json.str("host")
            val share = json.str("share")
            if (host.isEmpty() || share.isEmpty()) return null
            val port = (json["port"] as? Number)?.toInt() ?: 445
            return SavedShare(json.str("id").ifEmpty { "$host/$share" }, host, share, json.str("user"), json.bool("guest"), json.bool("remember"), port)
        }
    }
}

object FilesModel {
    const val MAX_TEXT_BYTES = 5L * 1024 * 1024
    const val MAX_OPEN_BYTES = 20L * 1024 * 1024
    const val EMPTY_FOLDER = "This folder is empty."

    private val textExtensions = setOf(
        "txt", "md", "markdown", "json", "yaml", "yml", "toml", "ini", "cfg", "conf", "csv", "tsv", "log", "xml", "html", "htm", "css",
        "js", "mjs", "cjs", "ts", "tsx", "jsx", "kt", "kts", "java", "swift", "py", "rb", "go", "rs", "c", "h", "cc", "cpp", "hpp", "m", "mm",
        "sh", "bash", "zsh", "fish", "ps1", "bat", "sql", "gradle", "properties", "env", "gitignore", "dockerfile", "makefile", "cmake",
        "lua", "php", "pl", "r", "dart", "vue", "svelte", "tex", "srt", "vtt", "rst", "adoc", "plist", "svg",
    )
    private val imageExtensions = setOf("png", "jpg", "jpeg", "gif", "heic", "heif", "webp", "bmp")

    fun extension(name: String): String = name.substringAfterLast('.', "").lowercase()

    fun kind(name: String): FileKind {
        val ext = extension(name)
        return when {
            ext in imageExtensions -> FileKind.Image
            ext == "pdf" -> FileKind.Pdf
            ext in textExtensions || name.lowercase() in setOf("makefile", "dockerfile", "readme", "license") -> FileKind.Text
            else -> FileKind.Other
        }
    }

    fun looksLikeText(bytes: ByteArray): Boolean {
        val sample = bytes.copyOf(minOf(bytes.size, 8192))
        if (sample.any { it == 0.toByte() }) return false
        val decoder = Charsets.UTF_8.newDecoder().onMalformedInput(CodingErrorAction.REPORT).onUnmappableCharacter(CodingErrorAction.REPORT)
        var end = sample.size
        if (bytes.size > sample.size) {
            var back = 0
            while (back < 3 && end - back - 1 >= 0 && (sample[end - back - 1].toInt() and 0xC0) == 0x80) back++
            if (end - back - 1 >= 0 && (sample[end - back - 1].toInt() and 0xC0) == 0xC0) end -= back + 1
        }
        return try {
            decoder.decode(ByteBuffer.wrap(sample, 0, end))
            true
        } catch (_: CharacterCodingException) {
            false
        }
    }

    fun parts(path: String): List<String> = path.replace('\\', '/').split('/').filter { it.isNotEmpty() }

    fun join(folder: String, name: String): String = (parts(folder) + parts(name)).joinToString("/")

    fun parent(path: String): String = parts(path).dropLast(1).joinToString("/")

    fun name(path: String): String = parts(path).lastOrNull() ?: ""

    fun isInside(path: String, ancestor: String): Boolean {
        val a = parts(ancestor)
        val p = parts(path)
        return p.size >= a.size && p.subList(0, a.size) == a
    }

    fun crumbs(title: String, path: String): List<Pair<String, String>> {
        val parts = parts(path)
        return listOf(title to "") + parts.indices.map { parts[it] to parts.subList(0, it + 1).joinToString("/") }
    }

    const val LOCAL_NAME_PROBLEM = "That file name cannot be saved on this phone."

    fun localName(name: String): String? {
        val last = parts(name).lastOrNull() ?: return null
        if (last == "." || last == ".." || last.any { it.code == 0 }) return null
        return last
    }

    fun localTarget(folder: File, name: String): File {
        val local = localName(name) ?: throw FileProblem(LOCAL_NAME_PROBLEM)
        val base = folder.toPath().toAbsolutePath().normalize()
        val target = base.resolve(local).normalize()
        if (target.parent != base) throw FileProblem(LOCAL_NAME_PROBLEM)
        return target.toFile()
    }

    fun nameProblem(name: String): String? {
        val trimmed = name.trim()
        return when {
            trimmed.isEmpty() -> "Enter a name."
            trimmed == "." || trimmed == ".." -> "That name is not allowed."
            trimmed.any { it in "/\\:*?\"<>|" || it.code < 32 } -> "Names cannot contain / \\ : * ? \" < > |."
            trimmed.length > 255 -> "That name is too long."
            else -> null
        }
    }

    fun freeName(existing: Collection<String>, name: String): String {
        val taken = existing.map { it.lowercase() }.toSet()
        if (name.lowercase() !in taken) return name
        val dot = name.lastIndexOf('.')
        val stem = if (dot > 0) name.substring(0, dot) else name
        val ext = if (dot > 0) name.substring(dot) else ""
        var n = 2
        while ("$stem ($n)$ext".lowercase() in taken) n++
        return "$stem ($n)$ext"
    }

    fun arrange(items: List<FileItem>, sort: FileSort, showHidden: Boolean, filter: String): List<FileItem> {
        val needle = filter.trim().lowercase()
        val visible = items.filter { (showHidden || !it.hidden) && (needle.isEmpty() || it.name.lowercase().contains(needle)) }
        val order: Comparator<FileItem> = when (sort) {
            FileSort.Name -> compareBy { it.name.lowercase() }
            FileSort.Date -> compareByDescending<FileItem> { it.modified }.thenBy { it.name.lowercase() }
            FileSort.Size -> compareByDescending<FileItem> { it.size }.thenBy { it.name.lowercase() }
        }
        return visible.sortedWith(compareByDescending<FileItem> { it.isFolder }.then(order))
    }

    fun formatSize(bytes: Long): String {
        if (bytes < 1024) return "$bytes B"
        val units = listOf("KB", "MB", "GB", "TB")
        var value = bytes / 1024.0
        var unit = 0
        while (value >= 1024 && unit < units.lastIndex) {
            value /= 1024
            unit++
        }
        return if (value < 10) "%.1f %s".format(Locale.US, value, units[unit]) else "%.0f %s".format(Locale.US, value, units[unit])
    }

    fun formatDate(millis: Long): String =
        if (millis <= 0L) "" else SimpleDateFormat("d MMM yyyy, HH:mm", Locale.US).format(Date(millis))

    fun detail(item: FileItem): String = listOf(
        if (item.isFolder) "Folder" else formatSize(item.size),
        formatDate(item.modified),
    ).filter { it.isNotEmpty() }.joinToString(" · ")

    fun kindLabel(item: FileItem): String = when (item.kind) {
        FileKind.Folder -> "Folder"
        FileKind.Text -> "Text"
        FileKind.Image -> "Image"
        FileKind.Pdf -> "PDF document"
        FileKind.Other -> extension(item.name).uppercase().ifEmpty { "File" }.let { if (it == "File") it else "$it file" }
    }

    fun deletePrompt(item: FileItem): String = if (item.isFolder) "Delete ${item.name} and everything in it?" else "Delete ${item.name}?"

    fun smbUrl(host: String, share: String, path: String, port: Int = 445): String {
        val hostPart = if (host.contains(':') && !host.startsWith("[")) "[$host]" else host
        val portPart = if (port != 445) ":$port" else ""
        val rest = parts(path).joinToString("/")
        return "smb://$hostPart$portPart/$share/" + rest
    }

    fun parseAddress(text: String): SmbAddress {
        var rest = text.trim()
        if (rest.isEmpty()) return SmbAddress("", 445, "", "", "Enter the file server's address, for example 192.168.1.1.")
        if (rest.lowercase().startsWith("smb://")) rest = rest.substring(6)
        rest = rest.replace('\\', '/').trimStart('/')
        val slash = rest.indexOf('/')
        val authority = if (slash < 0) rest else rest.substring(0, slash)
        val tail = if (slash < 0) "" else rest.substring(slash + 1)
        val at = authority.lastIndexOf('@')
        val hostPort = if (at >= 0) authority.substring(at + 1) else authority
        var host = hostPort
        var port = 445
        if (hostPort.startsWith("[")) {
            val close = hostPort.indexOf(']')
            if (close < 0) return SmbAddress("", 445, "", "", "\"${text.trim()}\" is not a file server address.")
            host = hostPort.substring(1, close)
            val after = hostPort.substring(close + 1)
            if (after.startsWith(":")) port = after.substring(1).toIntOrNull() ?: -1
        } else if (hostPort.count { it == ':' } == 1) {
            host = hostPort.substringBefore(':')
            port = hostPort.substringAfter(':').toIntOrNull() ?: -1
        }
        if (host.isEmpty() || port !in 1..65535 || host.any { it.isWhitespace() }) {
            return SmbAddress("", 445, "", "", "\"${text.trim()}\" is not a file server address.")
        }
        val tailParts = parts(tail)
        return SmbAddress(host, port, tailParts.firstOrNull() ?: "", tailParts.drop(1).joinToString("/"))
    }

    fun pump(input: InputStream, out: OutputStream, total: Long, progress: (Long, Long) -> Boolean): Long {
        val buffer = ByteArray(64 * 1024)
        var done = 0L
        while (true) {
            val n = input.read(buffer)
            if (n < 0) break
            out.write(buffer, 0, n)
            done += n
            if (!progress(done, total)) throw FileProblem(SmbStatus.message(SmbStatus.CANCELLED))
        }
        return done
    }
}
