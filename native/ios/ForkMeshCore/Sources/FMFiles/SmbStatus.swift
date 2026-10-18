import FMCrypto
import Foundation

enum SmbSigning {
    static func hmac(key: [UInt8], _ message: [UInt8]) -> [UInt8] { HmacSha256.mac(key: key, message) }
}

public enum SmbStatus {
    public static let success: UInt32 = 0
    public static let pending: UInt32 = 0x0000_0103
    public static let bufferOverflow: UInt32 = 0x8000_0005
    public static let noMoreFiles: UInt32 = 0x8000_0006
    public static let moreProcessingRequired: UInt32 = 0xC000_0016
    public static let endOfFile: UInt32 = 0xC000_0011
    public static let accessDenied: UInt32 = 0xC000_0022
    public static let nameNotFound: UInt32 = 0xC000_0034
    public static let nameCollision: UInt32 = 0xC000_0035
    public static let pathNotFound: UInt32 = 0xC000_003A
    public static let sharingViolation: UInt32 = 0xC000_0043
    public static let logonFailure: UInt32 = 0xC000_006D
    public static let diskFull: UInt32 = 0xC000_007F
    public static let mediaWriteProtected: UInt32 = 0xC000_00A2
    public static let notSupported: UInt32 = 0xC000_00BB
    public static let badNetworkName: UInt32 = 0xC000_00CC
    public static let directoryNotEmpty: UInt32 = 0xC000_0101
    public static let fileIsADirectory: UInt32 = 0xC000_00BA
    public static let notADirectory: UInt32 = 0xC000_0103
    public static let pipeEmpty: UInt32 = 0xC000_00D9

    public static func message(_ status: UInt32, host: String, share: String) -> String {
        switch status {
        case logonFailure: return "The user name or password is wrong."
        case accessDenied: return "Access denied."
        case badNetworkName: return "There is no share named \(share) on \(host)."
        case nameNotFound, pathNotFound: return "That file or folder no longer exists."
        case nameCollision: return "Something with that name is already there."
        case directoryNotEmpty: return "The folder is not empty."
        case sharingViolation: return "The file is open somewhere else."
        case diskFull: return "The disk is full."
        case mediaWriteProtected: return "The share is read only."
        default: return String(format: "The file server answered 0x%08X.", status)
        }
    }
}

public struct SmbError: Error, Equatable, Sendable, LocalizedError {
    public var status: UInt32
    public var message: String

    public init(status: UInt32 = 0, message: String) {
        self.status = status
        self.message = message
    }

    public var errorDescription: String? { message }

    public static let notLocal = SmbError(message: "Network shares must be on your local network.")

    public static func unreachable(_ host: String) -> SmbError {
        SmbError(message: "Could not reach \(host). Is file sharing on?")
    }

    public static func noDialect(_ host: String) -> SmbError {
        SmbError(status: SmbStatus.notSupported, message: "\(host) does not speak SMB 2.1.")
    }

    public static func status(_ status: UInt32, host: String, share: String) -> SmbError {
        SmbError(status: status, message: SmbStatus.message(status, host: host, share: share))
    }

    public static let tooLarge = SmbError(message: "The file is too large to open here. Download it instead.")
}

public struct SmbTransportError: Error, Equatable, Sendable {
    public var reason: String
    public init(_ reason: String) { self.reason = reason }
}
