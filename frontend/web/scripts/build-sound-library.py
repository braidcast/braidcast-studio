"""Build the bundled alert sound library from Kenney's CC0 packs.

Normalizes each selected clip to -16 LUFS (true peak under -1.5 dBTP), encodes it as OGG
Vorbis, and writes the manifest the overlay runtime and the editor read.

    python3.11 scripts/build-sound-library.py --packs <dir>

<dir> holds one folder per pack, named by the pack's slug (interface-sounds, ...), each the
unzipped kenney_<slug>.zip. Needs ffmpeg (with libvorbis) and ffprobe on PATH. Stdlib only.
"""

from __future__ import annotations

import argparse
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
OUT_DIR = HERE.parent / "public" / "overlay" / "library"

TARGET_LUFS = -16.0
TRUE_PEAK_DBTP = -1.5
# Short UI clips are measured over a window padded to 3 s, because EBU R128 gates in 400 ms
# blocks and a 120 ms click fills none of them. 1.5 LU is under the ~2 LU step listeners
# notice between consecutive alerts.
LUFS_TOLERANCE = 1.5
# Vorbis can add a few tenths of a dB of true peak over the PCM it was given.
TRUE_PEAK_CEILING_AFTER_ENCODE = -1.0
LIMITER_CEILING_DBFS = -2.0
MAX_PASSES = 3
DEFAULT_CAP_MS = 3000
CAP_MS = {"jingle": 6000}
SAMPLE_RATE = 48000
VORBIS_QUALITY = "5"

# slug -> (name, page, zip). The zips are the ones each page linked on 2026-10-02.
PACKS = {
    "interface-sounds": (
        "Interface Sounds",
        "https://kenney.nl/assets/interface-sounds",
        "https://kenney.nl/media/pages/assets/interface-sounds/fa43c1dd4d-1677589452/kenney_interface-sounds.zip",
    ),
    "digital-audio": (
        "Digital Audio",
        "https://kenney.nl/assets/digital-audio",
        "https://kenney.nl/media/pages/assets/digital-audio/216eac4753-1677590265/kenney_digital-audio.zip",
    ),
    "impact-sounds": (
        "Impact Sounds",
        "https://kenney.nl/assets/impact-sounds",
        "https://kenney.nl/media/pages/assets/impact-sounds/87b4ddecda-1677589768/kenney_impact-sounds.zip",
    ),
    "casino-audio": (
        "Casino Audio",
        "https://kenney.nl/assets/casino-audio",
        "https://kenney.nl/media/pages/assets/casino-audio/2472606a04-1721639069/kenney_casino-audio.zip",
    ),
    "music-jingles": (
        "Music Jingles",
        "https://kenney.nl/assets/music-jingles",
        "https://kenney.nl/media/pages/assets/music-jingles/f37e530b9e-1677590399/kenney_music-jingles.zip",
    ),
}

CATEGORIES = {
    "chime": "Chimes",
    "coin": "Coins & Rewards",
    "jingle": "Fanfare & Jingles",
    "arcade": "Arcade",
    "whoosh": "Whoosh & Swipe",
    "impact": "Impact",
}

# Ids are stable: stored settings name them as "library:<id>", so a replacement keeps the id
# and changes only the source row.
SELECTION = [
    ("chime-01", "Bright confirm", "interface-sounds", "Audio/confirmation_002.ogg"),
    ("chime-02", "Two-note confirm", "interface-sounds", "Audio/confirmation_004.ogg"),
    ("chime-03", "Rising question", "interface-sounds", "Audio/question_001.ogg"),
    ("chime-04", "Soft question", "interface-sounds", "Audio/question_002.ogg"),
    ("chime-05", "Glass tap", "interface-sounds", "Audio/maximize_009.ogg"),
    ("chime-06", "Select ping", "interface-sounds", "Audio/select_003.ogg"),
    ("coin-01", "Power up", "digital-audio", "Audio/powerUp1.ogg"),
    ("coin-02", "Long power up", "digital-audio", "Audio/powerUp3.ogg"),
    ("coin-03", "Quick reward", "digital-audio", "Audio/powerUp5.ogg"),
    ("coin-04", "Bonus", "digital-audio", "Audio/powerUp9.ogg"),
    ("coin-05", "Level up", "digital-audio", "Audio/powerUp12.ogg"),
    ("coin-06", "Three-tone reward", "digital-audio", "Audio/threeTone2.ogg"),
    ("jingle-01", "8-bit fanfare", "music-jingles", "Audio/8-Bit jingles/jingles_NES00.ogg"),
    ("jingle-02", "8-bit victory", "music-jingles", "Audio/8-Bit jingles/jingles_NES13.ogg"),
    ("jingle-03", "Pizzicato flourish", "music-jingles", "Audio/Pizzicato jingles/jingles_PIZZI03.ogg"),
    ("jingle-04", "Sax sting", "music-jingles", "Audio/Sax jingles/jingles_SAX07.ogg"),
    ("jingle-05", "Steel drum run", "music-jingles", "Audio/Steel jingles/jingles_STEEL07.ogg"),
    ("jingle-06", "Hit jingle", "music-jingles", "Audio/Hit jingles/jingles_HIT15.ogg"),
    ("arcade-01", "Laser", "digital-audio", "Audio/laser1.ogg"),
    ("arcade-02", "Phaser up", "digital-audio", "Audio/phaserUp3.ogg"),
    ("arcade-03", "Pep", "digital-audio", "Audio/pepSound1.ogg"),
    ("arcade-04", "Zap up", "digital-audio", "Audio/zapThreeToneUp.ogg"),
    ("arcade-05", "Low three-tone", "digital-audio", "Audio/lowThreeTone.ogg"),
    ("arcade-06", "Phase jump", "digital-audio", "Audio/phaseJump1.ogg"),
    ("whoosh-01", "Rise", "interface-sounds", "Audio/maximize_004.ogg"),
    ("whoosh-02", "Fall", "interface-sounds", "Audio/minimize_004.ogg"),
    ("whoosh-03", "Quick drop", "interface-sounds", "Audio/minimize_009.ogg"),
    ("whoosh-04", "Long scroll", "interface-sounds", "Audio/scroll_005.ogg"),
    ("whoosh-05", "Scratch", "interface-sounds", "Audio/scratch_005.ogg"),
    ("whoosh-06", "Card pull", "casino-audio", "Audio/cards-pack-take-out-2.ogg"),
    ("impact-01", "Metal tap", "impact-sounds", "Audio/impactMetal_light_000.ogg"),
    ("impact-02", "Metal ring", "impact-sounds", "Audio/impactMetal_light_002.ogg"),
    ("impact-03", "Metal knock", "impact-sounds", "Audio/impactMetal_light_004.ogg"),
    ("impact-04", "Plank thud", "impact-sounds", "Audio/impactPlank_medium_001.ogg"),
    ("impact-05", "Plank knock", "impact-sounds", "Audio/impactPlank_medium_003.ogg"),
    ("impact-06", "Glass clink", "impact-sounds", "Audio/impactGlass_light_002.ogg"),
]

LOUDNORM_REPORT = re.compile(r"\{[^{}]*\"input_i\"[^{}]*\}", re.S)


def run(args: list[str]) -> str:
    done = subprocess.run(args, capture_output=True, text=True, encoding="utf-8", errors="replace")
    if done.returncode != 0:
        raise RuntimeError(f"{args[0]} exited {done.returncode}: {done.stderr[-800:]}")
    return done.stdout + done.stderr


def duration_ms(path: Path) -> int:
    out = run(["ffprobe", "-v", "error", "-show_entries", "format=duration", "-of", "csv=p=0", str(path)])
    return round(float(out.strip()) * 1000)


def measure(path: Path) -> tuple[float, float]:
    """Integrated loudness (LUFS) and true peak (dBTP) over a window padded to 3 s."""
    log = run([
        "ffmpeg", "-hide_banner", "-nostats", "-i", str(path),
        "-af", f"apad=whole_dur=3,loudnorm=I={TARGET_LUFS}:TP={TRUE_PEAK_DBTP}:LRA=11:print_format=json",
        "-f", "null", "-",
    ])
    match = LOUDNORM_REPORT.search(log)
    if not match:
        raise RuntimeError(f"no loudnorm report for {path}")
    report = json.loads(match.group(0))
    return float(report["input_i"]), float(report["input_tp"])


def render(src: Path, dst: Path, gain_db: float, cap_ms: int) -> None:
    limit = 10 ** (LIMITER_CEILING_DBFS / 20)
    chain = f"volume={gain_db:.2f}dB,alimiter=limit={limit:.4f}:level=disabled:attack=1:release=20"
    run([
        "ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-i", str(src),
        "-af", chain, "-t", f"{cap_ms / 1000:.3f}",
        "-ar", str(SAMPLE_RATE), "-ac", "2", "-c:a", "libvorbis", "-q:a", VORBIS_QUALITY, str(dst),
    ])


def normalize(src: Path, dst: Path, cap_ms: int) -> tuple[float, float]:
    loudness, _ = measure(src)
    gain = TARGET_LUFS - loudness
    peak = 0.0
    for _ in range(MAX_PASSES):
        render(src, dst, gain, cap_ms)
        loudness, peak = measure(dst)
        if abs(loudness - TARGET_LUFS) <= LUFS_TOLERANCE / 2:
            break
        # The limiter ate part of the gain; ask for the shortfall again.
        gain += TARGET_LUFS - loudness
    return loudness, peak


def write_licenses(packs_dir: Path) -> None:
    """LICENSES.md: each pack's own License.txt, verbatim, beside the files taken from it."""
    lines = [
        "# Bundled alert sounds: sources and licences",
        "",
        "Every sound in `sounds/` is from a Kenney pack released under Creative Commons Zero",
        "(CC0 1.0, public domain). Generated by `scripts/build-sound-library.py`; do not edit by hand.",
        "",
    ]
    for slug, (name, page, zip_url) in PACKS.items():
        taken = [(sid, rel) for sid, _, pack, rel in SELECTION if pack == slug]
        if not taken:
            continue
        licence = (packs_dir / slug / "License.txt").read_text(encoding="utf-8", errors="replace")
        licence = "\n".join(line.strip() for line in licence.strip().splitlines())
        lines += [f"## {name}", "", f"- Page: {page}", f"- Download: {zip_url}", "", "Files taken:", ""]
        lines += [f"- `{sid}.ogg` from `{rel}`" for sid, rel in taken]
        lines += ["", "Its License.txt:", "", "```text", licence, "```", ""]
    (OUT_DIR / "LICENSES.md").write_text("\n".join(lines), encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--packs", type=Path, required=True)
    args = parser.parse_args()
    for tool in ("ffmpeg", "ffprobe"):
        if shutil.which(tool) is None:
            print(f"{tool} is not on PATH", file=sys.stderr)
            return 2

    sounds_dir = OUT_DIR / "sounds"
    if sounds_dir.exists():
        shutil.rmtree(sounds_dir)
    sounds_dir.mkdir(parents=True)

    manifest = []
    failures = []
    for sound_id, name, pack, rel in SELECTION:
        category = sound_id.split("-")[0]
        cap = CAP_MS.get(category, DEFAULT_CAP_MS)
        dst = sounds_dir / f"{sound_id}.ogg"
        loudness, peak = normalize(args.packs / pack / rel, dst, cap)
        dur = duration_ms(dst)
        ok = abs(loudness - TARGET_LUFS) <= LUFS_TOLERANCE and peak <= TRUE_PEAK_CEILING_AFTER_ENCODE and dur <= cap
        print(f"{'ok  ' if ok else 'FAIL'} {sound_id:<10} {loudness:6.2f} LUFS {peak:6.2f} dBTP {dur:5d} ms  {pack}/{rel}")
        if not ok:
            failures.append(sound_id)
        manifest.append({
            "id": sound_id,
            "name": name,
            "category": CATEGORIES[category],
            "file": f"sounds/{sound_id}.ogg",
            "durationMs": dur,
            "source": f"Kenney {PACKS[pack][0]}: {rel}",
        })

    (OUT_DIR / "sounds.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    write_licenses(args.packs)
    total = sum(p.stat().st_size for p in sounds_dir.iterdir())
    print(f"{len(manifest)} sounds, {total / 1024:.0f} KiB")
    if failures:
        print("outside tolerance: " + ", ".join(failures), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
