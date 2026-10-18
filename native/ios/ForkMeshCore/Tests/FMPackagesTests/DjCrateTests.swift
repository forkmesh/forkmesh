import FMPackages
import Foundation
import XCTest

final class DjCrateTests: XCTestCase {
    let one = "dQw4w9WgXcQ"
    let two = "9bZkp7q19f0"
    let three = "kJQP7kiw5Fk"
    let four = "JGwWNGJdvx8"

    func testTagNamesAreTidied() {
        XCTAssertEqual(DjCrate.tagName("  #Deep   House "), "deep house")
        XCTAssertEqual(DjCrate.tagNames("Chill, #chill, warm up,,"), ["chill", "warm up"])
        XCTAssertEqual(DjCrate.tagName(String(repeating: "a", count: 50)).count, DjCrate.tagLength)
        XCTAssertEqual(DjCrate.tagName(" # "), "")
    }

    func testTaggingKeepsTheSongAndCountsTags() {
        var crate = DjCrate()
        XCTAssertTrue(crate.tag(one, "chill, House", title: "First", author: "Someone"))
        XCTAssertTrue(crate.tag(two, "house"))
        XCTAssertFalse(crate.tag("short", "house"))
        XCTAssertFalse(crate.tag(three, " , "))
        XCTAssertEqual(crate.track(one)?.title, "First")
        XCTAssertEqual(crate.track(one)?.tags, ["chill", "house"])
        XCTAssertEqual(crate.tagCounts, [DjTagCount(name: "chill", count: 1), DjTagCount(name: "house", count: 2)])
        XCTAssertEqual(crate.songs(.tag("house")).map(\.id), [one, two])
        crate.untag(one, "house")
        XCTAssertEqual(crate.songs(.tag("house")).map(\.id), [two])
    }

    func testRenamingAndDeletingTagsFollowTheFocus() {
        var crate = DjCrate()
        crate.tag(one, "house, deep")
        crate.tag(two, "deep")
        crate.setFocus(.tag("deep"))
        XCTAssertTrue(crate.renameTag("deep", to: "House"))
        XCTAssertEqual(crate.track(one)?.tags, ["house"])
        XCTAssertEqual(crate.track(two)?.tags, ["house"])
        XCTAssertEqual(crate.focus, .tag("house"))
        crate.deleteTag("house")
        XCTAssertEqual(crate.focus, .everything)
        XCTAssertTrue(crate.tagCounts.isEmpty)
        XCTAssertFalse(crate.renameTag("house", to: "  "))
    }

    func testPlaylistsKeepOrderNamesAndMembers() {
        var crate = DjCrate()
        let id = crate.createPlaylist("  Friday   Night ", id: "p1")
        XCTAssertEqual(id, "p1")
        XCTAssertEqual(crate.playlist("p1")?.name, "Friday Night")
        XCTAssertEqual(crate.createPlaylist("friday night", id: "p2"), "p2")
        XCTAssertEqual(crate.playlist("p2")?.name, "friday night 2")
        crate.renamePlaylist("p2", "Sunday")
        XCTAssertEqual(crate.playlist("p2")?.name, "Sunday")
        XCTAssertEqual(crate.add([DjTrack(id: one, title: "A"), DjTrack(id: two), DjTrack(id: one), DjTrack(id: "bad")], to: "p1"), 2)
        XCTAssertEqual(crate.add([DjTrack(id: three)], to: "p1"), 1)
        XCTAssertEqual(crate.playlist("p1")?.songs, [one, two, three])
        crate.moveSongs(in: "p1", from: IndexSet(integer: 2), to: 0)
        XCTAssertEqual(crate.playlist("p1")?.songs, [three, one, two])
        crate.moveSongs(in: "p1", from: IndexSet(integer: 0), to: 3)
        XCTAssertEqual(crate.playlist("p1")?.songs, [one, two, three])
        crate.removeSongs(at: IndexSet(integer: 1), from: "p1")
        XCTAssertEqual(crate.songs(.playlist("p1")).map(\.id), [one, three])
        XCTAssertNotNil(crate.track(two))
        crate.setFocus(.playlist("p1"))
        crate.removeTrack(one)
        XCTAssertEqual(crate.playlist("p1")?.songs, [three])
        crate.deletePlaylist("p1")
        XCTAssertEqual(crate.focus, .everything)
    }

    func testFocusOnAPlaylistPlaysItInOrderAndWraps() {
        var crate = DjCrate()
        crate.createPlaylist("Set", id: "p")
        crate.add([DjTrack(id: one), DjTrack(id: two), DjTrack(id: three)], to: "p")
        crate.setFocus(.playlist("p"))
        crate.shuffle = false
        let library = [DjPlayedSong(id: four, title: "Elsewhere", author: "")]
        XCTAssertEqual(crate.next(library, loaded: [], playing: [], seed: 1)?.id, one)
        XCTAssertEqual(crate.next(library, loaded: [one], playing: [one], seed: 1)?.id, two)
        XCTAssertEqual(crate.next(library, loaded: [one, two], playing: [two], seed: 1)?.id, three)
        XCTAssertEqual(crate.next(library, loaded: [two, three], playing: [three], seed: 1)?.id, one)
        XCTAssertEqual(crate.next(library, loaded: [], playing: [], skip: [two], seed: 1)?.id, three)
    }

    func testShuffledFocusStaysInsideTheTag() {
        var crate = DjCrate()
        crate.tag(one, "chill")
        crate.tag(two, "chill")
        crate.tag(three, "loud")
        crate.setFocus(.tag("chill"))
        let library = [DjPlayedSong(id: four, title: "", author: "", plays: 9, last: 1), DjPlayedSong(id: one, title: "Played", author: "", plays: 3, last: 50)]
        for seed in UInt64(0)..<40 {
            var copy = crate
            let pick = copy.next(library, loaded: [], playing: [], seed: seed)
            XCTAssertTrue([one, two].contains(pick?.id ?? ""))
        }
        var copy = crate
        XCTAssertEqual(copy.next(library, loaded: [two], playing: [two], seed: 3)?.id, one)
        XCTAssertEqual(crate.pool(library).first { $0.id == one }?.plays, 3)
        XCTAssertEqual(crate.pool(library).first { $0.id == one }?.title, "Played")
        crate.setFocus(.tag("nobody"))
        XCTAssertEqual(crate.focus, .everything)
    }

    func testEverythingAddsKeptSongsToThePlayedOnes() {
        var crate = DjCrate()
        crate.tag(two, "new")
        let library = [DjPlayedSong(id: one, title: "", author: "")]
        XCTAssertEqual(crate.pool(library).map(\.id), [one, two])
        XCTAssertEqual(crate.pool(library, skip: [one]).map(\.id), [two])
        XCTAssertTrue(DjCrate().pool([]).isEmpty)
        var empty = DjCrate()
        XCTAssertNil(empty.next([], loaded: [], playing: [], seed: 0))
    }

    func testSavedCrateRoundTripsAndDropsBadEntries() throws {
        var crate = DjCrate()
        crate.tag(one, "chill", title: "One")
        crate.createPlaylist("Mine", id: "p")
        crate.add([DjTrack(id: one), DjTrack(id: two)], to: "p")
        crate.setFocus(.playlist("p"))
        crate.shuffle = false
        XCTAssertEqual(DjCrate.decode(crate.encoded()), crate)
        let raw = #"{"tracks":[{"id":"\#(one)","tags":["A"]},{"id":"bad"},{"id":"\#(one)"}],"playlists":[{"id":"p","name":"x","songs":["\#(one)","\#(three)"]}],"focus":"playlist:gone","shuffle":false}"#
        let decoded = DjCrate.decode(Data(raw.utf8))
        XCTAssertEqual(decoded.tracks.map(\.id), [one])
        XCTAssertEqual(decoded.tracks.first?.tags, ["a"])
        XCTAssertEqual(decoded.playlist("p")?.songs, [one])
        XCTAssertEqual(decoded.focus, .everything)
        XCTAssertFalse(decoded.shuffle)
        XCTAssertEqual(DjCrate.decode(Data("nope".utf8)), DjCrate())
        XCTAssertEqual(DjFocus(key: DjFocus.tag("deep house").key), .tag("deep house"))
        XCTAssertEqual(DjFocus(key: "tag: "), .everything)
    }

    func testExportImportsBackIntoAnotherCrate() throws {
        var crate = DjCrate()
        crate.tag(one, "chill", title: "One", author: "Artist")
        crate.createPlaylist("Warm Up", id: "p")
        crate.add([DjTrack(id: two, title: "Two"), DjTrack(id: one)], to: "p")
        let data = crate.exported(.everything, at: Date(timeIntervalSince1970: 0))
        let object = try XCTUnwrap(JSONSerialization.jsonObject(with: data) as? [String: Any])
        XCTAssertEqual(object["format"] as? String, DjCrate.format)
        XCTAssertEqual(object["exported"] as? String, "1970-01-01T00:00:00Z")
        let parsed = try XCTUnwrap(DjCrate.parse(data))
        XCTAssertEqual(parsed.tracks.map(\.id), [one, two])
        XCTAssertEqual(parsed.playlists, [DjImportedList(name: "Warm Up", songs: [two, one])])

        var other = DjCrate()
        other.tag(two, "loud")
        other.createPlaylist("warm up", id: "mine")
        other.add([DjTrack(id: three)], to: "mine")
        let report = other.merge(parsed, name: "Imported")
        XCTAssertEqual(report, DjMergeReport(songs: 1, playlists: 1))
        XCTAssertEqual(report.summary, "Imported 1 new song into 1 playlist.")
        XCTAssertEqual(other.playlists.count, 1)
        XCTAssertEqual(other.playlist("mine")?.songs, [three, two, one])
        XCTAssertEqual(other.track(two)?.title, "Two")
        XCTAssertEqual(other.track(two)?.tags, ["loud"])
        XCTAssertEqual(other.track(one)?.tags, ["chill"])
    }

    func testTagExportCarriesTagsWithoutMakingAPlaylist() throws {
        var crate = DjCrate()
        crate.tag(one, "chill")
        crate.tag(two, "loud")
        let parsed = try XCTUnwrap(DjCrate.parse(crate.exported(.tag("chill"))))
        XCTAssertEqual(parsed.tracks.map(\.id), [one])
        var other = DjCrate()
        XCTAssertEqual(other.merge(parsed, name: "Imported"), DjMergeReport(songs: 1, playlists: 0))
        XCTAssertTrue(other.playlists.isEmpty)
        XCTAssertEqual(other.songs(.tag("chill")).map(\.id), [one])
    }

    func testLinksAndM3uBecomeAPlaylist() throws {
        var crate = DjCrate()
        crate.createPlaylist("Set", id: "p")
        crate.add([DjTrack(id: one, title: "One\nline"), DjTrack(id: two)], to: "p")
        let m3u = crate.m3u(.playlist("p"))
        XCTAssertEqual(m3u, "#EXTM3U\n#EXTINF:-1,One line\nhttps://www.youtube.com/watch?v=\(one)\n#EXTINF:-1,YouTube video\nhttps://www.youtube.com/watch?v=\(two)\n")
        let parsed = try XCTUnwrap(DjCrate.parse(Data(m3u.utf8)))
        XCTAssertEqual(parsed.tracks.map(\.title), ["One line", "YouTube video"])

        let text = "My set\nhttps://youtu.be/\(three)?si=x\n\nwatch this https://www.youtube.com/watch?v=\(four)&t=3\nhttps://youtu.be/\(three)\nhttps://example.com\n"
        let links = try XCTUnwrap(DjCrate.parse(Data(text.utf8)))
        XCTAssertEqual(links.tracks.map(\.id), [three, four])
        var other = DjCrate()
        XCTAssertEqual(other.merge(links, name: "Pasted"), DjMergeReport(songs: 2, playlists: 1))
        XCTAssertEqual(other.playlists.first?.name, "Pasted")
        XCTAssertEqual(other.playlists.first?.songs, [three, four])
        XCTAssertNil(DjCrate.parse(Data("nothing here".utf8)))
        XCTAssertNil(DjCrate.parse(Data()))
    }

    func testOtherJsonShapesImport() throws {
        let array = #"["https://youtu.be/\#(one)", {"videoId":"\#(two)","title":"Two","artist":"Band","tags":"a, b"}, {"url":"nope"}]"#
        let parsed = try XCTUnwrap(DjCrate.parse(Data(array.utf8)))
        XCTAssertEqual(parsed.tracks.map(\.id), [one, two])
        XCTAssertEqual(parsed.tracks.last?.author, "Band")
        XCTAssertEqual(parsed.tracks.last?.tags, ["a", "b"])
        XCTAssertTrue(parsed.playlists.isEmpty)
        let named = #"{"name":"Road Trip","videos":[{"link":"https://www.youtube.com/watch?v=\#(three)"}]}"#
        XCTAssertEqual(DjCrate.parse(Data(named.utf8))?.playlists, [DjImportedList(name: "Road Trip", songs: [three])])
    }

    func testFileNamesAreSafe() {
        var crate = DjCrate()
        crate.createPlaylist("Late/Night: Set", id: "p")
        XCTAssertEqual(crate.fileName(.playlist("p")), "Late-Night- Set")
        XCTAssertEqual(crate.fileName(.tag("deep house")), "Tag deep house")
        XCTAssertEqual(crate.fileName(.everything), "DJ Playlists")
    }
}
