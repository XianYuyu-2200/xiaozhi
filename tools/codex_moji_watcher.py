import argparse
import re
import socket
import sqlite3
import time
from pathlib import Path


def send_state(host: str, port: int, state: str) -> None:
    data = state.encode("utf-8")
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        sock.sendto(data, (host, port))


def extract_turn_id(body: str) -> str | None:
    for pattern in (
        r"turn\.id=([0-9a-f-]+)",
        r"submission\.id=\"?([0-9a-f-]+)\"?",
        r"turn_id=([0-9a-f-]+)",
    ):
        match = re.search(pattern, body)
        if match:
            return match.group(1)
    return None


def is_user_submission(body: str) -> bool:
    return "Submission sub=Submission" in body and "op: UserInput" in body


def is_response_complete(body: str) -> bool:
    return "response.completed" in body or "turn.completed" in body or "turn_complete" in body


def open_readonly(db_path: Path) -> sqlite3.Connection:
    return sqlite3.connect(f"file:{db_path}?mode=ro", uri=True, timeout=1.0)


def get_max_log_id(db_path: Path) -> int:
    with open_readonly(db_path) as con:
        row = con.execute("select coalesce(max(id), 0) from logs").fetchone()
    return int(row[0] or 0)


def watch(args: argparse.Namespace) -> None:
    db_path = Path(args.db)
    last_id = get_max_log_id(db_path) if args.from_now else 0
    active_turn: str | None = None
    done_turns: set[str] = set()

    print(f"Watching thread {args.thread_id}")
    print(f"Database: {db_path}")
    print(f"Moji target: {args.host}:{args.port}")
    print(f"Starting after log id {last_id}")

    while True:
        try:
            with open_readonly(db_path) as con:
                rows = con.execute(
                    """
                    select id, feedback_log_body
                    from logs
                    where id>? and thread_id=?
                    order by id asc
                    """,
                    (last_id, args.thread_id),
                ).fetchall()
        except sqlite3.Error as exc:
            print(f"sqlite read failed: {exc}")
            time.sleep(args.interval)
            continue

        for row_id, body in rows:
            last_id = max(last_id, int(row_id))
            if not body:
                continue

            turn_id = extract_turn_id(body)
            if is_user_submission(body) and turn_id and turn_id != active_turn:
                active_turn = turn_id
                done_turns.discard(turn_id)
                send_state(args.host, args.port, "thinking")
                print(f"{time.strftime('%H:%M:%S')} turn {turn_id}: thinking")
                continue

            if active_turn and turn_id == active_turn and turn_id not in done_turns and is_response_complete(body):
                done_turns.add(turn_id)
                send_state(args.host, args.port, "done")
                print(f"{time.strftime('%H:%M:%S')} turn {turn_id}: done")
                active_turn = None

        time.sleep(args.interval)


def main() -> None:
    parser = argparse.ArgumentParser(description="Bridge Codex turns to the ESP32 Moji UDP expression controller.")
    parser.add_argument("--thread-id", required=True)
    parser.add_argument("--host", default="192.168.0.26")
    parser.add_argument("--port", type=int, default=3333)
    parser.add_argument("--db", default=str(Path.home() / ".codex" / "logs_2.sqlite"))
    parser.add_argument("--interval", type=float, default=0.5)
    parser.add_argument("--from-now", action="store_true", default=True)
    watch(parser.parse_args())


if __name__ == "__main__":
    main()
