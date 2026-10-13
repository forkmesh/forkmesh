package com.forkmesh.nativeapp.net

import java.io.BufferedInputStream
import java.io.ByteArrayOutputStream
import java.io.EOFException
import java.io.IOException
import java.net.InetAddress
import java.net.ServerSocket
import java.net.Socket
import java.util.concurrent.CopyOnWriteArrayList

class TestWebSocketServer(private val handler: (Connection) -> Unit) : AutoCloseable {
    private val server = ServerSocket(0, 50, InetAddress.getLoopbackAddress())

    val port: Int get() = server.localPort

    val baseUrl: String get() = "ws://127.0.0.1:$port"

    val connections = CopyOnWriteArrayList<Connection>()

    val failures = CopyOnWriteArrayList<Throwable>()

    init {
        Thread({
            while (!server.isClosed) {
                val socket = try {
                    server.accept()
                } catch (_: IOException) {
                    break
                }
                val connection = Connection(socket, connections.size)
                connections.add(connection)
                Thread({
                    try {
                        handler(connection)
                    } catch (failure: Throwable) {
                        failures.add(failure)
                    } finally {
                        connection.close()
                    }
                }, "TestWebSocketServer-connection").apply { isDaemon = true }.start()
            }
        }, "TestWebSocketServer-accept").apply { isDaemon = true }.start()
    }

    override fun close() {
        try {
            server.close()
        } catch (_: IOException) {
        }
        connections.forEach { it.close() }
    }

    class Frame(val fin: Boolean, val opcode: Int, val payload: ByteArray, val masked: Boolean) {
        val text: String get() = String(payload, Charsets.UTF_8)

        val closeCode: Int
            get() = if (payload.size >= 2) ((payload[0].toInt() and 0xff) shl 8) or (payload[1].toInt() and 0xff) else 1005

        val closeReason: String
            get() = if (payload.size > 2) String(payload, 2, payload.size - 2, Charsets.UTF_8) else ""
    }

    class Connection(val socket: Socket, val index: Int) {
        private val input = BufferedInputStream(socket.getInputStream())
        private val output = socket.getOutputStream()

        var requestLine = ""
            private set

        val requestHeaders = LinkedHashMap<String, String>()

        init {
            socket.soTimeout = 5_000
            socket.tcpNoDelay = true
        }

        fun header(name: String): String? = requestHeaders.entries.firstOrNull { it.key.equals(name, ignoreCase = true) }?.value

        fun readRequest(): Connection {
            requestLine = readLine()
            while (true) {
                val line = readLine()
                if (line.isEmpty()) break
                val colon = line.indexOf(':')
                requestHeaders[line.substring(0, colon).trim()] = line.substring(colon + 1).trim()
            }
            return this
        }

        fun accept(protocol: String? = null, acceptKey: String? = null, upgrade: String? = "websocket", trailing: ByteArray = ByteArray(0)) {
            val head = StringBuilder("HTTP/1.1 101 Switching Protocols\r\n")
            if (upgrade != null) head.append("Upgrade: ").append(upgrade).append("\r\n")
            head.append("Connection: Upgrade\r\n")
            head.append("Sec-WebSocket-Accept: ")
                .append(acceptKey ?: WebSocketClient.acceptFor(header("Sec-WebSocket-Key").orEmpty()))
                .append("\r\n")
            if (protocol != null) head.append("Sec-WebSocket-Protocol: ").append(protocol).append("\r\n")
            head.append("\r\n")
            sendRaw(head.toString().toByteArray(Charsets.ISO_8859_1) + trailing)
        }

        fun respond(raw: String) = sendRaw(raw.toByteArray(Charsets.UTF_8))

        fun sendRaw(bytes: ByteArray) {
            output.write(bytes)
            output.flush()
        }

        fun sendFrame(opcode: Int, payload: ByteArray, fin: Boolean = true, rsv: Int = 0, mask: Boolean = false) =
            sendRaw(encodeFrame(opcode, payload, fin, rsv, mask))

        fun sendText(text: String) = sendFrame(OP_TEXT, text.toByteArray(Charsets.UTF_8))

        fun sendBinary(data: ByteArray) = sendFrame(OP_BINARY, data)

        fun sendPing(payload: ByteArray) = sendFrame(OP_PING, payload)

        fun sendPong(payload: ByteArray) = sendFrame(OP_PONG, payload)

        fun sendClose(code: Int?, reason: String = "") {
            val payload = if (code == null) {
                ByteArray(0)
            } else {
                byteArrayOf((code shr 8).toByte(), code.toByte()) + reason.toByteArray(Charsets.UTF_8)
            }
            sendFrame(OP_CLOSE, payload)
        }

        fun readFrame(): Frame {
            val b0 = readByte()
            val b1 = readByte()
            val masked = (b1 and 0x80) != 0
            if (!masked) throw AssertionError("client frame is not masked")
            var length = (b1 and 0x7f).toLong()
            if (length == 126L) {
                length = ((readByte() shl 8) or readByte()).toLong()
            } else if (length == 127L) {
                length = 0
                repeat(8) { length = (length shl 8) or readByte().toLong() }
            }
            val mask = readBytes(4)
            val payload = readBytes(length.toInt())
            for (i in payload.indices) payload[i] = (payload[i].toInt() xor mask[i % 4].toInt()).toByte()
            return Frame((b0 and 0x80) != 0, b0 and 0x0f, payload, masked)
        }

        fun readMessageFrames(): List<Frame> {
            val frames = ArrayList<Frame>()
            while (true) {
                val frame = readFrame()
                if (frame.opcode >= OP_CLOSE) throw AssertionError("unexpected control frame ${frame.opcode}")
                frames.add(frame)
                if (frame.fin) return frames
            }
        }

        fun echoUntilClose(): Frame {
            val message = ByteArrayOutputStream()
            var messageOpcode = -1
            while (true) {
                val frame = readFrame()
                when (frame.opcode) {
                    OP_CLOSE -> {
                        sendFrame(OP_CLOSE, frame.payload)
                        return frame
                    }
                    OP_PING -> sendPong(frame.payload)
                    OP_PONG -> {}
                    else -> {
                        if (frame.opcode != OP_CONTINUATION) {
                            messageOpcode = frame.opcode
                            message.reset()
                        }
                        message.write(frame.payload)
                        if (frame.fin) sendFrame(messageOpcode, message.toByteArray())
                    }
                }
            }
        }

        fun close() {
            try {
                socket.close()
            } catch (_: IOException) {
            }
        }

        private fun readByte(): Int {
            val value = input.read()
            if (value < 0) throw EOFException("client closed the connection")
            return value
        }

        private fun readBytes(count: Int): ByteArray {
            val bytes = ByteArray(count)
            var read = 0
            while (read < count) {
                val n = input.read(bytes, read, count - read)
                if (n < 0) throw EOFException("client closed the connection")
                read += n
            }
            return bytes
        }

        private fun readLine(): String {
            val out = ByteArrayOutputStream()
            while (true) {
                val b = readByte()
                if (b == '\n'.code) break
                if (b != '\r'.code) out.write(b)
            }
            return String(out.toByteArray(), Charsets.ISO_8859_1)
        }
    }

    companion object {
        const val OP_CONTINUATION = 0x0
        const val OP_TEXT = 0x1
        const val OP_BINARY = 0x2
        const val OP_CLOSE = 0x8
        const val OP_PING = 0x9
        const val OP_PONG = 0xA

        fun encodeFrame(opcode: Int, payload: ByteArray, fin: Boolean = true, rsv: Int = 0, mask: Boolean = false): ByteArray {
            val out = ByteArrayOutputStream()
            out.write((if (fin) 0x80 else 0) or rsv or opcode)
            val maskBit = if (mask) 0x80 else 0
            when {
                payload.size <= 125 -> out.write(maskBit or payload.size)
                payload.size <= 0xFFFF -> {
                    out.write(maskBit or 126)
                    out.write(payload.size shr 8)
                    out.write(payload.size and 0xff)
                }
                else -> {
                    out.write(maskBit or 127)
                    for (i in 0 until 8) out.write(((payload.size.toLong() shr (56 - 8 * i)) and 0xff).toInt())
                }
            }
            if (mask) {
                val key = byteArrayOf(1, 2, 3, 4)
                out.write(key)
                out.write(ByteArray(payload.size) { (payload[it].toInt() xor key[it % 4].toInt()).toByte() })
            } else {
                out.write(payload)
            }
            return out.toByteArray()
        }
    }
}
