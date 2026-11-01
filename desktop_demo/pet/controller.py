"""Hardware-independent dialogue and state machine for the breakpoint pet."""

from __future__ import annotations

from collections import deque
from threading import RLock

from .models import ControllerResponse, PetState, hardware_feedback
from .storage import BreakpointStore


YES_WORDS = ("是", "好的", "好", "需要", "记录", "保存", "要")
NO_WORDS = ("不需要", "不用", "不要", "取消", "否")
MANUAL_COMMANDS = ("记录断点", "保存断点", "帮我记录", "记一下")
COMPLETE_COMMANDS = ("完成断点", "任务完成", "已经完成", "标记完成")


class BreakpointPetController:
    def __init__(self, store: BreakpointStore) -> None:
        self.store = store
        self.state = PetState.IDLE
        self._capture_source: str | None = None
        self._return_state = PetState.IDLE
        self._history: deque[dict[str, str]] = deque(maxlen=50)
        self._lock = RLock()
        self._record("system", "断点萌宠已启动。")

    def _record(self, speaker: str, text: str) -> None:
        self._history.append({"speaker": speaker, "text": text})

    def _respond(self, speech: str, event: str, breakpoint=None) -> ControllerResponse:
        self._record("pet", speech)
        return ControllerResponse(self.state, speech, event, breakpoint)

    def snapshot(self) -> dict[str, object]:
        with self._lock:
            return {
                "state": self.state.value,
                "hardware_feedback": hardware_feedback(self.state),
                "history": list(self._history),
                "breakpoints": [item.to_dict() for item in self.store.list_all()],
            }

    def pre_leave_detected(self) -> ControllerResponse:
        with self._lock:
            if self.state not in (PetState.IDLE, PetState.AWAY):
                return self._respond("我还在等待你完成刚才的回答。", "event_ignored")
            self.state = PetState.CONFIRM_SAVE
            return self._respond(
                "检测到你准备离开，需要帮你记录当前任务断点吗？",
                "pre_leave_detected",
            )

    def return_detected(self) -> ControllerResponse:
        with self._lock:
            self.state = PetState.IDLE
            pending = self.store.latest_pending()
            if pending is None:
                return self._respond("欢迎回来，目前没有未完成的任务断点。", "return_no_breakpoint")
            reminded = self.store.mark_reminded(pending.id)
            return self._respond(
                f"欢迎回来。你之前记录的是：{reminded.content}。",
                "return_reminder",
                reminded,
            )

    def receive_voice(self, text: str) -> ControllerResponse:
        with self._lock:
            normalized = "".join(text.strip().split())
            self._record("user", text.strip() or "（空白）")
            if not normalized:
                return self._respond("我没有听清，请再说一次。", "empty_voice")

            if any(command in normalized for command in COMPLETE_COMMANDS):
                completed = self.store.complete_latest()
                if completed is None:
                    return self._respond("目前没有未完成的断点。", "complete_none")
                self.state = PetState.IDLE
                return self._respond(
                    f"好的，已将“{completed.content}”标记为完成。",
                    "breakpoint_completed",
                    completed,
                )

            if self.state in (PetState.IDLE, PetState.AWAY) and any(
                command in normalized for command in MANUAL_COMMANDS
            ):
                self._return_state = self.state
                self._capture_source = "manual_voice"
                self.state = PetState.CAPTURE_CONTENT
                return self._respond("请告诉我需要记录什么。", "manual_capture_started")

            if self.state == PetState.CONFIRM_SAVE:
                if any(word in normalized for word in NO_WORDS):
                    self.state = PetState.AWAY
                    return self._respond("好的，这次不记录。等你回来。", "save_declined")
                if any(word in normalized for word in YES_WORDS):
                    self._capture_source = "automatic_departure"
                    self._return_state = PetState.AWAY
                    self.state = PetState.CAPTURE_CONTENT
                    return self._respond("请告诉我你做到哪里，以及回来后要继续做什么。", "save_confirmed")
                return self._respond("请回答需要或不需要。", "confirm_unclear")

            if self.state == PetState.CAPTURE_CONTENT:
                source = self._capture_source or "voice"
                saved = self.store.add(text.strip(), source)
                self.state = self._return_state
                self._capture_source = None
                return self._respond(
                    f"好的，已经记录：{saved.content}。",
                    "breakpoint_saved",
                    saved,
                )

            return self._respond(
                "你可以说“记录断点”，或者等待我检测到你准备离开。",
                "voice_not_understood",
            )

    def reset(self) -> ControllerResponse:
        with self._lock:
            self.store.clear()
            self.state = PetState.IDLE
            self._capture_source = None
            self._return_state = PetState.IDLE
            self._history.clear()
            self._record("system", "数据和状态已重置。")
            return self._respond("已经清空所有测试数据。", "reset")

