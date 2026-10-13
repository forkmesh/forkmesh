package com.forkmesh.nativeapp.api

object EventSigning {
    fun repoKey(owner: String, name: String): String {
        val o = owner.trim().lowercase()
        val n = name.trim().lowercase()
        if (o.isEmpty() || n.isEmpty()) return ""
        if (listOf(o, n).any { '/' in it || '\n' in it || '\u0000' in it }) return ""
        return "$o/$n"
    }

    fun prefix(kind: String, owner: String, name: String): String {
        val key = repoKey(owner, name)
        return if (key.isEmpty()) "forkmesh-$kind-v1\n" else "forkmesh-$kind-v2\n$key\n"
    }
}
