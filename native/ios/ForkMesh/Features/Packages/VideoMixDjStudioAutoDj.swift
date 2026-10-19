import FMPackages
import Foundation
import SwiftUI

extension DjStudioModel {
    func autoDj() {
        let wasMixing = automixOn
        setAutoDj(!autoDjOn)
        show(!autoDjOn ? (automixOn ? "AUTO DJ off. AUTOMIX keeps playing your queue and library." : "AUTO DJ off.")
            : a.playing || b.playing ? "AUTO DJ on: when this song ends, a related song people liked plays on the next deck."
            : wasMixing ? "AUTO DJ on: related songs people liked follow each song."
            : "AUTO DJ on: start a song and related songs people liked follow it.")
    }

    func setAutoDj(_ on: Bool, refresh: Bool = true) {
        guard on != autoDjOn else { return }
        guard on else {
            let owned = autoDjOwnsAutomix
            stopAutoDj()
            if owned, automixOn { setAutomix(false) }
            save()
            return
        }
        autoDjOn = true
        autoDjRefresh = refresh
        if !automixOn {
            setAutomix(true)
            autoDjOwnsAutomix = true
        }
        primeAutoDj()
        save()
    }

    func stopAutoDj() {
        autoDjTask?.cancel()
        autoDjTask = nil
        autoDjOn = false
        autoDjFinding = false
        autoDjSeed = ""
        autoDjChoices = []
        autoDjOwnsAutomix = false
        autoDjRefresh = false
    }

    var autoDjSeedTrack: DjTrack? {
        let playing = [a, b].filter { $0.playing && $0.loaded }
        if let live = playing.count > 1 ? deck(crossfader <= 0 ? .a : .b) : playing.first { return song(live.videoId) }
        return history.first.map { song($0.id) }
    }

    func primeAutoDj() {
        guard autoDjOn, let seed = autoDjSeedTrack, seed.id != autoDjSeed else { return }
        findAutoDj(seed)
    }

    func autoDjNext() -> DjAutoDjCandidate? {
        DjAutoDj.next(autoDjChoices, loaded: Set([a, b].filter(\.loaded).map(\.videoId)), skip: unplayable, played: autoDjPlayed)
    }

    func takeAutoDj() -> DjAutoDjCandidate? {
        guard let pick = autoDjNext() else { return nil }
        autoDjPlayed = DjAutoDj.remember(pick.id, in: autoDjPlayed)
        return pick
    }

    private func findAutoDj(_ seed: DjTrack) {
        autoDjTask?.cancel()
        autoDjSeed = seed.id
        autoDjChoices = []
        autoDjFinding = true
        let decks = [a, b].filter(\.loaded)
        let avoid = Set(history.map(\.id) + decks.map(\.videoId) + autoDjPlayed).union(unplayable)
        let heard = history.map(\.title) + decks.map(\.title)
        autoDjTask = Task { [weak self] in
            guard let self else { return }
            var instance: URL?
            var fresh: [DjAutoDjCandidate] = []
            if let found = await self.fetchInvidiousData({ DjStudio.similarURL($0, id: seed.id) }) {
                instance = found.instance
                fresh = DjAutoDj.eligible(DjAutoDj.candidates(found.data), seed: seed, avoid: avoid, heard: heard)
            }
            let query = DjSmart.searchQuery(seed)
            if fresh.isEmpty, !Task.isCancelled, query.count >= 2, let found = await self.fetchInvidiousData({ DjStudio.searchURL($0, query: query) }) {
                instance = found.instance
                fresh = DjAutoDj.eligible(DjAutoDj.candidates(found.data), seed: seed, avoid: avoid, heard: heard)
            }
            if let instance, !fresh.isEmpty, !Task.isCancelled {
                let top = fresh.prefix(DjAutoDj.checked)
                let liked = await withTaskGroup(of: (String, Double, Double)?.self, returning: [String: (likes: Double, views: Double)].self) { group in
                    for candidate in top {
                        group.addTask { await DjStudioModel.likes(candidate.id, on: instance).map { (candidate.id, $0.likes, $0.views) } }
                    }
                    var counts: [String: (likes: Double, views: Double)] = [:]
                    for await item in group { if let item { counts[item.0] = (item.1, item.2) } }
                    return counts
                }
                for index in fresh.indices {
                    guard let count = liked[fresh[index].id] else { continue }
                    fresh[index].likes = count.likes
                    if count.views > 0 { fresh[index].views = count.views }
                }
                fresh = DjAutoDj.ranked(Array(fresh.prefix(DjAutoDj.checked))) + DjAutoDj.ranked(Array(fresh.dropFirst(DjAutoDj.checked)))
            }
            guard !Task.isCancelled, self.autoDjOn, self.autoDjSeed == seed.id else { return }
            self.autoDjChoices = fresh
            self.autoDjFinding = false
            if fresh.isEmpty {
                self.show(instance == nil ? "AUTO DJ can’t reach YouTube suggestions right now, so AUTOMIX plays your library." : "AUTO DJ found nothing new like this song, so AUTOMIX plays your library.")
            }
            self.refreshAutoDj()
        }
    }

    private func refreshAutoDj() {
        guard autoDjRefresh else { return }
        autoDjRefresh = false
        guard automixOn, !automixing, shownQueue.isEmpty, autoDjNext() != nil else { return }
        let playing = [a, b].filter(\.playing)
        guard playing.count == 1, let live = playing.first else { return }
        let idle = deck(live.side.other)
        guard !idle.scratching, !idle.keepsPlaying else { return }
        preload(idle.side)
    }

    nonisolated static func likes(_ id: String, on instance: URL) async -> (likes: Double, views: Double)? {
        guard let url = DjAutoDj.likesURL(instance, id: id) else { return nil }
        var request = URLRequest(url: url)
        request.timeoutInterval = 6
        guard let (data, response) = try? await URLSession.shared.data(for: request), (response as? HTTPURLResponse)?.statusCode == 200 else { return nil }
        return DjAutoDj.likes(data)
    }
}

struct DjAutoDjButton: View {
    let model: DjStudioModel
    let size: CGFloat

    var body: some View {
        let on = model.autoDjOn
        DjRoundButton(size: size, lit: on, action: { model.autoDj() }) {
            Image(systemName: model.autoDjFinding ? "ellipsis" : "wand.and.stars")
        }
        .animation(.easeOut(duration: 0.2), value: on)
        .accessibilityLabel("Auto DJ")
        .accessibilityValue(on ? (model.autoDjFinding ? "On, finding the next song" : "On") : "Off")
        .accessibilityAddTraits(on ? .isSelected : [])
        .accessibilityHint("When on, each song is followed on the next deck by a related song people liked, found on YouTube")
    }
}
