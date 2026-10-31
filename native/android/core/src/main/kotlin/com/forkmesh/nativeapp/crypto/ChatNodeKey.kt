package com.forkmesh.nativeapp.crypto

import com.forkmesh.nativeapp.security.DeviceIdentity

class ChatNodeKey(val node: String, private val signer: (ByteArray) -> ByteArray) {
    fun sign(message: ByteArray): ByteArray = signer(message)

    companion object {
        fun fromSeed(seed: ByteArray): ChatNodeKey {
            val copy = seed.copyOf()
            val publicKey = Ed25519.publicKey(copy)
            return ChatNodeKey(ChatV3.b64u(publicKey)) { Ed25519.sign(copy, it, publicKey) }
        }

        fun generate(): ChatNodeKey = fromSeed(Ed25519.generateSeed())

        fun of(identity: DeviceIdentity): ChatNodeKey =
            ChatNodeKey(identity.nodeId) { ChatV3.unb64u(identity.sign(it)) ?: throw IllegalStateException("bad node signature") }
    }
}
