import FMPackages
import SwiftUI
import UIKit

struct VideoMixDjStudioScreen: View {
    @Environment(\.dismiss) private var dismiss
    @State private var model: DjStudioModel?
    @State private var presented = false
    @State private var opened = false

    var body: some View {
        Color.black
            .ignoresSafeArea()
            .overlay {
                if opened, !presented {
                    Button("Open Video Mix DJ Studio") { present() }
                        .buttonStyle(.borderedProminent)
                }
            }
            .toolbar(.hidden, for: .navigationBar)
            .toolbar(.hidden, for: .tabBar)
            .statusBarHidden()
            .onAppear {
                guard !opened else { return }
                opened = true
                present()
            }
            .fullScreenCover(isPresented: $presented, onDismiss: { dismiss() }) {
                if let model {
                    DjStudioView(model: model)
                }
            }
    }

    private func present() {
        let studio = model ?? DjStudioHub.shared.studio()
        let shown = $presented
        studio.onLeave = { shown.wrappedValue = false }
        model = studio
        var transaction = Transaction()
        transaction.disablesAnimations = true
        withTransaction(transaction) { presented = true }
    }
}

struct DjStudioView: View {
    @Bindable var model: DjStudioModel

    var body: some View {
        GeometryReader { geometry in
            if geometry.size.width > geometry.size.height {
                DjLandscape(model: model, size: geometry.size)
            } else {
                DjPortrait(model: model, size: geometry.size)
            }
        }
        .background(DjTheme.backdrop.ignoresSafeArea())
        .overlay(alignment: .top) { DjNotice(model: model) }
        .animation(.easeOut(duration: 0.2), value: model.notice)
        .sheet(item: $model.sheet, onDismiss: {
            guard model.leavingAfterSheet else { return }
            model.leavingAfterSheet = false
            model.leave()
        }) { (sheet: DjSheet) in
            DjSheetView(model: model, sheet: sheet)
        }
        .statusBarHidden()
        .persistentSystemOverlays(.hidden)
        .defersSystemGestures(on: .all)
        .preferredColorScheme(.dark)
        .onAppear { model.appear() }
        .onDisappear { model.disappear() }
    }
}

struct DjLandscape: View {
    let model: DjStudioModel
    let size: CGSize

    var body: some View {
        let u = min(1.7, max(0.78, min(size.height / 380, size.width / 800)))
        let top = 76 * u
        let bottom = 64 * u
        let middle = max(120, size.height - top - bottom)
        let strip = 58 * u
        let centre = 46 * u
        let d = max(100, min(middle - 10 * u, (size.width - 2 * strip - centre - 40 * u) / 2))
        VStack(spacing: 0) {
            HStack(alignment: .top, spacing: 8 * u) {
                DjBackButton(model: model, size: 34 * u)
                    .padding(.top, 2 * u)
                DjLibraryHandle(model: model, side: .a, u: u)
                VStack(spacing: 4 * u) {
                    HStack(spacing: 8 * u) {
                        DjDeckInfo(model: model, deck: model.a, mirrored: false, u: u)
                        Spacer(minLength: 4 * u)
                        HStack(spacing: 12 * u) {
                            DjGearMenu(model: model, size: 30 * u)
                            DjRecordButton(model: model, size: 44 * u)
                            DjInfoButton(model: model, size: 30 * u)
                        }
                        Spacer(minLength: 4 * u)
                        DjDeckInfo(model: model, deck: model.b, mirrored: true, u: u)
                    }
                    .frame(height: 42 * u)
                    HStack(spacing: 10 * u) {
                        DjWaveform(model: model, deck: model.a)
                        DjZoomButton(model: model, u: u)
                        DjWaveform(model: model, deck: model.b)
                    }
                    .frame(height: 24 * u)
                }
                DjLibraryHandle(model: model, side: .b, u: u)
            }
            .padding(.horizontal, 6 * u)
            .padding(.top, 4 * u)
            .frame(height: top)
            .background(DjTheme.bar.ignoresSafeArea(edges: [.top, .horizontal]))
            .overlay(alignment: .bottom) { Rectangle().fill(Color.black.opacity(0.6)).frame(height: 1) }
            HStack(spacing: 0) {
                DjPitchStrip(model: model, deck: model.a, u: u)
                    .frame(height: middle - 12 * u)
                Spacer(minLength: 4 * u)
                DjPlatter(model: model, deck: model.a, d: d)
                ZStack {
                    Rectangle().fill(Color.black.opacity(0.35)).frame(width: 1)
                    DjPadsButton(model: model, size: 32 * u)
                        .offset(y: middle * 0.18)
                }
                .frame(width: centre, height: middle)
                .overlay(alignment: .top) { DjLogoTab(u: u).fixedSize() }
                .overlay(alignment: .bottom) { DjAutomixTab(model: model, u: u).fixedSize() }
                DjPlatter(model: model, deck: model.b, d: d)
                Spacer(minLength: 4 * u)
                DjPitchStrip(model: model, deck: model.b, u: u)
                    .frame(height: middle - 12 * u)
            }
            .padding(.horizontal, 8 * u)
            .frame(height: middle)
            HStack(spacing: 0) {
                DjTransport(model: model, deck: model.a, u: u, cell: CGSize(width: 40, height: 40))
                Spacer(minLength: 6 * u)
                DjCueButtons(model: model, deck: model.a, u: u, segment: 42)
                Spacer(minLength: 6 * u)
                DjCrossfader(model: model, u: u)
                    .frame(width: max(120 * u, min(300 * u, size.width - 636 * u)), height: bottom - 8 * u)
                Spacer(minLength: 6 * u)
                DjCueButtons(model: model, deck: model.b, u: u, segment: 42)
                Spacer(minLength: 6 * u)
                DjTransport(model: model, deck: model.b, u: u, cell: CGSize(width: 40, height: 40))
            }
            .padding(.horizontal, 10 * u)
            .frame(height: bottom)
            .background(DjTheme.bar.ignoresSafeArea(edges: [.bottom, .horizontal]))
            .overlay(alignment: .top) { Rectangle().fill(Color.black.opacity(0.6)).frame(height: 1) }
        }
        .frame(width: size.width, height: size.height)
    }
}

struct DjPortrait: View {
    let model: DjStudioModel
    let size: CGSize

    var body: some View {
        let u = min(1.6, max(0.74, min(size.width / 390, size.height / 780)))
        let header = 40 * u
        let wave = 24 * u
        let mixer = 110 * u
        let deckBody = max(120, (size.height - mixer - 2 * (header + wave) - 32 * u) / 2)
        let d = max(100, min(deckBody - 6 * u, size.width - 166 * u))
        VStack(spacing: 0) {
            deck(model.a, deckBody: deckBody, d: d, header: header, wave: wave, u: u)
            VStack(spacing: 6 * u) {
                HStack(spacing: 14 * u) {
                    DjGearMenu(model: model, size: 30 * u)
                    DjRecordButton(model: model, size: 40 * u)
                    DjInfoButton(model: model, size: 30 * u)
                    Spacer(minLength: 0)
                    DjZoomButton(model: model, u: u)
                    DjPadsButton(model: model, size: 30 * u)
                }
                .padding(.horizontal, 12 * u)
                ZStack(alignment: .top) {
                    DjCrossfader(model: model, u: u)
                        .frame(height: 46 * u)
                        .padding(.top, 14 * u)
                        .padding(.horizontal, 24 * u)
                    DjAutomixTab(model: model, u: u)
                        .fixedSize()
                        .scaleEffect(0.85)
                        .offset(y: -10 * u)
                }
            }
            .frame(height: mixer)
            .frame(maxWidth: .infinity)
            .background(DjTheme.bar.ignoresSafeArea(edges: .horizontal))
            .overlay(alignment: .top) { Rectangle().fill(Color.black.opacity(0.6)).frame(height: 1) }
            .overlay(alignment: .bottom) { Rectangle().fill(Color.black.opacity(0.6)).frame(height: 1) }
            deck(model.b, deckBody: deckBody, d: d, header: header, wave: wave, u: u)
        }
        .frame(width: size.width, height: size.height)
    }

    private func deck(_ deck: DjDeck, deckBody: CGFloat, d: CGFloat, header: CGFloat, wave: CGFloat, u: CGFloat) -> some View {
        VStack(spacing: 4 * u) {
            HStack(spacing: 8 * u) {
                if deck.side == .a {
                    DjBackButton(model: model, size: 32 * u)
                }
                DjDeckInfo(model: model, deck: deck, mirrored: false, u: u * 0.82)
                Spacer(minLength: 0)
                DjLibraryHandle(model: model, side: deck.side, u: u * 0.8)
            }
            .frame(height: header)
            DjWaveform(model: model, deck: deck)
                .frame(height: wave)
            HStack(spacing: 6 * u) {
                VStack(spacing: 6 * u) {
                    DjTransport(model: model, deck: deck, u: u, vertical: true)
                    DjCueButtons(model: model, deck: deck, u: u, vertical: true)
                }
                .frame(width: 64 * u)
                Spacer(minLength: 0)
                DjPlatter(model: model, deck: deck, d: d)
                Spacer(minLength: 0)
                DjPitchStrip(model: model, deck: deck, u: u * 0.92)
                    .frame(height: deckBody - 4 * u)
            }
            .frame(height: deckBody)
        }
        .padding(.horizontal, 10 * u)
        .padding(.vertical, 4 * u)
    }
}

struct DjZoomButton: View {
    let model: DjStudioModel
    let u: CGFloat

    var body: some View {
        Button {
            DjTheme.tick()
            model.toggleZoom()
        } label: {
            Image(systemName: "waveform")
                .font(.system(size: 18 * u, weight: .semibold))
                .foregroundStyle(model.zoomed ? DjTheme.active : DjTheme.muted)
                .frame(width: 30 * u, height: 24 * u)
                .contentShape(Rectangle())
        }
        .buttonStyle(DjPressStyle())
        .accessibilityLabel(model.zoomed ? "Show whole tracks" : "Zoom waveforms")
    }
}

struct DjPadsButton: View {
    let model: DjStudioModel
    let size: CGFloat

    var body: some View {
        DjRoundButton(size: size, action: { model.sheet = .pads }) {
            Image(systemName: "square.grid.3x3.fill")
        }
        .accessibilityLabel("Sampler pads")
    }
}
