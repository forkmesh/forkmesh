import FMPackages
import Foundation
import XCTest

final class DjAutoDjTests: XCTestCase {
    let seed = DjTrack(id: "dQw4w9WgXcQ", title: "Night Owls - Sunrise (Official Video)", author: "Night Owls")

    func testCandidatesReadRecommendationsWithViewsAndSkipNonVideos() {
        let data = Data("""
            {"recommendedVideos": [
              {"videoId": "9bZkp7q19f0", "title": "Lights Out", "author": "Night Owls", "lengthSeconds": 215, "viewCount": 1200000},
              {"videoId": "kJQP7kiw5Fk", "title": "Study Beats", "author": "Quiet Room", "lengthSeconds": 190, "viewCountText": "3.4M views"},
              {"videoId": "9bZkp7q19f0", "title": "Lights Out again"},
              {"type": "playlist", "title": "A playlist"},
              {"title": "No id"}
            ]}
            """.utf8)
        let found = DjAutoDj.candidates(data)
        XCTAssertEqual(found.map(\.id), ["9bZkp7q19f0", "kJQP7kiw5Fk"])
        XCTAssertEqual(found.map(\.rank), [0, 1])
        XCTAssertEqual(found[0].views, 1_200_000)
        XCTAssertEqual(found[1].views, 3_400_000)
        XCTAssertEqual(found[1].seconds, 190)
        XCTAssertEqual(DjAutoDj.candidates(Data("[{\"videoId\":\"JGwWNGJdvx8\",\"title\":\"Search hit\",\"lengthSeconds\":200}]".utf8)).map(\.id), ["JGwWNGJdvx8"])
        XCTAssertEqual(DjAutoDj.candidates(Data("nope".utf8)), [])
    }

    func testCountReadsYouTubeViewText() {
        XCTAssertEqual(DjAutoDj.count("1,234,567 views"), 1_234_567)
        XCTAssertEqual(DjAutoDj.count("12K views"), 12_000)
        XCTAssertEqual(DjAutoDj.count("2.5B"), 2_500_000_000)
        XCTAssertEqual(DjAutoDj.count("No views"), 0)
    }

    func testLikesComeFromTheVideoRecord() {
        let likes = DjAutoDj.likes(Data("{\"likeCount\": 52000, \"viewCount\": 1000000}".utf8))
        XCTAssertEqual(likes?.likes, 52000)
        XCTAssertEqual(likes?.views, 1_000_000)
        XCTAssertNil(DjAutoDj.likes(Data("{\"viewCount\": 10}".utf8)))
        XCTAssertEqual(
            DjAutoDj.likesURL(URL(string: "https://inv.example")!, id: "9bZkp7q19f0")?.absoluteString,
            "https://inv.example/api/v1/videos/9bZkp7q19f0?fields=likeCount,viewCount")
        XCTAssertNil(DjAutoDj.likesURL(URL(string: "https://inv.example")!, id: "bad"))
    }

    func testSongKeyIgnoresArtistBracketsAndVideoWords() {
        XCTAssertEqual(DjAutoDj.songKey("Night Owls - Sunrise (Official Video)"), "sunrise")
        XCTAssertEqual(DjAutoDj.songKey("Sunrise [HD] Lyrics"), "sunrise")
        XCTAssertEqual(DjAutoDj.songKey("Sunrise (Remix)"), "sunrise")
        XCTAssertEqual(DjAutoDj.songKey("Lights Out"), "lights out")
        XCTAssertEqual(DjAutoDj.songKey("Official Video - Lights Out"), "lights out")
    }

    func testEligibleLeavesOutTheSeedRepeatsOtherUploadsAndOddLengths() {
        let found = [
            DjAutoDjCandidate(id: seed.id, title: "Sunrise", seconds: 200, rank: 0),
            DjAutoDjCandidate(id: "9bZkp7q19f0", title: "Sunrise (Lyrics)", author: "Fan", seconds: 200, rank: 1),
            DjAutoDjCandidate(id: "kJQP7kiw5Fk", title: "Lights Out", seconds: 200, rank: 2),
            DjAutoDjCandidate(id: "JGwWNGJdvx8", title: "Night Owls 3 hour mix", seconds: 3 * 3600, rank: 3),
            DjAutoDjCandidate(id: "OPf0YbXqDm0", title: "Short clip", seconds: 20, rank: 4),
            DjAutoDjCandidate(id: "YQHsXMglC9A", title: "Live now", seconds: 0, rank: 5),
            DjAutoDjCandidate(id: "fJ9rUzIMcZQ", title: "Heard Before", seconds: 200, rank: 6),
            DjAutoDjCandidate(id: "hTWKbfoikeg", title: "On a deck", seconds: 200, rank: 7),
            DjAutoDjCandidate(id: "RgKAFK5djSk", title: "Lights Out (Official Audio)", seconds: 200, rank: 8),
            DjAutoDjCandidate(id: "CevxZvSJLk8", title: "Fresh Song", seconds: 240, rank: 9),
        ]
        let kept = DjAutoDj.eligible(found, seed: seed, avoid: ["hTWKbfoikeg"], heard: ["Someone - Heard Before"])
        XCTAssertEqual(kept.map(\.id), ["RgKAFK5djSk", "CevxZvSJLk8"])
    }

    func testOnlySongsQualifyNotTalentShowsCoversOrLiveTakes() {
        let song = { (title: String, author: String) in DjAutoDjCandidate(id: "9bZkp7q19f0", title: title, author: author, seconds: 200) }
        XCTAssertFalse(DjAutoDj.isSong(song("Golden Buzzer: Teen Sings Hallelujah | America's Got Talent 2024", "America's Got Talent")))
        XCTAssertFalse(DjAutoDj.isSong(song("Nervous Singer STUNS The Judges | Auditions | BGT 2023", "Britain's Got Talent")))
        XCTAssertFalse(DjAutoDj.isSong(song("Hallelujah", "The X Factor UK")))
        XCTAssertFalse(DjAutoDj.isSong(song("Vocal Coach Reacts to Queen", "Music Room")))
        XCTAssertFalse(DjAutoDj.isSong(song("Bohemian Rhapsody (Live Aid 1985)", "Queen")))
        XCTAssertFalse(DjAutoDj.isSong(song("Queen - Bohemian Rhapsody - Live", "Fan")))
        XCTAssertFalse(DjAutoDj.isSong(song("Hallelujah | Acoustic Cover by Sam", "Sam")))
        XCTAssertFalse(DjAutoDj.isSong(song("Hallelujah piano cover", "Keys")))
        XCTAssertFalse(DjAutoDj.isSong(song("Sunrise (Sped Up)", "Edits")))
        XCTAssertFalse(DjAutoDj.isSong(song("Night Owls - Sunrise (Karaoke Version)", "Sing King")))
        XCTAssertTrue(DjAutoDj.isSong(song("Oasis - Live Forever (Remastered)", "Oasis")))
        XCTAssertTrue(DjAutoDj.isSong(song("Queen - Bohemian Rhapsody (Official Video Remastered)", "Queen Official")))
        XCTAssertTrue(DjAutoDj.isSong(song("Hallelujah", "Jeff Buckley - Topic")))
        XCTAssertTrue(DjAutoDj.isSong(song("Billy Idol - White Wedding", "Billy Idol")))
    }

    func testOriginalUploadsAreTheArtistsOwn() {
        let song = { (title: String, author: String) in DjAutoDjCandidate(id: "9bZkp7q19f0", title: title, author: author, seconds: 200) }
        XCTAssertTrue(DjAutoDj.isOriginal(song("Hallelujah", "Jeff Buckley - Topic")))
        XCTAssertTrue(DjAutoDj.isOriginal(song("Lights Out", "NightOwlsVEVO")))
        XCTAssertTrue(DjAutoDj.isOriginal(song("Night Owls - Lights Out (Official Audio)", "Records Ltd")))
        XCTAssertTrue(DjAutoDj.isOriginal(song("Oasis - Live Forever (Remastered)", "Big Brother")))
        XCTAssertTrue(DjAutoDj.isOriginal(song("Night Owls – Lights Out", "Night Owls")))
        XCTAssertFalse(DjAutoDj.isOriginal(song("Night Owls - Lights Out", "Lyrics Hub")))
        XCTAssertFalse(DjAutoDj.isOriginal(song("Lights Out", "")))
    }

    func testEligibleLeavesOutNonSongsAndPutsOriginalsFirst() {
        let found = [
            DjAutoDjCandidate(id: "9bZkp7q19f0", title: "Teen Sings Sunrise | AGT 2024", author: "America's Got Talent", seconds: 300, rank: 0),
            DjAutoDjCandidate(id: "kJQP7kiw5Fk", title: "Lights Out", author: "Lyrics Hub", seconds: 200, rank: 1),
            DjAutoDjCandidate(id: "JGwWNGJdvx8", title: "Moonset (Live)", author: "Night Owls", seconds: 200, rank: 2),
            DjAutoDjCandidate(id: "CevxZvSJLk8", title: "Fresh Song", author: "Night Owls - Topic", seconds: 240, rank: 3),
        ]
        let kept = DjAutoDj.eligible(found, seed: seed, avoid: [], heard: [])
        XCTAssertEqual(kept.map(\.id), ["CevxZvSJLk8", "kJQP7kiw5Fk"])
        let liked = [
            DjAutoDjCandidate(id: "kJQP7kiw5Fk", title: "Lights Out", author: "Lyrics Hub", seconds: 200, views: 9_000_000, likes: 400_000, rank: 0),
            DjAutoDjCandidate(id: "CevxZvSJLk8", title: "Fresh Song", author: "Night Owls - Topic", seconds: 240, views: 3000, likes: 20, rank: 1),
        ]
        XCTAssertEqual(DjAutoDj.ranked(liked).map(\.id), ["CevxZvSJLk8", "kJQP7kiw5Fk"])
    }

    func testRankingPrefersLikedSongsAmongRelatedOnes() {
        let found = [
            DjAutoDjCandidate(id: "9bZkp7q19f0", title: "Most related, few likes", seconds: 200, views: 20000, likes: 150, rank: 0),
            DjAutoDjCandidate(id: "kJQP7kiw5Fk", title: "Loved", seconds: 200, views: 5_000_000, likes: 240_000, rank: 1),
            DjAutoDjCandidate(id: "JGwWNGJdvx8", title: "Popular, unchecked", seconds: 200, views: 3_000_000, rank: 2),
        ]
        XCTAssertEqual(DjAutoDj.ranked(found).map(\.id), ["kJQP7kiw5Fk", "JGwWNGJdvx8", "9bZkp7q19f0"])
        let even = [
            DjAutoDjCandidate(id: "9bZkp7q19f0", title: "First", seconds: 200, views: 1000, likes: 50, rank: 0),
            DjAutoDjCandidate(id: "kJQP7kiw5Fk", title: "Second", seconds: 200, views: 1000, likes: 50, rank: 1),
        ]
        XCTAssertEqual(DjAutoDj.ranked(even).map(\.id), ["9bZkp7q19f0", "kJQP7kiw5Fk"])
    }

    func testNextSkipsLoadedFailedAndPlayedSongs() {
        let ranked = ["9bZkp7q19f0", "kJQP7kiw5Fk", "JGwWNGJdvx8", "OPf0YbXqDm0"].map { DjAutoDjCandidate(id: $0, title: $0, seconds: 200) }
        XCTAssertEqual(DjAutoDj.next(ranked, loaded: ["9bZkp7q19f0"], skip: ["kJQP7kiw5Fk"], played: ["JGwWNGJdvx8"])?.id, "OPf0YbXqDm0")
        XCTAssertNil(DjAutoDj.next(ranked, loaded: [], skip: [], played: ranked.map(\.id)))
    }

    func testSavedStateKeepsAutoDjOnlyWithAutomix() {
        var saved = DjSaved()
        saved.automix = true
        saved.autoDj = true
        XCTAssertTrue(DjSaved.decode(saved.encoded()).autoDj)
        XCTAssertFalse(DjSaved.decode(Data(#"{"automix":false,"autoDj":true}"#.utf8)).autoDj)
        XCTAssertFalse(DjSaved.decode(Data(#"{"automix":true}"#.utf8)).autoDj)
    }

    func testRememberKeepsTheSessionNewestFirstWithoutRepeats() {
        XCTAssertEqual(DjAutoDj.remember("b", in: ["a", "b", "c"]), ["b", "a", "c"])
        XCTAssertEqual(DjAutoDj.remember("x", in: (0..<DjAutoDj.sessionLimit).map(String.init)).count, DjAutoDj.sessionLimit)
    }
}
