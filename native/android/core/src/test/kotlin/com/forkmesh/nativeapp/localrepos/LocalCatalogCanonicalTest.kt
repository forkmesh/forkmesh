package com.forkmesh.nativeapp.localrepos

import com.forkmesh.nativeapp.git.bytes.encodeLowerHex
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.assertFails
import com.forkmesh.nativeapp.testing.expect
import java.security.MessageDigest

class LocalCatalogCanonicalTest {
    private val pinnedRecordJson =
        "{\"activityWeeks\":[" + List(52) { "0" }.joinToString(",") + "],\"artifactCount\":\"\"," +
            "\"branch\":\"main\",\"branchCount\":\"1\",\"channel\":\"#alice-demo-repo\"," +
            "\"cloneUrl\":\"https://forkmesh.test/alice/demo-repo\",\"clonesServed\":\"\"," +
            "\"commit\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"," +
            "\"commitAt\":\"1750000000\",\"commitAuthorName\":\"Alice\",\"commitCount\":\"3\"," +
            "\"commitSubject\":\"Initial commit\"," +
            "\"description\":\"Notes \\u2014 with \\u00fcnicode\",\"discussionCount\":\"\"," +
            "\"encryptedManifestHash\":\"\",\"encryptedManifestSig\":\"\",\"hostedSince\":\"\"," +
            "\"issueCount\":\"\",\"issueMaxNumber\":\"\",\"keyEpoch\":0," +
            "\"lastSync\":\"1750000000000\",\"logoMetadata\":{\"description\":\"\"," +
            "\"fileStructure\":[],\"frameworks\":[],\"languages\":{},\"projectCategory\":\"\"," +
            "\"topics\":[]},\"machineName\":\"Alice iPhone\"," +
            "\"maintainer\":\"MAINTAINER-PUBKEY\",\"mirrorEncryption\":\"\",\"name\":" +
            "\"demo-repo\",\"nodeId\":\"0123456789abcdef0123456789abcdef\"," +
            "\"opaqueRepoId\":\"\",\"owner\":\"alice\",\"platform\":\"mobile\",\"pullCount\":\"\"," +
            "\"rootCommit\":\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\",\"sizeBytes\":0," +
            "\"solana\":\"\",\"source\":\"local-node\",\"stateHash\":\"\",\"stateSig\":\"\"," +
            "\"updatedAt\":\"1750000000000\",\"version\":\"\",\"visibility\":\"public\"," +
            "\"websiteServed\":\"\",\"worktreeCount\":\"\"}"

    private val pinnedRecordHash = "56fe71ebe2b9f3b801fd4035248f07b474e8392e4bfeb5dd17c31ef9bafc4ff9"

    private val unicodeDescription = "Notes \u2014 with \u00fcnicode"

    private fun pinnedRecord(): LinkedHashMap<String, Any?> = buildCatalogRecord(
        owner = "alice",
        name = "demo-repo",
        description = unicodeDescription,
        isPublic = true,
        branch = "main",
        maintainer = "MAINTAINER-PUBKEY",
        nodeId = "0123456789abcdef0123456789abcdef",
        updatedAt = 1750000000000,
        head = PublishHead(
            commit = "a".repeat(40),
            rootCommit = "b".repeat(40),
            subject = "Initial commit",
            authorName = "Alice",
            at = 1750000000000,
            commitCount = 3,
            branchCount = 1,
        ),
        machineName = "Alice iPhone",
        cloneUrl = "https://forkmesh.test/alice/demo-repo",
    )

    fun testSortsKeysUsesCompactSeparatorsAndEmitsNoWhitespace() {
        val value = linkedMapOf<String, Any?>(
            "b" to 1,
            "a" to "x \u00e9\n\"q\"",
            "c" to listOf(true, linkedMapOf("z" to 0, "y" to "\u2603")),
        )
        assertEq(
            "{\"a\":\"x \\u00e9\\n\\\"q\\\"\",\"b\":1,\"c\":[true,{\"y\":\"\\u2603\",\"z\":0}]}",
            canonicalJson(value),
        )
        assertEq("2b035bece1e81a3df24f58805e2cdd882d04c72b3ddd9f9fa474099c637da385", catalogRecordHash(value))
    }

    fun testEscapesControlCharactersTheWayPyEncodeBasestringAsciiDoes() {
        assertEq("\"\\b\\f\\n\\r\\t\\u0001\\\\/\"", canonicalJson("\b\u000C\n\r\t\u0001\\/"))
        assertEq("\"\\ud83d\\ude00\"", canonicalJson("\uD83D\uDE00"))
        assertEq("\"\\u007f~\"", canonicalJson("\u007F~"))
    }

    fun testRefusesAValueShapeTheWorkerNeverStores() {
        assertFails<IllegalArgumentException> { canonicalJson(1.5) }
        assertFails<IllegalArgumentException> { canonicalJson(null) }
    }

    fun testBuildCatalogRecordSerializesToThePinnedCPythonString() {
        val signable = LinkedHashMap(pinnedRecord()).apply { remove("signature") }
        assertEq(pinnedRecordJson, canonicalJson(signable))
        assertEq(pinnedRecordHash, catalogRecordHash(signable))
        assertEq(pinnedRecordHash, encodeLowerHex(MessageDigest.getInstance("SHA-256").digest(pinnedRecordJson.toByteArray())))
    }

    fun testTheSignedBytesAreThePrefixANewlineAndTheHexDigest() {
        val signable = pinnedRecord().apply { remove("signature") }
        assertEq("forkmesh-catalog-v2", catalogV2Prefix)
        assertEq("forkmesh-catalog-v2\n$pinnedRecordHash", catalogV2Canonical(signable))
    }

    fun testTheSignatureFieldIsExcludedFromTheHash() {
        val record = pinnedRecord()
        assertEq("", record["signature"])
        val withSignature = LinkedHashMap(record).apply { put("signature", "legacy-v1-signature") }
        val without = LinkedHashMap(record).apply { remove("signature") }
        expect(canonicalJson(withSignature) != canonicalJson(without))
        assertEq(pinnedRecordJson, canonicalJson(without))
    }

    fun testVisibilityIsBoundByTheSignature() {
        val publicRecord = pinnedRecord().apply { remove("signature") }
        val privateRecord = buildCatalogRecord(
            owner = "alice",
            name = "demo-repo",
            description = unicodeDescription,
            isPublic = false,
            branch = "main",
            maintainer = "MAINTAINER-PUBKEY",
            nodeId = "0123456789abcdef0123456789abcdef",
            updatedAt = 1750000000000,
        ).apply { remove("signature") }
        assertEq("private", privateRecord["visibility"])
        expect(catalogV2Canonical(privateRecord) != catalogV2Canonical(publicRecord))
    }

    fun testOptionalHeadFieldsStayAbsentRatherThanEmpty() {
        val bare = buildCatalogRecord(
            owner = "alice",
            name = "demo-repo",
            description = "",
            isPublic = true,
            branch = "main",
            maintainer = "PUB",
            nodeId = "n",
            updatedAt = 1,
        )
        for (key in listOf("machineName", "commitSubject", "commitAuthorName", "commitAt")) {
            expect(!bare.containsKey(key), key)
        }
        assertEq("local-node", bare["source"])
        assertEq("mobile", bare["platform"])
        assertEq("#alice-demo-repo", bare["channel"])
        assertEq("1", bare["lastSync"])
        assertEq("1", bare["updatedAt"])
    }

    fun testCleanCatalogStringIsTheIdentityForAValueTheWorkerKeeps() {
        assertEq("hello world", cleanCatalogString("  hello world  ", 240))
        assertEq(unicodeDescription, cleanCatalogString(unicodeDescription, 240))
    }

    fun testCleanCatalogStringDropsControlCharactersAndTruncatesByCodePoint() {
        assertEq("abc", cleanCatalogString("a\u0007b\u0001c", 240))
        assertEq("\uD83D\uDE00\uD83D\uDE00", cleanCatalogString("\uD83D\uDE00\uD83D\uDE00\uD83D\uDE00", 2))
        assertEq("abc", cleanCatalogString("abcdef", 3))
    }

    fun testCleanCatalogStringFoldsExoticWhitespaceSoBothRuntimesTrimAlike() {
        assertEq("trim", cleanCatalogString("\u00a0trim\u3000", 240))
        assertEq("\u200bkeep", cleanCatalogString("\u200bkeep", 240))
    }
}
