#!/usr/bin/env python3
"""Algorithm tests for the worker's auth/crypto primitives.

src/entry.py implements TOTP, PBKDF2 and the blind index on top of WebCrypto
(pyodide), which can't run outside the Workers runtime. This file is a pure
stdlib *mirror* of the exact same algorithms, checked against published RFC /
known-answer vectors. If these pass, the math in entry.py is correct; what it
does NOT cover is the pyodide<->WebCrypto glue (verify that on a real deploy).

Run: python3 cloudflare_worker/tests/test_crypto.py   (exit 0 = all passed)
"""
import base64
import hashlib
import hmac
import struct
import sys

# --- Mirrors of entry.py -----------------------------------------------------


def b32_decode(secret):
    s = (secret or "").strip().upper().replace(" ", "")
    return base64.b32decode(s + "=" * ((8 - len(s) % 8) % 8))


def hotp(secret_bytes, counter, digits=6):
    mac = hmac.new(secret_bytes, struct.pack(">Q", counter), hashlib.sha1).digest()
    offset = mac[-1] & 0x0F
    code = ((mac[offset] & 0x7F) << 24 | (mac[offset + 1] & 0xFF) << 16 |
            (mac[offset + 2] & 0xFF) << 8 | (mac[offset + 3] & 0xFF)) % (10 ** digits)
    return str(code).zfill(digits)


def totp(secret_b32, unix_time, step=30, digits=6):
    return hotp(b32_decode(secret_b32), unix_time // step, digits)


def pbkdf2_sha256(password, salt, iters, dklen=32):
    return hashlib.pbkdf2_hmac("sha256", password, salt, iters, dklen)


def blind_index(data_key, value):
    key = hashlib.sha256((data_key + ":blind-index").encode()).digest()
    norm = (value or "").strip().lower().encode()
    return hmac.new(key, norm, hashlib.sha256).hexdigest()


def _sha256_hex(s):
    return hashlib.sha256(s.encode()).hexdigest()


def pull_comment_content(ev):
    # Mirrors entry.py:pull_comment_content / PullStore::contentForSigning.
    t = ev.get("type", "")
    def int_field(name):
        return str(int(ev.get(name, 0)))
    if t == "comment":
        return ev.get("body", "")
    if t == "review":
        return "\x00".join([ev.get("state", ""), ev.get("body", "")])
    anchor = (
        [str(ev.get("patchset", "") or ""), str(ev.get("anchorLines", "") or "")]
        if (ev.get("patchset") or ev.get("anchorLines")) else []
    )
    if t == "line-comment":
        return "\x00".join([
            ev.get("path", ""), ev.get("side", ""), int_field("line"),
            ev.get("body", ""),
        ] + anchor)
    if t == "thread-comment":
        return "\x00".join([
            ev.get("threadId", ""), ev.get("path", ""), ev.get("side", ""),
            int_field("lineStart"), int_field("lineEnd"), ev.get("body", ""),
            ev.get("suggestionPatch", ""),
        ] + anchor)
    if t == "thread-reply":
        return "\x00".join([
            ev.get("threadId", ""), ev.get("parentId", ""),
            ev.get("body", ""),
        ])
    if t == "thread-state":
        return "\x00".join([
            ev.get("threadId", ""), ev.get("state", ""), ev.get("body", ""),
        ])
    if t == "suggestion-state":
        return "\x00".join([
            ev.get("threadId", ""), ev.get("state", ""),
            ev.get("appliedCommit", ""), ev.get("body", ""),
        ])
    return ""


def pull_comment_canonical(number, ev):
    # Mirrors entry.py:verify_pull_comment_event canonical construction.
    return (
        "forkmesh-pull-comment-v1\n" + ev.get("type", "") + "\n" + str(int(number)) +
        "\n" + ev.get("author", "") + "\n" + str(int(ev.get("ts", 0))) + "\n" +
        _sha256_hex(pull_comment_content(ev))
    )


def discussion_event_content(ev):
    # Mirrors entry.py:discussion_event_content / DiscussionStore::contentForSigning.
    t = ev.get("type", "")
    if t == "open":
        return "\x00".join([
            ev.get("title", ""), ev.get("body", ""), ev.get("category", "")
        ])
    if t == "comment":
        return ev.get("body", "")
    return ""


def discussion_event_canonical(number, ev):
    # Mirrors entry.py:verify_discussion_event canonical construction.
    return (
        "forkmesh-discussion-event-v1\n" + ev.get("type", "") + "\n" +
        str(int(number)) + "\n" + ev.get("author", "") + "\n" +
        str(int(ev.get("ts", 0))) + "\n" +
        _sha256_hex(discussion_event_content(ev))
    )


# --- Test runner -------------------------------------------------------------

_failures = []


def check(name, got, expected):
    ok = got == expected
    print(("PASS " if ok else "FAIL ") + name)
    if not ok:
        print("      got:      %r" % (got,))
        print("      expected: %r" % (expected,))
        _failures.append(name)


def main():
    # RFC 4226 HOTP vectors — secret = ASCII "12345678901234567890".
    rfc4226_secret = b"12345678901234567890"
    rfc4226 = ["755224", "287082", "359152", "969429", "338314",
               "254676", "287922", "162583", "399871", "520489"]
    for counter, expected in enumerate(rfc4226):
        check("HOTP/RFC4226 count=%d" % counter,
              hotp(rfc4226_secret, counter), expected)

    # RFC 6238 TOTP (SHA-1) — 6-digit truncation of the published 8-digit codes.
    secret_b32 = base64.b32encode(rfc4226_secret).decode()
    rfc6238 = [
        (59, "287082"),
        (1111111109, "081804"),
        (1111111111, "050471"),
        (1234567890, "005924"),
        (2000000000, "279037"),
        (20000000000, "353130"),
    ]
    for t, expected in rfc6238:
        check("TOTP/RFC6238 T=%d" % t, totp(secret_b32, t), expected)

    # PBKDF2-HMAC-SHA256 known-answer vectors (dkLen=32).
    check(
        "PBKDF2-SHA256 password/salt/1",
        pbkdf2_sha256(b"password", b"salt", 1).hex(),
        "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b",
    )
    check(
        "PBKDF2-SHA256 password/salt/2",
        pbkdf2_sha256(b"password", b"salt", 2).hex(),
        "ae4d0c95af6b46d32d0adff928f06dd02a303f8ef3c251dfd6e2d85a95474c43",
    )
    # Round-trip at the worker's real iteration count is deterministic.
    h1 = pbkdf2_sha256(b"hunter2", b"\x00" * 16, 100000)
    h2 = pbkdf2_sha256(b"hunter2", b"\x00" * 16, 100000)
    check("PBKDF2-SHA256 deterministic (100k)", h1, h2)
    check("PBKDF2-SHA256 rejects wrong password",
          pbkdf2_sha256(b"wrong", b"\x00" * 16, 100000) == h1, False)

    # Blind index: deterministic, case/space-insensitive, distinct, fixed vector.
    k = "test-data-key"
    check("blind_index deterministic", blind_index(k, "Alice"), blind_index(k, "alice"))
    check("blind_index trims/lowercases", blind_index(k, "  ALICE  "), blind_index(k, "alice"))
    check("blind_index distinct values",
          blind_index(k, "alice") != blind_index(k, "bob"), True)
    check("blind_index key-separated",
          blind_index("key-a", "alice") != blind_index("key-b", "alice"), True)
    check(
        "blind_index regression vector",
        blind_index("forkmesh-dev-data-key", "alice/myrepo"),
        hmac.new(
            hashlib.sha256(b"forkmesh-dev-data-key:blind-index").digest(),
            b"alice/myrepo", hashlib.sha256,
        ).hexdigest(),
    )

    # PR conversation + commit comment canonical strings: these MUST match the
    # C++ vectors pinned in desktop/tests/test_crypto.cpp byte-for-byte, so the
    # client's signer and the worker's verifier agree.
    check(
        "pull-comment canonical vector",
        pull_comment_canonical(
            5, {"type": "comment", "author": "TESTPUB", "ts": 1000,
                "body": "Looks good"}),
        "forkmesh-pull-comment-v1\ncomment\n5\nTESTPUB\n1000\n"
        "5fc87d339144090b0ad2e192e6a6fe58e98d3a5a062467c7e62049c7d8c3db01",
    )
    check(
        "pull-review canonical vector",
        pull_comment_canonical(
            5, {"type": "review", "author": "TESTPUB", "ts": 2000,
                "state": "approved", "body": "LGTM"}),
        "forkmesh-pull-comment-v1\nreview\n5\nTESTPUB\n2000\n"
        "b863bbc11dd8fea92da94a7da47f815aceeaa9418483992d0a952273894a0731",
    )
    check(
        "pull line-comment canonical vector",
        pull_comment_canonical(
            5, {"type": "line-comment", "author": "TESTPUB", "ts": 2500,
                "path": "src/x.cpp", "side": "new", "line": 42,
                "body": "needs a guard"}),
        "forkmesh-pull-comment-v1\nline-comment\n5\nTESTPUB\n2500\n"
        "a2574c2b392fd0db6b7e4b0d025d4094bb410691dca7686ddf095d63881f4985",
    )
    check(
        "pull thread-comment canonical vector",
        pull_comment_canonical(
            5, {"type": "thread-comment", "author": "TESTPUB", "ts": 2600,
                "threadId": "thread-1", "path": "src/x.cpp", "side": "new",
                "lineStart": 42, "lineEnd": 44, "body": "Use guard",
                "suggestionPatch": "@@ -1 +1 @@\n-old\n+new\n"}),
        "forkmesh-pull-comment-v1\nthread-comment\n5\nTESTPUB\n2600\n"
        "adbf1313d68a9d32f69734e23b1b7713d6c30dcd77b628513f8b04b5a459fe81",
    )
    # Patchset anchors (issue #483): a line-anchored event that names the head
    # commit it was written against and the commented text signs both, so a
    # thread's claim about which revision and code it spoke of is verifiable.
    # An event without them signs exactly the pre-anchor bytes (above).
    check(
        "pull anchored thread-comment canonical vector",
        pull_comment_canonical(
            5, {"type": "thread-comment", "author": "TESTPUB", "ts": 2600,
                "threadId": "thread-1", "path": "src/x.cpp", "side": "new",
                "lineStart": 42, "lineEnd": 44, "body": "Use guard",
                "suggestionPatch": "@@ -1 +1 @@\n-old\n+new\n",
                "patchset": "0123456789abcdef0123456789abcdef01234567",
                "anchorLines": "int x = 1;\nreturn x;\n}"}),
        "forkmesh-pull-comment-v1\nthread-comment\n5\nTESTPUB\n2600\n"
        "3e917b7d28e0e5ac2f551e30dc6d30f986325bc015c7b2dd6d1095cd11ebd894",
    )
    check(
        "pull anchored line-comment canonical vector",
        pull_comment_canonical(
            5, {"type": "line-comment", "author": "TESTPUB", "ts": 2500,
                "path": "src/x.cpp", "side": "new", "line": 7,
                "body": "needs a guard",
                "patchset": "0123456789abcdef0123456789abcdef01234567",
                "anchorLines": "if (!p)"}),
        "forkmesh-pull-comment-v1\nline-comment\n5\nTESTPUB\n2500\n"
        "de9b901105f5216175574abb59e8dd1e28e192c85864c7cd07e28a629adebdfa",
    )
    check(
        "pull thread-reply canonical vector",
        pull_comment_canonical(
            5, {"type": "thread-reply", "author": "TESTPUB", "ts": 2700,
                "threadId": "thread-1", "parentId": "event-1",
                "body": "I pushed a fix"}),
        "forkmesh-pull-comment-v1\nthread-reply\n5\nTESTPUB\n2700\n"
        "cd9fc0caa43648948f6a976d570191b9a4d85bee1d984a7aacc483a7487196dd",
    )
    check(
        "pull thread-state canonical vector",
        pull_comment_canonical(
            5, {"type": "thread-state", "author": "TESTPUB", "ts": 2800,
                "threadId": "thread-1", "state": "resolved",
                "body": "resolved after update"}),
        "forkmesh-pull-comment-v1\nthread-state\n5\nTESTPUB\n2800\n"
        "b652054bd993fa340a565f6ec3e0c88dc728c8b69d7fdf723acc6bdccc1c07df",
    )
    check(
        "pull suggestion-state canonical vector",
        pull_comment_canonical(
            5, {"type": "suggestion-state", "author": "TESTPUB", "ts": 2900,
                "threadId": "thread-1", "state": "applied",
                "appliedCommit": "abc123def456",
                "body": "applied in follow-up"}),
        "forkmesh-pull-comment-v1\nsuggestion-state\n5\nTESTPUB\n2900\n"
        "07ce71665f21bcb839faeb956910a9ca369d79167e6f6e52f60c4d01344f3c52",
    )
    check(
        "discussion open canonical vector",
        discussion_event_canonical(
            1, {"type": "open", "author": "TESTPUB", "ts": 1000,
                "title": "Welcome", "category": "Announcements",
                "body": "Hello discussion"}),
        "forkmesh-discussion-event-v1\nopen\n1\nTESTPUB\n1000\n"
        "8e31495ce2e5559ce11564b67a45710aac9654c0f70b0fcfd0ab08dc51c83ab2",
    )
    check(
        "discussion inbox-open canonical vector",
        discussion_event_canonical(
            0, {"type": "open", "author": "TESTPUB", "ts": 1000,
                "title": "Welcome", "category": "Announcements",
                "body": "Hello discussion"}),
        "forkmesh-discussion-event-v1\nopen\n0\nTESTPUB\n1000\n"
        "8e31495ce2e5559ce11564b67a45710aac9654c0f70b0fcfd0ab08dc51c83ab2",
    )
    check(
        "discussion comment canonical vector",
        discussion_event_canonical(
            1, {"type": "comment", "author": "TESTPUB", "ts": 2000,
                "body": "Reply body"}),
        "forkmesh-discussion-event-v1\ncomment\n1\nTESTPUB\n2000\n"
        "b87e74db2baf019fb26d1a764aa329723024c6be7f13e5a92a60690b301bc3e9",
    )
    print()
    if _failures:
        print("%d test(s) FAILED: %s" % (len(_failures), ", ".join(_failures)))
        return 1
    print("All crypto algorithm tests passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
