package com.forkmesh.nativeapp.security

interface SecretStore {
    fun read(key: String): String?

    fun write(key: String, value: String)

    fun delete(key: String)
}

class MemorySecretStore(seed: Map<String, String> = emptyMap()) : SecretStore {
    private val values = HashMap(seed)

    @Synchronized
    override fun read(key: String): String? = values[key]

    @Synchronized
    override fun write(key: String, value: String) {
        values[key] = value
    }

    @Synchronized
    override fun delete(key: String) {
        values.remove(key)
    }

    @Synchronized
    fun snapshot(): Map<String, String> = HashMap(values)
}

interface DeviceIdentity {
    val publicKeyBytes: ByteArray

    val publicKeyB64url: String

    val nodeId: String

    val shortKey: String

    fun sign(data: ByteArray): String

    fun exportSeed(): ByteArray

    fun rotate()
}

interface OwnerKeys {
    val keyId: String

    val publicBundle: Map<String, Any?>

    fun isRecipientOf(envelope: Map<String, Any?>): Boolean

    fun open(envelope: Map<String, Any?>): ByteArray?

    fun openJson(envelope: Map<String, Any?>): Map<String, Any?>?
}
