# Our WebRTC stack and the group-call server

Group audio and video calls (Meet or Discord style) run through a selective
forwarding unit (SFU) that ForkMesh writes and hosts itself. The SFU and the
WebRTC protocol stack under it are our own Go code in `server/rtc/`, built only
on the Go standard library: no Pion and no other third-party module (decided
2026-10-21; `server/go.mod` has no requirements and keeps none). One-to-one
calls stay peer to peer (`docs/app/chat-direct-calls.md`).

The phones get their media engine from Google's open-source libwebrtc, built
from source by our own CI (`native/webrtc/`, see "Phone media engine"), never a
prebuilt binary from somebody else.

## Why an SFU, and what it can see

In a mesh every phone sends its picture to every other phone, which stops
working past about four people on video. The SFU receives each participant's
streams once and forwards them to everybody else, which is how Meet and Discord
scale.

Media stays end-to-end encrypted. Every client encrypts each encoded audio and
video frame before it is packetized (see "Frame encryption") with a key that is
shared through the channel's sealed chat messages, so the SFU forwards
ciphertext it cannot read. What the SFU learns: that a call is running in some
room (an opaque id), who is connected (opaque per-join peer ids), when, from
which IP addresses, and the size and timing of packets.

## Packages (`server/rtc/`)

Each package is standalone, standard library only, with its own tests.

| Package | Role | Spec |
| --- | --- | --- |
| `stun` | STUN messages: parse, build, MESSAGE-INTEGRITY, FINGERPRINT, XOR-MAPPED-ADDRESS | RFC 5389, RFC 8445 attributes |
| `sdp` | Parse and write the SDP subset WebRTC uses | RFC 8866, RFC 8829 (JSEP) |
| `rtp` | RTP header and RTCP compound parsing, the feedback packets an SFU forwards or makes | RFC 3550, RFC 4585, RFC 5104 |
| `srtp` | SRTP and SRTCP, AES_CM_128_HMAC_SHA1_80 and AEAD_AES_128_GCM | RFC 3711, RFC 7714 |
| `dtls` | DTLS 1.2 server for the DTLS-SRTP key exchange | RFC 6347, RFC 5246, RFC 5764, RFC 7627, RFC 8422 |
| `ice` | ICE-lite agent and the single UDP port all media shares | RFC 8445 (lite), RFC 7983 demux |
| `sfu` | Rooms, participants, forwarding, server-driven renegotiation | this page |
| `signal` | WebSocket signaling server and join tickets | RFC 6455 |

`server/cmd/forkmesh-calls` is the binary.

### `stun`

```go
type Message struct {
    Type          uint16
    TransactionID [12]byte
    Attributes    []Attribute
}
type Attribute struct { Type uint16; Value []byte }

const (
    BindingRequest       = 0x0001
    BindingSuccess       = 0x0101
    BindingError         = 0x0111
    AttrMappedAddress    = 0x0001
    AttrUsername         = 0x0006
    AttrMessageIntegrity = 0x0008
    AttrErrorCode        = 0x0009
    AttrXORMappedAddress = 0x0020
    AttrPriority         = 0x0024
    AttrUseCandidate     = 0x0025
    AttrFingerprint      = 0x8028
    AttrICEControlled    = 0x8029
    AttrICEControlling   = 0x802A
)

func IsMessage(b []byte) bool
func Parse(b []byte) (*Message, error)
func (m *Message) Get(t uint16) ([]byte, bool)
func (m *Message) Add(t uint16, v []byte)
func (m *Message) AddXORMappedAddress(addr netip.AddrPort)
func (m *Message) AddErrorCode(code int, reason string)
func (m *Message) Encode(integrityKey []byte, fingerprint bool) []byte
func CheckIntegrity(raw []byte, key []byte) bool
func CheckFingerprint(raw []byte) bool
func DecodeXORAddress(value []byte, tid [12]byte) (netip.AddrPort, error)
```

`Encode` writes MESSAGE-INTEGRITY (HMAC-SHA1 over the message with the length
field already counting the attribute) when `integrityKey` is not nil, then
FINGERPRINT (CRC-32 of everything before it, XOR `0x5354554e`). `Parse` rejects
anything that is not exactly 20 bytes of header plus padded attributes within
the stated length, or lacks the magic cookie `0x2112A442`. `CheckIntegrity`
recomputes over the bytes before MESSAGE-INTEGRITY with the length adjusted,
and compares in constant time.

### `sdp`

```go
type Session struct {
    Version    int
    Origin     string
    Name       string
    Timing     string
    Attributes []Attribute
    Media      []*Media
}
type Media struct {
    Kind       string   // audio, video, application
    Port       int
    Proto      string   // UDP/TLS/RTP/SAVPF
    Formats    []string
    Connection string
    Bandwidth  []string
    Attributes []Attribute
}
type Attribute struct { Key, Value string; Flag bool }

func Parse(text string) (*Session, error)
func (s *Session) String() string            // CRLF line ends
func (s *Session) Attr(key string) (string, bool)
func (m *Media) Attr(key string) (string, bool)
func (m *Media) Attrs(key string) []string
func (m *Media) Has(key string) bool
func (m *Media) Mid() string
func (m *Media) Direction() string           // sendrecv when none is given
func (m *Media) SSRCs() []uint32             // distinct a=ssrc ids, first-seen order
func (m *Media) FIDGroups() [][2]uint32      // a=ssrc-group:FID primary rtx
func (m *Media) RTPMaps() map[uint8]string   // payload type -> "opus/48000/2"
func (m *Media) Fmtps() map[uint8]string
func ICECredentials(s *Session, m *Media) (ufrag, pwd string)   // media level wins
func Fingerprint(s *Session, m *Media) (algorithm, value string)
```

`Parse` keeps unknown lines (`Attributes` keeps their order) so `String`
round-trips anything it read. It refuses input over 64 KiB or 512 media
sections.

### `rtp`

```go
type Header struct {
    Marker           bool
    PayloadType      uint8
    SequenceNumber   uint16
    Timestamp        uint32
    SSRC             uint32
    CSRC             []uint32
    ExtensionProfile uint16
    Extension        []byte // raw extension block, no profile/length words
    PayloadOffset    int
    PaddingLength    int
}

func ParseHeader(b []byte) (Header, error)
func IsRTCP(b []byte) bool                  // RFC 5761: second byte 192..223
func SetSSRC(b []byte, ssrc uint32)
func SetPayloadType(b []byte, pt uint8)

const (
    TypeSR = 200; TypeRR = 201; TypeSDES = 202; TypeBYE = 203
    TypeRTPFB = 205; TypePSFB = 206
    FmtNACK = 1; FmtPLI = 1; FmtFIR = 4; FmtREMB = 15
)

func SplitCompound(b []byte) ([][]byte, error)
func PacketType(p []byte) uint8
func Count(p []byte) uint8                  // RC / FMT field
func SenderSSRC(p []byte) uint32
func MediaSSRC(p []byte) uint32             // feedback messages
func SetMediaSSRC(p []byte, ssrc uint32)
func NewPLI(sender, media uint32) []byte
func NewFIR(sender, media uint32, seq uint8) []byte
func NewREMB(sender uint32, bitrate uint64, ssrcs []uint32) []byte
func NewReceiverReport(sender uint32) []byte  // RC = 0
func ParseNACK(p []byte) (media uint32, lost []uint16, err error)
func NewNACK(sender, media uint32, lost []uint16) []byte
```

### `srtp`

```go
type Profile uint16
const (
    ProfileAES128CMHMACSHA1_80 Profile = 0x0001
    ProfileAEADAES128GCM       Profile = 0x0007
)

func KeyingMaterialLength(p Profile) int
func SplitKeyingMaterial(p Profile, material []byte) (clientKey, clientSalt, serverKey, serverSalt []byte, err error)

type Context struct { /* one direction */ }
func NewContext(p Profile, masterKey, masterSalt []byte) (*Context, error)
func (c *Context) EncryptRTP(dst, packet []byte) ([]byte, error)
func (c *Context) DecryptRTP(dst, packet []byte) ([]byte, error)
func (c *Context) EncryptRTCP(dst, packet []byte) ([]byte, error)
func (c *Context) DecryptRTCP(dst, packet []byte) ([]byte, error)
```

A `Context` covers one direction and any number of SSRCs: it keeps the
rollover counter and highest sequence per SSRC (RFC 3711 3.3.1 index guess),
a 128-packet replay window per SSRC on decrypt, and the SRTCP index per SSRC on
encrypt. The key derivation is RFC 3711 4.3 (AES-CM PRF, key derivation rate
0). The DTLS exporter output is laid out client key, server key, client salt,
server salt (RFC 5764 4.2). Tags are compared in constant time. `dst` may alias
nothing; the result is a new slice when `dst` is too small.

### `dtls`

A DTLS 1.2 server only: in WebRTC the SFU answers or offers with
`a=setup:passive`, so the browser or phone is always the DTLS client.

- One cipher suite, `TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256` (0xC02B), with
  P-256 key exchange and an ECDSA P-256 certificate, signature algorithm
  `ecdsa_secp256r1_sha256`. Everything WebRTC endpoints ship supports it.
- Extensions: `use_srtp` (required; the server picks the first of its
  preferred profiles the client offers), `extended_master_secret` (required;
  RFC 7627, the handshake fails without it), `supported_groups`,
  `ec_point_formats`, `renegotiation_info` (empty, refuses renegotiation).
- A stateless cookie (HelloVerifyRequest, HMAC-SHA256 of the client's address
  and ClientHello random under a per-process secret) before any state is kept,
  so the port cannot be used to amplify traffic.
- A CertificateRequest: WebRTC authenticates both sides by certificate
  fingerprint. The server checks the client's CertificateVerify signature and
  hands the client certificate to `Config.VerifyPeer`, which compares its
  SHA-256 fingerprint to the one in the client's SDP.
- Handshake messages are fragmented to fit 1200-byte datagrams, reassembled on
  receipt, and retransmitted per flight (RFC 6347 4.2.4, 1 s doubling to 8 s,
  then the handshake fails).
- After the handshake the connection only does `ExportKeyingMaterial`
  (`EXTRACTOR-dtls_srtp`), alerts and close_notify. Application data and
  renegotiation are refused. Records use the 64-packet replay window.

```go
type Certificate struct {
    DER         []byte
    Key         *ecdsa.PrivateKey
    Fingerprint string // "sha-256 AB:CD:..."
}
func GenerateCertificate() (*Certificate, error)
func FingerprintOf(der []byte) string

type Config struct {
    Certificate  *Certificate
    SRTPProfiles []uint16
    CookieSecret []byte
    VerifyPeer   func(der []byte) error
}

type Server struct { /* one remote address */ }
func NewServer(cfg *Config, remote string, send func([]byte) error) *Server
func (s *Server) HandleDatagram(b []byte, now time.Time) error
func (s *Server) Tick(now time.Time) error
func (s *Server) Done() bool
func (s *Server) Failed() error
func (s *Server) SRTPProfile() uint16
func (s *Server) ExportKeyingMaterial(label string, length int) ([]byte, error)
func (s *Server) PeerCertificate() []byte
func VerifyCookie(cfg *Config, remote string, hello []byte) bool
```

The server is event driven: it owns no goroutine or socket. The ICE layer
hands it every datagram whose first byte is 20..63 and calls `Tick` from its
timer.

### `ice`

An ICE-lite agent on one UDP port (`Mux`). The SFU only ever answers
connectivity checks (it gathers one host candidate per configured public
address and never sends checks). Packets are demultiplexed by first byte
(RFC 7983): 0..3 STUN, 20..63 DTLS, 128..191 RTP/RTCP. A STUN binding request
is accepted when its USERNAME is `<sfu ufrag>:<peer ufrag>` of a known session
and its MESSAGE-INTEGRITY checks with the SFU password; the response carries
XOR-MAPPED-ADDRESS, MESSAGE-INTEGRITY and FINGERPRINT. The source address of
the last nominated (USE-CANDIDATE) check is where the session sends, and DTLS
and SRTP from any other address are dropped. A session with no valid check for
30 s is closed (consent freshness, RFC 7675).

### `sfu`

A room holds participants. Each participant has one `RTCPeerConnection` with
the SFU, bundled on one transport, rtcp-mux required:

- **Upstream.** The participant's offer carries its own audio and video send
  sections. The SFU learns each track's SSRCs from `a=ssrc` lines (and RTX
  from `ssrc-group:FID`).
- **Downstream.** For every other participant's track the SFU adds a
  `sendonly` media section to its offer to this participant, with the original
  SSRCs and an `msid` of `<peer id> <track id>`, so the receiving client knows
  whose tile it is. Joins and leaves trigger a new SFU offer (server-driven
  renegotiation); a section whose sender left is set `inactive` and reused.
- **Forwarding.** RTP from a sender is SRTP-decrypted with that participant's
  inbound context and re-encrypted with each receiver's outbound context,
  unchanged otherwise (frames are end-to-end encrypted, the SFU never parses
  payloads beyond the RTP header).
- **Feedback.** PLI and FIR from a receiver go to the sender of that SSRC
  (rewritten sender SSRC), throttled to one per 500 ms per stream; NACK is
  forwarded the same way; the SFU sends its own receiver reports and a REMB
  per sender capped at 1.5 Mbps (video) so senders adapt. Sender reports are
  forwarded so receivers can sync audio and video.
- **Limits.** 16 participants per room (`MaxParticipants`), 2 send tracks per
  participant, 1500-byte packets, per-participant packet and byte budgets.

### `signal`

Participants talk to the SFU over a WebSocket (`/calls/ws`):

```
client -> {"type":"join","ticket":"..."}
sfu    -> {"type":"joined","peer":"<id>","peers":["<id>",...],"iceServers":[...]}
client -> {"type":"offer","sdp":"..."}           # first offer, its send tracks
sfu    -> {"type":"answer","sdp":"..."}
sfu    -> {"type":"offer","sdp":"...","rev":N}   # renegotiation
client -> {"type":"answer","sdp":"...","rev":N}
sfu    -> {"type":"peer-joined"|"peer-left","peer":"<id>"}
client -> {"type":"leave"}
```

The ICE candidate is in the SFU's SDP (`a=candidate`, ICE-lite, no trickle
from the SFU); the client's trickled candidates are not needed.

A join ticket proves the caller may join that room. The app server mints it for
a member of the private channel (`POST /api/chat/channels/<id>/call-ticket`):
`base64url(json{"room","peer","exp"}) + "." + base64url(HMAC-SHA256)` under the
secret `FORKMESH_CALLS_SECRET`, which the app and the SFU share. `room` is
HMAC-SHA256 of the channel id under the same secret, so the SFU never sees the
channel id, `peer` is random per join, and tickets live 60 s.

## Frame encryption

Each sender encrypts every encoded frame with AES-GCM before packetization:
in browsers through `RTCRtpScriptTransform` or the older
`createEncodedStreams`, in the phone apps through libwebrtc's FrameEncryptor
and FrameDecryptor, exposed to Swift and Kotlin by our patch on the libwebrtc
build. The layout follows SFrame (RFC 9605) with a key per sender: the first
bytes a decoder needs unencrypted stay clear (VP8: 10 bytes on a keyframe, 3
otherwise; Opus: 1), then ciphertext, the 16-byte tag, an 8-byte frame counter
and a 1-byte key id. Keys are random 128-bit values sent to the other
participants in the channel's sealed chat frames (never to the SFU), and a
sender rotates its key whenever somebody leaves.

## Phone media engine

The iOS and Android apps use libwebrtc for one-to-one and group calls instead
of a WebView, so a call answered on the lock screen has audio at once and
CallKit can own the audio session (`RTCAudioSession.useManualAudio`, enabled in
`provider(_:didActivate:)`).

`native/webrtc/PIN` names the libwebrtc release branch and exact commit (branch
8078, the one Chrome 156 stable ships, commit `2a990ce0ccd0`, picked
2026-10-21 from chromiumdash's milestone list). `native/webrtc/build.sh ios`
(macOS with Xcode) or `native/webrtc/build.sh android` (Linux) clones
depot_tools, syncs exactly that commit from webrtc.googlesource.com (and fails
if the checkout is anywhere else), applies every `native/webrtc/patches/*.patch`
with `git apply --check` first, and runs libwebrtc's own
`tools_webrtc/ios/build_ios_libs.py` (arm64 device, arm64 and x86_64
simulator) or `tools_webrtc/android/build_aar.py` (arm64-v8a, armeabi-v7a,
x86_64), writing `WebRTC.xcframework.zip` or `libwebrtc.aar` and its SHA-256
into `$WEBRTC_OUT` (`./webrtc-out` by default; `$WEBRTC_WORK` holds the
checkout). A checkout needs about 25 GB, so it runs on the office Mac (iOS) or a
large Linux machine (Android), not on the agent box.

What `build.sh` adds to libwebrtc, all from `native/webrtc/fm/` (copied into
the checkout) and `patches/0001-fm-frame-crypto-objc.patch` (regenerated by
`make_patches.py <src> <patch>` when the pin moves):

- `fm/fm_frame_crypto.{h,cc}`: the frame format of "Frame encryption" above,
  written on `std::span` only because libwebrtc builds with
  `-Wunsafe-buffer-usage` as an error, on the EVP cipher API that both BoringSSL
  and OpenSSL offer. `app/tests/test_group_call_frame_crypto.py` compiles it
  against OpenSSL with `fm_frame_crypto_tool.cc` and checks it against the
  browser worker in both directions, and that tampered or unkeyed frames are
  refused.
- `fm/fm_frame_cryptor.{h,cc}`: `webrtc::FrameEncryptorInterface` and
  `FrameDecryptorInterface` around it. libwebrtc keeps the normal VP8
  packetization with an encryptor attached (raw packetization only comes from
  `a=packetization:raw`), and the clear header bytes keep keyframe detection
  working, so phones and browsers share one call.
- `objc/RTCFmFrameCryptor.{h,mm}`: `RTCFmFrameEncryptor` / `RTCFmFrameDecryptor`
  for Swift (`setKey:keyId:`, `attachToSender:` / `attachToReceiver:`).
- `objc/RTCFmVoice.{h,mm}`: `RTCFmCaptureProcessor` (a block called with each
  10 ms of microphone samples, after echo cancellation) and
  `RTCFmFactory.factory(withCaptureProcessor:)`, which builds the default
  peer-connection factory with that `webrtc::CustomProcessing` as capture
  post-processing, so the native voice changer runs inside libwebrtc's audio path.

The Android build needs the same pieces as JNI classes and a Linux machine with
about 30 GB free; neither exists yet (2026-10-21). To move the pin, take the
`webrtc_branch` of the newest stable milestone from
`https://chromiumdash.appspot.com/fetch_milestones` and its commit from
`git ls-remote https://webrtc.googlesource.com/src refs/branch-heads/<n>`. The face, background and voice filters move to native code
(Vision and Core Image on iOS, the same models on Android) because the WebView
that ran them is gone.
