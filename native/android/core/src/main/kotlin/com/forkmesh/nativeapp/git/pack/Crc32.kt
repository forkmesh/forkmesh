package com.forkmesh.nativeapp.git.pack

import java.util.zip.CRC32

internal fun crc32(bytes: ByteArray, start: Int = 0, end: Int = bytes.size): Long {
    val crc = CRC32()
    crc.update(bytes, start, end - start)
    return crc.value
}
