import FMPackages
import SwiftUI
import UIKit

struct DjSheetView: View {
    let model: DjStudioModel
    let sheet: DjSheet

    var body: some View {
        NavigationStack {
            Group {
                switch sheet {
                case .load: DjLoadSheet(model: model)
                case let .deck(side): DjDeckSheet(model: model, deck: model.deck(side))
                case .pads: DjPadsSheet(model: model)
                case .info: DjInfoSheet(model: model)
                case .jam: DjJamSheet(model: model)
                case .crate: DjCrateSheet(model: model)
                case let .song(id): DjSongEditor(model: model, id: id)
                case let .similar(side): DjSimilarSheet(model: model, side: side)
                case .diagnostics: DjDiagnosticsSheet(model: model)
                }
            }
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button("Done") { model.sheet = nil }
                }
            }
        }
        .presentationDetents(sheet == .pads ? [.medium] : sheet == .crate ? [.large] : [.medium, .large])
        .presentationDragIndicator(.visible)
        .presentationBackground(.regularMaterial)
        .preferredColorScheme(.dark)
    }
}

struct DjLoadSheet: View {
    @Bindable var model: DjStudioModel
    @FocusState private var focused: Bool

    var body: some View {
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
                HStack(spacing: 8) {
                    Image(systemName: "magnifyingglass").foregroundStyle(.secondary)
                    TextField("Search YouTube or paste a link", text: $model.searchText)
                        .focused($focused)
                        .submitLabel(.search)
                        .autocorrectionDisabled()
                        .textInputAutocapitalization(.never)
                        .onSubmit { model.search() }
                    if model.searching {
                        ProgressView()
                    } else {
                        PasteButton(payloadType: String.self) { strings in
                            Task { @MainActor in
                                model.searchText = strings.first ?? ""
                                model.search()
                            }
                        }
                        .labelStyle(.iconOnly)
                        .buttonBorderShape(.capsule)
                    }
                }
                if !model.searchProblem.isEmpty {
                    Text(model.searchProblem)
                        .font(.footnote)
                        .foregroundStyle(.secondary)
                }
            }
            if model.searchText.isEmpty {
                recent
                mostPlayed
                results
            } else {
                results
                recent
                mostPlayed
            }
        }
        .listStyle(.insetGrouped)
        .scrollContentBackground(.hidden)
        .navigationTitle(model.searchTarget == .queue ? "Add to Queue" : "Load \(model.searchTarget.label)")
        .onAppear { focused = true }
    }

    @ViewBuilder private var results: some View {
        if !model.searchResults.isEmpty {
            Section("Search Results") {
                ForEach(model.searchResults) { (result: DjSearchResult) in
                    row(result, plays: model.plays(result.id))
                }
            }
        }
    }

    @ViewBuilder private var recent: some View {
        if !model.history.isEmpty {
            let counts = Dictionary(model.library.map { ($0.id, $0.plays) }, uniquingKeysWith: max)
            Section {
                ForEach(model.history) { (item: DjHistoryItem) in
                    row(DjSearchResult(id: item.id, title: item.title.isEmpty ? "YouTube video" : item.title, author: item.author), plays: counts[item.id] ?? 0)
                }
                .onDelete { model.forget($0) }
            } header: {
                HStack {
                    Text("Recently Played")
                    Spacer()
                    Button("Clear") { model.clearHistory() }
                        .font(.footnote)
                        .textCase(nil)
                }
            }
        }
    }

    @ViewBuilder private var mostPlayed: some View {
        let top = DjStudio.mostPlayed(model.library)
        if !top.isEmpty {
            Section {
                ForEach(top) { (song: DjPlayedSong) in
                    row(DjSearchResult(id: song.id, title: song.title.isEmpty ? "YouTube video" : song.title, author: song.author), plays: song.plays)
                        .swipeActions {
                            Button("Remove", role: .destructive) { model.forgetPlayed(song.id) }
                        }
                }
            } header: {
                Text("Most Played")
            } footer: {
                Text("With the queue empty, AUTOMIX shuffles every song you have played (\(model.library.count)).")
            }
        }
    }

    private func row(_ result: DjSearchResult, plays: Int = 0) -> some View {
        Button {
            model.place(id: result.id, title: result.title, author: result.author)
            model.sheet = nil
        } label: {
            DjResultRow(result: result, loaded: model.searchTarget.side.map { model.deck($0).videoId == result.id }
                ?? model.shownQueue.contains { $0.videoId == result.id }, plays: plays)
        }
        .buttonStyle(.plain)
        .contextMenu {
            DjPlaylistMenu(model: model, id: result.id)
            Button("Tags and Playlists…", systemImage: "tag") { model.editSong(result.id) }
            DjSongActions(model: model, id: result.id)
        }
    }
}

struct DjResultRow: View {
    let result: DjSearchResult
    var loaded = false
    var plays = 0

    var body: some View {
        HStack(spacing: 10) {
            AsyncImage(url: result.thumbnail) { image in
                image.resizable().scaledToFill()
            } placeholder: {
                Color.white.opacity(0.08)
            }
            .frame(width: 72, height: 40)
            .clipShape(RoundedRectangle(cornerRadius: 4, style: .continuous))
            VStack(alignment: .leading, spacing: 2) {
                Text(result.title)
                    .font(.subheadline.weight(.medium))
                    .lineLimit(2)
                Text([result.author, result.seconds > 0 ? DjStudio.clock(result.seconds) : "", DjStudio.playsLabel(plays)].filter { !$0.isEmpty }.joined(separator: " · "))
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .lineLimit(1)
            }
            Spacer(minLength: 0)
            Image(systemName: loaded ? "checkmark.circle.fill" : "plus.circle")
                .foregroundStyle(.tint)
        }
        .contentShape(Rectangle())
    }
}

struct DjDeckSheet: View {
    let model: DjStudioModel
    let deck: DjDeck

    var body: some View {
        Form {
            Section {
                if deck.loaded {
                    VStack(alignment: .leading, spacing: 2) {
                        Text(deck.title.isEmpty ? "Loading…" : deck.title).lineLimit(2)
                        Text([deck.author.isEmpty ? "YouTube" : deck.author, DjStudio.playsLabel(model.plays(deck.videoId))].filter { !$0.isEmpty }.joined(separator: " · ")).font(.footnote).foregroundStyle(.secondary)
                    }
                } else {
                    Text("No video loaded").foregroundStyle(.secondary)
                }
            }
            Section("Mix") {
                LabeledContent("Volume") {
                    Slider(value: Binding(get: { deck.level }, set: { model.setLevel(deck.side, $0) }), in: 0...1)
                        .frame(maxWidth: 220)
                }
                LabeledContent("Tempo") {
                    HStack {
                        Text(String(format: "%+.1f%%", (deck.rate - 1) * 100)).monospacedDigit()
                        Button("Reset") { model.setRate(deck.side, 1) }
                            .buttonStyle(.bordered)
                            .controlSize(.small)
                    }
                }
                LabeledContent("BPM") {
                    HStack {
                        Text(deck.bpm > 0 ? String(format: "%.1f", deck.shownBpm) : "Not set").monospacedDigit()
                        Button("Tap") { model.tapTempo(deck.side) }
                            .buttonStyle(.bordered)
                            .controlSize(.small)
                        if deck.bpm > 0 {
                            Button("Clear") { model.clearBpm(deck.side) }
                                .buttonStyle(.bordered)
                                .controlSize(.small)
                        }
                    }
                }
                Toggle("8-second loop", isOn: Binding(get: { deck.loop }, set: { _ in model.toggleLoop(deck.side) }))
            }
            .disabled(!deck.loaded)
            Section("Cue") {
                Button("Set cue at \(DjStudio.clock(deck.position()))") { model.setCue(deck.side) }
                Button("Jump to cue at \(DjStudio.clock(deck.cue))") { model.jumpToCue(deck.side) }
            }
            .disabled(!deck.loaded)
            Section {
                Button("Load another video") { model.openLoad(deck.side) }
                Button("Load the next song") { model.quickNext(deck.side) }
                if deck.loaded {
                    Button("Find similar songs") { model.openSimilar(deck.side) }
                    Button("Tags and Playlists…") { model.editSong(deck.videoId) }
                    Toggle("Leave out of shuffle", isOn: Binding(get: { !model.crate.shuffles(deck.videoId) }, set: { _ in model.toggleShuffle(deck.videoId) }))
                }
                if deck.loaded {
                    Button("Delete song from library", role: .destructive) {
                        model.deleteSong(on: deck.side)
                    }
                    Button("Eject Deck \(deck.side.label)", role: .destructive) {
                        model.eject(deck.side)
                        model.sheet = nil
                    }
                }
            }
        }
        .scrollContentBackground(.hidden)
        .navigationTitle("Deck \(deck.side.label)")
    }
}

struct DjPadsSheet: View {
    let model: DjStudioModel
    private let columns = Array(repeating: GridItem(.flexible(), spacing: 8), count: 4)

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 12) {
                LazyVGrid(columns: columns, spacing: 8) {
                    ForEach(DjDrumPad.allCases) { (pad: DjDrumPad) in
                        DjPad(title: pad.label, detail: "", lit: model.flashedPad == pad, enabled: true) {
                            model.playPad(pad)
                        }
                    }
                }
                Text("Samples")
                    .font(.footnote.weight(.semibold))
                    .foregroundStyle(.secondary)
                LazyVGrid(columns: columns, spacing: 8) {
                    ForEach(0..<DjStudio.sectionCount, id: \.self) { index in
                        let section = model.sections[index]
                        DjPad(
                            title: "Sample \(index + 1)",
                            detail: section.map { "\($0.deck.label) · \(DjStudio.clock($0.start))" } ?? "Empty",
                            lit: false, enabled: section != nil
                        ) {
                            model.playSection(index)
                        }
                        .contextMenu {
                            if section != nil {
                                Button("Clear Sample \(index + 1)", systemImage: "trash", role: .destructive) { model.clearSection(index) }
                            }
                        }
                    }
                }
                HStack(spacing: 10) {
                    Image(systemName: "speaker.fill").foregroundStyle(.secondary)
                    Slider(value: Binding(get: { model.samplerLevel }, set: { model.setSamplerLevel($0) }), in: 0...1)
                    Image(systemName: "speaker.wave.3.fill").foregroundStyle(.secondary)
                }
                .accessibilityElement(children: .combine)
                .accessibilityLabel("Sampler level")
            }
            .padding(14)
        }
        .navigationTitle("Sampler")
    }
}

struct DjPad: View {
    let title: String
    let detail: String
    let lit: Bool
    let enabled: Bool
    let action: () -> Void
    @GestureState private var pressed = false

    var body: some View {
        VStack(spacing: 2) {
            Text(title).font(.subheadline.weight(.semibold))
            if !detail.isEmpty {
                Text(detail).font(.caption2).foregroundStyle(.secondary)
            }
        }
        .lineLimit(1)
        .minimumScaleFactor(0.7)
        .frame(maxWidth: .infinity, minHeight: 54)
        .background(
            RoundedRectangle(cornerRadius: 8, style: .continuous)
                .fill(lit || pressed ? DjTheme.active.opacity(0.45) : Color.white.opacity(0.08)))
        .overlay(RoundedRectangle(cornerRadius: 8, style: .continuous).strokeBorder(Color.white.opacity(0.1), lineWidth: 1))
        .opacity(enabled ? 1 : 0.45)
        .contentShape(Rectangle())
        .gesture(DragGesture(minimumDistance: 0).updating($pressed) { _, state, _ in state = true })
        .onChange(of: pressed) { _, now in if now, enabled { action() } }
        .accessibilityElement(children: .combine)
        .accessibilityAddTraits(.isButton)
        .accessibilityAction { if enabled { action() } }
    }
}

struct DjInfoSheet: View {
    let model: DjStudioModel

    var body: some View {
        List {
            ForEach([model.a, model.b], id: \.side) { (deck: DjDeck) in
                Section("Deck \(deck.side.label)") {
                    if deck.loaded {
                        VStack(alignment: .leading, spacing: 2) {
                            Text(deck.title.isEmpty ? "Loading…" : deck.title)
                            Text([deck.author.isEmpty ? "YouTube" : deck.author, DjStudio.playsLabel(model.plays(deck.videoId))].filter { !$0.isEmpty }.joined(separator: " · ")).font(.footnote).foregroundStyle(.secondary)
                        }
                        Link(destination: DjStudio.watchURL(deck.videoId)) {
                            Label("Open on YouTube", systemImage: "arrow.up.right.square")
                        }
                    } else {
                        Button("Load a video") { model.openLoad(deck.side) }
                    }
                }
            }
            Section("How it works") {
                Label("Search and press return: the top result loads straight onto the deck.", systemImage: "magnifyingglass")
                Label("Songs you play are kept under Recently Played and Most Played in the loader with how often each was played. With the queue empty, AUTOMIX shuffles them.", systemImage: "clock.arrow.circlepath")
                Label("Touch a record to slow it a little; drag it to scratch.", systemImage: "hand.draw")
                Label("START plays, PAUSE holds, STOP goes back to the beginning, RESTART starts over.", systemImage: "playpause")
                Label("SAMPLE starts recording, tap again to stop and arm it, and once more to play the sample.", systemImage: "record.circle")
                Label("The middle shows the queue: + adds a video, tap a thumbnail to load it on a deck, or hold it and drag it onto a deck.", systemImage: "rectangle.stack")
                Label("Each deck’s volume fader sits beside its tempo strip; double-tap it for full volume.", systemImage: "speaker.wave.2")
                Label("Jam (the people button) invites other ForkMesh users to share the decks and the queue. Everyone plays their own synchronised copy, and their icons show who is listening and shake when they dance.", systemImage: "person.2")
                Label("The smiley button opens 30 silent reactions that play over the studio for everyone in the jam.", systemImage: "face.smiling")
                Label("Push the tempo fader up to speed a deck up; double-tap it to reset.", systemImage: "slider.vertical.3")
                Label("Tap the BPM box on the beat, then press SYNC.", systemImage: "metronome")
                Label("Hold − or + to nudge a deck’s tempo.", systemImage: "plusminus")
                Label("AUTOMIX on plays the queue, then songs you have played (or the playlist or tag chosen in Playlists), one after another with an 8-second crossfade; hold it to mix to the other deck now.", systemImage: "arrow.left.arrow.right")
                Label("Shake the phone to mix into the next song.", systemImage: "iphone.radiowaves.left.and.right")
                Label("Playlists (the list button) keeps playlists and tagged songs, exports and imports them, and chooses what AUTOMIX plays: everything, one playlist or one tag. Hold a song in the loader to tag it or add it to a playlist.", systemImage: "music.note.list")
                Label("Back closes the studio; playing decks keep going and show in the top bar.", systemImage: "chevron.left")
                Label("Reset All (the circular arrow at the bottom) unloads both decks and puts every setting back to the start.", systemImage: "arrow.counterclockwise")
            }
            .font(.footnote)
            Section {
                Button("Reset All", role: .destructive) {
                    model.resettingAfterSheet = true
                    model.sheet = nil
                }
                Button("Leave Studio", role: .destructive) {
                    model.leavingAfterSheet = true
                    model.sheet = nil
                }
            }
        }
        .scrollContentBackground(.hidden)
        .navigationTitle("Video Mix DJ Studio")
    }
}
