package com.forkmesh.nativeapp.net

import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.json.str
import java.io.ByteArrayOutputStream
import java.io.IOException
import java.io.InputStream
import java.net.HttpURLConnection
import java.net.ProtocolException
import java.net.URL

object HttpClient {
    const val MAX_BODY_BYTES = 64 * 1024 * 1024

    fun interface Observer {
        fun onExchange(
            method: String,
            url: String,
            status: Int,
            bytesOut: Long,
            bytesIn: Long,
            elapsedMs: Long,
            error: Throwable?,
        )
    }

    @Volatile
    var observer: Observer? = null

    class Request(val method: String, val url: String) {
        val headers = LinkedHashMap<String, String>()
        var body: ByteArray? = null
        var connectTimeoutMs = 15_000
        var readTimeoutMs = 30_000

        fun header(name: String, value: String?) = apply { if (value != null) headers[name] = value }

        fun bearer(token: String?) = apply {
            if (!token.isNullOrEmpty()) headers["Authorization"] = "Bearer $token"
        }

        fun json(payload: Any?) = apply {
            body = Json.stringify(payload).toByteArray(Charsets.UTF_8)
            headers["Content-Type"] = "application/json"
        }

        fun body(bytes: ByteArray, contentType: String?) = apply {
            body = bytes
            if (contentType != null) headers["Content-Type"] = contentType
        }

        fun timeouts(connectMs: Int, readMs: Int) = apply {
            connectTimeoutMs = connectMs
            readTimeoutMs = readMs
        }

        fun send(): Response = HttpClient.send(this)

        companion object {
            fun get(url: String) = Request("GET", url)
            fun post(url: String) = Request("POST", url)
        }
    }

    class Response(val status: Int, val headers: Map<String?, List<String>>, val body: ByteArray) {
        val ok: Boolean get() = status in 200..299

        fun text(): String = String(body, Charsets.UTF_8)

        fun json(): Any? = Json.parse(text())

        fun jsonMap(): MutableMap<String, Any?> = Json.parseObject(text())

        fun errorCode(): String = jsonMap().str("error")

        fun header(name: String): String? =
            headers.entries.firstOrNull { it.key.equals(name, ignoreCase = true) }?.value?.firstOrNull()
    }

    fun send(request: Request): Response {
        val started = System.currentTimeMillis()
        val bytesOut = request.body?.size?.toLong() ?: 0L
        var connection: HttpURLConnection? = null
        try {
            connection = URL(request.url).openConnection() as HttpURLConnection
            setMethod(connection, request.method)
            connection.connectTimeout = request.connectTimeoutMs
            connection.readTimeout = request.readTimeoutMs
            connection.useCaches = false
            connection.instanceFollowRedirects = true
            for ((name, value) in request.headers) connection.setRequestProperty(name, value)
            val body = request.body
            if (body != null) {
                connection.doOutput = true
                connection.setFixedLengthStreamingMode(body.size)
                connection.outputStream.use { it.write(body) }
            }
            val status = connection.responseCode
            val stream = if (status >= 400) connection.errorStream else connection.inputStream
            val bytes = stream?.let { readAll(it, MAX_BODY_BYTES) } ?: ByteArray(0)
            val response = Response(status, connection.headerFields ?: emptyMap(), bytes)
            observer?.onExchange(
                request.method, request.url, status, bytesOut, bytes.size.toLong(),
                System.currentTimeMillis() - started, null,
            )
            return response
        } catch (error: IOException) {
            observer?.onExchange(
                request.method, request.url, 0, bytesOut, 0,
                System.currentTimeMillis() - started, error,
            )
            throw error
        } finally {
            connection?.disconnect()
        }
    }

    private fun setMethod(connection: HttpURLConnection, method: String) {
        try {
            connection.requestMethod = method
        } catch (unsupported: ProtocolException) {
            var type: Class<*>? = connection.javaClass
            while (type != null) {
                try {
                    val field = type.getDeclaredField("method")
                    field.isAccessible = true
                    field.set(connection, method)
                    return
                } catch (_: NoSuchFieldException) {
                    type = type.superclass
                }
            }
            throw unsupported
        }
    }

    fun readAll(stream: InputStream, limit: Int): ByteArray = stream.use { input ->
        val out = ByteArrayOutputStream()
        val buffer = ByteArray(16 * 1024)
        var total = 0
        while (true) {
            val read = input.read(buffer)
            if (read == -1) break
            total += read
            if (total > limit) throw IOException("response larger than $limit bytes")
            out.write(buffer, 0, read)
        }
        out.toByteArray()
    }
}
