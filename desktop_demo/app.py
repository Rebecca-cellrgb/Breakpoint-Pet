"""Flask debug application for the hardware-independent breakpoint pet."""

from __future__ import annotations

from pathlib import Path

from flask import Flask, jsonify, render_template, request

from pet import BreakpointPetController, BreakpointStore


ROOT = Path(__file__).resolve().parent
app = Flask(__name__)
controller = BreakpointPetController(BreakpointStore(ROOT / "data" / "breakpoints.db"))


@app.get("/")
def index():
    return render_template("index.html")


@app.get("/api/state")
def state():
    return jsonify(controller.snapshot())


@app.post("/api/events/pre-leave")
def pre_leave():
    return jsonify(controller.pre_leave_detected().to_dict())


@app.post("/api/events/return")
def returned():
    return jsonify(controller.return_detected().to_dict())


@app.post("/api/voice")
def voice():
    payload = request.get_json(silent=True) or {}
    text = str(payload.get("text", ""))
    return jsonify(controller.receive_voice(text).to_dict())


@app.post("/api/reset")
def reset():
    return jsonify(controller.reset().to_dict())


if __name__ == "__main__":
    print("\n请打开：http://127.0.0.1:5000/  （注意是 http，不是 https）\n")
    app.run(host="127.0.0.1", port=5000, debug=True)

