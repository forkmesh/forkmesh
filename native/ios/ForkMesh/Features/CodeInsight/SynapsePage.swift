import FMCodeInsight
import SwiftUI

struct SynapsePage: View {
    let context: CodeInsightContext
    @Environment(\.fmPalette) private var fm
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @State private var model = SynapseModel()
    @State private var detent: PresentationDetent = .medium
    @State private var pendingOpen: String?
    @State private var configured = false

    private var snapshotKey: String {
        SynapseModel.key(context.snapshot) + "|\(context.snapshot.error)"
    }

    var body: some View {
        VStack(spacing: 0) {
            if let message = model.unusable {
                FmEmptyState(icon: "hub_outlined", title: "Nothing to wire", message: message)
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
            } else {
                controls
                brain
            }
        }
        .background(fm.canvas)
        .searchable(text: $model.query, placement: .navigationBarDrawer(displayMode: .automatic), prompt: "Find a file")
        .searchSuggestions {
            ForEach(model.suggestions, id: \.self) { (file: Int) in
                Button {
                    model.choose(file)
                } label: {
                    VStack(alignment: .leading, spacing: 2) {
                        Text(Synapse.fileName(model.snapshot.files[file].path)).fmText(.bodyMedium)
                        Text(suggestionDetail(file)).fmText(.bodySmall, color: fm.textSecondary).lineLimit(1)
                    }
                }
                .accessibilityLabel("\(model.snapshot.files[file].path), \(suggestionDetail(file))")
            }
        }
        .onSubmit(of: .search) { model.submitSearch() }
        .sheet(isPresented: $model.sheetShown, onDismiss: openPending) {
            SynapseSheet(model: model, open: { (path: String) in
                pendingOpen = path
                model.sheetShown = false
            })
            .presentationDetents([.fraction(0.3), .medium, .large], selection: $detent)
            .presentationBackgroundInteraction(.enabled(upThrough: .medium))
            .presentationDragIndicator(.visible)
        }
        .task(id: snapshotKey) {
            model.load(context.snapshot)
            context.setSummary(model.summary)
        }
        .onAppear {
            if !configured {
                configured = true
                model.autoRotate = !reduceMotion
            }
            model.resume()
        }
        .onDisappear {
            model.cancel()
            model.sheetShown = false
        }
    }

    private func suggestionDetail(_ file: Int) -> String {
        let info = model.snapshot.files[file]
        return "\(info.path) · imports \(info.dependsOn.count) · imported by \(info.dependedOnBy.count)"
    }

    private func openPending() {
        guard let path = pendingOpen else { return }
        pendingOpen = nil
        context.openFile(path, 1)
    }

    private var controls: some View {
        VStack(alignment: .leading, spacing: FmSpace.x2) {
            FmEdgeFadeScroll {
                HStack(spacing: FmSpace.x2) {
                    Text("Colour").fmText(.labelSmall, color: fm.textTertiary)
                    ForEach(SynapseColourBy.allCases, id: \.self) { (mode: SynapseColourBy) in
                        chip(mode.label, active: model.colourBy == mode) { model.colourBy = mode }
                            .accessibilityLabel("Colour by \(mode.label)")
                    }
                }
                .padding(.horizontal, FmSpace.x3)
            }
            FmEdgeFadeScroll {
                HStack(spacing: FmSpace.x2) {
                    Text("Show").fmText(.labelSmall, color: fm.textTertiary)
                    ForEach(SynapseShowMode.allCases, id: \.self) { (mode: SynapseShowMode) in
                        chip(mode.label, active: model.showMode == mode && (mode != .cycles || model.isolatedCycle < 0)) {
                            model.setShowMode(mode)
                        }
                        .accessibilityLabel("Show \(mode.label)")
                    }
                    depthMenu
                }
                .padding(.horizontal, FmSpace.x3)
            }
        }
        .padding(.vertical, FmSpace.x2)
    }

    private var depthMenu: some View {
        Menu {
            Picker("Neighbourhood depth", selection: $model.depth) {
                Text("1 hop").tag(1)
                Text("2 hops").tag(2)
                Text("3 hops").tag(3)
            }
        } label: {
            HStack(spacing: FmSpace.x1) {
                Text("Depth \(model.depth)").fmText(.labelMedium, color: fm.textSecondary)
                FmIcon("expand_more", size: 16, color: fm.textSecondary)
            }
            .padding(.horizontal, FmSpace.x3)
            .frame(height: 32)
            .background(fm.bgRaised)
            .clipShape(Capsule())
        }
        .accessibilityLabel("Neighbourhood depth, \(model.depth) \(model.depth == 1 ? "hop" : "hops")")
    }

    private func chip(_ label: String, active: Bool, action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Text(label)
                .fmText(.labelMedium, color: active ? fm.accent : fm.textSecondary)
                .padding(.horizontal, FmSpace.x3)
                .frame(height: 32)
                .background(active ? fm.accentSubtle : fm.bgRaised)
                .clipShape(Capsule())
        }
        .buttonStyle(.plain)
        .accessibilityAddTraits(active ? .isSelected : [])
    }

    private var brain: some View {
        ZStack {
            SynapseBrainView(model: model)
            VStack(alignment: .leading, spacing: 0) {
                if model.running {
                    SynapseGrowth(model: model)
                        .allowsHitTesting(false)
                }
                Spacer(minLength: 0)
                if let empty = model.emptyGraph {
                    Text(empty)
                        .font(.system(size: 15))
                        .foregroundStyle(Color(.sRGB, red: 200 / 255, green: 210 / 255, blue: 225 / 255, opacity: 1))
                        .multilineTextAlignment(.center)
                        .frame(maxWidth: .infinity)
                        .padding(.horizontal, FmSpace.x5)
                    Spacer(minLength: 0)
                }
                HStack(alignment: .bottom, spacing: FmSpace.x2) {
                    SynapseLegend(model: model)
                        .allowsHitTesting(false)
                    Spacer(minLength: 0)
                    buttons
                }
            }
            .padding(FmSpace.x3)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .clipped()
    }

    private var buttons: some View {
        VStack(spacing: FmSpace.x2) {
            roundButton(model.autoRotate ? "pause" : "sync", label: model.autoRotate ? "Stop auto-rotate" : "Auto-rotate",
                        active: model.autoRotate) { model.toggleAutoRotate() }
            roundButton(model.pulses ? "bolt" : "bolt_outlined", label: model.pulses ? "Hide pulses" : "Show pulses",
                        active: model.pulses) { model.pulses.toggle() }
            roundButton("open_in_full", label: "Fit", active: false) { model.fit(animate: true) }
            roundButton("format_list_bulleted", label: model.selected >= 0 ? "File details" : "Overview", active: model.sheetShown) {
                detent = .medium
                model.sheetShown = true
            }
        }
    }

    private func roundButton(_ icon: String, label: String, active: Bool, action: @escaping () -> Void) -> some View {
        Button {
            FmHaptics.selection()
            action()
        } label: {
            FmIcon(icon, size: 20, color: active ? Color(.sRGB, red: 88 / 255, green: 166 / 255, blue: 1, opacity: 1) : Color.white.opacity(0.85))
                .frame(width: 40, height: 40)
                .background(Color(.sRGB, red: 13 / 255, green: 18 / 255, blue: 32 / 255, opacity: 0.85))
                .clipShape(Circle())
                .overlay(Circle().stroke(Color.white.opacity(0.12), lineWidth: 1))
        }
        .buttonStyle(.plain)
        .accessibilityLabel(label)
        .accessibilityAddTraits(active ? .isSelected : [])
    }
}

struct SynapseGrowth: View {
    let model: SynapseModel

    var body: some View {
        VStack(alignment: .leading, spacing: FmSpace.x1) {
            Text(model.graph == nil ? "Wiring the brain…" : "Growing the brain · \(Int(model.progress * 100))%")
                .font(.system(size: 12))
                .foregroundStyle(Color(.sRGB, red: 200 / 255, green: 214 / 255, blue: 235 / 255, opacity: 220 / 255))
            ProgressView(value: model.progress)
                .tint(Color(.sRGB, red: 88 / 255, green: 166 / 255, blue: 1, opacity: 1))
                .frame(width: 160)
        }
        .accessibilityElement(children: .combine)
        .accessibilityLabel(model.graph == nil ? "Wiring the brain" : "Growing the brain, \(Int(model.progress * 100)) percent")
    }
}

struct SynapseLegend: View {
    let model: SynapseModel

    private var rows: [SynapseLegendRow] {
        _ = model.revision
        guard let graph = model.graph else { return [] }
        return model.style.legend(graph, model.snapshot)
    }

    var body: some View {
        let rows = self.rows
        VStack(alignment: .leading, spacing: 4) {
            ForEach(rows.indices, id: \.self) { (index: Int) in
                let row = rows[index]
                HStack(spacing: 6) {
                    Circle().fill(SynapseRenderer.color(row.colour)).frame(width: 8, height: 8)
                    Text(row.label)
                        .font(.system(size: 11))
                        .foregroundStyle(Color(.sRGB, red: 200 / 255, green: 210 / 255, blue: 225 / 255, opacity: 200 / 255))
                        .lineLimit(1)
                        .truncationMode(.middle)
                }
            }
        }
        .frame(maxWidth: 180, alignment: .leading)
        .accessibilityElement(children: .combine)
    }
}
