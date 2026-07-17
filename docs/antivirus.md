# Antivirus / SmartScreen false positives — release runbook

Braidcast is an OBS Studio fork, and OBS-class capture software is an inherent
antivirus-heuristic magnet. This is a chronic, expected cost — not a bug to fix
once — so it is handled as a repeatable per-release process, exactly as OBS,
FanControl, and similar projects do.

## Why it happens (root cause, ranked)

1. **Runtime behavior — the dominant factor.** Braidcast injects DLLs into other
   processes (game/window capture hooks), hooks Windows APIs, captures screen +
   audio, and installs a virtual-camera driver. Those are the exact behaviors AV
   heuristics are built to catch, **regardless of code signature**. OBS's own
   maintainer: *"Code signing is a way to allow folks to eventually whitelist
   binaries; it does nothing to solve the first issue when you release software."*
2. **Low prevalence / reputation.** SmartScreen and Norton's Insight/FileRep
   model score a binary by how many users have downloaded and cleanly run that
   exact hash. A brand-new release hash is "uncommon" → flagged, independent of
   signing. Prevalence accrues over time and is *accelerated* by the submissions
   below.
3. **Missing signature.** A gating prerequisite — it lets vendors whitelist a
   publisher and lets reputation start accruing — but it is not itself a
   heuristic-suppression mechanism. See [signing.md](./signing.md).

Implication: **signing alone will not clear the AV problem.** The free
submissions below do the near-term heavy lifting; signing is the longer-term
prerequisite that makes the whitelisting durable.

## Per-release checklist

Run every time a new `v*` release is cut (the release ships from
`release-braidcast.yaml`).

### 1. Diagnose with VirusTotal (free)

Upload **both** artifacts **separately**:
- the portable `braidcast-<ver>-windows-x64.zip`'s `braidcast.exe`, and
- the NSIS installer `braidcast-<ver>-windows-x64.exe`.

Read the result as a diagnostic:
- **Installer flags but the portable exe is clean** → the **NSIS installer stub**
  is the trigger (some vendors signature the stub itself, not the payload). Act on
  the NSIS note below.
- **Both flag** → it's the **app behavior** (hooks/capture/driver), not packaging.
  Submissions + prevalence + signing are the levers; NSIS changes won't help.

Record which engines flagged so submissions target the right vendors.

### 2. Submit false-positive disputes (free)

Norton is the primary complaint, so it is first. Attach the flagged artifact and
state it is an open-source OBS Studio fork (link the repo).

| Vendor | Submit at | Typical turnaround |
| --- | --- | --- |
| **Norton / Symantec** (Broadcom) | https://symsubmit.symantec.com/ (or `false.positives@broadcom.com`) | ~2 business days |
| **Microsoft Defender / SmartScreen** | https://www.microsoft.com/en-us/wdsi/filesubmission | ~1–3 days |
| **Avast / AVG** | whitelisting / false-positive form | ~24 h |
| **Kaspersky** | https://opentip.kaspersky.com/ | ~1–3 days |
| **BitDefender / ESET / McAfee** | per-vendor FP forms — see the consolidated list | varies |

Consolidated vendor list + links: `yaronelh/False-Positive-Center` on GitHub.

### 3. Track

Keep a short log (release → which engines flagged → submitted → cleared) so a
recurring offender is visible and prevalence progress is measurable.

## NSIS installer note

Packaging is a standard CPack NSIS generator (`cmake/windows/cpackconfig.cmake`),
no custom stub, built with whatever `makensis` the CI runner ships (a modern
NSIS 3.x, else latest via Chocolatey — `.github/scripts/Package-Windows.ps1`).
Modern 3.x stubs are largely whitelisted, so the stub is an unlikely primary
cause — but if step 1 isolates the installer, options are: pin/upgrade the NSIS
version, change compression (`CPACK_NSIS` / solid LZMA), or switch installer
generator. Pinning the NSIS version also makes per-release VirusTotal results
comparable.

## Relationship to signing

Signing (see [signing.md](./signing.md)) is pursued in parallel as the
prerequisite that unlocks durable whitelisting and reputation — **not** as the
thing that stops heuristic flags. The realistic path out of the
"unsigned → no users → no reputation → can't qualify for free signing" catch-22
is: free FP-submissions + prevalence first, then SignPath's free OSS certificate
once prevalence clears their reputation gate.
