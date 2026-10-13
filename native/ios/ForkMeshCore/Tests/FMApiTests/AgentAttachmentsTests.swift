import FMBase
import FMNet
import Foundation
import XCTest
@testable import FMApi

private let jpeg = Data([0xFF, 0xD8, 0xFF, 0xE0] + [UInt8](repeating: 7, count: 60))
private let png = Data([0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A] + [UInt8](repeating: 0, count: 60))
private let idA = String(repeating: "a", count: 32)
private let idB = String(repeating: "b", count: 32)

private func photo(_ id: String, _ data: Data = jpeg, type: String = "image/jpeg") -> PromptAttachment {
    PromptAttachment(id: id, name: "\(id).jpg", bytes: data, mediaType: type)
}

final class AgentAttachmentModelTests: XCTestCase {
    func testTheRelayListIsParsedAndUnsafeIdsAreDropped() throws {
        let json = try JSON.parse(Data("""
        [{"id":"\(idA)","name":"../../etc/photo-1.jpg","mediaType":"image/jpeg","size":64},
         {"id":"../x","name":"bad","mediaType":"image/png","size":1},
         {"id":"\(idA)","name":"dup","mediaType":"image/png","size":1},
         {"id":"\(idB)","name":"","mediaType":"","size":"12"}]
        """.utf8))
        let list = AgentAttachmentMeta.list(json)
        XCTAssertEqual(list.map(\.id), [idA, idB])
        XCTAssertEqual(list[0].name, "photo-1.jpg")
        XCTAssertEqual(list[0].size, 64)
        XCTAssertEqual(list[1].name, "attachment")
        XCTAssertEqual(list[1].mediaType, "application/octet-stream")
        XCTAssertEqual(list[1].size, 12)
    }

    func testAHistoryTurnCarriesItsPhotosAndRefsNameTheirRoute() throws {
        let json = try JSON.parse(Data("""
        {"sessionId":"sess-1","history":[{"role":"user","author":"alice","text":"look",
          "attachments":[{"id":"\(idA)","name":"p.jpg","mediaType":"image/jpeg","size":64}]},
          {"role":"assistant","text":"ok"}]}
        """.utf8)).objectValue!
        var session = AgentLaunchSession(json: json)
        session.orgScope = true
        XCTAssertEqual(session.history[0].attachments.count, 1)
        XCTAssertTrue(session.history[1].attachments.isEmpty)
        let refs = session.attachmentRefs(session.history[0], owner: "Alice", repo: "Mobile")
        XCTAssertEqual(refs[0].origin, .relay(owner: "Alice", repo: "Mobile", sessionId: "sess-1", orgScope: true))
        XCTAssertEqual(refs[0].cacheKey, "relay:org:alice/mobile:sess-1:\(idA)")
        XCTAssertTrue(refs[0].showsThumbnail)
        XCTAssertEqual(refs[0].sizeLabel, "64 B")
    }

    func testMagicBytesDecideWhatIsAnImage() {
        XCTAssertEqual(AgentAttachmentMedia.sniff(jpeg), "image/jpeg")
        XCTAssertEqual(AgentAttachmentMedia.sniff(png), "image/png")
        XCTAssertEqual(AgentAttachmentMedia.sniff(Data("GIF89a......".utf8)), "image/gif")
        XCTAssertEqual(AgentAttachmentMedia.sniff(Data("RIFF\0\0\0\0WEBPVP8 ".utf8)), "image/webp")
        XCTAssertEqual(AgentAttachmentMedia.sniff(Data([0, 0, 0, 24] + Array("ftypheic".utf8) + [0, 0, 0, 0])), "image/heic")
        XCTAssertNil(AgentAttachmentMedia.sniff(Data("<html><script>".utf8)))
        XCTAssertThrowsError(try AgentAttachmentMedia.checkImage(Data()))
        XCTAssertThrowsError(try AgentAttachmentMedia.checkImage(Data("<svg onload=x>".utf8)))
        XCTAssertThrowsError(try AgentAttachmentMedia.checkImage(jpeg, maxBytes: 10))
        XCTAssertEqual(try AgentAttachmentMedia.checkImage(jpeg), "image/jpeg")
    }

    func testTranscriptMarkersBecomeRowsWithoutPaths() {
        let text = """
        Fix the header.
        Attached image: /home/f/data/agent-images/paste-Ab12.png
        {"type":"user","text":"Attached image: /home/f/shot.jpg\\n more"}
        Attached video: /tmp/clip.mp4
        Do it.

        Attached images (open them with your file-reading tool):
        - .forkmesh/attachments/1111.jpg
        - .forkmesh/attachments/2222.png
        done
        - not/a/listed/path.png
        Attached image: /home/f/data/agent-images/paste-Ab12.png
        """
        let paths = AgentTranscriptAttachments.paths(in: text)
        XCTAssertEqual(paths, [
            "/home/f/data/agent-images/paste-Ab12.png", "/home/f/shot.jpg", "/tmp/clip.mp4",
            ".forkmesh/attachments/1111.jpg", ".forkmesh/attachments/2222.png",
        ])
        let refs = AgentTranscriptAttachments.references(in: text)
        XCTAssertEqual(refs[0].name, "paste-Ab12.png")
        XCTAssertEqual(refs[0].origin, .elsewhere)
        XCTAssertFalse(refs[0].canFetch)
        XCTAssertFalse(refs[0].showsThumbnail)
        XCTAssertTrue(refs[2].isVideo)
        XCTAssertEqual(refs[0].rowDetail, "on the machine that ran the agent")
        XCTAssertTrue(AgentTranscriptAttachments.paths(in: "no markers here").isEmpty)
    }

    func testSentPhotosPairWithTheRelayCopiesOnlyWhenTheyLineUp() {
        let sent = [photo("l1"), photo("l2", png, type: "image/png")]
        let listed = [
            AgentAttachmentMeta(id: idA, name: "l1.jpg", mediaType: "image/jpeg", size: jpeg.count),
            AgentAttachmentMeta(id: idB, name: "l2.png", mediaType: "image/png", size: png.count),
        ]
        let pairs = AgentAttachmentReconcile.pair(sent: sent, listed: listed)
        XCTAssertEqual(pairs?.map(\.meta.id), [idA, idB])
        XCTAssertEqual(pairs?.map(\.photo.id), ["l1", "l2"])
        XCTAssertNil(AgentAttachmentReconcile.pair(sent: sent, listed: [listed[0]]))
        XCTAssertNil(AgentAttachmentReconcile.pair(sent: sent.reversed(), listed: listed))
        XCTAssertNil(AgentAttachmentReconcile.pair(sent: [], listed: []))

        let history = [
            AgentLaunchMessage(role: "user", text: "first", attachments: listed),
            AgentLaunchMessage(role: "assistant", text: "ok", attachments: listed),
            AgentLaunchMessage(role: "user", text: "second"),
        ]
        XCTAssertEqual(AgentAttachmentReconcile.turn(for: sent, in: history), 0)
        XCTAssertNil(AgentAttachmentReconcile.turn(for: [photo("x")], in: history))
    }

    @MainActor
    func testSentPhotosAreRememberedPerSessionAndBounded() {
        let sent = AgentSentPhotos(sessionLimit: 2)
        sent.record(sessionId: "s1", text: "look", photos: [photo("a")])
        sent.record(sessionId: "s2", text: "look", photos: [photo("b")])
        XCTAssertEqual(sent.photos(sessionId: "s1", text: "look")?.map(\.id), ["a"])
        XCTAssertNil(sent.photos(sessionId: "s1", text: "other"))
        sent.record(sessionId: "s3", text: "x", photos: [photo("c")])
        XCTAssertNil(sent.photos(sessionId: "s1", text: "look"))
        sent.rename("s2", to: "s9")
        XCTAssertEqual(sent.photos(sessionId: "s9", text: "look")?.map(\.id), ["b"])
        sent.record(sessionId: "s4", text: "none", photos: [])
        XCTAssertNil(sent.photos(sessionId: "s4", text: "none"))
    }
}

final class AgentAttachmentStoreTests: XCTestCase {
    func testTheCacheKeepsWithinItsBudgetLeastRecentlyUsedFirst() async {
        let store = AgentAttachmentStore(byteBudget: 100)
        await store.put("a", Data(count: 40))
        await store.put("b", Data(count: 40))
        _ = await store.cached("a")
        await store.put("c", Data(count: 40))
        let hasA = await store.contains("a")
        let hasB = await store.contains("b")
        let hasC = await store.contains("c")
        XCTAssertTrue(hasA)
        XCTAssertFalse(hasB)
        XCTAssertTrue(hasC)
        let total = await store.totalBytes
        XCTAssertEqual(total, 80)
        await store.put("huge", Data(count: 60))
        let hasHuge = await store.contains("huge")
        XCTAssertFalse(hasHuge)
        await store.removeAll()
        let empty = await store.totalBytes
        XCTAssertEqual(empty, 0)
    }

    func testConcurrentAsksShareOneFetchAndFailuresAreNotCached() async throws {
        let store = AgentAttachmentStore(byteBudget: 1_000)
        let counter = Counter()
        async let first = store.data(for: "k") {
            await counter.bump()
            try await Task.sleep(nanoseconds: 50_000_000)
            return jpeg
        }
        async let second = store.data(for: "k") {
            await counter.bump()
            return jpeg
        }
        let (one, two) = try await (first, second)
        XCTAssertEqual(one, jpeg)
        XCTAssertEqual(two, jpeg)
        let calls = await counter.value
        XCTAssertEqual(calls, 1)

        do {
            _ = try await store.data(for: "bad") { throw AgentAttachmentError("offline") }
            XCTFail("expected a failure")
        } catch {}
        let again = try await store.data(for: "bad") { png }
        XCTAssertEqual(again, png)
    }

    func testTheLoaderRoutesByOriginAndRefusesWhatIsNotAnImage() async throws {
        let store = AgentAttachmentStore()
        let loader = AgentAttachmentLoader(
            store: store,
            relay: { _ in jpeg },
            desktop: { _ in Data("<html>".utf8) })
        let relay = AgentAttachmentRef(
            meta: AgentAttachmentMeta(id: idA, name: "p.jpg", mediaType: "image/jpeg", size: jpeg.count),
            origin: .relay(owner: "alice", repo: "mobile", sessionId: "s", orgScope: false))
        let loaded = try await loader.load(relay)
        XCTAssertEqual(loaded, jpeg)
        let desktop = AgentAttachmentRef(
            meta: AgentAttachmentMeta(id: idB, name: "p.png", mediaType: "image/png"), origin: .desktop(sessionId: 3))
        do {
            _ = try await loader.load(desktop)
            XCTFail("a non-image must be refused")
        } catch let error as AgentAttachmentError {
            XCTAssertEqual(error.message, "The attachment is not an image.")
        }
        let elsewhere = AgentTranscriptAttachments.references(in: "Attached image: /x/y.png")[0]
        do {
            _ = try await loader.load(elsewhere)
            XCTFail("a path on another machine cannot be read")
        } catch {}

        let sent = photo("l1")
        await loader.seedLocal([sent])
        let local = try await loader.load(.local(sent))
        XCTAssertEqual(local, jpeg)
        let meta = AgentAttachmentMeta(id: idB, name: "l1.jpg", mediaType: "image/jpeg", size: jpeg.count)
        let origin = AgentAttachmentRef.Origin.relay(owner: "alice", repo: "mobile", sessionId: "s2", orgScope: false)
        await loader.seed([(meta, sent)], origin: origin)
        let seeded = await store.cached(AgentAttachmentRef(meta: meta, origin: origin).cacheKey)
        XCTAssertEqual(seeded, jpeg)
    }
}

private actor RelayState {
    var relayed = true
    var fetches: [Bool] = []
    func set(_ value: Bool) { relayed = value }
    func record(_ preview: Bool) { fetches.append(preview) }
}

final class AgentAttachmentPreviewTests: XCTestCase {
    func testRelayedPreviewsAreCachedApartFromFullPictures() async throws {
        let store = AgentAttachmentStore()
        let state = RelayState()
        let full = Data([0xFF, 0xD8, 0xFF, 0xE1] + [UInt8](repeating: 9, count: 200))
        let loader = AgentAttachmentLoader(
            store: store,
            relay: { _ in throw AgentAttachmentError("unused") },
            desktopPayload: { _, preview in
                await state.record(preview)
                if preview {
                    return AgentAttachmentPayload(data: jpeg, preview: true, originalSize: 4_000_000, width: 1600, height: 1200)
                }
                return .full(full)
            },
            desktopRelayed: { await state.relayed })
        let ref = AgentAttachmentRef(
            meta: AgentAttachmentMeta(id: idA, name: "shot.png", mediaType: "image/png"), origin: .desktop(sessionId: 5))

        let first = try await loader.loadPayload(ref)
        XCTAssertTrue(first.preview)
        XCTAssertEqual(first.data, jpeg)
        XCTAssertEqual(first.width, 1600)
        XCTAssertEqual(first.previewNote, "Preview — open on the desktop's network for the full image.")
        XCTAssertEqual(first.previewDetail, "1600 × 1200 · full image 3.8 MB")
        let again = try await loader.loadPayload(ref)
        XCTAssertEqual(again, first)
        let fullCached = await store.cached(ref.cacheKey)
        XCTAssertNil(fullCached)
        let previewCached = await store.cachedPreview(ref.previewCacheKey)
        XCTAssertEqual(previewCached?.originalSize, 4_000_000)

        await state.set(false)
        let home = try await loader.loadPayload(ref)
        XCTAssertFalse(home.preview)
        XCTAssertEqual(home.data, full)
        XCTAssertEqual(home.previewNote, "")
        let dropped = await store.cachedPreview(ref.previewCacheKey)
        XCTAssertNil(dropped)

        await state.set(true)
        let away = try await loader.load(ref)
        XCTAssertEqual(away, full)
        let fetches = await state.fetches
        XCTAssertEqual(fetches, [true, false])
    }

    func testAPreviewThatIsNotAPictureIsRefusedAndNotCached() async throws {
        let store = AgentAttachmentStore()
        let loader = AgentAttachmentLoader(
            store: store,
            relay: { _ in jpeg },
            desktopPayload: { _, _ in AgentAttachmentPayload(data: Data("<html>".utf8), preview: true) },
            desktopRelayed: { true })
        let ref = AgentAttachmentRef(
            meta: AgentAttachmentMeta(id: idB, name: "notes.txt", mediaType: "text/plain"), origin: .desktop(sessionId: 5))
        do {
            _ = try await loader.loadPayload(ref)
            XCTFail("a preview must be a picture")
        } catch let error as AgentAttachmentError {
            XCTAssertEqual(error.message, "The attachment is not an image.")
        }
        let total = await store.totalBytes
        XCTAssertEqual(total, 0)
        let relayRef = AgentAttachmentRef(
            meta: AgentAttachmentMeta(id: idA, name: "p.jpg", mediaType: "image/jpeg"),
            origin: .relay(owner: "alice", repo: "app", sessionId: "s", orgScope: false))
        let relayed = try await loader.loadPayload(relayRef)
        XCTAssertFalse(relayed.preview)
        XCTAssertEqual(relayed.data, jpeg)
    }

    func testEvictionForgetsThePreviewDetails() async {
        let store = AgentAttachmentStore(byteBudget: 200)
        await store.putPreview("a#preview", AgentAttachmentPayload(data: jpeg, preview: true, width: 10, height: 10))
        let kept = await store.cachedPreview("a#preview")
        XCTAssertEqual(kept?.width, 10)
        await store.put("b", Data(repeating: 1, count: 90))
        await store.put("c", Data(repeating: 2, count: 90))
        let evicted = await store.cachedPreview("a#preview")
        XCTAssertNil(evicted)
        await store.put("a#preview", jpeg)
        let plain = await store.cachedPreview("a#preview")
        XCTAssertNil(plain)
    }
}

private final class Reply: @unchecked Sendable {
    var status: Int
    var body: Data
    init(status: Int, body: Data) {
        self.status = status
        self.body = body
    }
    var response: HTTPResponse { HTTPResponse(status: status, body: body) }
}

private actor Counter {
    var value = 0
    func bump() { value += 1 }
}

@MainActor
final class ApiAgentAttachmentReadTests: XCTestCase {
    func testTheOwnerRouteIsReadWithTheSessionAndChecked() async throws {
        let http = FakeHTTPClient { _ in HTTPResponse(status: 200, headers: ["content-type": "image/jpeg"], body: jpeg) }
        let api = ApiPortFixture.api(http, serverUrl: "https://relay.test")
        let data = try await api.agentSessionAttachment("alice", "mobile", sessionId: "sess 1", attachmentId: idA)
        XCTAssertEqual(data, jpeg)
        let request = try XCTUnwrap(http.requests.first)
        XCTAssertEqual(portMethodPath(request), "GET /api/repo/alice/mobile/agent-sessions/sess%201/attachments/\(idA)")
        XCTAssertEqual(request.headers["Authorization"], "Bearer session-token")
    }

    func testTheOrganizationRouteAndEveryRefusalReadsAsASentence() async throws {
        let reply = Reply(status: 200, body: png)
        let http = FakeHTTPClient { _ in reply.response }
        let api = ApiPortFixture.api(http, serverUrl: "https://relay.test")
        _ = try await api.agentSessionAttachment("acme", "web", sessionId: "s", attachmentId: idB, orgScope: true)
        XCTAssertTrue(portRawPath(http.requests[0]).hasSuffix("/agent-bots/s/attachments/\(idB)"))
        XCTAssertTrue(portRawPath(http.requests[0]).hasPrefix("/api/orgs/"))

        for (code, words) in [(403, "Only the person"), (410, "a week"), (404, "does not keep"), (500, "HTTP 500")] {
            reply.status = code
            reply.body = Data("{}".utf8)
            do {
                _ = try await api.agentSessionAttachment("alice", "mobile", sessionId: "s", attachmentId: idA)
                XCTFail("expected \(code) to throw")
            } catch let error as AgentAttachmentError {
                XCTAssertTrue(error.message.contains(words), error.message)
            }
        }
        reply.status = 200
        reply.body = Data("<html>".utf8)
        do {
            _ = try await api.agentSessionAttachment("alice", "mobile", sessionId: "s", attachmentId: idA)
            XCTFail("expected a refusal")
        } catch {}
        let before = http.requests.count
        do {
            _ = try await api.agentSessionAttachment("alice", "mobile", sessionId: "s", attachmentId: "../../x")
            XCTFail("expected a refusal")
        } catch {}
        XCTAssertEqual(http.requests.count, before)
    }

    func testSignedOutNeverAsks() async {
        let http = FakeHTTPClient { _ in HTTPResponse(status: 200, body: jpeg) }
        let api = ApiPortFixture.api(http, token: "")
        do {
            _ = try await api.agentSessionAttachment("alice", "mobile", sessionId: "s", attachmentId: idA)
            XCTFail("expected a refusal")
        } catch {}
        XCTAssertTrue(http.requests.isEmpty)
    }
}
