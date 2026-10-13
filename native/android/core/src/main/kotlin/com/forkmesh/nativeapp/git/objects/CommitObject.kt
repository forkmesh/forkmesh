package com.forkmesh.nativeapp.git.objects

import com.forkmesh.nativeapp.git.bytes.dartTrim
import com.forkmesh.nativeapp.git.errors.GitErrorCode
import com.forkmesh.nativeapp.git.errors.GitException
import java.time.Instant

class GitCommitIdentity(
    val name: String,
    val email: String,
    val timestampSeconds: Long,
    val timezoneOffsetMinutes: Int,
) {
    val timeUtc: Instant get() = Instant.ofEpochMilli(timestampSeconds * 1000)

    companion object {
        private val pattern = Regex(
            "([^\\n\\r\\u2028\\u2029]*?)$JS_SPACE*<([^<>\\n]*)>$JS_SPACE([0-9]{1,19})$JS_SPACE([+-][0-9]{4})",
        )

        fun tryParse(value: String): GitCommitIdentity? {
            val match = pattern.matchEntire(value) ?: return null
            val seconds = match.groupValues[3].toLongOrNull() ?: return null
            val offsetText = match.groupValues[4]
            val offsetHours = offsetText.substring(1, 3).toInt()
            val offsetMinutes = offsetText.substring(3, 5).toInt()
            if (offsetMinutes > 59) return null
            val magnitude = offsetHours * 60 + offsetMinutes
            return GitCommitIdentity(
                name = match.groupValues[1].dartTrim(),
                email = match.groupValues[2],
                timestampSeconds = seconds,
                timezoneOffsetMinutes = if (offsetText.startsWith("-")) -magnitude else magnitude,
            )
        }

        fun parse(value: String): GitCommitIdentity =
            tryParse(value) ?: throw GitException(GitErrorCode.invalidObject, "commit identity header is malformed")
    }
}

internal const val JS_SPACE =
    "[\\t\\n\\u000B\\f\\r \\u00A0\\u1680\\u2000-\\u200A\\u2028\\u2029\\u202F\\u205F\\u3000\\uFEFF]"

class GitCommit private constructor(
    val tree: GitObjectId,
    val parents: List<GitObjectId>,
    val headers: Map<String, List<String>>,
    private val messageBytes: ByteArray,
) {
    val message: ByteArray get() = messageBytes.copyOf()

    val author: GitCommitIdentity? get() = GitCommitIdentity.tryParse(headers.getValue("author").single())

    val committer: GitCommitIdentity? get() = GitCommitIdentity.tryParse(headers.getValue("committer").single())

    companion object {
        fun parse(body: ByteArray): GitCommit {
            val bytes = body
            val separator = findHeaderBodySeparator(bytes)
            val headerBytes = if (separator == -1) bytes else bytes.copyOfRange(0, separator)
            val message = if (separator == -1) ByteArray(0) else bytes.copyOfRange(separator + 2, bytes.size)
            val headerText = decodeUtf8Headers(headerBytes)
            val headers = LinkedHashMap<String, MutableList<String>>()
            var previousKey: String? = null
            for (line in headerText.split("\n")) {
                if (line.startsWith(" ")) {
                    val key = previousKey
                        ?: throw GitException(GitErrorCode.invalidObject, "orphan header continuation")
                    val values = headers.getValue(key)
                    values[values.size - 1] = values[values.size - 1] + "\n" + line.substring(1)
                    continue
                }
                val separatorIndex = line.indexOf(' ')
                if (separatorIndex <= 0) {
                    throw GitException(GitErrorCode.invalidObject, "commit header is invalid")
                }
                val key = line.substring(0, separatorIndex)
                val value = line.substring(separatorIndex + 1)
                headers.getOrPut(key) { ArrayList() }.add(value)
                previousKey = key
            }
            val treeValues = headers["tree"]
            if (treeValues == null || treeValues.size != 1) {
                throw GitException(GitErrorCode.invalidObject, "commit must have one tree")
            }
            if (headers["author"]?.size != 1 || headers["committer"]?.size != 1) {
                throw GitException(GitErrorCode.invalidObject, "commit identity headers are invalid")
            }
            val parents = (headers["parent"] ?: emptyList<String>()).map { GitObjectId.parseSha1(it) }
            val frozen = LinkedHashMap<String, List<String>>()
            for ((key, value) in headers) frozen[key] = value.toList()
            return GitCommit(
                tree = GitObjectId.parseSha1(treeValues.single()),
                parents = parents,
                headers = frozen,
                messageBytes = message,
            )
        }
    }
}
