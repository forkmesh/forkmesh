package com.forkmesh.nativeapp.feature.account

import com.forkmesh.nativeapp.account.projects.FoldPrefs
import com.forkmesh.nativeapp.api.PrefStore
import com.forkmesh.nativeapp.platform.Prefs

class PrefsStore(private val prefs: Prefs) : PrefStore {
    override fun getString(key: String): String? = runCatching { prefs.getString(key) }.getOrNull()

    override fun putString(key: String, value: String) {
        prefs.putString(key, value)
    }

    override fun getBool(key: String): Boolean? = runCatching { prefs.getBool(key) }.getOrNull()

    override fun putBool(key: String, value: Boolean) {
        prefs.putBool(key, value)
    }

    override fun getDouble(key: String): Double? =
        if (prefs.contains(key)) runCatching { prefs.getDouble(key, 0.0) }.getOrNull() else null

    override fun putDouble(key: String, value: Double) {
        prefs.putDouble(key, value)
    }

    override fun contains(key: String): Boolean = prefs.contains(key)

    override fun remove(key: String) {
        prefs.remove(key)
    }
}

class FoldPrefsStore(private val prefs: Prefs) : FoldPrefs, PrefStore by PrefsStore(prefs) {
    override fun keys(): Set<String> = prefs.keys()

    override fun copyRaw(from: String, to: String): Boolean = prefs.commitRaw(to, prefs.raw(from))
}
