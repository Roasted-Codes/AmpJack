# Native Discord voice on a modded Xbox 360: feasibility

Research date: 2026-11-22. Target: RGH console, retail kernel 17559, DashLaunch 3.21
(`sockpatch=true`, `liveblock=true`, `livestrong=false`, `xhttp=false`), Aurora, homebrew built
with OpenXeChain (clang 20, ppc32 SVR4 ABI, Newlib, soft-float, no .bss; see `docs/INTERNALS.md`).

Question: can the 360 itself run a barebones Discord voice client, signed in to each player's
own normal Discord account, with no companion app, PC, phone relay or per-user bot, ideally
reachable from the guide and still running while a game plays?

Short answer: **the Discord side has become much harder since March 2026 and the account-risk
side is a real go/no-go problem.** Every Discord voice call now requires the client to implement
DAVE end-to-end encryption, which is an MLS (RFC 9420) group-key client on top of the normal
gateway/voice/UDP stack. The only browser-free login that fits a TV device is the undocumented
QR "remote auth" flow. Both the login and the client itself sit outside what Discord permits.
The 360 platform work (TLS, sockets from a resident plugin, Opus, CPU/memory) is the easier half.

Legend: **[V]** verified against a cited source during this research; **[A]** assumption,
inference or community lore, not re-verified; **[X]** verified from the XDK/kernel knowledge
already captured in this repo (`docs/INTERNALS.md`) or from xenia's reimplementation.

---

## 1. What a minimal voice client must implement today

### 1.1 Gateway (control plane)

**[V]** From Discord's gateway reference (docs.discord.com/developers/events/gateway, read
2026-11-22):

- URL `wss://gateway.discord.gg/?v=10&encoding=json`; `encoding` is `json` or `etf`; optional
  `compress=zlib-stream` or `zstd-stream` (transport compression). Per-payload zlib compression
  is a separate, optional `compress: true` in Identify. **Compression is optional**, so a C
  client can skip zlib/zstd entirely and take plain JSON text frames.
- Handshake: receive Hello (op 10, `heartbeat_interval`), send Identify (op 2: `token`,
  `properties{os,browser,device}`, bots also `intents`), heartbeat (op 1) with last sequence
  number, Heartbeat ACK (op 11), Resume (op 6) with `session_id` + `resume_gateway_url` + `seq`.
- Outbound payloads max 4096 bytes; 120 events / 60 s per connection.
- Get Gateway (`GET /gateway`) needs no auth; Hello arrives before Identify, so **a TLS +
  WebSocket + Hello + heartbeat test needs no account at all** (useful for milestone 1).

**[V]** For user accounts (unofficial but the de facto reference, docs.discord.food/topics/gateway,
read 2026-11-22): "User accounts do not require intents and their usage is not recommended";
user accounts have `max_concurrency` 1, max 200 guilds, max 50 sessions; identify carries a
`capabilities` bitfield instead of intents. **[A]** Real user clients send detailed
"super properties" (`os`, `browser`, `client_build_number`, …); third-party clients mimic a
desktop client here. Community lore says implausible properties get accounts flagged; I found
no authoritative statement either way.

**[A]** A user account's READY event contains the user's guilds, channels, relationships, read
states and settings and can be hundreds of KB to several MB for a busy account. A 360 client
must parse it as a stream and discard almost all of it (keep guild ids/names, voice channel
ids/names, own user id). Not confirmed from a primary source, but every third-party user client
(Abaddon, mautrix-discord) deals with this.

Joining a channel: send Voice State Update (op 4: `guild_id`, `channel_id`, `self_mute`,
`self_deaf`), then receive `VOICE_STATE_UPDATE` (`session_id`) and `VOICE_SERVER_UPDATE`
(`token`, `endpoint`). **[V]** Official voice-connections page.

### 1.2 Voice gateway

**[V]** docs.discord.com/developers/topics/voice-connections, read 2026-11-22:

- Versions: v8 recommended (server-side message buffering, `seq_ack` in heartbeats), v4–v7
  available, **v1–v3 and the "no version" default were discontinued 2024-11-18**.
- Opcodes: 0 Identify, 1 Select Protocol, 2 Ready, 3 Heartbeat, 4 Session Description,
  5 Speaking, 6 Heartbeat ACK, 7 Resume, 8 Hello, 9 Resumed, 15 Media Sink Wants,
  21–31 DAVE (below). Binary frames: optional big-endian uint16 sequence, 1-byte opcode, payload.
- Identify (op 0) carries `server_id`, `user_id`, `session_id`, `token`, and
  `max_dave_protocol_version` ("0 or omitted means no DAVE support").
- Ready (op 2) gives `ssrc`, `ip`, `port`, `modes`.
- Select Protocol (op 1) once per session (re-sending closes with 4006); Session Description
  (op 4) returns `mode`, `secret_key`, `dave_protocol_version`.

### 1.3 UDP media

**[V]** Same page:

- IP discovery: 74-byte packet, big-endian: type (2, `0x1` request / `0x2` response), length (2,
  value 70), SSRC (4), address (64, NUL-terminated), port (2).
- RTP header: `0x80`, payload type `0x78`, sequence (2), timestamp (4), SSRC (4); then the
  encrypted payload.
- Opus, **48 kHz, 2 channels** per the docs (mono Opus frames decode fine; frame size not
  specified on the page, 20 ms is the de facto standard). Send five silence frames
  (`F8 FF FE`) before going quiet.
- Transport encryption modes **as of now**: `aead_aes256_gcm_rtpsize` (preferred, may be absent
  depending on server hardware) and `aead_xchacha20_poly1305_rtpsize` (**"You must support"**).
  `xsalsa20_poly1305`, `_suffix`, `_lite`, `_lite_rtpsize` and plain `aead_aes256_gcm` were
  **discontinued 2024-11-18**. AEAD modes append a 32-bit incrementing nonce to the packet; the
  `_rtpsize` variants treat CSRCs and the extension preamble as unencrypted header (SRTP style).
- Speaking (op 5) must be sent before audio or the server drops it.

Implication for the 360: AES-256-GCM and XChaCha20-Poly1305 are both needed. mbedTLS provides
AES-GCM and ChaCha20-Poly1305; **XChaCha** needs a ~50-line HChaCha20 subkey derivation on top
(**[A]** standard construction, trivial).

### 1.4 DAVE end-to-end encryption: mandatory

**[V]** Timeline, from Discord's own posts:

- Blog "Bringing DAVE to All Discord Platforms", 2025-09-02: "Starting March 1st 2026, clients
  and apps without DAVE support will no longer be able to participate in Discord calls." Also
  "we're bringing DAVE support to all our remaining platforms, including browsers, consoles and
  our Social SDK"; the Social SDK "comes with built-in DAVE support".
- Blog "Every Voice and Video Call on Discord Is Now End-to-End Encrypted", 2026-05-18: "we
  required all clients to support DAVE before joining a call" and "it will not be possible to
  fall back to unencrypted connections"; E2EE "is now standard for every voice and video call on
  Discord, outside of stage channels" (stage channels keep transport encryption only). Secondary
  press (Help Net Security 2026-05-19, 9to5mac 2026-05-20) agrees; one outlet gives 2026-03-02.
- The voice-connections page still describes a "transition" that can downgrade to protocol
  version 0 when a non-DAVE client joins, but the blogs above say that fallback is gone.
  **[A]** Treat a client with `max_dave_protocol_version: 0` as unable to join any DM/group/guild
  voice call today; it would at best be accepted into a stage channel. Only an experiment can
  show exactly how the voice gateway refuses (close code vs. silent no-audio).

**[V]** What DAVE is (daveprotocol.com whitepaper + voice-connections page):

- MLS group per call. Ciphersuite: "DHKEMP256_AES128GCM_SHA256_P256 (MLS ciphersuite 2)";
  identity = ECDSA P-256 key with basic credential = 64-bit user id; keys may be ephemeral per
  call or persistent.
- The voice gateway is the MLS delivery service, authentication service and the only
  **external sender** (op 25); it sends Add/Remove proposals (op 27). **Members must build the
  commit and welcome themselves** (op 28); the gateway picks the first valid commit per epoch
  (op 29) and relays Welcome (op 30). Key packages (op 26) are single-use. Transition handling:
  op 21/24 announce, op 23 ready, op 22 execute. Invalid commit/welcome recovery via op 31.
- Media: per-sender 16-byte secret from MLS-Exporter (label "Discord Secure Frames v0"),
  AES-128-GCM with the tag truncated to 8 bytes, 32-bit nonce (ULEB128 in the payload) with a
  ratchet generation in its high byte, frame magic `0xFAFA`. Transport encryption still applies
  on top.

So a DAVE client is a **real MLS member**: TreeKEM (path update on every commit it authors),
HPKE DHKEM(P-256)+HKDF-SHA256+AES-128-GCM, the MLS key schedule, tls-syntax codec, KeyPackage /
Welcome / Commit / GroupInfo processing, plus the frame ratchet. Protocol version 1 is the only
non-zero version so far, but clients "must stay backward-compatible with all non-discontinued
versions".

**[V]** What libdave needs (github.com/discord/libdave, cpp/README, mlspp README):

- C++ (plus a JS build); MIT. The C++ library depends on **cisco/mlspp** (C++17, STL,
  **OpenSSL ≥ 1.1.1 / 3.x or BoringSSL**, nlohmann::json; built via vcpkg) and is what Discord's
  native clients use.
- Other implementations: `@snazzah/davey` (Rust/OpenMLS via NAPI, used by `@discordjs/voice`,
  which says it is "the only supported DAVE protocol library" there), `pydave` on PyPI
  (Python), Go and Elixir ports. All of them assume a hosted OS with OpenSSL/Rust std.

**[A]** Porting libdave to OpenXeChain means porting mlspp **and OpenSSL** to Newlib/ppc32
soft-float with no .bss; nobody has done either on the 360 as far as I can find. The realistic
route is a bespoke, minimal MLS member in C on top of mbedTLS (which already has P-256 ECDH /
ECDSA, HKDF-SHA256, AES-GCM; an HPKE layer must be written). The MLS RFC test vectors
(mlswg/mls-implementations) help with the primitives, but the only way to test DAVE end to end
is against the live voice gateway. This is the single biggest piece of work in the project.

### 1.5 Minimal client checklist

TLS 1.2 client · DNS · TCP · WebSocket client (RFC 6455 masking, ping/pong, close) · streaming
JSON parser/emitter · gateway state machine (identify/heartbeat/resume/op 4) · voice gateway v8
state machine · UDP, IP discovery, RTP · AES-256-GCM + XChaCha20-Poly1305 transport
· Opus encode/decode 48 kHz · DAVE: MLS member (ciphersuite 2) + AES-128-GCM frame crypto with
ratchet · remote-auth login (RSA-2048-OAEP, QR render) · token storage · UI.

---

## 2. Authentication without a browser

### 2.1 Password login: not realistic

**[V]** (docs.discord.food/authentication and the CAPTCHA page, unofficial but current):
`POST /auth/login` takes `login` + `password`; the response can demand a CAPTCHA
(`captcha_service: "hcaptcha"`, dynamic `captcha_sitekey`), a "new login location" e-mail/SMS
verification (`POST /auth/authorize-ip`), and MFA (`ticket` → `POST /auth/mfa/totp|sms|webauthn`).
The docs warn that Discord "does not take kindly to spam in their authentication endpoints" and
that suspicious logins lock the account behind a password reset.

An hCaptcha challenge needs a JavaScript-capable browser; a 360 has none. New-location checks
mean a fresh console IP will usually trigger e-mail verification. **Not viable as the primary
path.**

### 2.2 Remote auth (QR login): the only plausible path

**[V]** Protocol (docs.discord.food/remote-authentication/desktop, read 2026-11-22; community
implementations: mautrix-discord `remoteauth`, teaishealthy/discord_remote_auth, abaddon):

1. WebSocket to `wss://remote-auth-gateway.discord.gg/?v=2` **with `Origin: https://discord.com`**
   (connection rejected otherwise).
2. Server `hello` (`heartbeat_interval`, `timeout_ms`, example ~142 s). Client generates an
   **RSA-2048 OAEP (SHA-256) keypair** and sends `init` with the SPKI public key (base64).
3. Server `nonce_proof` (RSA-encrypted nonce) → client decrypts and replies with its SHA-256 →
   server `pending_remote_init` with a `fingerprint` (base64url SHA-256 of the public key).
4. Client **shows `https://discord.com/ra/<fingerprint>` as a QR code**; the user scans it with
   the official mobile app and taps approve.
5. Server `pending_ticket` (encrypted `id:discriminator:avatar:username`), then `pending_login`
   with a `ticket` (or `cancel`).
6. `POST /users/@me/remote-auth/login {ticket}` → `encrypted_token`; RSA-OAEP decrypt → the
   user's **account token**, same as the desktop client's.

On the 360 this needs: RSA-2048 keygen (mbedTLS; seconds on a 3.2 GHz PPC, **[A]**), a QR
encoder (~1k lines of C, many permissive ones exist), a screen to draw it on, and secure-ish
storage of the resulting token (plaintext on HDD unless we wrap it with the console's
`XeCrypt*` keys **[A]**).

Risks: **[V]** Discord does not document this protocol at all; it is reverse engineered and
versioned (`v1` already discontinued). **[A]** It is also exactly the login path Discord's
desktop app uses, so a login made this way shows up in the account's "Devices" list as a desktop
session with whatever `properties` the client claims; that is detectable. Any change by Discord
(new version, new signed payloads, a client-attestation step) kills the 360 login until
re-reverse-engineered. The mobile app approval screen tells the user where/what is logging in,
which is a feature, not a bug.

### 2.3 OAuth2: no route to voice for a normal app

**[V]** OAuth2 is for applications. The gateway page (docs.discord.food) notes that bearer
tokens can identify to the gateway only with `identify` plus `gateway.connect` or `voice`
scopes; **[A]** those scopes are restricted to Discord-approved partner/Social SDK apps and are
not grantable to a hobby app. The Social SDK's console flow is the **OAuth2 device
authorization grant** (`POST /oauth2/device/authorize` with scope `sdk.social_layer`, user enters
a code at discord.com/activate, app polls `/oauth2/token`) **[V]** (docs.discord.com Social SDK
"Account Linking on Consoles"), but it requires a registered application, "middleware approval",
being "an approved developer for the target console", and Social SDK voice runs in **lobbies**,
not server voice channels. The Social SDK is a C++ binary for supported platforms; the 360 is not
one and never will be. OAuth2 therefore cannot satisfy "join my server's voice channel as me".

**Conclusion:** remote-auth QR is the only workable login for a TV device. It yields a full user
token, which puts the client squarely into "unauthorized client" territory (section 3).

---

## 3. Terms of Service and account risk

**[V]** Current wording:

- Terms of Service, effective 2025-09-29 (discord.com/terms): the software licence says "You may
  not copy, modify, create derivative works based upon, distribute, sell, lease, or sublicense"
  and "You also may not reverse engineer or decompile our software or services, attempt to do so,
  or assist anyone in doing so". The restrictions list includes "using any unauthorized software
  designed to modify the services" and "copying, dismantling, or reverse engineering any of our
  services". Discord may suspend or terminate accounts of users who "breach, or … encourage or
  help others to breach, these terms".
- Community Guidelines, effective 2025-09-29 (discord.com/terms/guidelines): item 14 "Do not use
  self-bots or user-bots. Each account must be associated with a human, not a bot"; item 15 "Do
  not engage with our service in an inauthentic way".
- Support article "Automated user accounts (self-bots)" (support.discord.com/hc/articles/
  115002192352; text as quoted by search, the page itself returned 403 to this tool): "Automating
  normal user accounts (generally called 'self-bots') outside of the OAuth2/bot API is forbidden,
  and can result in an account termination if found."
- The Developer Policy page could not be fetched (403); nothing in it is needed for the verdict.

**[A]** How this applies here, honestly:

- A hand-written voice client logging in with a user token **is** a third-party client, and
  writing it requires reverse-engineered protocol docs (the ToS bans reverse engineering and
  assisting it). The client is not a "self-bot" in the automation sense (a human presses the
  buttons), but Discord has never carved out an exception for alternative clients, and the
  support article's framing ("outside of the OAuth2/bot API") covers it.
- Real-world enforcement, from forum/community reports only: bans attributed purely to using a
  third-party client (Vencord, Abaddon, mautrix bridges) are reported as rare and usually tied
  to abuse-like behaviour or premium-feature unlocks; anti-spam flags leading to phone/e-mail
  re-verification are reported as more common for unusual clients. No official enforcement
  statistics exist. Discord's Warning System (updated 2026) keeps violation records separate from
  restriction duration.
- The risk lands on **each player's personal account**, not on the project. A voice client that
  shows up as an unknown desktop session, identifies with fake client properties, and connects
  from a console IP is exactly the fingerprint anti-abuse systems look at. Expect the realistic
  failure mode to be "account temporarily locked, verify by phone" rather than a permanent ban,
  but nobody can promise that.
- Discord officially supports Xbox One/Series (voice via the console's party system) and
  PlayStation; there is no official path for the 360 and no partner programme a homebrew
  project could join.

**Go/no-go:** this is a policy violation by the letter of the ToS, with moderate practical risk
per user, and the project would be asking every player to accept that risk with their own
account. That should be stated to users plainly if it goes ahead.

---

## 4. Xbox 360 platform feasibility

### 4.1 TLS

- **[V]** Precedent: **XboxTLS** (github.com/JakobRangel/XboxTLS, MIT) is a TLS 1.2 client for
  360 homebrew wrapping **BearSSL**, built with the XDK/VS2010, "Winsock + XNet" I/O
  (`XNetStartup`, `XNetDnsLookup`, `send`/`recv`), entropy from **`XeCryptRandom`**, no TLS 1.3,
  trust anchors supplied manually as raw keys (BearSSL minimal X.509, "does not perform full
  certificate chain validation"). X-Store (a 360 homebrew game downloader) uses it for HTTPS and
  bundles certs (`githubCert.h`, `goDaddyRootCA.h`). So TLS 1.2 with ECDHE/P-256 on this CPU is
  proven in practice, just not with our toolchain.
- **[A]** For OpenXeChain, mbedTLS (pure C, big-endian clean, no OS deps, `MBEDTLS_NO_PLATFORM_
  ENTROPY` + `XeCryptRandom` as the entropy source, `MBEDTLS_HAVE_TIME` optional) is the natural
  port; wolfSSL also builds for bare-metal. Either must be built soft-float (they are integer
  code anyway) and with `-fno-zero-initialized-in-bss` (mbedTLS has few large static buffers).
  BearSSL via XboxTLS is the smallest option but lacks TLS 1.3 and full chain validation.
- Certificate validation: pin the handful of roots behind `gateway.discord.gg`,
  `discord.com`, `remote-auth-gateway.discord.gg` and the `*.discord.media` voice endpoints
  (**[A]** Cloudflare-fronted; which CA roots exactly must be checked at milestone 1). Validity
  checks need the console clock (`KeQuerySystemTime`, works without Live) or can be disabled
  when pinning specific roots. **[A]** TLS 1.2 is still accepted by those endpoints (Cloudflare's
  default minimum is 1.0/1.2); the first milestone verifies it.
- Memory/CPU: an ECDHE-RSA/ECDSA TLS handshake in software is a few hundred ms at most on the
  Xenon core (**[A]**); record encryption at voice/gateway data rates is negligible.

### 4.2 Sockets and DNS from a resident plugin while a title runs

- **[X]** Every `NetDll_*` export takes an `XNCALLER_TYPE` first argument
  (`XNCALLER_TITLE=1`, `XNCALLER_SYSAPP=2`, `XNCALLER_XBDM=3`, `XNCALLER_TEST=4`, from xenia
  `xam_net.cc`). XAM's own Live/marketplace traffic runs as a system caller, which is how Live
  sessions survive title launches; a resident DLL should use `XNCALLER_SYSAPP` for its startup,
  sockets and DNS so that a title's `XNetCleanup`/`WSACleanup` does not tear them down. **[A]**
  That the per-caller contexts are really independent on 17559 (socket limits, whether a title's
  `XNetStartup` with custom `cfgSockMaxDgramSockets` etc. affects system sockets) needs a test;
  xenia does not model it.
- **[X]** Insecure (plain IP, non-XNet-encapsulated) sockets need the "insecure sockets"
  privilege; titles get it via `XNetStartupParams.cfgFlags` `XNET_STARTUP_BYPASS_SECURITY` only if
  the XEX carries the privilege. **[V]** DashLaunch `sockpatch`: "all titles will have the
  insecure socket privilege" (consolemods.org DashLaunch page). **[A]** System callers already
  have it (XAM talks to arbitrary hosts), so a system DLL probably does not depend on
  `sockpatch`, but it is on anyway.
- **[A]** `liveblock=true` stops XAM from contacting Xbox Live (how exactly is not documented in
  the sources reached; believed to be hostname/DNS based). It should not affect `discord.gg`
  lookups, but the voice endpoints must be verified reachable with it on; `xhttp=false` only
  matters for XAM's HTTP helper, which we would not use.
- DNS: `NetDll_XNetDnsLookup(caller, host, event, &xndns)` is asynchronous with an event;
  works for system callers (XboxTLS uses it from a title).
- The existing `xw.xex` architecture (system thread pinned to hardware thread 2, priority 9,
  2 ms sleeps, no FP, thunked kernel callbacks) carries over; a voice client needs one more
  thread for the network/audio loop and must never block Usbd's DPC path.

### 4.3 CPU budget while a game runs

**[A]** Estimates, no 360 measurements exist:

- Opus fixed-point (`FIXED_POINT`, no FPU needed, so compatible with `-msoft-float`): public data
  points are ~30 MHz on a Cortex-A8 for 48 kHz mono encode at complexity 5 (xiph list, 2014) and
  ~3x real time for CELT encode on a 240 MHz ESP32-S3. A 3.2 GHz in-order PPC hardware thread
  without SIMD use is far faster than either; budget 1–3 % of one hardware thread for one encode
  plus 3–5 decodes at 20 ms frames, plus a mixer. Encode complexity can be lowered.
- Transport AES-256-GCM / XChaCha20 and DAVE AES-128-GCM: voice is ~10 KB/s per stream;
  software AES at tens of MB/s makes this negligible.
- MLS: P-256 operations only on membership changes (someone joins/leaves) — tens of ms each in
  software, fine. RSA-2048 keygen once per login — seconds, acceptable with a progress note.
- Risk: games use all six hardware threads; Halo 3 is heavy. The voice thread competes at the
  system-process priority ceiling (9) with XAM's workers. Under heavy load Opus jitter buffers
  must absorb scheduling gaps; expect to tune frame batching rather than hit a hard limit.

### 4.4 Memory budget for a resident DLL

**[A]** Code: mbedTLS ~200–300 KB, Opus fixed-point ~250–350 KB, WebSocket+JSON+gateway tens of
KB, bespoke MLS+HPKE ~100–200 KB, QR+UI small. Data: TLS I/O buffers (16 KB records × 2 per
connection × up to 3 connections), gateway READY streaming buffer (keep bounded, e.g. 64 KB),
Opus state (~30 KB encoder + ~20 KB per decoder), jitter/mix buffers (~200 KB), MLS tree state
(small for a channel-sized group). Total 1.5–3 MB resident. `xw.xex` today is 416 KB. The
console has 512 MB and titles assume nearly all of it; a 2–3 MB resident plugin is in the range
other plugins live in, but memory-hungry titles need testing (SynthXEX maps only file bytes, so
all buffers must be in the image or allocated at runtime from a system pool).

### 4.5 Audio interfaces (handoff to AUDIO.md)

**[A]** If the audio agent gets mic/earpiece PCM from the controllers' 3.5 mm jacks via GIP
audio packets, the Discord path never touches the title's audio graph (no XAudio/XMA hooking):
decoded Opus is resampled 48 kHz → the GIP headset rate (Xbox One chat audio is 16 kHz mono
**[A]**) and mixed in the plugin; the mic goes the other way. That keeps game audio untouched and
is the cleanest split. Mixing Discord voice into the TV/HDMI output would require hooking the
title's XAudio output, which is a separate and much nastier problem.

---

## 5. UI options

1. **Standalone homebrew app launched from Aurora (recommended):** a normal title XEX built
   with OpenXeChain that draws a QR code and simple lists (own renderer or XUI), performs
   remote-auth, lets the user pick guild → voice channel, and writes `token` + choice to a config
   file. **[V]** `XamShowKeyboardUI` (`user_index, flags, default_text, title, description,
   buffer, buffer_length, overlapped`) and `XamShowMessageBoxUI` are XAM exports (xenia
   `xam_ui.cc`) for text entry and confirmations; XUI is also exported from xam.xex. The resident
   plugin then owns the session and voice. Smallest viable surface: QR login, channel list,
   join/leave, mute. Nothing else.
2. **Resident plugin runtime controls:** no UI at all; a controller chord (e.g. Guide+Y = mute,
   Guide+B = leave) hooked in the same XInput path `xw.xex` already owns, plus `XNotifyQueueUI`
   toasts ("Joined #general", "Muted"). This is how the client stays usable mid-game without
   touching the guide.
3. **Aurora-side integration:** **[V]** Aurora's Lua API (tuxuser/AuroraScripts) has
   `Http.Get/Post`, `Script.ShowKeyboard`, `Script.ShowPopupList`, `Script.ShowMessageBox`, XUI
   list/button controls via GizmoUI, but no raw sockets and no documented HTTPS, and scripts run
   only while Aurora is in the foreground. Aurora can host the *settings* screen (channel pick,
   written to the plugin's config) and launch the app, not the client.
4. **Guide (XAM HUD) injection:** the guide is XUI scenes inside xam.xex. **[A]** Known
   community work is resource-level skinning/patching of XAM, in-game mod menus that render
   with their own D3D code (e.g. Hayzen, an XDK DLL loaded as a DashLaunch plugin), and
   DashLaunch's own patches; I found no documented, reusable hook for adding a scene or button
   to the retail guide on 17559. It would mean hooking XUI scene navigation inside XAM with our
   64-bit-ABI thunks, with no symbols, against a dashboard that must keep working. Technically
   possible, large, fragile, and it adds nothing a chord + toast cannot do. Not recommended for
   the first version.

---

## 6. Alternatives that keep "no companion app, no per-user bot"

**Project-hosted relay with one bot per server.** A server admin adds the project's bot once
(not per user). The 360 speaks a trivial protocol (plain UDP Opus or even raw PCM, pre-shared
key) to a relay on a PC-class host, which joins the voice channel with `@discordjs/voice`
(DAVE handled by `@snazzah/davey`) and mixes/forwards audio.

- Avoids on the 360: TLS, WebSocket, JSON, MLS, remote-auth, token storage, ToS exposure of the
  players' accounts. The 360 part becomes a week of work on top of the audio I/O.
- Does **not** meet "own account": all 360 players appear as the bot (one speaking indicator,
  one name), and bots cannot join DM/group calls. Per-user identity could be faked with a
  per-user bot (ruled out) or by the relay holding user tokens (self-bot by definition, worse
  than the native client).
- Moves problems to the operator: the relay is the E2EE endpoint and hears everything in
  plaintext; hosting, uptime, abuse; each community needs the bot invited.

**OAuth2 linking to a relay** only gives identity (`identify`, `guilds`), never voice, so it
is cosmetic (the relay can name the bot's current speakers in a text channel). **Discord Social
SDK** is the sanctioned way to put Discord voice into a game with each player's own account, but
it requires an approved application, approved console middleware, and a supported platform; the
360 is not one.

So: the relay removes the technical and policy problems from the console by moving them to a
server the project runs, at the cost of the "own account" requirement. There is no option that
keeps all three of own-account, no-relay, and no-bot except the native client, and the native
client is a ToS violation by its nature.

---

## 7. Verdict

| | Feasibility | Notes |
|---|---|---|
| (a) Native voice-only client, own account, **without DAVE** | **Not possible** | Discord refuses non-DAVE clients since 2026-03-01 (fallback removed per 2026-05-18 blog). |
| (b) Native client **with DAVE** | Low–moderate technically; high account/policy risk | Needs a bespoke MLS member + HPKE on mbedTLS; tested only against the live service; undocumented QR login; every player breaks ToS with their own account. |
| (c) Running during games | Moderate | Resident-plugin model already exists; open questions are system-caller socket lifetime across title boots, CPU headroom under Halo 3, 2–3 MB resident footprint. |
| (d) Guide integration | Low | No known reusable guide-scene hook on 17559; chord + `XNotifyQueueUI` toasts + an Aurora-launched settings app is the practical UI. |

**Effort (one experienced engineer, full time, [A]):**

- TLS (mbedTLS port) + WebSocket + JSON + gateway state machine: 3–5 weeks.
- Remote-auth QR login, QR render, token storage: 1–2 weeks (plus breakage whenever Discord
  changes it).
- Voice gateway v8 + UDP + Opus fixed-point port + AES-GCM/XChaCha transport: 2–3 weeks.
- **DAVE/MLS client in C: 2–4 months**, the riskiest item; no offline test harness exists.
- Resident-plugin integration (sockets as system caller, threads, memory, controller audio path
  from AUDIO.md, chords/toasts): 1–2 months.
- Settings app UI: 2–4 weeks.
- Total ≈ 5–8 months to a usable first version, then permanent maintenance against protocol
  drift. The relay alternative is ≈ 2–3 weeks of 360 work plus a Node service.

**Biggest blockers, in order:**

1. DAVE is mandatory, and it is a full MLS client with no embedded-friendly implementation.
2. Login requires the undocumented remote-auth protocol; password login is blocked by hCaptcha.
3. Terms of Service: reverse engineering is prohibited, the client is unauthorized, and the risk
   is borne by each player's account.
4. Unverified 360 details: system-caller socket survival across title launches, resident memory
   headroom in heavy titles, CPU jitter under load.

**Minimal first milestone (proves the riskiest 360-side part, zero account risk):** an
OpenXeChain title XEX that calls `NetDll_XNetStartup`/`WSAStartup` as `XNCALLER_SYSAPP`, resolves
`gateway.discord.gg` with `XNetDnsLookup`, opens a TLS 1.2 session with a ported mbedTLS
(`XeCryptRandom` entropy, pinned roots, verify the chain), completes the WebSocket upgrade to
`/?v=10&encoding=json`, receives Hello (op 10) and keeps heartbeating (op 1 / op 11) for ten
minutes, logging RTT and heap use. No token is needed for any of that. Second milestone: the same
code moved into a resident system DLL and kept alive across launching and quitting Halo 3, which
answers the socket-lifetime question. Only after those two would it be worth deciding whether to
commit to the MLS work or fall back to the relay design.

---

## Sources

Discord (official):
- Voice connections reference: https://docs.discord.com/developers/topics/voice-connections
- Gateway reference: https://docs.discord.com/developers/events/gateway
- "Bringing DAVE to All Discord Platforms", 2025-09-02: https://discord.com/blog/bringing-dave-to-all-discord-platforms
- "Every Voice and Video Call on Discord Is Now End-to-End Encrypted", 2026-05-18: https://discord.com/blog/every-voice-and-video-call-on-discord-is-now-end-to-end-encrypted
- DAVE whitepaper: https://daveprotocol.com/ and https://github.com/discord/dave-protocol
- libdave: https://github.com/discord/libdave (cpp/README: mlspp + OpenSSL/BoringSSL)
- mlspp: https://github.com/cisco/mlspp
- Social SDK, account linking on consoles: https://docs.discord.com/developers/discord-social-sdk/development-guides/account-linking-on-consoles
- Terms of Service (eff. 2025-09-29): https://discord.com/terms
- Community Guidelines (eff. 2025-09-29): https://discord.com/terms/guidelines
- Self-bots support article: https://support.discord.com/hc/en-us/articles/115002192352

Unofficial Discord docs / libraries:
- Remote auth (desktop): https://docs.discord.food/remote-authentication/desktop
- Authentication / login: https://docs.discord.food/authentication
- Gateway (user accounts): https://docs.discord.food/topics/gateway
- CAPTCHA handling: https://discord-userdoccers.vercel.app/topics/captcha-handling
- @discordjs/voice DAVE docs: https://tessl.io/registry/tessl/npm-discordjs--voice/files/docs/dave-encryption.md ; @snazzah/davey: https://www.npmjs.com/package/@snazzah/davey
- mautrix-discord remoteauth: https://pkg.go.dev/go.mau.fi/mautrix-discord/remoteauth

Press on the DAVE cutover: Help Net Security 2026-05-19 https://www.helpnetsecurity.com/2026/05/19/discord-voice-and-video-call-encryption/ ; 9to5mac 2026-05-20 https://9to5mac.com/2026/05/20/discord-now-uses-end-to-end-encryption-for-all-voice-and-video-calls-by-default/ ; TechRepublic https://www.techrepublic.com/article/news-discord-dave-end-to-end-encryption/

Xbox 360:
- XboxTLS (BearSSL on 360): https://github.com/JakobRangel/XboxTLS ; X-Store: https://github.com/951261/X-Store
- xenia `xam_net.cc` (NetDll_* signatures, XNCALLER enum): https://github.com/xenia-project/xenia/blob/master/src/xenia/kernel/xam/xam_net.cc
- xenia `xam_ui.cc` (XamShowKeyboardUI etc.): https://github.com/xenia-project/xenia/blob/master/src/xenia/kernel/xam/xam_ui.cc
- DashLaunch options: https://consolemods.org/wiki/Xbox_360:DashLaunch
- Aurora Lua API: https://github.com/tuxuser/AuroraScripts
- Hayzen (XDK in-game menu DLL loaded as DashLaunch plugin): https://github.com/ClementDreptin/Hayzen
- Opus performance data points: https://lists.xiph.org/pipermail/opus/2014-November/002725.html ; https://components.espressif.com/components/esphome/micro-opus/versions/0.4.1/examples/encode_benchmark
- This repo: `docs/INTERNALS.md` (toolchain/kernel constraints), `CLAUDE.md`.
