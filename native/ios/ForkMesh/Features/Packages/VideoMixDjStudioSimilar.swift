import FMPackages
import SwiftUI

struct DjNextButton: View {
    let model: DjStudioModel
    let deck: DjDeck
    let size: CGFloat

    var body: some View {
        Button {
            DjTheme.tick()
            model.quickNext(deck.side)
        } label: {
            DjRoundFace(size: size) {
                Image(systemName: "forward.end.fill")
            }
            .frame(width: max(36, size), height: max(36, size))
            .contentShape(Rectangle())
        }
        .buttonStyle(DjPressStyle())
        .accessibilityLabel("Load the next song on Deck \(deck.side.label)")
        .accessibilityHint(deck.playing ? "The next song starts playing at once" : "Loads the next queued or AUTOMIX song")
    }
}

struct DjSimilarButton: View {
    let model: DjStudioModel
    let deck: DjDeck
    let size: CGFloat

    var body: some View {
        Button {
            DjTheme.tick()
            model.openSimilar(deck.side)
        } label: {
            DjRoundFace(size: size) {
                Image(systemName: "sparkle.magnifyingglass")
            }
            .frame(width: max(36, size), height: max(36, size))
            .contentShape(Rectangle())
        }
        .buttonStyle(DjPressStyle())
        .accessibilityLabel("Find songs like Deck \(deck.side.label)’s")
        .accessibilityHint("Opens suggested songs to load on the other deck")
    }
}

struct DjSimilarSheet: View {
    @Bindable var model: DjStudioModel
    let side: DjDeckSide

    var body: some View {
        let deck = model.deck(side)
        let seed = model.song(deck.videoId)
        let mine = model.similarSongs(side)
        let loaded = Set([model.a, model.b].map(\.videoId))
        let online = model.similarResults.filter { !loaded.contains($0.id) }
        List {
            Section {
                Picker("Load onto", selection: $model.searchTarget) {
                    ForEach(DjLoadTarget.allCases) { (target: DjLoadTarget) in
                        Text(target.label).tag(target)
                    }
                }
                .pickerStyle(.segmented)
                .listRowBackground(Color.clear)
                .listRowInsets(EdgeInsets())
                if deck.loaded {
                    DjTrackRow(track: seed, plays: model.plays(seed.id))
                }
            } footer: {
                Text("Songs like Deck \(side.label)’s. Tap one to load it on \(model.searchTarget == .queue ? "the queue" : model.searchTarget.label).")
            }
            if !mine.isEmpty {
                Section("From Your Library") {
                    ForEach(mine) { (match: DjSimilarSong) in
                        row(match.track, detail: match.reasons.prefix(3).joined(separator: " · "))
                    }
                }
            }
            Section {
                if model.similarLoading {
                    HStack(spacing: 10) {
                        ProgressView()
                        Text("Finding suggestions…").foregroundStyle(.secondary)
                    }
                }
                ForEach(online) { (result: DjSearchResult) in
                    row(DjTrack(id: result.id, title: result.title, author: result.author), detail: result.seconds > 0 ? DjStudio.clock(result.seconds) : "")
                }
                if !model.similarProblem.isEmpty {
                    Text(model.similarProblem)
                        .font(.footnote)
                        .foregroundStyle(.secondary)
                    Button("Try Again") {
                        model.similarSeed = ""
                        model.findSimilar(seed)
                    }
                }
            } header: {
                Text("Suggested on YouTube")
            }
        }
        .listStyle(.insetGrouped)
        .scrollContentBackground(.hidden)
        .navigationTitle("Find Similar")
        .onChange(of: deck.videoId) { _, id in
            if !id.isEmpty { model.findSimilar(model.song(id)) }
        }
    }

    private func row(_ track: DjTrack, detail: String) -> some View {
        Button {
            model.place(id: track.id, title: track.title, author: track.author)
            model.sheet = nil
        } label: {
            HStack(spacing: 10) {
                DjTrackRow(track: track, plays: model.plays(track.id))
                if !detail.isEmpty {
                    Text(detail)
                        .font(.caption2)
                        .foregroundStyle(.secondary)
                        .lineLimit(2)
                        .multilineTextAlignment(.trailing)
                        .frame(maxWidth: 110, alignment: .trailing)
                }
                Image(systemName: "plus.circle").foregroundStyle(.tint)
            }
        }
        .buttonStyle(.plain)
        .contextMenu {
            Button("Load on Deck A", systemImage: "a.circle") { model.loadTrack(track, on: .a) }
            Button("Load on Deck B", systemImage: "b.circle") { model.loadTrack(track, on: .b) }
            Button("Add to the Queue", systemImage: "text.badge.plus") { model.enqueue(id: track.id, title: track.title, author: track.author) }
            DjPlaylistMenu(model: model, id: track.id)
        }
    }
}

struct DjSongActions: View {
    let model: DjStudioModel
    let id: String
    var deleting: (() -> Void)?

    var body: some View {
        Toggle(isOn: Binding(get: { !model.crate.shuffles(id) }, set: { _ in model.toggleShuffle(id) })) {
            Label("Don’t Shuffle", systemImage: "shuffle")
        }
        Button("Delete from Library", systemImage: "trash", role: .destructive) {
            if let deleting { deleting() } else { model.removeSong(id) }
        }
    }
}

struct DjSmartKindView: View {
    let model: DjStudioModel
    let kind: DjSmartKind

    var body: some View {
        let lists = model.crate.smart.filter { $0.kind == kind }
        List {
            Section {
                ForEach(lists) { (list: DjSmartList) in
                    NavigationLink(value: DjCrateRoute.smart(list.id)) {
                        LabeledContent {
                            Text("\(list.songs.count)").monospacedDigit()
                        } label: {
                            Label(list.name, systemImage: model.crate.focus == .smart(list.id) ? "scope" : list.symbol)
                        }
                    }
                }
            } footer: {
                Text(lists.isEmpty ? "Nothing here yet: play or save more songs." : DjSmartKindView.footer(kind))
            }
        }
        .listStyle(.insetGrouped)
        .scrollContentBackground(.hidden)
        .navigationTitle(kind.title)
    }

    static func footer(_ kind: DjSmartKind) -> String {
        switch kind {
        case .listening: return "Sorted by how you play: the most played, the latest, songs saved but never played, and songs not heard for a month."
        case .genre: return "Sorted from each song’s title and tags, such as house, hip-hop or lo-fi. Tag a song to put it in a genre its title does not name."
        case .style: return "Remixes, DJ sets, live and acoustic versions and more, from each song’s title and tags."
        case .decade: return "From a year or a decade such as 1998 or 90s in the title or tags."
        case .artist: return "Every artist or channel with two songs or more."
        }
    }
}

struct DjSmartView: View {
    let model: DjStudioModel
    let id: String

    var body: some View {
        let list = model.crate.smartList(id)
        let songs = model.crate.songs(.smart(id))
        List {
            DjFocusActions(model: model, focus: .smart(id), count: songs.count)
            Section {
                ForEach(songs) { (track: DjTrack) in
                    DjTrackMenu(model: model, track: track)
                }
            } footer: {
                Text(list == nil ? "No songs fit this smart playlist any more." : "Smart playlists fill themselves as you play, save and tag songs. Save as Playlist keeps a copy you can reorder.")
            }
        }
        .listStyle(.insetGrouped)
        .scrollContentBackground(.hidden)
        .navigationTitle(list?.name ?? "Smart Playlist")
        .toolbar {
            ToolbarItem(placement: .topBarTrailing) {
                Menu {
                    Button("Save as Playlist", systemImage: "music.note.list") { model.saveSmart(id) }
                        .disabled(songs.isEmpty)
                    DjExportMenu(model: model, scope: .smart(id))
                } label: {
                    Image(systemName: "ellipsis.circle")
                }
                .accessibilityLabel("Smart playlist options")
            }
        }
    }
}
