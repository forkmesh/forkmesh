import SwiftUI

struct SettingsSectionFrame: Equatable {
    let title: String
    let minY: CGFloat
    let maxY: CGFloat
}

struct SettingsSectionFramesKey: PreferenceKey {
    static var defaultValue: [SettingsSectionFrame] { [] }

    static func reduce(value: inout [SettingsSectionFrame], nextValue: () -> [SettingsSectionFrame]) {
        value.append(contentsOf: nextValue())
    }
}

struct SettingsPinnedSection: Equatable {
    let title: String
    let offset: CGFloat

    static func pick(_ frames: [SettingsSectionFrame], line: CGFloat, height: CGFloat) -> SettingsPinnedSection? {
        let ordered: [SettingsSectionFrame] = frames.sorted { (a: SettingsSectionFrame, b: SettingsSectionFrame) in
            a.minY < b.minY
        }
        guard let index = ordered.lastIndex(where: { (frame: SettingsSectionFrame) in frame.minY < line }),
              ordered[index].maxY > line
        else { return nil }
        var offset: CGFloat = 0
        if index + 1 < ordered.count {
            offset = min(0, ordered[index + 1].minY - line - height)
        }
        return SettingsPinnedSection(title: ordered[index].title, offset: offset)
    }
}

struct SettingsPinnedHeader: View {
    @Environment(\.fmPalette) private var fm
    let frames: [SettingsSectionFrame]
    @State private var line: CGFloat = 0
    @State private var height: CGFloat = 0

    var body: some View {
        let pinned: SettingsPinnedSection? = SettingsPinnedSection.pick(frames, line: line, height: height)
        ZStack(alignment: .top) {
            GeometryReader { (proxy: GeometryProxy) in
                Color.clear
                    .onAppear { line = proxy.frame(in: .global).minY }
                    .onChange(of: proxy.frame(in: .global).minY) { (_: CGFloat, top: CGFloat) in
                        line = top
                    }
            }
            .frame(height: 0)
            if let pinned = pinned {
                FmSectionHeader(title: pinned.title)
                    .padding(.horizontal, FmSpace.x4)
                    .padding(.vertical, FmSpace.x2)
                    .background(fm.canvas)
                    .overlay(alignment: .bottom) {
                        Rectangle().fill(fm.border).frame(height: 0.5)
                    }
                    .background {
                        GeometryReader { (proxy: GeometryProxy) in
                            Color.clear
                                .onAppear { height = proxy.size.height }
                                .onChange(of: proxy.size.height) { (_: CGFloat, next: CGFloat) in
                                    height = next
                                }
                        }
                    }
                    .offset(y: pinned.offset)
                    .accessibilityHidden(true)
                    .accessibilityIdentifier("settings-pinned-section")
            }
        }
        .frame(maxWidth: .infinity, alignment: .top)
        .clipped()
    }
}

extension View {
    func settingsSectionFrame(_ title: String) -> some View {
        background {
            GeometryReader { (proxy: GeometryProxy) in
                Color.clear.preference(
                    key: SettingsSectionFramesKey.self,
                    value: [
                        SettingsSectionFrame(
                            title: title, minY: proxy.frame(in: .global).minY, maxY: proxy.frame(in: .global).maxY),
                    ])
            }
        }
    }

    func settingsPinnedHeaders() -> some View {
        overlayPreferenceValue(SettingsSectionFramesKey.self, alignment: .top) { (frames: [SettingsSectionFrame]) in
            SettingsPinnedHeader(frames: frames)
        }
    }
}
