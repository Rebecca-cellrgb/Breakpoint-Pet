"""Show live ESP32-S3 camera frames and YOLO pose results in a local browser."""

from __future__ import annotations

import argparse
import json
import socket
import struct
import threading
import time
import webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import cv2
import numpy as np
import onnxruntime as ort
from PIL import Image

from run_motion_test import MODEL, annotate, choose_actor, decode, letterbox, pose_metrics


DISCOVERY_PORT = 8764
STREAM_PORT = 8765
LOCAL_PORT = 8766
DISCOVERY_QUERY = b"CAMDISC1"
DISCOVERY_REPLY = b"CAMERA1"
FRAME_HEADER = struct.Struct("!4sHHI")
MAX_FRAME_BYTES = 2 * 1024 * 1024
PET_EVENT_PORT = DISCOVERY_PORT

state_lock = threading.Lock()
latest_jpeg: bytes | None = None
status = {"state": "正在寻找摄像头", "fps": 0.0, "error": ""}
stopping = threading.Event()


class PostureTracker:
    def __init__(self) -> None:
        self.history: list[tuple[float, float | None, float | None]] = []
        self.candidates: list[str | None] = []
        self.posture: str | None = None
        self.event = ""
        self.event_until = 0.0
        self.last_transition = 0.0
        self.pending_event: str | None = None
        self.was_absent = False

    @staticmethod
    def _classify(aspect: float, knee_angle: float | None,
                  thigh_verticality: float | None) -> str | None:
        votes: list[str] = []
        if knee_angle is not None:
            if knee_angle <= 125.0:
                votes.append("sitting")
            elif knee_angle >= 155.0:
                votes.append("standing")
        if thigh_verticality is not None:
            if thigh_verticality <= 0.55:
                votes.append("sitting")
            elif thigh_verticality >= 0.90:
                votes.append("standing")
        if aspect <= 1.80:
            votes.append("sitting")
        elif aspect >= 2.40:
            votes.append("standing")

        sitting = votes.count("sitting")
        standing = votes.count("standing")
        if sitting >= 2 and sitting > standing:
            return "sitting"
        if standing >= 2 and standing > sitting:
            return "standing"
        return None

    def update(self, actor, image_width: int, image_height: int) -> str:
        aspect, knee_angle, thigh_verticality = pose_metrics(actor, image_width, image_height)
        self.history.append((aspect, knee_angle, thigh_verticality))
        self.history = self.history[-7:]
        now = time.monotonic()
        recent = self.history[-5:]
        median = lambda column: float(np.median([
            row[column] for row in recent if row[column] is not None
        ])) if any(row[column] is not None for row in recent) else None
        knee = median(1)
        thigh = median(2)
        candidate = self._classify(median(0), knee, thigh)
        self.candidates = (self.candidates + [candidate])[-4:]
        stable_candidate = next(
            (state for state in ("sitting", "standing")
             if self.candidates.count(state) >= 3),
            None,
        )
        if stable_candidate is not None and stable_candidate != self.posture:
            if self.posture is not None:
                self.event = "\u521a\u521a\u8d77\u8eab" if stable_candidate == "standing" else "\u521a\u521a\u5750\u4e0b"
                self.event_until = now + 2.0
                if stable_candidate == "sitting":
                    self.pending_event = "RETURN"
                elif not self.was_absent:
                    self.pending_event = "PRE_LEAVE"
            self.posture = stable_candidate
            self.last_transition = now
        if stable_candidate is not None:
            self.was_absent = False

        if now < self.event_until:
            return self.event
        if self.posture == "standing":
            return "\u7ad9\u7740"
        if self.posture == "sitting":
            return "\u5750\u7740"
        return "\u7b49\u5f85\u52a8\u4f5c\u5224\u65ad"

    def take_event(self) -> str | None:
        event = self.pending_event
        self.pending_event = None
        return event

    def mark_absent(self) -> None:
        self.history.clear()
        self.candidates.clear()
        self.was_absent = True


def send_pet_event(board_ip: str, event: str) -> bool:
    command = f"PET1:{event}".encode("ascii")
    expected_ack = f"PET1:ACK:{event}".encode("ascii")
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as control:
            control.settimeout(0.25)
            for _ in range(3):
                control.sendto(command, (board_ip, PET_EVENT_PORT))
                try:
                    response, _peer = control.recvfrom(64)
                except socket.timeout:
                    continue
                if response == expected_ack:
                    print(f"桌宠事件已由主板接收: {event}", flush=True)
                    return True
    except OSError as error:
        print(f"桌宠事件发送失败 ({event}): {error}", flush=True)
        return False

    print(f"桌宠事件未收到主板确认: {event}", flush=True)
    return False


def set_status(state: str, fps: float = 0.0, error: str = "") -> None:
    with state_lock:
        status.update(state=state, fps=fps, error=error)


def describe_camera_error(error: Exception) -> str:
    code = getattr(error, "winerror", None)
    if code in (10051, 10064, 10065):
        return "\u5f53\u524d\u7f51\u7edc\u627e\u4e0d\u5230\u4e3b\u677f\uff1b\u8bf7\u786e\u8ba4\u7535\u8111\u548c\u4e3b\u677f\u8fde\u7740\u540c\u4e00\u4e2a Wi-Fi\uff0c\u5e76\u91cd\u65b0\u786e\u8ba4\u4e3b\u677f IP\u3002"
    if code == 10061:
        return "\u5df2\u627e\u5230\u4e3b\u677f\uff0c\u4f46\u6444\u50cf\u5934\u753b\u9762\u670d\u52a1\u6ca1\u6709\u54cd\u5e94\uff1b\u8bf7\u91cd\u542f\u4e3b\u677f\u540e\u518d\u8bd5\u3002"
    if code == 10013:
        return "\u5f53\u524d\u8fd0\u884c\u73af\u5883\u4e0d\u5141\u8bb8\u8bbf\u95ee\u4e3b\u677f\u7f51\u7edc\uff1b\u8bf7\u5728\u7535\u8111\u4e0a\u8fd0\u884c\u6444\u50cf\u5934\u5b9e\u65f6\u6d4b\u8bd5\u7a0b\u5e8f\u3002"
    return str(error)


def get_local_broadcasts() -> list[str]:
    addresses = {"255.255.255.255"}
    probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        probe.connect(("8.8.8.8", 80))
        local_ip = probe.getsockname()[0]
        octets = local_ip.split(".")
        if len(octets) == 4:
            addresses.add(".".join(octets[:3] + ["255"]))
            third_octet = int(octets[2])
            if third_octet < 255:
                # Also try the broadcast address for a /23 Wi-Fi subnet.
                addresses.add(".".join(octets[:2] + [str((third_octet & 0xFE) + 1), "255"]))
    except OSError:
        pass
    finally:
        probe.close()
    return list(addresses)


def discover_camera(host: str | None) -> str:
    if host:
        return host

    udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    udp.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    udp.settimeout(1.0)
    try:
        for broadcast in get_local_broadcasts():
            try:
                udp.sendto(DISCOVERY_QUERY, (broadcast, DISCOVERY_PORT))
            except OSError:
                continue
        deadline = time.monotonic() + 2.0
        while time.monotonic() < deadline:
            try:
                reply, peer = udp.recvfrom(64)
            except socket.timeout:
                break
            if reply == DISCOVERY_REPLY:
                return peer[0]
    finally:
        udp.close()
    raise TimeoutError("没有找到主板。请确认电脑和主板连着同一个 Wi-Fi。")


def connect_camera(host: str | None, last_host: str | None) -> tuple[socket.socket, str]:
    candidates = [host] if host else ([last_host] if last_host else [])
    last_error: OSError | None = None

    for candidate in candidates:
        try:
            return socket.create_connection((candidate, STREAM_PORT), timeout=5.0), candidate
        except OSError as error:
            last_error = error

    if host:
        raise last_error or ConnectionError(f"无法连接主板 {host}")

    board_ip = discover_camera(None)
    if board_ip == last_host and last_error:
        raise last_error
    return socket.create_connection((board_ip, STREAM_PORT), timeout=5.0), board_ip


def recv_exact(sock: socket.socket, length: int) -> bytes:
    chunks = bytearray()
    while len(chunks) < length:
        block = sock.recv(length - len(chunks))
        if not block:
            raise ConnectionError("主板连接已断开")
        chunks.extend(block)
    return bytes(chunks)


def process_frames(host: str | None) -> None:
    global latest_jpeg

    session = ort.InferenceSession(str(MODEL), providers=["CPUExecutionProvider"])
    output_names = [item.name for item in session.get_outputs()]
    previous_box = None
    posture_tracker = PostureTracker()
    last_board_ip = None
    stream_connected = False
    frame_count = 0

    while not stopping.is_set():
        try:
            set_status("正在寻找主板")
            conn, board_ip = connect_camera(host, last_board_ip)
            last_board_ip = board_ip
            set_status(f"已找到主板 {board_ip}，正在连接画面")
            with conn:
                stream_connected = True
                conn.settimeout(10.0)
                previous_box = None
                posture_tracker = PostureTracker()
                frame_count = 0
                window_start = time.monotonic()
                current_state = "已连接，正在识别人体"
                current_fps = 0.0
                set_status("已连接，正在识别人体")
                while not stopping.is_set():
                    magic, width, height, payload_size = FRAME_HEADER.unpack(
                        recv_exact(conn, FRAME_HEADER.size)
                    )
                    if magic != b"UYVY" or width == 0 or height == 0:
                        raise ValueError("收到的画面格式不正确")
                    if payload_size != width * height * 2 or payload_size > MAX_FRAME_BYTES:
                        raise ValueError("收到的画面大小不正确")

                    raw = np.frombuffer(recv_exact(conn, payload_size), dtype=np.uint8)
                    uyvy = raw.reshape((height, width, 2))
                    bgr = cv2.cvtColor(uyvy, cv2.COLOR_YUV2BGR_UYVY)
                    bgr = cv2.rotate(bgr, cv2.ROTATE_90_CLOCKWISE)
                    rgb = cv2.cvtColor(bgr, cv2.COLOR_BGR2RGB)
                    image = Image.fromarray(rgb)
                    luminance = float(cv2.cvtColor(bgr, cv2.COLOR_BGR2GRAY).mean())
                    is_black = luminance < 2.0 and float(bgr.std()) < 2.5
                    tensor, info = letterbox(image)
                    outputs = session.run(None, {"images": tensor})
                    detections = decode(dict(zip(output_names, outputs)), info)
                    actor = choose_actor(detections, previous_box)
                    if actor is not None:
                        previous_box = actor.box.copy()
                        display = annotate(image, [actor])
                        posture = posture_tracker.update(actor, image.width, image.height)
                        event = posture_tracker.take_event()
                        if event is not None:
                            send_pet_event(board_ip, event)
                        label = f"检测到人 | {posture} | {actor.score:.2f}"
                    else:
                        previous_box = None
                        posture_tracker.mark_absent()
                        display = image
                        label = "画面几乎全黑" if is_black else "画面正常，未检测到人"

                    display_bgr = cv2.cvtColor(np.asarray(display), cv2.COLOR_RGB2BGR)
                    cv2.putText(display_bgr, label, (8, 22), cv2.FONT_HERSHEY_SIMPLEX,
                                0.55, (30, 220, 80) if actor else (40, 170, 240), 2)
                    ok, encoded = cv2.imencode(".jpg", display_bgr, [cv2.IMWRITE_JPEG_QUALITY, 82])
                    if not ok:
                        raise RuntimeError("电脑无法生成画面预览")
                    with state_lock:
                        latest_jpeg = encoded.tobytes()

                    if is_black:
                        current_state = "摄像头有画面数据，但画面几乎全黑"
                    elif actor is not None:
                        current_state = f"实时识别中：{posture}"
                    else:
                        current_state = "已收到画面，暂未检测到人"
                    set_status(current_state, current_fps)

                    frame_count += 1
                    now = time.monotonic()
                    if now - window_start >= 1.0:
                        current_fps = frame_count / (now - window_start)
                        set_status(current_state, current_fps)
                        frame_count = 0
                        window_start = now
        except socket.timeout:
            if stream_connected:
                set_status(f"主板 {last_board_ip} 已连上，但10秒没收到图像帧；摄像头仍未出画面")
            else:
                set_status("局域网搜索主板超时；请确认主板和电脑连接同一个 Wi-Fi")
            stream_connected = False
            time.sleep(2.0)
        except Exception as error:
            set_status("等待连接或重试中", error=describe_camera_error(error))
            stream_connected = False
            time.sleep(2.0)


PAGE = """<!doctype html>
<html lang=zh-CN><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>
<title>摄像头实时测试</title>
<style>
body{margin:0;background:#171a1c;color:#f3f5f4;font:16px Arial,sans-serif}
header{padding:12px 18px;background:#252a2c;display:flex;gap:20px;align-items:center}
strong{font-size:18px}#state{color:#b8d8ca}#fps{margin-left:auto;color:#ddd}
main{height:calc(100vh - 50px);display:grid;place-items:center;padding:8px;box-sizing:border-box}
img{max-width:100%;max-height:100%;object-fit:contain;background:#090a0a}
</style>
<header><strong>摄像头实时测试</strong><span id=state>正在启动</span><span id=fps></span></header>
<main><img src=/stream alt='等待摄像头画面'></main>
<script>setInterval(async()=>{try{let s=await(await fetch('/status')).json();
document.querySelector('#state').textContent=s.error||s.state;
document.querySelector('#fps').textContent=s.fps?`${s.fps.toFixed(1)} 帧/秒`:''}catch{}},1000)</script>
</html>""".encode("utf-8")


class ViewerHandler(BaseHTTPRequestHandler):
    def do_GET(self) -> None:
        if self.path == "/":
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(PAGE)))
            self.end_headers()
            self.wfile.write(PAGE)
            return
        if self.path == "/status":
            with state_lock:
                body = json.dumps(status, ensure_ascii=False).encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        if self.path == "/stream":
            self.send_response(200)
            self.send_header("Cache-Control", "no-store")
            self.send_header("Content-Type", "multipart/x-mixed-replace; boundary=frame")
            self.end_headers()
            try:
                while not stopping.is_set():
                    with state_lock:
                        frame = latest_jpeg
                    if frame:
                        self.wfile.write(b"--frame\r\nContent-Type: image/jpeg\r\nContent-Length: "
                                         + str(len(frame)).encode() + b"\r\n\r\n" + frame + b"\r\n")
                        self.wfile.flush()
                    time.sleep(0.05)
            except (BrokenPipeError, ConnectionResetError):
                pass
            return
        self.send_error(404)

    def log_message(self, _format: str, *_args: object) -> None:
        pass


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", help="主板 IP；留空时自动在局域网查找")
    parser.add_argument("--port", type=int, default=LOCAL_PORT, help="电脑本地预览端口")
    args = parser.parse_args()

    worker = threading.Thread(target=process_frames, args=(args.host,), daemon=True)
    worker.start()
    server = ThreadingHTTPServer(("127.0.0.1", args.port), ViewerHandler)
    url = f"http://127.0.0.1:{args.port}"
    print(f"Live preview: {url}", flush=True)
    webbrowser.open(url)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        stopping.set()
        server.server_close()


if __name__ == "__main__":
    main()
