# FMBase

Foundations with no dependencies: ordered JSON, encodings, formatting, config,
storage protocols, clock, log, service registry. `import FMBase`.

## JSON (`JSONValue.swift`, `JSON.swift`, `JSONAccess.swift`)

Own parser and encoder; never use JSONSerialization or Codable for anything
that is signed or sent. `JSON.encode` output is byte-identical to Dart's
`jsonEncode` (checked against the real `dart` binary).

- `enum JSONValue`: `.null .bool(Bool) .int(Int64) .double(Double) .string .array([JSONValue]) .object(JSONObject)`. Equatable, Hashable, Sendable, all literal protocols (`["a": 1, "b": [true, nil]]`).
- `.int` and `.double` are different values (`.int(1) != .double(1)`) and encode differently ("1" vs "1.0"). An integer field must be `.int`. Use `numEquals(_:)` for numeric equality (`1 == 1.0`).
- `struct JSONObject`: insertion-ordered. `subscript(key) -> JSONValue?` (set nil removes; set `.null` stores null; re-setting keeps position), `keys`, `values`, `count`, `isEmpty`, `contains`, `removeValue(forKey:)`, `removeAll()`, `merge(_:)`, `init(pairs)`, Sequence of `(key, value)`. Equality ignores key order.
- `JSON.parse(String|Data) throws -> JSONValue` (throws `JSONError`; strict JSON grammar; integers past 64 bits become doubles; duplicate keys keep first position, last value; nesting over 512 throws). `JSON.parseObject(String|Data) -> JSONObject` never throws (empty on failure).
- `JSON.encode(JSONValue|JSONObject) -> String`, `JSON.encodeData(_) -> Data`, `JSON.encodePretty(_, indent: "  ")`, `JSON.formatDouble(_)` (Dart's `double.toString()` form, which the encoder uses).
- Wrapping inits: `JSONValue(String|Int|Int64|Double|Bool|[JSONValue]|JSONObject|[String])` and optionals (`JSONValue(optionalString)` gives `.null` for nil).
- Strict views (nil unless that type): `stringValue boolValue intValue int64Value doubleValue arrayValue objectValue isNull`; `intValue`/`doubleValue` accept either number kind (a double truncates toward zero). `value["key"]`, `value[0]` return `JSONValue?` (nil when missing / wrong type), so chain with `?`.
- Lenient views: `text` (the value as text, null is ""), `string(or:)`, `int(or:)`, `int64(or:)`, `double(or:)` (numbers or numeric strings), `isTrue` (`== true`), `bool(or:)`, `objectOrEmpty`, `arrayOrEmpty`, `objects` (the object entries), `strings`.
- By key, on `JSONObject` and on `JSONValue` (Kotlin JsonAccess names): `str(k)`, `str(k, fallback)`, `firstStr(k1, k2...)`, `optStr(k)`, `int(k, fallback)`, `int64(k, fallback)`, `double(k, fallback)`, `bool(k)`, `optBool(k)`, `obj(k)`, `list(k)`, `maps(k)`, `strings(k)`.

Surprises: NaN/Infinity encode as `null`. A lone `\uD800` escape parses to U+FFFD (Swift strings cannot hold half a pair). "-0" parses to `.int(0)`. `JSON.parse(Data)` drops a UTF-8 BOM; `parse(String)` does not.

## Encodings (`Encoding.swift`)

- `B64.encode(bytes)`, `B64.encodeUrl(bytes)`, `B64.encodeUrlNoPadding(bytes)`; any `Sequence<UInt8>` (Data, [UInt8]).
- `B64.decode(String) throws -> Data`: strict. Accepts both alphabets and `%3D` padding; REJECTS unpadded input whose length is not a multiple of 4, whitespace and newlines. `B64.decodeOrNil`. For the unpadded base64url the signed fields use: `B64.decodeUnpadded` (= `decode(base64Url.normalize(x))`).
- `Hex.encode(bytes)` (lower case), `Hex.decode(String) throws -> Data` (either case, even length), `Hex.decodeOrNil`.
- `Utf8.encode(String) -> Data`, `Utf8.decode(bytes) throws` (strict, drops BOM), `Utf8.decodeLossy`.
- `Bytes.random(n)` (system CSPRNG), `Bytes.concat(Data...)`, `Bytes.constantTimeEquals(a, b)`, `Bytes.isAllZero`.
- `String.dartTrimmed`: trims Dart's `trim()` whitespace set (not Foundation's), the set the signed formats trim with. Use it wherever a trimmed string feeds a key, salt or signature. It slices the scalar view and returns the string itself when there is nothing to trim, so a long prompt or title is not copied scalar by scalar on every list redraw.
- Errors are `EncodingError`.

## Formatting (`Format.swift`)

`Format.relativeTimeFrom(ms, nowMillis:)` / `(Date, now:)`, `Format.relativeTime(epochMs, nowMillis:)` ("never" for <= 0), `Format.absoluteDate(epochMs)` (UTC yyyy-MM-dd), `Format.countOrNil(n, singular:, plural:)`, `Format.clockTime12(Date, timeZone:)`, `Format.clockTime12From24("15:04:05")`.

## Config (`AppEnv.swift`, `AppBuild.swift`)

- `struct AppEnv` (instance, not statics: inject it). `AppEnv.prod` is the default; `AppEnv(infoDictionary: Bundle.main.infoDictionary)` reads FORKMESH_ENV, FORKMESH_API_HOST, FORKMESH_SERVER_URL, FORKMESH_WORLD_URL (missing, empty or unsubstituted `$(...)` values keep defaults). `environment`, `isDev`, `defaultServerUrl`, `worldSiteUrl`, `describe(serverUrl)`; statics `prodHost`, `relayPath`, `prodServerUrl`, `devServerUrl`, `relayUrlForHost`, `isLoopbackHost`, `isLoopbackUrl`.
- `struct AppBuild { version, platform, osVersion }`; `AppBuild(infoDictionary:)` gives "<CFBundleShortVersionString>+<CFBundleVersion>" (for example "1.2.2+13"). `platform` is "ios" / "macos" / "linux".
- `struct AppConfig { env, build }`, `AppConfig(infoDictionary:)`: what the app target builds once at startup.

## Storage (`KeyValueStore.swift`, `SecureStore.swift`)

Both protocols are synchronous, class-bound and Sendable (UserDefaults and the Keychain are synchronous).

- `protocol KeyValueStore`: `string|bool|int|double|stringList(forKey:) -> T?` (nil when unset or another type), `set(_:forKey:)` for String, Bool, Int, Double, [String] (and `String?`: nil removes), `remove(_)`, `contains(_)`, `keys: Set<String>`, `clear()`; `string(forKey:or:)` etc. Keys are the names the retired Flutter app stored ("server/url"), so migrated data reads back.
- `InMemoryKeyValueStore(seed: JSONObject)`, `.snapshot`. `FileKeyValueStore(url:)`: one pretty JSON file, rewritten atomically on every change; an unreadable file starts empty.
- `protocol SecureStore`: `read(_) -> String?`, `write(_, _)`, `delete(_)`, `readAll() -> [String: String]`; extension `migrateFromPrefs(_ prefs, keys:)`. `InMemorySecureStore(seed)`.
- The app target implements these in `ForkMesh/Platform` (UserDefaults, Keychain). Note `int(forKey:)` must return nil for a stored double; `double(forKey:)` may read a stored int.

## Clock, Log, Services

- `protocol Clock { var now: Date }` + `nowMillis`, `nowSeconds`; `SystemClock()`, `FixedClock(epochMillis:)` with settable `now` and `advance(by:)`. `Date.epochMillis`, `Date(epochMillis:)` (rounded to nearest ms).
- `Log.debug|info|warning|error(tag, message)`; message is an autoclosure, not built when below `Log.minimumLevel`. `Log.sink` is replaceable (default: stderr); the app target can route it to os_log.
- `Services.put(x)`, `Services.put(x, as: Proto.self)`, `Services.get(T.self)`, `Services.require()`, `Services.remove`, `Services.clear()`: the Kotlin port's registry, keyed by static type. Prefer constructor injection in the core; this is for app wiring.

## Editing trap

Tools that write files may decode a literal backslash-u-XXXX sequence in the text they are given. If a test needs one inside a Swift raw string, check the file on disk afterwards.
