package com.forkmesh.nativeapp.git

import com.forkmesh.nativeapp.testing.assertBytes
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.assertFails

class GitPackIndexTest {
    private fun singleObjectIndex(id: GitObjectId): ByteArray {
        var body = byteArrayOf(0xff.toByte(), 0x74, 0x4f, 0x63) + uint32(2)
        val first = id.bytes.first().toInt() and 0xff
        for (index in 0 until 256) {
            body += uint32(if (index < first) 0 else 1)
        }
        body += id.bytes + uint32(0x12345678) + uint32(12)
        val withPackChecksum = body + ByteArray(20) { 0xaa.toByte() }
        return withSha1Trailer(withPackChecksum)
    }

    fun testParsesAnIndexV2FanoutLookupAndChecksums() {
        val id = GitObjectId.parseSha1("ce013625030ba8dba906f756967f9e9ca394464a")
        val index = GitPackIndexV2.parse(singleObjectIndex(id), GitResourceLimits())

        assertBytes(ByteArray(20) { 0xaa.toByte() }, index.packChecksum)
        assertEq(12L, index.lookup(id)!!.offset)
        assertEq(0x12345678L, index.lookup(id)!!.crc32)
    }

    fun testRejectsAnIndexChecksumMismatchAndANonmonotonicFanout() {
        val id = GitObjectId.parseSha1("ce013625030ba8dba906f756967f9e9ca394464a")
        val checksumChanged = singleObjectIndex(id).also { it[0] = (it[0].toInt() xor 1).toByte() }
        val fanoutChanged = singleObjectIndex(id).also { it[8 + 4] = 1 }

        assertFails<GitException> { GitPackIndexV2.parse(checksumChanged, GitResourceLimits()) }
        assertFails<GitException> { GitPackIndexV2.parse(fanoutChanged, GitResourceLimits()) }
    }
}
