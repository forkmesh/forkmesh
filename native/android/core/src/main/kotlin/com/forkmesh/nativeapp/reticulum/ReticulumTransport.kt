package com.forkmesh.nativeapp.reticulum

import java.io.IOException
import java.net.ConnectException
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetAddress
import java.net.InetSocketAddress
import java.net.NoRouteToHostException
import java.net.Socket
import java.net.SocketTimeoutException
import java.net.UnknownHostException
import java.util.concurrent.ArrayBlockingQueue
import java.util.concurrent.ScheduledThreadPoolExecutor
import java.util.concurrent.ThreadFactory
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicInteger
import kotlin.concurrent.thread

interface ReticulumConnection {
    fun write(bytes: ByteArray): Boolean
    fun close()
}

interface ReticulumConnectionEvents {
    fun connected()
    fun received(bytes: ByteArray, count: Int)
    fun failed(reason: String)
    fun closed()
}

interface ReticulumTransport {
    fun openTcp(host: String, port: Int, timeoutMs: Int, events: ReticulumConnectionEvents): ReticulumConnection

    @Throws(IOException::class)
    fun openUdp(listenAddress: String, listenPort: Int, forwardAddress: String, forwardPort: Int, maxDatagram: Int, events: ReticulumConnectionEvents): ReticulumConnection
}

fun interface ReticulumTimer {
    fun cancel()
}

interface ReticulumTimers {
    fun schedule(delayMs: Long, task: () -> Unit): ReticulumTimer
    fun every(periodMs: Long, task: () -> Unit): ReticulumTimer
    fun shutdown()
}

class ExecutorTimers : ReticulumTimers {
    private val executor = ScheduledThreadPoolExecutor(1, ThreadFactory { runnable ->
        Thread(runnable, "reticulum-timers-${counter.incrementAndGet()}").also { it.isDaemon = true }
    }).apply { removeOnCancelPolicy = true }

    override fun schedule(delayMs: Long, task: () -> Unit): ReticulumTimer {
        val future = executor.schedule({ runCatching(task) }, delayMs.coerceAtLeast(0), TimeUnit.MILLISECONDS)
        return ReticulumTimer { future.cancel(false) }
    }

    override fun every(periodMs: Long, task: () -> Unit): ReticulumTimer {
        val future = executor.scheduleAtFixedRate({ runCatching(task) }, periodMs, periodMs, TimeUnit.MILLISECONDS)
        return ReticulumTimer { future.cancel(false) }
    }

    override fun shutdown() {
        executor.shutdownNow()
    }

    private companion object {
        val counter = AtomicInteger()
    }
}

class SocketTransport : ReticulumTransport {
    override fun openTcp(host: String, port: Int, timeoutMs: Int, events: ReticulumConnectionEvents): ReticulumConnection =
        TcpConnection(host, port, timeoutMs, events).also { it.start() }

    override fun openUdp(listenAddress: String, listenPort: Int, forwardAddress: String, forwardPort: Int, maxDatagram: Int, events: ReticulumConnectionEvents): ReticulumConnection =
        UdpConnection(listenAddress, listenPort, forwardAddress, forwardPort, maxDatagram, events).also { it.start() }

    companion object {
        const val REFUSED = "Connection refused"

        fun describe(problem: Throwable, host: String): String {
            val message = problem.message.orEmpty()
            return when {
                problem is UnknownHostException -> "Host $host not found"
                problem is SocketTimeoutException -> "No answer from $host"
                problem is NoRouteToHostException || "EHOSTUNREACH" in message -> "No route to $host"
                "ENETUNREACH" in message || "unreachable" in message.lowercase() -> "The network is unreachable"
                "ECONNREFUSED" in message || "refused" in message.lowercase() -> REFUSED
                problem is ConnectException -> REFUSED
                message.isNotBlank() -> message
                else -> problem.javaClass.simpleName
            }
        }
    }
}

private class TcpConnection(
    private val host: String,
    private val port: Int,
    private val timeoutMs: Int,
    private val events: ReticulumConnectionEvents,
) : ReticulumConnection {
    @Volatile private var closed = false
    @Volatile private var open = false
    private val socket = Socket()
    private val queue = ArrayBlockingQueue<ByteArray>(QUEUE_FRAMES)

    fun start() {
        thread(name = "reticulum-tcp-$host", isDaemon = true) { run() }
    }

    private fun run() {
        try {
            val address = InetSocketAddress(host, port)
            if (address.isUnresolved) throw UnknownHostException(host)
            socket.tcpNoDelay = true
            socket.keepAlive = true
            socket.connect(address, timeoutMs)
        } catch (problem: Exception) {
            val wasClosed = closed
            close()
            if (!wasClosed) events.failed(SocketTransport.describe(problem, host))
            return
        }
        if (closed) {
            close()
            return
        }
        open = true
        thread(name = "reticulum-tcp-write-$host", isDaemon = true) { writeLoop() }
        events.connected()
        try {
            val input = socket.getInputStream()
            val buffer = ByteArray(READ_BUFFER)
            while (!closed) {
                val count = input.read(buffer)
                if (count < 0) break
                if (count > 0) events.received(buffer, count)
            }
        } catch (_: IOException) {
        }
        val wasClosed = closed
        close()
        if (!wasClosed) events.closed()
    }

    private fun writeLoop() {
        try {
            val output = socket.getOutputStream()
            while (!closed) {
                val bytes = queue.poll(1, TimeUnit.SECONDS) ?: continue
                if (bytes === STOP) break
                output.write(bytes)
                output.flush()
            }
        } catch (_: Exception) {
            runCatching { socket.close() }
        }
    }

    override fun write(bytes: ByteArray): Boolean = open && !closed && queue.offer(bytes)

    override fun close() {
        closed = true
        open = false
        queue.clear()
        queue.offer(STOP)
        runCatching { socket.close() }
    }

    private companion object {
        val STOP = ByteArray(0)
        const val QUEUE_FRAMES = 512
        const val READ_BUFFER = 16384
    }
}

private class UdpConnection(
    listenAddress: String,
    listenPort: Int,
    forwardAddress: String,
    private val forwardPort: Int,
    private val maxDatagram: Int,
    private val events: ReticulumConnectionEvents,
) : ReticulumConnection {
    @Volatile private var closed = false
    private val target: InetAddress = InetAddress.getByName(forwardAddress)
    private val socket = DatagramSocket(null).apply {
        reuseAddress = true
        broadcast = true
        try {
            bind(InetSocketAddress(InetAddress.getByName(listenAddress), listenPort))
        } catch (problem: IOException) {
            close()
            throw problem
        }
    }
    private val queue = ArrayBlockingQueue<ByteArray>(QUEUE_DATAGRAMS)

    fun start() {
        thread(name = "reticulum-udp-read", isDaemon = true) { readLoop() }
        thread(name = "reticulum-udp-write", isDaemon = true) { writeLoop() }
    }

    private fun readLoop() {
        val buffer = ByteArray(maxDatagram + 1)
        val packet = DatagramPacket(buffer, buffer.size)
        try {
            while (!closed) {
                packet.length = buffer.size
                socket.receive(packet)
                events.received(buffer.copyOf(packet.length), packet.length)
            }
        } catch (_: IOException) {
        }
        val wasClosed = closed
        close()
        if (!wasClosed) events.closed()
    }

    private fun writeLoop() {
        while (!closed) {
            val bytes = runCatching { queue.poll(1, TimeUnit.SECONDS) }.getOrNull() ?: continue
            if (bytes === STOP) break
            runCatching { socket.send(DatagramPacket(bytes, bytes.size, target, forwardPort)) }
        }
    }

    override fun write(bytes: ByteArray): Boolean = !closed && queue.offer(bytes)

    override fun close() {
        closed = true
        queue.clear()
        queue.offer(STOP)
        runCatching { socket.close() }
    }

    private companion object {
        val STOP = ByteArray(0)
        const val QUEUE_DATAGRAMS = 256
    }
}
