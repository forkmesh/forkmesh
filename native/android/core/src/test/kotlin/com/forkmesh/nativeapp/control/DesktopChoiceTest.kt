package com.forkmesh.nativeapp.control

import com.forkmesh.nativeapp.api.MemoryPrefStore
import com.forkmesh.nativeapp.crypto.Bytes
import com.forkmesh.nativeapp.security.Identity
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.assertFails
import com.forkmesh.nativeapp.testing.expect

class DesktopChoiceTest {
    private val desktop = LocalControlTest.FakeDesktop()
    private val identity = Identity.fromSeed(ByteArray(32) { 9 })
    private val old = PairedDesktop(controlB64(Bytes.random(32)), "old-laptop", "alice")
    private val studio = PairedDesktop(desktop.key, "studio", "alice")
    private val dialled = java.util.Collections.synchronizedList(ArrayList<String>())

    private fun hub(prefs: MemoryPrefStore) = DesktopControlHub(
        null,
        { target ->
            dialled.add(target.name)
            if (target.name == "old-laptop") throw LocalControlException("timeout")
            RelayControlConnection.open(LocalControlTest.LoopChannel(LocalControlTest.FakeDesktop()), identity, target.key, target.name, timeoutMs = 5000)
        },
        accountDesktopsLoader = { listOf(old, studio) },
        prefs = prefs,
    ) { emptyList() }

    fun testSeveralDesktopsShowTheirStatusAndTheChosenOneIsDialledAlone() {
        val prefs = MemoryPrefStore()
        val hub = hub(prefs)
        hub.link()
        assertEq(DesktopReach.Connected(ControlTransport.Relay, "studio"), hub.reach)
        assertEq(
            listOf(
                DesktopChoice(studio.key, "studio", DesktopPresence.Online, active = true),
                DesktopChoice(old.key, "old-laptop", DesktopPresence.Offline, active = false),
            ),
            hub.choices,
        )
        assertEq("Desktop studio, running, in use", hub.choices[0].label)
        assertEq("Desktop old-laptop, not running or not reachable", hub.choices[1].label)
        assertEq(ShellLocalDesktop.Relay("studio"), hub.localDesktop)

        hub.choose(old.key)
        assertEq(old.key, prefs.getString(DesktopControlHub.PREFERRED_KEY))
        assertEq(null, hub.currentLink)
        assertEq("old-laptop", hub.desktop?.name)
        expect(hub.choices.first { it.id == old.key }.active)
        dialled.clear()
        assertFails<LocalControlException> { hub.link() }
        assertEq(listOf("old-laptop"), dialled.toList())

        val again = hub(prefs)
        assertEq(old.key, again.preferredKey)
        again.refreshAccountDesktops()
        assertEq("old-laptop", again.desktop?.name)

        hub.chooseNamed("studio")
        hub.link()
        assertEq(DesktopReach.Connected(ControlTransport.Relay, "studio"), hub.reach)
        hub.checkDesktops(force = true)
        assertEq(DesktopPresence.Offline, hub.presenceOf(old.key))
        assertEq(DesktopPresence.Online, hub.presenceOf(studio.key))
        hub.disconnectRelay()
    }

    fun testAForgottenPreferenceFallsBackToAnyDesktop() {
        val prefs = MemoryPrefStore(mapOf(DesktopControlHub.PREFERRED_KEY to controlB64(Bytes.random(32))))
        val hub = hub(prefs)
        hub.link()
        assertEq(DesktopReach.Connected(ControlTransport.Relay, "studio"), hub.reach)
        expect(hub.currentLink != null)
        hub.disconnectRelay()
    }
}
