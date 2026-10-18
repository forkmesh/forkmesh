import FMBase
import Foundation

public struct SecureDriveDevice: Equatable, Sendable, Identifiable {
    public var id: String = ""
    public var name: String = ""
    public var keys = SecureDrivePublicKeys()
    public var addedBy: String = ""
    public var addedAt: Int64 = 0
    public var mailbox: Int = -1
    public var removed = false
    public var removedBy: String = ""
    public var removedAt: Int64 = 0
    public var removedSeq: Int64 = -1
    public var root = false
}

public struct SecureDriveFileVersion: Equatable, Sendable {
    public var blob: String = ""
    public var key: [UInt8] = []
    public var pages: [SecureDriveRun] = []
    public var size: Int64 = 0
    public var sha256: String = ""
    public var time: Int64 = 0
    public var device: String = ""
}

public struct SecureDriveFileEntry: Equatable, Sendable, Identifiable {
    public var id: String = ""
    public var name: String = ""
    public var kind: String = ""
    public var versions: [SecureDriveFileVersion] = []
    public var created: Int64 = 0
    public var modified: Int64 = 0
    public var modifiedBy: String = ""
    public var createdBy: String = ""
    public var deleted = false
    public var conflicts = 0
    public var opens = 0
    public var lastOpened: Int64 = 0
    public var lastOpenedBy: String = ""

    public var current: SecureDriveFileVersion? { versions.last }
}

public struct SecureDriveRecord: Equatable, Sendable, Identifiable {
    public var device: String = ""
    public var seq: Int64 = 0
    public var lamport: Int64 = 0
    public var time: Int64 = 0
    public var action: String = ""
    public var data = JSONObject()
    public var prev: String = ""
    public var hash: String = ""
    public var witness = JSONObject()
    public var stored = JSONObject()

    public var id: String { hash }
}

public struct SecureDriveIssue: Equatable, Hashable, Sendable {
    public var code: String
    public var device: String
    public var detail: String

    public init(code: String, device: String = "", detail: String = "") {
        self.code = code
        self.device = device
        self.detail = detail
    }
}

public struct SecureDriveInvite: Equatable, Sendable, Identifiable {
    public var id: String = ""
    public var requestKey: [UInt8] = []
    public var approveKey: [UInt8] = []
    public var mailbox: Int = -1
    public var created: Int64 = 0
    public var expires: Int64 = 0
    public var createdBy: String = ""
    public var used = false
    public var revoked = false

    public func isOpen(_ now: Int64) -> Bool { !used && !revoked && expires > now }
}

public struct SecureDriveJoinRequest: Equatable, Sendable, Identifiable {
    public var mailbox: Int = -1
    public var inviteId: String = ""
    public var device: String = ""
    public var name: String = ""
    public var keys = SecureDrivePublicKeys()
    public var time: Int64 = 0
    public var safety: String = ""

    public var id: Int { mailbox }
}

public struct SecureDriveLeaseTiming: Equatable, Sendable {
    public var settleMs: Int
    public var staleMs: Int
    public var pollMs: Int

    public init(settleMs: Int = 600, staleMs: Int = 120_000, pollMs: Int = 250) {
        self.settleMs = settleMs
        self.staleMs = staleMs
        self.pollMs = pollMs
    }

    public static let standard = SecureDriveLeaseTiming()
}

public enum SecureDriveText {
    static let images: Set<String> = ["png", "jpg", "jpeg", "gif", "bmp", "webp"]

    public static func fileKind(_ name: String) -> String {
        let suffix = (name as NSString).pathExtension.lowercased()
        switch suffix {
        case "html", "htm": return "document"
        case "csv": return "spreadsheet"
        case "svg": return "drawing"
        case "txt", "md", "json", "log": return "text"
        default: return images.contains(suffix) ? "image" : "file"
        }
    }

    static func quoted(_ text: String) -> String { "\u{201C}\(text)\u{201D}" }

    public static func describe(_ record: SecureDriveRecord, deviceName: (String) -> String) -> String {
        let x = record.data
        let name = quoted(SecureDriveJSON.str(x, "name"))
        switch record.action {
        case "drive_created": return "Created the drive \(quoted(SecureDriveJSON.str(x, "label")))"
        case "drive_opened": return "Connected to the drive"
        case "device_added": return "Approved device \(name)"
        case "device_removed": return "Removed device \(name) and replaced the drive key"
        case "invite_created": return "Created an invitation code"
        case "invite_revoked": return "Cancelled an invitation code"
        case "invite_used": return "Used an invitation code to add \(name)"
        case "join_denied": return "Turned down a request to join from \(name)"
        case "file_created": return "Added \(name)"
        case "file_saved": return "Saved a new version of \(name)"
        case "file_restored": return "Restored version \(SecureDriveJSON.int(x, "version") + 1) of \(name)"
        case "file_renamed": return "Renamed \(quoted(SecureDriveJSON.str(x, "from"))) to \(name)"
        case "file_deleted": return "Deleted \(name)"
        case "file_rekeyed": return "Re-encrypted \(name) under the new drive key"
        case "file_opened":
            switch SecureDriveJSON.str(x, "purpose") {
            case "edit": return "Opened \(name) to edit"
            case "export": return "Saved a decrypted copy of \(name) outside the drive"
            case "external": return "Opened \(name) in another app"
            default: return "Opened \(name)"
            }
        case "drive_rekeyed": return "Replaced the drive key"
        case "log_exported": return "Exported the activity log"
        case "integrity_alert":
            let issue = SecureDriveIssue(code: SecureDriveJSON.str(x, "code"), device: SecureDriveJSON.str(x, "device"), detail: SecureDriveJSON.str(x, "detail"))
            return "Reported a problem: \(issueText(issue, deviceName: deviceName))"
        default: return record.action
        }
    }

    public static func issueText(_ issue: SecureDriveIssue, deviceName: (String) -> String = { $0 }) -> String {
        let device = deviceName(issue.device)
        switch issue.code {
        case "records_missing":
            return "Activity records from \(device) are missing from the drive (\(issue.detail)). Someone with access to the drive may have deleted them."
        case "log_forked":
            return "\(device) has two different activity records with the same number. The log was altered or a device's keys were copied."
        case "chain_broken": return "The activity records from \(device) do not link up (\(issue.detail))."
        case "bad_signature": return "A record claiming to come from \(device) has an invalid signature and was ignored."
        case "unknown_device": return "Records signed by a device that was never approved were ignored."
        case "after_removal": return "\(device) wrote records after it was removed; they were ignored."
        case "own_records_missing": return "Activity records this device wrote are missing from the drive."
        case "rolled_back" where issue.device.isEmpty:
            return "The drive's encrypted index went back to an older version (\(issue.detail)). The drive file may have been restored from an old copy."
        case "untrusted_root":
            return "The drive's encrypted index was written by a device that is not an approved member (\(device)) and was not used."
        case "keyring_missing":
            return "This device's key slot on the drive is gone: it was probably removed from the drive, or the drive file was damaged. It can no longer make changes."
        case "keyring_restored":
            return "This device's key slot on the drive was overwritten. The drive still opens with its keys, so it writes the slot again with its next change."
        case "index_restored":
            return "The drive's encrypted index was older than the one this device last saw, or was damaged. This device used its own verified copy; the next change saves it back to the drive."
        case "drive_truncated": return "The drive file is shorter than its index says. Part of it was cut off or lost."
        case "rolled_back":
            return "The drive shows older activity from \(device) than this device saw before (\(issue.detail)). The drive may have been restored from an old copy."
        case "missing_content": return "The encrypted content of \(quoted(issue.detail)) is missing from the drive."
        case "content_damaged": return "The encrypted content of \(quoted(issue.detail)) was changed or damaged and was not opened."
        case "untrusted_keyring": return "A key file that no member device signed was ignored."
        case "unreadable_objects": return "\(issue.detail) encrypted parts of the drive could not be opened with this device's keys."
        case "misplaced_object": return "An object from a different drive was found in this folder and ignored."
        default: return issue.code
        }
    }

    public static func errorText(_ code: String) -> String {
        switch code {
        case "locked": return "Unlock Secure Drive first."
        case "wrong_passphrase": return "That passphrase is not right."
        case "passphrase_too_short": return "Use at least 10 characters; a few unrelated words work well."
        case "device_exists": return "This iPhone is already set up for Secure Drive."
        case "device_unreadable": return "This iPhone's Secure Drive key file cannot be read."
        case "device_write_failed": return "Could not save this iPhone's key file."
        case "folder_unavailable": return "The folder cannot be reached or is read-only. Check that the router's drive is plugged in and shared, and that this iPhone can write to it."
        case "container_exists": return "A file with that name already exists. Pick a new name, or use Join a drive to open an existing drive."
        case "container_unavailable": return "The drive file cannot be reached or is read-only. Check that the router's drive is plugged in and shared, and that this device can write to it."
        case "not_a_drive": return "That file is not a Secure Drive."
        case "drive_unreadable": return "This device cannot read the drive's index with its keys. It may have been removed from the drive, or the file was damaged."
        case "drive_busy": return "Another device has been writing to the drive for a long time. Try again in a minute."
        case "drive_changed": return "Another device changed the drive at the same moment. Try again."
        case "drive_full": return "The drive has no room left. Free space on the router's USB drive, or delete files."
        case "drive_index_full": return "The drive's index is full. Delete old files or versions."
        case "folder_already_connected": return "This device is already connected to that drive."
        case "invalid_invite_code": return "That invitation code is not valid. Check it for typos."
        case "invite_expired": return "That invitation expired or was cancelled. Create a new one."
        case "unknown_request": return "That request is no longer on the drive."
        case "content_damaged": return "The file's encrypted content was changed or damaged, so it was not opened. This is recorded in the activity log."
        case "content_missing": return "The file's encrypted content is missing from the drive."
        case "drive_write_failed": return "Could not write to the drive. Check that it is still connected and has free space."
        case "device_removed": return "This iPhone was removed from the drive and can no longer change it."
        case "cannot_remove_self": return "To take this iPhone off the drive, use Forget this drive."
        case "invalid_name": return "Use a name without slashes, up to 255 characters."
        case "read_failed": return "Could not read that file on this iPhone."
        case "write_failed": return "Could not write the decrypted copy."
        case "unknown_file": return "That file is no longer on the drive."
        case "unknown_version": return "That version is no longer on the drive."
        case "unknown_invite": return "That invitation is no longer open."
        case "not_pending": return "This drive is not waiting for approval."
        case "no_drive_key": return "This iPhone holds no key for the drive."
        case "unknown_drive": return "That drive is not on this iPhone."
        case "unknown_device": return "That device is not a member of the drive."
        case "": return "Something went wrong."
        default: return code
        }
    }
}
