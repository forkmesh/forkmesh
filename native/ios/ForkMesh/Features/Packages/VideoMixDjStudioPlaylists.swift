import FMPackages
import Foundation

extension DjStudioModel {
    func song(_ id: String) -> DjTrack {
        if let track = crate.track(id) { return track }
        if let played = library.first(where: { $0.id == id }) { return DjTrack(id: id, title: played.title, author: played.author) }
        if let item = history.first(where: { $0.id == id }) { return DjTrack(id: id, title: item.title, author: item.author) }
        if let deck = [a, b].first(where: { $0.videoId == id }) { return DjTrack(id: id, title: deck.title, author: deck.author) }
        if let item = shownQueue.first(where: { $0.videoId == id }) { return DjTrack(id: id, title: item.title, author: item.author) }
        if let result = searchResults.first(where: { $0.id == id }) { return DjTrack(id: id, title: result.title, author: result.author) }
        return DjTrack(id: id)
    }

    var librarySongs: [DjTrack] {
        let kept = Dictionary(crate.tracks.map { ($0.id, $0) }, uniquingKeysWith: { first, _ in first })
        let played = Set(library.map(\.id))
        return library.map { kept[$0.id] ?? DjTrack(id: $0.id, title: $0.title, author: $0.author) }
            + crate.tracks.filter { !played.contains($0.id) }.reversed()
    }

    func refreshSmart(now: Bool = false) {
        smartTask?.cancel()
        let songs = librarySongs
        let library = library
        let time = Date().timeIntervalSince1970
        guard !now else {
            crate.categorize(songs, library: library, now: time)
            return
        }
        smartTask = Task { [weak self] in
            let lists = await Task.detached(priority: .userInitiated) { DjSmart.lists(songs, library: library, now: time) }.value
            guard !Task.isCancelled, let self, self.crate.smart != lists else { return }
            self.crate.setSmart(lists)
        }
    }

    func similarSongs(_ side: DjDeckSide) -> [DjSimilarSong] {
        let deck = deck(side)
        guard deck.loaded else { return [] }
        let loaded = Set([a, b].map(\.videoId))
        return DjSmart.similar(to: song(deck.videoId), among: librarySongs.filter { !loaded.contains($0.id) }, playlists: crate.playlists)
    }

    func toggleShuffle(_ id: String) {
        let track = song(id)
        let on = !crate.shuffles(id)
        guard crate.setShuffles(id, on, title: track.title, author: track.author) else {
            show("The library is full: remove a song first.")
            return
        }
        saveCrate()
        let name = track.title.isEmpty ? "This song" : track.title
        show(on ? "\(name) is back in the shuffle." : "\(name) will not come up in shuffle.")
    }

    func saveSmart(_ id: String) {
        guard let list = crate.smartList(id) else { return }
        newPlaylist(list.name, with: list.songs.map(\.id))
    }

    func editSong(_ id: String) {
        guard DjStudio.isVideoId(id) else { return }
        sheet = .song(id)
    }

    func addTags(_ raw: String, to id: String) {
        let known = song(id)
        guard !DjCrate.tagNames(raw).isEmpty else { return }
        guard crate.tag(id, raw, title: known.title, author: known.author) else {
            show("The library is full: remove a song to tag another.")
            return
        }
        saveCrate()
    }

    func toggleTag(_ name: String, on id: String) {
        if crate.track(id)?.tags.contains(name) == true {
            crate.untag(id, name)
            saveCrate()
        } else {
            addTags(name, to: id)
        }
    }

    func renameTag(_ old: String, to raw: String) {
        guard crate.renameTag(old, to: raw) else { return }
        saveCrate()
    }

    func deleteTag(_ name: String) {
        crate.deleteTag(name)
        saveCrate()
        show("Removed the #\(name) tag.")
    }

    func inPlaylist(_ id: String, _ playlist: String) -> Bool {
        crate.playlist(playlist)?.songs.contains(id) == true
    }

    func toggle(_ id: String, in playlist: String) {
        guard let list = crate.playlist(playlist) else { return }
        if list.songs.contains(id) {
            crate.remove(id, from: playlist)
            show("Removed from \(list.name).")
        } else if crate.add([song(id)], to: playlist) > 0 {
            show("Added to \(list.name).")
        } else {
            show(list.songs.count >= DjCrate.playlistSongLimit ? "\(list.name) is full." : "The library is full: remove a song first.")
        }
        saveCrate()
    }

    @discardableResult
    func newPlaylist(_ name: String, with ids: [String] = []) -> String? {
        guard let id = crate.createPlaylist(name) else {
            show("You already have \(DjCrate.playlistLimit) playlists.")
            return nil
        }
        crate.add(ids.map { song($0) }, to: id)
        saveCrate()
        show("Created \(crate.label(.playlist(id))).")
        return id
    }

    func renamePlaylist(_ id: String, _ name: String) {
        crate.renamePlaylist(id, name)
        saveCrate()
    }

    func deletePlaylist(_ id: String) {
        let name = crate.label(.playlist(id))
        crate.deletePlaylist(id)
        saveCrate()
        show("Deleted \(name).")
    }

    func removeSongs(_ offsets: IndexSet, from playlist: String) {
        crate.removeSongs(at: offsets, from: playlist)
        saveCrate()
    }

    func moveSongs(in playlist: String, from offsets: IndexSet, to destination: Int) {
        crate.moveSongs(in: playlist, from: offsets, to: destination)
        saveCrate()
    }

    func removeSong(_ id: String) {
        crate.removeTrack(id)
        saveCrate()
        forgetPlayed(id)
    }

    func addLink(_ text: String, to playlist: String) -> Bool {
        let id = DjStudio.videoId(text)
        guard !id.isEmpty else { return false }
        if !inPlaylist(id, playlist) { toggle(id, in: playlist) }
        return true
    }

    func move(_ side: DjDeckSide, to target: DjFocus) {
        let deck = deck(side)
        guard deck.loaded else { return }
        let id = deck.videoId
        let track = song(id)
        let from = crate.focus
        let fromLabel = crate.label(from)
        let result = crate.move(id, title: track.title, author: track.author, to: target)
        guard result != .full else {
            show("\(crate.label(target)) or the library is full.")
            return
        }
        saveCrate()
        let done: String
        switch result {
        case .removed: done = "Took it out of \(fromLabel)"
        case .moved: done = "Moved from \(fromLabel) to \(crate.label(target))"
        default: done = "Added to \(crate.label(target))"
        }
        guard automixOn, result == .moved || result == .removed, !crate.songs(from).contains(where: { $0.id == id }) else {
            show(done + ".")
            return
        }
        if skipOut(deck) {
            show(done + ". Skipping to the next song.")
        } else {
            show(done + ". Nothing else in \(fromLabel) to play.")
        }
    }

    func setShuffle(_ on: Bool) {
        crate.shuffle = on
        saveCrate()
    }

    func focusAutomix(_ focus: DjFocus, start: Bool) {
        crate.setFocus(focus)
        saveCrate()
        if start, !automixOn { setAutomix(true) }
        refocusAutomix()
        let label = crate.focus == .everything ? "everything you have played and saved" : crate.label(crate.focus) + (crate.shuffle ? ", shuffled" : " in order")
        show(automixOn ? "AUTOMIX plays \(label)." : "AUTOMIX will play \(label).")
    }

    func queueAll(_ focus: DjFocus, shuffled: Bool) {
        var songs = crate.songs(focus)
        if shuffled { songs.shuffle() }
        enqueueAll(songs)
    }

    func loadTrack(_ track: DjTrack, on side: DjDeckSide) {
        load(side, id: track.id, title: track.title, author: track.author)
        sheet = nil
    }

    func importData(_ data: Data, name: String) {
        guard let found = DjCrate.parse(data) else {
            show("No YouTube songs were found to import.")
            return
        }
        let report = crate.merge(found, name: name.isEmpty ? "Imported" : name)
        saveCrate()
        show(report.summary)
    }
}
