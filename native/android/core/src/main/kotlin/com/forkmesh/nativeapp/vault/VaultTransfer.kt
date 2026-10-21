package com.forkmesh.nativeapp.vault

import java.nio.ByteBuffer
import java.nio.charset.CharacterCodingException
import java.nio.charset.CodingErrorAction
import java.util.zip.CRC32
import java.util.zip.DataFormatException
import java.util.zip.Inflater

private fun String.trimSpaces(): String = trim { it.isWhitespace() && it != '\n' && it != '\r' }

data class VaultTransferLogin(
    val title: String = "",
    val url: String = "",
    val username: String = "",
    val password: String = "",
    val notes: String = "",
    val otpauth: String = "",
) {
    val host: String get() = VaultCrypto.loginHost(url)

    val displayTitle: String
        get() {
            val trimmed = title.trim()
            if (trimmed.isNotEmpty()) return trimmed
            if (host.isNotEmpty()) return host
            val user = username.trim()
            return user.ifEmpty { VaultTransfer.UNTITLED }
        }

    val fields: Map<String, Any?>
        get() {
            var body = notes.trim()
            val code = otpauth.trim()
            if (code.isNotEmpty() && !body.contains(code)) body = if (body.isEmpty()) code else "$body\n\n$code"
            return mapOf(
                "title" to displayTitle,
                "url" to url.trim(),
                "username" to username,
                "password" to password,
                "notes" to body,
            )
        }

    internal val identity: String
        get() {
            val user = username.trim().lowercase()
            val place = host.ifEmpty { displayTitle.lowercase() }
            return "$place\u0000$user\u0000$password"
        }

    companion object {
        fun of(item: VaultItem): VaultTransferLogin {
            val noteLines = ArrayList<String>()
            var code = ""
            for (line in item.notes.split("\n")) {
                val trimmed = line.trimSpaces()
                if (code.isEmpty() && trimmed.lowercase().startsWith("otpauth://")) code = trimmed else noteLines.add(line)
            }
            return VaultTransferLogin(item.title, item.url, item.username, item.password, noteLines.joinToString("\n").trim(), code)
        }
    }
}

enum class VaultTransferSource(val label: String) {
    APPLE_PASSWORDS("Apple Passwords"),
    DUCK_DUCK_GO("DuckDuckGo"),
    CHROME("Chrome"),
    FIREFOX("Firefox"),
    BITWARDEN("Bitwarden"),
    ONE_PASSWORD("1Password"),
    OTHER("a password manager"),
}

data class VaultCsvImport(val source: VaultTransferSource, val logins: List<VaultTransferLogin>, val skippedRows: Int)

data class VaultImportPlan(val fresh: List<VaultTransferLogin>, val duplicates: Int)

data class VaultImportReport(val added: Int, val duplicates: Int, val remaining: Int = 0, val stoppedBy: String = "")

data class VaultImportReview(
    val source: String,
    val logins: List<VaultTransferLogin>,
    val fresh: Int,
    val duplicates: Int,
    val skippedRows: Int,
) {
    val title: String get() = "Import from $source?"

    val message: String
        get() {
            val parts = arrayListOf("$fresh new login${if (fresh == 1) "" else "s"} will be encrypted into your vault.")
            if (duplicates > 0) parts.add("$duplicates already in the vault will be skipped.")
            if (skippedRows > 0) parts.add("$skippedRows row${if (skippedRows == 1) " has" else "s have"} no username or password and will be left out.")
            return parts.joinToString(" ")
        }

    val canImport: Boolean get() = fresh > 0
}

sealed class VaultImportStart {
    data class Review(val review: VaultImportReview) : VaultImportStart()
    data class Message(val text: String) : VaultImportStart()
}

object VaultTransfer {
    const val UNTITLED = "Imported login"
    const val EXPORT_FILE_NAME = "ForkMesh Passwords.csv"
    val EXPORT_HEADER = listOf("Title", "URL", "Username", "Password", "Notes", "OTPAuth")
    const val MAX_FILE_BYTES = 16 * 1024 * 1024

    private val COLUMNS: List<Pair<String, List<String>>> = listOf(
        "title" to listOf("title", "name", "account", "item name", "login name", "display name"),
        "url" to listOf("url", "login_uri", "uri", "website", "web site", "web address", "site", "hostname", "login url", "urls"),
        "username" to listOf("username", "login_username", "user name", "user", "login", "email", "email address", "user id"),
        "password" to listOf("password", "login_password", "pass"),
        "notes" to listOf("notes", "note", "comments", "comment", "extra"),
        "otpauth" to listOf("otpauth", "login_totp", "totp", "otp", "one-time password", "otp auth", "otp url"),
    )

    fun parseCsv(input: String): List<List<String>> {
        val text = input.removePrefix("﻿")
        val rows = ArrayList<List<String>>()
        var row = ArrayList<String>()
        val field = StringBuilder()
        var quoted = false
        var atFieldStart = true
        fun endField() {
            row.add(field.toString())
            field.setLength(0)
            atFieldStart = true
        }
        fun endRow() {
            endField()
            if (!(row.size == 1 && row[0].isEmpty())) rows.add(row)
            row = ArrayList()
        }
        var i = 0
        while (i < text.length) {
            val c = text[i]
            if (quoted) {
                if (c == '"') {
                    if (i + 1 < text.length && text[i + 1] == '"') {
                        field.append('"')
                        i++
                    } else {
                        quoted = false
                    }
                } else {
                    field.append(c)
                }
            } else if (c == '"' && atFieldStart) {
                quoted = true
                atFieldStart = false
            } else if (c == ',') {
                endField()
            } else if (c == '\r') {
                if (i + 1 < text.length && text[i + 1] == '\n') i++
                endRow()
            } else if (c == '\n') {
                endRow()
            } else {
                field.append(c)
                atFieldStart = false
            }
            i++
        }
        if (field.isNotEmpty() || row.isNotEmpty() || quoted) endRow()
        return rows
    }

    private fun normalized(header: String): String = header.trim().lowercase()

    fun source(header: List<String>): VaultTransferSource {
        val names = header.map(::normalized).toSet()
        if ("login_uri" in names || "login_password" in names) return VaultTransferSource.BITWARDEN
        if ("httprealm" in names || "formactionorigin" in names || "guid" in names) return VaultTransferSource.FIREFOX
        if ("archived" in names || "one-time password" in names) return VaultTransferSource.ONE_PASSWORD
        if ("otpauth" in names) return VaultTransferSource.APPLE_PASSWORDS
        if (names.containsAll(listOf("name", "url", "username", "password", "note"))) return VaultTransferSource.CHROME
        if (names.containsAll(listOf("title", "url", "username", "password", "notes"))) return VaultTransferSource.DUCK_DUCK_GO
        return VaultTransferSource.OTHER
    }

    fun readCsv(data: ByteArray): VaultCsvImport {
        if (data.size > MAX_FILE_BYTES) throw VaultException("csv_too_large")
        if (!VaultZip.isZip(data)) return readCsv(decode(data))
        val inner = VaultZip.firstCsv(data, MAX_FILE_BYTES)
        if (VaultZip.isZip(inner)) throw VaultException("zip_unreadable")
        return readCsv(decode(inner))
    }

    private fun decode(data: ByteArray): String = try {
        Charsets.UTF_8.newDecoder()
            .onMalformedInput(CodingErrorAction.REPORT)
            .onUnmappableCharacter(CodingErrorAction.REPORT)
            .decode(ByteBuffer.wrap(data))
            .toString()
    } catch (_: CharacterCodingException) {
        String(data, Charsets.ISO_8859_1)
    }

    fun readCsv(text: String): VaultCsvImport {
        val rows = parseCsv(text)
        val header = rows.firstOrNull() ?: throw VaultException("csv_empty")
        val names = header.map(::normalized)
        val index = HashMap<String, Int>()
        for ((field, aliases) in COLUMNS) {
            for (alias in aliases) {
                val at = names.indexOf(alias)
                if (at >= 0) {
                    index[field] = at
                    break
                }
            }
        }
        if (index["password"] == null) throw VaultException("csv_unknown_columns")
        val logins = ArrayList<VaultTransferLogin>()
        var skipped = 0
        for (row in rows.drop(1)) {
            fun value(field: String): String {
                val at = index[field] ?: return ""
                return row.getOrNull(at) ?: ""
            }
            val login = VaultTransferLogin(
                title = value("title").trim(),
                url = value("url").trim(),
                username = value("username"),
                password = value("password"),
                notes = value("notes"),
                otpauth = value("otpauth").trim(),
            )
            if (login.password.isEmpty() && login.username.trimSpaces().isEmpty()) {
                if (row.any { it.trimSpaces().isNotEmpty() }) skipped++
                continue
            }
            logins.add(login)
        }
        if (logins.isEmpty()) throw VaultException("csv_no_logins")
        return VaultCsvImport(source(header), logins, skipped)
    }

    fun writeCsv(logins: List<VaultTransferLogin>): String {
        val lines = arrayListOf(EXPORT_HEADER.joinToString(",", transform = ::quote))
        for (login in logins) {
            lines.add(listOf(login.title, login.url, login.username, login.password, login.notes, login.otpauth).joinToString(",", transform = ::quote))
        }
        return lines.joinToString("\r\n") + "\r\n"
    }

    internal fun quote(value: String): String {
        val needsQuotes = value.any { it == ',' || it == '"' || it == '\n' || it == '\r' } || value.startsWith(" ") || value.endsWith(" ")
        return if (needsQuotes) "\"" + value.replace("\"", "\"\"") + "\"" else value
    }

    fun logins(items: List<VaultItem>): List<VaultTransferLogin> =
        items.filter { it.readable && !(it.password.isEmpty() && it.username.isEmpty()) }
            .map(VaultTransferLogin::of)
            .sortedWith(compareBy<VaultTransferLogin> { it.displayTitle.lowercase() }.thenBy { it.username.lowercase() })

    fun plan(incoming: List<VaultTransferLogin>, existing: List<VaultItem>): VaultImportPlan {
        val seen = existing.filter { it.readable }.map { VaultTransferLogin.of(it).identity }.toHashSet()
        val fresh = ArrayList<VaultTransferLogin>()
        var duplicates = 0
        for (login in incoming) {
            if (seen.add(login.identity)) fresh.add(login) else duplicates++
        }
        return VaultImportPlan(fresh, duplicates)
    }

    fun prepare(source: String, logins: List<VaultTransferLogin>, existing: List<VaultItem>, skippedRows: Int = 0): VaultImportStart {
        val plan = plan(logins, existing)
        if (plan.fresh.isNotEmpty()) {
            return VaultImportStart.Review(VaultImportReview(source, logins, plan.fresh.size, plan.duplicates, skippedRows))
        }
        return VaultImportStart.Message(
            if (plan.duplicates > 0) "Every login from $source is already in your vault." else "${source.replaceFirstChar { it.uppercase() }} had no logins to import.",
        )
    }

    fun prepareFile(data: ByteArray, existing: List<VaultItem>): VaultImportStart = try {
        val read = readCsv(data)
        prepare(read.source.label, read.logins, existing, read.skippedRows)
    } catch (error: Exception) {
        VaultImportStart.Message(errorText(error))
    }

    fun reportText(report: VaultImportReport, source: String): String {
        val parts = arrayListOf("Imported ${report.added} login${if (report.added == 1) "" else "s"} from $source.")
        if (report.duplicates > 0) parts.add("Skipped ${report.duplicates} already in the vault.")
        if (report.stoppedBy.isNotEmpty()) parts.add("Stopped with ${report.remaining} left: ${report.stoppedBy}")
        return parts.joinToString(" ")
    }

    fun savedText(count: Int, name: String): String =
        "$count login${if (count == 1) "" else "s"} saved to $name. Delete the file once the other app has imported it."

    private val WORDS = mapOf(
        "csv_too_large" to "That file is too large to be a password export.",
        "csv_unreadable" to "That file is not a text CSV file.",
        "csv_empty" to "That file is empty.",
        "csv_unknown_columns" to "That file has no Password column. Export a CSV file from your password manager and try again.",
        "csv_no_logins" to "That file has no logins in it.",
        "zip_unreadable" to "That ZIP file is damaged or uses a format this app cannot open.",
        "zip_encrypted" to "That ZIP file is password-protected. Export it again without a password.",
        "zip_no_csv" to "That ZIP file has no CSV file of passwords in it.",
    )

    const val FILE_UNREADABLE = "That file could not be opened."
    const val NOTHING_TO_EXPORT = "There are no logins to export yet."

    fun errorText(error: Throwable): String {
        if (error is VaultException) WORDS[error.code]?.let { return it }
        return VaultScreenLogic.errorText(error)
    }
}

object VaultZip {
    fun isZip(data: ByteArray): Boolean =
        data.size >= 4 && data[0] == 0x50.toByte() && data[1] == 0x4b.toByte() && data[2] == 0x03.toByte() && data[3] == 0x04.toByte()

    fun firstCsv(bytes: ByteArray, maximumBytes: Int): ByteArray {
        if (bytes.size < 22) throw VaultException("zip_unreadable")
        var eocd = -1
        var index = bytes.size - 22
        val floor = maxOf(0, bytes.size - 22 - 0xFFFF)
        while (index >= floor) {
            if (u32(bytes, index) == 0x06054b50L) {
                eocd = index
                break
            }
            index--
        }
        if (eocd < 0) throw VaultException("zip_unreadable")
        val count = u16(bytes, eocd + 10)
        var cursor = u32(bytes, eocd + 16).toInt()
        repeat(count) {
            if (cursor < 0 || cursor + 46 > bytes.size || u32(bytes, cursor) != 0x02014b50L) throw VaultException("zip_unreadable")
            val flags = u16(bytes, cursor + 8)
            val method = u16(bytes, cursor + 10)
            val crc = u32(bytes, cursor + 16)
            val compressed = u32(bytes, cursor + 20)
            val size = u32(bytes, cursor + 24)
            val nameLength = u16(bytes, cursor + 28)
            val extraLength = u16(bytes, cursor + 30)
            val commentLength = u16(bytes, cursor + 32)
            val local = u32(bytes, cursor + 42)
            val nameStart = cursor + 46
            if (nameStart + nameLength > bytes.size) throw VaultException("zip_unreadable")
            val name = String(bytes, nameStart, nameLength, Charsets.UTF_8)
            cursor = nameStart + nameLength + extraLength + commentLength
            val leaf = name.split("/").lastOrNull { it.isNotEmpty() } ?: ""
            if (!name.lowercase().endsWith(".csv") || name.startsWith("__MACOSX/") || leaf.startsWith(".")) return@repeat
            if (flags and 0x1 != 0) throw VaultException("zip_encrypted")
            if (size > maximumBytes) throw VaultException("csv_too_large")
            if (local + 30 > bytes.size || u32(bytes, local.toInt()) != 0x04034b50L) throw VaultException("zip_unreadable")
            val at = local.toInt()
            val start = at + 30 + u16(bytes, at + 26) + u16(bytes, at + 28)
            val end = start.toLong() + compressed
            if (end > bytes.size) throw VaultException("zip_unreadable")
            val output = when (method) {
                0 -> bytes.copyOfRange(start, end.toInt())
                8 -> inflateRaw(bytes, start, end.toInt(), maximumBytes)
                else -> throw VaultException("zip_unreadable")
            }
            val check = CRC32().apply { update(output) }.value
            if (output.size.toLong() != size || check != crc) throw VaultException("zip_unreadable")
            return output
        }
        throw VaultException("zip_no_csv")
    }

    private fun inflateRaw(input: ByteArray, start: Int, end: Int, maximumOutput: Int): ByteArray {
        val inflater = Inflater(true)
        try {
            inflater.setInput(input, start, end - start)
            val output = java.io.ByteArrayOutputStream()
            val chunk = ByteArray(64 * 1024)
            while (!inflater.finished()) {
                val produced = try {
                    inflater.inflate(chunk)
                } catch (_: DataFormatException) {
                    throw VaultException("zip_unreadable")
                }
                if (output.size() + produced > maximumOutput) throw VaultException("csv_too_large")
                output.write(chunk, 0, produced)
                if (produced == 0 && (inflater.needsInput() || inflater.needsDictionary())) throw VaultException("zip_unreadable")
            }
            return output.toByteArray()
        } finally {
            inflater.end()
        }
    }

    private fun u16(bytes: ByteArray, at: Int): Int {
        if (at < 0 || at + 2 > bytes.size) return 0
        return (bytes[at].toInt() and 0xff) or ((bytes[at + 1].toInt() and 0xff) shl 8)
    }

    private fun u32(bytes: ByteArray, at: Int): Long {
        if (at < 0 || at + 4 > bytes.size) return 0
        return (bytes[at].toLong() and 0xff) or ((bytes[at + 1].toLong() and 0xff) shl 8) or
            ((bytes[at + 2].toLong() and 0xff) shl 16) or ((bytes[at + 3].toLong() and 0xff) shl 24)
    }
}
