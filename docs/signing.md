# Windows Code Signing (SignPath)

Braidcast ships Windows-only today, unsigned. `.github/workflows/release-braidcast.yaml`
has a `sign-windows` job wired up for [SignPath](https://signpath.io/)'s free
open-source signing program, gated behind the `SIGNPATH_API_TOKEN` secret — until
that secret exists, the job no-ops and the draft release still ships with the
current unsigned zip/installer. This doc is the runbook for turning it on.

## 1. Apply to the SignPath Foundation OSS program

Apply at [signpath.org](https://signpath.org/) (the OSS-sponsorship arm of
SignPath, distinct from the paid `signpath.io` product). Eligibility (per
[signpath.org/terms.html](https://signpath.org/terms.html)):

- OSI-approved open-source license for the whole project, no proprietary or
  commercially dual-licensed components. Braidcast is GPL-2.0 — qualifies.
- Publicly available source repository. Braidcast's repo is public — qualifies.

Approval is manual and not instant; budget for a wait before secrets exist.

## 2. After approval: org, project, signing policy, artifact configuration

Once approved, in the SignPath dashboard:

1. **Create an organization** (or use the one SignPath Foundation provisions on
   approval) and a **project** for `braidcast-studio`.
2. **Create a signing policy** — the policy that authorizes release-grade Windows
   Authenticode signing (OV cert; see the caveat below). Every signing request
   against this policy needs manual approval per SignPath OSS program rules.
3. **Create an artifact configuration** that recursively signs everything the CI
   job submits. The job stages and submits **one artifact** containing both the
   portable zip (`braidcast-<version>-windows-x64.zip`) and the NSIS installer
   (`braidcast-<version>-windows-x64.exe`) — mirroring upstream OBS's `bouf`
   packaging approach (`sign_exts = ['exe', 'dll', 'pyd']`, everything signed
   before/while packing). The artifact configuration must:
   - Treat the `.exe` as an **NSIS installer container** and the `.zip` as a
     **zip container**, both signed recursively.
   - Sign every `.exe` and `.dll` found inside each container — this covers
     `braidcast.exe`, `graphics-hook*.dll`, `obs-*.dll`, the win-capture DLLs,
     and every other bundled plugin/hook DLL, plus the outer installer `.exe`
     and zip contents themselves.
   - Preserve the input file/directory structure in the signed output artifact
     (the release job matches signed files back out by the same
     `braidcast-*-windows-x64.{zip,exe}` glob it used going in).
4. Note the **organization ID**, **project slug**, **signing policy slug**, and
   **artifact configuration slug** — they map directly to the GitHub Actions
   variables in step 4.

## 3. Create a CI user + API token

Create a SignPath user dedicated to CI (not a personal account) with submitter
permissions on the project/signing policy from step 2, then issue an API token
for it. See [SignPath's API token docs](https://about.signpath.io/redirects/connectors/api-token).

MFA is required on both the SignPath account and the GitHub account/org that
can modify these secrets, per the SignPath OSS program's security requirements.

## 4. Add the GitHub secrets and variables

In the `braidcast/braidcast-studio` repo settings (Settings → Secrets and
variables → Actions), add:

| Name | Kind | Value |
| --- | --- | --- |
| `SIGNPATH_API_TOKEN` | Secret | The CI user's API token from step 3. |
| `SIGNPATH_ORGANIZATION_ID` | Secret **or** variable | The SignPath organization ID. Not especially sensitive (it's a GUID visible in SignPath URLs), so either works — the workflow reads `secrets.SIGNPATH_ORGANIZATION_ID \|\| vars.SIGNPATH_ORGANIZATION_ID`. |
| `SIGNPATH_PROJECT_SLUG` | Variable | The project slug from step 2. |
| `SIGNPATH_SIGNING_POLICY_SLUG` | Variable | The signing policy slug from step 2. |
| `SIGNPATH_ARTIFACT_CONFIG_SLUG` | Variable | The artifact configuration slug from step 2. |

Once `SIGNPATH_API_TOKEN` exists, the next `v*` tag push runs `sign-windows` for
real: it submits the packaged zip+installer via
[`signpath/github-action-submit-signing-request`](https://github.com/SignPath/github-action-submit-signing-request)
(pinned in `release-braidcast.yaml`), waits for the signing request to be
approved and completed, and `create-release` attaches the signed files to the
draft release instead of the unsigned ones. Until then, every release ships
unsigned exactly as it does today — no CI failure, just an `::warning::`
annotation on the run and unsigned assets on the release.

## Caveat: OV vs EV, SmartScreen ramp-up

SignPath's OSS program issues an **OV-class** (Organization Validation)
certificate, not EV (Extended Validation). Practical effect:

- The binary **is** cryptographically signed and shows a verified publisher —
  no "Unknown Publisher" warning.
- SmartScreen/Norton reputation for an OV cert **ramps up over days to weeks**
  of accumulated downloads/telemetry — it is not instant. Expect early
  SignPath-signed releases to still trip a SmartScreen "Windows protected your
  PC" prompt until enough install telemetry accumulates against the cert.
- **EV certs no longer bypass SmartScreen.** That instant-reputation behavior
  was removed in 2024 — EV-signed files now go through the same per-file-hash
  reputation ramp as OV. So the EV premium ($400+/yr, plus a registered legal
  entity to purchase) buys **nothing** for the SmartScreen problem; not worth
  pursuing for that reason. (Source: Microsoft,
  [Code signing options](https://learn.microsoft.com/en-us/windows/apps/package-and-deploy/code-signing-options).)

This is a real limitation to communicate to users during the ramp-up window,
not a bug in the CI wiring.

## The one free path with zero SmartScreen warnings: the Microsoft Store (MSIX)

Every option above (SignPath OV, Azure Artifact Signing, OV, EV) still trips a
SmartScreen prompt until per-file reputation accrues. **The only free path that
shows no SmartScreen warning from day one is publishing an MSIX through the
Microsoft Store** — Microsoft re-signs the package after certification, no cert
to buy or manage, Store dev account is free. Per Microsoft: *"If you publish
your app as an MSIX package through the Microsoft Store, code signing is free and
handled for you automatically."* This only covers the Store MSIX bundle — the
direct-download `.exe`/`.zip` from GitHub releases still needs SignPath (above).
The MSIX packaging + virtualcam-manifest work is tracked in
[roadmap.md](./roadmap.md) ("Microsoft Store / MSIX"). Note this fixes only the
**SmartScreen** half — Norton/AV heuristic flags are behavior-driven and are
handled separately in [antivirus.md](./antivirus.md).

## Non-Store alternative: Azure Artifact Signing (formerly Trusted Signing)

Microsoft's own signing service, renamed from **Trusted Signing** to **Azure
Artifact Signing**. ~$9.99/month, CI-integrated (no hardware token), OV-level
SmartScreen behavior (reputation ramp, not instant). **Geo-gated:** organizations
in USA/Canada/EU/UK; **individual developers only in USA and Canada.** An
individual maintainer outside US/Canada cannot use it — SignPath (free OSS) or a
traditional OV cert are the alternatives. Not currently pursued for Braidcast
(SignPath is free and covers the same OV-level ground).

## Reference

- SignPath GitHub Actions docs: <https://docs.signpath.io/trusted-build-systems/github>
- SignPath Foundation (OSS program) terms: <https://signpath.org/terms.html>
- `signpath/github-action-submit-signing-request` action source: <https://github.com/SignPath/github-action-submit-signing-request>
- Upstream OBS's own Windows-signing approach (`bouf`), which this fork's
  artifact-configuration mirrors in spirit: `.github/actions/windows-signing/`
  in `obsproject/obs-studio` (Braidcast's own copy of this machinery was
  deleted — see `docs/roadmap.md`).
