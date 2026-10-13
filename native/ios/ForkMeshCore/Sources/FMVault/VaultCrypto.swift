// Password Vault cryptography, suite "fmv1",
// the same formats the browser writes (marketplace/packages/vault/content/crypto/vault-crypto.js)
// and the desktop reads (content/desktop/VaultCrypto.cpp). There is no Kotlin counterpart.
//
// Nothing new is implemented here: FMCrypto supplies Argon2id, HKDF-SHA-256, AES-256-GCM, X25519,
// Ed25519, ML-KEM-768 (keys expanded from their d||z seed) and SHA3-256 for the X-Wing combiner.
// Tests/FMVaultTests/VaultInteropTests.swift opens the blobs the browser, the desktop and the
// retired Flutter app committed.
//
// Everything is synchronous. `stretch` is seconds of work and must run off the main actor
// (VaultClient does); the rest is milliseconds.
//
// NFKC comes from Foundation (`VaultCrypto.nfkc`). A padded-looking base64 value with non-zero
// trailing bits is `invalid_encoding`.

import FMBase
import FMCrypto
import Foundation

/// A failure carries the same code the browser suite throws, so every client explains one
/// problem in one way.
public struct VaultCryptoError: Error, Equatable, Sendable, CustomStringConvertible {
    public let code: String

    public init(_ code: String) {
        self.code = code
    }

    public var description: String { code.replacingOccurrences(of: "_", with: " ") }
}

/// NFKC of a password. Throw `VaultCryptoError("normalization_unavailable")`
/// when it cannot be done.
public typealias VaultNormalizer = @Sendable (String) throws -> String

public enum VaultCrypto {
    public static let suite = "fmv1"
    public static let permissions = ["read", "write"]

    static let context = "forkmesh-vault/fmv1/"
    static let mlkemPublicBytes = 1184
    static let mlkemCiphertextBytes = 1088
    /// XWingLabel, the six bytes \.//^\ that close the X-Wing combiner input.
    static let xwingLabel: [UInt8] = [0x5c, 0x2e, 0x2f, 0x2f, 0x5e, 0x5c]

    // MARK: - Bytes

    public static func randomBytes(_ count: Int) -> [UInt8] { CryptoBytes.random(count) }

    /// Unpadded base64url.
    public static func toBase64(_ bytes: [UInt8]) -> String { B64.encodeUrlNoPadding(bytes) }

    /// Unpadded (or padded-free) base64url from a JSON value; anything else is `invalid_encoding`.
    public static func fromBase64(_ value: JSONValue?) throws -> [UInt8] {
        try fromBase64(value?.stringValue ?? "")
    }

    public static func fromBase64(_ text: String) throws -> [UInt8] {
        let bad = VaultCryptoError("invalid_encoding")
        let utf8 = Array(text.utf8)
        for c in utf8 where !isBase64UrlByte(c) { throw bad }
        if utf8.count % 4 == 1 { throw bad }
        let padded = text + String(repeating: "=", count: (4 - utf8.count % 4) % 4)
        guard let data = B64.decodeOrNil(padded) else { throw bad }
        return Array(data)
    }

    public static func toHex(_ bytes: [UInt8]) -> String { Hex.encode(bytes) }

    /// Pairs of hex digits; nil for anything else.
    public static func fromHex(_ hex: String) -> [UInt8]? { Hex.decodeOrNil(hex).map { Array($0) } }

    /// Best effort: Swift may have copied the bytes, but the copy passed in is zeroed.
    public static func wipe(_ bytes: inout [UInt8]) { CryptoBytes.wipe(&bytes) }

    /// Compares byte strings without leaking through timing.
    public static func sameBytes(_ a: [UInt8], _ b: [UInt8]) -> Bool { CryptoBytes.constantTimeEquals(a, b) }

    private static func isBase64UrlByte(_ c: UInt8) -> Bool {
        (c >= 0x41 && c <= 0x5a) || (c >= 0x61 && c <= 0x7a) || (c >= 0x30 && c <= 0x39) || c == 0x2d || c == 0x5f
    }

    private static func utf8(_ text: String) -> [UInt8] { Array(text.utf8) }

    /// `^[a-z0-9](?:[a-z0-9-]{0,62})$` over the trimmed, lower-cased name.
    static func account(_ value: String) throws -> String {
        let name = value.dartTrimmed.lowercased()
        let bytes = Array(name.utf8)
        func alnum(_ c: UInt8) -> Bool { (c >= 0x61 && c <= 0x7a) || (c >= 0x30 && c <= 0x39) }
        guard (1...63).contains(bytes.count), alnum(bytes[0]),
              bytes.dropFirst().allSatisfy({ alnum($0) || $0 == 0x2d }) else {
            throw VaultCryptoError("invalid_account")
        }
        return name
    }

    /// `^[0-9a-f]{32}$`.
    static func itemIdChecked(_ value: String) throws -> String {
        let bytes = Array(value.utf8)
        guard bytes.count == 32,
              bytes.allSatisfy({ ($0 >= 0x30 && $0 <= 0x39) || ($0 >= 0x61 && $0 <= 0x66) }) else {
            throw VaultCryptoError("invalid_item_id")
        }
        return value
    }

    // MARK: - Symmetric

    static func hkdf(_ secret: [UInt8], _ info: String) -> [UInt8] {
        Hkdf.sha256(ikm: secret, salt: [UInt8](repeating: 0, count: 32), info: utf8(context + info), length: 32)
    }

    /// iv || ciphertext || tag, base64url. `label` is bound as associated data, so a blob moved to
    /// another row, item, or account fails to open.
    static func seal(_ key: [UInt8], _ label: String, _ plaintext: [UInt8]) throws -> String {
        if key.count != 32 { throw VaultCryptoError("invalid_key") }
        let box = try AesGcm.encrypt(plaintext, key: key, aad: utf8(context + label))
        return toBase64(box.nonce + box.cipherText + box.mac)
    }

    static func open(_ key: [UInt8], _ label: String, _ blob: JSONValue?) throws -> [UInt8] {
        let bytes = try fromBase64(blob)
        if key.count != 32 { throw VaultCryptoError("invalid_key") }
        if bytes.count < 12 + 16 { throw VaultCryptoError("invalid_ciphertext") }
        do {
            return try AesGcm.decrypt(
                cipherText: Array(bytes[12..<(bytes.count - 16)]),
                nonce: Array(bytes[0..<12]),
                mac: Array(bytes[(bytes.count - 16)...]),
                key: key,
                aad: utf8(context + label))
        } catch {
            throw VaultCryptoError("decryption_failed")
        }
    }

    // MARK: - Master password

    /// NFKC from Foundation: the default normalizer.
    public static let nfkc: VaultNormalizer = { text in text.precomposedStringWithCompatibilityMapping }

    static func normalized(_ password: String, _ normalizer: VaultNormalizer?) throws -> String {
        if password.utf16.allSatisfy({ $0 < 0x80 }) { return password }
        guard let normalizer else {
            // Deriving from an un-normalized password would make a key no other client can
            // reproduce: fail loudly instead.
            throw VaultCryptoError("normalization_unavailable")
        }
        return try normalizer(password)
    }

    /// Argon2id over the NFKC form of the password. Seconds of work: never on the main actor.
    public static func stretch(_ password: String, _ kdf: VaultKdf, normalizer: VaultNormalizer? = VaultCrypto.nfkc) throws -> [UInt8] {
        _ = try VaultKdf(json: .object(kdf.toJSON()))
        var secret = utf8(try normalized(password, normalizer))
        defer { wipe(&secret) }
        do {
            return try Argon2id.deriveKey(
                password: secret, salt: kdf.salt, parallelism: kdf.parallelism,
                memoryKiB: kdf.memoryKiB, iterations: kdf.passes, hashLength: 32)
        } catch {
            throw VaultCryptoError("unsafe_kdf_parameters")
        }
    }

    // MARK: - Keyset wrapping

    public static func wrapKeyset(_ secret: [UInt8], purpose: String, account: String, keyset: VaultKeyset) throws -> String {
        let name = try self.account(account)
        var kek = hkdf(secret, "keyset-kek|\(purpose)")
        defer { wipe(&kek) }
        return try seal(kek, "keyset|\(purpose)|\(name)", utf8(JSON.encode(keyset.toJSON())))
    }

    public static func unwrapKeyset(_ secret: [UInt8], purpose: String, account: String, blob: JSONValue?) throws -> VaultKeyset {
        let name = try self.account(account)
        var kek = hkdf(secret, "keyset-kek|\(purpose)")
        defer { wipe(&kek) }
        do {
            var plain = try open(kek, "keyset|\(purpose)|\(name)", blob)
            defer { wipe(&plain) }
            return try VaultKeyset(json: try JSON.parse(try Utf8.decode(plain)))
        } catch let error as VaultCryptoError {
            // A wrong master password and a corrupted blob look the same on purpose.
            let passthrough: Set<String> = ["unsupported_keyset", "invalid_keyset"]
            throw VaultCryptoError(passthrough.contains(error.code) ? error.code : "wrong_password")
        } catch {
            throw VaultCryptoError("wrong_password")
        }
    }

    // MARK: - Recovery code
    // 256 random bits as 52 Crockford base32 symbols (the last carries 4 padding bits), in groups
    // of four.

    static let base32 = Array("0123456789ABCDEFGHJKMNPQRSTVWXYZ")

    public static func newRecoveryCode() -> String {
        var bytes = randomBytes(32)
        defer { wipe(&bytes) }
        return recoveryCode(bytes)
    }

    /// The code for 32 given bytes (the tests' fixed input).
    static func recoveryCode(_ bytes: [UInt8]) -> String {
        // 256 bits then 4 zero bits, read five at a time from the top.
        var bits = [UInt8]()
        bits.reserveCapacity(260)
        for byte in bytes {
            for shift in stride(from: 7, through: 0, by: -1) { bits.append((byte >> UInt8(shift)) & 1) }
        }
        bits.append(contentsOf: [0, 0, 0, 0])
        var symbols = ""
        for i in stride(from: 51, through: 0, by: -1) {
            let start = (51 - i) * 5
            var value = 0
            for bit in bits[start..<start + 5] { value = (value << 1) | Int(bit) }
            symbols.append(base32[value])
            if i % 4 == 0 && i != 0 { symbols.append("-") }
        }
        return symbols
    }

    public static func recoverySecret(_ code: String) throws -> [UInt8] {
        var clean = ""
        for char in code.uppercased() {
            if char.isWhitespace || char == "-" { continue }
            switch char {
            case "O": clean.append("0")
            case "I", "L": clean.append("1")
            default: clean.append(char)
            }
        }
        let symbols = Array(clean)
        let values = symbols.map { symbol in base32.firstIndex(of: symbol) }
        guard symbols.count == 52, values.allSatisfy({ $0 != nil }) else {
            throw VaultCryptoError("invalid_recovery_code")
        }
        var bits = [UInt8]()
        bits.reserveCapacity(260)
        for value in values {
            for shift in stride(from: 4, through: 0, by: -1) { bits.append(UInt8((value! >> shift) & 1)) }
        }
        // The last four bits are padding.
        var out = [UInt8](repeating: 0, count: 32)
        for i in 0..<256 { out[i / 8] |= bits[i] << UInt8(7 - i % 8) }
        return out
    }

    // MARK: - Items

    public static func newItemId() -> String { toHex(randomBytes(16)) }

    /// Padded to a 256-byte step so a ciphertext's size says little about the password inside
    /// it. JSON ignores the trailing spaces.
    public static func sealItem(_ itemKey: [UInt8], itemId: String, fields: JSONObject) throws -> String {
        let body = utf8(JSON.encode(fields))
        let size = (body.count + 1 + 255) / 256 * 256
        var padded = body + [UInt8](repeating: 0x20, count: size - body.count)
        defer { wipe(&padded) }
        return try seal(itemKey, "item|\(try itemIdChecked(itemId))", padded)
    }

    public static func openItem(_ itemKey: [UInt8], itemId: String, blob: JSONValue?) throws -> JSONObject {
        var plain = try open(itemKey, "item|\(try itemIdChecked(itemId))", blob)
        defer { wipe(&plain) }
        guard let text = try? Utf8.decode(plain), let value = try? JSON.parse(text) else {
            throw VaultCryptoError("invalid_item")
        }
        guard let object = value.objectValue else { throw VaultCryptoError("invalid_item") }
        return object
    }

    public static func wrapItemKey(_ keyset: VaultKeyset, itemId: String, itemKey: [UInt8]) throws -> String {
        try seal(keyset.vaultKey, "itemkey|\(try itemIdChecked(itemId))", itemKey)
    }

    public static func unwrapItemKey(_ keyset: VaultKeyset, itemId: String, blob: JSONValue?) throws -> [UInt8] {
        try open(keyset.vaultKey, "itemkey|\(try itemIdChecked(itemId))", blob)
    }

    public static func sealPrivate(_ keyset: VaultKeyset, label: String, value: JSONObject) throws -> String {
        try seal(keyset.vaultKey, "private|\(label)", utf8(JSON.encode(value)))
    }

    /// The object sealed under `label`; empty when it decrypts to something else. Undecodable
    /// JSON throws the parser's error.
    public static func openPrivate(_ keyset: VaultKeyset, label: String, blob: JSONValue?) throws -> JSONObject {
        let plain = try open(keyset.vaultKey, "private|\(label)", blob)
        return try JSON.parse(try Utf8.decode(plain)).objectValue ?? JSONObject()
    }

    // MARK: - Shares: X25519 + ML-KEM-768, X-Wing combiner

    static func grantContext(_ itemId: String, _ sender: String, _ recipient: String, _ permission: String) throws -> String {
        ["grant", try itemIdChecked(itemId), try account(sender), try account(recipient), permission].joined(separator: "|")
    }

    static func grantMessage(_ label: String, _ recipientEd: [UInt8], _ ctX: [UInt8], _ ctM: [UInt8], _ wrapped: [UInt8]) -> [UInt8] {
        let digest = Sha256.hash(ctX, ctM, wrapped)
        return utf8([context + label, toBase64(recipientEd), toBase64(digest)].joined(separator: "\n"))
    }

    static func dh(_ privateKey: [UInt8], _ theirs: [UInt8], onZero: String) throws -> [UInt8] {
        let shared: [UInt8]
        do {
            shared = try X25519.sharedSecret(privateKey: privateKey, remotePublicKey: theirs)
        } catch {
            throw VaultCryptoError(onZero)
        }
        // An all-zero result means a low-order public key was supplied.
        if CryptoBytes.isAllZero(shared) { throw VaultCryptoError(onZero) }
        return shared
    }

    /// Seal an item key to one recipient and sign the result as the sender.
    public static func sealGrant(
        keyset: VaultKeyset,
        sender: String,
        recipient: VaultBundle,
        itemId: String,
        itemKey: [UInt8],
        permission: String
    ) throws -> JSONObject {
        if !permissions.contains(permission) { throw VaultCryptoError("invalid_permission") }
        if itemKey.count != 32 { throw VaultCryptoError("invalid_share") }
        let label = try grantContext(itemId, sender, recipient.account, permission)
        var ephemeral = X25519.generatePrivateKey()
        defer { wipe(&ephemeral) }
        let ctX = try X25519.publicKey(privateKey: ephemeral)
        var ssX = try dh(ephemeral, recipient.x25519, onZero: "invalid_public_keys")
        let encapsulation: MlKem768.Encapsulation
        do {
            encapsulation = try MlKem768.encapsulate(publicKey: recipient.mlkem768)
        } catch {
            throw VaultCryptoError("invalid_public_keys")
        }
        let ctM = encapsulation.ciphertext
        var ssM = encapsulation.sharedSecret
        var shared = Keccak.sha3_256(ssM, ssX, ctX, recipient.x25519, xwingLabel)
        var wrapKey = hkdf(shared, label)
        defer {
            wipe(&ssX)
            wipe(&ssM)
            wipe(&shared)
            wipe(&wrapKey)
        }
        let wrapped = try seal(wrapKey, label, itemKey)
        let signature = try Ed25519.sign(
            seed: keyset.ed25519,
            message: grantMessage(label, recipient.ed25519, ctX, ctM, try fromBase64(wrapped)))
        return [
            "v": 1,
            "suite": .string(suite),
            "x25519": .string(toBase64(ctX)),
            "mlkem768": .string(toBase64(ctM)),
            "wrapped": .string(wrapped),
            "signature": .string(toBase64(signature)),
        ]
    }

    /// Returns the item key, or throws: a share is only opened after its signature checks out
    /// against the sender's published bundle.
    public static func openGrant(
        keyset: VaultKeyset,
        recipient: String,
        sender: VaultBundle,
        itemId: String,
        grant: JSONValue?,
        permission: String
    ) throws -> [UInt8] {
        let map = grant?.objectValue ?? JSONObject()
        let bad = VaultCryptoError("invalid_share")
        guard map["v"]?.numEquals(1) == true, map["suite"] == .string(suite), permissions.contains(permission) else {
            throw bad
        }
        let ctX: [UInt8], ctM: [UInt8], wrapped: [UInt8], signature: [UInt8]
        do {
            ctX = try fromBase64(map["x25519"])
            ctM = try fromBase64(map["mlkem768"])
            wrapped = try fromBase64(map["wrapped"])
            signature = try fromBase64(map["signature"])
        } catch {
            throw bad
        }
        if ctX.count != 32 || ctM.count != mlkemCiphertextBytes || signature.count != 64 { throw bad }
        let label = try grantContext(itemId, sender.account, recipient, permission)
        let myEd = try Ed25519.publicKey(seed: keyset.ed25519)
        let signed = Ed25519.verify(
            publicKey: sender.ed25519,
            message: grantMessage(label, myEd, ctX, ctM, wrapped),
            signature: signature)
        if !signed { throw VaultCryptoError("invalid_share_signature") }

        let myX = try X25519.publicKey(privateKey: keyset.x25519)
        var ssX = try dh(keyset.x25519, ctX, onZero: "invalid_share")
        var ssM: [UInt8]
        do {
            ssM = try MlKem768.decapsulate(secretKey: keyset.mlkem.secretKey, ciphertext: ctM)
        } catch {
            throw bad
        }
        var shared = Keccak.sha3_256(ssM, ssX, ctX, myX, xwingLabel)
        var wrapKey = hkdf(shared, label)
        defer {
            wipe(&ssX)
            wipe(&ssM)
            wipe(&shared)
            wipe(&wrapKey)
        }
        let itemKey = try open(wrapKey, label, map["wrapped"])
        if itemKey.count != 32 { throw bad }
        return itemKey
    }

    // MARK: - Password generator

    static let classes: [(String, String)] = [
        ("lower", "abcdefghijkmnopqrstuvwxyz"), // no l
        ("upper", "ABCDEFGHJKLMNPQRSTUVWXYZ"), // no I, O
        ("digits", "23456789"), // no 0, 1
        ("symbols", "!@#$%^&*-_=+?"),
    ]

    public static func generatePassword(
        length: Int = 24,
        lower: Bool = true,
        upper: Bool = true,
        digits: Bool = true,
        symbols: Bool = true
    ) throws -> String {
        let wanted = [lower, upper, digits, symbols]
        let chosen = zip(classes, wanted).filter { $0.1 }.map { Array($0.0.1) }
        if chosen.isEmpty { throw VaultCryptoError("no_character_classes") }
        let alphabet = chosen.flatMap { $0 }
        let size = min(max(length, 8), 128)
        var generator = SystemRandomNumberGenerator()
        while true {
            // Uniform over the alphabet.
            let out = (0..<size).map { _ in alphabet[Int.random(in: 0..<alphabet.count, using: &generator)] }
            // Redraw the whole password rather than patch a class in: every accepted password is
            // then equally likely.
            if chosen.allSatisfy({ set in out.contains(where: set.contains) }) { return String(out) }
        }
    }

    // MARK: - Sites

    private static func loginComponents(_ url: String) -> URLComponents? {
        let text = url.dartTrimmed
        if text.isEmpty { return nil }
        guard var components = URLComponents(string: hasScheme(text) ? text : "https://" + text),
              let host = hostOf(components), !host.isEmpty else { return nil }
        let scheme = components.scheme?.lowercased() ?? ""
        let local = ["localhost", "127.0.0.1", "::1"].contains(host)
        if scheme != "https" && !(scheme == "http" && local) { return nil }
        components.scheme = scheme
        components.percentEncodedHost = components.percentEncodedHost?.lowercased()
        return components
    }

    /// `^[a-z][a-z0-9+.-]*://`, case-insensitive.
    private static func hasScheme(_ text: String) -> Bool {
        let bytes = Array(text.utf8)
        guard let first = bytes.first, isLetter(first) else { return false }
        var i = 1
        while i < bytes.count, isLetter(bytes[i]) || (bytes[i] >= 0x30 && bytes[i] <= 0x39)
            || bytes[i] == 0x2b || bytes[i] == 0x2e || bytes[i] == 0x2d {
            i += 1
        }
        return i + 2 < bytes.count && bytes[i] == 0x3a && bytes[i + 1] == 0x2f && bytes[i + 2] == 0x2f
    }

    private static func isLetter(_ c: UInt8) -> Bool { (c >= 0x41 && c <= 0x5a) || (c >= 0x61 && c <= 0x7a) }

    /// The host in lower case, IPv6 without brackets.
    private static func hostOf(_ components: URLComponents) -> String? {
        guard var host = components.host?.lowercased() else { return nil }
        if host.hasPrefix("[") && host.hasSuffix("]") { host = String(host.dropFirst().dropLast()) }
        return host
    }

    /// The host a saved login belongs to: "" unless https (or http on localhost).
    public static func loginHost(_ url: String) -> String {
        let host = loginComponents(url).flatMap(hostOf) ?? ""
        return host.hasPrefix("www.") ? String(host.dropFirst(4)) : host
    }

    /// The address an Open button may visit: never javascript:, data: or file:.
    public static func loginHref(_ url: String) -> String { loginComponents(url)?.string ?? "" }

    /// Exact host only. "Ends with" would hand a login saved for one tenant of a shared suffix
    /// (github.io, a co.uk) to every other tenant of it.
    public static func matchesHost(_ savedUrl: String, _ pageHost: String) -> Bool {
        let saved = loginHost(savedUrl)
        var page = pageHost.dartTrimmed.lowercased()
        if page.hasPrefix("www.") { page = String(page.dropFirst(4)) }
        return !saved.isEmpty && saved == page
    }
}

// MARK: - Stretch settings

/// RFC 9106's second recommended setting. The floor is the recommended minimum: a server that hands back
/// weaker numbers is refused, never obeyed.
public struct VaultKdf: Equatable, Sendable {
    public static let floorMemoryKiB = 19456
    public static let floorPasses = 2

    public let salt: [UInt8]
    public let memoryKiB: Int
    public let passes: Int
    public let parallelism: Int

    public init(salt: [UInt8], memoryKiB: Int = 65536, passes: Int = 3, parallelism: Int = 1) {
        self.salt = salt
        self.memoryKiB = memoryKiB
        self.passes = passes
        self.parallelism = parallelism
    }

    public static func fresh(memoryKiB: Int = 65536, passes: Int = 3) -> VaultKdf {
        VaultKdf(salt: VaultCrypto.randomBytes(16), memoryKiB: memoryKiB, passes: passes)
    }

    /// Validates what a server sent; `unsafe_kdf_parameters` for anything weak or malformed.
    public init(json: JSONValue?) throws {
        let map = json?.objectValue ?? JSONObject()
        let unsafe = VaultCryptoError("unsafe_kdf_parameters")
        let salt: [UInt8]
        do {
            salt = try VaultCrypto.fromBase64(map["salt"])
        } catch {
            throw unsafe
        }
        // A JSON integer, never a double.
        func integer(_ key: String) -> Int? {
            if case .int(let value)? = map[key] { return Int(exactly: value) }
            return nil
        }
        guard map["algorithm"] == .string("argon2id"),
              map["version"]?.numEquals(19) == true,
              let memory = integer("memoryKiB"),
              let passes = integer("passes"),
              let lanes = integer("parallelism"),
              salt.count >= 16, salt.count <= 64,
              memory >= VaultKdf.floorMemoryKiB, memory <= 1_048_576,
              passes >= VaultKdf.floorPasses, passes <= 16,
              lanes >= 1, lanes <= 16 else {
            throw unsafe
        }
        self.init(salt: salt, memoryKiB: memory, passes: passes, parallelism: lanes)
    }

    public func toJSON() -> JSONObject {
        [
            "algorithm": "argon2id",
            "version": 19,
            "memoryKiB": JSONValue(memoryKiB),
            "passes": JSONValue(passes),
            "parallelism": JSONValue(parallelism),
            "salt": .string(VaultCrypto.toBase64(salt)),
        ]
    }
}

// MARK: - Keyset

/// The raw keys of one unlocked account. A class so `wipe()` clears the one copy everyone holds.
public final class VaultKeyset: @unchecked Sendable {
    /// Wraps the owner's own item keys.
    public private(set) var vaultKey: [UInt8]
    /// Classical half of the share KEM (RFC 7748 clamps on use).
    public private(set) var x25519: [UInt8]
    /// d || z of the ML-KEM-768 key.
    public private(set) var mlkemSeed: [UInt8]
    /// Signs the bundle and every share (RFC 8032: any 32 bytes are a seed).
    public private(set) var ed25519: [UInt8]

    private let lock = NSLock()
    private var expanded: MlKem768.KeyPair?

    public init(vaultKey: [UInt8], x25519: [UInt8], mlkemSeed: [UInt8], ed25519: [UInt8]) throws {
        if vaultKey.count != 32 || x25519.count != 32 || mlkemSeed.count != 64 || ed25519.count != 32 {
            throw VaultCryptoError("invalid_keyset")
        }
        self.vaultKey = vaultKey
        self.x25519 = x25519
        self.mlkemSeed = mlkemSeed
        self.ed25519 = ed25519
    }

    public static func generate() -> VaultKeyset {
        // The lengths are right by construction.
        try! VaultKeyset(
            vaultKey: VaultCrypto.randomBytes(32),
            x25519: VaultCrypto.randomBytes(32),
            mlkemSeed: VaultCrypto.randomBytes(64),
            ed25519: VaultCrypto.randomBytes(32))
    }

    public convenience init(json: JSONValue?) throws {
        let map = json?.objectValue ?? JSONObject()
        guard map["v"]?.numEquals(1) == true else { throw VaultCryptoError("unsupported_keyset") }
        do {
            try self.init(
                vaultKey: VaultCrypto.fromBase64(map["vaultKey"]),
                x25519: VaultCrypto.fromBase64(map["x25519"]),
                mlkemSeed: VaultCrypto.fromBase64(map["mlkemSeed"]),
                ed25519: VaultCrypto.fromBase64(map["ed25519"]))
        } catch {
            throw VaultCryptoError("invalid_keyset")
        }
    }

    /// The expanded ML-KEM-768 pair. Expanding the seed costs as much as a decapsulation, so it
    /// is done once per unlocked keyset.
    public var mlkem: MlKem768.KeyPair {
        lock.lock()
        defer { lock.unlock() }
        if let expanded { return expanded }
        let pair = try! MlKem768.generateKeyPair(seed: mlkemSeed) // 64 bytes, checked in init
        expanded = pair
        return pair
    }

    /// An independent copy (with the expanded ML-KEM pair) for work on another task, so a
    /// `wipe()` of this one never races it. Wipe the copy when done.
    func copy() -> VaultKeyset {
        lock.lock()
        defer { lock.unlock() }
        let twin = try! VaultKeyset(vaultKey: vaultKey, x25519: x25519, mlkemSeed: mlkemSeed, ed25519: ed25519)
        twin.expanded = expanded
        return twin
    }

    public func toJSON() -> JSONObject {
        [
            "v": 1,
            "vaultKey": .string(VaultCrypto.toBase64(vaultKey)),
            "x25519": .string(VaultCrypto.toBase64(x25519)),
            "mlkemSeed": .string(VaultCrypto.toBase64(mlkemSeed)),
            "ed25519": .string(VaultCrypto.toBase64(ed25519)),
        ]
    }

    public func wipe() {
        lock.lock()
        defer { lock.unlock() }
        CryptoBytes.wipe(&vaultKey)
        CryptoBytes.wipe(&x25519)
        CryptoBytes.wipe(&mlkemSeed)
        CryptoBytes.wipe(&ed25519)
        expanded = nil
    }
}

// MARK: - Public keys

/// What an account publishes. Construct it with `verify` for anything that came from the server.
public struct VaultBundle: Equatable, Sendable {
    public let account: String
    public let x25519: [UInt8]
    public let mlkem768: [UInt8]
    public let ed25519: [UInt8]
    public let signature: [UInt8]

    static func message(_ account: String, _ x: [UInt8], _ m: [UInt8], _ ed: [UInt8]) -> [UInt8] {
        Array([
            VaultCrypto.context + "identity",
            account,
            VaultCrypto.toBase64(x),
            VaultCrypto.toBase64(m),
            VaultCrypto.toBase64(ed),
        ].joined(separator: "\n").utf8)
    }

    public static func of(_ keyset: VaultKeyset, account: String) throws -> VaultBundle {
        let name = try VaultCrypto.account(account)
        let x = try X25519.publicKey(privateKey: keyset.x25519)
        let ed = try Ed25519.publicKey(seed: keyset.ed25519)
        let m = keyset.mlkem.publicKey
        let signature = try Ed25519.sign(seed: keyset.ed25519, message: message(name, x, m, ed), publicKey: ed)
        return VaultBundle(account: name, x25519: x, mlkem768: m, ed25519: ed, signature: signature)
    }

    /// The name inside a bundle is part of what its signature covers, so one account's bundle
    /// served under another name never verifies.
    public static func verify(_ json: JSONValue?, account: String) throws -> VaultBundle {
        let map = json?.objectValue ?? JSONObject()
        let bad = VaultCryptoError("invalid_public_keys")
        let name: String, x: [UInt8], m: [UInt8], ed: [UInt8], signature: [UInt8]
        do {
            name = try VaultCrypto.account(account)
            x = try VaultCrypto.fromBase64(map["x25519"])
            m = try VaultCrypto.fromBase64(map["mlkem768"])
            ed = try VaultCrypto.fromBase64(map["ed25519"])
            signature = try VaultCrypto.fromBase64(map["signature"])
        } catch {
            throw bad
        }
        guard map["v"]?.numEquals(1) == true,
              map["suite"] == .string(VaultCrypto.suite),
              map["account"] == .string(name),
              x.count == 32, ed.count == 32, signature.count == 64,
              m.count == VaultCrypto.mlkemPublicBytes,
              mlkemKeyReduced(m) else {
            throw bad
        }
        guard Ed25519.verify(publicKey: ed, message: message(name, x, m, ed), signature: signature) else { throw bad }
        return VaultBundle(account: name, x25519: x, mlkem768: m, ed25519: ed, signature: signature)
    }

    public func toJSON() -> JSONObject {
        [
            "v": 1,
            "suite": .string(VaultCrypto.suite),
            "account": .string(account),
            "x25519": .string(VaultCrypto.toBase64(x25519)),
            "mlkem768": .string(VaultCrypto.toBase64(mlkem768)),
            "ed25519": .string(VaultCrypto.toBase64(ed25519)),
            "signature": .string(VaultCrypto.toBase64(signature)),
        ]
    }

    /// Read aloud or compared out of band before trusting a first share: 16 groups of four hex
    /// digits.
    public func fingerprint() -> String {
        let digest = Sha256.hash(
            Array("\(VaultCrypto.context)fingerprint\n\(account)\n".utf8), x25519, mlkem768, ed25519)
        let hex = Array(VaultCrypto.toHex(digest))
        return stride(from: 0, to: hex.count, by: 4).map { String(hex[$0..<min($0 + 4, hex.count)]) }
            .joined(separator: " ")
    }

    /// FIPS 203 7.2 modulus check: every 12-bit coefficient is below q = 3329.
    static func mlkemKeyReduced(_ key: [UInt8]) -> Bool {
        var i = 0
        while i + 2 < 1152 {
            let a = Int(key[i]) | ((Int(key[i + 1]) & 0x0f) << 8)
            let b = (Int(key[i + 1]) >> 4) | (Int(key[i + 2]) << 4)
            if a >= 3329 || b >= 3329 { return false }
            i += 3
        }
        return true
    }
}
