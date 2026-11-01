"""Run the repository's YOLO11n-pose ONNX model on the small test set.

The decoder mirrors the equations in:
apps/main/pose/private/esp-dl/vision/detect/dl_pose_yolo11_postprocessor.cpp
"""

from __future__ import annotations

import csv
import math
import os
import time
from dataclasses import dataclass
from pathlib import Path

import numpy as np
import onnxruntime as ort
from PIL import Image, ImageDraw


ROOT = Path(__file__).resolve().parent
PROJECT = ROOT.parent
MODEL = PROJECT / "apps/main/pose/private/coco_pose/models/yolo11n-pose.onnx"
MANIFEST = ROOT / "manifest.csv"
OUTPUT_DIR = ROOT / "results"
INPUT_SIZE = 640
SCORE_THRESHOLD = 0.25
KEYPOINT_LOGIT_THRESHOLD = float(os.environ.get("POSE_KPT_LOGIT_THRESHOLD", "0.5"))
NMS_THRESHOLD = 0.7
MAX_DETECTIONS = 10

SKELETON = (
    (5, 7), (7, 9), (6, 8), (8, 10),
    (5, 6), (5, 11), (6, 12), (11, 12),
    (11, 13), (13, 15), (12, 14), (14, 16),
    (0, 1), (0, 2), (1, 3), (2, 4),
)


@dataclass
class Detection:
    score: float
    box: np.ndarray
    keypoints: np.ndarray
    visible: np.ndarray


@dataclass
class LetterboxInfo:
    scale: float
    left: int
    top: int
    original_width: int
    original_height: int


def sigmoid(value: np.ndarray | float) -> np.ndarray | float:
    return 1.0 / (1.0 + np.exp(-value))


def letterbox(image: Image.Image) -> tuple[np.ndarray, LetterboxInfo]:
    image = image.convert("RGB")
    width, height = image.size
    scale = min(INPUT_SIZE / width, INPUT_SIZE / height)
    resized_width = round(width * scale)
    resized_height = round(height * scale)
    left = (INPUT_SIZE - resized_width) // 2
    top = (INPUT_SIZE - resized_height) // 2

    resized = image.resize((resized_width, resized_height), Image.Resampling.BILINEAR)
    canvas = Image.new("RGB", (INPUT_SIZE, INPUT_SIZE), (114, 114, 114))
    canvas.paste(resized, (left, top))
    tensor = np.asarray(canvas, dtype=np.float32).transpose(2, 0, 1)[None] / 255.0
    return tensor, LetterboxInfo(scale, left, top, width, height)


def dfl(values: np.ndarray) -> np.ndarray:
    values = values.reshape(4, 16)
    shifted = values - values.max(axis=1, keepdims=True)
    probability = np.exp(shifted)
    probability /= probability.sum(axis=1, keepdims=True)
    return (probability * np.arange(16, dtype=np.float32)).sum(axis=1)


def iou(box: np.ndarray, boxes: np.ndarray) -> np.ndarray:
    x1 = np.maximum(box[0], boxes[:, 0])
    y1 = np.maximum(box[1], boxes[:, 1])
    x2 = np.minimum(box[2], boxes[:, 2])
    y2 = np.minimum(box[3], boxes[:, 3])
    intersection = np.maximum(0, x2 - x1) * np.maximum(0, y2 - y1)
    area_a = np.maximum(0, box[2] - box[0]) * np.maximum(0, box[3] - box[1])
    area_b = np.maximum(0, boxes[:, 2] - boxes[:, 0]) * np.maximum(0, boxes[:, 3] - boxes[:, 1])
    return intersection / np.maximum(area_a + area_b - intersection, 1e-6)


def nms(detections: list[Detection]) -> list[Detection]:
    detections.sort(key=lambda detection: detection.score, reverse=True)
    kept: list[Detection] = []
    while detections and len(kept) < MAX_DETECTIONS:
        best = detections.pop(0)
        kept.append(best)
        if detections:
            boxes = np.stack([detection.box for detection in detections])
            overlap = iou(best.box, boxes)
            detections = [detection for detection, value in zip(detections, overlap) if value <= NMS_THRESHOLD]
    return kept


def restore_point(x: float, y: float, info: LetterboxInfo) -> tuple[float, float]:
    original_x = np.clip((x - info.left) / info.scale, 0, info.original_width - 1)
    original_y = np.clip((y - info.top) / info.scale, 0, info.original_height - 1)
    return float(original_x), float(original_y)


def decode(outputs: dict[str, np.ndarray], info: LetterboxInfo) -> list[Detection]:
    detections: list[Detection] = []
    score_logit_threshold = math.log(SCORE_THRESHOLD / (1.0 - SCORE_THRESHOLD))

    for stage, stride in enumerate((8, 16, 32)):
        scores = outputs[f"score{stage}"][0, 0]
        boxes = outputs[f"box{stage}"][0]
        keypoints = outputs[f"kpt{stage}"][0]
        ys, xs = np.where(scores > score_logit_threshold)

        for y, x in zip(ys.tolist(), xs.tolist()):
            distances = dfl(boxes[:, y, x]) * stride
            center_x = x * stride + stride / 2
            center_y = y * stride + stride / 2
            raw_box = np.array(
                [
                    center_x - distances[0], center_y - distances[1],
                    center_x + distances[2], center_y + distances[3],
                ],
                dtype=np.float32,
            )
            x1, y1 = restore_point(raw_box[0], raw_box[1], info)
            x2, y2 = restore_point(raw_box[2], raw_box[3], info)

            raw_keypoints = keypoints[:, y, x].reshape(17, 3)
            visible = raw_keypoints[:, 2] >= KEYPOINT_LOGIT_THRESHOLD
            decoded_keypoints = np.zeros((17, 2), dtype=np.float32)
            for index, (raw_x, raw_y, _) in enumerate(raw_keypoints):
                model_x = raw_x * 2.0 * stride + x * stride
                model_y = raw_y * 2.0 * stride + y * stride
                decoded_keypoints[index] = restore_point(model_x, model_y, info)

            detections.append(
                Detection(
                    score=float(sigmoid(scores[y, x])),
                    box=np.array([x1, y1, x2, y2], dtype=np.float32),
                    keypoints=decoded_keypoints,
                    visible=visible,
                )
            )
    return nms(detections)


def annotate(image: Image.Image, detections: list[Detection]) -> Image.Image:
    output = image.convert("RGB").copy()
    draw = ImageDraw.Draw(output)
    line_width = max(2, round(min(output.size) / 250))
    radius = max(2, round(min(output.size) / 180))

    for detection in detections:
        box = tuple(float(value) for value in detection.box)
        draw.rectangle(box, outline=(0, 220, 90), width=line_width)
        draw.text((box[0] + 3, box[1] + 3), f"person {detection.score:.2f}", fill=(0, 220, 90))
        for first, second in SKELETON:
            if detection.visible[first] and detection.visible[second]:
                draw.line(
                    [tuple(detection.keypoints[first]), tuple(detection.keypoints[second])],
                    fill=(255, 190, 0),
                    width=line_width,
                )
        for point, is_visible in zip(detection.keypoints, detection.visible):
            if is_visible:
                x, y = point
                draw.ellipse((x - radius, y - radius, x + radius, y + radius), fill=(255, 70, 70))
    return output


def main() -> None:
    if not MODEL.exists():
        raise FileNotFoundError(MODEL)
    if not MANIFEST.exists():
        raise FileNotFoundError(f"Run download_samples.py first: {MANIFEST}")

    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    session = ort.InferenceSession(str(MODEL), providers=["CPUExecutionProvider"])
    output_names = [output.name for output in session.get_outputs()]
    rows = list(csv.DictReader(MANIFEST.open(encoding="utf-8-sig")))
    results: list[dict[str, str]] = []

    for index, row in enumerate(rows, start=1):
        image_path = ROOT / row["file"]
        image = Image.open(image_path)
        tensor, info = letterbox(image)
        started = time.perf_counter()
        raw_outputs = session.run(None, {"images": tensor})
        elapsed_ms = (time.perf_counter() - started) * 1000
        detections = decode(dict(zip(output_names, raw_outputs)), info)
        best = detections[0] if detections else None

        relative_output = Path(row["expected_scene"]) / image_path.name
        output_path = OUTPUT_DIR / relative_output
        output_path.parent.mkdir(parents=True, exist_ok=True)
        annotate(image, detections).save(output_path, quality=90)

        results.append(
            {
                "file": row["file"],
                "expected_scene": row["expected_scene"],
                "person_detected": "yes" if best else "no",
                "person_count": str(len(detections)),
                "best_score": f"{best.score:.4f}" if best else "",
                "visible_keypoints": str(int(best.visible.sum())) if best else "0",
                "inference_ms": f"{elapsed_ms:.2f}",
                "annotated_file": str(relative_output),
            }
        )
        status = f"score={best.score:.2f}, kpt={int(best.visible.sum())}/17" if best else "NO PERSON"
        print(f"{index:02d}/{len(rows)} {row['expected_scene']:<18} {status}", flush=True)

    result_csv = ROOT / "results.csv"
    with result_csv.open("w", newline="", encoding="utf-8-sig") as handle:
        writer = csv.DictWriter(handle, fieldnames=results[0].keys())
        writer.writeheader()
        writer.writerows(results)

    print("\nSummary")
    for scene in sorted({row["expected_scene"] for row in results}):
        scene_rows = [row for row in results if row["expected_scene"] == scene]
        detected = [row for row in scene_rows if row["person_detected"] == "yes"]
        average_keypoints = np.mean([int(row["visible_keypoints"]) for row in detected]) if detected else 0
        print(f"{scene}: {len(detected)}/{len(scene_rows)} detected, avg visible keypoints={average_keypoints:.1f}")
    print(f"Results: {result_csv}")
    print(f"Annotated images: {OUTPUT_DIR}")


if __name__ == "__main__":
    main()
