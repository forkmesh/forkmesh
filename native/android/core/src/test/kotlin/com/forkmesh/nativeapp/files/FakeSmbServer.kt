package com.forkmesh.nativeapp.files

import java.util.ArrayDeque

class FakeSmbServer(
    var password: String = "secret",
    var user: String = "alice",
    var allowGuest: Boolean = false,
    var allowAnonymous: Boolean = false,
    var signingRequired: Boolean = false,
    var maxRead: Int = 4096,
    var dialect: Int = Smb2.DIALECT_210,
    var shareNames: List<Pair<String, Long>> = listOf("media" to 0L, "IPC$" to 3L, "backup" to 0L, "print$" to 0x80000000L),
) : SmbTransport {
    val files = sortedMapOf<String, ByteArray>()
    val folders = sortedSetOf("")
    val outbox = ArrayDeque<ByteArray>()
    val commands = mutableListOf<Int>()
    var dropNext = false
    var closed = false
    var lastUser = ""
    private val serverChallenge = ByteArray(8) { (it + 1).toByte() }
    private var sessionKey: ByteArray? = null
    private var authenticatedGuest = false
    private val trees = mutableMapOf<Long, String>()
    private var nextTree = 1L
    private val handles = mutableMapOf<Long, Handle>()
    private var nextHandle = 100L
    private var pipeOutput = ByteArray(0)
    val badSignatures = mutableListOf<Int>()

    private class Handle(val path: String, val pipe: Boolean, var deletePending: Boolean = false, var listed: Boolean = false)

    fun folder(path: String) = apply { FilesModel.parts(path).indices.forEach { folders += FilesModel.parts(path).subList(0, it + 1).joinToString("/") } }

    fun file(path: String, text: String) = file(path, text.toByteArray())

    fun file(path: String, bytes: ByteArray) = apply {
        folder(FilesModel.parent(path))
        files[FilesModel.join("", path)] = bytes
    }

    override fun send(message: ByteArray) {
        if (dropNext) {
            dropNext = false
            throw java.io.IOException("dropped")
        }
        outbox += handle(message)
    }

    override fun receive(): ByteArray = outbox.removeFirst()

    override fun close() {
        closed = true
    }

    private fun normal(wire: String) = wire.replace('\\', '/').trim('/')

    private fun respond(request: ByteReader, status: Long, body: ByteArray, sessionId: Long? = null, treeId: Long? = null): ByteArray {
        val header = ByteWriter()
            .bytes(byteArrayOf(0xFE.toByte(), 'S'.code.toByte(), 'M'.code.toByte(), 'B'.code.toByte()))
            .u16(64).u16(1).u32(status).u16(request.u16(12)).u16(64).u32(Smb2.FLAG_RESPONSE).u32(0)
            .u64(request.u64(24)).u32(0).u32(treeId ?: request.u32(36)).u64(sessionId ?: request.u64(40)).zeros(16)
        return header.toByteArray() + body
    }

    private fun error(request: ByteReader, status: Long) = respond(request, status, ByteWriter().u16(9).u16(0).u32(0).u8(0).toByteArray())

    private fun handle(message: ByteArray): ByteArray {
        val r = ByteReader(message)
        val command = r.u16(12)
        commands += command
        val key = sessionKey
        if (key != null && command != Smb2.SESSION_SETUP && command != Smb2.NEGOTIATE) {
            val signed = (r.u32(16) and Smb2.FLAG_SIGNED) != 0L
            val copy = message.copyOf().also { java.util.Arrays.fill(it, 48, 64, 0) }
            val expected = Ntlm.hmacSha256(key, copy).copyOf(16)
            if (!signed || !expected.contentEquals(message.copyOfRange(48, 64))) {
                badSignatures += command
                return error(r, SmbStatus.ACCESS_DENIED)
            }
        }
        val b = 64
        return when (command) {
            Smb2.NEGOTIATE -> {
                val count = r.u16(b + 2)
                val offered = (0 until count).map { r.u16(b + 36 + it * 2) }
                if (dialect !in offered) return error(r, SmbStatus.NOT_SUPPORTED)
                respond(r, 0, ByteWriter().u16(65).u16(if (signingRequired) 3 else 1).u16(dialect).u16(0).zeros(16).u32(0)
                    .u32(65536).u32(maxRead).u32(maxRead).u64(0).u64(0).u16(128).u16(0).u32(0).toByteArray())
            }
            Smb2.SESSION_SETUP -> sessionSetup(r)
            Smb2.TREE_CONNECT -> {
                val path = fromUtf16(r.bytes(r.u16(b + 4), r.u16(b + 6)))
                val share = path.substringAfterLast('\\')
                if (shareNames.none { it.first.equals(share, true) }) return error(r, SmbStatus.BAD_NETWORK_NAME)
                val id = nextTree++
                trees[id] = share
                respond(r, 0, ByteWriter().u16(16).u8(if (share == "IPC$") 2 else 1).u8(0).u32(0).u32(0).u32(0x1F01FF).toByteArray(), treeId = id)
            }
            Smb2.CREATE -> create(r)
            Smb2.CLOSE -> {
                val h = handles.remove(r.u64(b + 16))
                if (h != null && h.deletePending) {
                    files.remove(h.path)
                    folders.remove(h.path)
                }
                respond(r, 0, ByteWriter().u16(60).zeros(58).toByteArray())
            }
            Smb2.QUERY_DIRECTORY -> {
                val h = handles[r.u64(b + 16)] ?: return error(r, SmbStatus.ACCESS_DENIED)
                if (h.listed && (r.u8(b + 3) and 1) == 0) return error(r, SmbStatus.NO_MORE_FILES)
                h.listed = true
                val names = listOf("." to true, ".." to true) + children(h.path)
                val out = ByteWriter()
                names.forEachIndexed { index, (name, folder) ->
                    val nameBytes = utf16(name)
                    val entry = ByteWriter().u32(0).u32(0).u64(0).u64(0).u64(133_000_000_000_000_000L).u64(0)
                        .u64(if (folder) 0 else files[FilesModel.join(h.path, name)]!!.size.toLong()).u64(0)
                        .u32(if (folder) 0x10L else if (name.startsWith("~")) 0x22L else 0x20L).u32(nameBytes.size).bytes(nameBytes).align(8).toByteArray()
                    val last = index == names.lastIndex
                    if (!last) ByteWriter().u32(entry.size).toByteArray().copyInto(entry, 0)
                    out.bytes(entry)
                }
                val data = out.toByteArray()
                respond(r, 0, ByteWriter().u16(9).u16(b + 8).u32(data.size).bytes(data).toByteArray())
            }
            Smb2.READ -> {
                val length = r.u32(b + 4).toInt()
                val offset = r.u64(b + 8)
                val h = handles[r.u64(b + 24)] ?: return error(r, SmbStatus.ACCESS_DENIED)
                val data = if (h.pipe) {
                    val chunk = pipeOutput.copyOf(minOf(length, pipeOutput.size))
                    pipeOutput = pipeOutput.copyOfRange(chunk.size, pipeOutput.size)
                    chunk
                } else {
                    val content = files[h.path] ?: return error(r, SmbStatus.OBJECT_NAME_NOT_FOUND)
                    if (offset >= content.size) return error(r, SmbStatus.END_OF_FILE)
                    content.copyOfRange(offset.toInt(), minOf(content.size, offset.toInt() + minOf(length, maxRead)))
                }
                respond(r, 0, ByteWriter().u16(17).u8(b + 16).u8(0).u32(data.size).u32(0).u32(0).bytes(data).toByteArray())
            }
            Smb2.WRITE -> {
                val dataOffset = r.u16(b + 2)
                val length = r.u32(b + 4).toInt()
                val offset = r.u64(b + 8).toInt()
                val h = handles[r.u64(b + 24)] ?: return error(r, SmbStatus.ACCESS_DENIED)
                val data = r.bytes(dataOffset, length)
                if (h.pipe) {
                    pipeOutput = rpc(data)
                } else {
                    val current = files[h.path] ?: ByteArray(0)
                    val grown = current.copyOf(maxOf(current.size, offset + length))
                    data.copyInto(grown, offset)
                    files[h.path] = grown
                }
                respond(r, 0, ByteWriter().u16(17).u16(0).u32(length).u32(0).u16(0).u16(0).toByteArray())
            }
            Smb2.SET_INFO -> {
                val infoClass = r.u8(b + 3)
                val buffer = r.bytes(r.u16(b + 8), r.u32(b + 4).toInt())
                val h = handles[r.u64(b + 24)] ?: return error(r, SmbStatus.ACCESS_DENIED)
                when (infoClass) {
                    13 -> {
                        if (h.path in folders && children(h.path).isNotEmpty()) return error(r, SmbStatus.DIRECTORY_NOT_EMPTY)
                        h.deletePending = true
                    }
                    10 -> {
                        val length = ByteReader(buffer).u32(16).toInt()
                        val target = normal(fromUtf16(buffer.copyOfRange(20, 20 + length)))
                        if (target in files || target in folders) return error(r, SmbStatus.OBJECT_NAME_COLLISION)
                        files.remove(h.path)?.let { files[target] = it }
                        if (h.path in folders) {
                            val moved = folders.filter { FilesModel.isInside(it, h.path) }
                            moved.forEach { folders.remove(it); folders += target + it.removePrefix(h.path) }
                            files.keys.filter { FilesModel.isInside(it, h.path) }.forEach { files[target + it.removePrefix(h.path)] = files.remove(it)!! }
                        }
                    }
                    else -> return error(r, SmbStatus.NOT_SUPPORTED)
                }
                respond(r, 0, ByteWriter().u16(2).toByteArray())
            }
            Smb2.LOGOFF, Smb2.TREE_DISCONNECT -> respond(r, 0, ByteWriter().u16(4).u16(0).toByteArray())
            else -> error(r, SmbStatus.NOT_SUPPORTED)
        }
    }

    private fun children(path: String): List<Pair<String, Boolean>> =
        folders.filter { it.isNotEmpty() && FilesModel.parent(it) == path }.map { FilesModel.name(it) to true } +
            files.keys.filter { FilesModel.parent(it) == path }.map { FilesModel.name(it) to false }

    private fun sessionSetup(r: ByteReader): ByteArray {
        val b = 64
        val token = r.bytes(r.u16(b + 12), r.u16(b + 14))
        val start = Ntlm.findMessage(token)
        val ntlm = token.copyOfRange(start, token.size)
        val m = ByteReader(ntlm)
        if (m.u32(8) == 1L) {
            val targetInfo = ByteWriter().u16(2).u16(12).bytes(utf16("Domain")).u16(1).u16(12).bytes(utf16("Server")).u16(0).u16(0).toByteArray()
            val challenge = ByteWriter().bytes(Ntlm.signature).u32(2).u16(0).u16(0).u32(56).u32(Ntlm.baseFlags).bytes(serverChallenge).zeros(8)
                .u16(targetInfo.size).u16(targetInfo.size).u32(56).zeros(8).bytes(targetInfo).toByteArray()
            return respond(r, SmbStatus.MORE_PROCESSING_REQUIRED, ByteWriter().u16(9).u16(0).u16(b + 8).u16(challenge.size).bytes(challenge).toByteArray(), sessionId = 0x1234)
        }
        fun field(at: Int) = m.bytes(m.u32(at + 4).toInt(), m.u16(at))
        val nt = field(20)
        val domain = fromUtf16(field(28))
        val name = fromUtf16(field(36))
        lastUser = name
        fun ok(flags: Int) = respond(r, 0, ByteWriter().u16(9).u16(flags).u16(0).u16(0).toByteArray())
        if (nt.isEmpty() && name.isEmpty()) {
            if (!allowAnonymous) return error(r, SmbStatus.LOGON_FAILURE)
            authenticatedGuest = true
            return ok(2)
        }
        if (name == "guest" && allowGuest) {
            authenticatedGuest = true
            return ok(1)
        }
        if (name != user) return error(r, SmbStatus.LOGON_FAILURE)
        val key = Ntlm.ntowfV2(password, name, domain)
        val blob = nt.copyOfRange(16, nt.size)
        val proof = Ntlm.hmacMd5(key, serverChallenge, blob)
        if (!proof.contentEquals(nt.copyOf(16))) return error(r, SmbStatus.LOGON_FAILURE)
        if (signingRequired) sessionKey = Ntlm.hmacMd5(key, proof)
        return ok(0)
    }

    private fun create(r: ByteReader): ByteArray {
        val b = 64
        val share = trees[r.u32(36)] ?: return error(r, SmbStatus.ACCESS_DENIED)
        val disposition = r.u32(b + 36).toInt()
        val options = r.u32(b + 40).toInt()
        val nameLength = r.u16(b + 46)
        val path = normal(if (nameLength == 0) "" else fromUtf16(r.bytes(r.u16(b + 44), nameLength)))
        if (share == "IPC$") {
            if (path != "srvsvc") return error(r, SmbStatus.OBJECT_NAME_NOT_FOUND)
            val id = nextHandle++
            handles[id] = Handle(path, true)
            return created(r, id, 0x80, 0)
        }
        if (FilesModel.parent(path) !in folders) return error(r, SmbStatus.OBJECT_PATH_NOT_FOUND)
        val isFolder = path in folders
        val exists = isFolder || path in files
        when (disposition) {
            Smb2.FILE_OPEN -> if (!exists) return error(r, SmbStatus.OBJECT_NAME_NOT_FOUND)
            Smb2.FILE_CREATE -> if (exists) return error(r, SmbStatus.OBJECT_NAME_COLLISION)
        }
        if ((options and Smb2.DIRECTORY_FILE) != 0 && exists && !isFolder) return error(r, SmbStatus.NOT_SUPPORTED)
        if ((options and Smb2.NON_DIRECTORY_FILE) != 0 && isFolder) return error(r, SmbStatus.ACCESS_DENIED)
        if (!exists) {
            if ((options and Smb2.DIRECTORY_FILE) != 0) folders += path else files[path] = ByteArray(0)
        } else if (disposition == Smb2.FILE_OVERWRITE_IF && !isFolder) {
            files[path] = ByteArray(0)
        }
        val id = nextHandle++
        handles[id] = Handle(path, false)
        val folder = path in folders
        return created(r, id, if (folder) 0x10 else 0x20, files[path]?.size?.toLong() ?: 0)
    }

    private fun created(r: ByteReader, id: Long, attributes: Long, size: Long): ByteArray = respond(
        r,
        0,
        ByteWriter().u16(89).u8(0).u8(0).u32(1).u64(0).u64(0).u64(133_000_000_000_000_000L).u64(0).u64(size).u64(size)
            .u32(attributes).u32(0).u64(id).u64(id).u32(0).u32(0).toByteArray(),
    )

    private fun rpc(pdu: ByteArray): ByteArray {
        val r = ByteReader(pdu)
        val type = r.u8(2)
        val callId = r.u32(12)
        fun wrap(ptype: Int, body: ByteArray) = ByteWriter().u8(5).u8(0).u8(ptype).u8(3).u8(0x10).zeros(3)
            .u16(16 + body.size).u16(0).u32(callId).bytes(body).toByteArray()
        if (type == 11) return wrap(12, ByteWriter().u16(4280).u16(4280).u32(0x1234).u16(0).align(4).u8(1).zeros(3).u16(0).u16(0).zeros(20).toByteArray())
        val stub = ByteWriter().u32(1).u32(1).u32(0x20000).u32(shareNames.size).u32(0x20004).u32(shareNames.size)
        shareNames.forEachIndexed { i, (_, type) -> stub.u32(0x20008L + i * 8).u32(type).u32(0x2000CL + i * 8) }
        for ((name, _) in shareNames) {
            for (text in listOf(name, "remark")) {
                val chars = utf16(text + "\u0000")
                stub.align(4).u32(chars.size / 2).u32(0).u32(chars.size / 2).bytes(chars)
            }
        }
        stub.align(4).u32(shareNames.size).u32(0).u32(0)
        val data = stub.toByteArray()
        return wrap(2, ByteWriter().u32(data.size).u16(0).u8(0).u8(0).bytes(data).toByteArray())
    }
}
