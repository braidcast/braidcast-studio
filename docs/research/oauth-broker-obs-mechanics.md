# OAuth Token-Exchange Broker — OBS Mechanics + Braidcast Design Research

Research-only findings. No broker code written. Confidence flags used throughout:
**[verified from code]** (read the actual source), **[strong inference]** (deduced from
adjacent verified code / docs), **[uncertain]** (needs confirmation before relying on it).

Primary discovery: **OBS's broker is open source.** It is
[`obsproject/obs-oauth-cf`](https://github.com/obsproject/obs-oauth-cf) — a **Rust**
worker deployed to **Cloudflare Workers** at `auth.obsproject.com`. So most of Part 1 is
*verified from code*, not inferred from client requests.

---

## Executive summary (the 5-line bottom line)

1. **OBS's broker is a stateless Cloudflare Worker (Rust) at `auth.obsproject.com`.** Routes: `GET /v1/{platform}/redirect` (302 to provider authorize, injecting `client_id`), `GET /v1/{platform}/finalise` (the registered `redirect_uri`, returns a "done" page), `POST /v1/{platform}/token` (injects `client_secret` from Worker env, forwards code→token and refresh grants to the provider). **[verified from code]**
2. **No database. None.** The Worker has zero KV / D1 / Durable Object bindings; `state` is random per-request and never stored server-side (CSRF is validated client-side). A token broker for a native app is genuinely stateless. **[verified from code]**
3. **Secrets live only at the broker** for Twitch and Restream (`TWITCH_SECRET`, `RESTREAM_SECRET` as Worker secrets). The client ships only an *obfuscated client_id*, no secret. **[verified from code]**
4. **YouTube is the exception: OBS does NOT broker it.** It ships the Google `client_id` *and* `client_secret` obfuscated in the binary and talks directly to Google over a **127.0.0.1 loopback** listener — because Google forbids embedded webviews, so it can't use the embedded-browser broker flow. **[verified from code]** To broker YouTube without shipping the secret, Braidcast needs the loopback-handoff pattern (below).
5. **Hosting rec: Cloudflare Workers** — it's what OBS uses, has a native secrets store, no cold start, a free tier that covers this load, and is stateless by nature. Refresh tokens stay **client-side**; the client re-calls `/token` with `grant_type=refresh_token`.

---

## Part 1 — How OBS Studio's OAuth broker actually works

Client-side code lives in `frontend/oauth/` (recent OBS; older trees had it in `UI/auth-*.cpp`).
Broker-side code is the separate repo `obsproject/obs-oauth-cf`.

### 1.1 Broker endpoint shapes (on `auth.obsproject.com`)

From the client, the URL constants are built as `OAUTH_BASE_URL "v1/{platform}/..."`
(`OAUTH_BASE_URL` is a compile-time constant = `https://auth.obsproject.com/`,
configurable for third-party builds). **[verified from code]**
`frontend/oauth/TwitchAuth.hpp`, `frontend/oauth/RestreamAuth.hpp`:

```
#define TWITCH_AUTH_URL    OAUTH_BASE_URL "v1/twitch/redirect"
#define TWITCH_TOKEN_URL   OAUTH_BASE_URL "v1/twitch/token"
#define RESTREAM_AUTH_URL  OAUTH_BASE_URL "v1/restream/redirect"
#define RESTREAM_TOKEN_URL OAUTH_BASE_URL "v1/restream/token"
```

Broker routes, from `obs-oauth-cf/src/lib.rs` **[verified from code]**:

| Route | Method | Behavior |
|---|---|---|
| `/v1/{platform}/redirect` | GET | `handle_redirects` → builds provider authorize URL (`client_id`, `redirect_uri`, `response_type=code`, `scope`, random `state`) and returns a **302** to the provider (`id.twitch.tv/oauth2/authorize`, etc.). |
| `/v1/{platform}/finalise` | GET | This is the `redirect_uri` registered with the provider. Returns a static `OAUTH_COMPLETE` page. Does **not** exchange or store anything. |
| `/v1/{platform}/token` | POST | `handle_token` → injects `client_secret` from Worker env, forwards `grant_type=authorization_code` (and `refresh_token`) to the provider token URL, returns the provider's JSON verbatim. |
| `/app-auth/{platform}` (GET) / `/app-auth/{platform}-token` (POST) | — | Legacy routes, same behavior, `legacy=true`. |
| `/` GET, `/*catchall` | — | Blank page / 404. |

Registered redirect URIs, from `wrangler.toml` **[verified from code]**:
`TWITCH_REDIRECT_URL = https://auth.obsproject.com/v1/twitch/finalise`,
`RESTREAM_REDIRECT_URL = https://auth.obsproject.com/v1/restream/finalise`
(plus `*_LEGACY_REDIRECT_URL` on `obsproject.com/app-auth/...`).

### 1.2 Secrets: broker-held, not shipped (except YouTube)

- **Twitch / Restream (brokered):** `wrangler.toml` + `src/platforms/twitch.rs` read `ctx.secret("TWITCH_ID")` and `ctx.secret("TWITCH_SECRET")` (and `RESTREAM_*`) — the **secret is a Cloudflare Worker secret, never in the client**. The client ships only `TWITCH_CLIENTID` via `deobfuscate_str(...)`. **[verified from code]**
- **The core proxy** `src/platforms/oauth.rs` `get_token_internal`: reads `grant_type` from the client's form data, builds POST data with `client_id` + `client_secret` (from the `OAuthConfig`/env) + `grant_type` + `code`/`refresh_token`, POSTs to the provider token URL, returns the JSON. **Client secret injection is server-side and direct.** **[verified from code]**
- **YouTube (NOT brokered):** `frontend/oauth/YoutubeAuth.cpp` deobfuscates **both** `YOUTUBE_CLIENTID` *and* `YOUTUBE_SECRET` in the binary and hits Google **directly** — auth `https://accounts.google.com/o/oauth2/v2/auth`, token `https://www.googleapis.com/oauth2/v4/token`. No `auth.obsproject.com` involvement. **[verified from code]**

### 1.3 Client type + redirect/handoff mechanism per service

Two distinct handoff mechanisms in OBS:

- **Twitch / Restream → embedded-browser + broker HTTPS callback.** Client type is a **confidential web client** (the broker is the confidential party). The client opens `TWITCH_AUTH_URL` in OBS's **embedded CEF browser** (`OAuthLogin`), the broker 302s to Twitch, Twitch redirects to `.../v1/twitch/finalise`, and OBS **scrapes the `?code=` off the browser URL** because it watches for navigations whose URL starts with `OAUTH_BASE_URL`. It then POSTs the code to `/v1/twitch/token`. **[strong inference]** — verified: `redirect_uri` = `.../finalise` returning a page (`wrangler.toml`, `lib.rs`) and the client checks URLs against `OAUTH_BASE_URL`; the "embedded CEF panel" identity of `OAuthLogin` is inferred from OBS's browser-panel architecture.
- **YouTube → system browser + 127.0.0.1 loopback listener.** `frontend/oauth/AuthListener.cpp` binds `QHostAddress::LocalHost` on port `0` (OS-assigned), and `YoutubeAuth.cpp` sets `redirect_uri = http://127.0.0.1:{port}`. Google is opened in the **system browser** (Google blocks embedded webviews: `disallowed_useragent`), redirects to the loopback, and `AuthListener` regex-extracts `code` and validates `state` (CSRF) before `emit ok(code)`. **[verified from code]** No PKCE (`response_type=code&client_id=..&redirect_uri=..&state=..` only). **[verified from code]**

**Why the split matters for Braidcast:** Braidcast is CEF+Svelte. For Google/YouTube you *cannot* use an embedded CEF webview for the consent screen — you are forced onto system-browser + loopback. That is exactly why OBS shipped the Google secret instead of brokering it: brokering a system-browser/loopback flow needs the extra handoff step in Part 2.2.

### 1.4 Refresh

Refresh token lives **on the client**; the client re-calls the broker `/token` to refresh.
`frontend/oauth/OAuth.cpp` builds the refresh POST as
`action=redirect&client_id=<id>&grant_type=refresh_token&refresh_token=<token>`
(secret omitted client-side — the broker adds it). The exchange POST is
`action=redirect&client_id=<id>&client_secret=<secret,if any>&redirect_uri=<uri,if any>&grant_type=authorization_code&code=<code>`.
The client stores `access_token`/`refresh_token`/`expires_in` via `config_set_*`.
**[verified from code]** No server-side refresh state → consistent with the stateless Worker.

### 1.5 Statelessness / no server source secrecy caveat — resolved

The task assumed the broker source would be closed and its behavior only inferrable. **It
is not** — `obs-oauth-cf` is public, so §1.1–1.4 broker behavior is *verified*, not inferred.
Confirmed statelessness: `src/platforms/oauth.rs` generates `state` with `rand::thread_rng()`
per request and **never persists it**; `wrangler.toml` has **no** `[[kv_namespaces]]`,
`[[d1_databases]]`, or `[[durable_objects]]` bindings. **[verified from code]**

---

## Part 2 — Recommended minimal stateless broker for Braidcast (2026)

### 2.1 The pattern: token broker / BFF-for-native, no DB

The server does exactly two secret-dependent things: **(a) code→token exchange** and
**(b) token refresh**, in both cases injecting the platform `client_secret` it holds. The
**refresh token stays on the client** (Braidcast's local config), which is standard for
native apps (RFC 8252) and is what keeps the broker stateless. **No database is required** —
OBS proves it in production. Confirm the reasoning:

- Nothing needs to persist across requests. `state`/`code_verifier` are minted by the *client* and validated by the *client*; the broker just forwards.
- Refresh is client-initiated, so the broker holds no session.
- The only long-lived secrets are the platform `client_secret`s — those go in the host's **secret store** (Cloudflare Secrets / Worker secrets), not a DB.

> **Honest divergence from generic BFF advice.** Web-SPA BFF guidance (Auth0/FusionAuth) says *keep refresh tokens server-side* because a browser client is untrusted. That advice does **not** transfer to a **native desktop** app: the client is a trusted first-party binary on the user's machine, RFC 8252 expects the refresh token there, and server-side refresh would force a stateful broker (DB + rotation + revocation) — defeating the stateless goal. Trade-off to accept consciously: **client-side refresh ⇒ no central revocation** (you rely on the provider's token revocation, not your own). For Braidcast this is the right call; flag it in design review rather than silently inheriting SPA guidance. **[strong inference]**

Endpoints (mirror OBS):
`GET /v1/{platform}/start` (→302 authorize), `GET /v1/{platform}/callback` (registered
redirect_uri), `POST /v1/{platform}/token` (exchange + refresh). Add per-IP rate limits (§2.4).

### 2.2 The loopback-handoff problem (confidential WEB client)

When a provider client is a **confidential web client**, its `redirect_uri` must be the
broker's **HTTPS** callback (`https://broker/v1/{platform}/callback`), *not* a `localhost`
URL. But the app that needs the tokens is on the desktop. Two proven handoff patterns; pick
per platform:

- **Pattern A — embedded browser + URL scrape (what OBS does for Twitch/Restream).** App opens the flow in an in-app CEF webview; when the webview navigates to the broker callback, the app reads the `?code=` off the URL and POSTs it to `/token`. **Only works where the provider allows embedded webviews** (Twitch, Kick — *not* Google).
- **Pattern B — broker 302 back to a loopback listener (needed for Google/YouTube).** App starts a `http://127.0.0.1:{port}` listener and opens the **system browser**. Flow:
  1. App → system browser → `GET /v1/youtube/start?...` with the app's loopback port + a client nonce encoded in a **signed `state`** (e.g. `state = base64url(hmac_signed({port, nonce}))`).
  2. Broker 302 → Google authorize with `redirect_uri = https://broker/v1/youtube/callback` and that `state`.
  3. Google → `GET https://broker/v1/youtube/callback?code=..&state=..`.
  4. Broker verifies the `state` HMAC, **exchanges the code for tokens itself** (it has the secret), then **302s the browser to `http://127.0.0.1:{port}/?...`** — handing the result to the app's loopback listener. Prefer handing back a **one-time code** (or the tokens) here; tokens-in-redirect is acceptable for same-machine loopback but a one-time code avoids browser-history/log leakage.
  5. App's loopback listener catches it, POSTs the one-time code to `/token` (or already has tokens), shuts the listener.

  **Transient state, still no DB:** encode everything the callback needs (`port`, `nonce`, `code_verifier` hash, platform) in the **signed `state`** that round-trips through the provider. That is *self-carried* state — no server memory, no TTL store. (An in-memory TTL map is the alternative but **does not survive** across Cloudflare Worker isolates, so signed-state is the correct stateless choice on Workers.) **[strong inference]**

  Note the Web-client loopback subtlety: Google **Web application** clients require an *exact* `redirect_uri` match including port, so you can't use a dynamic loopback port *directly on the Google client* — which is precisely why redirect_uri is the broker's fixed HTTPS callback and the *dynamic* loopback is only the broker→app final hop. **[strong inference; confirm against Google console behavior]**

### 2.3 Provider specifics

- **Google / YouTube.** Moving from an "installed app"/PKCE client to a **"Web application" confidential client**: the secret now lives at the broker, `redirect_uri` becomes the broker's HTTPS `/callback` (Web clients still permit `http://localhost`/`http://127.0.0.1` but with **exact-port** matching, unlike Desktop clients which allow any loopback port — so use the broker callback + Pattern B). **PKCE still applies and is recommended** even for confidential clients; the `code_verifier` stays on the client and is forwarded through `/token`. Token endpoint requires `client_id` + `client_secret` + `code` + `redirect_uri` + (`code_verifier`). **Google verification tax:** the `youtube`/`youtube.readonly` scopes are *sensitive* → brand + sensitive-scope review; any scope change resets verification; restricted scopes can trigger yearly CASA audits. Budget for this. **[strong inference, community-confirmed]**
- **Twitch.** Two viable broker shapes: **(a) authorization-code + secret via broker** (OBS's approach — simplest broker, client sends `code`, broker adds secret at `/token`), or **(b) keep device-code** (no redirect, no secret, no broker needed — but clunkier UX: user types a code on a second page). For a *broker*, **(a) is simpler and matches OBS**; Twitch permits embedded webviews so Pattern A works. Twitch also *requires* `client_secret` for the authorization-code token exchange, so brokering removes it from the binary. **[strong inference]**
- **Kick.** Already `needsSecret:true` **PKCE + loopback**. Fits the broker cleanly: client does PKCE loopback, sends `code` + `code_verifier` to `/token`, broker injects `client_secret`, forwards to Kick. Both PKCE *and* secret are used (Kick is a confidential client that also mandates PKCE). Watch: Kick's send/token endpoints have hit Cloudflare 403s (per project memory) and Kick prefers a fixed loopback port. **[strong inference]**

### 2.4 Abuse controls (keep brief)

`/start` and `/token` are **public** — anyone can drive an OAuth flow under *our* registered
client_id, which means **quota burn** (Google/YouTube quota, Twitch rate limits) and
token-farming under our project identity. Minimum controls:

- **Per-IP rate limiting** on `/start` and `/token` (Cloudflare Rate Limiting rules / the Workers rate-limit binding). Cap tokens-per-IP-per-minute.
- **Tight route + method allowlist** and a strict outbound allowlist (only the three provider token hosts).
- Optionally a lightweight shared header / app attestation to make casual scraping harder (not a real secret — it ships in the binary — but raises the bar).
- Reject unknown `{platform}` and malformed `grant_type` early.

---

## Part 3 — Hosting a stateless secret-holding broker

| Option | Cold start | Secret storage | Cost (this load) | Stateless fit | Notes |
|---|---|---|---|---|---|
| **Cloudflare Workers** | **None** (V8 isolates) | **Native** Worker Secrets / Secrets Store | **Free tier** ~100k req/day covers it | **Perfect** (no shared memory *by design*) | **What OBS uses.** Rust (`workers-rs`) or TS. Native rate-limiting/WAF. Slight lock-in to Workers runtime APIs. |
| **VPS / Fly.io (Node/Deno container)** | Some (if scale-to-zero) | Env / Fly secrets / vault | ~$3–5/mo min, you patch it | Fine | Full control, but you own TLS, uptime, OS patching, scaling. Overkill for a proxy. |
| **Vercel / Netlify functions** | **Yes** on free tier (lambda cold start) | Env vars (encrypted) | Free tier ok | Fine (stateless) | Works, but cold-start latency on the auth path, less edge-local than Workers, no built-in rate-limit primitive as clean as CF. |

**Recommendation: Cloudflare Workers.** It is the exact stack OBS runs for this exact
purpose; zero cold start on the token path, a first-class secret store, a free tier that
comfortably covers a single desktop app's auth traffic, native per-IP rate limiting, and it
is stateless by construction (no idle server, no DB to run). Language: TypeScript is the
lowest-friction (`wrangler` + Hono), or Rust `workers-rs` to mirror OBS. The only real
downside — Workers-runtime lock-in — is irrelevant for a ~200-line proxy you could re-host
on Fly in an afternoon.

---

## Open questions for design

1. **YouTube handoff:** commit to Pattern B (system browser + broker HTTPS callback + signed-state 302 to loopback). Confirm a Google **Web** client accepts the broker HTTPS callback and that the final broker→app hop to `http://127.0.0.1:{dynamic port}` is done by the *broker* (not registered on Google). **[needs console verification]**
2. **Twitch:** move device-code → authorization-code-via-broker (matches OBS, simpler broker) or keep device-code (no broker)? Recommend auth-code-via-broker.
3. **One-time code vs tokens-in-redirect** on the broker→loopback hop — recommend one-time code to keep tokens out of browser history/logs.
4. **Signed-state secret:** the broker needs its own HMAC key (a 4th Worker secret) to sign/verify `state`. Confirm rotation story.
5. **Refresh model:** confirm client-side refresh (stateless broker, no revocation) is acceptable vs a stateful broker with central revocation. Recommend client-side.
6. **Google verification/CASA:** which YouTube scopes does Braidcast actually need, and do they fall in sensitive/restricted tiers (verification + possible annual audit)?
7. **Kick fixed loopback port + Cloudflare-403 on send** — pre-existing risks to carry into broker testing.

---

## Primary sources

**OBS broker (server) — `obsproject/obs-oauth-cf`, verified from code:**
- Repo: https://github.com/obsproject/obs-oauth-cf
- `src/lib.rs` — routes `/v1/{platform}/redirect|finalise|token`, legacy `/app-auth/*`.
- `src/platforms/oauth.rs` — `get_redirect_url` (302 to provider), `get_token_internal` (injects `client_secret`, forwards exchange + refresh); `state` via `rand::thread_rng()`, never stored → stateless.
- `src/platforms/twitch.rs` — `id.twitch.tv/oauth2/authorize` + `/token`; reads `ctx.secret("TWITCH_ID"|"TWITCH_SECRET")`; scope `channel:read:stream_key`.
- `wrangler.toml` — `TWITCH_REDIRECT_URL=https://auth.obsproject.com/v1/twitch/finalise`, no KV/D1/DO bindings (stateless).
- `README.md` — Worker secrets `TWITCH_ID/TWITCH_SECRET/RESTREAM_ID/RESTREAM_SECRET`; deploy via `wrangler`.

**OBS client — `obsproject/obs-studio`, `frontend/oauth/`, verified from code:**
- `OAuth.cpp` / `OAuth.hpp` — `GetToken`/refresh POST bodies (`action=redirect&client_id=..&client_secret=..(opt)&redirect_uri=..(opt)&grant_type=authorization_code&code=..`; refresh omits secret); `OAUTH_BASE_URL "v1/{platform}/..."`.
- `TwitchAuth.hpp` / `RestreamAuth.hpp` — `*_AUTH_URL`/`*_TOKEN_URL` macros; obfuscated `client_id`, no secret client-side.
- `YoutubeAuth.cpp` — deobfuscates `YOUTUBE_CLIENTID` **and** `YOUTUBE_SECRET`; hits Google directly (`accounts.google.com/o/oauth2/v2/auth`, `googleapis.com/oauth2/v4/token`); `redirect_uri=http://127.0.0.1:{port}`; no PKCE.
- `AuthListener.cpp` — binds `QHostAddress::LocalHost:0`, regex-extracts `code`, validates `state`, `emit ok(code)`.

**Standards / provider docs / community:**
- RFC 8252 (OAuth for Native Apps) — loopback redirect, refresh token on client.
- Google native-app OAuth: https://developers.google.com/identity/protocols/oauth2/native-app
- Google loopback migration: https://developers.google.com/identity/protocols/oauth2/resources/loopback-migration ; OOB removal (Jan 31 2023): https://developers.google.com/identity/protocols/oauth2/resources/oob-migration
- Cloudflare Workers OAuth precedent: https://github.com/cloudflare/workers-oauth-provider
- BFF pattern (SPA context, contrasted): https://auth0.com/blog/the-backend-for-frontend-pattern-bff/
- Google CASA 2025 (verification/audit tax): https://deepstrike.io/blog/google-casa-security-assessment-2025
- Community sentiment (last-30-days sweep): loopback `127.0.0.1` vs `localhost` mismatch → `redirect_uri_mismatch`; Web client CAN back a desktop app and is HTTPS-exempt for loopback; loopback flow deprecated for mobile/Chrome (2022) but supported for Desktop; 2025 Google console hides secret after creation, scope changes reset verification.
