package com.forkmesh.nativeapp.git.objects

import com.forkmesh.nativeapp.git.errors.GitErrorCode
import com.forkmesh.nativeapp.git.errors.GitException

class GitAnnotatedTag private constructor(
    val target: GitObjectId,
    val targetType: GitObjectType,
    val name: String,
    private val messageBytes: ByteArray,
) {
    val message: ByteArray get() = messageBytes.copyOf()

    companion object {
        private val forbiddenNameCharacters = Regex("[\\u0000\\r\\n]")

        fun parse(body: ByteArray): GitAnnotatedTag {
            val bytes = body
            val separator = findHeaderBodySeparator(bytes)
            val headerBytes = if (separator == -1) bytes else bytes.copyOfRange(0, separator)
            val message = if (separator == -1) ByteArray(0) else bytes.copyOfRange(separator + 2, bytes.size)
            val values = LinkedHashMap<String, String>()
            for (line in decodeUtf8Headers(headerBytes).split("\n")) {
                val index = line.indexOf(' ')
                if (index <= 0 || values.containsKey(line.substring(0, index))) {
                    throw GitException(GitErrorCode.invalidObject, "tag header is invalid")
                }
                values[line.substring(0, index)] = line.substring(index + 1)
            }
            val target = values["object"]
            val type = values["type"]
            val name = values["tag"]
            if (target == null ||
                type == null ||
                name == null ||
                name.isEmpty() ||
                forbiddenNameCharacters.containsMatchIn(name)
            ) {
                throw GitException(GitErrorCode.invalidObject, "tag headers are incomplete")
            }
            return GitAnnotatedTag(
                target = GitObjectId.parseSha1(target),
                targetType = when (type) {
                    "blob" -> GitObjectType.blob
                    "tree" -> GitObjectType.tree
                    "commit" -> GitObjectType.commit
                    "tag" -> GitObjectType.tag
                    else -> throw GitException(GitErrorCode.invalidObject, "tag target type is invalid")
                },
                name = name,
                messageBytes = message,
            )
        }
    }
}
