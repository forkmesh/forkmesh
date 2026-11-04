package com.forkmesh.nativeapp.reticulum

import com.forkmesh.nativeapp.testing.expect
import java.io.File

private fun pause(timeoutMs: Long, condition: () -> Boolean): Boolean {
    val deadline = System.currentTimeMillis() + timeoutMs
    while (!condition()) {
        if (System.currentTimeMillis() > deadline) return false
        Thread.sleep(50)
    }
    return true
}

private fun report(ok: Boolean, text: String): Boolean {
    println("${if (ok) "PASS" else "FAIL"} $text")
    return ok
}

class ReticulumInteropTest {
    fun testInterop() {
        val tcp = System.getenv("RETICULUM_INTEROP_TCP").orEmpty()
        val sharedPort = System.getenv("RETICULUM_INTEROP_SHARED_PORT").orEmpty()
        val target = Rns.parseHash(System.getenv("RETICULUM_INTEROP_TARGET").orEmpty())
        val out = System.getenv("RETICULUM_INTEROP_OUT").orEmpty()
        if ((tcp.isEmpty() && sharedPort.isEmpty()) || target == null) return
        val node = ReticulumNode()
        var ok = true
        try {
            node.setIdentity(RnsIdentity.generate())
            node.setAnnounceName("ForkMesh interop")
            val config = if (sharedPort.isNotEmpty()) {
                InterfaceConfig(id = "interop", name = "Python RNS", kind = InterfaceKind.SharedInstance, port = sharedPort.toInt())
            } else {
                InterfaceConfig(id = "interop", name = "Python RNS", host = tcp.substringBefore(':'), port = tcp.substringAfter(':').toInt())
            }
            node.setInterfaces(listOf(config))
            node.start()
            ok = report(pause(10000) { node.onlineCount() == 1 }, "Interop: connected to Python RNS over ${config.kindLabel} ${config.endpoint}") && ok
            ok = report(pause(20000) { node.destinations().values.any { it.displayName == "Python peer" && it.kind == "peer" } }, "Interop: Python RNS announce received, validated and named") && ok
            node.forgetDestination(target)
            ok = report(node.requestPath(target) != null, "Interop: path request sent") && ok
            ok = report(pause(20000) { (node.lookups()[Rns.hex(target)]?.answered ?: 0L) > 0L }, "Interop: Python RNS answered the path request") && ok
            ok = report(node.announce(), "Interop: announced this node") && ok
            File(out).writeText(Rns.hex(node.destinationHash()))
            pause(20000) { node.counters().pathRequestsAnswered > 0 }
            ok = report(node.counters().pathRequestsAnswered > 0, "Interop: answered Python's path request for this node") && ok
            Thread.sleep(1500)
            val status = node.status(config.id)
            val counters = node.counters()
            println("INTEROP rx=${status.rxBytes} tx=${status.txBytes} announces=${counters.announces} invalid=${counters.invalid} rejected=${counters.rejectedAnnounces}")
        } finally {
            node.close()
        }
        expect(ok, "Interop checks failed")
    }

    fun testLxmfInterop() {
        val tcp = System.getenv("RETICULUM_LXMF_TCP").orEmpty()
        val peer = Rns.parseHash(System.getenv("RETICULUM_LXMF_PEER").orEmpty())
        val out = System.getenv("RETICULUM_LXMF_OUT").orEmpty()
        if (tcp.isEmpty() || peer == null) return
        val node = ReticulumNode()
        val messenger = ReticulumMessenger(node)
        var ok = true
        try {
            node.setIdentity(RnsIdentity.generate())
            messenger.setDisplayName("ForkMesh Android")
            node.announceHandler = { messenger.destinationAnnounced(it) }
            node.packetHandler = { id, packet, raw, hops -> messenger.handlePacket(id, packet, raw, hops) }
            node.setInterfaces(
                listOf(
                    InterfaceConfig(
                        id = "lxmf", name = "Python LXMF", host = tcp.substringBefore(':'), port = tcp.substringAfter(':').toInt(),
                        networkName = System.getenv("RETICULUM_LXMF_NETNAME").orEmpty(), passphrase = System.getenv("RETICULUM_LXMF_PASSPHRASE").orEmpty(),
                    ),
                ),
            )
            node.start()
            ok = report(pause(10000) { node.onlineCount() == 1 }, "LXMF: connected") && ok
            ok = report(pause(20000) { node.destination(peer) != null }, "LXMF: Python peer announce heard (stamp cost ${messenger.peerStampCost(peer)})") && ok
            node.announce()
            File(out).writeText(Rns.hex(messenger.address()))
            val longText = (0 until 400).joinToString("") { "line $it of a long message from ForkMesh\n" }
            val ids = listOf(
                messenger.send(peer, "Hello from ForkMesh", "short"),
                messenger.send(peer, "m".repeat(300), "medium"),
                messenger.send(peer, longText, "long"),
            )
            pause(120000) { ids.all { messenger.message(it)?.state == MessageState.Delivered } }
            for (id in ids) {
                val message = messenger.message(id)
                ok = report(message?.state == MessageState.Delivered, "LXMF: ${message?.title} message delivered by ${message?.method} (${message?.state?.label} ${message?.note})") && ok
            }
            fun received(title: String) = messenger.messages(peer).firstOrNull { !it.outgoing && it.title == title }
            val titles = listOf("py-short", "py-medium", "py-long")
            pause(90000) { titles.all { received(it) != null } }
            for (title in titles) {
                val message = received(title)
                ok = report(message != null && message.verified && message.content.isNotEmpty(),
                    "LXMF: received $title by ${message?.method ?: "-"}, ${message?.content?.length ?: 0} characters, signature ${if (message?.verified == true) "verified" else "NOT verified"}") && ok
            }
            Thread.sleep(3000)
        } finally {
            messenger.close()
            node.close()
        }
        expect(ok, "LXMF interop checks failed")
    }
}
