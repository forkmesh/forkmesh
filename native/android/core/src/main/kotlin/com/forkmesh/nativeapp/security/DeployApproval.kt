package com.forkmesh.nativeapp.security

object DeployApproval {
    const val VERSION = "forkmesh-deploy-approval-v1"
    const val PULL = "pull"
    const val BRANCH = "branch"

    fun message(
        kind: String,
        owner: String,
        repo: String,
        target: String,
        baseOid: String,
        headOid: String,
        requestId: String,
        ts: Long,
    ): ByteArray =
        listOf(VERSION, kind, owner.lowercase(), repo, target, baseOid, headOid, requestId, ts.toString())
            .joinToString("\n")
            .toByteArray(Charsets.UTF_8)

    fun sign(
        identity: DeviceIdentity,
        kind: String,
        owner: String,
        repo: String,
        target: String,
        baseOid: String,
        headOid: String,
        requestId: String,
        ts: Long,
    ): Map<String, Any?> = linkedMapOf(
        "publicKey" to identity.publicKeyB64url,
        "ts" to ts,
        "signature" to identity.sign(message(kind, owner, repo, target, baseOid, headOid, requestId, ts)),
    )
}

class DeployApprover(
    private val identity: () -> DeviceIdentity?,
    private val clock: () -> Long = System::currentTimeMillis,
) {
    fun approve(
        kind: String,
        owner: String,
        repo: String,
        target: String,
        baseOid: String,
        headOid: String,
        requestId: String,
    ): Map<String, Any?>? = try {
        identity()?.let { DeployApproval.sign(it, kind, owner, repo, target, baseOid, headOid, requestId, clock()) }
    } catch (_: Exception) {
        null
    }
}
