"""SQLite persistence for task breakpoints."""

from __future__ import annotations

import sqlite3
from contextlib import contextmanager
from datetime import datetime, timedelta, timezone
from pathlib import Path
from typing import Iterator

from .models import Breakpoint


CHINA_TIMEZONE = timezone(timedelta(hours=8))


def now_iso() -> str:
    return datetime.now(CHINA_TIMEZONE).isoformat(timespec="seconds")


class BreakpointStore:
    def __init__(self, database_path: str | Path) -> None:
        self.database_path = Path(database_path)
        self.database_path.parent.mkdir(parents=True, exist_ok=True)
        self._initialize()

    @contextmanager
    def _connect(self) -> Iterator[sqlite3.Connection]:
        connection = sqlite3.connect(self.database_path)
        connection.row_factory = sqlite3.Row
        try:
            yield connection
            connection.commit()
        except Exception:
            connection.rollback()
            raise
        finally:
            connection.close()

    def _initialize(self) -> None:
        with self._connect() as connection:
            connection.execute(
                """
                CREATE TABLE IF NOT EXISTS breakpoints (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    content TEXT NOT NULL,
                    source TEXT NOT NULL,
                    created_at TEXT NOT NULL,
                    reminded_at TEXT,
                    completed_at TEXT
                )
                """
            )

    @staticmethod
    def _from_row(row: sqlite3.Row | None) -> Breakpoint | None:
        if row is None:
            return None
        return Breakpoint(
            id=row["id"],
            content=row["content"],
            source=row["source"],
            created_at=row["created_at"],
            reminded_at=row["reminded_at"],
            completed_at=row["completed_at"],
        )

    def add(self, content: str, source: str) -> Breakpoint:
        with self._connect() as connection:
            cursor = connection.execute(
                "INSERT INTO breakpoints(content, source, created_at) VALUES (?, ?, ?)",
                (content.strip(), source, now_iso()),
            )
            row = connection.execute(
                "SELECT * FROM breakpoints WHERE id = ?", (cursor.lastrowid,)
            ).fetchone()
        result = self._from_row(row)
        assert result is not None
        return result

    def latest_pending(self) -> Breakpoint | None:
        with self._connect() as connection:
            row = connection.execute(
                """
                SELECT * FROM breakpoints
                WHERE completed_at IS NULL
                ORDER BY id DESC
                LIMIT 1
                """
            ).fetchone()
        return self._from_row(row)

    def mark_reminded(self, breakpoint_id: int) -> Breakpoint:
        with self._connect() as connection:
            connection.execute(
                "UPDATE breakpoints SET reminded_at = ? WHERE id = ?",
                (now_iso(), breakpoint_id),
            )
            row = connection.execute(
                "SELECT * FROM breakpoints WHERE id = ?", (breakpoint_id,)
            ).fetchone()
        result = self._from_row(row)
        if result is None:
            raise KeyError(f"Breakpoint {breakpoint_id} does not exist")
        return result

    def complete_latest(self) -> Breakpoint | None:
        pending = self.latest_pending()
        if pending is None:
            return None
        with self._connect() as connection:
            connection.execute(
                "UPDATE breakpoints SET completed_at = ? WHERE id = ?",
                (now_iso(), pending.id),
            )
            row = connection.execute(
                "SELECT * FROM breakpoints WHERE id = ?", (pending.id,)
            ).fetchone()
        return self._from_row(row)

    def list_all(self, limit: int = 50) -> list[Breakpoint]:
        with self._connect() as connection:
            rows = connection.execute(
                "SELECT * FROM breakpoints ORDER BY id DESC LIMIT ?", (limit,)
            ).fetchall()
        return [item for row in rows if (item := self._from_row(row)) is not None]

    def clear(self) -> None:
        with self._connect() as connection:
            connection.execute("DELETE FROM breakpoints")

