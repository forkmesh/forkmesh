package com.forkmesh.nativeapp.files

import java.io.ByteArrayOutputStream

object SrvSvc {
    private const val BIND = 11
    private const val BIND_ACK = 12
    private const val REQUEST = 0
    private const val RESPONSE = 2
    private const val FIRST_AND_LAST = 0x03
    private const val LAST_FRAG = 0x02

    fun uuid(text: String): ByteArray {
        val hex = text.replace("-", "")
        fun part(from: Int, to: Int) = hex.substring(from, to).chunked(2).map { it.toInt(16).toByte() }
        return (part(0, 8).reversed() + part(8, 12).reversed() + part(12, 16).reversed() + part(16, 32)).toByteArray()
    }

    private val srvsvc = uuid("4b324fc8-1670-01d3-1278-5a47bf6ee188")
    private val ndr = uuid("8a885d04-1ceb-11c9-9fe8-08002b104860")

    private fun pdu(type: Int, callId: Int, body: ByteArray): ByteArray = ByteWriter()
        .u8(5).u8(0).u8(type).u8(FIRST_AND_LAST)
        .u8(0x10).u8(0).u8(0).u8(0)
        .u16(16 + body.size).u16(0).u32(callId)
        .bytes(body)
        .toByteArray()

    fun bind(): ByteArray {
        val body = ByteWriter().u16(4280).u16(4280).u32(0)
            .u8(1).zeros(3)
            .u16(0).u8(1).u8(0)
            .bytes(srvsvc).u16(3).u16(0)
            .bytes(ndr).u32(2)
            .toByteArray()
        return pdu(BIND, 1, body)
    }

    private fun ndrString(writer: ByteWriter, text: String) {
        val chars = utf16(text + "\u0000")
        val count = chars.size / 2
        writer.u32(count).u32(0).u32(count).bytes(chars).align(4)
    }

    fun shareEnumRequest(host: String): ByteArray {
        val stub = ByteWriter().u32(0x00020000)
        ndrString(stub, "\\\\$host")
        stub.u32(1).u32(1).u32(0x00020004).u32(0).u32(0)
            .u32(0xFFFFFFFFL)
            .u32(0x00020008).u32(0)
        val body = ByteWriter().u32(stub.size).u16(0).u16(15).bytes(stub.toByteArray()).toByteArray()
        return pdu(REQUEST, 2, body)
    }

    private fun readString(reader: ByteReader): String {
        reader.align(4)
        reader.u32()
        reader.u32()
        val actual = reader.u32().toInt()
        val text = fromUtf16(reader.bytes(actual * 2))
        return text.trimEnd('\u0000')
    }

    data class Share(val name: String, val type: Long, val remark: String)

    fun parseShareEnum(stub: ByteArray): List<Share> {
        val reader = ByteReader(stub)
        reader.u32()
        reader.u32()
        val container = reader.u32()
        if (container == 0L) return emptyList()
        val count = reader.u32().toInt()
        val array = reader.u32()
        if (array == 0L || count == 0) return emptyList()
        reader.u32()
        val raw = (0 until count).map { Triple(reader.u32(), reader.u32(), reader.u32()) }
        return raw.map { (namePointer, type, remarkPointer) ->
            val name = if (namePointer != 0L) readString(reader) else ""
            val remark = if (remarkPointer != 0L) readString(reader) else ""
            Share(name, type, remark)
        }
    }

    fun diskShares(shares: List<Share>): List<String> =
        shares.filter { (it.type and 0xFFFFL) == 0L && (it.type and 0x80000000L) == 0L && !it.name.endsWith("$") && it.name.isNotEmpty() }
            .map { it.name }
            .sortedBy { it.lowercase() }

    fun responseStub(pdus: ByteArray): Pair<ByteArray, Boolean> {
        val reader = ByteReader(pdus)
        val stub = ByteArrayOutputStream()
        var last = false
        while (reader.remaining >= 16) {
            val start = reader.position
            val type = reader.u8(start + 2)
            val flags = reader.u8(start + 3)
            val length = reader.u16(start + 8)
            if (length < 16 || start + length > pdus.size) break
            if (type != RESPONSE) throw SmbException(SmbStatus.ACCESS_DENIED, SmbStatus.message(SmbStatus.ACCESS_DENIED))
            stub.write(pdus, start + 24, length - 24)
            reader.position = start + length
            if ((flags and LAST_FRAG) != 0) {
                last = true
                break
            }
        }
        return stub.toByteArray() to last
    }

    fun shareEnum(ipc: SmbShare, host: String): List<String> {
        val handle = ipc.create("srvsvc", 0x0012019FL, Smb2.FILE_OPEN, 0, 0L, 3)
        try {
            val ack = ipc.pipeCall(handle, bind())
            if (ack.size < 3 || (ack[2].toInt() and 0xff) != BIND_ACK) throw SmbException(SmbStatus.ACCESS_DENIED, SmbStatus.message(SmbStatus.ACCESS_DENIED))
            var data = ipc.pipeCall(handle, shareEnumRequest(host))
            var (stub, last) = responseStub(data)
            while (!last) {
                val more = ipc.pipeRead(handle)
                if (more.isEmpty()) break
                data += more
                val parsed = responseStub(data)
                stub = parsed.first
                last = parsed.second
            }
            return diskShares(parseShareEnum(stub))
        } finally {
            ipc.close(handle)
        }
    }
}
