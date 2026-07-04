# Branding & Trademark Research: Can we ship as "OBS MultiStreamer"?

**Date:** 2026-07-04
**Author:** Research pass (content/business analyst — *not a lawyer*; see confidence labels and "Verify yourself" section)
**Subject:** Legal safety of a public product / OAuth-app brand name containing "OBS" for this OBS Studio fork.

---

## Executive summary — the bottom line

**No. Do not ship a public product or OAuth app under a name that contains "OBS" (including "OBS MultiStreamer" / "OBS Multistream").** This is a *high-confidence* conclusion, and it is the same answer whether you look at it from trademark law, the OBS Project's own published rules, its demonstrated enforcement behavior, or Google/Meta's OAuth-app branding policies. Every one of those four independent angles points the same way.

Short version of *why*:

1. **"OBS" is a protected mark of the OBS Project, and they enforce it.** They have publicly forced at least two parties to drop "OBS" branding — Streamlabs (2021, a product name) and Fedora (2025, a *fork/repackage*, which is exactly your situation). Your fork is squarely in the fact pattern they have already litigated the loudest about. *(High confidence — direct precedent, cited below.)*
2. **The GPLv2 gives you the code, not the name.** Copyleft licenses grant copyright permissions; they explicitly do **not** grant trademark rights. A GPL fork may reuse the source but generally may **not** brand itself with the upstream project's name/logo. *(High confidence — settled principle.)*
3. **The name becomes your Google/Meta OAuth "app name," and their verification explicitly rejects names that aren't distinctively *yours*.** Google requires the app name/logo to "uniquely identify your brand" and not be "confused with... other organizations' brands." "OBS" is not your brand, so this is a plausible verification-review failure and, worse, an invitation for the real OBS Project to file a trademark complaint against your OAuth app / Facebook Login app. *(Medium-high confidence — policy text is clear; whether a specific reviewer catches it is probabilistic.)*
4. **"Multistream" alone is descriptive/generic** and used generically by Restream, Streamlabs, and StreamYard — it's weak as a trademark and won't distinguish you. Dropping "OBS" *and* leaning only on "Multistream" is not a fix. *(High confidence on descriptiveness; a coined brand is advisable.)*

**Safe pattern:** pick a **distinctive, coined standalone brand** (not "OBS…", not just "Multistream"), and *describe* the product in body copy as **"built on OBS Studio" / "an open-source fork of OBS Studio"** under nominative fair use — never putting "OBS" in the product name, logo, domain, or OAuth app name. Candidate directions are in the final section, with the caveat that **you must run your own trademark + domain clearance** before committing.

**Is the current "OBS MultiStreamer" name safe to keep for a public / OAuth-facing launch? No — it is not safe.** It should be changed before any public website, OAuth verification submission, or Facebook Login app review.

---

## Q1 — Is "OBS" / "OBS Studio" a trademark? Owned by whom? What does the policy forbid for forks?

**Findings:**

- **"OBS" functions as a trademark of the OBS Project** (the org behind OBS Studio; Joel Bethke is a lead / "project manager" figure who has signed enforcement demands). OBS Studio is GPLv2+ open-source code, but the *name and logo* are treated by the Project as its protected marks, separate from the code license.
- **Registration status is muddy, and this matters.** A USPTO search for "OBS" surfaces marks owned by **unrelated** entities — e.g. "OBS" by *OBS Holding Company, LLC* (Las Vegas; serial 90002531, reported **abandoned**) and *Wizards of OBS LLC* (serial 88436186) — these are **not** the streaming-software OBS Project. The one clearly on-point federal registration born of the dispute is **"STREAMLABS OBS"** (General Workings Inc., reg. 6097322 / serial 88296466), i.e. the *other side's* filing. I did **not** find a clean, live USPTO word-mark registration for "OBS Studio" owned by the OBS Project itself. **Best assessment: the OBS Project relies primarily on *common-law* (unregistered) trademark rights in "OBS" / "OBS Studio," backed by long, continuous, well-known use — plus whatever they secured around the Streamlabs phrase.** Common-law marks are still enforceable; they are just territorially narrower and must be proven by use rather than a registration certificate. *(Confidence: high that "OBS" is at minimum a strong common-law mark; medium on the exact registration status — this is the single item most worth a professional trademark search to pin down.)*
- **The OBS Project's own published rule** (Forum Terms & Rules) states you **"may not under any circumstances use the OBS branding in whole or in part for"** self-promotion, affiliation, or representation, nor impersonate OBS Project contributors. The operative phrase is **"in whole or in part"** — that reaches *"OBS MultiStreamer,"* which uses the branding *in part*. *(High confidence — direct quote from their site.)*
- **There is no permissive "forks may use the OBS name" carve-out.** I found no OBS trademark policy that grants forks the right to keep "OBS" in a product name. Their demonstrated position (Q3) is the opposite: forks/repackages must **remove** OBS branding.

**Citations:**
- OBS Forums — Terms and rules (the "OBS branding in whole or in part" prohibition): https://obsproject.com/forum/help/terms/
- OBS Project overview / leadership: https://obs-versions.com/blog/obs-project
- USPTO "OBS" (OBS Holding Company, LLC — unrelated, abandoned): https://uspto.report/TM/90002531
- USPTO "OBS" (Wizards of OBS LLC — unrelated): https://uspto.report/TM/88436186
- USPTO "STREAMLABS OBS" (General Workings Inc.): https://uspto.report/TM/88296466 · https://trademarks.justia.com/882/96/streamlabs-88296466.html

---

## Q2 — GPL vs. trademark: does the license let a fork use the name?

**The principle is settled: no.** An open-source *copyright* license (GPLv2/GPLv3/AGPL) grants rights to **copy, modify, and redistribute the code**. It does **not** transfer, license, or waive the project's **trademark** rights in its name or logo. Trademark is a separate legal regime (source-identification / consumer-confusion) from copyright (the code), and copyleft licenses are silent on — and do not grant — it. The FSF itself models this exact separation: it licenses the GPL *text* under copyleft-style terms while **retaining** its own (common-law) trademarks on the "GNU"/"GPL" names, and it "does not allow... confusing uses of the FSF's trademarks." The practical upshot for you: **you may lawfully fork and ship OBS Studio's GPLv2+ code; you may not lawfully brand that fork "OBS \<anything\>."**

This is also *why* the OBS↔Fedora fight (Q3) was even possible: Fedora had every GPL right to build and ship the code, and the OBS Project still validly demanded removal of the **name and logo** — because those were never covered by the GPL grant.

**Citations:**
- FSF — Principles of community-oriented GPL enforcement (GPL is a copyright-enforcement instrument; trademarks are handled separately): https://www.fsf.org/licensing/enforcement-principles
- Software Freedom Conservancy — same principles: https://sfconservancy.org/copyleft-compliance/principles.html
- TermsFeed — "Protecting Your Brand in Open Source: Trademarks, Forks, and Enforcement" (forks should choose names that clearly set them apart; the license ≠ the brand): https://www.termsfeed.com/blog/open-source-trademark/

---

## Q3 — Real-world enforcement: does the OBS Project actually make people rename?

**Yes — repeatedly, publicly, and specifically against the two patterns that describe you (a product using "OBS" in its name, and a fork/repackage of OBS Studio).** This is the strongest single reason not to keep "OBS" in your name: it is not a theoretical risk, it is demonstrated behavior within the last few years.

**Precedent 1 — Streamlabs (2021):** The OBS Project publicly stated it had **asked Streamlabs (Logitech-owned) to remove "OBS" from its product name** "Streamlabs OBS." Streamlabs instead trademarked "Streamlabs OBS"; the OBS Project pushed back publicly, and under community pressure Streamlabs announced **"We are taking immediate action to remove OBS from our name"** — roughly **15 hours** after the OBS Project went public. This is a *commercial product name using "OBS"* being forced off it. *(High confidence.)*

**Precedent 2 — Fedora (Feb 2025) — this is *your* fact pattern:** OBS lead Joel Bethke sent Fedora a **formal legal demand**: *"This is a formal request to remove all of our branding, including but not limited to, our name, our logo, any additional IP belonging to the OBS Project, from your distribution,"* adding *"Failure to comply may result in further legal action taken."* He gave a **seven-business-day deadline (Feb 21, 2025)** and characterized the poorly-maintained downstream Flatpak as a **"hostile fork."** Fedora complied by moving the package to end-of-life / removing it. The trigger was **user confusion** — people hit bugs in the repackaged build and blamed the real OBS Project. *(High confidence — direct quotes.)*

**Community/"lived reality" read (Reddit):** The Streamlabs episode is the dominant community memory of OBS-name enforcement — top threads across r/Twitch ("Streamlabs were told not to use the OBS name but did anyway and then filed a trademark"), r/pcgaming, and r/LivestreamFail all frame it as OBS defending its trademark, and community sentiment broadly sided *with* the OBS Project. The takeaway from both the drama and the enforcement record: small hobby tools with "OBS" in a plugin/script name often fly under the radar, **but the moment something looks like a product, has a website, an OAuth login, or repackages OBS Studio itself, the Project engages — and wins, fast.** Your project (public site + OAuth apps + a full fork) is firmly on the "engages" side of that line. *(Medium-high confidence on the sentiment read; high on the enforcement facts.)*

**Citations:**
- The Register — "Streamlabs changes name after complaint from OBS Project" (2021-11-18): https://www.theregister.com/2021/11/18/streamlabs_drops_obs/
- Inven Global — OBS accuses Streamlabs of stealing name/trademark: https://www.invenglobal.com/articles/15737/obs-project-accuses-streamlabs-of-stealing-their-name-and-trademark
- It's FOSS — Fedora feud, exact demand text + "hostile fork" + 7-day deadline: https://itsfoss.com/news/obs-studio-fedora-feud/
- GamingOnLinux — Fedora threatened over Flatpak packaging: https://www.gamingonlinux.com/2025/02/fedora-threatened-with-legal-action-from-obs-studio-due-to-their-flatpak-packaging/
- heise online — OBS & Fedora reach deal: https://www.heise.de/en/news/After-threats-of-legal-action-OBS-and-Fedora-reach-deal-in-Flatpak-dispute-10287893.html
- Reddit r/Twitch: https://old.reddit.com/r/Twitch/comments/qvo5mf/streamlabs_were_told_not_to_use_the_obs_name_but/ · r/pcgaming: https://old.reddit.com/r/pcgaming/comments/qvtd9y/ · r/LivestreamFail: https://old.reddit.com/r/LivestreamFail/comments/qvpct5/

---

## Q4 — Google & Meta OAuth app-name / branding policy

Because your brand name becomes the **OAuth consent-screen app name** (Google/YouTube) and later the **Facebook Login app name** (Meta), it must pass each platform's *branding verification*, which independently penalizes using a third party's brand you don't own.

**Google (YouTube Data API / Google Sign-In OAuth):**
- Apps shown to external users that display a name/logo on the consent screen **require brand verification**; changing the name/logo forces re-verification before it goes live.
- Google's App Identity & Branding rules: your app name and logo must **"uniquely identify your brand and identity"** and **cannot suggest you represent another organization.** Google instructs developers to *"Choose an app name that distinctively represents your business"* and to **not use names that "may be confused with... other organizations' brands."** Verification is done specifically **"to ensure you are not impersonating another brand or organization."**
- **Application to "OBS MultiStreamer":** "OBS" is a third party's brand, not yours. This app name (a) does **not** uniquely identify *your* brand, and (b) is confusable with / implies affiliation to the OBS Project. That is a plausible **verification rejection**, and independently it is exactly the kind of thing the OBS Project can report to Google as trademark impersonation to get the app suspended. *(Medium-high confidence: the written policy clearly covers this; whether an individual reviewer flags it on first pass is probabilistic, but the downside risk — mid-launch suspension after users depend on it — is severe.)*

**Meta (Facebook Login App Review):**
- Meta prohibits content/accounts/apps that infringe a third party's trademark or that impersonate or misrepresent identity ("Authentic Identity Representation"; IP policy), and runs Brand Rights Protection tooling that trademark owners use to report **impersonation/trademark** misuse of their brand in app names.
- **Application:** same exposure — an "OBS …" app display name is reportable by the OBS Project and cuts against Meta's authentic-identity / no-impersonation rules. *(Medium confidence — Meta's app-name enforcement is less mechanically documented than Google's, but the trademark-report and impersonation surfaces clearly reach it.)*

**Net:** Both platforms give the *real* trademark owner a fast, self-service lever to disrupt your login integration if your app name borrows their brand. Shipping OAuth under "OBS MultiStreamer" puts your entire multi-platform login flow at the mercy of an OBS Project complaint.

**Citations:**
- Google — Submit for brand verification: https://developers.google.com/identity/protocols/oauth2/production-readiness/brand-verification
- Google — App Identity & Branding (Cloud Console Help): https://support.google.com/cloud/answer/13804963?hl=en
- Google — Manage OAuth App Branding: https://support.google.com/cloud/answer/15549049?hl=en
- Google — Comply with OAuth 2.0 policies: https://developers.google.com/identity/protocols/oauth2/production-readiness/policy-compliance
- Meta — Intellectual property across Meta platforms: https://www.meta.com/help/policies/3234337743488413/
- Meta — Authentic Identity Representation: https://transparency.meta.com/policies/community-standards/authentic-identity-representation/
- Meta — Trademark reporting: https://www.facebook.com/help/507663689427413

---

## Q5 — Is "Multistream" / "MultiStreamer" alone safe or ownable?

**Safe-ish to *use*, but weak to *own*, and it won't distinguish you.** "Multistream" / "multistreaming" is a **descriptive-to-generic** term for the act of broadcasting one source to multiple destinations — the exact function of your product. Under US trademark law, **merely descriptive** marks are refused registration on the Principal Register unless they acquire **secondary meaning** (proof that consumers associate the term with *you* specifically — expensive and slow to establish), and **generic** terms can't be trademarked at all. "Multistream" sits near that generic line because the industry already uses it as a common noun/verb:

- **Restream** markets "Create and Multistream Live Video";
- **Streamlabs** has a feature/page literally called **"Multistream"** (multistream to Twitch/YouTube/TikTok);
- **StreamYard** and others use "multistream/multistreaming" generically in copy.

So "MultiStreamer" as your *primary* brand would be (a) hard to protect, (b) easily confused with several incumbents, and (c) not distinctive enough to build equity or clear OAuth "uniquely identifies your brand" review comfortably. **A distinctive, coined (or arbitrary/suggestive) brand is advisable** — you can then use "multistream/multistreaming" freely as a plain-English *description* of what the app does, without relying on it as the trademark.

**Citations:**
- BitLaw — Strength of trademarks (generic < descriptive < suggestive < arbitrary/fanciful): https://www.bitlaw.com/trademark/degrees.html
- Cohn Legal — Descriptive vs. generic trademarks: https://www.cohnlg.com/the-differences-between-descriptive-and-generic-trademarks/
- Justia — Generic terms excluded from protection: https://www.justia.com/intellectual-property/trademarks/strength-of-marks/generic-terms/
- Restream: https://restream.io/ · Streamlabs Multistream: https://streamlabs.com/multistream · StreamYard: https://streamyard.com/blog/multistream-to-twitch

---

## Q6 — Recommendation

### (a) Can you keep "OBS" in the name? — No (public product / OAuth). *High confidence.*
For a public product with a website, OAuth consent screens, and a Facebook Login app, keeping "OBS" in the name is not safe. It conflicts with the OBS Project's published branding rule ("in whole or in part"), matches the exact fact patterns they've enforced against (Streamlabs product-name, Fedora fork), isn't yours to use under the GPL, and creates a standing takedown lever inside Google/Meta OAuth verification. **Change it before any public/OAuth-facing launch.** (For a purely *private*, non-distributed dev build with no OAuth and no website, the practical risk is low — but that's not where this project is headed.)

### (b) The safe pattern
1. **Product name / logo / domain / OAuth app name:** a **distinctive, coined standalone brand** — no "OBS," and not merely "Multistream."
2. **Body copy / docs / README / store listing:** you *may* truthfully say **"built on OBS Studio," "an open-source fork of OBS Studio," or "OBS Studio–compatible,"** under **nominative fair use** — the doctrine that lets you name another's trademark to accurately describe your product's origin/compatibility. Keep it factual and non-prominent: use OBS's name only as much as needed to identify the upstream, never styled as *your* logo, never implying endorsement/affiliation, and ideally with a short disclaimer ("not affiliated with or endorsed by the OBS Project"). *(Medium-high confidence — nominative fair use is well-established, but its boundaries are fact-specific; keep OBS out of the name/logo/domain and you stay well inside it.)*
3. **GPL compliance stays intact regardless** — keep the upstream copyright notices, license text, and source availability. That's a *copyright* obligation and is unaffected by the rebrand.

### (c) Example brand-name directions (distinctive; illustrative only)
These are *directions to explore*, chosen to be suggestive/coined rather than descriptive. **None are cleared** — see "Verify yourself."
- **Coined / arbitrary (strongest legally):** *Castr* (note: an existing multistreaming service already uses "Castr" — avoid), *Streamforge*, *Beacan*, *Relayo*, *Nimbcast*.
- **Suggestive (evokes the function without describing it):** *Fanout* / *FanoutTV*, *Everywhere Live*, *OmniCast*, *Broadwave*, *Simulstage*.
- **Concept anchors you can build a coined mark around:** "fan-out / one-to-many" (your actual encode-once architecture), "relay," "beacon," "simulcast." Turn one into a coined spelling for distinctiveness (e.g. *Fanout* → *Fanaut*, *Relay* → *Relayo*).

Pick something short, pronounceable, spellable-from-hearing, with a plausible `.com`/`.tv`/`.io`, and *not* echoing an incumbent (Restream, Streamlabs, StreamYard, Castr, OneStream, etc.).

### Verify yourself (do NOT skip — this research is not a legal clearance)
1. **Professional trademark clearance search** on your shortlisted name — USPTO TESS/EUIPO + common-law/web use — ideally via a trademark attorney, in every class/region you'll operate. (Screening tools: uspto.gov, euipo.europa.eu, trademarkia.com.)
2. **Domain + social handle availability** for the finalists (`.com` at minimum, plus `.tv`/`.io`; matching handles on X/YouTube/Twitch/GitHub).
3. **Pin down OBS's exact registration status** (open item from Q1) if you ever need to argue the boundary — but note this does **not** change the recommendation; common-law rights + demonstrated enforcement already make "OBS" unsafe to use.
4. **Confirm nominative-fair-use wording** with counsel before publishing marketing that names "OBS Studio," and add a non-affiliation disclaimer.
5. **Re-run Google/Meta brand verification** against the *final* name/logo early (before you have users depending on the login flow), since any later name change forces re-verification.

---

## Confidence summary
| Claim | Confidence | Basis |
|---|---|---|
| "OBS" is a protected mark the Project actively enforces | **High** | Their forum terms + Streamlabs (2021) + Fedora (2025) |
| GPL grants code, not the name/logo | **High** | Settled principle; FSF/SFC + own trademark model |
| Forks must drop OBS branding (matches your case) | **High** | Fedora "hostile fork" demand, direct quotes |
| "OBS MultiStreamer" would jeopardize Google/Meta OAuth verification | **Medium-high** | Policy text clear; reviewer catch is probabilistic; owner-report lever is real |
| Exact OBS Project USPTO registration status | **Medium** | No clean live word-mark for OBS Project found; likely common-law — needs professional search |
| "Multistream" is descriptive/weak; coined brand advisable | **High** | Descriptiveness doctrine + generic industry use |
| Specific example names are *available* | **Not verified** | Illustrative only — clearance is the user's to run |

*Prepared as researched guidance, not legal advice. Engage a trademark attorney before final naming and before OAuth verification submission.*
