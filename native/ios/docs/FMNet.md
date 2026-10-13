# FMNet

HTTP and WebSocket behind protocols, with public fakes for every module's
tests. `import FMNet` (re-import `FMBase` yourself for JSON).

## HTTP (`HTTPClient.swift`, `URLSessionHTTPClient.swift`, `FakeHTTPClient.swift`)

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

### FakeHTTPClient

```swift
let http = FakeHTTPClient()                       // or FakeHTTPClient { request in HTTPResponse(...) } as the fallback
http.enqueue(status: 200, json: ["ok": true])     // answered first, in order
http.enqueue(error: HTTPError.timedOut)
http.on("GET", path: "/api/status", json: [...])  // by URL path; method nil = any; later routes win
http.on(where: { $0.url.host == "x" }) { request in HTTPResponse(status: 201, text: request.bodyText) }
http.requests / http.lastRequest                   // recorded HTTPRequest values
```
Order of precedence: queue, then routes, then the fallback handler, else it throws `HTTPError.transport("FakeHTTPClient: no response for ...")`. It honours `maxResponseBytes`.

## WebSocket (`WebSocket.swift`, `URLSessionWebSocket.swift`, `FakeWebSocket.swift`)

Maps web_socket_channel: `connect` = `connect` + `await channel.ready`; `messages` = `channel.stream` (finishes = `onDone`, throws = `onError`); `send` = `sink.add`; `close(code:reason:)` = `sink.close(code)`.

- `protocol WebSocketConnector: Sendable`: `connect(url:headers:protocols:options:) async throws -> WebSocketConnection`; shorthand `connect(url:options:)`. Returns after the handshake; throws `WebSocketError`.
- `struct WebSocketOptions { connectTimeout = 15, pingInterval: TimeInterval? = nil, maxMessageBytes = 16 MiB }`. `pingInterval`: ping every interval, close with 1001 when the previous pong never came (the relay uses 25 s).
- `protocol WebSocketConnection: AnyObject, Sendable`: `messages: AsyncThrowingStream<WebSocketMessage, Error>` (ONE consumer), `send(_:) async throws` (+ `send(text:)`, `send(data:)`, `send(json:)`), `ping() async throws`, `close(code:reason:) async` (idempotent; `close()` = 1000), `closeCode`/`closeReason` (nil while open and after a break without a close frame), `negotiatedProtocol`.
- `enum WebSocketMessage { .text(String), .data(Data) }` with `.text` / `.data` accessors. `WebSocketStatus.normalClosure` etc.
- `enum WebSocketError`: `.handshakeFailed(status:message:)` (status 0 when there was no HTTP answer), `.connectTimedOut`, `.notConnected` (send/ping after close), `.transport(String)`.
- `URLSessionWebSocketConnector` exists only under `#if canImport(Darwin)`: the app target constructs it. On Linux there is only the fake. No hand-written RFC 6455.

### FakeWebSocketConnector / FakeWebSocketConnection

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

## Not verified

`URLSessionWebSocketConnector` has never run: it type-checks on Linux against swift-corelibs-foundation's identical API (`swift build -Xswiftc -DFM_LINUX_WEBSOCKET`) but has not been compiled by Xcode or connected to a server. `URLSessionHTTPClient` is tested through a URLProtocol stub only (mapping, streaming, limits, timeout, cancellation); redirects and real sockets are untested.
