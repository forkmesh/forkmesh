import FMApi
import FMBase
import FMPackages
import Foundation
import QuartzCore
import UIKit

extension DjStudioModel {
    static let jamKey = "fm.videoMixDjStudio.jam"
    static let motionKey = "fm.videoMixDjStudio.shareMotion"

    var shownQueue: [DjQueueItem] {
        if let jam, jam.joined { return jam.queue }
        return queue
    }

    var inJam: Bool { jam?.joined == true }
    var pendingInvites: Int { jams.filter(\.invited).count }

    func enqueue(id: String, title: String, author: String) {
        if playNow(id: id, title: title, author: author) { return }
        if let jam, jam.joined {
            show(title.isEmpty ? "Added to the jam queue." : "Queued for the jam: \(title)")
            Task {
                let reply = await jamRequest("POST", "/jams/\(jam.id)/queue", [
                    "videoId": .string(id), "title": .string(title), "author": .string(author),
                ])
                receive(reply)
            }
            return
        }
        guard queue.count < DjStudio.queueLimit else {
            show("The queue is full. Play or remove a video first.")
            return
        }
        queue.append(DjQueueItem(id: UUID().uuidString, videoId: id, title: title, author: author))
        show(title.isEmpty ? "Added to the queue." : "Queued: \(title)")
        save()
    }

    func enqueueAll(_ songs: [DjTrack]) {
        guard let first = songs.first else {
            show("There are no songs to queue.")
            return
        }
        if playNow(id: first.id, title: first.title, author: first.author) {
            if songs.count > 1 { enqueueAll(Array(songs.dropFirst())) }
            return
        }
        if let jam, jam.joined {
            let room = max(0, DjStudio.queueLimit - jam.queue.count)
            let picked = Array(songs.prefix(room))
            show(picked.isEmpty ? "The jam queue is full." : "Queueing \(picked.count) songs for the jam.")
            Task {
                for song in picked {
                    let reply = await jamRequest("POST", "/jams/\(jam.id)/queue", [
                        "videoId": .string(song.id), "title": .string(song.title), "author": .string(song.author),
                    ])
                    receive(reply)
                    if reply == nil { break }
                }
            }
            return
        }
        let room = max(0, DjStudio.queueLimit - queue.count)
        let added = songs.prefix(room).map { DjQueueItem(id: UUID().uuidString, videoId: $0.id, title: $0.title, author: $0.author) }
        queue += added
        show(added.isEmpty ? "The queue is full. Play or remove a video first."
            : added.count < songs.count ? "Queued \(added.count) songs: the queue is full." : added.count == 1 ? "Queued 1 song." : "Queued \(added.count) songs.")
        save()
    }

    private func playNow(id: String, title: String, author: String) -> Bool {
        guard !automixing, !a.scratching, !b.scratching, !a.keepsPlaying, !b.keepsPlaying,
            let side = DjAutomix.playNow(a: snapshot(a), b: snapshot(b), crossfader: crossfader)
        else { return false }
        load(side, id: id, title: title, author: author)
        play(deck(side))
        return true
    }

    func playFromQueue(_ item: DjQueueItem, on side: DjDeckSide) {
        load(side, id: item.videoId, title: item.title, author: item.author)
        removeFromQueue(item)
    }

    func removeFromQueue(_ item: DjQueueItem) {
        if let current = jam, current.joined {
            jam?.queue.removeAll { $0.id == item.id }
            Task { receive(await jamRequest("DELETE", "/jams/\(current.id)/queue/\(item.id)")) }
            return
        }
        queue.removeAll { $0.id == item.id }
        save()
    }

    func share(_ side: DjDeckSide) {
        guard !applyingRemote, let jam, jam.joined else { return }
        shareTasks[side]?.cancel()
        shareTasks[side] = Task { [weak self] in
            try? await Task.sleep(for: .milliseconds(250))
            guard !Task.isCancelled, let self else { return }
            let reply = await self.jamRequest("PUT", "/jams/\(jam.id)/decks/\(side.rawValue)", self.deckState(side).body)
            if let fresh = reply.flatMap(DjJam.parse), let mine = fresh.decks[side] {
                self.applied[side] = max(self.applied[side] ?? 0, mine.at)
            }
            self.receive(reply)
            if !Task.isCancelled { self.shareTasks[side] = nil }
        }
    }

    func deckState(_ side: DjDeckSide) -> DjJamDeck {
        let deck = deck(side)
        return DjJamDeck(
            videoId: deck.videoId, title: deck.title, author: deck.author,
            playing: deck.playing, position: deck.position(), rate: deck.rate)
    }

    func refreshJams() {
        Task {
            guard let reply = await jamRequest("GET", "/jams") else { return }
            jams = DjJamSummary.list(reply)
        }
    }

    func resumeJam() {
        refreshJams()
        guard jam == nil, let id = defaults.string(forKey: Self.jamKey), !id.isEmpty else {
            if inJam { startPolling() }
            return
        }
        openJam(id)
    }

    func openJam(_ id: String) {
        Task {
            jamBusy = true
            defer { jamBusy = false }
            receive(await jamRequest("GET", "/jams/\(id)"))
        }
    }

    func startJam() {
        Task {
            jamBusy = true
            defer { jamBusy = false }
            var decks = JSONObject()
            for side in DjDeckSide.allCases where deck(side).loaded {
                decks[side.rawValue] = .object(deckState(side).body)
            }
            let reply = await jamRequest("POST", "/jams", ["decks": .object(decks)])
            receive(reply)
            if inJam { show("Jam started. Invite people to share the decks and the queue.") }
        }
    }

    func joinJam(_ id: String) {
        Task {
            jamBusy = true
            defer { jamBusy = false }
            applied = [:]
            receive(await jamRequest("POST", "/jams/\(id)/join"))
            if inJam { show("You joined the jam. The decks now follow the session.") }
            refreshJams()
        }
    }

    func leaveJam() {
        guard let current = jam else { return }
        Task {
            jamBusy = true
            defer { jamBusy = false }
            let reply = await jamRequest(current.isOwner ? "DELETE" : "POST", current.isOwner ? "/jams/\(current.id)" : "/jams/\(current.id)/leave")
            guard reply != nil else { return }
            clearJam()
            show(current.isOwner ? "Jam ended." : "You left the jam.")
            refreshJams()
        }
    }

    func invite(_ names: [String]) {
        guard let current = jam, !names.isEmpty else { return }
        Task {
            jamBusy = true
            defer { jamBusy = false }
            let reply = await jamRequest("POST", "/jams/\(current.id)/members", ["names": JSONValue(names)])
            receive(reply)
            let invited = reply?["invited"]?.arrayValue?.compactMap(\.stringValue) ?? []
            let skipped = reply?["skipped"]?.arrayValue?.compactMap(\.stringValue) ?? []
            if !skipped.isEmpty {
                jamProblem = "No ForkMesh account for \(skipped.joined(separator: ", "))."
            } else if !invited.isEmpty {
                inviteText = ""
                inviteSuggestions = []
                show("Invited \(invited.joined(separator: ", ")).")
            }
        }
    }

    func removeMember(_ name: String) {
        guard let current = jam else { return }
        Task { receive(await jamRequest("DELETE", "/jams/\(current.id)/members/\(name)")) }
    }

    func suggestPeople(_ text: String) {
        suggestTask?.cancel()
        let query = text.trimmingCharacters(in: .whitespaces).lowercased().replacingOccurrences(of: "@", with: "")
        guard query.count >= 2, let encoded = query.addingPercentEncoding(withAllowedCharacters: .urlQueryAllowed) else {
            inviteSuggestions = []
            return
        }
        suggestTask = Task { [weak self] in
            try? await Task.sleep(for: .milliseconds(250))
            guard !Task.isCancelled, let self, let api = AppServices.current?.api,
                let reply = try? await api.accountApiCall("GET", "/api/chat/direct-messages/users?query=\(encoded)")
            else { return }
            guard !Task.isCancelled else { return }
            let taken = Set((self.jam?.members ?? []).map(\.name) + [self.jam?.me ?? ""])
            self.inviteSuggestions = Array(DjJam.userNames(reply, excluding: taken).prefix(8))
        }
    }

    func receive(_ reply: JSONObject?, latency: Double = 0) {
        guard let reply else { return }
        let began = CACurrentMediaTime()
        defer { DjVitalsRecorder.shared.work(.jam, from: began, to: CACurrentMediaTime()) }
        if let fresh = DjJam.parse(reply) {
            adopt(fresh, latency: latency)
        } else if reply["jam"] == .null {
            clearJam()
        }
    }

    func clearJam() {
        jamTask?.cancel()
        jamTask = nil
        for task in shareTasks.values { task.cancel() }
        shareTasks = [:]
        applied = [:]
        jam = nil
        crowd = []
        seenReactions = []
        motion.stop()
        defaults.removeObject(forKey: Self.jamKey)
    }

    private func adopt(_ fresh: DjJam, latency: Double) {
        let continuing = jam?.id == fresh.id && jam?.joined == true
        if !continuing {
            applied = [:]
            seenReactions = fresh.reactions.map(\.id)
        }
        if jam?.shared != fresh.shared { jam = fresh }
        if crowd != fresh.presence {
            crowd = fresh.presence
            crowdDate = Date()
        }
        defaults.set(fresh.id, forKey: Self.jamKey)
        guard fresh.joined else { return }
        if continuing { hear(fresh.reactions, me: fresh.me) }
        var timed = fresh
        timed.now += Int64((latency * 1000).rounded())
        let wasPlaying = a.playing || b.playing
        for side in DjDeckSide.allCases { follow(side, timed) }
        if wasPlaying { holdAutomix() }
        startPolling()
    }

    private func hear(_ reactions: [DjJamReaction], me: String) {
        for reaction in reactions where !seenReactions.contains(reaction.id) {
            seenReactions.append(reaction.id)
            guard reaction.name != me, reaction.ago < 8000, onScreen, let kind = DjReaction.find(reaction.kind) else { continue }
            spawn(kind, from: reaction.name)
        }
        if seenReactions.count > 160 { seenReactions.removeFirst(seenReactions.count - 160) }
    }

    func react(_ reaction: DjReaction) {
        spawn(reaction, from: jam?.joined == true ? jam?.me ?? "" : "")
        guard let current = jam, current.joined else { return }
        let now = CACurrentMediaTime()
        sentReactions.removeAll { now - $0 > 12 }
        guard sentReactions.count < 8 else { return }
        sentReactions.append(now)
        Task { receive(await jamRequest("POST", "/jams/\(current.id)/reactions", ["kind": .string(reaction.id)])) }
    }

    func spawn(_ reaction: DjReaction, from name: String) {
        if effects.count >= 10 { effects.removeFirst(effects.count - 9) }
        let effect = DjEffect(reaction: reaction, sender: name)
        effects.append(effect)
        Task { [weak self] in
            try? await Task.sleep(for: .seconds(reaction.seconds))
            self?.effects.removeAll { $0.id == effect.id }
        }
    }

    func setShareMotion(_ on: Bool) {
        shareMotion = on
        defaults.set(on, forKey: Self.motionKey)
        if !on { motion.stop() }
    }

    func status(of member: DjJamMember) -> DjPersonStatus {
        guard member.name == jam?.me else { return DjJam.status(of: member, seen: crowd.first { $0.name == member.name }) }
        if onScreen, UIApplication.shared.applicationState != .background { return .studio }
        return a.playing || b.playing ? .listening : .idle
    }

    private var presenceBody: JSONObject {
        let playing = a.playing || b.playing
        let visible = onScreen && UIApplication.shared.applicationState != .background
        let state: DjPresenceState = visible ? .studio : playing ? .listening : .idle
        return DjJamPresence.body(state: state, playing: playing, motion: shareMotion ? motion.drain() : [])
    }

    private func follow(_ side: DjDeckSide, _ jam: DjJam) {
        guard let remote = jam.decks[side], remote.at > (applied[side] ?? 0) else { return }
        applied[side] = remote.at
        guard remote.by != jam.me else { return }
        let deck = deck(side)
        guard !deck.scratching, shareTasks[side] == nil else { return }
        applyingRemote = true
        defer { applyingRemote = false }
        guard !remote.videoId.isEmpty else {
            if deck.loaded { eject(side) }
            return
        }
        let target = remote.expectedPosition(serverNow: jam.now)
        if deck.videoId != remote.videoId {
            prepare(deck, id: remote.videoId, title: remote.title, author: remote.author, start: target, keepSettings: false)
        }
        if abs(deck.rate - remote.rate) > 0.0005 { setRate(side, remote.rate) }
        if DjJam.needsSeek(local: deck.position(), remote: target, playing: remote.playing) {
            deck.player.seek(target)
            deck.time = target
            deck.tickDate = CACurrentMediaTime()
            deck.settle()
        }
        if remote.playing, !deck.playing {
            play(deck)
        } else if !remote.playing, deck.playing {
            pause(deck)
        }
    }

    private func startPolling() {
        guard jamTask == nil else { return }
        jamTask = Task { [weak self] in
            while !Task.isCancelled {
                let pause = self?.onScreen == true ? 1.5 : 2.5
                try? await Task.sleep(for: .seconds(pause))
                guard !Task.isCancelled, let self else { return }
                guard let current = self.jam, current.joined else {
                    self.motion.stop()
                    self.jamTask = nil
                    return
                }
                guard self.onScreen || self.a.playing || self.b.playing else {
                    self.motion.stop()
                    continue
                }
                if self.shareMotion { self.motion.start() }
                let began = CACurrentMediaTime()
                let reply = await self.jamRequest("POST", "/jams/\(current.id)/presence", self.presenceBody)
                guard !Task.isCancelled else { return }
                self.receive(reply, latency: (CACurrentMediaTime() - began) / 2)
            }
        }
    }

    func jamRequest(_ method: String, _ path: String, _ body: JSONObject? = nil) async -> JSONObject? {
        guard let api = AppServices.current?.api, api.hasSession else {
            jamProblem = "Sign in to ForkMesh to jam with other people."
            return nil
        }
        do {
            let reply = try await api.videoMixDjApi(method, path, body)
            jamProblem = ""
            return reply
        } catch let error as OrgApiException {
            switch error.code {
            case "jam_not_found":
                if path.hasPrefix("/jams/\(jam?.id ?? "-")") {
                    clearJam()
                    show("That jam has ended.")
                }
                jamProblem = "That jam has ended or you were removed from it."
            case "too_many_jams": jamProblem = "You already have 20 jams. End one first."
            case "queue_full": jamProblem = "The jam queue is full."
            case "too_many_reactions": jamProblem = "Slow down a little: reactions are limited to eight every 12 seconds."
            case "jam_not_joined": jamProblem = "Join the jam first."
            default: jamProblem = error.message
            }
            return nil
        } catch {
            jamProblem = "ForkMesh could not be reached. Check your connection."
            return nil
        }
    }
}
