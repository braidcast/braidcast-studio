# OAuth Verification & Compliance Requirements

Research reference for what this app (an OBS Studio fork, CEF + Svelte desktop streaming client) must do to
ship its platform OAuth integrations legitimately. Grounded against the actual code in `frontend/src/oauth/`
and cited to primary sources. Last researched **2026-07-04**.

Confidence labels: **[verified]** = confirmed against an authoritative primary source and/or the code;
**[strong]** = strong inference from primary sources; **[uncertain]** = community/indirect, flagged as such.

---

## Executive summary — the bottom line

**What we must do RIGHT NOW (pre-release, dev + small tester group): essentially nothing new.** Every provider's
OAuth works today in an un-reviewed / testing posture for a small user set. No security assessment, no paid
audit, no verification is required to keep developing and testing. **[verified]**

**The one thing that actually bites us during testing:** Google/YouTube in "Testing" publishing status expires
**refresh tokens after 7 days** (External + sensitive scope). Testers will have to re-authenticate their YouTube
account weekly until we publish + verify. This is real and documented. **[verified]**

**What we owe before a public launch, per provider:**

| Provider | Scope tier | Gate to public use | Rough effort | Paid audit? |
|---|---|---|---|---|
| **Google / YouTube** | **Sensitive** (not restricted) | Full OAuth app verification (homepage, privacy policy, verified domain, demo video, per-scope justification) | Official "up to 10 days"; **real-world 4–6 weeks** | **No** — CASA does **not** apply to sensitive scopes |
| **Twitch** | n/a (no tier system) | Register an app (2FA on account); **no scope review/verification** exists | Minutes | No |
| **Kick** | n/a | Register an app in Kick dev dashboard; API is new/thin | Minutes, but API-stability risk | No |
| **Meta / Facebook** (future, website) | n/a | App Review per Live-video permission + **Business Verification** + Data Deletion Callback + privacy policy | Weeks; separate deep-dive | No (but heaviest process of all four) |

**Single most important fact:** our Google scope is **sensitive, not restricted**, so the expensive annual
third-party **CASA security assessment ($1.5k–3k + weeks) does NOT apply to us.** **[verified]** The Google
work is paperwork + a demo video + a public website with a privacy policy, reviewed for free over several weeks.

**The website with a privacy policy is the long pole** — it's a hard prerequisite for Google verification (and
later for Meta), so stand it up early even though nothing forces it during private testing.

---

## Our actual OAuth footprint (from the code)

Verified by reading `frontend/src/oauth/*_provider.cpp` on 2026-07-04.

### Google / YouTube — `youtube_provider.cpp`
- **Scopes:** exactly one — `https://www.googleapis.com/auth/youtube.force-ssl`
  (the single broad write scope covering `channels.list`, `liveBroadcasts`/`liveStreams` insert+bind,
  `videos.update`, `thumbnails.set`, `videoCategories.list`).
- **Flow:** `pkce-loopback`, `needsSecret: false` — i.e. a **Desktop/Installed (native) app** client with PKCE
  and a `http://localhost` loopback redirect. No client secret shipped. This is the Google-blessed pattern for
  native apps ([OAuth for Mobile & Desktop](https://developers.google.com/identity/protocols/oauth2/native-app)).

### Twitch — `twitch_provider.cpp`
- **Scopes (9):** `channel:read:stream_key`, `channel:manage:broadcast`, `chat:read`, `chat:edit`,
  `user:read:chat`, `user:write:chat`, `moderator:read:followers`, `channel:read:subscriptions`, `bits:read`.
- **Flow:** `device-code` grant, `needsSecret: false`. **No redirect URI is involved** in the device code flow
  (the user authorizes on a Twitch page via a code), which sidesteps redirect-URI validation entirely.

### Kick — `kick_provider.cpp`
- **Scopes (5):** `channel:read`, `channel:write`, `user:read`, `chat:write`, `events:subscribe`.
- **Flow:** `pkce-loopback`, `needsSecret: true`. Note the code comment: Kick's OAuth frontend rewrites a
  literal `127.0.0.1` redirect host, so the loopback `redirect_uri` must advertise `localhost` (still resolves
  to the local listener).

### Meta / Facebook
- **Not implemented.** Flagged in project memory as deferred ("8f Facebook deferred"). Future website-hosted
  Facebook Login. Included here for planning only.

---

## 1. Google / YouTube

### 1.1 Scope tier — `youtube.force-ssl` is SENSITIVE, not restricted  **[verified]**

Google classifies OAuth scopes into three tiers ([OAuth App Verification Help Center](https://support.google.com/cloud/answer/13463073),
[OAuth 2.0 Scopes](https://developers.google.com/identity/protocols/oauth2/scopes)):

- **Non-sensitive** — basic profile (`openid`, `userinfo.email`, `userinfo.profile`). No verification required.
- **Sensitive** — "require review by Google before a Google Account can grant access." Google's own example of a
  sensitive scope is **"deleting a YouTube video."** `youtube.force-ssl` grants exactly "see, edit, and
  permanently delete the user's YouTube videos, ratings, comments and captions" — squarely sensitive.
- **Restricted** — a **short** list of scopes giving "wide access to Google user data": Gmail
  (`gmail.readonly/modify/compose/send`), Drive (`drive`, `drive.readonly`), and similar. These trigger the
  extra security-assessment (CASA) requirement. **No YouTube scope is on the restricted list.**

Sources:
[Sensitive scope verification](https://developers.google.com/identity/protocols/oauth2/production-readiness/sensitive-scope-verification) ·
[Restricted scope verification](https://developers.google.com/identity/protocols/oauth2/production-readiness/restricted-scope-verification) ·
[Scopes list](https://developers.google.com/identity/protocols/oauth2/scopes)

> **Our tier map:** the only sensitive/restricted scope we request across ALL providers is Google's
> `youtube.force-ssl` = **sensitive**. Twitch and Kick have no equivalent Google-style tiering. We request
> **zero restricted scopes.**

### 1.2 Does CASA (annual third-party security assessment) apply to us? — NO  **[verified]**

CASA / the App Defense Alliance security assessment applies **only** to apps that "request access to Google
users' **restricted** data and [have] the ability to access data from or through a third-party server"
([Restricted scope verification](https://developers.google.com/identity/protocols/oauth2/production-readiness/restricted-scope-verification)).
Because `youtube.force-ssl` is **sensitive, not restricted**, CASA does **not** apply.

**If it did apply** (it doesn't — for context only): a Google-empanelled lab runs the assessment, it must be
**recertified annually**, and lived-experience puts the third-party-lab cost at **~$1,500–$3,000** with
scheduling that "adds weeks" ([r/SaaS lived report, Gmail restricted scopes](https://old.reddit.com/r/SaaS/comments/1udeu9s/going_through_googles_oauth_verification_for/)).
We avoid all of this by never requesting restricted scopes.

### 1.3 Testing vs Production — what works without verification  **[verified]**

While the OAuth consent screen stays in **Testing** publishing status ([Manage App Audience](https://support.google.com/cloud/answer/15549945),
[legacy consent-screen doc](https://support.google.com/cloud/answer/10311615)):

- **100 test-user cap.** "Projects configured with a publishing status of Testing are limited to up to
  **100 test users** listed in the OAuth consent screen." Each tester's Google account must be added by email.
- **Unverified-app warning screen.** Because we request a sensitive scope, testers see Google's "Google hasn't
  verified this app" interstitial and must click through *Advanced → Go to (unsafe)*. Expected and harmless for
  a known tester group.
- **7-day refresh-token expiry — REAL, and it applies to us.** For an **External** app in **Testing** status,
  "authorizations by a test user will expire **seven days** from the time of consent," and the refresh token
  dies with it. The documented exemption is only for apps requesting **solely** basic-profile / Sign-in-with-Google
  scopes (`userinfo.email`, `userinfo.profile`, `openid`). We request `youtube.force-ssl`, so **we are NOT
  exempt** — testers must re-auth their YouTube account weekly until we publish + verify.
  ([Manage App Audience](https://support.google.com/cloud/answer/15549945);
  corroborated: [Unipile](https://www.unipile.com/google-oauth-refresh-token/),
  [HomeSeer forum](https://forums.homeseer.com/forum/internet-or-network-related-plug-ins/internet-or-network-discussion/ak-google-calendar-alexbk66/1545936-refresh-token-expires-in-7-days-if-oauth-consent-screen-publishing-status-is-testing).)

> Publishing to **Production** (which requires passing verification) removes the 7-day expiry, the 100-user cap,
> and the warning screen. There is no way to remove the 7-day expiry while staying in Testing with a sensitive
> scope (short of an Internal Google Workspace app, which doesn't fit a public consumer product).

### 1.4 Full sensitive-scope verification checklist (public launch)  **[verified]**

From [Sensitive scope verification](https://developers.google.com/identity/protocols/oauth2/production-readiness/sensitive-scope-verification)
and [OAuth App Verification Help Center](https://support.google.com/cloud/answer/13463073). Every item is required:

**Consent-screen / branding prerequisites**
1. **App homepage URL** — publicly accessible, clearly relevant to the app under review.
2. **Privacy policy URL** — hosted on the **same domain** as the homepage, **linked on the OAuth consent screen**,
   disclosing what data is accessed, how it's used, and how it's shared.
3. **Verified domain ownership** — the app's domain verified in **Google Search Console** under an account that
   is an owner/editor of the Cloud project.
4. **Authorized domains** — declared and verified in the Cloud project configuration.
5. **App branding** — app name + logo consistent with the app, complying with the relevant API branding
   guidelines (e.g. YouTube branding).
6. **Support email** — current, on the consent screen.
7. **Developer contact information** — accurate owner/editor roles and contact emails on the project.
8. **Policy agreement** — adherence to the Google API Services Terms of Service and the
   [API Services User Data Policy](https://developers.google.com/terms/api-services-user-data-policy)
   (incl. the *Limited Use* requirements for how YouTube user data may be used).

**Submission**
9. **Scope declaration** — all requested scopes listed on the Cloud Console **Data Access** page.
10. **Per-scope justification** — for each sensitive scope, a detailed written explanation of why it's needed and
    why a narrower scope is insufficient. (For us: one scope, `youtube.force-ssl`, justified by live-broadcast
    create/bind + video metadata/thumbnail updates.)
11. **Demo video** — an **unlisted YouTube video** showing: the OAuth grant flow, the consent screen displaying
    the correct app name, the **browser address bar showing the OAuth client ID**, and the in-app functionality
    that exercises each requested sensitive scope.

**No CASA / third-party security assessment** appears in the sensitive-scope checklist (it is restricted-only).

### 1.5 Timeline + cost  **[verified official] / [strong community]**

- **Cost:** free. Sensitive-scope verification carries no Google fee and (per §1.2) no paid third-party audit.
- **Official timeline:** "The sensitive scope verification process can take **up to 10 days** to complete."
  ([Sensitive scope verification](https://developers.google.com/identity/protocols/oauth2/production-readiness/sensitive-scope-verification)).
- **Real-world timeline: expect 4–6 weeks, not 10 days.** Lived reports:
  - "Ours took around **5 weeks** for GA4 + GSC scopes… if you're running in Testing mode while you wait,
    refresh tokens expire every 7 days." — [r/microsaas](https://old.reddit.com/r/microsaas/comments/1ukb8p7/how_long_did_google_oauth_verification_actually/)
  - "the estimated timeline was **4–6 weeks**. In practice, the biggest delays came from providing all the
    required information clearly rather than the review itself." — same thread.
- **Common rejection / delay causes** (community): privacy policy not detailed enough or not on the same
  domain / not linked on the consent screen; consent-screen app name mismatching the demo video; weak per-scope
  justification; demo video not clearly showing **each** scope in use; reviewers going quiet then re-asking a
  question already answered. ([r/microsaas](https://old.reddit.com/r/microsaas/comments/1ukb8p7/how_long_did_google_oauth_verification_actually/),
  [gmass.co on OAuth scope verification pain points](https://www.gmass.co/blog/five-annoying-issues-google-oauth-scope-verification/)).

> **Google bottom line:** sensitive, not restricted → **no CASA, free, ~4–6 weeks of paperwork + a demo video +
> a live website w/ privacy policy on a Search-Console-verified domain.** Nothing required until we go public;
> the only testing-phase pain is the 7-day token expiry. Start the website/privacy-policy early — it's the
> gating prerequisite and it's also reusable for Meta later.

---

## 2. Twitch

### 2.1 App registration & scope review  **[verified]**

- **Registration** ([Register Your App](https://dev.twitch.tv/docs/authentication/register-app/)): the Twitch
  account must have **two-factor authentication enabled**; then in the developer console create an application
  (unique name, OAuth redirect URL(s), category, CAPTCHA). You get a **Client ID**; a Client Secret is optional
  and, for our public device-code client, **not shipped** (`needsSecret: false`).
- **No scope review / verification process.** Twitch has **no** Google-style verification gate. Any registered
  app may request any scope; the **user's consent screen** is the only gate. The scopes we use
  (`chat:edit`, `channel:manage:broadcast`, `channel:read:stream_key`, `moderator:read:followers`,
  `channel:read:subscriptions`, `bits:read`, etc.) are all standard user-consented scopes with no app-side
  approval step. ([Twitch scopes reference](https://dev.twitch.tv/docs/authentication/scopes/).) **[verified]**

### 2.2 Redirect URI / loopback  **[verified]**

We use the **Device Code Grant flow** for Twitch, which **does not use a redirect URI at all** — the user enters
a code at `twitch.tv/activate`. So redirect-URI validation rules are moot for us. (Twitch's authorization-code
flow does require an exact registered `redirect_uri` match and historically permits `http://localhost`, but that
path isn't ours.) ([Device Code Grant Flow](https://dev.twitch.tv/docs/authentication/getting-tokens-oauth/#device-code-grant-flow)).

### 2.3 One real consideration — chat send rate limits / verified bot  **[strong]**

Not a "verification" gate, but relevant to our `chat:edit` / `user:write:chat` send path: Twitch rate-limits
chat messages per account, and heavy/automated senders can apply to become a **Known/Verified Bot** for higher
message throughput. For a single-user streaming client sending occasional messages this is a non-issue; flag it
only if we ever fan chat sends at scale. ([Twitch chatbot / rate-limit docs](https://dev.twitch.tv/docs/irc/#rate-limits)).

> **Twitch bottom line:** register an app (2FA required), ship the Client ID. **No verification, no review, no
> cost.** Nothing owed for launch beyond the registration we already have.

---

## 3. Kick

### 3.1 Current state — official API now exists, but young  **[verified state] / [uncertain longevity]**

Kick shipped an **official public OAuth 2.0 API** at [docs.kick.com](https://docs.kick.com/) (superseding the
earlier reverse-engineered era). We register an app in Kick's developer settings to get a client ID/secret and
use PKCE. The scopes we request (`channel:read`, `channel:write`, `user:read`, `chat:write`, `events:subscribe`)
are documented Kick scopes. **[verified against docs.kick.com, per code comments dated 2026-06]**

**Honest uncertainty:**
- Kick's API is **much newer and thinner** than Twitch/YouTube; endpoints and scope names have been moving, and
  parts of our integration (per project memory) touch behavior that has been fragile — e.g. **chat send hitting
  Cloudflare 403s** and a **fixed-port loopback** assumption. These are stability/robustness risks, not
  verification gates. **[uncertain]**
- **Loopback quirk (verified in our code):** Kick's OAuth frontend rewrites a literal `127.0.0.1` redirect host,
  so our `redirect_uri` must advertise `localhost`. Already handled in `kick_provider.cpp`.
- **No formal app-review / verification** program is documented for third-party scopes (as of 2026-06). The main
  risk is **ToS / platform-discretion**: a young platform can change endpoints, tighten access, or revoke app
  credentials with little notice. Treat Kick as best-effort and expect churn. **[uncertain — flagged]**

> **Kick bottom line:** register an app; no verification/audit exists. The real risk is **API immaturity and
> potential breaking changes / ToS discretion**, not a compliance gate. Keep the integration defensively coded
> and be ready to adapt. Re-check docs.kick.com before launch.

---

## 4. Meta / Facebook (future — website-hosted Facebook Login)

**Not built yet; this is planning-level only and warrants its own deep-dive before implementation.** Meta's
process is the **heaviest** of the four. High-level requirements to livestream via Facebook and read/write on a
user's behalf ([App Review](https://developers.facebook.com/docs/resp-plat-initiatives/individual-processes/app-review/submission-guide),
[Permissions Reference](https://developers.facebook.com/docs/permissions/)):

1. **App Review, per permission.** Any permission beyond the basics (e.g. Live-video publishing / `publish_video`,
   page/pages permissions) requires **Advanced Access**, granted only after Meta reviews a **video walkthrough for
   each permission** showing real in-app usage. **[verified]**
2. **Business Verification.** "Business Verification is required for all apps making requests for Advanced
   Access." Expect to submit business/legal documentation. **[verified]**
3. **Data Deletion Callback URL** (or data-deletion instructions URL) — required for apps handling user data.
   **[strong]**
4. **Privacy Policy URL** + valid app domain / platform settings. **[strong]**
5. Live-video APIs specifically sit behind additional product/permission review and Meta has periodically
   restricted or deprecated pieces of the Live API — **verify current availability at implementation time.**
   **[uncertain — flag for the deep-dive]**

> **Meta bottom line:** a multi-week process gated on **Business Verification + per-permission App Review + a
> public website with privacy policy + a data-deletion callback.** No cash cost, but the most bureaucratic path.
> Reuse the same website/privacy-policy asset we build for Google. **Do a dedicated deep-dive before committing.**

---

## Open risks / unknowns

- **[verified] 7-day token expiry during testing is unavoidable** for YouTube while in Testing status with our
  sensitive scope. Mitigation options: (a) publish + verify sooner; (b) tell testers to expect weekly YouTube
  re-auth; (c) keep the tester set ≤100 (the cap). Twitch/Kick tokens are unaffected.
- **[strong] The website + privacy policy is a shared, gating prerequisite** for both Google verification and
  Meta App Review. It doesn't exist yet and is the longest lead-time item. Recommend standing up a simple
  homepage + privacy policy on a domain we can verify in Search Console, well before public launch.
- **[uncertain] Kick API stability.** Young API with known fragile spots (chat-send Cloudflare 403, loopback
  port assumptions). Not a compliance blocker but a launch-reliability risk; re-verify docs.kick.com near launch.
- **[uncertain] Meta Live API scope/availability drift.** Meta has changed Live-video API access before; the
  exact permissions and their review bar must be re-confirmed when that work starts.
- **[verified] Google official "10 days" vs real "4–6 weeks."** Plan the launch runway around 4–6 weeks (or more,
  with back-and-forth), not the optimistic official figure.
- **[not researched here] Privacy-policy CONTENT specifics** (GDPR/CCPA disclosures, Google API *Limited Use*
  wording, Meta data-deletion wording). Each verification checks the policy's substance, not just its existence —
  worth a focused pass when drafting the policy.
- **[note] Twitch/Kick client IDs and Meta app IDs are not secrets**, but the repo's `.env` holds real client
  secrets (Kick `needsSecret:true`; any Meta secret later). Those must stay out of the shipped binary / public
  repo — not a verification requirement, but a security one.

### Sources (primary)
- Google OAuth App Verification Help Center — https://support.google.com/cloud/answer/13463073
- Google Sensitive scope verification — https://developers.google.com/identity/protocols/oauth2/production-readiness/sensitive-scope-verification
- Google Restricted scope verification (CASA) — https://developers.google.com/identity/protocols/oauth2/production-readiness/restricted-scope-verification
- Google OAuth 2.0 Scopes list — https://developers.google.com/identity/protocols/oauth2/scopes
- Google Manage App Audience (test-user cap, 7-day expiry) — https://support.google.com/cloud/answer/15549945
- Google API Services User Data Policy (Limited Use) — https://developers.google.com/terms/api-services-user-data-policy
- Twitch Register Your App — https://dev.twitch.tv/docs/authentication/register-app/
- Twitch scopes reference — https://dev.twitch.tv/docs/authentication/scopes/
- Twitch Device Code Grant Flow — https://dev.twitch.tv/docs/authentication/getting-tokens-oauth/#device-code-grant-flow
- Kick developer docs — https://docs.kick.com/
- Meta App Review — https://developers.facebook.com/docs/resp-plat-initiatives/individual-processes/app-review/submission-guide
- Meta Permissions Reference — https://developers.facebook.com/docs/permissions/

### Sources (community lived-experience)
- r/microsaas "How long did Google OAuth verification actually take you?" — https://old.reddit.com/r/microsaas/comments/1ukb8p7/how_long_did_google_oauth_verification_actually/
- r/SaaS "Going through Google's OAuth verification for Gmail scopes" (CASA $1.5k–3k) — https://old.reddit.com/r/SaaS/comments/1udeu9s/going_through_googles_oauth_verification_for/
- gmass.co "Five annoying issues with Google's OAuth Scope Verification" — https://www.gmass.co/blog/five-annoying-issues-google-oauth-scope-verification/
