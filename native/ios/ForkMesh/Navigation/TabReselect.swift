import FMApi
import SwiftUI

struct TabTopAnchor: View {
    static let id: String = "fm.tab.top"

    var body: some View {
        Color.clear
            .frame(height: 0)
            .id(TabTopAnchor.id)
    }
}

@MainActor
private struct TabReselectObserver: ViewModifier {
    let tab: AppTab
    let action: () -> Void

    func body(content: Content) -> some View {
        let count: Int = AppNavigator.shared?.router.reselects[tab] ?? 0
        return content.onChange(of: count) { (_: Int, _: Int) in
            action()
        }
    }
}

extension View {
    func onTabReselect(_ tab: AppTab, perform action: @escaping () -> Void) -> some View {
        modifier(TabReselectObserver(tab: tab, action: action))
    }
}

@MainActor
@Observable
final class TabStackTops {
    private var shown: [AppTab: Int] = [:]

    func appeared(_ tab: AppTab) { shown[tab, default: 0] += 1 }

    func disappeared(_ tab: AppTab) { shown[tab] = max((shown[tab] ?? 0) - 1, 0) }

    func covered(_ tab: AppTab) -> Bool { (shown[tab] ?? 0) == 0 }
}

private struct TabStackPage: ViewModifier {
    let tops: TabStackTops
    let tab: AppTab

    func body(content: Content) -> some View {
        content
            .onAppear { tops.appeared(tab) }
            .onDisappear { tops.disappeared(tab) }
    }
}

extension View {
    func tabStackPage(_ tab: AppTab, tops: TabStackTops) -> some View {
        modifier(TabStackPage(tops: tops, tab: tab))
    }
}
