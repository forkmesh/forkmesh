// Git blob ids computed from bytes alone, with no repository. A blob's object
// id is `sha1("blob " + <byte length> + "\0" + <bytes>)`. Nothing about that
// needs an object store, which is what lets the phone build a valid
// `index <old>..<new>` line — and therefore a three-way applicable patch — for
// a file it fetched over the API from a repository it has never cloned.

import FMBase
import FMGit
import Foundation

public enum GitBlobId {
    /// The git object id `bytes` would have when stored as a blob.
    ///
    /// The whole input is held in memory once, so callers should keep this to
    /// the file sizes the in-app editor accepts rather than pointing it at a
    /// pack.
    public static func of(_ bytes: [UInt8]) -> GitObjectId {
        var framed = Array("blob \(bytes.count)".utf8)
        framed.append(0)
        framed.append(contentsOf: bytes)
        return GitObjectId.sha1Of(framed)
    }

    public static func of(_ data: Data) -> GitObjectId { of([UInt8](data)) }

    /// Whether a blob id a server reported agrees with the bytes it served.
    ///
    /// `served` is the id carried alongside a tree entry, which not every
    /// serving node supplies; an empty or unparseable value means "nothing to
    /// check" and answers true, because the computed id is then the only claim
    /// anyone is making and it is correct by construction.
    ///
    /// A disagreement means the bytes in hand are not the bytes git has — a
    /// transcoding proxy, a stale cache, or a decoded string that was
    /// re-encoded on the way. Building a patch on top of that produces an
    /// `index` line whose pre-image nobody can resolve, so the caller should
    /// refuse rather than submit it.
    public static func servedAgrees(_ served: String, _ computed: GitObjectId) -> Bool {
        let claimed = served.dartTrimmed.lowercased()
        let bytes = Array(claimed.utf8)
        let isHex = bytes.count == 40 && bytes.allSatisfy { (0x30...0x39).contains($0) || (0x61...0x66).contains($0) }
        if !isHex { return true }
        return claimed == computed.hex.lowercased()
    }
}
