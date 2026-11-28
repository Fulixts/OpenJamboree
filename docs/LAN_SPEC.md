# LAN play (LDN over a real LAN / ZeroTier): implementation spec

Status: research spec, written from source (no guessing where a source exists). Where the two references disagree, both are listed.

**References** (cloned read-only into `C:\Git\Claude\ldn-refs\`):

| Tag | Source | Commit |
|---|---|---|
| **R** | Ryubing/Ryujinx, `src/Ryujinx.HLE/HOS/Services/` (git.ryujinx.app/ryubing/ryujinx) | `eaff2d33` (2026-11-23) |
| **RC** | Ryujinx `src/Ryujinx.Common/Utilities/NetworkHelpers.cs` | same |
| **M** | spacemeowx2/ldn_mitm, `ldn_mitm/source/` | `9a2639ef` (2026-04-07) |
| **SB** | switchbrew.org/wiki/LDN_services and /Sockets_services; libnx `nx/source/services/bsd.c` | fetched 2026-11-26 |

`R:Ldn/UserServiceCreator/IUserLocalCommunicationService.cs:77` means file:line. `LDN/...` paths are under `Ldn/UserServiceCreator/`.

**Scope.** ldn_mitm mode only ("direct"): each emulated console uses its **real** LAN or ZeroTier IP. No relay server is involved. A PC running this mode works with Ryujinx in "ldn_mitm" mode and with real consoles running ldn_mitm, because they all speak the same protocol on port 11452.

All multi-byte fields are **little-endian** unless marked BE. All structs are packed exactly as listed (`#pragma pack(1)` plus explicit padding is the safe choice in C).

---

## 1. `ldn:u` and `IUserLocalCommunicationService`

### 1.1 Service creator `ldn:u` (`nn::ldn::detail::IUserServiceCreator`)

| Cmd | Name | In | Out | Notes |
|---|---|---|---|---|
| 0 | CreateUserLocalCommunicationService | - | object `IUserLocalCommunicationService` | One object per client. It holds all the state below. R:`Ldn/IUserServiceCreator.cs` |
| 1 | CreateClientProcessMonitor [18.0.0+] | - | object `IClientProcessMonitor` | Its cmd 0 is `RegisterClient(pid, u64 placeholder)`, which you can stub to return success. A 17.5 SDK game is not expected to call it. R:`LDN/IClientProcessMonitor.cs:9` |

ldn_mitm also adds cmd 65000 (`CreateLdnMitmConfigService`), which is not needed here (M:`interfaces/iservice.hpp`).

### 1.2 Commands of `IUserLocalCommunicationService`

"raw" is the CMIF input data, after the header, at the offsets shown. Buffer types: `0x19` = in, pointer (X) or auto-select; `0x1A` = out, recv-list (C) or auto-select; `0x21` = in, auto-select (A or X); `0x22` = out, auto-select (B or C); `0x09` = in pointer (X); `0x0A` = out pointer (C). **Auto-select** means the client may send either form, so read whichever descriptor is non-empty. The runtime's `buf_in`/`buf_out` helpers already do this.

Result codes (module 203 = 0xCB) are in §1.5. "IS" below means InvalidState (0x40CB).

| Cmd | Name | Input | Output | Behaviour (R line / M line) |
|---|---|---|---|---|
| 0 | GetState | - | u32 state | Returns the current `NetworkState`. Ryujinx returns 6 (Error) if Initialize failed. R:77. In M:`ldn_icommunication.cpp:73`, when 402 was called with a non-zero arg and a disconnect reason is set, it returns the Result `MAKERESULT(0x10, reason)`; optional. |
| 1 | GetNetworkInfo | - | buf 0x1A `NetworkInfo` (0x480) | Valid only in AccessPointCreated or StationConnected, otherwise IS. Zero-fill the buffer first. R:106-156, M:`lan_discovery.cpp:487` |
| 2 | GetIpv4Address | - | u32 ip, u32 mask (**host order**: 192.168.1.5 = 0xC0A80105) | In ldn_mitm mode this is the real IP and mask of the chosen interface. M uses nifm and `ntohl` (M:`ldn_icommunication.cpp:85-95`). R returns InvalidArgument unless AccessPointCreated or StationConnected, and falls back to 127.0.0.1/255.255.255.0 (R:174-226). |
| 3 | GetDisconnectReason | - | u32 (s16 value) | The last `DisconnectReason`. R:228 writes s16; M writes u32. Write a u32 to cover both. |
| 4 | GetSecurityParameter | - | 0x20 raw `SecurityParameter` | `{ ldn.SecurityParameter[16] (zero in both refs), networkId.SessionId[16] }`. IS if there is no network. R:249, M:`ldn_types.cpp` NetworkInfo2SecurityParameter |
| 5 | GetNetworkConfig | - | 0x20 raw `NetworkConfig` | Built from the current NetworkInfo: IntentId, Common.Channel, Ldn.NodeCountMax, Nodes[0].LocalCommunicationVersion. R:278 |
| 100 | AttachStateChangeEvent | - | copy handle (readable event) | Create the event once. M uses an auto-clear event (M:`ldn_icommunication.cpp:16`). Signal it on every state change and on every NetworkInfo or node change (§1.4). R:311 |
| 101 | GetNetworkInfoLatestUpdate ("AndHistory") | - | buf0 0x1A `NetworkInfo`; buf1 0x0A `NodeLatestUpdate[n]` | Copies NetworkInfo and, for i < n (max 8), the per-node change flags, **then clears them**. M:`lan_discovery.cpp:499-521`. **R bug:** R:348 writes the updates into buffer 0, not buffer 1, so they are lost. Use buffer index 1. |
| 102 | Scan | raw: +0 u16 channel (pad to 8); +8 `ScanFilter` (0x60). Total 0x68 | u16 count (written as u32/u64 in refs, low 16 bits used); buf 0x22 `NetworkInfo[]` | Broadcasts a LAN scan, waits, filters (§2.4) and fills up to `bufsize/0x480` entries (switchbrew: clamped 1..0x18). R:431 (`_state - 3 >= AccessPoint`) returns IS only in StationConnected or Error; a host in AccessPointCreated may scan. R:386-486, M:`lan_discovery.cpp:361` |
| 103 | ScanPrivate | same as 102 | same | Same as Scan. Both refs: treat as Scan (R) or stub returning 0 results (M). |
| 104 | SetWirelessControllerRestriction [5.0.0+] | u32 (0 or 1) | - | Return success if value ≤ 1 and state is Initialized, otherwise InvalidArgument or IS. No effect. R:488 |
| 105 | SetWirelessAudioPolicy [13.1.0+] | u32 | - | Stub success (SB). |
| 106 | SetProtocol [18.0.0+, works 20.0.0+] | u32 (1 or 3) | - | Stub success. R:512 |
| 200 | OpenAccessPoint | - | - | Initialized → AccessPoint. Closes any station first. Otherwise IS. R:530, M:608 |
| 201 | CloseAccessPoint | - | - | AccessPoint or AccessPointCreated → Initialized. Destroys the network, closing the TCP listener and dropping all stations. R:558, M:624 |
| 202 | CreateNetwork | raw: +0x00 `SecurityConfig` (0x44); +0x44 `UserConfig` (0x30); +0x74 pad 4; +0x78 `NetworkConfig` (0x20). Total 0x98 | - | Requires AccessPoint, otherwise IS. Checks NodeCountMax ≤ 8, LocalCommunicationVersion bit 31 clear, SecurityMode ≤ 1 (Retail) and passphrase size ≤ 64, else InvalidArgument (R:603-683). If `IntentId.LocalCommunicationId == -1`, substitute the NACP's `LocalCommunicationId[0]`. Builds NetworkInfo and starts the TCP server (§2.5). → AccessPointCreated, signal. |
| 203 | CreateNetworkPrivate | raw: SecurityConfig 0x44; SecurityParameter 0x20 @0x44; UserConfig 0x30 @0x64; pad 4 @0x94; NetworkConfig @0x98. buf 0x09 `AddressEntry[]` | - | Not used by ldn_mitm. Stub success (both refs). |
| 204 | DestroyNetwork | - | - | AccessPointCreated → AccessPoint. Closes the server and stations. Otherwise IS. R:685-719, M:588 |
| 205 | Reject | u32 node ip/id | - | IS unless AccessPointCreated. ldn_mitm: no-op success. |
| 206 | SetAdvertiseData | buf 0x21 (1..0x180 bytes) | - | Allowed in AccessPoint or AccessPointCreated (else IS); size 0 or > 0x180 gives InvalidArgument. Copies into `Ldn.AdvertiseData`/`AdvertiseDataSize`. **The data must survive the later CreateNetwork**, because games call this before 202. If hosting, re-push SyncNetwork to all stations. R:748, M:188-203 |
| 207 | SetStationAcceptPolicy | u8 policy (0..3) | - | Store it in `Ldn.StationAcceptPolicy`. Otherwise a no-op in both refs. |
| 208 | AddAcceptFilterEntry | 6-byte MAC | - | Stub success. |
| 209 | ClearAcceptFilter | - | - | Stub success. |
| 300 | OpenStation | - | - | Initialized → Station, otherwise IS. R:841, M:639 |
| 301 | CloseStation | - | - | Station or StationConnected → Initialized. Closes the TCP connection. R:872, M:655 |
| 302 | Connect | raw: +0x00 SecurityConfig (0x44); +0x44 UserConfig (0x30); +0x74 u32 LocalCommunicationVersion; +0x78 u32 option (0/1). Total 0x7C. buf 0x19 `NetworkInfo` (0x480) from a previous Scan | - | Requires Station, otherwise IS. Validates option ≤ 1, version ≤ 0x7FFF and passphrase ≤ 64. Then connects TCP to `Nodes[0].Ipv4Address:11452` and sends `Connect(NodeInfo)` (§2.5). M sleeps 1 s and returns 0 (M:670-708). R waits up to 4000 ms for the first SyncNetwork and returns ConnectTimeout (0x84CB) if none arrives (R:`LdnMitm/LanDiscovery.cs:523-557`). → StationConnected when the first SyncNetwork arrives. |
| 303 | ConnectPrivate | raw: SecurityConfig; SecurityParameter @0x44; UserConfig @0x64; u32 version @0x94; u32 option @0x98; pad @0x9C; NetworkConfig @0xA0 | - | Stub success (both refs). |
| 304 | Disconnect | - | - | StationConnected → Station. Closes TCP and sets reason DisconnectedByUser. Otherwise IS. R:1015-1052, M:599 |
| 400 | Initialize | raw: u64 pid_placeholder; pid descriptor | - | None → Initialized. Starts UDP on 11452 (§2.3). Fails with DeviceDisabled (0x2CCB) if there is no usable network (R:1162-1229). |
| 401 | Finalize | - | - | Tears down from any state (AP → destroy, Station → disconnect) → None. Closes the UDP socket. R:1061-1147, M:710 |
| 402 | InitializeWithVersion [7.0.0+] | raw: s32 version + u64 pid_placeholder; pid | - | Same as 400; ignore the arguments. Version is 1 on 7.0+ (SB). R:1149 misreads these as "two IPs" and ignores them; harmless. |
| 403 | SetOperationMode [19.0.0+] | - | - | Stub success. |
| 500-601 | ActionFrame and TxPower [18.0.0+] | - | - | Not used by 17.5 SDK games. Return success. |

### 1.3 Struct layouts

All checked against R `Ldn/Types/*.cs` and M `ldn_types.hpp`. M asserts `sizeof(NetworkInfo)==0x480`, `ConnectNetworkData==0x7C` and `ScanFilter==0x60` (M:`ldn_icommunication.cpp:5-7`).

**IntentId (0x10)**: `+0 s64 LocalCommunicationId; +8 u16 reserved; +0xA u16 SceneId; +0xC u32 reserved`

**SessionId (0x10)**: 16 random bytes (M stores them as two u64 values: high, then low).

**NetworkId (0x20)**: `+0 IntentId; +0x10 SessionId`

**Ssid (0x22)**: `+0 u8 Length; +1 char Name[33]`. ldn_mitm uses the fixed string `"12345678123456781234567812345678"` with length 32 (M:`lan_discovery.cpp:13`). R uses 32 random bytes (R:`LdnMitm/LanDiscovery.cs:103-107`).

**CommonNetworkInfo (0x30)**

| Off | Size | Field | LAN value |
|---|---|---|---|
| 0x00 | 6 | MacAddress (BSSID) | Fake: `02 00` followed by 4 IP bytes (see §2.6) |
| 0x06 | 0x22 | Ssid | see above |
| 0x28 | 2 | Channel (s16) | `NetworkConfig.Channel`, or 6 if that is 0 |
| 0x2A | 1 | LinkLevel (s8) | 3 |
| 0x2B | 1 | NetworkType (u8) | 2 (Ldn) |
| 0x2C | 4 | reserved | 0 |

**NodeInfo (0x40)**

| Off | Size | Field |
|---|---|---|
| 0x00 | 4 | Ipv4Address (u32, **host order**) |
| 0x04 | 6 | MacAddress |
| 0x0A | 1 | NodeId (0 = host, 1..7 = stations) |
| 0x0B | 1 | IsConnected (0/1) |
| 0x0C | 33 | UserName (NUL-terminated, from UserConfig) |
| 0x2D | 1 | reserved (Platform on 19.0.0+: 0 = NX) |
| 0x2E | 2 | LocalCommunicationVersion (s16) |
| 0x30 | 16 | reserved |

**LdnNetworkInfo (0x430)**

| Off | Size | Field |
|---|---|---|
| 0x000 | 16 | SecurityParameter (zero) |
| 0x010 | 2 | SecurityMode (u16) |
| 0x012 | 1 | StationAcceptPolicy (u8) |
| 0x013 | 1 | reserved (M: `_unk1[3]` covers 0x13..0x15) |
| 0x014 | 2 | reserved |
| 0x016 | 1 | NodeCountMax |
| 0x017 | 1 | NodeCount |
| 0x018 | 0x200 | NodeInfo Nodes[8] |
| 0x218 | 2 | reserved |
| 0x21A | 2 | AdvertiseDataSize |
| 0x21C | 0x180 | AdvertiseData |
| 0x39C | 0x8C | reserved |
| 0x428 | 8 | AuthenticationId (u64; R only, M treats it as reserved; send 0) |

**NetworkInfo (0x480)**: `+0x000 NetworkId (0x20); +0x020 CommonNetworkInfo (0x30); +0x050 LdnNetworkInfo (0x430)`.
So `Nodes[i]` sits at `0x68 + i*0x40`, AdvertiseDataSize at `0x26A` and AdvertiseData at `0x26C`.

**NetworkConfig (0x20, align 8)**: `+0 IntentId; +0x10 u16 Channel; +0x12 u8 NodeCountMax; +0x13 u8 reserved; +0x14 u16 LocalCommunicationVersion; +0x16 u8 reserved[10]`

**SecurityConfig (0x44)**: `+0 u16 SecurityMode (0 All, 1 Retail, 2 Debug); +2 u16 PassphraseSize; +4 u8 Passphrase[64]`

**SecurityParameter (0x20)**: `+0 u8 Data[16]; +0x10 SessionId`

**UserConfig (0x30)**: `+0 char UserName[33]; +0x21 u8 reserved[15]`

**ScanFilter (0x60, align 8)**: `+0 NetworkId; +0x20 u32 NetworkType; +0x24 MacAddress Bssid; +0x2A Ssid; +0x4C u8 reserved[16]; +0x5C u32 Flag`. R types Flag as a byte, which is the same thing in LE.

ScanFilterFlag bits: 0x01 LocalCommunicationId, 0x02 SessionId, 0x04 NetworkType, 0x08 MacAddress, 0x10 Ssid, 0x20 SceneId (R:`Types/ScanFilterFlag.cs`).

**NodeLatestUpdate (8)**: `+0 u8 StateChange (bit0 Connect, bit1 Disconnect; 3 = both); +1 u8 reserved[7]`

**AddressEntry (0xC)**: `u32 ipv4; u8 mac[6]; u16 reserved`. **AddressList (0x60)** = `AddressEntry[8]`.

**Enums**

| Enum | Values |
|---|---|
| NetworkState (u32) | 0 None, 1 Initialized, 2 AccessPoint, 3 AccessPointCreated, 4 Station, 5 StationConnected, 6 Error |
| DisconnectReason | 0 None, 1 DisconnectedByUser, 2 DisconnectedBySystem, 3 DestroyedByUser, 4 DestroyedBySystem, 5 Rejected, 6 SignalLost |
| AcceptPolicy (u8) | 0 AcceptAll, 1 RejectAll, 2 BlackList, 3 WhiteList |
| NetworkType (u32) | 0 None, 1 General, 2 Ldn, 3 All |

Constants (R:`Ldn/LdnConst.cs`): SsidLengthMax 32, AdvertiseDataSizeMax 0x180, UserNameBytesMax 32, NodeCountMax 8, StationCountMax 7, PassphraseLengthMax 64.

### 1.4 State machine and the state-change event

```
            400/402 Initialize                 401 Finalize (from any state)
  None ───────────────────────► Initialized ◄──────────────────────────────
                                 │      ▲  ▲
            200 OpenAccessPoint  │      │  │ 201 CloseAccessPoint
                                 ▼      │  │
                            AccessPoint ┘  │        300 OpenStation / 301 CloseStation
                     202 Create │  ▲ 204 Destroy                 Initialized ⇄ Station
                                ▼  │                                           │  ▲
                        AccessPointCreated               302 Connect + first   │  │ 304 Disconnect,
                                                         SyncNetwork received  ▼  │ host TCP closed
                                                                       StationConnected
  Error (6): reported when network init failed (no usable interface) or the UDP socket died (M:82-85 sets SignalLost + Error).
```

Rules from the refs:
- 200 and 300 are only legal from Initialized. 202 is legal from AccessPoint, 302 from Station.
- When the **host's TCP connection drops**, ldn_mitm moves the station StationConnected → Station (M:174-179). Ryujinx's LdnMitm client never notices, because `LdnProxyTcpClient` has no OnDisconnected handler: this is a gap in R. Recommended: go to Station, set DisconnectReason = SignalLost (6) (or DestroyedByUser (3) if you add a graceful close), and signal.
- When a **station's TCP drops**, the host clears that node: M sets `isConnected=0` (M:42-46); R zeroes the whole NodeInfo (R:`LanDiscovery.cs:171-197`). The host then recounts NodeCount, re-sends SyncNetwork to the remaining stations and signals.
- **Signal the event** on: every NetworkState change; every change of any `Nodes[i].IsConnected`, which also sets the NodeLatestUpdate bits (M:338-353, R:`Types/NodeLatestUpdate.cs`); and, for stations, every SyncNetwork whose contents changed (R:`LanDiscovery.cs:128-149`). Games call 101 after the event fires.
- NodeLatestUpdate bits accumulate (OR) per node until 101 reads and clears them. M resets them in createNetwork and connect (`initNodeStateChange`).

### 1.5 Result codes (`(desc << 9) | 0xCB`; R:`Ldn/ResultCode.cs`)

| Name | Desc | Value |
|---|---|---|
| DeviceNotAvailable | 16 | 0x20CB |
| DeviceDisabled | 22 | 0x2CCB |
| InvalidState | 32 | 0x40CB |
| NodeNotFound | 48 | 0x60CB |
| ConnectFailure | 64 | 0x80CB |
| ConnectNotFound | 65 | 0x82CB |
| ConnectTimeout | 66 | 0x84CB |
| ConnectRejected | 67 | 0x86CB |
| InvalidArgument | 96 | 0xC0CB |
| InvalidObject | 97 | 0xC2CB |
| VersionTooLow | 113 | 0xE2CB |
| VersionTooHigh | 114 | 0xE4CB |
| TooManyPlayers | 144 | 0x120CB |

How R maps network errors to these results (R:`LDN/Station.cs:64-80`): TCP connect fails → ConnectFailure; no TCP socket → ConnectNotFound; no SyncNetwork within 4 s → ConnectTimeout. The SDK shows an error applet for some Connect failures when option bit 0 is set (SB). Prefer InvalidState or a quiet failure over error-applet-range codes during bring-up.

---

## 2. The ldn_mitm LAN protocol

### 2.1 Overview

| Item | Value | Source |
|---|---|---|
| Port | **11452**, for both UDP (discovery) and TCP (session) | M:`lan_discovery.hpp:119`, R:`LanDiscovery.cs:18` |
| Magic | u32 **0x11451400**, LE, so the bytes on the wire are `00 14 45 11` | M:`lan_protocol.hpp:34`, R:`LanProtocol.cs:18` |
| Max packet | 2048 bytes (header + body, after compression) | M:`lan_protocol.hpp:33`, R:`LanProtocol.cs:20` |
| Who is host | The console in AccessPointCreated. Node 0 is always the host. | |
| Who assigns IPs | **Nobody.** Each node reports its own real IP in its NodeInfo. The host only assigns **node ids** (1..7). | M:523-542, R:465-476 |
| Keepalive / ping | None. Liveness is detected only by TCP close or error. | |

### 2.2 Packet header (12 bytes)

| Off | Size | Field |
|---|---|---|
| 0 | 4 | magic 0x11451400 |
| 4 | 1 | type (u8): 0 Scan, 1 ScanResp, 2 Connect, 3 SyncNetwork |
| 5 | 1 | compressed (0/1) |
| 6 | 2 | length: body size **on the wire** |
| 8 | 2 | decompress_length: body size after decompression (0 if not compressed) |
| 10 | 2 | reserved, 0 |

(M:`lan_protocol.hpp:5-10,23-30`, R:`LdnMitm/Types/LanPacketHeader.cs`.) The body follows immediately.

**Compression** (M:`lan_protocol.cpp:218-281`, R:`LanProtocol.cs:233-314`): a zero-run RLE.
- Encode: a non-zero byte is copied as is. A `0x00` byte is written as `00 n`, where `n` (0..255) is the number of **additional** zeros that follow it.
- Decode: for each byte, output it; if it is 0, read `n` and output `n` more zeros.
- The sender compresses when the body is non-empty and sets `compressed=1`, `decompress_length=original`, `length=compressed size`. ldn_mitm falls back to raw (compressed=0) when the output would be larger than the input; R only falls back at 2048. Receivers must accept both.
- A receiver must reject a packet whose decompressed size differs from decompress_length.
- A NetworkInfo is mostly zeros, so it shrinks from 0x480 to a few hundred bytes.

**Framing over TCP**: it is a stream, so accumulate bytes until you have 12 header bytes, then `12+length` bytes. Resynchronise by discarding the buffer on a bad magic or when length exceeds 2048 (M:68-105, R:`LanProtocol.cs:82-162`). Several packets can arrive in one read: loop.

**Body checks**: ScanResp and SyncNetwork bodies must be exactly 0x480 bytes; Connect must be exactly 0x40. M ignores or drops anything else (M:27,67,96).

### 2.3 Sockets each node opens

| When | Socket | Bind | Options |
|---|---|---|---|
| Initialize (400/402) | UDP | M: `0.0.0.0:11452`. R: `<local ip>:11452`, plus `<broadcast>:11452` on Linux and macOS (R:`LanDiscovery.cs:314-339`) | SO_BROADCAST, SO_REUSEADDR (M:238-258) |
| CreateNetwork (host) | TCP listener | `0.0.0.0:11452` (M:276-284), backlog 10; R binds `<local ip>:11452` | SO_REUSEADDR |
| Connect (station) | TCP client | ephemeral port → `host_ip:11452` | - |

Windows does not deliver broadcasts to a socket bound to the broadcast address, but it does deliver them to `0.0.0.0:11452` or `<local ip>:11452` (R comment at `LanDiscovery.cs:321-323`). **Use `0.0.0.0:11452` with SO_REUSEADDR** and filter by interface yourself.

### 2.4 Discovery (UDP)

1. The scanner (any state, typically Station) sends a header-only packet `type=Scan, length=0` to **`(ip | ~mask):11452`**, the subnet-directed broadcast of the chosen interface (M:`lan_protocol.cpp:205-213` with `getBroadcast` M:130-141; R:`LanProtocol.cs:164-167`, `LanDiscovery.cs:45-52`).
2. Every host in **AccessPointCreated** answers with `type=ScanResp`, body = its full current `NetworkInfo` (0x480, compressed), **unicast to the scan's source address and port** (M:54-63, R:`LanProtocol.cs:56-61`). Non-hosts ignore Scan.
3. The scanner collects ScanResp packets keyed by `Common.MacAddress` (BSSID) (M:70; R:`Proxy/LdnProxyUdpServer.cs:85-97`).
   - M waits a fixed **1 s** (M:369).
   - R waits until the first response (1 s max), then 33 ms more, merges in the previous scan's results, and rate-limits scans to 1 per second (R:`LdnProxyUdpServer.cs:99-155`).
4. Then filter with the ScanFilter flags: LocalCommunicationId, SessionId, NetworkType, Ssid, SceneId. MacAddress is unsupported (R throws). See M:378-394, R:`LanDiscovery.cs:352-394`. R also drops entries whose `Nodes[0].UserName` is empty.
5. R drops packets whose source IP equals its own address (R:`LanProtocol.cs:84-87`), so a node never sees its own broadcast.

### 2.5 Session (TCP)

**Host side: CreateNetwork** (M:544-586, R:`LanDiscovery.cs:478-503`)
- Start the TCP listener and fill in Common: fake MAC, Ssid, channel (config or 6), LinkLevel 3, NetworkType 2.
- Set every `Nodes[i].NodeId = i` with `IsConnected = 0`.
- `Ldn.NodeCountMax = cfg.NodeCountMax`, `Ldn.SecurityMode = sec.SecurityMode`, `NetworkId.IntentId = cfg.IntentId`, `NetworkId.SessionId = 16 random bytes`.
- Fill `Nodes[0]` with the host's own IP, MAC, UserName and LocalCommunicationVersion, plus `IsConnected = 1`. `NodeCount = 1`.
- Keep AdvertiseData if it was already set. State → AccessPointCreated; signal.

**Station side: Connect** (M:670-708, R:`LanDiscovery.cs:523-557`)
1. Fail if `info.Ldn.NodeCount == 0`.
2. TCP connect to `Nodes[0].Ipv4Address` (host order → htonl) on port 11452.
3. Send `type=Connect` with body = the station's own `NodeInfo`: its IP (host order), fake MAC, `IsConnected=1`, UserName from UserConfig, LocalCommunicationVersion. NodeId does not matter, because the host overwrites it.
4. Wait for the first SyncNetwork: R waits ≤ 4 s, M sleeps 1 s and returns success regardless.

**Host on accept** (M:87-123,151-172,17-40; R:`LanDiscovery.cs:151-169`, `Proxy/LdnProxyTcpSession.cs:65-81`)
1. Allocate the first free station slot (node ids **1..7**, lowest first). Close the socket if full.
2. When that socket's `Connect` packet arrives, copy the received NodeInfo **verbatim** into `Nodes[slot]`, then force `NodeId = slot` and `IsConnected = 1`.
3. Call **updateNodes**:
   - `NodeCount = 1 + connected stations`;
   - re-run the overrideInfo step that sets NodeId and IsConnected for every slot;
   - send `type=SyncNetwork` (full NetworkInfo) to **every** connected station;
   - recompute the node-change bits and signal the host's own event (M:423-444, R:428-463).

**Pushes**: the host calls updateNodes (a full SyncNetwork to all stations) on every join, leave and SetAdvertiseData.
- R only pushes on SetAdvertiseData when Nodes[0] is connected (R:199-221). R's comment warns that otherwise games report "SessionKeepFailed or MasterDisconnected".
- There is no delta format: every update is the whole 0x480 struct.

**Station on SyncNetwork**: replace its NetworkInfo copy. On the first one: Station → StationConnected. On every one, recompute the node-change bits; signal if they changed (M:143-149, 181-186) or if anything changed (R).

**Disconnect**
- A station leaving (CloseStation, Disconnect, Finalize) just closes its TCP socket. The host sees EOF (M treats `recv()==0` as close, M:`lan_protocol.cpp:184-190`), clears the node and re-syncs.
- The host leaving (DestroyNetwork, CloseAccessPoint, Finalize) closes the listener and all station sockets (M:588-597). Each station sees EOF and goes back to Station (M:174-179).
- No disconnect message exists on the wire.

### 2.6 Addresses, MAC and what the game sees

- **IP encoding.** `NodeInfo.Ipv4Address`, `GetIpv4Address` and the station's connect target are all **host-order u32** values. M takes the nifm address (network order) and applies `ntohl` (M:530; `ldn_icommunication.cpp:89`). R uses `ReadUInt32BigEndian(addressBytes)` (RC:74-77). Example: 10.147.17.5 = 0x0A931105, stored LE as `05 11 93 0A`.
- **Fake MAC** = `02:00` followed by 4 IP bytes. R uses the network byte order, `02 00 a b c d` (R:`LanDiscovery.cs:245-255`). M does `memcpy` of the `ntohl` value on LE, giving `02 00 d c b a` (M:224-236). Both are valid because the MAC is only an identifier, used as the scan-result key and the BSSID. Use R's form.
- **Which addresses the game uses**: games read `GetNetworkInfo().Ldn.Nodes[i].Ipv4Address` and `GetIpv4Address()`, then open **ordinary BSD sockets** (bsd:u), mostly UDP, sometimes with subnet broadcast, to those IPs. Under ldn_mitm these are the **real LAN or ZeroTier IPs and the real subnet mask**, so the game's traffic goes straight between the PCs and consoles on the host network stack. There is no tunnelling in this mode (R `LdnMitmClient.Config` is a zero `ProxyConfig`, so R:195-210 reports the interface IP and mask). On real hardware without ldn_mitm, LDN would use a 169.254.x.x Wi-Fi subnet. With ldn_mitm, every node must sit in the **same IPv4 subnet** so that `ip|~mask` broadcasts reach everyone.
- The game's own ports and protocol (Jamboree likely uses Nintendo's Pia library; unverified) are not part of the ldn_mitm protocol. Trace the bsd calls to learn them (§4).

### 2.7 "Direct" (ldn_mitm) vs "proxy" (RyuLDN) in Ryujinx

| | LdnMitm (direct) | LdnRyu (proxy) |
|---|---|---|
| ldn backend | `LdnMitmClient` → `LanDiscovery` (UDP/TCP 11452 on the LAN) | `LdnMasterProxyClient` to a RyuLDN server (R:`LDN/IUserLocalCommunicationService.cs:1181-1201`) |
| `GetIpv4Address` | real interface IP and mask (ProxyIp == 0) | server-assigned virtual `ProxyIp`/`ProxySubnetMask` (R:212-218) |
| Game sockets | `DefaultSocket`, the host OS socket (R:`Sockets/Bsd/Proxy/SocketHelpers.cs:62-82`) | After `SocketHelpers.RegisterProxy`, every new AF_INET TCP or UDP socket is an `LdnProxySocket`. Its traffic is wrapped in `ProxyDataHeader{ProxyInfo{srcIp,srcPort,dstIp,dstPort,proto}, len}` and relayed through the server or a P2P host. Broadcast is `ip|~mask` of the virtual subnet (R:`LdnRyu/Proxy/LdnProxy.cs:25-52,232-240`, `Types/ProxyInfo.cs`). |
| Interop with consoles | yes (ldn_mitm consoles) | no (Ryujinx only) |

Proxy mode is only needed for NAT or Internet play. ZeroTier already provides a flat L2 network, so **implement direct mode**.

---

## 3. BSD sockets (`bsd:u` / `bsd:s`, `nn::socket::sf::IClient`)

### 3.1 Conventions

- The IPC result is **always 0**. The answer is in the output raw data: `s32 ret, u32 errno`, followed by command-specific extras. `ret = -1` on error. The errno values are **Linux** numbers (SB: "errno macros come from Linux, not FreeBSD"; R:`IClient.cs:33-47`).
- Everything else is **FreeBSD ABI**: socket types and flags, `SOL_SOCKET` and option numbers, MSG_* flags, sockaddr, fd_set, pollfd. Translate every one of them to Winsock values.
- On a non-blocking socket, a call that would block returns `-1` with errno `EAGAIN` (11).

| errno (Linux) | value | | errno | value |
|---|---|---|---|---|
| EBADF | 9 | | EADDRINUSE | 98 |
| EAGAIN / EWOULDBLOCK | 11 | | ECONNRESET | 104 |
| EINVAL | 22 | | EISCONN | 106 |
| ENOPROTOOPT | 92 | | ENOTCONN | 107 |
| EOPNOTSUPP | 95 | | ETIMEDOUT | 110 |
| ECONNREFUSED | 111 | | EALREADY | 114 |
| EINPROGRESS | 115 | | EHOSTUNREACH | 113 |

The Winsock→Linux mapping table is in R:`Sockets/Bsd/Impl/WinSockHelper.cs:10-90`. Map `WSAEWOULDBLOCK` to **EINPROGRESS** on a non-blocking `connect`, and to EAGAIN everywhere else (R:`Impl/ManagedSocket.cs:161-163`).

**sockaddr_in (0x10, FreeBSD style)** (R:`Types/BsdSockAddr.cs`)

| Off | Size | Field |
|---|---|---|
| 0 | 1 | sin_len (0x10; R writes 0, which games accept) |
| 1 | 1 | sin_family (AF_INET = 2) |
| 2 | 2 | sin_port (**BE**) |
| 4 | 4 | sin_addr (**BE**, network-order bytes) |
| 8 | 8 | zero |

Winsock's `sockaddr_in` has a **u16** `sin_family` at offset 0, so always rebuild the struct when translating; never cast it.

**Constants** (FreeBSD values the guest uses):

| What | Values |
|---|---|
| domain | AF_INET 2 (AF_INET6 28; reject) |
| type | SOCK_STREAM 1, SOCK_DGRAM 2, SOCK_RAW 3, SOCK_SEQPACKET 5; flags in the top bits: SOCK_CLOEXEC `0x10000000`, SOCK_NONBLOCK `0x20000000` (R:`Types/BsdSocketCreationFlags.cs`, `IClient.cs` SocketInternal) |
| protocol | 0 → TCP for STREAM, UDP for DGRAM; IPPROTO_TCP 6, IPPROTO_UDP 17 |
| levels | SOL_SOCKET `0xFFFF`, IPPROTO_IP 0, IPPROTO_TCP 6 |
| SOL_SOCKET options | SO_REUSEADDR 0x4, SO_KEEPALIVE 0x8, SO_BROADCAST 0x20, SO_LINGER 0x80, SO_REUSEPORT 0x200, SO_SNDBUF 0x1001, SO_RCVBUF 0x1002, SO_SNDTIMEO 0x1005, SO_RCVTIMEO 0x1006, SO_ERROR 0x1007, SO_TYPE 0x1008 |
| IP options | IP_TOS 3, IP_TTL 4, IP_MULTICAST_IF 9, IP_MULTICAST_TTL 10, IP_MULTICAST_LOOP 11, IP_ADD_MEMBERSHIP 12, IP_DROP_MEMBERSHIP 13 |
| TCP options | TCP_NODELAY 1 |
| MSG flags | MSG_OOB 0x1, MSG_PEEK 0x2, MSG_DONTROUTE 0x4, MSG_WAITALL 0x40, **MSG_DONTWAIT 0x80**, MSG_NOSIGNAL 0x20000 |
| poll events | POLLIN 0x1, POLLPRI 0x2, POLLOUT 0x4, POLLERR 0x8, POLLHUP 0x10, POLLNVAL 0x20, POLLRDNORM 0x40, POLLRDBAND 0x80, POLLWRBAND 0x100 |
| fcntl | F_GETFL 3, F_SETFL 4, O_NONBLOCK **0x800** (the value R uses, `IClient.cs:883,888`; log the actual value the game passes) |
| ioctl | FIONREAD 0x4004667F, FIONBIO 0x8004667E, SIOCATMARK 0x40047307 |

(Option table: R:`Types/BsdSocketOption.cs`; flags: R:`Types/BsdSocketFlags.cs`; poll: R:`Types/PollEventTypeMask.cs`.)

Winsock differences that bite:
- The SO_* numbers match FreeBSD, and the IP_* numbers are the same on Winsock2.
- **SO_RCVTIMEO and SO_SNDTIMEO** take a 16-byte timeval `{s64 sec, s64 usec}` from the guest, but a DWORD in milliseconds on Windows. R passes the first 4 bytes through as an int, which is wrong (R:`Impl/ManagedSocket.cs:439,454`).
- **IP_ADD_MEMBERSHIP** takes an 8-byte `ip_mreq`, passed through byte-for-byte. R's int passthrough breaks it.
- Winsock has no `MSG_DONTWAIT`: make the socket non-blocking for that one call (R does this, `ManagedSocket.cs:293-331`). `MSG_WAITALL` is 0x8 on Windows.
- **WSAPoll uses different bit values** (POLLRDNORM 0x100, POLLWRNORM 0x10, ...): translate both ways, or use `select()`.

### 3.2 Commands (ids from SB; layouts from R `Sockets/Bsd/IClient.cs` and libnx `bsd.c`)

All "in" offsets are into the raw data. "out" always starts with `s32 ret, u32 errno`.

| Cmd | Name | In (raw / buffers) | Out (after ret, errno) | Notes |
|---|---|---|---|---|
| 0 | RegisterClient | `LibraryConfigData` (0x20: u32 version, tcp_tx, tcp_rx, tcp_tx_max, tcp_rx_max, udp_tx, udp_rx, sb_efficiency) + u64 pid_placeholder + u64 tmem_size; copy-handle tmem; pid | **only `u32 errno`** (R writes a single 0) | Close the transfer-memory handle. libnx defaults: tcp 0x8000/0x10000, max 0x40000, udp 0x2400/0xA500, sb 4. R:151 |
| 1 | StartMonitoring | u64 pid_placeholder; pid | (nothing) | Return success. R:181 |
| 2 | Socket | u32 domain, u32 type, u32 protocol | ret = fd | type carries the NONBLOCK and CLOEXEC flag bits; bsd:u rejects RAW and SEQPACKET except ICMP. R:`IClient.cs:67-131` |
| 3 | SocketExempt | same as 2 | same | Socket that is shut down at creation; rare. |
| 5 | Select | +0 s32 nfds; +8 timeval `{s64 sec @8, s64 usec @0x10}`; +0x18 bool is_null (libnx `BsdSelectTimeval`); bufs 0x21 ×3 readfds/writefds/exceptfds in, 0x22 ×3 out | ret = ready count | fd_set is FreeBSD-style: bit `fd` lives at byte `fd/8`, bit `fd%8` (LE word layout), 1024 bits. An empty buffer means NULL. `is_null` means block forever. **R bug:** R:233 reads the timeout as an int32 at +4, which is padding. |
| 6 | Poll | +0 s32 nfds; +4 s32 timeout_ms (-1 = infinite); buf 0x21 `pollfd[nfds]` | ret = count; buf 0x22 `pollfd[nfds]` with revents | `pollfd` = `{s32 fd; s16 events; s16 revents}` (8 bytes). Bad fd → EBADF in R (R:348-489); POSIX would set POLLNVAL instead. R polls in 50 ms slices so the thread can be interrupted. |
| 7 | Sysctl | | | Return -1/EOPNOTSUPP. |
| 8 | Recv | u32 fd, u32 flags; buf 0x22 data | ret = bytes | R:500 |
| 9 | RecvFrom | u32 fd, u32 flags; buf0 0x22 data, buf1 0x22 sockaddr (0x10) | ret = bytes, **u32 addrlen** | libnx reads addrlen from out+8, so write 0x10. R never writes addrlen (R:530-569). |
| 10 | Send | u32 fd, u32 flags; buf 0x21 data | ret = bytes | |
| 11 | SendTo | u32 fd, u32 flags; buf0 0x21 data, buf1 0x21 sockaddr | ret = bytes | Broadcast requires SO_BROADCAST, which the game sets. R:598 |
| 12 | Accept | u32 fd; buf 0x22 sockaddr | ret = new fd, u32 addrlen (0x10) | R:631-675 (`Write(0x10)` at 667) |
| 13 | Bind | u32 fd; buf 0x21 sockaddr | ret 0 | Pass 0.0.0.0 through unchanged; R comments that the guest must still receive loopback traffic (R:`Proxy/DefaultSocket.cs:62-68`). |
| 14 | Connect | u32 fd; buf 0x21 sockaddr | ret 0 | Non-blocking → -1/EINPROGRESS. |
| 15 | GetPeerName | u32 fd; buf 0x22 sockaddr | ret 0, u32 addrlen | ENOTCONN if not connected. R writes the output twice (R:744 then 749), which is a bug: write once. |
| 16 | GetSockName | u32 fd; buf 0x22 sockaddr | ret 0, u32 addrlen | Same double-write bug in R:770. |
| 17 | GetSockOpt | u32 fd, s32 level, s32 optname; buf 0x22 optval | ret 0, u32 optlen | R:776 |
| 18 | Listen | u32 fd, s32 backlog | ret 0 | |
| 19 | Ioctl | u32 fd, u32 request, u32 bufcount; up to 4 × 0x21 in, 4 × 0x22 out | ret | Implement FIONREAD and FIONBIO; SIOCATMARK → 0; anything else EOPNOTSUPP (R:825-863). |
| 20 | Fcntl | u32 fd, u32 cmd, u32 arg | ret = flags | Only F_GETFL and F_SETFL with O_NONBLOCK (SB, R:864-899). |
| 21 | SetSockOpt | u32 fd, s32 level, s32 optname; buf 0x21 optval | ret 0 | See the Winsock caveats in §3.1. Unknown options: return 0 and log, rather than failing the game. |
| 22 | Shutdown | u32 fd, u32 how (0..2) | ret 0 | |
| 23 | ShutdownAllSockets | u32 how | ret 0 | |
| 24 | Write | u32 fd; buf 0x21 | ret = bytes | Equivalent to send with flags 0. |
| 25 | Read | u32 fd; buf 0x22 | ret = bytes | Equivalent to recv. |
| 26 | Close | u32 fd | ret 0 | EBADF for an unknown fd. |
| 27 | DuplicateSocket | u32 fd, u64 | | bsd:s only. |
| 29 / 30 | RecvMMsg / SendMMsg | R:1061 / 1106 | | Unlikely to be used; return EOPNOTSUPP until a trace shows otherwise. |
| 31 | EventFd [7.0.0+] | u32 flags, pad 4, u64 initval | ret = fd | R:1147 |

Fds are small per-client integers chosen by you (R `BsdContext.RegisterFileDescriptor`). Keep your own table from guest fd to host SOCKET; never expose Windows SOCKET values, which are large.

### 3.3 How R maps guest sockets to host sockets

- Each guest socket is one host socket (`ManagedSocket` → `DefaultSocket`) with the same type and protocol; non-blocking state is mirrored (R:`Impl/ManagedSocket.cs`).
- **Interface pinning**: if the user picked a LAN interface, R binds an unbound socket to `<that ip>:0` before the first `connect`, `send`, `sendto` or `recv` (R:`Proxy/DefaultSocket.cs:47-55`). An explicit guest `bind(0.0.0.0:p)` is honoured as is. This is what makes broadcasts and outgoing traffic leave through ZeroTier rather than the default route.
- Broadcast and multicast are not special-cased in direct mode: the guest's SO_BROADCAST, IP_MULTICAST_* and IP_ADD_MEMBERSHIP calls go straight through to the host.
- Blocking calls block the calling guest thread. R logs a warning that blocking mode is "not yet working properly" (R:`ManagedSocket.cs:251-254`).

### 3.4 nifm during LDN

- The ldn service itself is what talks to nifm on a real console. ldn_mitm switches nifm to local-network mode and sets MTU 1500 (M:`lan_discovery.cpp:747-815`, `ipinfo.cpp`). In a C runtime that does nothing, so you do not need it.
- Make these nifm calls succeed and report the **same interface** that LDN uses:

| Call | Answer |
|---|---|
| IGeneralService 4 CreateRequest → IRequest | 0 GetRequestState → **3 (Available)**; 1 GetResult → 0; 2 GetSystemEventReadableHandles → 2 events, first signalled; 3 Cancel, 4 Submit, 11 SetConnectionConfirmationOption → 0 (R:`Nifm/StaticService/IRequest.cs`; RequestState: 1 Error, 2 OnHold, 3 Available) |
| 12 GetCurrentIpAddress | `IpV4Address` u32 in **network byte order** (the raw address bytes; R:`Nifm/StaticService/Types/IpV4Address.cs`). Note: **not** the host order that LDN uses. |
| 15 GetCurrentIpConfigInfo | `IpAddressSetting` (0xD: u8 dhcp, ip, mask, gateway; NBO) + `DnsSetting` (9: u8 dyn, dns1, dns2) (R:`IGeneralService.cs:113-130`) |
| 18 GetInternetConnectionStatus | `{u8 type=1 WiFi, u8 strength=3, u8 state=4 Connected}`. Only report this if you want the game to believe it is online; LDN does not need it. |
| 5 GetCurrentNetworkProfile | Optional (0x17C buffer). |

The runtime currently returns `RC(110,300)` for nifm 5 and 12 (`runtime/svc_core.c:670`). Change 12 and 15 to return the LDN interface once LAN is in place.

---

## 4. Implementation plan for the C runtime

### 4.1 Order of work

1. **Trace first.** Keep `net_trace` (`runtime/svc_core.c:1146`) and add output-side logging. Walk through Jamboree's local-wireless menu as host and as client, and record the exact ldn command order, and the bsd sockets, ports, options and broadcast addresses it uses after connecting. Expected order: 402/400 → 100 → 300/200 → (206) → 202 or 102… → 302 → 1/101 on each event.
2. **Interface selection** (a `net_iface` module): pick one IPv4 interface and report its IP, mask and broadcast address. Every component uses the same answer: ldn, nifm and the bsd pinning. Details in §4.3.
3. **ldn state machine with a loopback "solo" backend.** Commands 0-5, 100, 101, 200-209, 300-304, 400-402 with real state transitions and the event. CreateNetwork builds NetworkInfo locally; Scan returns 0 results. Goal: the game reaches its "waiting for players" screen as host with no error applet.
4. **Packet codec**: header, RLE compress and decompress, TCP stream reassembly. Unit-test the codec against hex captured from Ryujinx or ldn_mitm (a ScanResp is a good fixture) before any networking.
5. **Discovery**: a UDP socket on `0.0.0.0:11452` (SO_REUSEADDR, SO_BROADCAST), owned by a **dedicated network thread** that answers Scan while hosting. Scan = send a broadcast, collect replies for ~1 s, filter, return. At this point a Ryujinx (ldn_mitm mode) or console host should appear in the game's room list.
6. **Station connect**: TCP connect, send Connect, process SyncNetwork, reach StationConnected, signal; handle EOF (go back to Station, reason SignalLost). Test by joining a Ryujinx host.
7. **Host session**: TCP listener, slots 1..7, apply Connect, updateNodes and broadcast SyncNetwork; on station EOF, clear the node and re-sync. Test with a Ryujinx station.
8. **bsd:u for game traffic**: an fd table, then Socket, Bind, Connect, Send/SendTo, Recv/RecvFrom, Poll/Select, SetSockOpt/GetSockOpt, Fcntl, Ioctl(FIONREAD/FIONBIO), Close, GetSockName, Shutdown, with FreeBSD↔Winsock translation and Linux errno.
9. **nifm**: answers consistent with step 2 (§3.4).
10. Two PCs over ZeroTier, then mixed with Ryujinx, then with an ldn_mitm console if one is available.

### 4.2 What can be stubbed (return success, no effect)

LDN: 103, 104, 105, 106, 203, 205, 207 (store only), 208, 209, 303, 403, 500-601; ldn:u 1 with IClientProcessMonitor 0.

BSD: 1, 3, 4 (EOPNOTSUPP), 7, 23, 27-30, 31 (implement later if traced).

SecurityConfig passphrases are never checked by ldn_mitm; accept any.

### 4.3 Pitfalls

- **Struct packing.** Use `_Static_assert(sizeof(...))` for every struct in §1.3: 0x480, 0x430, 0x40, 0x30, 0x20, 0x44, 0x60. In particular, CreateNetwork has a 4-byte pad before NetworkConfig (raw +0x74). Scan's ScanFilter starts at raw +8, not +2.
- **Three byte orders for IPv4**:
  - LDN structs and `GetIpv4Address` use host-order u32;
  - nifm `IpV4Address` and `sin_addr` are network order;
  - `sin_port` is BE.
  Write one helper for each conversion and nothing else.
- **Compression**: a zero byte at the end of the input still emits `00 n`. When decompressing, check the length against decompress_length and against 2048. Never trust `length` without bounding it to 2048 − 12.
- **TCP stream**: one recv can hold several packets or a partial one. ldn_mitm handles only one per poll (M:100-104), which is why you must loop.
- **Timeouts**:
  - Scan blocks the calling guest thread for about 1 s on every implementation. That is expected, but do it without holding any global runtime lock.
  - Connect: wait ≤ 4 s for SyncNetwork, as R does, then return ConnectTimeout (0x84CB).
  - Use a TCP connect timeout of about 3 s; a non-blocking connect plus select is enough.
  - There is no keepalive. Optionally set SO_KEEPALIVE on the 11452 TCP sockets so a dead peer is noticed on cable pull.
- **Threads and events**: socket I/O for 11452 runs on one network thread (select over the UDP socket, the listener and up to 7 station sockets; M uses 100 ms poll plus 10 ms sleep, M:446-485). That thread updates NetworkInfo under a mutex and signals the guest KEvent. IPC handlers only read state, copy and return.
- **Interface selection with several adapters** (Ethernet, Wi-Fi, ZeroTier, Hyper-V/WSL vEthernet, VPNs):
  - R's auto choice takes the **first non-loopback IPv4 adapter that has a gateway and DNS** (RC:10-67). ZeroTier adapters usually have neither, so the auto choice **skips ZeroTier**. You need an explicit setting, for example `LanInterface=10.147.17.0/24` or an adapter name in `SwitchRecomp.ini`.
  - Recommended default: if one adapter's description contains "ZeroTier", prefer it; otherwise use the adapter of the default route. Log the choice, and show it in the menu.
  - Enumerate with `GetAdaptersAddresses` (it gives the prefix length, so the mask). Skip loopback, link-local 169.254/16 and down adapters.
  - Use that IP for everything: the NodeInfo IP, the fake MAC, GetIpv4Address, nifm, and the broadcast `ip|~mask`.
  - Pin **outgoing** game sockets by binding to `<ip>:0` before connect or sendto when the guest did not bind, as R does (§3.3).
  - Leave `0.0.0.0` binds alone. For the 11452 sockets, ignore datagrams whose source is not in the chosen subnet, and ignore your own IP (R:`LanProtocol.cs:84`).
- **Broadcast on Windows**:
  - Send to the **subnet-directed** address (e.g. 10.147.17.255), never 255.255.255.255, because the limited broadcast leaves only through the default-route adapter.
  - Receive on a socket bound to `0.0.0.0:port` or `<ip>:port`, not to the broadcast address.
  - ZeroTier networks must have "Enable Broadcast" on, which is the default.
- **Windows UDP ICMP quirk**: after sending to a closed port, the next `recvfrom` fails with WSAECONNRESET. Disable this with `WSAIoctl(SIO_UDP_CONNRESET, FALSE)` on every UDP socket, the guest's included. R does not do this.
- **Firewall**: inbound UDP and TCP 11452, plus the game's own ports, must be allowed. Windows marks ZeroTier networks as *Public* by default, which blocks them. Document how to set the network to Private, or add a rule for the exe.
- **Same subnet requirement**: all peers must be in one IPv4 subnet so broadcasts and the reported node IPs work. NodeInfo IPs are self-reported, so a peer that picked the wrong adapter advertises an unreachable IP, and the symptom is "room visible, join times out".
- **Two instances on one PC**: both need port 11452, so SO_REUSEADDR is required, and Windows will then deliver unicast replies to only one of them. Test with two machines, or two VMs on ZeroTier.
- **LocalCommunicationId**: Jamboree may pass -1 in IntentId. Substitute the NACP's `LocalCommunicationId[0]` (R:613-619) so that hosts and stations match, and ldn_mitm consoles in particular, since the scan filter compares this id.
- **Version and name fields**: copy `LocalCommunicationVersion` and the UserName exactly. Games reject a mismatched version (VersionTooLow/High) and show the host's `Nodes[0].UserName` in the lobby; R drops hosts with an empty name from scan results.
- **AdvertiseData** carries the game's room state. Push a SyncNetwork on every SetAdvertiseData while hosting, or stations will drop with session-keep errors (R comment at `LanDiscovery.cs:211`).
- **Known Ryujinx bugs not to copy**: 101 writes NodeLatestUpdate to the wrong buffer (R:348); Select timeout offset (R:233); GetSockName and GetPeerName double output (R:744/770); RecvFrom missing addrlen; SO_RCVTIMEO and ip_mreq passthrough (R:`ManagedSocket.cs:439`); a station never detects host loss in LdnMitm mode.
