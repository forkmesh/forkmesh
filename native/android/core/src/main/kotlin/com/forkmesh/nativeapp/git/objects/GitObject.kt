package com.forkmesh.nativeapp.git.objects

import com.forkmesh.nativeapp.git.bytes.DartFormatException
import com.forkmesh.nativeapp.git.bytes.GitBytesBuilder
import com.forkmesh.nativeapp.git.bytes.asciiDecode
import com.forkmesh.nativeapp.git.bytes.asciiEncode
import com.forkmesh.nativeapp.git.bytes.constantTimeEquals
import com.forkmesh.nativeapp.git.bytes.decodeLowerHex
import com.forkmesh.nativeapp.git.bytes.encodeLowerHex
import com.forkmesh.nativeapp.git.bytes.indexOfByte
import com.forkmesh.nativeapp.git.crypto.ForkMeshSha1
import com.forkmesh.nativeapp.git.errors.GitErrorCode
import com.forkmesh.nativeapp.git.errors.GitException
import com.forkmesh.nativeapp.git.security.GitResourceLimits

@Suppress("EnumEntryName")
enum class GitObjectType { blob, tree, commit, tag }

class GitObjectId private constructor(private val idBytes: ByteArray) {
    @Volatile
    private var cachedHex: String? = null

    val bytes: ByteArray get() = idBytes.copyOf()

    val hex: String
        get() = cachedHex ?: encodeLowerHex(idBytes).also { cachedHex = it }

    internal val firstByte: Int get() = idBytes[0].toInt() and 0xff

    internal val rawBytes: ByteArray get() = idBytes

    internal fun compareHex(other: GitObjectId): Int {
        for (index in 0 until 20) {
            val left = idBytes[index].toInt() and 0xff
            val right = other.idBytes[index].toInt() and 0xff
            if (left != right) return left - right
        }
        return 0
    }

    override fun equals(other: Any?): Boolean =
        other is GitObjectId && constantTimeEquals(idBytes, other.idBytes)

    override fun hashCode(): Int = idBytes.contentHashCode()

    override fun toString(): String = hex

    companion object {
        fun parseSha1(value: String): GitObjectId {
            val bytes = decodeLowerHex(value)
            if (bytes.size != 20) {
                throw GitException(GitErrorCode.invalidObject, "expected SHA-1 object ID")
            }
            return GitObjectId(bytes)
        }

        fun fromSha1Bytes(value: ByteArray): GitObjectId {
            if (value.size != 20) {
                throw GitException(GitErrorCode.invalidObject, "expected SHA-1 object ID")
            }
            return GitObjectId(value.copyOf())
        }

        internal fun fromSha1Bytes(value: ByteArray, offset: Int): GitObjectId =
            GitObjectId(value.copyOfRange(offset, offset + 20))

        fun sha1Of(canonicalBytes: ByteArray): GitObjectId {
            val digest = ForkMeshSha1()
            digest.add(canonicalBytes)
            return GitObjectId(digest.close())
        }
    }
}

class GitObjectFrame(val type: GitObjectType, val body: ByteArray) {
    companion object {
        private val canonicalSize = Regex("0|[1-9][0-9]*")

        fun encode(type: GitObjectType, body: ByteArray): ByteArray {
            val header = asciiEncode("${type.name} ${body.size}\u0000")
            val output = GitBytesBuilder(header.size + body.size)
            output.add(header)
            output.add(body)
            return output.takeBytes()
        }

        fun decode(bytes: ByteArray, limits: GitResourceLimits): GitObjectFrame {
            val separator = bytes.indexOfByte(0)
            if (separator <= 0) {
                throw GitException(GitErrorCode.invalidObject, "object header is missing NUL")
            }
            val header = try {
                asciiDecode(bytes, 0, separator)
            } catch (error: DartFormatException) {
                throw GitException(GitErrorCode.invalidObject, "object header is not ASCII")
            }
            val separatorIndex = header.indexOf(' ')
            if (separatorIndex <= 0 || separatorIndex != header.lastIndexOf(' ')) {
                throw GitException(GitErrorCode.invalidObject, "object header is not canonical")
            }
            val typeText = header.substring(0, separatorIndex)
            val sizeText = header.substring(separatorIndex + 1)
            if (!canonicalSize.matches(sizeText)) {
                throw GitException(GitErrorCode.invalidObject, "object size is not canonical")
            }
            val declaredSize = sizeText.toLongOrNull() ?: Long.MAX_VALUE
            val actualSize = bytes.size - separator - 1
            if (declaredSize > limits.maxObjectBytes || actualSize > limits.maxObjectBytes) {
                throw GitException(GitErrorCode.resourceLimitExceeded, "object exceeds configured limit")
            }
            if (declaredSize != actualSize.toLong()) {
                throw GitException(GitErrorCode.invalidObject, "object body length does not match header")
            }
            val type = when (typeText) {
                "blob" -> GitObjectType.blob
                "tree" -> GitObjectType.tree
                "commit" -> GitObjectType.commit
                "tag" -> GitObjectType.tag
                else -> throw GitException(GitErrorCode.invalidObject, "unsupported object type")
            }
            return GitObjectFrame(type, bytes.copyOfRange(separator + 1, bytes.size))
        }
    }
}
