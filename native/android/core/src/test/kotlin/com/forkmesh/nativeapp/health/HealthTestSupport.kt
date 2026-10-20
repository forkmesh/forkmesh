package com.forkmesh.nativeapp.health

import com.forkmesh.nativeapp.api.PrefStore
import com.forkmesh.nativeapp.model.OrgApiException

class HealthMemoryPrefs : PrefStore {
    val values = HashMap<String, Any>()
    override fun getString(key: String): String? = values[key] as? String
    override fun putString(key: String, value: String) { values[key] = value }
    override fun getBool(key: String): Boolean? = values[key] as? Boolean
    override fun putBool(key: String, value: Boolean) { values[key] = value }
    override fun getDouble(key: String): Double? = values[key] as? Double
    override fun putDouble(key: String, value: Double) { values[key] = value }
    override fun contains(key: String): Boolean = key in values
    override fun remove(key: String) { values.remove(key) }
}

class FakeHealthApi {
    class Call(val method: String, val suffix: String, val body: HealthJson?)

    val calls = ArrayList<Call>()
    val replies = HashMap<String, HealthJson>()
    val failures = HashMap<String, OrgApiException>()

    val call = HealthCall { method, suffix, body ->
        calls += Call(method, suffix, body)
        failures["$method $suffix"]?.let { throw it }
        replies["$method $suffix"] ?: replies[method] ?: mapOf("ok" to true)
    }

    fun bodies(method: String, suffix: String): List<HealthJson> =
        calls.filter { it.method == method && it.suffix == suffix }.mapNotNull { it.body }
}
