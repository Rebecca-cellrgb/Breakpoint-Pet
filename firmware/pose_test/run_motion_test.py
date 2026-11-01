"""Evaluate pose stability and motion trends on HMDB51 sit/stand clips."""

from __future__ import annotations

import csv
import math
import os
from dataclasses import dataclass
from pathlib import Path

import cv2
import numpy as np
import onnxruntime as ort
from PIL import Image, ImageDraw

from run_onnx_test import MODEL, Detection, annotate, decode, letterbox


ROOT = Path(__file__).resolve().parent
# Environment overrides let the same evaluator run against additional datasets
# without moving or renaming the original HMDB51 fixtures.
MANIFEST = Path(os.environ.get("POSE_MANIFEST", ROOT / "motion_manifest.csv"))
VIDEO_ROOT = Path(os.environ.get("POSE_VIDEO_ROOT", ROOT))
RUN_TAG = os.environ.get("POSE_RUN_TAG", "")
RESULTS_CSV = ROOT / f"motion_results{RUN_TAG}.csv"
FRAME_RESULTS_CSV = ROOT / f"motion_frame_results{RUN_TAG}.csv"
STORYBOARD_DIR = ROOT / f"motion_results{RUN_TAG}"
SAMPLES_PER_VIDEO = 15


@dataclass
class FrameMeasurement:
    frame_index: int
    time_seconds: float
    detected: bool
    score: float | None
    visible_keypoints: int
    bbox_aspect: float | None
    knee_angle: float | None
    thigh_verticality: float | None
    annotated: Image.Image


def angle_degrees(first: np.ndarray, middle: np.ndarray, last: np.ndarray) -> float:
    vector_a = first - middle
    vector_b = last - middle
    denominator = np.linalg.norm(vector_a) * np.linalg.norm(vector_b)
    if denominator < 1e-6:
        return float("nan")
    cosine = np.clip(np.dot(vector_a, vector_b) / denominator, -1.0, 1.0)
    return float(np.degrees(np.arccos(cosine)))


def pose_metrics(detection: Detection, image_width: int, image_height: int) -> tuple[float, float | None, float | None]:
    box_width = max(float(detection.box[2] - detection.box[0]), 1.0)
    box_height = max(float(detection.box[3] - detection.box[1]), 1.0)
    bbox_aspect = box_height / box_width

    knee_angles: list[float] = []
    thigh_verticalities: list[float] = []
    for hip, knee, ankle in ((11, 13, 15), (12, 14, 16)):
        if detection.visible[hip] and detection.visible[knee]:
            delta = detection.keypoints[knee] - detection.keypoints[hip]
            norm = float(np.linalg.norm(delta))
            if norm > 1e-6:
                thigh_verticalities.append(abs(float(delta[1])) / norm)
        if detection.visible[hip] and detection.visible[knee] and detection.visible[ankle]:
            value = angle_degrees(
                detection.keypoints[hip],
                detection.keypoints[knee],
                detection.keypoints[ankle],
            )
            if not math.isnan(value):
                knee_angles.append(value)

    knee_angle = float(np.mean(knee_angles)) if knee_angles else None
    thigh_verticality = float(np.mean(thigh_verticalities)) if thigh_verticalities else None
    return bbox_aspect, knee_angle, thigh_verticality


def choose_actor(detections: list[Detection], previous_box: np.ndarray | None) -> Detection | None:
    if not detections:
        return None
    if previous_box is None:
        return max(
            detections,
            key=lambda item: item.score * math.sqrt(max(1.0, float((item.box[2] - item.box[0]) * (item.box[3] - item.box[1])))),
        )

    def tracking_score(item: Detection) -> float:
        x1 = max(float(item.box[0]), float(previous_box[0]))
        y1 = max(float(item.box[1]), float(previous_box[1]))
        x2 = min(float(item.box[2]), float(previous_box[2]))
        y2 = min(float(item.box[3]), float(previous_box[3]))
        intersection = max(0.0, x2 - x1) * max(0.0, y2 - y1)
        area_a = max(1.0, float((item.box[2] - item.box[0]) * (item.box[3] - item.box[1])))
        area_b = max(1.0, float((previous_box[2] - previous_box[0]) * (previous_box[3] - previous_box[1])))
        overlap = intersection / max(area_a + area_b - intersection, 1.0)
        return overlap * 2.0 + item.score

    return max(detections, key=tracking_score)


def robust_delta(values: list[float | None]) -> float | None:
    valid = [(index, value) for index, value in enumerate(values) if value is not None]
    if len(valid) < 4:
        return None
    window = max(2, len(valid) // 3)
    start = np.median([value for _, value in valid[:window]])
    end = np.median([value for _, value in valid[-window:]])
    return float(end - start)


def direction_result(expected_action: str, aspect_delta: float | None, knee_delta: float | None, thigh_delta: float | None) -> tuple[str, int, int]:
    expected_sign = 1 if expected_action == "stand_up" else -1
    checks: list[bool] = []
    # Ignore tiny changes that are indistinguishable from landmark jitter.
    if aspect_delta is not None and abs(aspect_delta) >= 0.08:
        checks.append(aspect_delta * expected_sign > 0)
    if knee_delta is not None and abs(knee_delta) >= 8.0:
        checks.append(knee_delta * expected_sign > 0)
    if thigh_delta is not None and abs(thigh_delta) >= 0.08:
        checks.append(thigh_delta * expected_sign > 0)
    if not checks:
        return "insufficient_signal", 0, 0
    positives = sum(checks)
    return ("pass" if positives > len(checks) / 2 else "fail"), positives, len(checks)


def make_storyboard(measurements: list[FrameMeasurement], title: str) -> Image.Image:
    chosen = [measurements[0], measurements[len(measurements) // 2], measurements[-1]]
    width = 320
    panels: list[Image.Image] = []
    for measurement in chosen:
        image = measurement.annotated.copy()
        scale = width / image.width
        image = image.resize((width, max(1, round(image.height * scale))), Image.Resampling.LANCZOS)
        panel = Image.new("RGB", (width, image.height + 30), "white")
        panel.paste(image, (0, 30))
        draw = ImageDraw.Draw(panel)
        draw.text((6, 7), f"t={measurement.time_seconds:.2f}s  kpt={measurement.visible_keypoints}/17", fill="black")
        panels.append(panel)
    height = max(panel.height for panel in panels)
    storyboard = Image.new("RGB", (width * 3, height + 30), "white")
    draw = ImageDraw.Draw(storyboard)
    draw.text((8, 7), title, fill="black")
    for index, panel in enumerate(panels):
        storyboard.paste(panel, (index * width, 30))
    return storyboard


def main() -> None:
    rows = list(csv.DictReader(MANIFEST.open(encoding="utf-8-sig")))
    session = ort.InferenceSession(str(MODEL), providers=["CPUExecutionProvider"])
    output_names = [output.name for output in session.get_outputs()]
    video_results: list[dict[str, str]] = []
    frame_results: list[dict[str, str]] = []

    for video_number, row in enumerate(rows, start=1):
        video_path = VIDEO_ROOT / row["file"]
        capture = cv2.VideoCapture(str(video_path))
        frame_count = int(capture.get(cv2.CAP_PROP_FRAME_COUNT))
        fps = float(capture.get(cv2.CAP_PROP_FPS)) or 30.0
        if frame_count <= 0:
            raise RuntimeError(f"Could not read frames from {video_path}")
        sample_indices = np.linspace(0, frame_count - 1, min(SAMPLES_PER_VIDEO, frame_count), dtype=int)
        measurements: list[FrameMeasurement] = []
        previous_box: np.ndarray | None = None

        for frame_index in sample_indices.tolist():
            capture.set(cv2.CAP_PROP_POS_FRAMES, frame_index)
            ok, bgr = capture.read()
            if not ok:
                continue
            rgb = cv2.cvtColor(bgr, cv2.COLOR_BGR2RGB)
            image = Image.fromarray(rgb)
            tensor, info = letterbox(image)
            raw_outputs = session.run(None, {"images": tensor})
            detections = decode(dict(zip(output_names, raw_outputs)), info)
            actor = choose_actor(detections, previous_box)

            if actor is None:
                measurement = FrameMeasurement(
                    frame_index, frame_index / fps, False, None, 0, None, None, None, image.copy()
                )
            else:
                previous_box = actor.box.copy()
                aspect, knee_angle, thigh_verticality = pose_metrics(actor, image.width, image.height)
                measurement = FrameMeasurement(
                    frame_index,
                    frame_index / fps,
                    True,
                    actor.score,
                    int(actor.visible.sum()),
                    aspect,
                    knee_angle,
                    thigh_verticality,
                    annotate(image, [actor]),
                )
            measurements.append(measurement)
            frame_results.append(
                {
                    "video": row["file"],
                    "expected_action": row["expected_action"],
                    "frame_index": str(frame_index),
                    "time_seconds": f"{measurement.time_seconds:.3f}",
                    "detected": "yes" if measurement.detected else "no",
                    "score": f"{measurement.score:.4f}" if measurement.score is not None else "",
                    "visible_keypoints": str(measurement.visible_keypoints),
                    "bbox_aspect": f"{measurement.bbox_aspect:.4f}" if measurement.bbox_aspect is not None else "",
                    "knee_angle": f"{measurement.knee_angle:.2f}" if measurement.knee_angle is not None else "",
                    "thigh_verticality": f"{measurement.thigh_verticality:.4f}" if measurement.thigh_verticality is not None else "",
                }
            )
        capture.release()

        if not measurements:
            raise RuntimeError(f"No frames decoded from {video_path}")
        detected = [item for item in measurements if item.detected]
        detection_rate = len(detected) / len(measurements)
        average_keypoints = float(np.mean([item.visible_keypoints for item in detected])) if detected else 0.0
        average_score = float(np.mean([item.score for item in detected if item.score is not None])) if detected else 0.0
        aspect_delta = robust_delta([item.bbox_aspect for item in measurements])
        knee_delta = robust_delta([item.knee_angle for item in measurements])
        thigh_delta = robust_delta([item.thigh_verticality for item in measurements])
        direction, correct_checks, total_checks = direction_result(
            row["expected_action"], aspect_delta, knee_delta, thigh_delta
        )

        storyboard_path = STORYBOARD_DIR / row["expected_action"] / f"{video_path.stem}_storyboard.jpg"
        storyboard_path.parent.mkdir(parents=True, exist_ok=True)
        make_storyboard(
            measurements,
            f"{row['expected_action']} | detection={detection_rate:.0%} | direction={direction}",
        ).save(storyboard_path, quality=90)

        video_results.append(
            {
                "file": row["file"],
                "expected_action": row["expected_action"],
                "sampled_frames": str(len(measurements)),
                "detection_rate": f"{detection_rate:.4f}",
                "average_score": f"{average_score:.4f}",
                "average_visible_keypoints": f"{average_keypoints:.2f}",
                "bbox_aspect_delta": f"{aspect_delta:.4f}" if aspect_delta is not None else "",
                "knee_angle_delta": f"{knee_delta:.2f}" if knee_delta is not None else "",
                "thigh_verticality_delta": f"{thigh_delta:.4f}" if thigh_delta is not None else "",
                "motion_direction": direction,
                "direction_checks": f"{correct_checks}/{total_checks}",
                "storyboard": str(storyboard_path.relative_to(ROOT)),
            }
        )
        print(
            f"{video_number:02d}/{len(rows)} {row['expected_action']:<10} "
            f"detect={detection_rate:>5.0%} kpt={average_keypoints:>4.1f}/17 direction={direction}",
            flush=True,
        )

    with RESULTS_CSV.open("w", newline="", encoding="utf-8-sig") as handle:
        writer = csv.DictWriter(handle, fieldnames=video_results[0].keys())
        writer.writeheader()
        writer.writerows(video_results)
    with FRAME_RESULTS_CSV.open("w", newline="", encoding="utf-8-sig") as handle:
        writer = csv.DictWriter(handle, fieldnames=frame_results[0].keys())
        writer.writeheader()
        writer.writerows(frame_results)

    print("\nSummary")
    for action in ("sit_down", "stand_up"):
        group = [item for item in video_results if item["expected_action"] == action]
        detection_rate = np.mean([float(item["detection_rate"]) for item in group])
        keypoints = np.mean([float(item["average_visible_keypoints"]) for item in group])
        passes = sum(item["motion_direction"] == "pass" for item in group)
        evaluable = sum(item["motion_direction"] != "insufficient_signal" for item in group)
        print(
            f"{action}: frame detection={detection_rate:.1%}, visible keypoints={keypoints:.1f}/17, "
            f"direction pass={passes}/{evaluable} evaluable"
        )
    print(f"Video results: {RESULTS_CSV}")
    print(f"Frame results: {FRAME_RESULTS_CSV}")
    print(f"Storyboards: {STORYBOARD_DIR}")


if __name__ == "__main__":
    main()
