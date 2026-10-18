import FMPackages
import Foundation
import XCTest

final class DjSmartTests: XCTestCase {
    let one = "dQw4w9WgXcQ"
    let two = "9bZkp7q19f0"
    let three = "kJQP7kiw5Fk"
    let four = "JGwWNGJdvx8"
    let five = "OPf0YbXqDm0"

    var songs: [DjTrack] {
        [
            DjTrack(id: one, title: "Sunrise (Deep House Mix) 1998", author: "Night Owls - Topic"),
            DjTrack(id: two, title: "Lights Out - Deep House Remix", author: "Night Owls"),
            DjTrack(id: three, title: "Study Beats", author: "Quiet Room", tags: ["lofi"]),
            DjTrack(id: four, title: "Rain Window lofi hip-hop", author: "Quiet Room"),
            DjTrack(id: five, title: "Old Song (Official Video) 1994", author: ""),
        ]
    }

    func testTextIsLowercasedWordsWithAmpersands() {
        XCTAssertEqual(DjSmart.text("Lo-Fi  Hip-Hop / R&B!"), " lo fi hip hop r&b ")
    }

    func testArtistDropsTopicAndVevoAndFallsBackToTheTitle() {
        XCTAssertEqual(DjSmart.artist(DjTrack(id: one, author: "Night Owls - Topic")), "Night Owls")
        XCTAssertEqual(DjSmart.artist(DjTrack(id: one, author: "SomeoneVEVO")), "Someone")
        XCTAssertEqual(DjSmart.artist(DjTrack(id: one, title: "Band Name - The Song")), "Band Name")
        XCTAssertEqual(DjSmart.artist(DjTrack(id: one, title: "No dash here")), "")
    }

    func testDecadesComeFromYearsAndShortForms() {
        XCTAssertEqual(DjSmart.decade(DjTrack(id: one, title: "Track (1987)")), 1980)
        XCTAssertEqual(DjSmart.decade(DjTrack(id: one, title: "Best of 90s dance")), 1990)
        XCTAssertNil(DjSmart.decade(DjTrack(id: one, title: "Track 3000")))
    }

    func testListsGroupSongsByGenreStyleDecadeArtistAndListening() {
        let library = [
            DjPlayedSong(id: one, title: "", author: "", plays: 4, last: 100),
            DjPlayedSong(id: two, title: "", author: "", plays: 2, last: 300),
            DjPlayedSong(id: three, title: "", author: "", plays: 1, last: 200),
        ]
        let lists = DjSmart.lists(songs, library: library, now: 400)
        let byId = Dictionary(uniqueKeysWithValues: lists.map { ($0.id, $0) })
        XCTAssertEqual(byId["genre:house"]?.songs.map(\.id), [one, two])
        XCTAssertEqual(byId["genre:chill"]?.songs.map(\.id), [three, four])
        XCTAssertEqual(byId["decade:1990"]?.songs.map(\.id), [one, five])
        XCTAssertEqual(byId["artist:night owls"]?.name, "Night Owls")
        XCTAssertEqual(byId["artist:night owls"]?.songs.map(\.id), [one, two])
        XCTAssertEqual(byId["artist:quiet room"]?.songs.map(\.id), [three, four])
        XCTAssertEqual(byId["listen:most"]?.songs.map(\.id), [one, two])
        XCTAssertEqual(byId["listen:recent"]?.songs.map(\.id), [two, three, one])
        XCTAssertEqual(byId["listen:new"]?.songs.map(\.id), [four, five])
        XCTAssertNil(byId["genre:hiphop"], "a category with one song is left out")
        XCTAssertNil(byId["listen:rediscover"])
        XCTAssertEqual(DjSmart.lists(songs, library: library, now: 400 + DjSmart.rediscoverAfter + 200).first { $0.id == "listen:rediscover" }?.songs.map(\.id), [one, two, three])
    }

    func testSimilarRanksArtistTagsPlaylistsAndGenres() {
        let seed = DjTrack(id: four, title: "Rain Window lofi hip-hop", author: "Quiet Room", tags: ["lofi"])
        let pool = songs + [DjTrack(id: "aaaaaaaaaaa", title: "Unrelated", author: "Other")]
        let found = DjSmart.similar(to: seed, among: pool, playlists: [DjPlaylist(id: "p", name: "Evening", songs: [four, one])])
        XCTAssertEqual(found.map(\.id), [three, one])
        XCTAssertEqual(found.first?.reasons.first, "Same artist")
        XCTAssertTrue(found.first?.reasons.contains("#lofi") == true)
        XCTAssertEqual(found.last?.reasons, ["Evening"])
        XCTAssertFalse(found.contains { $0.id == four })
    }

    func testSearchQueryUsesTheArtistOrTheCleanedTitle() {
        XCTAssertEqual(DjSmart.searchQuery(DjTrack(id: one, title: "Sunrise deep house", author: "Night Owls - Topic")), "Night Owls House")
        XCTAssertEqual(DjSmart.searchQuery(DjTrack(id: one, title: "Sunrise (Official Video) [HD]")), "Sunrise")
    }

    func testSimilarResultsReadRecommendedVideos() throws {
        let body = #"{"recommendedVideos":[{"videoId":"dQw4w9WgXcQ","title":"One","author":"A","lengthSeconds":212},{"videoId":"dQw4w9WgXcQ","title":"Again"},{"videoId":"bad"}]}"#
        let found = DjStudio.similarResults(Data(body.utf8))
        XCTAssertEqual(found, [DjSearchResult(id: one, title: "One", author: "A", seconds: 212)])
        XCTAssertEqual(
            DjStudio.similarURL(try XCTUnwrap(URL(string: "https://example.com")), id: one)?.absoluteString,
            "https://example.com/api/v1/videos/\(one)?fields=recommendedVideos")
        XCTAssertNil(DjStudio.similarURL(try XCTUnwrap(URL(string: "https://example.com")), id: "bad"))
    }

    func testSmartFocusPlaysItsSongsAndIsDroppedWhenTheListGoes() {
        var crate = DjCrate()
        crate.categorize(songs, library: [], now: 0)
        crate.setFocus(.smart("genre:house"))
        XCTAssertEqual(crate.focus, .smart("genre:house"))
        XCTAssertEqual(crate.label(crate.focus), "House")
        XCTAssertEqual(crate.pool([]).map(\.id), [one, two])
        XCTAssertEqual(crate.move(one, to: .tag("keep")), .added)
        XCTAssertEqual(crate.focus, .smart("genre:house"))
        let saved = DjCrate.decode(crate.encoded())
        XCTAssertEqual(saved.focus, .smart("genre:house"))
        XCTAssertTrue(saved.smart.isEmpty)
        crate.categorize([songs[2], songs[3]], library: [], now: 0)
        XCTAssertEqual(crate.focus, .everything)
    }

    func testSmartListsExportAsAPlaylist() throws {
        var crate = DjCrate()
        crate.categorize(songs, library: [], now: 0)
        let data = crate.exported(.smart("genre:house"))
        let object = try XCTUnwrap(JSONSerialization.jsonObject(with: data) as? [String: Any])
        let lists = try XCTUnwrap(object["playlists"] as? [[String: Any]])
        XCTAssertEqual(lists.first?["name"] as? String, "House")
        XCTAssertEqual(lists.first?["songs"] as? [String], [one, two])
        XCTAssertEqual(crate.fileName(.smart("genre:house")), "House")
    }
}
