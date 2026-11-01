"""Download a small sit-down / stand-up video subset from HMDB51.

Source: https://huggingface.co/datasets/divm/hmdb51
Only public test-split MP4 files whose labels are `sit` or `stand` are used.
"""

from __future__ import annotations

import csv
import json
import re
import time
import urllib.parse
import urllib.request
from pathlib import Path


ROOT = Path(__file__).resolve().parent
OUTPUT = ROOT / "motion_data" / "hmdb51"
MANIFEST = ROOT / "motion_manifest.csv"
DATASET = "divm/hmdb51"
REVISION = "main"
TREE_API = f"https://huggingface.co/api/datasets/{DATASET}/tree/{REVISION}/test"
USER_AGENT = "xuduan-motion-evaluation/1.0"
COUNT_PER_CLASS = 10


def get_json(url: str) -> tuple[list[dict], str | None]:
    request = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    with urllib.request.urlopen(request, timeout=120) as response:
        data = json.load(response)
        link = response.headers.get("Link", "")
    match = re.search(r'<([^>]+)>;\s*rel="next"', link)
    return data, match.group(1) if match else None


def get_all_tree_entries(url: str) -> list[dict]:
    entries: list[dict] = []
    next_url: str | None = url
    while next_url:
        page, next_url = get_json(next_url)
        entries.extend(page)
    return entries


def download(url: str, destination: Path) -> None:
    if destination.exists() and destination.stat().st_size > 0:
        return
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = destination.with_suffix(destination.suffix + ".part")
    request = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    with urllib.request.urlopen(request, timeout=120) as response:
        temporary.write_bytes(response.read())
    temporary.replace(destination)


def main() -> None:
    query = urllib.parse.urlencode({"recursive": "true", "expand": "false", "limit": "1000"})
    entries = get_all_tree_entries(f"{TREE_API}?{query}")
    selected: dict[str, list[dict]] = {"sit": [], "stand": []}

    for entry in entries:
        path = entry.get("path", "")
        for label in selected:
            if path.startswith(f"test/{label}_") and path.endswith(".mp4"):
                selected[label].append(entry)

    for label, entries_for_label in selected.items():
        entries_for_label.sort(key=lambda item: item["path"])
        if len(entries_for_label) < COUNT_PER_CLASS:
            raise RuntimeError(
                f"The Hub tree page only exposed {len(entries_for_label)} '{label}' videos; "
                f"need {COUNT_PER_CLASS}."
            )

    records: list[dict[str, str]] = []
    for label in ("sit", "stand"):
        for index, entry in enumerate(selected[label][:COUNT_PER_CLASS], start=1):
            source_path = entry["path"]
            encoded_path = urllib.parse.quote(source_path, safe="/")
            source_url = (
                f"https://huggingface.co/datasets/{DATASET}/resolve/{REVISION}/"
                f"{encoded_path}?download=true"
            )
            destination = OUTPUT / label / f"{label}_{index:02d}.mp4"
            download(source_url, destination)
            records.append(
                {
                    "file": str(destination.relative_to(ROOT)),
                    "expected_action": "sit_down" if label == "sit" else "stand_up",
                    "source_dataset": DATASET,
                    "source_path": source_path,
                    "source_url": source_url,
                    "bytes": str(destination.stat().st_size),
                }
            )
            print(
                f"[{label}] {index}/{COUNT_PER_CLASS}: "
                f"{destination.name} ({destination.stat().st_size / 1024:.0f} KiB)",
                flush=True,
            )
            time.sleep(0.05)

    with MANIFEST.open("w", newline="", encoding="utf-8-sig") as handle:
        writer = csv.DictWriter(handle, fieldnames=records[0].keys())
        writer.writeheader()
        writer.writerows(records)
    print(f"Done: {len(records)} videos; manifest: {MANIFEST}")


if __name__ == "__main__":
    main()
