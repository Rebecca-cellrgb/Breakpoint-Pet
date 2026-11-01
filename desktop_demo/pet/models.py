"""Domain models shared by the software prototype and future hardware adapter."""

from __future__ import annotations

from dataclasses import asdict, dataclass
from enum import StrEnum


class PetState(StrEnum):
    IDLE = "idle"
    CONFIRM_SAVE = "confirm_save"
    CAPTURE_CONTENT = "capture_content"
    AWAY = "away"


STATE_HARDWARE_FEEDBACK: dict[PetState, dict[str, str]] = {
    PetState.IDLE: {"led": "green_breathe", "servo": "center"},
    PetState.CONFIRM_SAVE: {"led": "blue_breathe", "servo": "look_at_user"},
    PetState.CAPTURE_CONTENT: {"led": "blue_blink", "servo": "listen_pose"},
    PetState.AWAY: {"led": "amber_breathe", "servo": "sleep_pose"},
}


def hardware_feedback(state: PetState) -> dict[str, str]:
    """Return a stable output contract shared with the future ESP-IDF adapter."""
    return dict(STATE_HARDWARE_FEEDBACK[state])


@dataclass(frozen=True)
class Breakpoint:
    id: int
    content: str
    source: str
    created_at: str
    reminded_at: str | None = None
    completed_at: str | None = None

    def to_dict(self) -> dict[str, object]:
        return asdict(self)


@dataclass(frozen=True)
class ControllerResponse:
    state: PetState
    speech: str
    event: str
    breakpoint: Breakpoint | None = None

    def to_dict(self) -> dict[str, object]:
        return {
            "state": self.state.value,
            "speech": self.speech,
            "event": self.event,
            "hardware_feedback": hardware_feedback(self.state),
            "breakpoint": self.breakpoint.to_dict() if self.breakpoint else None,
        }

