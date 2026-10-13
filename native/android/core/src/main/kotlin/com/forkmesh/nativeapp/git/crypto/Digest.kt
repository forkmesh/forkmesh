package com.forkmesh.nativeapp.git.crypto

interface GitDigest {
    fun add(bytes: ByteArray)

    fun add(bytes: ByteArray, offset: Int, length: Int) = add(bytes.copyOfRange(offset, offset + length))

    fun close(): ByteArray
}

fun rotateLeft32(value: Long, bits: Int): Long =
    ((value shl bits) or ((value and 0xffffffffL) ushr (32 - bits))) and 0xffffffffL

fun rotateRight32(value: Long, bits: Int): Long =
    (((value and 0xffffffffL) ushr bits) or (value shl (32 - bits))) and 0xffffffffL
