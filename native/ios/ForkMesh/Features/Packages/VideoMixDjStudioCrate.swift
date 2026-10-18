import FMPackages
import SwiftUI
import UniformTypeIdentifiers

enum DjCrateRoute: Hashable {
    case playlist(String)
    case tag(String)
    case songs
    case smartKind(DjSmartKind)
    case smart(String)
}

struct DjCrateExport: Transferable, Sendable {
    let crate: DjCrate
    let scope: DjFocus

    static var transferRepresentation: some TransferRepresentation {
        FileRepresentation(exportedContentType: .json) { item in
            let url = FileManager.default.temporaryDirectory.appendingPathComponent(item.crate.fileName(item.scope)).appendingPathExtension("json")
            try item.crate.exported(item.scope).write(to: url, options: .atomic)
            return SentTransferredFile(url)
        }
    }
}

struct DjCrateLinks: Transferable, Sendable {
    let crate: DjCrate
    let scope: DjFocus

    static var transferRepresentation: some TransferRepresentation {
        FileRepresentation(exportedContentType: .m3uPlaylist) { item in
            let url = FileManager.default.temporaryDirectory.appendingPathComponent(item.crate.fileName(item.scope)).appendingPathExtension("m3u")
            try Data(item.crate.m3u(item.scope).utf8).write(to: url, options: .atomic)
            return SentTransferredFile(url)
        }
    }
}

struct DjCrateSheet: View {
    let model: DjStudioModel
    @State private var naming = false
    @State private var newName = ""
    @State private var importing = false

    var body: some View {
        let crate = model.crate
        List {
            DjFocusPicker(model: model)
            Section {
                ForEach(crate.playlists) { (list: DjPlaylist) in
                    NavigationLink(value: DjCrateRoute.playlist(list.id)) {
                        LabeledContent {
                            Text("\(list.songs.count)").monospacedDigit()
                        } label: {
                            Label(list.name, systemImage: crate.focus == .playlist(list.id) ? "scope" : "music.note.list")
                        }
                    }
                    .swipeActions {
                        Button("Delete", role: .destructive) { model.deletePlaylist(list.id) }
                    }
                }
                Button {
                    newName = ""
                    naming = true
                } label: {
                    Label("New Playlist", systemImage: "plus")
                }
            } header: {
                Text("Playlists")
            }
            Section {
                ForEach(DjSmartKind.allCases, id: \.self) { (kind: DjSmartKind) in
                    let lists = crate.smart.filter { $0.kind == kind }
                    if !lists.isEmpty {
                        NavigationLink(value: DjCrateRoute.smartKind(kind)) {
                            LabeledContent {
                                Text("\(lists.count)").monospacedDigit()
                            } label: {
                                Label(kind.title, systemImage: lists.contains { crate.focus == .smart($0.id) } ? "scope" : DjCrateSheet.symbol(kind))
                            }
                        }
                    }
                }
            } header: {
                Text("Smart Playlists")
            } footer: {
                Text(crate.smart.isEmpty
                    ? "As you play and save songs they are sorted into smart playlists by genre, style, decade, artist and how often you play them."
                    : "Every song is sorted into these automatically from its title, channel, tags and plays. They update as you play.")
            }
            Section {
                ForEach(crate.tagCounts) { (tag: DjTagCount) in
                    NavigationLink(value: DjCrateRoute.tag(tag.name)) {
                        LabeledContent {
                            Text("\(tag.count)").monospacedDigit()
                        } label: {
                            Label("#\(tag.name)", systemImage: crate.focus == .tag(tag.name) ? "scope" : "tag")
                        }
                    }
                    .swipeActions {
                        Button("Delete", role: .destructive) { model.deleteTag(tag.name) }
                    }
                }
                NavigationLink(value: DjCrateRoute.songs) {
                    LabeledContent {
                        Text("\(model.librarySongs.count)").monospacedDigit()
                    } label: {
                        Label("All Songs", systemImage: "music.note")
                    }
                }
            } header: {
                Text("Tags")
            } footer: {
                Text("Tag a song from All Songs, a playlist, or by holding it in the loader. Tags group songs across playlists, such as #warm up or #chill.")
            }
            Section {
                ShareLink(item: DjCrateExport(crate: crate, scope: .everything), preview: SharePreview("DJ Playlists")) {
                    Label("Export Everything", systemImage: "square.and.arrow.up")
                }
                .disabled(crate.tracks.isEmpty)
                Button {
                    importing = true
                } label: {
                    Label("Import from Files", systemImage: "square.and.arrow.down")
                }
                PasteButton(payloadType: String.self) { strings in
                    Task { @MainActor in
                        model.importData(Data(strings.joined(separator: "\n").utf8), name: "Pasted Songs")
                    }
                }
                .buttonBorderShape(.capsule)
            } header: {
                Text("Import and Export")
            } footer: {
                Text("Export saves your songs, tags and playlists as a file to keep or send to another phone. Import reads that file, M3U playlists and lists of YouTube links; songs already here keep their tags and playlists with the same name are combined.")
            }
        }
        .listStyle(.insetGrouped)
        .scrollContentBackground(.hidden)
        .navigationTitle("Playlists")
        .navigationDestination(for: DjCrateRoute.self) { (route: DjCrateRoute) in
            switch route {
            case let .playlist(id): DjPlaylistView(model: model, id: id)
            case let .tag(name): DjTagView(model: model, name: name)
            case .songs: DjAllSongsView(model: model)
            case let .smartKind(kind): DjSmartKindView(model: model, kind: kind)
            case let .smart(id): DjSmartView(model: model, id: id)
            }
        }
        .alert("New Playlist", isPresented: $naming) {
            TextField("Name", text: $newName)
            Button("Create") { model.newPlaylist(newName) }
            Button("Cancel", role: .cancel) {}
        }
        .fileImporter(isPresented: $importing, allowedContentTypes: [.json, .m3uPlaylist, .plainText, .text]) { (result: Result<URL, Error>) in
            guard case let .success(url) = result else { return }
            let scoped = url.startAccessingSecurityScopedResource()
            let data = try? Data(contentsOf: url)
            if scoped { url.stopAccessingSecurityScopedResource() }
            guard let data else {
                model.show("That file could not be read.")
                return
            }
            model.importData(data, name: url.deletingPathExtension().lastPathComponent)
        }
    }

    static func symbol(_ kind: DjSmartKind) -> String {
        switch kind {
        case .listening: return "chart.bar"
        case .genre: return "music.quarternote.3"
        case .style: return "slider.horizontal.3"
        case .decade: return "calendar"
        case .artist: return "person.2"
        }
    }
}

struct DjFocusPicker: View {
    let model: DjStudioModel

    var body: some View {
        let crate = model.crate
        Section {
            Picker(selection: Binding(get: { model.crate.focus }, set: { model.focusAutomix($0, start: false) })) {
                Text("Everything").tag(DjFocus.everything)
                ForEach(crate.playlists) { (list: DjPlaylist) in
                    Text(list.name).tag(DjFocus.playlist(list.id))
                }
                ForEach(crate.tagCounts) { (tag: DjTagCount) in
                    Text("#\(tag.name)").tag(DjFocus.tag(tag.name))
                }
                ForEach(DjSmartKind.allCases, id: \.self) { (kind: DjSmartKind) in
                    let lists = crate.smart.filter { $0.kind == kind }
                    if !lists.isEmpty {
                        Section(kind.title) {
                            ForEach(lists) { (list: DjSmartList) in
                                Text(list.name).tag(DjFocus.smart(list.id))
                            }
                        }
                    }
                }
            } label: {
                Label("Plays", systemImage: "scope")
            }
            .pickerStyle(.menu)
            Toggle(isOn: Binding(get: { model.crate.shuffle }, set: { model.setShuffle($0) })) {
                Label("Shuffle", systemImage: "shuffle")
            }
            .disabled(crate.focus == .everything)
        } header: {
            Text("AUTOMIX")
        } footer: {
            Text(crate.focus == .everything
                ? "With the queue empty, AUTOMIX shuffles every song you have played or saved, except those marked Don’t Shuffle. Choose a playlist, tag or smart playlist to keep it to those songs."
                : "With the queue empty, AUTOMIX plays only \(crate.label(crate.focus)), \(crate.shuffle ? "shuffled" : "in order"), and starts over at the end.")
        }
    }
}

struct DjFocusActions: View {
    let model: DjStudioModel
    let focus: DjFocus
    let count: Int

    var body: some View {
        let playing = model.automixOn && model.crate.focus == focus
        Section {
            Button {
                model.focusAutomix(focus, start: true)
            } label: {
                Label(playing ? "AUTOMIX Is Playing These" : "Play with AUTOMIX", systemImage: playing ? "scope" : "play.circle")
            }
            .disabled(count == 0 || playing)
            Button {
                model.queueAll(focus, shuffled: false)
            } label: {
                Label("Add All to the Queue", systemImage: "text.badge.plus")
            }
            .disabled(count == 0)
            Button {
                model.queueAll(focus, shuffled: true)
            } label: {
                Label("Shuffle into the Queue", systemImage: "shuffle")
            }
            .disabled(count < 2)
        }
    }
}

struct DjExportMenu: View {
    let model: DjStudioModel
    let scope: DjFocus

    var body: some View {
        ShareLink(item: DjCrateExport(crate: model.crate, scope: scope), preview: SharePreview(model.crate.label(scope))) {
            Label("Export", systemImage: "square.and.arrow.up")
        }
        ShareLink(item: DjCrateLinks(crate: model.crate, scope: scope), preview: SharePreview(model.crate.label(scope))) {
            Label("Export as M3U", systemImage: "link")
        }
    }
}

struct DjPlaylistView: View {
    let model: DjStudioModel
    let id: String
    @Environment(\.dismiss) private var dismiss
    @State private var renaming = false
    @State private var name = ""
    @State private var adding = false

    var body: some View {
        let songs = model.crate.songs(.playlist(id))
        List {
            DjFocusActions(model: model, focus: .playlist(id), count: songs.count)
            Section {
                ForEach(songs) { (track: DjTrack) in
                    DjTrackMenu(model: model, track: track)
                }
                .onDelete { model.removeSongs($0, from: id) }
                .onMove { model.moveSongs(in: id, from: $0, to: $1) }
                Button {
                    adding = true
                } label: {
                    Label("Add Songs", systemImage: "plus")
                }
            } footer: {
                Text(songs.isEmpty ? "Add songs you have played, paste YouTube links, or hold a search result in the loader." : "Edit reorders and removes songs; AUTOMIX plays them in this order unless Shuffle is on.")
            }
        }
        .listStyle(.insetGrouped)
        .scrollContentBackground(.hidden)
        .navigationTitle(model.crate.playlist(id)?.name ?? "Playlist")
        .toolbar {
            ToolbarItemGroup(placement: .topBarTrailing) {
                Menu {
                    Button("Rename", systemImage: "pencil") {
                        name = model.crate.playlist(id)?.name ?? ""
                        renaming = true
                    }
                    DjExportMenu(model: model, scope: .playlist(id))
                    Button("Delete Playlist", systemImage: "trash", role: .destructive) {
                        model.deletePlaylist(id)
                        dismiss()
                    }
                } label: {
                    Image(systemName: "ellipsis.circle")
                }
                .accessibilityLabel("Playlist options")
                EditButton()
            }
        }
        .alert("Rename Playlist", isPresented: $renaming) {
            TextField("Name", text: $name)
            Button("Rename") { model.renamePlaylist(id, name) }
            Button("Cancel", role: .cancel) {}
        }
        .sheet(isPresented: $adding) {
            NavigationStack {
                DjSongPicker(model: model, playlist: id)
            }
            .presentationDetents([.large])
            .presentationBackground(.regularMaterial)
            .preferredColorScheme(.dark)
        }
    }
}

struct DjTagView: View {
    let model: DjStudioModel
    let name: String
    @Environment(\.dismiss) private var dismiss
    @State private var renaming = false
    @State private var newName = ""

    var body: some View {
        let songs = model.crate.songs(.tag(name))
        List {
            DjFocusActions(model: model, focus: .tag(name), count: songs.count)
            Section {
                ForEach(songs) { (track: DjTrack) in
                    DjTrackMenu(model: model, track: track)
                        .swipeActions {
                            Button("Untag") { model.toggleTag(name, on: track.id) }
                                .tint(.orange)
                        }
                }
            } footer: {
                Text("Swipe a song to take the tag off it.")
            }
        }
        .listStyle(.insetGrouped)
        .scrollContentBackground(.hidden)
        .navigationTitle("#\(name)")
        .toolbar {
            ToolbarItem(placement: .topBarTrailing) {
                Menu {
                    Button("Rename", systemImage: "pencil") {
                        newName = name
                        renaming = true
                    }
                    DjExportMenu(model: model, scope: .tag(name))
                    Button("Delete Tag", systemImage: "trash", role: .destructive) {
                        model.deleteTag(name)
                        dismiss()
                    }
                } label: {
                    Image(systemName: "ellipsis.circle")
                }
                .accessibilityLabel("Tag options")
            }
        }
        .alert("Rename Tag", isPresented: $renaming) {
            TextField("Tag", text: $newName)
                .textInputAutocapitalization(.never)
            Button("Rename") {
                model.renameTag(name, to: newName)
                dismiss()
            }
            Button("Cancel", role: .cancel) {}
        } message: {
            Text("Renaming to a tag that exists merges the two.")
        }
    }
}

struct DjAllSongsView: View {
    let model: DjStudioModel
    @State private var filter = ""

    var body: some View {
        let songs = DjSongFilter.apply(model.librarySongs, filter)
        List {
            Section {
                ForEach(songs) { (track: DjTrack) in
                    DjTrackMenu(model: model, track: track)
                        .swipeActions {
                            Button("Remove", role: .destructive) { model.removeSong(track.id) }
                        }
                }
            } footer: {
                Text("Songs you have played or saved. Removing one also takes it out of every playlist, its tags and its play count.")
            }
        }
        .listStyle(.insetGrouped)
        .scrollContentBackground(.hidden)
        .searchable(text: $filter, placement: .navigationBarDrawer(displayMode: .always), prompt: "Filter by title, channel or #tag")
        .navigationTitle("All Songs")
    }
}

enum DjSongFilter {
    static func apply(_ songs: [DjTrack], _ text: String) -> [DjTrack] {
        let words = text.lowercased().split(whereSeparator: \.isWhitespace).map(String.init)
        guard !words.isEmpty else { return songs }
        return songs.filter { song in
            let haystack = ([song.title, song.author] + song.tags.map { "#\($0)" }).joined(separator: " ").lowercased()
            return words.allSatisfy { haystack.contains($0) }
        }
    }
}

struct DjSongPicker: View {
    let model: DjStudioModel
    let playlist: String
    @Environment(\.dismiss) private var dismiss
    @State private var filter = ""

    var body: some View {
        let songs = DjSongFilter.apply(model.librarySongs, filter)
        let link = DjStudio.videoId(filter)
        List {
            if !link.isEmpty {
                Section {
                    Button {
                        if model.addLink(filter, to: playlist) { filter = "" }
                    } label: {
                        Label(model.inPlaylist(link, playlist) ? "Already in this playlist" : "Add this YouTube link", systemImage: "link.badge.plus")
                    }
                    .disabled(model.inPlaylist(link, playlist))
                }
            }
            Section {
                ForEach(songs) { (track: DjTrack) in
                    Button {
                        model.toggle(track.id, in: playlist)
                    } label: {
                        DjTrackRow(track: track, plays: model.plays(track.id), icon: model.inPlaylist(track.id, playlist) ? "checkmark.circle.fill" : "plus.circle")
                    }
                    .buttonStyle(.plain)
                }
            } footer: {
                Text(model.librarySongs.isEmpty ? "Songs you play show here. You can also paste a YouTube link above." : "Tap songs to add or remove them. Paste a YouTube link above to add a new one.")
            }
        }
        .listStyle(.insetGrouped)
        .scrollContentBackground(.hidden)
        .searchable(text: $filter, placement: .navigationBarDrawer(displayMode: .always), prompt: "Filter or paste a YouTube link")
        .autocorrectionDisabled()
        .textInputAutocapitalization(.never)
        .navigationTitle("Add Songs")
        .navigationBarTitleDisplayMode(.inline)
        .toolbar {
            ToolbarItem(placement: .confirmationAction) {
                Button("Done") { dismiss() }
            }
        }
    }
}

struct DjTrackRow: View {
    let track: DjTrack
    var plays = 0
    var icon = ""

    var body: some View {
        HStack(spacing: 10) {
            AsyncImage(url: track.thumbnail) { image in
                image.resizable().scaledToFill()
            } placeholder: {
                Color.white.opacity(0.08)
            }
            .frame(width: 72, height: 40)
            .clipShape(RoundedRectangle(cornerRadius: 4, style: .continuous))
            VStack(alignment: .leading, spacing: 2) {
                Text(track.shownTitle)
                    .font(.subheadline.weight(.medium))
                    .lineLimit(2)
                Text([track.author, DjStudio.playsLabel(plays), track.noShuffle ? "Not shuffled" : ""].filter { !$0.isEmpty }.joined(separator: " · "))
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .lineLimit(1)
                if !track.tags.isEmpty {
                    Text(track.tags.map { "#\($0)" }.joined(separator: " "))
                        .font(.caption2)
                        .foregroundStyle(.tint)
                        .lineLimit(1)
                }
            }
            Spacer(minLength: 0)
            if !icon.isEmpty {
                Image(systemName: icon).foregroundStyle(.tint)
            }
        }
        .contentShape(Rectangle())
        .accessibilityElement(children: .combine)
    }
}

struct DjTrackMenu: View {
    let model: DjStudioModel
    let track: DjTrack
    @State private var editing = false

    var body: some View {
        Menu {
            Button("Load on Deck A", systemImage: "a.circle") { model.loadTrack(track, on: .a) }
            Button("Load on Deck B", systemImage: "b.circle") { model.loadTrack(track, on: .b) }
            Button("Add to the Queue", systemImage: "text.badge.plus") { model.enqueue(id: track.id, title: track.title, author: track.author) }
            DjPlaylistMenu(model: model, id: track.id)
            Button("Tags and Playlists…", systemImage: "tag") { editing = true }
            DjSongActions(model: model, id: track.id)
        } label: {
            DjTrackRow(track: track, plays: model.plays(track.id), icon: "ellipsis")
        }
        .buttonStyle(.plain)
        .sheet(isPresented: $editing) {
            NavigationStack {
                DjSongEditor(model: model, id: track.id)
                    .toolbar {
                        ToolbarItem(placement: .confirmationAction) {
                            Button("Done") { editing = false }
                        }
                    }
            }
            .presentationDetents([.medium, .large])
            .presentationBackground(.regularMaterial)
            .preferredColorScheme(.dark)
        }
    }
}

struct DjPlaylistMenu: View {
    let model: DjStudioModel
    let id: String

    var body: some View {
        Menu {
            ForEach(model.crate.playlists) { (list: DjPlaylist) in
                Button {
                    model.toggle(id, in: list.id)
                } label: {
                    if model.inPlaylist(id, list.id) {
                        Label(list.name, systemImage: "checkmark")
                    } else {
                        Text(list.name)
                    }
                }
            }
            Button("New Playlist with This Song", systemImage: "plus") {
                model.newPlaylist("New Playlist", with: [id])
            }
        } label: {
            Label("Add to Playlist", systemImage: "music.note.list")
        }
    }
}

struct DjSongEditor: View {
    let model: DjStudioModel
    let id: String
    @State private var tagText = ""
    @State private var naming = false
    @State private var newName = ""
    @FocusState private var typing: Bool

    var body: some View {
        let track = model.song(id)
        let known = model.crate.tagCounts.map(\.name)
        Form {
            Section {
                DjTrackRow(track: track, plays: model.plays(id))
            }
            Section {
                HStack {
                    TextField("Add tags, separated by commas", text: $tagText)
                        .focused($typing)
                        .submitLabel(.done)
                        .autocorrectionDisabled()
                        .textInputAutocapitalization(.never)
                        .onSubmit(addTags)
                    Button("Add", action: addTags)
                        .disabled(DjCrate.tagNames(tagText).isEmpty)
                }
                ForEach(Array(Set(known + track.tags)).sorted(), id: \.self) { (name: String) in
                    Button {
                        model.toggleTag(name, on: id)
                    } label: {
                        HStack {
                            Text("#\(name)").foregroundStyle(.primary)
                            Spacer()
                            if track.tags.contains(name) {
                                Image(systemName: "checkmark").foregroundStyle(.tint)
                            }
                        }
                        .contentShape(Rectangle())
                    }
                    .accessibilityAddTraits(track.tags.contains(name) ? .isSelected : [])
                }
            } header: {
                Text("Tags")
            }
            Section {
                ForEach(model.crate.playlists) { (list: DjPlaylist) in
                    Button {
                        model.toggle(id, in: list.id)
                    } label: {
                        HStack {
                            Text(list.name).foregroundStyle(.primary)
                            Spacer()
                            if model.inPlaylist(id, list.id) {
                                Image(systemName: "checkmark").foregroundStyle(.tint)
                            }
                        }
                        .contentShape(Rectangle())
                    }
                    .accessibilityAddTraits(model.inPlaylist(id, list.id) ? .isSelected : [])
                }
                Button {
                    newName = ""
                    naming = true
                } label: {
                    Label("New Playlist", systemImage: "plus")
                }
            } header: {
                Text("Playlists")
            }
            Section {
                Toggle("Leave Out of Shuffle", isOn: Binding(get: { !model.crate.shuffles(id) }, set: { _ in model.toggleShuffle(id) }))
            } footer: {
                Text("AUTOMIX and Next never pick a song left out of shuffle; it still plays in order in a playlist and from the queue.")
            }
            Section {
                Button("Load on Deck A") { model.loadTrack(track, on: .a) }
                Button("Load on Deck B") { model.loadTrack(track, on: .b) }
                Button("Add to the Queue") { model.enqueue(id: track.id, title: track.title, author: track.author) }
            }
        }
        .scrollContentBackground(.hidden)
        .navigationTitle("Tags and Playlists")
        .navigationBarTitleDisplayMode(.inline)
        .alert("New Playlist", isPresented: $naming) {
            TextField("Name", text: $newName)
            Button("Create") { model.newPlaylist(newName, with: [id]) }
            Button("Cancel", role: .cancel) {}
        }
    }

    private func addTags() {
        model.addTags(tagText, to: id)
        tagText = ""
    }
}

struct DjCrateButton: View {
    let model: DjStudioModel
    let size: CGFloat

    var body: some View {
        DjRoundButton(size: size, action: { model.sheet = .crate }) {
            Image(systemName: "music.note.list")
        }
        .accessibilityLabel("Playlists and tags")
    }
}

enum DjNewList {
    case playlist
    case tag
}

struct DjFileButton: View {
    let model: DjStudioModel
    let deck: DjDeck
    let size: CGFloat
    @State private var naming: DjNewList?
    @State private var name = ""

    var body: some View {
        let crate = model.crate
        let id = deck.videoId
        let focus = crate.focus
        let inFocus = focus.isFiled && crate.songs(focus).contains { $0.id == id }
        let filed = crate.track(id).map { !$0.tags.isEmpty } == true || crate.playlists.contains { $0.songs.contains(id) }
        Menu {
            Section(inFocus ? "Move from \(crate.label(focus)) to" : "Add to") {
                ForEach(crate.playlists) { (list: DjPlaylist) in
                    Button {
                        model.move(deck.side, to: .playlist(list.id))
                    } label: {
                        Label(list.name, systemImage: list.songs.contains(id) ? "checkmark" : "music.note.list")
                    }
                }
                ForEach(crate.tagCounts) { (tag: DjTagCount) in
                    Button {
                        model.move(deck.side, to: .tag(tag.name))
                    } label: {
                        Label("#\(tag.name)", systemImage: crate.track(id)?.tags.contains(tag.name) == true ? "checkmark" : "tag")
                    }
                }
                Button("New Playlist…", systemImage: "plus") {
                    name = ""
                    naming = .playlist
                }
                Button("New Tag…", systemImage: "number") {
                    name = ""
                    naming = .tag
                }
            }
            if inFocus {
                Button("Take Out of \(crate.label(focus))", systemImage: "minus.circle", role: .destructive) {
                    model.move(deck.side, to: .everything)
                }
            }
            Button("Tags and Playlists…", systemImage: "slider.horizontal.3") { model.editSong(id) }
            Button("Find Similar…", systemImage: "sparkle.magnifyingglass") { model.openSimilar(deck.side) }
            Section {
                DjSongActions(model: model, id: id) { model.deleteSong(on: deck.side) }
            }
        } label: {
            DjRoundFace(size: size, lit: filed) {
                Image(systemName: "text.badge.plus")
            }
            .frame(width: max(44, size), height: max(44, size))
            .contentShape(Rectangle())
        }
        .accessibilityLabel("File Deck \(deck.side.label) song in a playlist or tag")
        .accessibilityValue(filed ? "Filed" : "Not filed")
        .alert(naming == .tag ? "New Tag" : "New Playlist", isPresented: Binding(get: { naming != nil }, set: { if !$0 { naming = nil } })) {
            TextField(naming == .tag ? "Tag" : "Name", text: $name)
                .autocorrectionDisabled()
            Button(inFocus ? "Move Here" : "Add") { create() }
            Button("Cancel", role: .cancel) {}
        } message: {
            Text(inFocus ? "The song leaves \(crate.label(focus)) and AUTOMIX skips to the next one." : "The song is added and keeps playing.")
        }
    }

    private func create() {
        let kind = naming
        naming = nil
        switch kind {
        case .playlist:
            if let list = model.newPlaylist(name) { model.move(deck.side, to: .playlist(list)) }
        case .tag:
            let tag = DjCrate.tagName(name)
            if !tag.isEmpty { model.move(deck.side, to: .tag(tag)) }
        case nil:
            break
        }
    }
}
