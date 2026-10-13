package com.forkmesh.nativeapp.platform

import android.content.Context
import android.content.SharedPreferences

class Prefs(context: Context, name: String = "forkmesh") {
    private val prefs: SharedPreferences =
        context.applicationContext.getSharedPreferences(name, Context.MODE_PRIVATE)

    fun getString(key: String): String? = prefs.getString(key, null)

    fun getString(key: String, fallback: String): String = prefs.getString(key, fallback) ?: fallback

    fun putString(key: String, value: String?) {
        if (value == null) prefs.edit().remove(key).apply() else prefs.edit().putString(key, value).apply()
    }

    fun getBool(key: String): Boolean? = if (prefs.contains(key)) prefs.getBoolean(key, false) else null

    fun getBool(key: String, fallback: Boolean): Boolean = prefs.getBoolean(key, fallback)

    fun putBool(key: String, value: Boolean) = prefs.edit().putBoolean(key, value).apply()

    fun getLong(key: String, fallback: Long = 0L): Long = when (val raw = prefs.all[key]) {
        is Long -> raw
        is Int -> raw.toLong()
        else -> fallback
    }

    fun putLong(key: String, value: Long) = prefs.edit().putLong(key, value).apply()

    fun getInt(key: String, fallback: Int = 0): Int = when (val raw = prefs.all[key]) {
        is Int -> raw
        is Long -> raw.toInt()
        else -> fallback
    }

    fun putInt(key: String, value: Int) = prefs.edit().putInt(key, value).apply()

    fun getDouble(key: String, fallback: Double): Double =
        if (prefs.contains(key)) java.lang.Double.longBitsToDouble(prefs.getLong(key, 0L)) else fallback

    fun putDouble(key: String, value: Double) =
        prefs.edit().putLong(key, java.lang.Double.doubleToRawLongBits(value)).apply()

    fun getStringList(key: String): List<String>? {
        val raw = prefs.all[key] ?: return null
        if (raw is Set<*>) return raw.map { it.toString() }
        val text = raw as? String ?: return null
        if (!text.startsWith(LIST_MARKER)) return null
        val parsed = com.forkmesh.nativeapp.json.Json.parse(text.substring(LIST_MARKER.length)) as? List<*> ?: return null
        return parsed.map { it?.toString() ?: "" }
    }

    fun putStringList(key: String, value: List<String>) {
        prefs.edit().putString(key, LIST_MARKER + com.forkmesh.nativeapp.json.Json.stringify(value)).apply()
    }

    fun putStringListNow(key: String, value: List<String>): Boolean =
        prefs.edit().putString(key, LIST_MARKER + com.forkmesh.nativeapp.json.Json.stringify(value)).commit()

    fun raw(key: String): Any? = prefs.all[key]

    fun commitRaw(key: String, value: Any?): Boolean {
        val editor = prefs.edit()
        when (value) {
            null -> editor.remove(key)
            is String -> editor.putString(key, value)
            is Boolean -> editor.putBoolean(key, value)
            is Long -> editor.putLong(key, value)
            is Int -> editor.putInt(key, value)
            is Float -> editor.putFloat(key, value)
            else -> return false
        }
        return editor.commit()
    }

    fun contains(key: String): Boolean = prefs.contains(key)

    fun remove(key: String) = prefs.edit().remove(key).apply()

    fun removeNow(key: String) {
        prefs.edit().remove(key).commit()
    }

    fun keys(): Set<String> = prefs.all.keys

    companion object {
        const val LIST_MARKER = "fm-list:"
    }

    fun clear() {
        prefs.edit().clear().commit()
    }
}
