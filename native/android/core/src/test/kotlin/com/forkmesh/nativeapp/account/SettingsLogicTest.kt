package com.forkmesh.nativeapp.account

import com.forkmesh.nativeapp.model.auth.AccountDevice
import com.forkmesh.nativeapp.model.auth.AuthException
import com.forkmesh.nativeapp.model.auth.AuthSession
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class SettingsLogicTest {
    private val now = 1_770_000_000_000L

    private fun session(extra: Map<String, Any?> = emptyMap()): AuthSession = AuthSession.fromJson(
        linkedMapOf<String, Any?>("nodeName" to "alice", "email" to "alice@example.com", "status" to "active") + extra,
    )

    private fun device(
        id: String,
        kind: String,
        capabilities: List<String> = listOf("owner_sign"),
        enabled: Boolean = true,
        revokedAt: Long = 0,
        lastSeen: Long = 0,
        label: String = "",
        current: Boolean = false,
    ) = AccountDevice(
        id = id,
        pubkey = "",
        kind = kind,
        label = label,
        capabilities = capabilities,
        enabled = enabled,
        lastSeen = lastSeen,
        revokedAt = revokedAt,
        current = current,
    )

    fun testProfileCardCoversPreviewSignedOutAndEmailOnly() {
        assertEq(
            ProfileCardText(
                "Preview mode",
                "This is local preview data. Sign out and log in to show your Worker account.",
            ),
            profileCardText(session(mapOf("sessionKind" to "preview")), guest = false),
        )
        assertEq(
            ProfileCardText("Not signed in", "No Worker account session found on this device"),
            profileCardText(null, guest = false),
        )
        assertEq(
            ProfileCardText("bob@example.com", "bob@example.com"),
            profileCardText(AuthSession.fromJson(mapOf("email" to "bob@example.com")), guest = false),
        )
    }

    fun testDisplayNameAdoptsTheAccountOnlyWhenEmptyOrPreview() {
        assertEq("alice", adoptedDisplayName("", session()))
        assertEq("alice", adoptedDisplayName("preview-node", session()))
        assertEq(null, adoptedDisplayName("Alice W", session()))
        assertEq(null, adoptedDisplayName("", null))
    }

    fun testFundingStatus() {
        val none = fundingStatus(session())
        expect(!none.connected)
        assertEq("No payout address connected", none.title)
        expect(none.detail.startsWith("Add a public Solana address only"))
        val connected = fundingStatus(session(mapOf("hasPayoutAddress" to true, "solana" to " So1ana ")))
        expect(connected.connected)
        assertEq("Payout address connected", connected.title)
        assertEq("So1ana", connected.detail)
    }

    fun testThisDeviceCardStates() {
        assertEq(ThisDeviceCard.SignInToRegister, thisDeviceCard(null, now))
        assertEq(ThisDeviceCard.SignInToRegister, thisDeviceCard(session(mapOf("sessionKind" to "preview")), now))
        assertEq(ThisDeviceCard.NotRegistered, thisDeviceCard(session(), now))

        val bound = session(
            mapOf(
                "sessionKind" to "mobile_node",
                "deviceId" to "abcdef0123456789",
                "capabilities" to listOf("publish_repo", "owner_sign"),
                "devices" to listOf(
                    mapOf(
                        "id" to "abcdef0123456789",
                        "kind" to "mobile_node",
                        "label" to " Pixel 9 · Android 16 ",
                        "capabilities" to listOf("publish_repo", "owner_sign"),
                        "lastSeen" to now - 5 * 60_000,
                    ),
                ),
            ),
        )
        assertEq(
            ThisDeviceCard.MobileNode(
                label = "Pixel 9 · Android 16",
                deviceId = "abcdef…6789",
                capabilities = "Publishes · signs as owner",
                lastSeen = "5m ago",
                canServeAsNode = true,
            ),
            thisDeviceCard(bound, now),
        )

        val rowless = session(mapOf("sessionKind" to "mobile_node", "capabilities" to listOf("owner_sign")))
        assertEq(
            ThisDeviceCard.MobileNode("This phone", "—", "Signs as owner", "now", canServeAsNode = true),
            thisDeviceCard(rowless, now),
        )
    }

    fun testShortFormsAndCounts() {
        assertEq("Browse only", capabilitySummary(emptyList()))
        assertEq("Publishes", capabilitySummary(listOf("publish_repo", "host_repo")))
        assertEq("short", shortDeviceId("short"))
        assertEq("0123456789", shortDeviceId("0123456789"))
        assertEq("abcdefghi...pqrstu", shortDeviceKey("abcdefghijklmnopqrstu"))
        assertEq("18-characters-key!", shortDeviceKey("18-characters-key!"))
        assertEq("1 desktop node", countLabel(1, "desktop node"))
        assertEq("0 mobile nodes", countLabel(0, "mobile node"))
    }

    fun testNodesSummaryPrefersTheRefreshedRegistryAndSkipsRevokedNodes() {
        val cached = session(
            mapOf("devices" to listOf(mapOf("id" to "d1", "kind" to "desktop_node", "capabilities" to listOf("host_repo")))),
        )
        val fromLogin = nodesSummary(null, cached)
        assertEq("1 desktop node · 0 mobile nodes", fromLogin.headline)
        expect(fromLogin.showsDesktopNote)

        val refreshed = nodesSummary(
            listOf(
                device("d1", "desktop_node", revokedAt = 5),
                device("m1", "mobile_node"),
                device("m2", "mobile_node", enabled = false),
            ),
            cached,
        )
        assertEq("0 desktop nodes · 1 mobile node", refreshed.headline)
        expect(!refreshed.showsDesktopNote)

        val empty = nodesSummary(emptyList(), cached)
        assertEq("No nodes registered yet", empty.headline)
        assertEq("Loading nodes…", nodesEmptyText(loading = true))
        expect(nodesEmptyText(loading = false).startsWith("Log in from the ForkMesh desktop app"))
    }

    fun testDeviceRows() {
        val bound = session(mapOf("deviceId" to "m1"))
        val mine = device("m1", "mobile_node", label = "Pixel", lastSeen = now - 2 * 3_600_000)
        val other = device("d1", "desktop_node", capabilities = listOf("host_repo"), current = false)
        expect(isCurrentDevice(mine, bound))
        expect(!isCurrentDevice(other, bound))
        expect(isCurrentDevice(other.copy(current = true), session()))
        assertEq("Pixel (this device)", deviceRowTitle(mine, isCurrent = true))
        assertEq("Desktop node", deviceRowTitle(other, isCurrent = false))
        assertEq("Mobile node · Owner signing", deviceRowDetail(mine))
        assertEq("Last seen 2h ago", deviceRowTimeline(mine, now))
        assertEq("Revoked just now", deviceRowTimeline(mine.copy(revokedAt = now), now))
        assertEq(null, deviceRowTimeline(other, now))
        assertEq(DeviceTone.Success, deviceTone(mine))
        assertEq(DeviceTone.Warning, deviceTone(other))
        assertEq(DeviceTone.Offline, deviceTone(mine.copy(enabled = false)))
    }

    fun testWebSettingsUrlFollowsTheRelayOrigin() {
        assertEq(
            "https://app.forkmesh.com/dashboard/settings",
            webSettingsUrl("wss://app.forkmesh.com/api/repo/mainnode/forkmesh/rooms/world-general/ws"),
        )
        assertEq("http://localhost:8787/dashboard/settings", webSettingsUrl("ws://localhost:8787/api/x/ws"))
        assertEq("https://staging.forkmesh.com/dashboard/settings", webSettingsUrl("wss://staging.forkmesh.com:443/ws"))
        assertEq("http://test.local/dashboard/settings", webSettingsUrl("ws://test.local:80/ws"))
        assertEq("https://app.forkmesh.com/dashboard/settings", webSettingsUrl(""))
        assertEq("https://app.forkmesh.com/dashboard/settings", webSettingsUrl("not a url"))
    }

    fun testServeAsNodeNote() {
        expect(serveAsNodeNote(true, "ForkMesh").startsWith("Read this account's inbox"))
        assertEq(
            "Home lab does not serve the node drain, so this phone cannot act as a node there. Switch to a relay that does.",
            serveAsNodeNote(false, "Home lab"),
        )
        expect(serveAsNodeNote(false, "").startsWith("This relay does not serve"))
    }

    fun testPushLabels() {
        expect(showsPushCard(supported = true, guest = false))
        expect(!showsPushCard(supported = false, guest = false))
        assertEq("Allowed", pushPermissionLabel("authorized"))
        assertEq("Turned off in iOS Settings", pushPermissionLabel("denied"))
        assertEq("Not checked yet", pushPermissionLabel("unknown"))
        assertEq("Not requested", pushTokenLabel("", "denied", "", false))
        assertEq("Waiting for Apple to issue one", pushTokenLabel("", "authorized", "", false))
        assertEq("Registered with the relay (APNs sandbox)", pushTokenLabel("t", "authorized", "development", true))
        assertEq("Issued, not yet registered (APNs production)", pushTokenLabel("t", "authorized", "production", false))
        assertEq("Unknown until the next registration", pushRelayLabel(null))
        assertEq("No APNs credentials configured", pushRelayLabel(false))
        expect(pushCanEnable("", "notDetermined"))
        expect(!pushCanEnable("", "denied"))
        expect(!pushCanEnable("t", "authorized"))
        assertEq("Sending…", pushSendLabel(true))
    }

    fun testAccountCardAndDeleteAccountRules() {
        assertEq("Current account", signedInAsName(null))
        assertEq("bob@example.com", signedInAsName(AuthSession.fromJson(mapOf("email" to "bob@example.com"))))
        expect(!showsDeleteAccount(session(mapOf("sessionKind" to "preview"))))
        assertEq("Type DELETE to confirm account deletion.", deleteAccountConfirmError("delete"))
        assertEq(null, deleteAccountConfirmError(" DELETE "))
        assertEq(
            "Incorrect password. Check it and try again.",
            deleteAccountFailure(AuthException("bad_password", "Incorrect password. Check it and try again.")),
        )
        assertEq("Could not delete account. Check your password and try again.", deleteAccountFailure(RuntimeException("x")))
    }
}
