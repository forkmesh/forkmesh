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
| `rtp` | RTP header and RTCP compound parsing, the feedback packets an SFU forwards or makes, RTX, transport-wide congestion control feedback | RFC 3550, RFC 4585, RFC 5104, RFC 4588, RFC 8285, draft-holmer-rmcat-transport-wide-cc-extensions-01 |
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
func (m *Media) Extensions() map[string]int  // a=extmap URI -> id, "/direction" dropped
func (m *Media) RTXFor(primary uint8) (uint8, bool)  // rtx payload type whose apt= is primary
func (m *Media) Feedback(pt uint8) []string  // a=rtcp-fb values for pt (or *)
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
    FmtNACK = 1; FmtPLI = 1; FmtFIR = 4; FmtREMB = 15; FmtTCC = 15
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
type ReportBlock struct {
    SSRC uint32; FractionLost uint8; Lost int32; HighestSeq, Jitter, LSR, DLSR uint32
}
func NewReceiverReport(sender uint32, blocks ...ReportBlock) []byte  // at most 31 blocks
func ParseReportBlocks(p []byte) ([]ReportBlock, error)              // SR or RR
func SenderReportNTP(p []byte) (uint64, bool)
func ParseNACK(p []byte) (media uint32, lost []uint16, err error)
func NewNACK(sender, media uint32, lost []uint16) []byte

const TransportCCURI = "http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01"
func ExtensionElement(h Header, id uint8) ([]byte, bool)   // one-byte (0xBEDE) and two-byte (0x100X) forms
func TransportSequence(h Header, id uint8) (uint16, bool)
func Payload(b []byte, h Header) []byte                    // without padding

type Packet struct {
    Marker bool; PayloadType uint8; SequenceNumber uint16; Timestamp, SSRC uint32
    CSRC []uint32
    TransportID uint8; TransportSeq uint16  // TransportID 0: no extension
    Payload []byte; Padding int             // Padding <= 255
}
func (p *Packet) Marshal(dst []byte) []byte
func (p *Packet) Size() int
func RTXPayload(dst []byte, originalSeq uint16, payload []byte) []byte  // RFC 4588: OSN, then the payload
func ParseRTX(payload []byte) (uint16, []byte, error)

type FeedbackStatus struct { Received bool; Delta int32 }  // Delta in 250 us ticks
type TransportFeedback struct {
    SenderSSRC, MediaSSRC uint32
    BaseSequence uint16
    ReferenceTime uint32  // 24 bits, 64 ms units
    FeedbackCount uint8
    Statuses []FeedbackStatus
}
func (f *TransportFeedback) Marshal() ([]byte, error)
func ParseTransportFeedback(p []byte) (*TransportFeedback, error)
```

`Packet.Marshal` writes the transport-wide sequence number as the only header
extension, one-byte form for ids 1 to 14 and two-byte form above. The
transport-wide feedback (RTPFB, FMT 15) picks its packet status chunks the way
libwebrtc does (a run length while every status is the same, else a 2-bit
vector of 7, or a 1-bit vector of 14 when no delta is large), so the bytes
match Chrome's for the same input; `rtp/tcc_test.go` checks it against
feedback captured from Chrome. The first receive delta counts from the
reference time, each further one from the packet before it; a delta over
0..255 ticks takes two bytes. `ParseTransportFeedback` refuses reserved
symbols, deltas past the packet, an empty feedback, and more than 4096
statuses (`MaxParsedStatuses`), since one 2-byte run-length chunk can claim
8191 of them.

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
  receipt, and retransmitted per flight (RFC 6347 4.2.4): after 300 ms, then
  doubling to at most 4 s, seven times, so a handshake fails 20.5 s after the
  server's first flight. RFC 6347 suggests a 1 s first timer; with it one lost
  datagram cost a second of the join, and the bench showed it under loss
  ([call-bench.md](call-bench.md)). RFC 9147, the DTLS 1.3 successor, lowers
  the suggestion to 100 ms; 300 ms stays above the round trip of any path a
  call works on. The 4 s cap is below the RFCs' 60 s because a call that has
  not connected in 20 s is not worth waiting for. It stays safe: the server
  only keeps state and retransmits after the client has echoed the cookie, so
  the address is proven, and each resend is the same flight, at most seven
  times. The schedule moved on 2026-10-21 (it was 1 s doubling to 8 s, four
  resends).
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

An ICE-lite agent on one UDP port (`Mux`). It gathers one host candidate per
configured public address. Packets are demultiplexed by first byte
(RFC 7983): 0..3 STUN, 20..63 DTLS, 128..191 RTP/RTCP. A STUN binding request
is accepted when its USERNAME is `<sfu ufrag>:<peer ufrag>` of a known session
and its MESSAGE-INTEGRITY checks with the SFU password; the response carries
XOR-MAPPED-ADDRESS, MESSAGE-INTEGRITY and FINGERPRINT.

A valid request alone never moves media. Someone on the path can capture a
check and replay it from their own address, and if the SFU sent to the source
of any valid check, that would steal the stream. So the SFU sends its own
check back to that address, signed with the peer's password from its SDP
(`NewSession(remoteUfrag, remotePwd, handler)`). The address becomes the
selected path only when an authenticated response to that check's random
transaction id comes back from it: the first such address, then the address of
any nominated (USE-CANDIDATE) check. A session remembers at most 8 candidate
addresses and 16 outstanding checks, and DTLS and SRTP from any address other
than the selected one are dropped.

Consent (RFC 7675): every 5 s the SFU sends a check on the selected path
(`Session.Refresh`, called from the room tick), and a session with no
authenticated response for 30 s expires and its participant leaves.

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
  When the answer comes back, each track newly negotiated with that receiver
  gets a keyframe request; tracks it already had do not, so a join no longer
  makes every sender send a keyframe to everybody. A newcomer's answer
  usually arrives before its DTLS handshake ends, and a keyframe sent then
  could not reach it (and used up the one request a second), so its requests
  wait until the handshake ends (both changed 2026-10-21; the second took a
  second off video joins under loss).
- **Forwarding.** RTP from a sender is SRTP-decrypted with that participant's
  inbound context and re-encrypted with each receiver's outbound context.
  The header keeps its marker, timestamp, SSRC and sequence number; the
  payload type becomes the SFU's (96 VP8, 111 Opus), the sender's header
  extensions are dropped and the SFU's own transport-wide sequence number for
  that receiver is the only extension. Frames are end-to-end encrypted, so the
  SFU never parses payloads beyond the RTP header.
- **Feedback.** PLI and FIR from a receiver go to the sender of that SSRC
  (rewritten sender SSRC), at most one per second per stream however many
  receivers ask. Receivers' NACKs are answered by the SFU and never forwarded
  ("Loss repair"). Transport-wide feedback goes both ways and the SFU caps
  each sender with a REMB ("Rate control"). Sender reports are forwarded so
  receivers can sync audio and video, and their NTP time is kept for the
  SFU's own receiver reports.
- **SSRCs.** An offer is refused if it repeats an SSRC within itself (RTX
  included) or uses one any participant of the room has used before, so a
  participant cannot take over another's stream, even after that one left. A
  room remembers its last 4096 SSRCs.
- **Renegotiation.** A participant that leaves an SFU offer unanswered for
  10 s is removed, so one stuck client cannot freeze renegotiation for the
  room.
- **Work.** Each room has its own goroutine and a queue of 4096 packets; the
  UDP reader only hands packets over and drops them when the queue is full, so
  one busy room cannot stall the others.
- **Limits.** 16 participants per room (`MaxParticipants`), 4 per account
  in a room (`MaxPerAccount`, from the ticket's `acct`), 400 on a node
  (`MaxServerPeople`), 2 send tracks per participant, 1500-byte packets, and
  per participant 600 packets and 500 kB per second upstream (enough for
  1.5 Mbps video and Opus with its retransmissions and probes; the rest is
  dropped). Joining is checked under the
  server lock, so two joins cannot both take the last place.
- **Sockets.** At most 1000 open WebSockets per node and 200 that have not
  joined yet; more get `503`. Before the join a message may be 4 KiB, after it
  64 KiB. `/calls/health` answers `{"ok":true}` and does not say how many rooms
  are open.

Loss repair and rate control below were added on 2026-10-21 after the bench
([call-bench.md](call-bench.md)) showed the SFU had neither: under 1 fps of
video at 5% loss and no video at all under a 1 Mbps cap. The before and after
figures are there.

#### What is negotiated

- **Upstream** (the SFU's answer to the participant's offer, and the same
  sections in every later SFU offer): VP8 with `goog-remb`, `ccm fir`, `nack`
  and `nack pli`, plus the sender's RTX payload type (`apt=` the VP8 one) when
  it offers one; Opus as before. If the offer carries the transport-wide
  extension (`a=extmap:<id> http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01`),
  the answer accepts it with the same id and adds `transport-cc` feedback to
  both codecs. RTX is refused if its payload type would collide with the
  SFU's downstream ones (96, 97, 111), since a BUNDLE group cannot map one
  payload type to two codecs.
- **Downstream** (each forwarded track's section): VP8 96 with RTX 97
  (`apt=96`), `nack`, `nack pli`, `ccm fir` and `transport-cc`, and
  `a=ssrc-group:FID <ssrc> <rtx ssrc>` with an RTX SSRC the SFU draws for this
  receiver and track (registered in the room's used SSRCs, so no sender can
  claim it); Opus 111 with `transport-cc`; the transport-wide extension with
  the id the participant used in its own offer. A participant whose offer had
  no transport-wide extension gets none downstream either (its id for that
  URI is unknown, and reusing one could clash with its earlier offer), and is
  rate-controlled by REMB alone. The answer tells the SFU per section whether
  the receiver took RTX, and whether it took the extension.
  `goog-remb` is not offered downstream: receivers send transport-wide
  feedback instead.

#### Loss repair (NACK and RTX)

A receiver's NACK is never forwarded: one receiver's NACKs made the sender
retransmit to everybody (the reason it was removed), so the SFU repairs loss
itself, one hop at a time.

- **Downstream.** Every video track keeps the plain RTP the SFU forwarded,
  1024 slots, 256 KiB and 1 s at most (`packetCache`). A NACK from a receiver
  for a track it is negotiated on is answered from that cache, to that
  receiver only, as RTX (RFC 4588: payload type 97, the receiver's own RTX
  SSRC and its own sequence numbers, the original sequence number in the first
  two bytes, then the original payload). A packet is resent to one receiver at
  most 4 times and not again within 30 ms; only packets that receiver was sent
  (not ones from before it joined or while its video was paused) and not older
  than 1 s.
- **Upstream.** The SFU tracks the sequence numbers of each sender's video and
  sends its own NACK to the sender for gaps: at once, then again after 1.25
  round trips (measured from NACK to the RTX that answers it, 40 to 300 ms),
  6 times at most, for up to 1 s and 256 missing packets (a bigger gap asks
  for a keyframe instead). The sender answers on its RTX SSRC; the SFU
  rebuilds the original packet, caches it and forwards it once to every
  receiver, who all lacked it. A cache miss on a receiver's NACK therefore
  needs nothing upstream: either the SFU never had the packet and is already
  asking for it, or it is too old to be worth having. That is the decision
  for "forward a NACK upstream on a miss": never.
- **Duplicates.** A packet whose sequence number is already in the cache is
  dropped (a late original after its RTX, a second RTX, Chrome's RTX probes
  that resend old packets), and padding-only RTX is dropped after its arrival
  time is recorded.

#### Rate control

- **Upstream: the sender's own estimator.** The SFU records the arrival time
  of every authenticated packet by its transport-wide sequence number (4096
  in a ring) and sends transport-wide feedback each room tick (100 ms),
  several packets if deltas or count do not fit one. Chrome's congestion
  controller (GCC) then adapts the uplink by itself from delay and loss. Every
  500 ms the SFU also sends a receiver report with a real block per sender
  track: fraction lost since the last report, cumulative lost, extended
  highest sequence, interarrival jitter (RFC 3550 A.8, 90 kHz video, 48 kHz
  audio), LSR and DLSR from the last sender report. Loss counts only packets
  that never arrived as originals (RTX does not hide it), so Opus sees the
  real loss and turns in-band FEC on, and Chrome gets a round trip from
  LSR/DLSR that also times its NACKs.
- **Downstream: a per-receiver estimate.** Every packet to a receiver carries
  the SFU's transport-wide sequence number for that receiver, and the SFU
  keeps its send time and size (4096 in a ring). The receiver's transport-wide
  feedback drives an estimator (`downlink`, `sfu/estimate.go`) modelled on
  GCC: a delay trend (packets grouped by 5 ms of send time, bursts merged, the
  least-squares slope of 20 smoothed groups against an adaptive threshold), a
  queue guard (one-way delay above its 10 s minimum, smoothed, over 100 ms),
  loss over the last second, and the acknowledged rate over 500 ms. On
  overuse the estimate drops to 0.85 times the acknowledged rate, or lower to
  drain a measured queue within about a second (down to half), then holds for
  300 ms; while the queue stands it keeps dropping 10% a second; loss over 10%
  with a queue cuts it by half the loss; otherwise it grows 8% a second up to
  1.5 times the acknowledged rate. To find capacity it has not used, it probes:
  a burst of 5 to 40 padding-only RTX packets (240 bytes of padding) worth 20
  ms at twice the estimate, read back from the receive times as min(0.9 times
  the dispersion rate, twice the estimate); after a full result the next
  probe is 1 s later, else 2 s and then 4 s; no probe within 1 s of overuse or
  a decrease, or above 5% loss, and probe packets never enter the delay
  signal. The estimate starts unknown (no cap), becomes known at the first
  congestion, goes back to unknown once it reaches 1.2 times what the
  receiver could use (its senders times the ceiling), and is reset when the
  receiver's ICE path changes.
  This uses the receiver's transport-wide feedback rather than its REMB
  because Chrome only sends REMB from its receive-side estimator, which runs
  for streams without transport-wide feedback; the upstream needs
  transport-wide feedback anyway, and the per-packet loss and delay it gives
  is what the repair budget and the probes need.
- **Caps.** Each sender gets a REMB of min(the room ceiling, over every
  receiver of its tracks with a known estimate and video not paused,
  max(estimate / the senders that receiver gets, 80 kbps)). The ceiling is
  min(1.5 Mbps, max(200 kbps, 3 Mbps / (people - 1))): 1.5 Mbps up to three
  people, 1 Mbps at four, 500 kbps at seven, 200 kbps at sixteen. Chrome takes
  REMB as an upper limit on what its own estimator allows. A REMB goes out
  when the cap falls 5%, rises 5% (at most every 200 ms) or at least every
  second.
- **Pausing video.** A receiver whose share stays under 80 kbps for 1 s stops
  getting video (audio always flows) and stops capping its senders; when the
  share is back to 120 kbps, video resumes with a keyframe request for each
  track. Without simulcast this is the only way one person on a bad link does
  not hold everyone at 80 kbps. A receiver just above the floor still holds
  every sender it receives down to its share.

#### Why it stays safe

- **No amplification.** Repairs go only to the receiver that asked, over its
  consent-checked ICE path, so a NACK can never aim traffic at someone else.
  Per receiver and second: 50 NACKs, 64 sequence numbers each, 200 RTX
  packets and 20% of its estimate in bytes (4 KiB to 256 KiB; 256 KiB while
  unknown), each packet 4 times at most. The receiver's NACK and feedback
  packets still count against its 600-packet, 500 kB inbound budget.
- **Senders are never driven by receivers.** Receivers' NACKs stop at the
  SFU. The SFU's own NACKs to a sender are for gaps in what it received, at
  most a quarter of the packets the stream delivered in the last second
  (floor 20), so a sender on a congested uplink is not asked to resend into
  it; the sender's retransmissions count against its own upstream budget.
- **SRTP never seals an index twice.** RTX has the receiver's own SSRC and
  sequence numbers, probes too; a recovered packet is forwarded only if that
  sequence number is not cached (the original never went out), and the late
  original is dropped. `srtp` refuses to seal an index again in any case.
- **Only authenticated input moves anything.** Arrival times, NACKs and
  feedback are read after SRTP authentication. Transport-wide feedback holds
  at most 4096 statuses. A receiver's feedback only moves its own estimate,
  which can lower its senders' caps no further than 80 kbps (below that it
  loses its own video instead) and never raise them above the ceiling.
- **Keyframes** stay at one request per second per stream, including the ones
  for resumed video and for upstream gaps too big to repair.
- **Memory** is bounded per video track (256 KiB of packets plus 40 KiB of
  slots), per participant (224 KiB of send history, arrivals and rate
  samples) and per forwarded video track and receiver once it has NACKed
  (10 KiB of resend marks): at the 400 people a node allows, under 300 MB at
  worst.

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
`base64url(json{"room","peer","acct","exp"}) + "." + base64url(HMAC-SHA256)`
under the secret `FORKMESH_CALLS_SECRET`, which the app and the SFU share.
`room` is HMAC-SHA256 of the channel id under the same secret, so the SFU never
sees the channel id, `peer` is random per join, and tickets live 60 s. `acct`
is the first 32 hex digits of an HMAC of the account under the same secret: it
lets the SFU count one account's places in a room without learning who the
account is.

Two optional ticket fields belong to FM Media ([fmm-server.md](fmm-server.md)):
`"engine":"fmm"` and `"standby":true`. They are left out of WebRTC tickets,
so those are byte for byte what they were. The WebRTC SFU refuses a ticket
that has either field (`ticket_invalid`). Both servers in `forkmesh-calls`
share `signal.RoomEngines`: the server cap counts people on both engines,
and a room belongs to the engine of its first joiner until it empties. A
WebRTC join to a room that runs on FM Media gets
`{"type":"error","error":"room_engine","engine":"fmm"}`.

## Frame encryption

Each sender encrypts every encoded frame with AES-GCM before packetization:
in browsers through `RTCRtpScriptTransform` or the older
`createEncodedStreams`, in the phone apps through libwebrtc's FrameEncryptor
and FrameDecryptor, exposed to Swift and Kotlin by our patch on the libwebrtc
build. The layout follows SFrame (RFC 9605) with a key per sender: the first
bytes a decoder needs unencrypted stay clear (VP8: 10 bytes on a keyframe, 3
otherwise; Opus: 1), then ciphertext, the 16-byte tag, an 8-byte frame counter
and a 1-byte key id. Keys are random 128-bit values sent to the other
participants in the channel's sealed chat frames (never to the SFU). A sender
draws a new key on every join or rejoin and rotates it whenever somebody
leaves, and audio and video share one encryptor and so one counter, so a
counter is never used twice under a key. Receivers keep a 1024-frame replay
window per sender and key id; `docs/app/chat-group-calls.md` has the details.

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
