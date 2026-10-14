package com.forkmesh.nativeapp.market

import com.forkmesh.nativeapp.api.PrefStore

object PhonePackages {
    const val SIGN_IN = PackageGate.SIGN_IN
    const val SIGN_IN_KEY = "packages/sign-in/on"
    const val REMOVED_KEY = "packages/removed"

    fun isOn(id: String, prefs: PrefStore?, legacy: Boolean): Boolean? = when (id) {
        SIGN_IN -> prefs?.getBool(SIGN_IN_KEY) ?: true
        in PackageGate.ORGANIZATION_PACKAGES -> id !in removed(prefs)
        else -> null
    }

    fun set(id: String, on: Boolean, prefs: PrefStore?) {
        when (id) {
            SIGN_IN -> prefs?.putBool(SIGN_IN_KEY, on)
            in PackageGate.ORGANIZATION_PACKAGES -> setRemoved(id, !on, prefs)
        }
    }

    fun removed(prefs: PrefStore?): Set<String> =
        prefs?.getString(REMOVED_KEY).orEmpty().split(',').filter(PackagesText::isPackageId).toSet()

    fun setRemoved(id: String, removed: Boolean, prefs: PrefStore?) {
        val store = prefs ?: return
        val next = if (removed) removed(store) + id else removed(store) - id
        if (next.isEmpty()) store.remove(REMOVED_KEY) else store.putString(REMOVED_KEY, next.sorted().joinToString(","))
    }
}
