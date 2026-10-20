package com.forkmesh.nativeapp.nexus

import java.io.ByteArrayOutputStream
import kotlin.random.Random

class DnsResolver(
    val resolver: Endpoint = Endpoint("1.1.1.1", 53),
    private val connector: DatagramConnector = UdpConnector,
) : HostResolving {
    fun lookup(name: String, type: DnsRecordType, timeoutMs: Int = 5_000): DnsLookup {
        val normalized = name.trim().removeSuffix(".")
        if (normalized.isEmpty()) throw ConnectionFailure.NameResolution("Enter a hostname.")
        val identifier = Random.nextInt(0, 0x10000)
        val request = DnsWire.query(identifier, normalized, type)
        val started = System.nanoTime()
        val socket = connector.open(resolver, timeoutMs)
        try {
            socket.send(request)
            val parsed = DnsWire.parse(socket.receive(timeoutMs), identifier)
            return DnsLookup(
                name = normalized, type = type, answers = parsed.answers, authority = parsed.authority, additional = parsed.additional,
                responseCode = parsed.responseCode, resolver = resolver.toString(), latencyMilliseconds = elapsedMs(started),
                error = DnsWire.responseError(parsed.responseCode),
            )
        } finally {
            socket.close()
        }
    }

    fun reverse(address: IPAddr, timeoutMs: Int = 5_000): DnsLookup = lookup(address.reversePointerName, DnsRecordType.PTR, timeoutMs)

    override fun resolve(host: String, timeoutMs: Int): ResolvedHost {
        IPAddr.parse(host)?.let { return ResolvedHost(host, listOf(it)) }
        val results = listOf(DnsRecordType.A, DnsRecordType.AAAA).mapNotNull { runCatching { lookup(host, it, timeoutMs) }.getOrNull() }
        val addresses = results.flatMap { result -> result.answers.mapNotNull { IPAddr.parse(it.value) } }.distinct().sorted()
        if (addresses.isEmpty()) throw ConnectionFailure.NameResolution("No A or AAAA record returned for $host.")
        return ResolvedHost(host, addresses)
    }
}

object DnsWire {
    data class Message(val responseCode: Int, val answers: List<DnsRecord>, val authority: List<DnsRecord>, val additional: List<DnsRecord>)

    private val malformed get() = ConnectionFailure.System("Malformed DNS response.")

    fun query(identifier: Int, name: String, type: DnsRecordType): ByteArray {
        val out = ByteArrayOutputStream()
        fun put16(value: Int) {
            out.write(value shr 8 and 0xFF)
            out.write(value and 0xFF)
        }
        put16(identifier)
        put16(0x0100)
        put16(1)
        put16(0)
        put16(0)
        put16(0)
        for (label in name.split(".")) {
            val bytes = label.toByteArray()
            if (bytes.isEmpty() || bytes.size > 63) throw ConnectionFailure.NameResolution("Invalid DNS name $name.")
            out.write(bytes.size)
            out.write(bytes)
        }
        out.write(0)
        put16(type.code)
        put16(1)
        return out.toByteArray()
    }

    fun parse(data: ByteArray, expectedIdentifier: Int): Message {
        val cursor = intArrayOf(0)
        if (read16(data, cursor) != expectedIdentifier) throw ConnectionFailure.System("DNS response identifier did not match request.")
        val flags = read16(data, cursor)
        if (flags and 0x8000 == 0) throw ConnectionFailure.System("DNS response was not a reply.")
        val questions = read16(data, cursor)
        val answerCount = read16(data, cursor)
        val authorityCount = read16(data, cursor)
        val additionalCount = read16(data, cursor)
        repeat(questions) {
            readName(data, cursor)
            cursor[0] += 4
            if (cursor[0] > data.size) throw malformed
        }
        val answers = readRecords(data, cursor, answerCount)
        val authority = readRecords(data, cursor, authorityCount)
        val additional = readRecords(data, cursor, additionalCount)
        return Message(flags and 0x000F, answers, authority, additional)
    }

    private fun readRecords(data: ByteArray, cursor: IntArray, count: Int): List<DnsRecord> = (0 until count).mapNotNull {
        val name = readName(data, cursor)
        val rawType = read16(data, cursor)
        read16(data, cursor)
        val ttl = read32(data, cursor)
        val length = read16(data, cursor)
        val end = cursor[0] + length
        if (end > data.size) throw malformed
        val type = DnsRecordType.of(rawType)
        val record = type?.let { DnsRecord(name, it, ttl, recordValue(data, cursor[0], end, it)) }
        cursor[0] = end
        record
    }

    private fun recordValue(data: ByteArray, offset: Int, end: Int, type: DnsRecordType): String = when (type) {
        DnsRecordType.A -> {
            if (end - offset != 4) throw malformed
            IPv4Addr.of(data.copyOfRange(offset, end)).toString()
        }
        DnsRecordType.AAAA -> {
            if (end - offset != 16) throw malformed
            IPv6Addr.of(data.copyOfRange(offset, end)).toString()
        }
        DnsRecordType.CNAME, DnsRecordType.NS, DnsRecordType.PTR -> readName(data, intArrayOf(offset))
        DnsRecordType.MX -> {
            val cursor = intArrayOf(offset)
            val preference = read16(data, cursor)
            "$preference ${readName(data, cursor)}"
        }
        DnsRecordType.TXT -> {
            var cursor = offset
            val values = mutableListOf<String>()
            while (cursor < end) {
                val length = data[cursor].toInt() and 0xFF
                cursor++
                if (cursor + length > end) throw malformed
                values += String(data, cursor, length, Charsets.UTF_8)
                cursor += length
            }
            values.joinToString(" ")
        }
        DnsRecordType.SOA -> {
            val cursor = intArrayOf(offset)
            val primary = readName(data, cursor)
            val mailbox = readName(data, cursor)
            "$primary $mailbox"
        }
    }

    fun readName(data: ByteArray, cursor: IntArray): String {
        val labels = mutableListOf<String>()
        var position = cursor[0]
        var jumped = false
        var jumps = 0
        while (true) {
            if (position >= data.size) throw malformed
            val length = data[position].toInt() and 0xFF
            if (length and 0xC0 == 0xC0) {
                if (position + 1 >= data.size || ++jumps > 20) throw malformed
                val pointer = (length and 0x3F) shl 8 or (data[position + 1].toInt() and 0xFF)
                if (!jumped) cursor[0] = position + 2
                position = pointer
                jumped = true
                continue
            }
            if (length and 0xC0 != 0) throw malformed
            position++
            if (length == 0) {
                if (!jumped) cursor[0] = position
                return if (labels.isEmpty()) "." else labels.joinToString(".")
            }
            val end = position + length
            if (end > data.size) throw malformed
            labels += String(data, position, length, Charsets.UTF_8)
            position = end
        }
    }

    private fun read16(data: ByteArray, cursor: IntArray): Int {
        if (cursor[0] + 2 > data.size) throw malformed
        val value = (data[cursor[0]].toInt() and 0xFF) shl 8 or (data[cursor[0] + 1].toInt() and 0xFF)
        cursor[0] += 2
        return value
    }

    private fun read32(data: ByteArray, cursor: IntArray): Long {
        if (cursor[0] + 4 > data.size) throw malformed
        var value = 0L
        repeat(4) { value = value shl 8 or (data[cursor[0] + it].toLong() and 0xFF) }
        cursor[0] += 4
        return value
    }

    fun responseError(code: Int): String? = when (code) {
        0 -> null
        1 -> "Format error"
        2 -> "Server failure"
        3 -> "Name does not exist"
        4 -> "Not implemented"
        5 -> "Refused"
        else -> "DNS response code $code"
    }
}
