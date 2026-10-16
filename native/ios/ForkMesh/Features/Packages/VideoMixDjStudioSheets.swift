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
                }
            }
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button("Done") { model.sheet = nil }
                }
            }
        }
        .presentationDetents(sheet == .pads ? [.medium] : [.medium, .large])
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
                Picker("Deck", selection: $model.searchTarget) {
                    Text("Deck A").tag(DjDeckSide.a)
                    Text("Deck B").tag(DjDeckSide.b)
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
            Section {
                ForEach(model.searchResults) { (result: DjSearchResult) in
                    Button {
                        model.load(model.searchTarget, id: result.id, title: result.title, author: result.author)
                        model.sheet = nil
                    } label: {
                        DjResultRow(result: result)
                    }
                    .buttonStyle(.plain)
                }
            }
        }
        .listStyle(.insetGrouped)
        .scrollContentBackground(.hidden)
        .navigationTitle("Load Deck \(model.searchTarget.label)")
        .onAppear { focused = model.searchResults.isEmpty }
    }
}

struct DjResultRow: View {
    let result: DjSearchResult

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
                Text([result.author, result.seconds > 0 ? DjStudio.clock(result.seconds) : ""].filter { !$0.isEmpty }.joined(separator: " · "))
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .lineLimit(1)
            }
            Spacer(minLength: 0)
            Image(systemName: "plus.circle")
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
                        Text(deck.author.isEmpty ? "YouTube" : deck.author).font(.footnote).foregroundStyle(.secondary)
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
                if deck.loaded {
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
                Text("Captured sections")
                    .font(.footnote.weight(.semibold))
                    .foregroundStyle(.secondary)
                LazyVGrid(columns: columns, spacing: 8) {
                    ForEach(0..<DjStudio.sectionCount, id: \.self) { index in
                        let section = model.sections[index]
                        DjPad(
                            title: "Section \(index + 1)",
                            detail: section.map { "\($0.deck.label) · \(DjStudio.clock($0.start))" } ?? "Empty",
                            lit: false, enabled: section != nil
                        ) {
                            model.playSection(index)
                        }
                        .contextMenu {
                            if section != nil {
                                Button("Clear Section \(index + 1)", systemImage: "trash", role: .destructive) { model.clearSection(index) }
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
                            Text(deck.author.isEmpty ? "YouTube" : deck.author).font(.footnote).foregroundStyle(.secondary)
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
                Label("Drag a record to search through its track.", systemImage: "hand.draw")
                Label("Tap the BPM box on the beat, then press SYNC.", systemImage: "metronome")
                Label("Hold − or + to nudge a deck’s tempo.", systemImage: "plusminus")
                Label("AUTOMIX fades to the other deck over 8 seconds.", systemImage: "arrow.left.arrow.right")
                Label("The record button captures 8 seconds for the sampler.", systemImage: "record.circle")
            }
            .font(.footnote)
            Section {
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
