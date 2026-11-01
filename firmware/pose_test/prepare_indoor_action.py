"""Extract the official Indoor Action Dataset test split and build a manifest."""

from __future__ import annotations

import csv
import zipfile
from pathlib import Path


ROOT = Path(__file__).resolve().parent
ARCHIVE = ROOT / "indoor_action" / "IndoorActionDataset-video.zip"
OUTPUT = ROOT / "indoor_action" / "official_test"
MANIFEST = ROOT / "indoor_action_test_manifest.csv"

LABELS = {
    "sitting down": "sit_down",
    "standing up": "stand_up",
}


def main() -> None:
    rows: list[dict[str, str]] = []
    with zipfile.ZipFile(ARCHIVE) as archive:
        for member in archive.infolist():
            parts = Path(member.filename).parts
            if member.is_dir() or len(parts) < 4:
                continue
            if parts[1] != "test" or parts[2] not in LABELS:
                continue
            relative = Path(*parts[1:])
            target = OUTPUT / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            with archive.open(member) as source, target.open("wb") as destination:
                while chunk := source.read(1024 * 1024):
                    destination.write(chunk)
            rows.append({"file": relative.as_posix(), "expected_action": LABELS[parts[2]]})

    rows.sort(key=lambda row: (row["expected_action"], row["file"]))
    with MANIFEST.open("w", newline="", encoding="utf-8-sig") as handle:
        writer = csv.DictWriter(handle, fieldnames=("file", "expected_action"))
        writer.writeheader()
        writer.writerows(rows)

    counts = {label: sum(row["expected_action"] == label for row in rows) for label in LABELS.values()}
    print(f"Extracted {len(rows)} videos to {OUTPUT}")
    print(f"sit_down={counts['sit_down']}, stand_up={counts['stand_up']}")
    print(f"Manifest: {MANIFEST}")


if __name__ == "__main__":
    main()
