import FMBase
import Foundation
import Observation

@MainActor
@Observable
public final class BookingModel {
    public private(set) var overview: BookingOverview?
    public private(set) var loading = false
    public private(set) var working = false
    public private(set) var loaded = false
    public var errorText = ""
    public var notice = ""

    @ObservationIgnored public let client: PackagesClient

    public init(client: PackagesClient) {
        self.client = client
    }

    public var installed: Bool { overview?.installed ?? false }

    public func refresh() async {
        loading = true
        defer {
            loading = false
            loaded = true
        }
        do {
            overview = try await client.bookingOverview()
            errorText = ""
        } catch {
            errorText = BookingText.describe(error)
        }
    }

    @discardableResult
    public func install(slug: String, timezone: String = TimeZone.current.identifier) async -> Bool {
        await run("Could not create the page: ") {
            self.overview = try await self.client.installBooking(slug: slug, timezone: timezone)
            self.notice = "Your booking page is live. Copy the link and send it to anyone."
        }
    }

    @discardableResult
    public func save(_ body: JSONObject, notice done: String) async -> Bool {
        await run("Could not save: ") {
            self.overview = try await self.client.saveBooking(body)
            self.notice = done
        }
    }

    @discardableResult
    public func setEnabled(_ enabled: Bool) async -> Bool {
        await save(
            ["enabled": .bool(enabled)],
            notice: enabled
                ? "Your page is live again."
                : "Your page is paused. The link shows a paused notice until you turn it back on.")
    }

    @discardableResult
    public func cancel(_ booking: Booking, reason: String, notifyGuest: Bool) async -> Bool {
        await run("Could not cancel: ") {
            self.overview = try await self.client.cancelBooking(id: booking.id, reason: reason, notifyGuest: notifyGuest)
            let name = booking.guest.name.isEmpty ? "your guest" : booking.guest.name
            self.notice = notifyGuest ? "Cancelled. We emailed \(name)." : "Cancelled."
        }
    }

    public func export() async -> BookingExport? {
        working = true
        defer { working = false }
        do {
            return try await client.exportBooking()
        } catch {
            errorText = "Could not export: " + BookingText.describe(error)
            return nil
        }
    }

    @discardableResult
    public func uninstall() async -> Bool {
        await run("Could not uninstall: ") {
            try await self.client.uninstallBooking()
            self.notice = "Booking uninstalled."
            self.overview = try await self.client.bookingOverview()
        }
    }

    private func run(_ prefix: String, _ work: @escaping () async throws -> Void) async -> Bool {
        working = true
        defer { working = false }
        do {
            try await work()
            errorText = ""
            return true
        } catch {
            notice = ""
            errorText = prefix + BookingText.describe(error)
            return false
        }
    }
}
