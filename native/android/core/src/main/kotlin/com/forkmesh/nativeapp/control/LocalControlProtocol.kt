package com.forkmesh.nativeapp.control

import com.forkmesh.nativeapp.crypto.AesGcm
import com.forkmesh.nativeapp.crypto.B64
import com.forkmesh.nativeapp.crypto.Bytes
import com.forkmesh.nativeapp.crypto.Ed25519
import com.forkmesh.nativeapp.crypto.HmacSha256
import com.forkmesh.nativeapp.crypto.Sha256
import com.forkmesh.nativeapp.crypto.X25519
import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.json.asJsonMap
import com.forkmesh.nativeapp.json.jsonText
import com.forkmesh.nativeapp.security.DeviceIdentity
import java.io.ByteArrayOutputStream
import java.io.InputStream
import java.io.OutputStream
import java.net.InetSocketAddress
import java.net.Socket
import java.util.concurrent.CompletableFuture
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.CopyOnWriteArrayList
import java.util.concurrent.ExecutionException
import java.util.concurrent.TimeUnit
import java.util.concurrent.TimeoutException
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicInteger

const val CONTROL_PROTOCOL_VERSION = 1
const val CONTROL_HELLO_TAG = "fm-control-hello"
const val CONTROL_WELCOME_TAG = "fm-control-welcome"
const val CONTROL_AUTH_TAG = "fm-control-auth"
const val CONTROL_MAX_FRAME_BYTES = 4 * 1024 * 1024
const val CONTROL_FILE_CHUNK_BYTES = 256 * 1024
const val CONTROL_DEFAULT_PORT = 48732

private val LABEL = "forkmesh-local-control-v1".toByteArray(Charsets.UTF_8)

private fun utf8(text: String): ByteArray = text.toByteArray(Charsets.UTF_8)

fun controlTranscript(clientKey: String, clientEph: String, clientNonce: String, serverKey: String, serverEph: String): ByteArray =
    utf8("forkmesh-local-control-v1\n$clientKey\n$clientEph\n$clientNonce\n$serverKey\n$serverEph")

fun controlServerSignedBytes(transcript: ByteArray): ByteArray = Bytes.concat(LABEL, utf8(" server\n"), transcript)

fun controlClientSignedBytes(transcript: ByteArray): ByteArray = Bytes.concat(LABEL, utf8(" client\n"), transcript)

class ControlKeys(val clientToServer: ByteArray, val serverToClient: ByteArray)

fun deriveControlKeys(sharedSecret: ByteArray, transcript: ByteArray): ControlKeys {
    val salt = Sha256.hash(transcript)
    val prk = HmacSha256.mac(salt, sharedSecret)
    fun expand(direction: String) = HmacSha256.mac(prk, Bytes.concat(LABEL, utf8(" $direction"), byteArrayOf(1)))
    return ControlKeys(expand("c2s"), expand("s2c"))
}

fun controlPairingCode(transcript: ByteArray): String {
    val digest = Sha256.hash(Bytes.concat(LABEL, utf8(" pair\n"), transcript))
    val value = ((digest[0].toLong() and 0xff) shl 24) or ((digest[1].toLong() and 0xff) shl 16) or
        ((digest[2].toLong() and 0xff) shl 8) or (digest[3].toLong() and 0xff)
    return (value % 1_000_000).toString().padStart(6, '0')
}

internal fun controlNonce(counter: Long): ByteArray {
    val nonce = ByteArray(12)
    var value = counter
    for (i in 11 downTo 4) {
        nonce[i] = (value and 0xff).toByte()
        value = value shr 8
    }
    return nonce
}

fun sealControlFrame(key: ByteArray, counter: Long, plain: ByteArray): ByteArray {
    val box = AesGcm.encrypt(plain, key, controlNonce(counter), LABEL)
    return Bytes.concat(box.cipherText, box.mac)
}

fun openControlFrame(key: ByteArray, counter: Long, frame: ByteArray): ByteArray? {
    if (frame.size < 16) return null
    val plain = AesGcm.decryptOrNull(
        frame.copyOfRange(0, frame.size - 16),
        controlNonce(counter),
        frame.copyOfRange(frame.size - 16, frame.size),
        key,
        LABEL,
    ) ?: return null
    return if (plain.isEmpty()) null else plain
}

fun controlB64(bytes: ByteArray): String = B64.encodeUrlNoPadding(bytes)

private val B64URL = Regex("^[A-Za-z0-9_-]+$")

fun controlFromB64(text: Any?, length: Int): ByteArray? {
    if (text !is String || text.isEmpty() || text.length > 200 || !B64URL.matches(text)) return null
    val padded = text + "=".repeat((4 - text.length % 4) % 4)
    val bytes = B64.decodeOrNull(padded) ?: return null
    return if (bytes.size == length) bytes else null
}

fun controlFrame(payload: ByteArray): ByteArray {
    val out = ByteArray(payload.size + 4)
    val length = payload.size
    out[0] = (length shr 24).toByte()
    out[1] = (length shr 16).toByte()
    out[2] = (length shr 8).toByte()
    out[3] = length.toByte()
    System.arraycopy(payload, 0, out, 4, payload.size)
    return out
}

class LocalControlException(val code: String, val reason: String = "") : RuntimeException(code) {
    override fun toString(): String = "LocalControlException($code)"
}

class ControlFrameReader(private val input: InputStream) {
    fun next(): ByteArray? {
        val head = ByteArray(4)
        if (!readFully(head)) return null
        val length = ((head[0].toInt() and 0xff) shl 24) or ((head[1].toInt() and 0xff) shl 16) or
            ((head[2].toInt() and 0xff) shl 8) or (head[3].toInt() and 0xff)
        if (length <= 0 || length > CONTROL_MAX_FRAME_BYTES) throw LocalControlException("bad_frame")
        val payload = ByteArray(length)
        if (!readFully(payload)) return null
        return payload
    }

    private fun readFully(buffer: ByteArray): Boolean {
        var at = 0
        while (at < buffer.size) {
            val n = input.read(buffer, at, buffer.size - at)
            if (n < 0) return false
            at += n
        }
        return true
    }
}

data class ControlEvent(val event: String, val data: Map<String, Any?>)

interface ControlLink {
    val transport: ControlTransport
    val isOpen: Boolean
    val desktopName: String

    fun request(method: String, params: Map<String, Any?> = emptyMap(), timeoutMs: Long = 20_000): Map<String, Any?>

    fun addEventTap(tap: (ControlEvent) -> Unit): Any

    fun removeEventTap(token: Any)
}

enum class ControlTransport(val label: String) {
    Lan("On this network"),
    Relay("Through the relay"),
}

class ControlWelcome(val json: Map<String, Any?>)

internal class ControlHandshake(
    val identity: DeviceIdentity,
    val deviceName: String,
    val user: String,
    val platform: String,
) {
    val ephPrivate: ByteArray = X25519.generatePrivateKey()
    val ephB64: String = controlB64(X25519.publicKey(ephPrivate))
    val nonceB64: String = controlB64(Bytes.random(16))

    fun hello(): ByteArray {
        val hello = linkedMapOf<String, Any?>(
            "t" to CONTROL_HELLO_TAG,
            "v" to CONTROL_PROTOCOL_VERSION,
            "key" to identity.publicKeyB64url,
            "eph" to ephB64,
            "nonce" to nonceB64,
        )
        if (deviceName.isNotEmpty()) hello["name"] = deviceName
        if (user.isNotEmpty()) hello["user"] = user
        if (platform.isNotEmpty()) hello["platform"] = platform
        return utf8(Json.stringify(hello))
    }

    class Result(val serverKey: String, val serverName: String, val serverUser: String, val transcript: ByteArray, val keys: ControlKeys, val auth: ByteArray)

    fun complete(w: Map<String, Any?>, expectedKey: String?): Result {
        val serverKey = jsonText(w["key"])
        val serverEph = jsonText(w["eph"])
        val serverKeyBytes = controlFromB64(serverKey, 32)
        val serverEphBytes = controlFromB64(serverEph, 32)
        val sigBytes = controlFromB64(w["sig"], 64)
        val version = (w["v"] as? Number)?.toInt()
        if (version != CONTROL_PROTOCOL_VERSION || serverKeyBytes == null || serverEphBytes == null || sigBytes == null) {
            throw LocalControlException("bad_welcome")
        }
        if (expectedKey != null && expectedKey != serverKey) throw LocalControlException("wrong_desktop")
        val transcript = controlTranscript(identity.publicKeyB64url, ephB64, nonceB64, serverKey, serverEph)
        if (!Ed25519.verify(serverKeyBytes, controlServerSignedBytes(transcript), sigBytes)) {
            throw LocalControlException("bad_signature")
        }
        val shared = X25519.sharedSecret(ephPrivate, serverEphBytes)
        val keys = deriveControlKeys(shared, transcript)
        val sig = identity.sign(controlClientSignedBytes(transcript))
        val auth = utf8(Json.stringify(linkedMapOf("t" to CONTROL_AUTH_TAG, "sig" to sig)))
        return Result(serverKey, jsonText(w["name"]), jsonText(w["user"]), transcript, keys, auth)
    }
}

internal class ControlDispatcher {
    private val pending = ConcurrentHashMap<Int, CompletableFuture<Map<String, Any?>>>()
    private val taps = CopyOnWriteArrayList<Pair<Any, (ControlEvent) -> Unit>>()
    private val early = ArrayList<ControlEvent>()
    private val nextId = AtomicInteger(1)

    fun newRequest(): Pair<Int, CompletableFuture<Map<String, Any?>>> {
        val id = nextId.getAndIncrement()
        val future = CompletableFuture<Map<String, Any?>>()
        pending[id] = future
        return id to future
    }

    fun forget(id: Int) {
        pending.remove(id)
    }

    fun dispatch(message: Map<String, Any?>) {
        val event = message["event"]
        if (event is String) {
            deliver(ControlEvent(event, message["data"].asJsonMap()))
            return
        }
        val id = (message["id"] as? Number)?.toInt() ?: return
        val future = pending.remove(id) ?: return
        if (message["ok"] == true) {
            future.complete(message["result"].asJsonMap())
        } else {
            val error = message["error"]
            future.completeExceptionally(
                LocalControlException(if (error == null) "failed" else jsonText(error), jsonText(message["message"]).trim()),
            )
        }
    }

    fun deliver(event: ControlEvent) {
        val current = taps.toList()
        if (current.isEmpty()) {
            synchronized(early) {
                early.add(event)
                if (early.size > 64) early.removeAt(0)
            }
            return
        }
        for ((_, tap) in current) runCatching { tap(event) }
    }

    fun addTap(tap: (ControlEvent) -> Unit): Any {
        val token = Any()
        taps.add(token to tap)
        val buffered = synchronized(early) { ArrayList(early).also { early.clear() } }
        for (event in buffered) runCatching { tap(event) }
        return token
    }

    fun removeTap(token: Any) {
        taps.removeAll { it.first === token }
    }

    fun takeEarly(predicate: (ControlEvent) -> Boolean): ControlEvent? = synchronized(early) {
        val index = early.indexOfFirst(predicate)
        if (index < 0) null else early.removeAt(index)
    }

    fun failAll(code: String) {
        val all = pending.values.toList()
        pending.clear()
        for (future in all) future.completeExceptionally(LocalControlException(code))
    }
}

internal fun <T> awaitControl(future: CompletableFuture<T>, timeoutMs: Long): T = try {
    future.get(timeoutMs, TimeUnit.MILLISECONDS)
} catch (_: TimeoutException) {
    throw LocalControlException("timeout")
} catch (wrapped: ExecutionException) {
    throw wrapped.cause ?: wrapped
}

class LocalControlConnection private constructor(
    private val socket: Socket,
    private val output: OutputStream,
    val serverKey: String,
    val serverName: String,
    val serverUser: String,
    val transcript: ByteArray,
    private val keys: ControlKeys,
) : ControlLink {
    private val dispatcher = ControlDispatcher()
    private val closedFlag = AtomicBoolean(false)
    private val writeLock = Any()
    private var sendCounter = 0L
    private var receiveCounter = 0L
    private val closeListeners = CopyOnWriteArrayList<() -> Unit>()

    override val transport: ControlTransport get() = ControlTransport.Lan
    override val isOpen: Boolean get() = !closedFlag.get()
    override val desktopName: String get() = serverName

    val pairingCode: String get() = controlPairingCode(transcript)
    val isClosed: Boolean get() = closedFlag.get()

    fun onClose(listener: () -> Unit) {
        closeListeners.add(listener)
        if (closedFlag.get()) listener()
    }

    override fun addEventTap(tap: (ControlEvent) -> Unit): Any = dispatcher.addTap(tap)

    override fun removeEventTap(token: Any) = dispatcher.removeTap(token)

    private fun startReader(reader: ControlFrameReader, leftover: List<ByteArray>) {
        Thread({
            try {
                for (payload in leftover) if (!handle(payload)) return@Thread
                while (!closedFlag.get()) {
                    val payload = reader.next() ?: break
                    if (!handle(payload)) break
                }
            } catch (_: Exception) {
            } finally {
                close()
            }
        }, "LocalControl-reader").apply { isDaemon = true }.start()
    }

    private fun handle(payload: ByteArray): Boolean {
        val plain = openControlFrame(keys.serverToClient, receiveCounter, payload) ?: return false
        receiveCounter++
        val decoded = try {
            Json.parse(String(plain, Charsets.UTF_8))
        } catch (_: Exception) {
            null
        }
        if (decoded is Map<*, *>) dispatcher.dispatch(decoded.asJsonMap())
        return true
    }

    override fun request(method: String, params: Map<String, Any?>, timeoutMs: Long): Map<String, Any?> {
        if (isClosed) throw LocalControlException("closed")
        val (id, future) = dispatcher.newRequest()
        val body = utf8(Json.stringify(linkedMapOf("id" to id, "method" to method, "params" to params)))
        try {
            synchronized(writeLock) {
                if (isClosed) throw LocalControlException("closed")
                output.write(controlFrame(sealControlFrame(keys.clientToServer, sendCounter++, body)))
                output.flush()
            }
        } catch (error: LocalControlException) {
            dispatcher.forget(id)
            throw error
        } catch (_: Exception) {
            dispatcher.forget(id)
            close()
            throw LocalControlException("closed")
        }
        try {
            return awaitControl(future, timeoutMs)
        } finally {
            dispatcher.forget(id)
        }
    }

    fun close() {
        if (!closedFlag.compareAndSet(false, true)) return
        dispatcher.failAll("closed")
        runCatching { socket.close() }
        for (listener in closeListeners) runCatching { listener() }
    }

    companion object {
        fun open(
            host: String,
            port: Int,
            identity: DeviceIdentity,
            deviceName: String = "",
            user: String = "",
            platform: String = "",
            expectedKey: String? = null,
            timeoutMs: Int = 10_000,
        ): LocalControlConnection {
            val socket = Socket()
            try {
                socket.connect(InetSocketAddress(host, port), timeoutMs)
                socket.tcpNoDelay = true
                socket.soTimeout = timeoutMs
                val output = socket.getOutputStream()
                val handshake = ControlHandshake(identity, deviceName, user, platform)
                output.write(controlFrame(handshake.hello()))
                output.flush()
                val reader = ControlFrameReader(socket.getInputStream())
                var welcome: Map<String, Any?>? = null
                while (welcome == null) {
                    val payload = reader.next() ?: throw LocalControlException("closed")
                    val decoded = try {
                        Json.parse(String(payload, Charsets.UTF_8))
                    } catch (_: Exception) {
                        null
                    }
                    if (decoded is Map<*, *> && decoded["t"] == CONTROL_WELCOME_TAG) welcome = decoded.asJsonMap()
                }
                val result = handshake.complete(welcome, expectedKey)
                output.write(controlFrame(result.auth))
                output.flush()
                socket.soTimeout = 0
                val connection = LocalControlConnection(
                    socket,
                    output,
                    result.serverKey,
                    result.serverName,
                    result.serverUser,
                    result.transcript,
                    result.keys,
                )
                connection.startReader(reader, emptyList())
                return connection
            } catch (error: java.net.SocketTimeoutException) {
                runCatching { socket.close() }
                throw LocalControlException("timeout")
            } catch (error: Exception) {
                runCatching { socket.close() }
                throw error
            }
        }
    }
}

internal class ByteChunks {
    private val buffer = ByteArrayOutputStream()

    fun add(bytes: ByteArray) = buffer.write(bytes)

    val size: Int get() = buffer.size()

    fun take(): ByteArray = buffer.toByteArray().also { buffer.reset() }
}
