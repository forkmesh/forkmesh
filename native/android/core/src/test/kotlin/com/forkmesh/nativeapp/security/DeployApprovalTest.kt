package com.forkmesh.nativeapp.security

import com.forkmesh.nativeapp.crypto.Ed25519
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect
import java.util.Base64

class DeployApprovalTest {
    private val identity = Identity.fromSeed(ByteArray(32) { it.toByte() })
    private val ts = 1791500000000L

    fun testPullApprovalMatchesGoldenVector() {
        assertEq("A6EHv_POEL4dcN0Y50vAmWfk1jCbpQ1fHdyGZBJVMbg", identity.publicKeyB64url)
        val requestId = "mobile_0123456789abcdef0123456789abcdef0123"
        val message = DeployApproval.message(DeployApproval.PULL, "ForkMesh", "forkmesh", "42", "a".repeat(40), "b".repeat(40), requestId, ts)
        assertEq(
            "forkmesh-deploy-approval-v1\npull\nforkmesh\nforkmesh\n42\n${"a".repeat(40)}\n${"b".repeat(40)}\n$requestId\n1791500000000",
            String(message, Charsets.UTF_8),
        )
        val approval = DeployApproval.sign(identity, DeployApproval.PULL, "ForkMesh", "forkmesh", "42", "a".repeat(40), "b".repeat(40), requestId, ts)
        assertEq(listOf("publicKey", "ts", "signature"), approval.keys.toList())
        assertEq("A6EHv_POEL4dcN0Y50vAmWfk1jCbpQ1fHdyGZBJVMbg", approval["publicKey"])
        assertEq(ts, approval["ts"])
        assertEq("74RN64qcUus-AwkE42AcAMoJKR9EcXQ9Dp32W5yp9tFubzSj8fQK5NeQWN5t-gull9AnAD82G1ef1cR77_DwBw", approval["signature"])
        val signature = Base64.getUrlDecoder().decode(approval["signature"] as String)
        expect(Ed25519.verify(identity.publicKeyBytes, message, signature))
    }

    fun testBranchApprovalMatchesGoldenVector() {
        val message = DeployApproval.message(DeployApproval.BRANCH, "forkmesh", "forkmesh", "sess-1", "", "", "mobile_req", ts)
        assertEq("forkmesh-deploy-approval-v1\nbranch\nforkmesh\nforkmesh\nsess-1\n\n\nmobile_req\n1791500000000", String(message, Charsets.UTF_8))
        val approver = DeployApprover({ identity }, clock = { ts })
        val approval = approver.approve(DeployApproval.BRANCH, "forkmesh", "forkmesh", "sess-1", "", "", "mobile_req")!!
        assertEq("pUSma40Gi6pAd31NUblKmuZqxZ3ZNlOMQKWV7WchNhMPrxKdudlpzRA_02zGnF3PgCrcMZ4d3qLgv2Rj5u7xAg", approval["signature"])
    }

    fun testMissingOrBrokenKeyGivesNoApproval() {
        assertEq(null, DeployApprover({ null }).approve(DeployApproval.PULL, "a", "b", "1", "", "", "r"))
        assertEq(null, DeployApprover({ throw IllegalStateException("locked") }).approve(DeployApproval.PULL, "a", "b", "1", "", "", "r"))
    }
}
