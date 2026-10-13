package com.forkmesh.nativeapp.git.objects

import com.forkmesh.nativeapp.git.bytes.GitBytesBuilder
import com.forkmesh.nativeapp.git.bytes.zlibEncode
import com.forkmesh.nativeapp.git.crypto.GitSha1CollisionDetector
import com.forkmesh.nativeapp.git.crypto.GitSha1ObjectOrigin
import com.forkmesh.nativeapp.git.crypto.Sha1CollisionScreen
import com.forkmesh.nativeapp.git.errors.GitErrorCode
import com.forkmesh.nativeapp.git.errors.GitException
import com.forkmesh.nativeapp.git.security.GitCancellationToken
import com.forkmesh.nativeapp.git.security.GitResourceLimits
import com.forkmesh.nativeapp.git.worktree.createDirectories
import com.forkmesh.nativeapp.git.worktree.fileExists
import com.forkmesh.nativeapp.git.worktree.nowMicros
import com.forkmesh.nativeapp.git.worktree.renameFile
import com.forkmesh.nativeapp.git.worktree.writeBytesFlushed
import java.io.File
import java.io.IOException
import java.util.zip.DataFormatException
import java.util.zip.Inflater

class LooseObjectStore(
    val gitDirectory: File,
    val limits: GitResourceLimits,
    val detector: GitSha1CollisionDetector? = null,
) {
    fun fileFor(id: GitObjectId): File =
        File(File(File(gitDirectory, "objects"), id.hex.substring(0, 2)), id.hex.substring(2))

    fun write(type: GitObjectType, body: ByteArray): GitObjectId {
        if (body.size > limits.maxObjectBytes) {
            throw GitException(GitErrorCode.resourceLimitExceeded, "object exceeds configured limit")
        }
        val frame = GitObjectFrame.encode(type, body)
        val id = GitObjectId.sha1Of(frame)
        val destination = fileFor(id)
        createDirectories(destination.parentFile)
        if (fileExists(destination)) return id
        val temporary = File("${destination.path}.tmp-${nowMicros()}")
        try {
            writeBytesFlushed(temporary, zlibEncode(frame))
            try {
                renameFile(temporary, destination)
            } catch (error: IOException) {
                if (!fileExists(destination)) throw error
            }
        } finally {
            if (fileExists(temporary)) temporary.delete()
        }
        return id
    }

    fun read(
        expectedId: GitObjectId,
        origin: GitSha1ObjectOrigin = GitSha1ObjectOrigin.trustedLocal,
        cancellation: GitCancellationToken? = null,
    ): GitObjectFrame {
        val source = fileFor(expectedId)
        if (!fileExists(source)) {
            throw GitException(GitErrorCode.invalidObject, "object is absent")
        }
        val compressed = source.readBytes()
        val frameBytes = inflateBounded(compressed, limits.maxObjectBytes + 128)
        val frame = GitObjectFrame.decode(frameBytes, limits)
        val screening = Sha1CollisionScreen(detector = detector).screenFrame(
            frame = frameBytes,
            objectType = frame.type,
            declaredBodySize = frame.body.size,
            origin = origin,
            limits = limits,
            cancellation = cancellation,
        )
        if (!screening.approved) throw screening.toException()
        val actualId = GitObjectId.fromSha1Bytes(screening.digest)
        if (actualId != expectedId) {
            throw GitException(GitErrorCode.objectHashMismatch, "loose object path does not match its contents")
        }
        return frame
    }
}

private fun inflateBounded(compressed: ByteArray, maximumBytes: Int): ByteArray {
    val inflater = Inflater()
    try {
        inflater.setInput(compressed)
        val output = GitBytesBuilder(minOf(maximumBytes, maxOf(64, compressed.size * 4)))
        val buffer = ByteArray(64 * 1024)
        while (!inflater.finished()) {
            val count = try {
                inflater.inflate(buffer)
            } catch (error: DataFormatException) {
                throw GitException(GitErrorCode.invalidObject, "object zlib stream is invalid")
            }
            if (count == 0) {
                if (inflater.needsInput() || inflater.needsDictionary()) {
                    throw GitException(GitErrorCode.invalidObject, "object zlib stream is invalid")
                }
                continue
            }
            if (output.length.toLong() + count > maximumBytes) {
                throw GitException(GitErrorCode.resourceLimitExceeded, "inflated object exceeds configured limit")
            }
            output.add(buffer, 0, count)
        }
        if (inflater.remaining != 0) {
            throw GitException(GitErrorCode.invalidObject, "object zlib stream is invalid")
        }
        return output.takeBytes()
    } finally {
        inflater.end()
    }
}
