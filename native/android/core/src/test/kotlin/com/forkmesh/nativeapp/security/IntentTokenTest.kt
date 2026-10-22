package com.forkmesh.nativeapp.security

import com.forkmesh.nativeapp.api.MemoryPrefStore
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class IntentTokenTest {
    fun testTheTokenIsMadeOnceAndKeptForThisInstall() {
        val store = MemoryPrefStore()
        val first = IntentToken(store).value()
        expect(first.length >= 43, first)
        assertEq(first, IntentToken(store).value())
        assertEq(first, store.getString(IntentToken.PREFS_KEY))
        expect(IntentToken(MemoryPrefStore()).value() != first, "each install has its own token")
    }

    fun testOnlyTheInstallTokenIsAccepted() {
        val token = IntentToken(MemoryPrefStore())
        val value = token.value()
        expect(token.accepts(value))
        expect(!token.accepts(null))
        expect(!token.accepts(""))
        expect(!token.accepts(value.dropLast(1)))
        expect(!token.accepts(value + "x"))
        expect(!token.accepts(IntentToken(MemoryPrefStore()).value()))
    }

    fun testAShortStoredValueIsReplaced() {
        val store = MemoryPrefStore()
        store.putString(IntentToken.PREFS_KEY, "short")
        val token = IntentToken(store)
        expect(!token.accepts("short"))
        expect(token.value().length >= 43)
    }
}
