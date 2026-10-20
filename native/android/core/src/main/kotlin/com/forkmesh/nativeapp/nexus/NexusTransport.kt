package com.forkmesh.nativeapp.nexus

import java.io.InterruptedIOException
import java.net.ConnectException
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetAddress
import java.net.InetSocketAddress
import java.net.NoRouteToHostException
import java.net.Socket
import java.net.SocketException
import java.net.SocketTimeoutException
import java.net.UnknownHostException
import java.util.concurrent.Callable
import java.util.concurrent.ExecutionException
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit
import java.util.concurrent.TimeoutException
import java.util.concurrent.atomic.AtomicBoolean

class NexusCancel {
    private val flag = AtomicBoolean(false)
    private val hooks = mutableListOf<() -> Unit>()

    val isCancelled: Boolean get() = flag.get()

    fun cancel() {
        if (!flag.compareAndSet(false, true)) return
        val pending = synchronized(hooks) { hooks.toList().also { hooks.clear() } }
        pending.forEach { runCatching(it) }
    }

    fun onCancel(hook: () -> Unit) {
        val runNow = synchronized(hooks) {
            if (flag.get()) true else {
                hooks += hook
                false
            }
        }
        if (runNow) runCatching(hook)
    }

    fun check() {
        if (flag.get()) throw ConnectionFailure.Cancelled
    }
}

interface NexusStream {
    val remoteAddress: String?
    fun send(data: ByteArray, timeoutMs: Int)
    fun receive(maximumLength: Int, timeoutMs: Int): ByteArray
    fun close()

    fun receiveUntil(maximumLength: Int, timeoutMs: Int, done: (ByteArray) -> Boolean): ByteArray {
        val deadline = System.nanoTime() + timeoutMs * 1_000_000L
        var buffer = ByteArray(0)
        while (buffer.size < maximumLength && !done(buffer)) {
            val remaining = ((deadline - System.nanoTime()) / 1_000_000L).toInt()
            if (remaining <= 0) break
            val chunk = try {
                receive(maximumLength - buffer.size, remaining)
            } catch (timeout: ConnectionFailure) {
                if (timeout == ConnectionFailure.TimedOut && buffer.isNotEmpty()) break
                throw timeout
            }
            if (chunk.isEmpty()) break
            buffer += chunk
        }
        return buffer
    }
}

fun interface StreamConnector {
    fun connect(endpoint: Endpoint, timeoutMs: Int): NexusStream
}

interface NexusDatagram {
    fun send(data: ByteArray)
    fun receive(timeoutMs: Int): ByteArray
    fun close()
}

fun interface DatagramConnector {
    fun open(endpoint: Endpoint, timeoutMs: Int): NexusDatagram
}

fun interface HostResolving {
    fun resolve(host: String, timeoutMs: Int): ResolvedHost
}

object NexusFailures {
    fun map(error: Throwable): ConnectionFailure {
        if (error is ConnectionFailure) return error
        val message = error.message.orEmpty()
        return when {
            error is SocketTimeoutException -> ConnectionFailure.TimedOut
            error is InterruptedIOException || error is InterruptedException -> ConnectionFailure.Cancelled
            error is UnknownHostException -> ConnectionFailure.NameResolution(message.ifEmpty { "unknown host" })
            error is SecurityException -> ConnectionFailure.PermissionDenied
            error is NoRouteToHostException || "EHOSTUNREACH" in message || "No route to host" in message -> ConnectionFailure.HostUnreachable
            "ENETUNREACH" in message || "Network is unreachable" in message -> ConnectionFailure.NetworkUnreachable
            "ETIMEDOUT" in message || "timed out" in message -> ConnectionFailure.TimedOut
            "EACCES" in message || "EPERM" in message || "Permission denied" in message || "Operation not permitted" in message -> ConnectionFailure.PermissionDenied
            error is ConnectException || "ECONNREFUSED" in message || "Connection refused" in message -> ConnectionFailure.Refused
            "ECONNRESET" in message || "Connection reset" in message -> ConnectionFailure.ResetByPeer
            error is javax.net.ssl.SSLException -> ConnectionFailure.Tls(message.ifEmpty { error.javaClass.simpleName })
            else -> ConnectionFailure.System(message.ifEmpty { error.javaClass.simpleName })
        }
    }

    inline fun <T> guard(block: () -> T): T = try {
        block()
    } catch (error: Exception) {
        throw map(error)
    }
}

class SocketStream(private val socket: Socket) : NexusStream {
    override val remoteAddress: String? get() = socket.inetAddress?.hostAddress

    override fun send(data: ByteArray, timeoutMs: Int) = NexusFailures.guard {
        socket.soTimeout = timeoutMs.coerceAtLeast(1)
        socket.getOutputStream().apply {
            write(data)
            flush()
        }
        Unit
    }

    override fun receive(maximumLength: Int, timeoutMs: Int): ByteArray = NexusFailures.guard {
        socket.soTimeout = timeoutMs.coerceAtLeast(1)
        val buffer = ByteArray(maximumLength.coerceIn(1, 65_536))
        val read = socket.getInputStream().read(buffer)
        if (read <= 0) ByteArray(0) else buffer.copyOf(read)
    }

    override fun close() {
        runCatching { socket.close() }
    }
}

object SocketConnector : StreamConnector {
    override fun connect(endpoint: Endpoint, timeoutMs: Int): NexusStream {
        val socket = Socket()
        try {
            val address = IPAddr.parse(endpoint.host)?.let { InetAddress.getByName(it.toString().substringBefore('%')) }
            val target = if (address != null) InetSocketAddress(address, endpoint.port) else InetSocketAddress(endpoint.host, endpoint.port)
            if (target.isUnresolved) throw ConnectionFailure.NameResolution("No address found for ${endpoint.host}.")
            socket.tcpNoDelay = true
            socket.connect(target, timeoutMs.coerceAtLeast(1))
            return SocketStream(socket)
        } catch (error: Exception) {
            runCatching { socket.close() }
            throw NexusFailures.map(error)
        }
    }
}

class UdpDatagram(private val socket: DatagramSocket) : NexusDatagram {
    override fun send(data: ByteArray) = NexusFailures.guard { socket.send(DatagramPacket(data, data.size)) }

    override fun receive(timeoutMs: Int): ByteArray = NexusFailures.guard {
        socket.soTimeout = timeoutMs.coerceAtLeast(1)
        val buffer = ByteArray(4096)
        val packet = DatagramPacket(buffer, buffer.size)
        socket.receive(packet)
        buffer.copyOf(packet.length)
    }

    override fun close() {
        runCatching { socket.close() }
    }
}

object UdpConnector : DatagramConnector {
    override fun open(endpoint: Endpoint, timeoutMs: Int): NexusDatagram = NexusFailures.guard {
        val socket = DatagramSocket()
        try {
            socket.connect(InetSocketAddress(InetAddress.getByName(endpoint.host), endpoint.port))
            UdpDatagram(socket)
        } catch (error: Exception) {
            socket.close()
            throw error
        }
    }
}

object SystemResolver : HostResolving {
    private val pool = Executors.newCachedThreadPool { task -> Thread(task, "nexus-resolve").apply { isDaemon = true } }

    override fun resolve(host: String, timeoutMs: Int): ResolvedHost {
        IPAddr.parse(host)?.let { return ResolvedHost(host, listOf(it)) }
        val future = pool.submit(Callable { InetAddress.getAllByName(host).toList() })
        val found = try {
            future.get(timeoutMs.coerceAtLeast(1).toLong(), TimeUnit.MILLISECONDS)
        } catch (timeout: TimeoutException) {
            future.cancel(true)
            throw ConnectionFailure.NameResolution("No answer for $host before the timeout.")
        } catch (failed: ExecutionException) {
            throw ConnectionFailure.NameResolution("No A or AAAA record returned for $host.")
        }
        val addresses = found.mapNotNull { IPAddr.parse(it.hostAddress ?: "") }.distinct().sorted()
        if (addresses.isEmpty()) throw ConnectionFailure.NameResolution("No A or AAAA record returned for $host.")
        return ResolvedHost(host, addresses)
    }
}

internal fun elapsedMs(startedNanos: Long): Double = (System.nanoTime() - startedNanos) / 1_000_000.0
