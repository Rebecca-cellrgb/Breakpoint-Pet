"""Download a small, reproducible pose test set from Hugging Face.

This script intentionally uses only Python's standard library.  It downloads:
1. sitting / using-laptop images from Bingsu/Human_Action_Recognition
2. cropped-person images from vrg-prague/CropCOCO

The images are for functional evaluation only; see README.md for source licences.
"""

from __future__ import annotations

import csv
import hashlib
import json
import mimetypes
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path


ROOT = Path(__file__).resolve().parent
DATA_DIR = ROOT / "data"
API = "https://datasets-server.huggingface.co/rows"
USER_AGENT = "xuduan-pose-evaluation/1.0"
CACHE_DIR = ROOT / ".cache"


def get_json(url: str) -> dict:
    CACHE_DIR.mkdir(parents=True, exist_ok=True)
    cache_key = hashlib.sha256(url.encode("utf-8")).hexdigest()[:20]
    cache_file = CACHE_DIR / f"rows_{cache_key}.json"
    if cache_file.exists():
        return json.loads(cache_file.read_text(encoding="utf-8"))

    for attempt in range(5):
        request = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
        try:
            with urllib.request.urlopen(request, timeout=60) as response:
                result = json.load(response)
            cache_file.write_text(json.dumps(result), encoding="utf-8")
            return result
        except urllib.error.HTTPError as error:
            if error.code != 429 or attempt == 4:
                raise
            retry_after = error.headers.get("Retry-After")
            delay = int(retry_after) if retry_after and retry_after.isdigit() else 10 * (2**attempt)
            print(f"Hugging Face 限流，{delay} 秒后重试……", flush=True)
            time.sleep(delay)
    raise RuntimeError("Unreachable")


def get_bytes(url: str) -> tuple[bytes, str]:
    request = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    with urllib.request.urlopen(request, timeout=90) as response:
        return response.read(), response.headers.get_content_type()


def rows_url(dataset: str, config: str, split: str, offset: int, length: int) -> str:
    query = urllib.parse.urlencode(
        {
            "dataset": dataset,
            "config": config,
            "split": split,
            "offset": offset,
            "length": length,
        }
    )
    return f"{API}?{query}"


def image_url(row: dict) -> str:
    image = row.get("image")
    if isinstance(image, dict):
        for key in ("src", "url"):
            if image.get(key):
                return image[key]
    if isinstance(image, str):
        return image
    raise ValueError("Dataset row does not contain a downloadable image URL")


def extension_for(payload: bytes, content_type: str) -> str:
    if payload.startswith(b"\xff\xd8\xff"):
        return ".jpg"
    if payload.startswith(b"\x89PNG\r\n\x1a\n"):
        return ".png"
    if content_type == "image/jpeg":
        return ".jpg"
    if content_type == "image/png":
        return ".png"
    return mimetypes.guess_extension(content_type) or ".img"


def save_image(url: str, folder: Path, stem: str) -> Path:
    folder.mkdir(parents=True, exist_ok=True)
    existing = list(folder.glob(f"{stem}.*"))
    if existing:
        return existing[0]
    payload, content_type = get_bytes(url)
    destination = folder / f"{stem}{extension_for(payload, content_type)}"
    destination.write_bytes(payload)
    return destination


def collect_action_samples(count_per_class: int = 12) -> list[dict[str, str]]:
    dataset = "Bingsu/Human_Action_Recognition"
    wanted = {11: "sitting", 14: "using_laptop"}
    found = {label: 0 for label in wanted}
    records: list[dict[str, str]] = []

    seen_rows: set[int] = set()

    def consume_page(page: dict) -> None:
        for item in page.get("rows", []):
            row = item.get("row", {})
            row_index = int(item.get("row_idx", -1))
            if row_index in seen_rows:
                continue
            seen_rows.add(row_index)
            label = row.get("labels", row.get("label"))
            if label not in wanted or found[label] >= count_per_class:
                continue

            class_name = wanted[label]
            index = found[label] + 1
            source_url = image_url(row)
            local_path = save_image(
                source_url,
                DATA_DIR / class_name,
                f"{class_name}_{index:02d}",
            )
            found[label] = index
            records.append(
                {
                    "file": str(local_path.relative_to(ROOT)),
                    "expected_scene": class_name,
                    "source_dataset": dataset,
                    "source_row": str(row_index),
                    "source_url": source_url,
                }
            )
            print(f"[{class_name}] {index}/{count_per_class}: {local_path.name}", flush=True)
            time.sleep(0.05)

    # Reuse pages cached by an earlier run, including caches created before the
    # stable SHA-256 cache naming was introduced.
    for cache_file in CACHE_DIR.glob("rows_*.json"):
        try:
            consume_page(json.loads(cache_file.read_text(encoding="utf-8")))
        except (json.JSONDecodeError, OSError, ValueError):
            continue
        if all(value >= count_per_class for value in found.values()):
            break

    for offset in range(0, 12600, 100):
        if all(value >= count_per_class for value in found.values()):
            break
        page = get_json(rows_url(dataset, "default", "train", offset, 100))
        consume_page(page)

    missing = {wanted[k]: count_per_class - v for k, v in found.items() if v < count_per_class}
    if missing:
        raise RuntimeError(f"Could not collect enough samples: {missing}")
    return records


def collect_cropcoco_samples(count: int = 12) -> list[dict[str, str]]:
    dataset = "vrg-prague/CropCOCO"
    page = get_json(rows_url(dataset, "default", "train", 0, count))
    records: list[dict[str, str]] = []

    for index, item in enumerate(page.get("rows", []), start=1):
        row = item.get("row", {})
        source_url = image_url(row)
        local_path = save_image(
            source_url,
            DATA_DIR / "cropcoco",
            f"cropcoco_{index:02d}",
        )
        records.append(
            {
                "file": str(local_path.relative_to(ROOT)),
                "expected_scene": "person_keypoints",
                "source_dataset": dataset,
                "source_row": str(item.get("row_idx", index - 1)),
                "source_url": source_url,
            }
        )
        print(f"[cropcoco] {index}/{count}: {local_path.name}", flush=True)
        time.sleep(0.05)
    return records


def write_manifest(records: list[dict[str, str]]) -> None:
    manifest = ROOT / "manifest.csv"
    with manifest.open("w", newline="", encoding="utf-8-sig") as handle:
        writer = csv.DictWriter(
            handle,
            fieldnames=("file", "expected_scene", "source_dataset", "source_row", "source_url"),
        )
        writer.writeheader()
        writer.writerows(records)
    print(f"Manifest: {manifest}")


def main() -> None:
    DATA_DIR.mkdir(parents=True, exist_ok=True)
    records = collect_action_samples()
    records.extend(collect_cropcoco_samples())
    write_manifest(records)
    print(f"Done: downloaded {len(records)} images into {DATA_DIR}")


if __name__ == "__main__":
    main()
