# FMBase and FMNet

Part of [iOS core modules](README.md).
The two foundation modules every other module builds on: FMBase (no
dependencies) and FMNet (HTTP and WebSocket behind protocols, with fakes for
tests).

## FMBase

Foundations with no dependencies: ordered JSON, encodings, formatting, config,
storage protocols, clock, log, service registry. `import FMBase`.

### JSON (`JSONValue.swift`, `JSON.swift`, `JSONAccess.swift`)

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

### Encodings (`Encoding.swift`)

- `B64.encode(bytes)`, `B64.encodeUrl(bytes)`, `B64.encodeUrlNoPadding(bytes)`; any `Sequence<UInt8>` (Data, [UInt8]).
- `B64.decode(String) throws -> Data`: strict. Accepts both alphabets and `%3D` padding; REJECTS unpadded input whose length is not a multiple of 4, whitespace and newlines. `B64.decodeOrNil`. For the unpadded base64url the signed fields use: `B64.decodeUnpadded` (= `decode(base64Url.normalize(x))`).
- `Hex.encode(bytes)` (lower case), `Hex.decode(String) throws -> Data` (either case, even length), `Hex.decodeOrNil`.
- `Utf8.encode(String) -> Data`, `Utf8.decode(bytes) throws` (strict, drops BOM), `Utf8.decodeLossy`.
- `Bytes.random(n)` (system CSPRNG), `Bytes.concat(Data...)`, `Bytes.constantTimeEquals(a, b)`, `Bytes.isAllZero`.
- `String.dartTrimmed`: trims Dart's `trim()` whitespace set (not Foundation's), the set the signed formats trim with. Use it wherever a trimmed string feeds a key, salt or signature. It slices the scalar view and returns the string itself when there is nothing to trim, so a long prompt or title is not copied scalar by scalar on every list redraw.
- Errors are `EncodingError`.

### Formatting (`Format.swift`)

`Format.relativeTimeFrom(ms, nowMillis:)` / `(Date, now:)`, `Format.relativeTime(epochMs, nowMillis:)` ("never" for <= 0), `Format.absoluteDate(epochMs)` (UTC yyyy-MM-dd), `Format.countOrNil(n, singular:, plural:)`, `Format.clockTime12(Date, timeZone:)`, `Format.clockTime12From24("15:04:05")`.

### Config (`AppEnv.swift`, `AppBuild.swift`)

- `struct AppEnv` (instance, not statics: inject it). `AppEnv.prod` is the default; `AppEnv(infoDictionary: Bundle.main.infoDictionary)` reads FORKMESH_ENV, FORKMESH_API_HOST, FORKMESH_SERVER_URL, FORKMESH_WORLD_URL (missing, empty or unsubstituted `$(...)` values keep defaults). `environment`, `isDev`, `defaultServerUrl`, `worldSiteUrl`, `describe(serverUrl)`; statics `prodHost`, `relayPath`, `prodServerUrl`, `devServerUrl`, `relayUrlForHost`, `isLoopbackHost`, `isLoopbackUrl`.
- `struct AppBuild { version, platform, osVersion }`; `AppBuild(infoDictionary:)` gives "<CFBundleShortVersionString>+<CFBundleVersion>" (for example "1.2.2+13"). `platform` is "ios" / "macos" / "linux".
- `struct AppConfig { env, build }`, `AppConfig(infoDictionary:)`: what the app target builds once at startup.

### Storage (`KeyValueStore.swift`, `SecureStore.swift`)

Both protocols are synchronous, class-bound and Sendable (UserDefaults and the Keychain are synchronous).

- `protocol KeyValueStore`: `string|bool|int|double|stringList(forKey:) -> T?` (nil when unset or another type), `set(_:forKey:)` for String, Bool, Int, Double, [String] (and `String?`: nil removes), `remove(_)`, `contains(_)`, `keys: Set<String>`, `clear()`; `string(forKey:or:)` etc.
- `InMemoryKeyValueStore(seed: JSONObject)`, `.snapshot`. `FileKeyValueStore(url:)`: one pretty JSON file, rewritten atomically on every change; an unreadable file starts empty.
- `protocol SecureStore`: `read(_) -> String?`, `write(_, _)`, `delete(_)`, `readAll() -> [String: String]`; extension `migrateFromPrefs(_ prefs, keys:)`. `InMemorySecureStore(seed)`.
- The app target implements these in `ForkMesh/Platform` (UserDefaults, Keychain). Note `int(forKey:)` must return nil for a stored double; `double(forKey:)` may read a stored int.

### Clock, Log, Services

- `protocol Clock { var now: Date }` + `nowMillis`, `nowSeconds`; `SystemClock()`, `FixedClock(epochMillis:)` with settable `now` and `advance(by:)`. `Date.epochMillis`, `Date(epochMillis:)` (rounded to nearest ms).
- `Log.debug|info|warning|error(tag, message)`; message is an autoclosure, not built when below `Log.minimumLevel`. `Log.sink` is replaceable (default: stderr); the app target can route it to os_log.
- `Services.put(x)`, `Services.put(x, as: Proto.self)`, `Services.get(T.self)`, `Services.require()`, `Services.remove`, `Services.clear()`: the Kotlin port's registry, keyed by static type. Prefer constructor injection in the core; this is for app wiring.

### Editing trap

Tools that write files may decode a literal backslash-u-XXXX sequence in the text they are given. If a test needs one inside a Swift raw string, check the file on disk afterwards.

## FMNet

HTTP and WebSocket behind protocols, with public fakes for every module's
tests. `import FMNet` (re-import `FMBase` yourself for JSON).

### HTTP (`HTTPClient.swift`, `URLSessionHTTPClient.swift`, `FakeHTTPClient.swift`)

- `protocol HTTPClient: Sendable`
  - `send(_ request: HTTPRequest) async throws -> HTTPResponse`: any HTTP status is a response (4xx/5xx do not throw); failing to get one throws `HTTPError`.
  - `stream(_ request:) async throws -> HTTPStreamedResponse` (status, headers, `body: AsyncThrowingStream<Data, Error>`); default implementation reads the whole body via `send`. Only the Git transport needs it.
  - Conveniences: `request(method, url, headers:, body:, timeout:)`, `get`, `post`, `delete`, `postJSON(url, JSONValue, headers:)`.
- `struct HTTPRequest { method, url: URL, headers: HTTPHeaders, body: Data?, timeout = 30, followRedirects = true, maxResponseBytes = 64 MiB }`; `.bearer(token)` (no header for nil/empty), `.json(JSONValue)` (FMBase's canonical encoding + content type), `bodyText`.
- `struct HTTPResponse { status, headers, body }`: `ok`, `text` (lossy UTF-8), `json() throws`, `jsonObject` (empty when not an object), `errorCode` (the relay's `{"error": ...}`), `header(name)`. Inits: `(status:headers:body:)`, `(status:headers:text:)`, `(status:json:)`.
- `struct HTTPHeaders`: case-insensitive names, insertion ordered; `headers["content-type"]` get/set (nil removes; several fields join with ", "), `values(name)`, `add(name, value)`, `dictionary` (lower-case names), dictionary literal, `init([String: String])`.
- `enum HTTPError`: `.timedOut`, `.cancelled`, `.responseTooLarge(limit:)`, `.invalidResponse`, `.transport(String)`. Equatable.
- `URLSessionHTTPClient(configuration:)`: ephemeral session, no cookies, no cache. `timeout` bounds the WHOLE exchange for `send`; for `stream` it bounds the wait for the headers, after which URLSession's idle timeout applies. `followRedirects: false` hands back the 3xx itself. Task cancellation cancels the transfer. Works on Linux (FoundationNetworking).
- Telemetry is a decorator: wrap any `HTTPClient` in another that records and forwards. There is no global observer.

#### FakeHTTPClient

```swift
let http = FakeHTTPClient()                       // or FakeHTTPClient { request in HTTPResponse(...) } as the fallback
http.enqueue(status: 200, json: ["ok": true])     // answered first, in order
http.enqueue(error: HTTPError.timedOut)
http.on("GET", path: "/api/status", json: [...])  // by URL path; method nil = any; later routes win
http.on(where: { $0.url.host == "x" }) { request in HTTPResponse(status: 201, text: request.bodyText) }
http.requests / http.lastRequest                   // recorded HTTPRequest values
```
Order of precedence: queue, then routes, then the fallback handler, else it throws `HTTPError.transport("FakeHTTPClient: no response for ...")`. It honours `maxResponseBytes`.

### WebSocket (`WebSocket.swift`, `URLSessionWebSocket.swift`, `FakeWebSocket.swift`)

Maps web_socket_channel: `connect` = `connect` + `await channel.ready`; `messages` = `channel.stream` (finishes = `onDone`, throws = `onError`); `send` = `sink.add`; `close(code:reason:)` = `sink.close(code)`.

- `protocol WebSocketConnector: Sendable`: `connect(url:headers:protocols:options:) async throws -> WebSocketConnection`; shorthand `connect(url:options:)`. Returns after the handshake; throws `WebSocketError`.
- `struct WebSocketOptions { connectTimeout = 15, pingInterval: TimeInterval? = nil, maxMessageBytes = 16 MiB }`. `pingInterval`: ping every interval, close with 1001 when the previous pong never came (the relay uses 25 s).
- `protocol WebSocketConnection: AnyObject, Sendable`: `messages: AsyncThrowingStream<WebSocketMessage, Error>` (ONE consumer), `send(_:) async throws` (+ `send(text:)`, `send(data:)`, `send(json:)`), `ping() async throws`, `close(code:reason:) async` (idempotent; `close()` = 1000), `closeCode`/`closeReason` (nil while open and after a break without a close frame), `negotiatedProtocol`.
- `enum WebSocketMessage { .text(String), .data(Data) }` with `.text` / `.data` accessors. `WebSocketStatus.normalClosure` etc.
- `enum WebSocketError`: `.handshakeFailed(status:message:)` (status 0 when there was no HTTP answer), `.connectTimedOut`, `.notConnected` (send/ping after close), `.transport(String)`.
- `URLSessionWebSocketConnector` exists only under `#if canImport(Darwin)`: the app target constructs it. On Linux there is only the fake. No hand-written RFC 6455.

#### FakeWebSocketConnector / FakeWebSocketConnection

```swift
let ws = FakeWebSocketConnector()
ws.onConnect { c in c.receive(json: ["type": "hello"]); c.onSend { c, m in /* auto-reply */ } }
ws.failNext(WebSocketError.handshakeFailed(status: 401, message: ""))   // queued per attempt
ws.refuseConnections(WebSocketError.transport("offline"))               // until set back to nil
// after the code under test connected:
let server = ws.lastConnection!            // ws.connections, ws.attempts (url, headers, protocols, options)
server.receive(text:) / receive(json:) / receive(.data(...))
server.serverClose(code: 4001, reason: "replaced")    // messages finishes; closeCode set
server.fail()                                         // messages throws .transport
server.sent / sentTexts / sentJSON / pingCount / isOpen / closedByClient
server.failSends(with: error)                         // sends throw, socket stays open
await server.waitForSent(2) / waitUntilClosed(); await ws.waitForConnections(2)   // polling helpers, Bool
```

### Not verified

`URLSessionWebSocketConnector` has never run: it type-checks on Linux against swift-corelibs-foundation's identical API (`swift build -Xswiftc -DFM_LINUX_WEBSOCKET`) but has not been compiled by Xcode or connected to a server. `URLSessionHTTPClient` is tested through a URLProtocol stub only (mapping, streaming, limits, timeout, cancellation); redirects and real sockets are untested.
