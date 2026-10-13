package com.forkmesh.nativeapp.localrepos

import com.forkmesh.nativeapp.git.GitObjectId

fun gitBlobId(bytes: ByteArray): GitObjectId {
    val header = "blob ${bytes.size}".toByteArray(Charsets.US_ASCII)
    val framed = ByteArray(header.size + 1 + bytes.size)
    header.copyInto(framed)
    framed[header.size] = 0
    bytes.copyInto(framed, header.size + 1)
    return GitObjectId.sha1Of(framed)
}

private val SHA1_HEX = Regex("^[0-9a-f]{40}$")

fun servedBlobIdAgrees(served: String, computed: GitObjectId): Boolean {
    val claimed = served.trim().lowercase()
    if (claimed.length != 40 || !SHA1_HEX.matches(claimed)) return true
    return claimed == computed.hex.lowercase()
}
