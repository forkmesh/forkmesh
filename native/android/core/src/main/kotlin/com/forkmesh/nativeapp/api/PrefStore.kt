package com.forkmesh.nativeapp.api

interface PrefStore {
    fun getString(key: String): String?

    fun putString(key: String, value: String)

    fun getBool(key: String): Boolean?

    fun putBool(key: String, value: Boolean)

    fun getDouble(key: String): Double?

    fun putDouble(key: String, value: Double)

    fun contains(key: String): Boolean

    fun remove(key: String)
}

class MemoryPrefStore(seed: Map<String, Any> = emptyMap()) : PrefStore {
    private val values = HashMap<String, Any>(seed)

    @Synchronized
    override fun getString(key: String): String? = values[key] as? String

    @Synchronized
    override fun putString(key: String, value: String) {
        values[key] = value
    }

    @Synchronized
    override fun getBool(key: String): Boolean? = values[key] as? Boolean

    @Synchronized
    override fun putBool(key: String, value: Boolean) {
        values[key] = value
    }

    @Synchronized
    override fun getDouble(key: String): Double? = (values[key] as? Number)?.toDouble()

    @Synchronized
    override fun putDouble(key: String, value: Double) {
        values[key] = value
    }

    @Synchronized
    override fun contains(key: String): Boolean = values.containsKey(key)

    @Synchronized
    override fun remove(key: String) {
        values.remove(key)
    }

    @Synchronized
    fun snapshot(): Map<String, Any> = HashMap(values)
}
