package com.forkmesh.nativeapp.account

import com.forkmesh.nativeapp.model.auth.AuthSession
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect
import kotlin.coroutines.Continuation
import kotlin.coroutines.EmptyCoroutineContext
import kotlin.coroutines.startCoroutine

class GuestGateLogicTest {
    private fun <T> runSuspend(block: suspend () -> T): T {
        var outcome: Result<T>? = null
        block.startCoroutine(Continuation(EmptyCoroutineContext) { outcome = it })
        return (outcome ?: throw AssertionError("the block suspended")).getOrThrow()
    }

    private fun session(): AuthSession = AuthSession.fromJson(
        mapOf(
            "nodeName" to "demo-node",
            "email" to "demo@example.com",
            "status" to "active",
            "emailVerified" to true,
            "capabilities" to listOf("submit_issue", "submit_pr"),
        ),
    )

    fun testAHarnessWithoutAuthBehavesAsSignedIn() {
        expect(!isGuestMode(authAvailable = false, isGuest = true))
        expect(isGuestMode(authAvailable = true, isGuest = true))
        expect(!isGuestMode(authAvailable = true, isGuest = false))
    }

    fun testLetsASignedInUserStraightThrough() {
        var sheets = 0
        val allowed = runSuspend {
            requireAccount(
                guest = false,
                showSheet = { sheets++; null },
                openSignIn = { throw AssertionError("no sign-in for a signed-in user") },
                isAuthenticated = { true },
            )
        }
        expect(allowed)
        assertEq(0, sheets)
    }

    fun testAsksAGuestToSignInAndBlocksTheWrite() {
        var sheets = 0
        var signIns = 0
        val allowed = runSuspend {
            requireAccount(
                guest = true,
                showSheet = { sheets++; null },
                openSignIn = { signIns++ },
                isAuthenticated = { false },
            )
        }
        expect(!allowed)
        assertEq(1, sheets)
        assertEq(0, signIns)
    }

    fun testLogInOpensTheLoginPageAndBackReturnsWithoutTheWrite() {
        val opened = ArrayList<AuthFlowStart>()
        val allowed = runSuspend {
            requireAccount(
                guest = true,
                showSheet = { AuthFlowStart.Login },
                openSignIn = { opened.add(it) },
                isAuthenticated = { false },
            )
        }
        expect(!allowed)
        assertEq(listOf(AuthFlowStart.Login), opened)
    }

    fun testSigningInFromTheSheetLetsTheWriteContinue() {
        var signedIn = false
        val allowed = runSuspend {
            requireAccount(
                guest = true,
                showSheet = { AuthFlowStart.Signup },
                openSignIn = { signedIn = true },
                isAuthenticated = { signedIn },
            )
        }
        expect(allowed)
    }

    fun testGuestSettingsOfferSignInInsteadOfSignOut() {
        assertEq(AccountCard.Guest, accountCard(guest = true))
        assertEq(AccountCard.SignOut, accountCard(guest = false))
        val profile = profileCardText(session = null, guest = true)
        assertEq("Guest", profile.title)
        assertEq("Exploring read-only without an account", profile.subtitle)
        expect(!showsDeleteAccount(null))
        expect(!showsPushCard(supported = true, guest = true))
    }

    fun testSignedInSettingsShowTheAccount() {
        val s = session()
        assertEq("demo-node", signedInAsName(s))
        assertEq(ProfileCardText("demo-node", "demo@example.com"), profileCardText(s, guest = false))
        expect(showsDeleteAccount(s))
    }

    fun testAGuestGoesByAStableGuestNameNotItsNodeKey() {
        assertEq("Guest 3813", guestChatName("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"))
        assertEq("Guest 4804", guestChatName("demo-key"))
        assertEq("Guest", guestChatName(""))
    }
}
