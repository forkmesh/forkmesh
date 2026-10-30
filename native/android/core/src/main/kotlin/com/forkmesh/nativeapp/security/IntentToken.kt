package com.forkmesh.nativeapp.security

import com.forkmesh.nativeapp.api.PrefStore
import java.security.MessageDigest
import java.security.SecureRandom
import java.util.Base64

class IntentToken(private val store: PrefStore) {
    companion object {
        const val EXTRA = "com.forkmesh.intent.TOKEN"
        const val PREFS_KEY = "intents/token"
        const val BYTES = 32
        private const val MIN_CHARS = 43
    }

    @Volatile
    private var cached: String? = null

    fun value(): String {
        cached?.let { return it }
        return synchronized(this) {
            cached ?: run {
                val stored = store.getString(PREFS_KEY)?.takeIf { it.length >= MIN_CHARS }
                val token = stored ?: generate().also { store.putString(PREFS_KEY, it) }
                cached = token
                token
            }
        }
    }

    fun accepts(candidate: String?): Boolean {
        if (candidate.isNullOrEmpty()) return false
        return MessageDigest.isEqual(candidate.toByteArray(Charsets.UTF_8), value().toByteArray(Charsets.UTF_8))
    }

    private fun generate(): String {
        val bytes = ByteArray(BYTES)
        SecureRandom().nextBytes(bytes)
        return Base64.getUrlEncoder().withoutPadding().encodeToString(bytes)
    }
}
