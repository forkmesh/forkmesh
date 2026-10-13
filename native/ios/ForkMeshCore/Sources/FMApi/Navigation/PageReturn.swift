import Foundation

public struct PageReturn<Page: Equatable & Sendable>: Equatable, Sendable {
    public let page: Page
    public let tab: AppTab
    public let landing: [Route]

    public init(page: Page, tab: AppTab, landing: [Route]) {
        self.page = page
        self.tab = tab
        self.landing = landing
    }

    public static func after(
        leaving page: Page?, fromTab: AppTab, fromPath: [Route], tab: AppTab, path: [Route]
    ) -> PageReturn? {
        guard let page = page, !path.isEmpty, tab != fromTab || path != fromPath else { return nil }
        return PageReturn(page: page, tab: tab, landing: path)
    }

    public func restores(tab now: AppTab, path: [Route]) -> Bool {
        now == tab && path.count < landing.count && landing.starts(with: path)
    }

    public func holds(tab now: AppTab, path: [Route]) -> Bool {
        now == tab && path.starts(with: landing)
    }
}
