# Online play ("Todo o mundo") with the host's game as the server: research spec

Status: research only, nothing implemented. Written 2026-11-26 from source and from the game's own code (2.3.0 /
update 1.1.0 exefs). Where something was not verified it says so.

Goal: friends play the game's **online** mode between PCs over ZeroTier, with **no dedicated server**: the host
player's own OpenJamboree process answers everything the game would ask Nintendo's servers.

## 0. The headline: Jamboree is NPLN, not NEX

OpenSmash-Ultimate's online mode emulates **NEX** (PRUDP + RMC, the 2017-era Nintendo game server stack). Super Mario
Party Jamboree does **not** use NEX at all. It uses **NPLN**, Nintendo's newer backend: **gRPC over HTTP/2 over TLS**,
protobuf messages, plus **Pia 6.34** for the peer-to-peer game traffic.

Evidence from `exefs/main` (decompressed with a small LZ4 script; offsets are into the decompressed image):

| What | Finding |
|---|---|
| NEX | **0** symbols with `_ZN2nn3nex` and no `nn::nex` strings. No PRUDP, no `NgsFacade`, no NEX game server id / access key. |
| NPLN | Middleware tags at `0x19c9616..0x19c96be`: `NintendoSDK_NPLN-1_26_1`, `NintendoSDK_Google_APIs_For_NPLN-1_26_1`, `NintendoSDK_gRPC_For_NPLN`, `NintendoSDK_OpenSSL_For_NPLN`. |
| Pia | Tags at `0x19c975c..0x19c98a2`: `Pia`, `PiaCommon`, `PiaClone`, `PiaLocal`, `PiaSession`, `PiaSync`, `PiaTransport`, `Pia_ZStandard`, `PiaNplnd`, all **6.34.0**. Classes `nn::pia::nplnd::{NplndFacade, NplndService, NplndLoginJob, AttachMeshJob, NplndRelayClient, IceServerConfigGetter, NplndHostMigrationJob}`, `nn::pia::turn::TurnProtocol`, `nn::pia::nat::NatCheckJob`, WebRTC `cricket::Stun*`. |
| Game side | `bex::NetworkModule::{ConnectNpln, CreateNplnSession, SearchNplnSession, JoinNplnSession, JoinSelectedNplnSession, JoinRandomNplnCompanionSession, SetupNplnSession, AbortNplnMatchmake, GetNplnShortGameSessionID}`, `bq::Net::UpdateNplnSessionPropaties` (sic). |
| Other libs | libcurl 17.5.9 (uses `nnssl*`, the system TLS), OpenSSL for NPLN (in-process TLS for gRPC), `bex::NplnGamesync`. |

So **almost none of OpenSmash's NEX protocol code applies**. What carries over is the architecture (a server inside the
game process, an io/store split), the name-override approach, the fake id token, and Pia's NAT-check responder (§1.4,
§4).

### 0.1 Per-game values found (Jamboree)

| Value | Where | Notes |
|---|---|---|
| NPLN tenant id **`t-adf89f68`** | string `0x1575f6a`, passed at `main+0x1f93dc` to the channel setup `main+0x9aafc0` | Matches the wiki's tenant list for Jamboree (`adf89f68`) [K-NS]. |
| Endpoint format `%s.%s%s.t.npln.srv.nintendo.net:443` | string `0x15c7447`, built at `main+0xa1d3bc` | `main+0x9aa968` makes the first part: `t-<tenant>-<env>` (or `<tenant>-<env>` when the tenant already starts with `t-`), with `<env>` from `nn::nsd::GetEnvironmentIdentifier` (`lp1` on retail). Result: **`t-adf89f68-lp1.lp1.t.npln.srv.nintendo.net:443`**. |
| gRPC metadata `npln-tenant-id` | string `0x155b93f`, used at `main+0x9c4a80`, `+0x9c4c18` | Required on every call [K-NS]. Also `authorization: Bearer <access token>` and `uid`. |
| Pia NAT check servers | `nncs1-%.n.n.srv.nintendo.net:10025` (`0x15546fb`, used at `main+0x10cc158`), `nncs2-%...:10025` (`0x158c893`, `main+0x10cc180`) | `%` is replaced through `nn::nsd::ResolveEx` → `nncs1-lp1.n.n.srv.nintendo.net`. UDP 10025 and 10125 [K-NC]. |
| Pia monitoring host | `g2122d301` (`0x15544c0`) formatted with `%s.%%.p.srv.nintendo.net` at `main+0xfbc084` | → `g2122d301.lp1.p.srv.nintendo.net`, used by `MonitoringDataSendJob`. Not needed: let it fail. |
| Gamesync | `gamesync.npln.nintendo.net` (`0x15a7eb1`, `main+0xd672b8`) | Gamesync is an NPLN service (`nn.npln.gamesync.v1.Gamesync`). |
| Optional romfs config | `_npln:/npln_config.json` (`0x156054e`) | `main+0xa2bba4` does `nn::fs::MountRom("_npln")` and reads up to 4 KiB of `npln_config.json`, then parses it into a map at `main+0x1c4c1a8`. Retail romfs has no such file. The only lookup is `main+0xa2baf0`, called from `main+0x9aa98c` on the tenant before the host name is built. Its gate byte `main+0x1bc7600` is 1 in `.data` and nothing in `main` stores to it, so the file **would** be read on retail. What it does (from the disassembly of `+0xa2bd50` and `+0xa2be50`): an optional allow-list of tenants (a tenant not in a non-empty list aborts), then a **tenant remap** (`tenant` → mapped value, else the `"*"` entry, else unchanged). It only renames the tenant, so it is **not needed**: the DNS override does the job. |
| Embedded test CA | PEM "NPLN Local Test Root" (Nintendo Co., Ltd.) near `0x155b9xx` | For Nintendo's local test servers. We don't have its key; irrelevant except as a sign that the CA list is pluggable. |
| No NSO licence API | none of `nn::account::*NetworkServiceLicense*`, `nn::ec` imported | Membership is only checked **server side** (by Nintendo's Auth service from the id token). Our server just doesn't check. |

NPLN services the game links (all gRPC method paths are in `main`, found by string search): `auth.v1.Auth`
(`IssueToken`, `RefreshToken`, `CreateUser`, `ListUsers`, `DeleteUser`, `IssuePrearrangedUserToken`,
`IssueAnonymousUserToken`, `RefreshAnonymousUserToken`, `ValidateToken`), `auth.v1.UserService` (`GetUser`,
`QueryUserExternalIds`, `QueryUserShortIds`), `appconfig.v1.Appconfig` (`FindApplicationConfig`,
`PullModifiedApplicationConfig`), `friends.v1.Friends` (`ActivateUser`, `ListFriendUsers`, `ListBlockingUsers`,
`SubscribeFriendUsers`), `gamesync.v1.Gamesync` (`IssueToken`, `RefreshToken`, `KeepUserSession`, `GetDocument`,
`ListDocuments`, `ReadDocuments`, `WriteDocuments`, `Begin/Commit/RollbackTransaction`, `CreateRound`,
`QueryCollectionIds`), `matchmaking.v1.GameSessionService` (16 methods, §3.3), `matchmaking.v1.Matchmaker`
(`CreateMatchmakingTicket`, `TrackMatchmakingTicket`, `CancelMatchmakingTicket`, `CreateAcceptance`),
`leaderboard.v1.LeaderboardService` (19 methods), `ugcstore.v1.Ugcstore` (10 methods).

The protobuf code is **lite runtime** (`proto/<svc>/v1/*.pb.cc`, no descriptors, no field names in the binary). Field
numbers must come from kinnay's hand-decompiled `NPLN-Protocols` (`latest/`) [K-NP] and be checked against this
binary's `_InternalParse` / `_InternalSerialize` (NPLN SDK 1.26.1).

---

## 1. How OpenSmash emulates Nintendo online (NEX) for Smash

Source: OpenSmash-Ultimate at commit `657a4d06`. Paths below are relative to its repository root.

### 1.1 Where it intercepts

**SDK symbol hooks, not IPC.** OpenSmash replaces SDK functions by mangled name (`HLE(_ZN2nn...)`) and game
functions by address (`HLE_WRAP(71xxxxxxxx)`, checked by a SHA-256 of the first 16 words):

- `runtime/hle_net.c:41-72`: `nn::nifm::*` (network available, IP config from the virtual LAN).
- `runtime/hle_net.c:1052-1068`: `connect()` of a stream socket goes to `online_stream_connect` (the game server);
  `hle_net.c:1095-1112`: `sendto()` gives datagrams to `online_nex_datagram` first (NAT check, in-process server).
- `runtime/hle_net.c:1364-1365`: `nn::socket::GetAddrInfo` always fails (EAI_FAIL); names are resolved by the hooks
  below instead.
- `runtime/hle_env.c:247-341`: `nn::account::*` (one user; NSA available when netplay is configured; NSA id = XOR of
  the user id halves, `hle_env.c:292`; **fake id token** `"ssbu-port.1.<nsa id hex>.<name>"`,
  `hle_env.c:320-327`; async contexts always done, `hle_env.c:332-340`; country from `SSBU_ONLINE_COUNTRY`,
  `hle_env.c:310-319`).
- `runtime/hle_env.c:386`: `nn::nsd::ResolveEx` returns the name unchanged.
- `runtime/hle_offline.c:55-91`: `nn::bcat` (no data), `nn::friends` (nobody), `hle_offline.c:151-152`:
  `nnsslInitialize/Finalize` OK. No real SSL is needed because of the direct-address trick below.
- `runtime/hle_env.c:343-344`: `pctl` free communication allowed.

### 1.2 How the game finds the server

- Smash logs in to `https://g23380901-lp1.s.n.srv.nintendo.net/` with **PRUDP Lite inside a WebSocket inside TLS**
  (`runtime/hle_online.c:1-17`).
- NEX has an unused **direct-address path** in its `NgsFacade` (host `nn::nex::String` at +0x240, port at +0x268). The
  hook on NEX's login job `HLE_WRAP(7100249190)` writes `10.13.0.1:60000` there (`hle_online.c:105-125`), so NEX
  opens the same WebSocket **without TLS**.
- `HLE_WRAP(71001becd0)` (NEX `InetAddress` from a name) and `HLE_WRAP(71000ba850)` (Pia's resolver) map any name
  containing `nintendo.net` to the server address; `nncs2` gets `x.x.255.254` because Pia's NAT check needs two
  addresses (`hle_online.c:127-160`).
- All of that is **Smash-specific** (function addresses, struct offsets).

### 1.3 PRUDP, keys, logins (`runtime/net_nex_core.c`, `net_nex.h`)

- **PRUDP Lite only** (stream framing `0x80`, 12-byte header, `net_nex_core.c:393-442`), over raw TCP or WebSocket
  frames. PRUDP V1 over UDP is not served (`net_nex_core.c:1410`). Lite connection signatures are HMAC-MD5 with
  **random per-start keys** (`net_nex_core.c:669`, keys drawn at `:1206-1212`): Lite does not use the game's NEX
  access key, so **OpenSmash never needed Smash's access key**. The game server id appears once, `0x23380901`, in the
  NAT Traversal relay-key answer (`net_nex_match.c:868-876`).
- Ports: auth 60000, secure 60001, NAT check 10025/10125, Pia keepalive 33335 (`net_nex.h:22-28`).
- Login: Ticket Granting 10/6 `ValidateAndRequestTicketWithParam` parses the fake token and derives the PID with
  FNV-1a (`net_nex_core.c:806-856`, `:332`); 10/3 `RequestTicket` (`:858-871`). Kerberos-style tickets: RC4 +
  HMAC-MD5, ticket key = MD5(server key ‖ random) (`:215-247`, `:714-791`). Secure URL
  `prudps:/address=10.13.0.1;port=60001;...` (`:711`).

### 1.4 NEX protocols implemented

Dispatch: `net_nex_core.c:1156-1193` and `net_nex_match.c:879-925`, `net_nex_ranked.c:328-340`.

| Protocol | Methods |
|---|---|
| 10 Ticket Granting | 3, 6 |
| 11 Secure Connection | 1 Register, 3 RequestURLs, 4 RegisterEx, 6 UpdateURLs, 7 ReplaceURL, 8 (empty) |
| 110 Utility | 7, 9, 10, 11 |
| 115 DataStore | 8 GetMeta, 21, 60/61, 82/84 (Smash's GSP "fighter powers"), 95, plus a list answered "no reply" |
| 124 Screening | 1 |
| 109 Matchmake Extension | 1, 2, 8, 11, 12, 34, 36, 37, 38 Create, 39 Join, 40 AutoMatchmake, 41/49, 44, 58; Battle Arenas 51, 52, 55, 56 |
| 21 Match Making | 2, 41, 42, 44 |
| 50 Match Making Ext | 1, 2 |
| 3 NAT Traversal | 3 ProbeExt, 4, 5, 6 relay key, 7 |
| 120 Matchmake Referee (ranked) | 1-4, 8-11 |

- Gatherings: a `NexStore` interface (`net_nex.h:86-115`) so they can live locally (`nex_store_local`) or in a shared
  backend. New session: 32 random bytes as the **Pia session key** (`net_nex_match.c:270-281`); joins notify everyone
  (Pia connects its mesh to the station URLs in those notifications, `net_nex_match.c:283-286`).
- **Pia NAT check responder** (`net_nex_core.c:1387-1411`): 16-byte big-endian requests, types 101/102 to nncs1 and
  103 to nncs2; the answer echoes the sender's own (virtual) address and port, so every player looks "open". Pia
  keepalives (`32 AB 98 64` on 33335) are recorded.

### 1.5 After matchmaking (Pia)

Pia traffic runs between the players' virtual addresses on OpenSmash's virtual LAN, the same network Local Wireless
uses, relayed by the OpenSmash lobby (`docs/MULTIPLAYER.md`, "Online mode"). Pia's encryption is untouched: the key is
the session key the NEX server handed out, both games derive the same AES key, and nobody needs to decrypt.

### 1.6 Where the server runs, and how much is Smash-specific

- Normally the NEX server is `tools/nex` (Python) inside the **online lobby** (a dedicated server,
  `tools/ssbu_online_lobby.py` in the lobby's own source, not in this repository). The C port `net_nex_*.c` runs **in the game** only with
  `SSBU_NEX_LOCAL=1`, for tests (`hle_online.c:15-17`, `:50-58`). So OpenSmash itself is **not** a "host is the server"
  design for real play.
- Generic: PRUDP Lite, RMC encoding, Kerberos tickets, Secure Connection, Matchmake Extension core, NAT check.
- Smash-specific: every `HLE_WRAP` address and struct offset, the token format, DataStore 82/84, Battle Arenas,
  ranked Elo, the `0x23380901` id.

---

## 2. What Jamboree needs instead

### 2.1 Interception point in OpenJamboree

OpenJamboree HLEs Horizon **services over IPC** (`runtime/svc_core.c:1200-1221`); unknown services get a generic
"everything succeeds, zeros out" stub (`svc_core.c:30-36`). It can also replace SDK functions reached through the
PLT dispatch table (`runtime/kernel.c:565-576`). NPLN, gRPC and OpenSSL are **statically linked into `main`**, so their
internals are direct calls and cannot be hooked that way; but everything they ask the system goes through IPC or
through SDK imports, which we can answer.

What the game asks, and the current state:

| Need | Used for | Current OpenJamboree | Needed |
|---|---|---|---|
| `nifm` | "connected" | `svc_core.c:666-687`, `online=1` ini gate (work in progress) | keep |
| `acc` `IManagerForApplication` | `EnsureNetworkServiceAccountAvailable`, `GetNetworkServiceAccountId`, **`EnsureNetworkServiceAccountIdTokenCacheAsync` + `LoadNetworkServiceAccountIdTokenCache`** (the NSA id token NPLN's `NsaIdTokenRetriever` sends to `Auth/IssueToken`) | `svc_core.c:548-553`: only cmd 0 (CheckAvailability, returns 0 = unavailable) and 1 | CheckAvailability true; cmd 2 `EnsureIdTokenCacheAsync` returning a done `IAsyncContext`; cmd 3 `LoadIdTokenCache` returning a fake token (any string; OpenSmash-style `ojam.1.<nsa hex>.<nick>`); stable NSA id per PC |
| `time` | `AsyncEnsureNetworkClockAvailabilityTask`, `StandardNetworkSystemClock` | check trace | network clock "accurate", task finishes OK |
| `nsd` (`nsd:u`) | `GetEnvironmentIdentifier` (→ `lp1`), `ResolveEx` (Pia's `%` hosts) | **not registered**, so the generic stub returns an empty environment and empty FQDN | `nsd:u`/`nsd:a`: cmd 11 GetEnvironmentIdentifier `"lp1"`, cmd 21 ResolveEx (copy FQDN, `%` → `lp1`) [SB] |
| DNS | gRPC's native resolver and libcurl call `nn::socket::GetAddrInfo` / `nnsocketGetAddrInfo` / `nnsocketGetHostByNameWithOptions` (all PLT imports of `main`) | `sfdnsres` not registered | Either HLE those SDK imports via the dispatch table (simplest, as OpenSmash does), or implement `sfdnsres` GetAddrInfoRequest(WithOptions). Any `*.nintendo.net` → host's ZeroTier IP |
| `bsd` | gRPC's iomgr: **`RecvMsg`/`SendMsg`** (imports `nnsocketRecvMsg`/`SendMsg`), **`EventFd`** (`nn::socket::EventFd`, `EventFdRead/Write`), `Poll`, non-blocking connect | `runtime/lan.c:539-684`: cmds 29/30 (RecvMsg/SendMsg) return EOPNOTSUPP (`lan.c:681`); EventFd (cmd 31) falls to "not handled" (`lan.c:683`) | Implement RecvMsg/SendMsg (iovec + msghdr, FreeBSD layout), EventFd (a socketpair or an event with poll support), and a port remap for NPLN (§3.1) |
| `ssl` | (a) gRPC's OpenSSL loads root CAs through `nn::ssl::BuiltInManager::GetBuiltInCertificates` (file `external/openssl/nnsdk/nnsdk_LoadBuiltinCerts.cpp`); (b) libcurl uses `nnssl*` | `svc_core.c:1001-1003`: quiet stub | `ISslService` cmd 3 GetCertificateBufSize / cmd 2 GetCertificates [SB] return **our own CA** (DER) for every requested id. For curl's `ISslConnection`: either real TLS or pass-through (we are the "system", so `Read/Write` can go straight to the socket in plaintext if the peer is our server) |
| `friend` | `GetFriendList` (NSA ids), invitations | `svc_core.c:689-696` | Return the other PCs' NSA ids as friends (they are the host's players), or leave empty if the server ignores the friend filter (§3.3) |
| `pctl` | `IsFreeCommunicationAvailable` | `svc_core.c:648-661`, cmd 1018 → 1 | keep |

Note: `runtime/svc_core.c:666-667` (uncommitted work in the tree) currently has a raw newline inside a string literal
(`"... (online=1)` / `");`), which will not compile.

### 2.2 TLS: how to be trusted

The gRPC channel is TLS 1.2/1.3 with **OpenSSL inside the game** (`external/openssl/...` strings, `SSL_CTX_new`).
Its trust store comes from the system through `nn::ssl::BuiltInManager::GetBuiltInCertificates`
(imported by `main`). Since we are the system:

1. Generate a private CA once per install (or ship one; it only needs to be trusted by our own runtimes).
2. Return its DER from `ISslService::GetCertificates` for every `CaCertificateId` asked.
3. The host's server presents a leaf for `*.lp1.t.npln.srv.nintendo.net` (+ `gamesync.npln.nintendo.net`)
   signed by that CA, with ALPN `h2`.

Fallback if the game also pins something: kinnay's `NPLN-Protocols/generate_patch.py` disables NPLN's certificate
verification with an IPS patch [K-NP]. In a recompiled build the same effect needs a recompiler-side function
override of the verify callback; prefer the CA route.

### 2.3 How to discover the remaining per-game values

The values that matter for NPLN are the tenant (found), the matchmaking/search **config names** the game sends
(e.g. `QueryGameSessionsRequest.game_session_search_config`, `GameSessionCreationTicket.matchmaking_config`) and the
`properties` keys. They are strings built at runtime and are easiest to get from **the game's own requests**: log
every decoded RPC on the host server (step 4 of §4). The throwaway scripts used for this spec (not committed, they
read the dump): `nso.py` (NSO → flat image, LZ4 segments), `strs.py` (string search), `xref.py` / `blx.py` (ADRP+ADD
and BL references), `plt.py` (PLT stub → import name), `da.py` (capstone disassembly).

Pia per-game keys: Pia 6.34 over NPLN takes its session crypto from what matchmaking gives it (NEX did the same with
the gathering's session key). kinnay documents the NPLN **nonce** (4-byte network id + 8-byte header nonce, Pia
6.16-6.30) but **not** the NPLN session-key derivation [K-PP]; Jamboree is not in the Pia game-key table [K-GK]. As long
as the key is derived from data our server hands out (both peers run the same Pia code), we never need it: we only
carry Pia's UDP.

---

## 3. Design: the host's game is the server

### 3.1 Network layout

```
 joiner PC (ZeroTier 10.x.y.B)                        host PC (ZeroTier 10.x.y.A)
 game --TLS/h2/gRPC--> bsd connect(A, 443) ---------> [host runtime] NPLN server: TLS + HTTP/2 + gRPC
      --UDP nncs1/2 (10025/10125)-------------------> [host runtime] Pia NAT check responder
      <==== Pia UDP (game's port), direct over ZeroTier ====> host game's Pia
 host game itself talks to its own server through loopback (127.0.0.1 or A)
```

- **Name resolution**: every `*.nintendo.net` name resolves to the host's ZeroTier IP (`online_host=` in the `.ini`).
  On the host, it resolves to its own IP. `nncs2` needs a second address (Pia compares the two answers): use a second
  IP on the host's adapter, or make the joiner's runtime answer nncs2 locally (no packet leaves the PC; the answer only
  has to echo the sender's own address, as OpenSmash's does).
- **Ports**: the game connects to 443. Windows lets a normal user bind 443, but it may be taken, so the runtime's
  `bsd` Connect (`lan.c:624`) should remap `(host IP, 443)` → `(host IP, online_port=, default 47443)`. NAT check:
  UDP 10025/10125, or remapped the same way in SendTo (`lan.c` cmd 11).
- **Pia**: P2P UDP between ZeroTier IPs. ICE: `AllocateIceServerSet` returns no TURN and a STUN server = the host's
  NAT-check-style responder (or none), so Pia uses host candidates, which are directly reachable on ZeroTier.

### 3.2 Server stack inside the host runtime

1. **TLS server**: mbedTLS (Apache-2.0, AGPL-compatible) or Windows SChannel. One thread, `select()` over the
   listening socket, like `lan.c` already does for ldn_mitm.
2. **HTTP/2**: SETTINGS, HEADERS + CONTINUATION with HPACK (Huffman decode needed), DATA, WINDOW_UPDATE, PING,
   RST_STREAM, GOAWAY; server-streaming responses (`TrackGameSessionCreationTicket`, `TrackMatchmakingTicket`,
   `SubscribeFriendUsers`). nghttp2 (MIT) would save most of this.
3. **gRPC framing**: 5-byte length prefix, `content-type: application/grpc`, trailers `grpc-status`/`grpc-message`;
   errors as status codes (+ `grpc-status-details-bin` with `nn.npln.errdetails.NError` if the game needs details).
4. **protobuf**: a hand-written varint/length-delimited codec, one encoder/decoder per message (field numbers from
   [K-NP], checked against `main`).
5. **State**: users (NSA id → `tenants/t-adf89f68/users/<id>`), tokens (opaque random strings; the client treats the
   access token as a JWT only if it parses it, unverified here), game sessions, tickets. Kept in memory; the host is the
   only authority.

### 3.3 Minimal RPC set (first target: private session with friends)

Field numbers below are from kinnay's `latest/proto/matchmaking/v1` [K-NP] and must be re-checked for NPLN 1.26.1.

| Step | RPCs | Server behaviour |
|---|---|---|
| Login | `Auth/ListUsers`, `Auth/CreateUser`, `Auth/IssueToken`, `Auth/RefreshToken`, `Auth/IssuePrearrangedUserToken` (guests / sub-users: `SubUser%d UserContext Create(PrearrangedUser)` at `main+0x16d9ac`) | Accept any `external_id_token.nsa_id_token`; one user per NSA id; `access_token` + `refresh_token` + `ttl` 28800 [K-AS]. |
| App config | `Appconfig/FindApplicationConfig` | Unknown payload; first answer empty/default and see what the game needs. |
| Gamesync | `Gamesync/IssueToken`, `KeepUserSession`, `GetDocument`/`ListDocuments`/`ReadDocuments`/`WriteDocuments`, transactions | Probably the player's online profile/records. Start with an in-memory document store per user; may need empty-but-valid documents. Unknown. |
| Friends | `Friends/ActivateUser`, `ListFriendUsers`, `SubscribeFriendUsers` (stream) | Everyone on this host is everyone's friend. |
| Create | `GameSessionService/CreateGameSessionCreationTicket` (parent=1, ticket=2, tokens=3), `TrackGameSessionCreationTicket` (stream; ticket state SUCCEEDED + `game_session`) | Make a `GameSession` (name=1, max=2, current=3, can_participate=4, is_public=5, password=6, state=7 ACTIVE, host=8, port=9, create_time=10, properties=11 MapValue, user_sessions=12). |
| Code for friends | `CreateGameSessionShortAlias`, `GetGameSessionShortAlias` (`GetNplnShortGameSessionID`) | A short code; with one host there is only one live session anyway. |
| Find / join | `QueryGameSessions` (tenant=1, search config=3, min_vacancy=4, properties=6, users=7), `GetGameSession`, `BatchGetGameSessions`, `JoinGameSession` (name=1, password=2, user_definitions=3 → matched_user_sessions=1, game_session=2), `SyncGameSession` (keepalive) | Return the host's open sessions, ignoring friend filters. |
| Random (worldwide) | `Matchmaker/CreateMatchmakingTicket`, `TrackMatchmakingTicket` (stream), `CancelMatchmakingTicket`, `CreateAcceptance` | Match whoever on this host is searching; this is what "Todo o mundo" needs. |
| Pia | `AllocateIceServerSet` (tenant=1, user=2 → `IceServerSet{stun_server=2, turn_servers=3, ttl=4}`), `ListLatencyMeasurementServers` (empty), `IssueMatchmakingIdToken`, `IssueUserDelegationToken` | Minimal or empty. |
| Not needed at first | Leaderboard, Ugcstore, Monitoring (`g2122d301...p.srv`) | Fail cleanly (gRPC UNAVAILABLE) and see if the game tolerates it. |

### 3.4 How Pia finds the other players (the main unknown)

`UserSession` has no address fields; a `GameSession` has one `host` (8) and `port` (9) [K-NP]. Pia 6.34's NPLN
plugin has `NplndRelayClient`, `AttachMeshJob`, `WaitSetupRelayAddress`, `ResultTurnServerProcessTimeout` and WebRTC
STUN types. Two possibilities, to be settled by reverse engineering `nn::pia::nplnd::AttachMeshJob` and
`NplndRelayClient` and by logging what the host game writes into the session (`properties`, `UserDefinition.attributes`):

1. Peers publish their Pia station info in session/user properties and connect directly (ICE with host candidates).
   Then the server only stores and echoes what the game writes. Easy.
2. `GameSession.host:port` is a **Nintendo relay/rendezvous server** that Pia attaches to (`NplndRelayClient`). Then
   the host runtime must also emulate that relay protocol (undocumented). Substantial extra work.

### 3.5 Risks

| Risk | Note |
|---|---|
| Wrong assumption "NEX" | Settled: NPLN. Plan for gRPC/HTTP2/TLS, not PRUDP. |
| NSO membership | Not checked client-side (no licence API imported); Nintendo's Auth checks the id token. Our Auth accepts anything. |
| Token validation | The client may parse the access token (ES256 JWT per [K-AS]) for `exp`/`sub`. If so, emit a JWT with an arbitrary signature; the client cannot verify Nintendo's key unless it fetches one (`IssuePublicKey` exists in GameSessionService). |
| TLS trust | Depends on `GetBuiltInCertificates` being the only trust source. If not, need a verify override. |
| Pia encryption | Key source for NPLN undocumented [K-PP]. Fine if derived from server-provided data; a problem only if it needs a server-side secret both peers must get identically (then we must find which field and fill it). |
| Pia relay / NAT traversal | §3.4. NAT check is easy (OpenSmash's responder). TURN/relay emulation would be hard. |
| Unknown schemas | `appconfig`, `gamesync`, `friends` payloads are not in [K-NP] matchmaking; lite protobuf means hand RE of `_InternalParse`. |
| Game data from the server | Online Koopathlon / ranked features may read server documents (gamesync) or leaderboards; empty answers might block menus. |
| gRPC socket API | `RecvMsg`/`SendMsg`/`EventFd` must be exact (FreeBSD `msghdr`, eventfd semantics) or gRPC stalls silently. |
| Version skew | All peers must run the same game version (the server can enforce it with a property). |

---

## 4. Effort and step order

Rough, one person, full time; the uncertainty is mostly in steps 5-6.

| # | Step | Effort | Reuse |
|---|---|---|---|
| 1 | **Trace pass**: `online=1`; implement `nsd:u`, acc id-token cmds, network clock, DNS → a local address; a plain TCP listener on 443 that logs the TLS ClientHello (SNI, ALPN). Confirms the endpoint list and that the game reaches the network. | 2-3 days | OpenSmash `hle_env.c:292-340` (token/NSA idea) |
| 2 | **bsd for gRPC**: RecvMsg/SendMsg, EventFd, port remap. | 2-3 days | - |
| 3 | **TLS + CA**: `ISslService` GetCertificates/GetCertificateBufSize with our CA; mbedTLS server; see the first HTTP/2 preface. | 3-4 days | - |
| 4 | **HTTP/2 + gRPC + protobuf** skeleton that logs every call (path, decoded fields as raw tag/value) and answers UNIMPLEMENTED. This is the discovery tool for everything after. | 4-6 days (2-3 with nghttp2) | - |
| 5 | **Auth, appconfig, gamesync, friends** until the game reaches the online menu. | 1-2 weeks | - |
| 6 | **Matchmaking** (private session + short alias first, then Matchmaker for worldwide), and the Pia question of §3.4 (RE `AttachMeshJob`/`NplndRelayClient`). | 2-4 weeks | OpenSmash's gathering model (`net_nex.h:35-115`, `net_nex_match.c` join/leave/notify logic, as design reference) |
| 7 | **Pia traffic**: NAT check responder, STUN-less ICE, host migration, 2 then 4 PCs. | 1-2 weeks | OpenSmash `net_nex_core.c:1387-1411` (NAT check), directly adaptable |
| 8 | UI/ini (`online=1`, `online_host=`, `online_port=`), docs, tests with two instances on one PC (127.0.0.1 / 127.0.0.2, like `lan_peers=`). | 3-5 days | `lan.c` adapter/peers code |

Total: about **6-10 weeks**, with a real chance that step 6 finds a relay protocol (§3.4 case 2) that adds more.

Licensing: OpenSmash-Ultimate is GPL-3.0-or-later (`LICENSE`, `NOTICE.md`), compatible with OpenJamboree's AGPL-3.0.
Code adapted from it (the NAT check responder, any encoding helpers) must keep its copyright/licence header and say
where it came from (file and commit `657a4d06`). Field numbers from kinnay's NPLN-Protocols are facts; check that
repo's licence before copying `.proto` files verbatim.

---

## 5. Status (2026-11-28)

Tested with two copies of the game on one PC (`_work/online_pair.ps1`: host + a second copy with
`online_host=127.0.0.1`, `online_user=amigo`), all in background mode.

What works, in order:

1. Auth (CreateUser, IssueToken, IssuePrearrangedUserToken) and Ugcstore (GetDocument, CommitDocuments) on the host's
   built-in server (`runtime/online.c` + `runtime/npln.c`).
2. Access tokens are ES256-shaped JWTs whose payload carries `sub` (user id) and `npln.tid`; the server reads the
   caller back from `sub`. With plain-string tokens the game could not find itself in the matchmaking result and
   failed with `GetResultNplnSessionRpc` = 9.
3. Matchmaking: the game opens its own room (`CreateGameSessionCreationTicket`, config `BqP1MatchmakingConfigs`, with
   room properties `_Bex_PropIsHost`, `gameMode`, `_Bex_PropIsVoiceChat`) and then searches
   (`CreateMatchmakingTicket`, `BqP4MatchmakingConfigs`, user attribute `gameMode`). The host groups searchers into
   the room of whoever searched first; Track*Ticket are gRPC server streams. Players are told apart by the caller's
   token (requests only say `users/current`).
4. `nxloader.py` patches the 2.3.0 `main` so gRPC's `ssl_check_peer` accepts the target name of the post-match channel
   (`0xACA000`, `cbz w0` → `nop`). Without it the client dropped the connection right after the TLS handshake.

Where it stops: about 1 s after the match both games run bex "Disconnection Start". bex logs
`bex::NetworkModuleImpl ... result=12` (a fixed label for a failed join) with the stored join status
**14 (`UNAVAILABLE`)**. The only network activity in that second is one new TLS + HTTP/2 connection to the host
(`SETTINGS`, `WINDOW_UPDATE`, `PING`, never a `HEADERS`), so the call fails inside the client before it is sent.
Leading guess: a per-call credentials step (gRPC fails such calls as `UNAVAILABLE` without sending them), or a
"wait for the channel to be ready" with a short deadline. Next check: `SWR_TRACE_DYN=8a43310-8a43314,8006000-b000000`
with `SWR_DUMP_GRPC=1` dumps every `grpc_call_start_batch` (`0x8a43310` is `CoreCodegen::grpc_call_start_batch`,
found through `SWR_VTSLOT=9aa0048,108`) to see whether that call ever starts and with which metadata.

Not done yet: Gamesync itself (document store + the `KeepUserSession` bidirectional stream; bex reads user-session
documents with fields `uid`, `ussid`, `ucsid`, `st`, `att`, `ltc`, `tn`), Pia traffic between the players, and the
two-PC test over ZeroTier.

Debug switches added along the way (all off unless set):

| Variable | What it shows |
|---|---|
| `SWR_ONLINE_FRAMES=1` | every HTTP/2 frame received and sent by the built-in server |
| `SWR_ONLINE_BT=1` | the guest stack when the game shuts down a connection to the server (frame chain + stack scan) |
| `SWR_TRACE_DYN_MAX=n`, `SWR_TRACE_DYN_CHANGES=1` | more calls per traced target; log a polled function's return when it changes |
| `SWR_TRACE_DYN_STR=1` + `SWR_POKE_LOGLEVEL=9a8c7e0,3` | the game's own bex log (trace `vsnprintf` at `a8061e0-a8061e4` from `8100000-8600000`) |
| `SWR_BEX_BT=text` | stack, registers and the join status field when a bex message containing `text` is logged |
| `SWR_VTSLOT=got,off` | logs the target of a vtable slot once the object exists (to find indirect call targets) |

---

## References

- **[OS]** OpenSmash-Ultimate, https://github.com/OpenSmash-Ultimate/OpenSmash-Ultimate, commit `657a4d06` Files cited inline.
- **[K-NS]** kinnay, NintendoClients wiki, "NPLN Servers": https://github.com/kinnay/NintendoClients/wiki/NPLN-Servers
  (host pattern `<tenant>.lp1.t.npln.srv.nintendo.net`, tenant `t-<id>-lp1`, metadata, tenant list incl. Jamboree).
- **[K-AS]** kinnay, "Auth Service": https://github.com/kinnay/NintendoClients/wiki/Auth-Service
- **[K-NP]** kinnay, NPLN-Protocols (decompiled `.proto`, `latest/` by hand for lite-runtime builds):
  https://github.com/kinnay/NPLN-Protocols
- **[K-NR]** kinnay, "NPLN Resources": https://github.com/kinnay/NintendoClients/wiki/NPLN-Resources
- **[K-PO]** kinnay, "Pia Overview": https://github.com/kinnay/NintendoClients/wiki/Pia-Overview
- **[K-PP]** kinnay, "Pia Protocol": https://github.com/kinnay/NintendoClients/wiki/Pia-Protocol
- **[K-NC]** kinnay, "NAT Check Server": https://github.com/kinnay/NintendoClients/wiki/NAT-Check-Server
- **[K-GK]** kinnay, "Pia Game Keys": https://github.com/kinnay/NintendoClients/wiki/Pia-Game-Keys
- kinnay, "Game Server List" (NEX ids/access keys; Jamboree has none since it isn't NEX):
  https://github.com/kinnay/NintendoClients/wiki/Game-Server-List
- **[SB]** switchbrew, "SSL services", "NSD services", "Sockets services" (command ids to re-check before coding):
  https://switchbrew.org/wiki/SSL_services , https://switchbrew.org/wiki/NSD_services ,
  https://switchbrew.org/wiki/Sockets_services
- OpenJamboree `docs/LAN_SPEC.md`, `runtime/lan.c`, `runtime/svc_core.c`.
