import FMBase
import Foundation

extension PackagesClient {
    public static let bookingPackageId = "booking"
    public static let bookingApi = "/api/booking"

    @discardableResult
    func bookingCall(_ method: String, _ path: String, body: JSONObject? = nil) async throws -> JSONObject {
        let reply = try await transport.send(method, path, query: [], body: body)
        if reply.ok && reply.json.optBool("ok") != false { return reply.json }
        var error = PackagesError.from(reply)
        if error.code.isEmpty && reply.ok { error.code = "send_failed" }
        throw error
    }

    public func bookingOverview() async throws -> BookingOverview {
        let reply = try await bookingCall("GET", Self.bookingApi)
        return await Task.detached(priority: .userInitiated) { BookingOverview(json: reply) }.value
    }

    public func installBooking(slug: String, timezone: String) async throws -> BookingOverview {
        let body: JSONObject = [
            "slug": .string(slug.trimmingCharacters(in: .whitespacesAndNewlines).lowercased()), "timezone": .string(timezone),
        ]
        return BookingOverview(json: try await bookingCall("POST", Self.bookingApi + "/install", body: body))
    }

    public func saveBooking(_ body: JSONObject) async throws -> BookingOverview {
        BookingOverview(json: try await bookingCall("PUT", Self.bookingApi + "/page", body: body))
    }

    public func cancelBooking(id: String, reason: String, notifyGuest: Bool) async throws -> BookingOverview {
        let path = Self.bookingApi + "/bookings/" + (try Self.item(id)) + "/cancel"
        let body: JSONObject = ["reason": .string(reason), "notifyGuest": .bool(notifyGuest)]
        return BookingOverview(json: try await bookingCall("POST", path, body: body))
    }

    public func checkBookingSlug(_ slug: String) async throws -> BookingSlugCheck {
        let clean = slug.trimmingCharacters(in: .whitespacesAndNewlines).lowercased()
        guard BookingText.isSlug(clean) else { return BookingSlugCheck(slug: clean, available: false, reason: "invalid_slug") }
        return BookingSlugCheck(json: try await bookingCall("GET", Self.bookingApi + "/slug/" + clean))
    }

    public func exportBooking() async throws -> BookingExport {
        let reply = try await bookingCall("GET", Self.bookingApi + "/export")
        let filename = reply.str("filename").isEmpty ? "forkmesh-booking.json" : reply.str("filename")
        let data = JSON.encodeData(reply["export"] ?? .object(JSONObject()))
        return BookingExport(filename: filename, json: String(decoding: data, as: UTF8.self))
    }

    public func uninstallBooking() async throws {
        try await bookingCall("POST", Self.bookingApi + "/uninstall", body: [:])
    }
}
