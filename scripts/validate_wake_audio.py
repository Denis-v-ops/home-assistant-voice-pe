"""Validate the mastered NOVA wake cues using ffprobe and ffmpeg."""

from __future__ import annotations

from array import array
import json
from pathlib import Path
import re
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]
WAKE_DIR = ROOT / "sounds" / "wake"
EXPECTED_FILES = {"1.flac", "2.flac", "3.flac", "4.flac"}
LOUDNESS_MIN = -20.5
LOUDNESS_MAX = -19.5
TRUE_PEAK_MAX = -3.0


def _run(*arguments: str, binary: bool = False) -> str | bytes:
    result = subprocess.run(
        arguments,
        check=True,
        capture_output=True,
        text=not binary,
    )
    return result.stdout if binary else f"{result.stdout}\n{result.stderr}"


def validate(path: Path) -> list[str]:
    failures: list[str] = []
    probe = json.loads(
        str(
            _run(
                "ffprobe",
                "-v",
                "error",
                "-select_streams",
                "a:0",
                "-show_entries",
                "stream=codec_name,sample_rate,channels",
                "-of",
                "json",
                str(path),
            )
        )
    )["streams"][0]
    if probe.get("codec_name") != "flac":
        failures.append(f"codec={probe.get('codec_name')!r}, expected FLAC")
    if probe.get("sample_rate") != "24000":
        failures.append(f"sample_rate={probe.get('sample_rate')!r}, expected 24000")
    if probe.get("channels") != 1:
        failures.append(f"channels={probe.get('channels')!r}, expected mono")

    analysis = str(
        _run(
            "ffmpeg",
            "-hide_banner",
            "-nostats",
            "-i",
            str(path),
            "-filter_complex",
            "ebur128=peak=true",
            "-f",
            "null",
            "-",
        )
    )
    loudness_matches = re.findall(r"^\s*I:\s*(-?\d+(?:\.\d+)?) LUFS", analysis, re.MULTILINE)
    peak_matches = re.findall(r"^\s*Peak:\s*(-?\d+(?:\.\d+)?) dBFS", analysis, re.MULTILINE)
    if not loudness_matches or not peak_matches:
        failures.append("ffmpeg did not report integrated loudness and true peak")
        return failures
    loudness = float(loudness_matches[-1])
    true_peak = float(peak_matches[-1])
    if not LOUDNESS_MIN <= loudness <= LOUDNESS_MAX:
        failures.append(f"integrated loudness={loudness:.1f} LUFS")
    if true_peak > TRUE_PEAK_MAX:
        failures.append(f"true peak={true_peak:.1f} dBFS")

    raw = _run(
        "ffmpeg",
        "-v",
        "error",
        "-i",
        str(path),
        "-f",
        "s16le",
        "-acodec",
        "pcm_s16le",
        "-",
        binary=True,
    )
    samples = array("h")
    samples.frombytes(raw if isinstance(raw, bytes) else raw.encode())
    if sys.byteorder != "little":
        samples.byteswap()
    clipped_samples = sum(sample in {-32768, 32767} for sample in samples)
    if clipped_samples:
        failures.append(f"full-scale samples={clipped_samples}")

    print(
        f"{path.name}: 24 kHz mono FLAC, {loudness:.1f} LUFS, "
        f"{true_peak:.1f} dBTP, clipped_samples={clipped_samples}"
    )
    return failures


def main() -> int:
    files = sorted(WAKE_DIR.glob("*.flac"))
    names = {path.name for path in files}
    failures: list[str] = []
    if names != EXPECTED_FILES:
        failures.append(
            f"wake files are {sorted(names)!r}, expected {sorted(EXPECTED_FILES)!r}"
        )
    for path in files:
        failures.extend(f"{path.name}: {failure}" for failure in validate(path))
    if failures:
        print("Wake-audio validation failed:", file=sys.stderr)
        for failure in failures:
            print(f"- {failure}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
