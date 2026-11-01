from __future__ import annotations

import unittest
from pathlib import Path
from uuid import uuid4

from pet.controller import BreakpointPetController
from pet.models import PetState
from pet.storage import BreakpointStore


class WorkflowTests(unittest.TestCase):
    def setUp(self) -> None:
        temporary_directory = Path(__file__).resolve().parent / ".tmp"
        temporary_directory.mkdir(exist_ok=True)
        self.database = temporary_directory / f"{uuid4().hex}.db"
        self.store = BreakpointStore(self.database)
        self.controller = BreakpointPetController(self.store)

    def tearDown(self) -> None:
        self.database.unlink(missing_ok=True)

    def test_automatic_departure_and_return(self) -> None:
        response = self.controller.pre_leave_detected()
        self.assertEqual(response.state, PetState.CONFIRM_SAVE)
        self.assertEqual(response.to_dict()["hardware_feedback"]["servo"], "look_at_user")

        response = self.controller.receive_voice("需要")
        self.assertEqual(response.state, PetState.CAPTURE_CONTENT)

        response = self.controller.receive_voice("报告写到第三部分，回来继续补充案例")
        self.assertEqual(response.state, PetState.AWAY)
        self.assertEqual(response.breakpoint.content, "报告写到第三部分，回来继续补充案例")
        self.assertEqual(response.to_dict()["hardware_feedback"]["led"], "amber_breathe")

        response = self.controller.return_detected()
        self.assertEqual(response.state, PetState.IDLE)
        self.assertIn("报告写到第三部分", response.speech)
        self.assertIsNotNone(response.breakpoint.reminded_at)

    def test_manual_voice_capture(self) -> None:
        response = self.controller.receive_voice("帮我记录断点")
        self.assertEqual(response.state, PetState.CAPTURE_CONTENT)

        response = self.controller.receive_voice("正在整理比赛资料")
        self.assertEqual(response.state, PetState.IDLE)
        self.assertEqual(response.breakpoint.source, "manual_voice")

    def test_decline_does_not_create_breakpoint(self) -> None:
        self.controller.pre_leave_detected()
        response = self.controller.receive_voice("不需要")
        self.assertEqual(response.state, PetState.AWAY)
        self.assertEqual(self.store.list_all(), [])

    def test_complete_latest_breakpoint(self) -> None:
        self.controller.receive_voice("记录断点")
        self.controller.receive_voice("完成报名材料")
        response = self.controller.receive_voice("任务完成")
        self.assertEqual(response.event, "breakpoint_completed")
        self.assertIsNotNone(response.breakpoint.completed_at)


if __name__ == "__main__":
    unittest.main()
