package com.forkmesh.nativeapp.api

import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.net.HttpClient
import com.forkmesh.nativeapp.security.DeviceIdentity
import java.net.URI
import java.net.URLDecoder
import java.security.KeyFactory
import java.security.KeyPairGenerator
import java.security.PrivateKey
import java.security.Signature
import java.security.spec.X509EncodedKeySpec
import java.util.Base64
import java.util.concurrent.CopyOnWriteArrayList

class FakeTransport(var handler: (HttpClient.Request) -> HttpClient.Response) : HttpTransport {
    val requests = CopyOnWriteArrayList<HttpClient.Request>()

    override fun send(request: HttpClient.Request): HttpClient.Response {
        requests.add(request)
        return handler(request)
    }
}

fun jsonResponse(body: Any?, status: Int = 200): HttpClient.Response =
    HttpClient.Response(status, mapOf("Content-Type" to listOf("application/json")), Json.stringify(body).toByteArray())

fun textResponse(text: String, status: Int = 200): HttpClient.Response =
    HttpClient.Response(status, emptyMap(), text.toByteArray())

fun HttpClient.Request.bodyText(): String = body?.let { String(it, Charsets.UTF_8) } ?: ""

@Suppress("UNCHECKED_CAST")
fun HttpClient.Request.bodyJson(): Map<String, Any?> = Json.parse(bodyText()) as Map<String, Any?>

fun HttpClient.Request.path(): String = URI(url).rawPath

fun HttpClient.Request.query(): Map<String, String> {
    val raw = URI(url).rawQuery ?: return emptyMap()
    return raw.split('&').filter { it.isNotEmpty() }.associate { pair ->
        val key = pair.substringBefore('=')
        val value = pair.substringAfter('=', "")
        URLDecoder.decode(key, "UTF-8") to URLDecoder.decode(value, "UTF-8")
    }
}

fun HttpClient.Request.headerValue(name: String): String? =
    headers.entries.firstOrNull { it.key.equals(name, ignoreCase = true) }?.value

class TestSettings(
    override var serverUrl: String = "ws://test.local/ws",
    override var room: String = "world-general",
) : ClientSettings

class TestIdentity : DeviceIdentity {
    private var privateKey: PrivateKey
    private var publicRaw: ByteArray
    var rotations = 0
        private set

    init {
        val pair = KeyPairGenerator.getInstance("Ed25519").generateKeyPair()
        privateKey = pair.private
        publicRaw = pair.public.encoded.takeLast(32).toByteArray()
    }

    override val publicKeyBytes: ByteArray get() = publicRaw.copyOf()

    override val publicKeyB64url: String get() = b64url(publicRaw)

    override val nodeId: String get() = publicKeyB64url

    override val shortKey: String
        get() {
            val k = publicKeyB64url
            return if (k.length <= 16) k else k.substring(0, 8) + "..." + k.substring(k.length - 8)
        }

    override fun sign(data: ByteArray): String {
        val signer = Signature.getInstance("Ed25519")
        signer.initSign(privateKey)
        signer.update(data)
        return b64url(signer.sign())
    }

    override fun exportSeed(): ByteArray = privateKey.encoded.takeLast(32).toByteArray()

    override fun rotate() {
        val pair = KeyPairGenerator.getInstance("Ed25519").generateKeyPair()
        privateKey = pair.private
        publicRaw = pair.public.encoded.takeLast(32).toByteArray()
        rotations++
    }

    companion object {
        private val SPKI_PREFIX = byteArrayOf(0x30, 0x2a, 0x30, 0x05, 0x06, 0x03, 0x2b, 0x65, 0x70, 0x03, 0x21, 0x00)

        fun b64url(bytes: ByteArray): String = Base64.getUrlEncoder().withoutPadding().encodeToString(bytes)

        fun verify(data: ByteArray, signatureB64url: String, publicKeyB64url: String): Boolean {
            val raw = Base64.getUrlDecoder().decode(publicKeyB64url)
            val key = KeyFactory.getInstance("Ed25519").generatePublic(X509EncodedKeySpec(SPKI_PREFIX + raw))
            val verifier = Signature.getInstance("Ed25519")
            verifier.initVerify(key)
            verifier.update(data)
            return verifier.verify(Base64.getUrlDecoder().decode(signatureB64url))
        }
    }
}
