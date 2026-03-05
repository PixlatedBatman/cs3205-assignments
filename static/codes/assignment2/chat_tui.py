#!/usr/bin/env python3
import argparse
import curses
import json
import os
import queue
import socket
import struct
import subprocess
import threading
import time
from collections import defaultdict
from datetime import datetime


MAX_PAYLOAD = 4096

MSG_DISC_REGISTER_REQ = 1
MSG_DISC_REGISTER_RESP = 2
MSG_LOGIN_REQ = 10
MSG_LOGIN_RESP = 11
MSG_BROADCAST_REQ = 12
MSG_BROADCAST_DELIVER = 13
MSG_PRIVATE_REQ = 14
MSG_PRIVATE_DELIVER = 15
MSG_LIST_REQ = 16
MSG_LIST_RESP = 17
MSG_STATUS = 18
MSG_ACK = 19
MSG_ERROR = 20
MSG_DISCONNECT = 21
MSG_STATUS_SET_REQ = 22
MSG_HISTORY_REQ = 23
MSG_HISTORY_RESP = 24

PAIR_HEADER = 1
PAIR_STATUS_OK = 2
PAIR_STATUS_AWAY = 3
PAIR_STATUS_BUSY = 4
PAIR_SYSTEM = 5
PAIR_ERROR = 6


def init_colors():
    if not curses.has_colors():
        return
    curses.start_color()
    try:
        curses.use_default_colors()
    except Exception:
        pass
    # -1 background keeps terminal theme/background.
    curses.init_pair(PAIR_HEADER, curses.COLOR_CYAN, -1)
    curses.init_pair(PAIR_STATUS_OK, curses.COLOR_GREEN, -1)
    curses.init_pair(PAIR_STATUS_AWAY, curses.COLOR_YELLOW, -1)
    curses.init_pair(PAIR_STATUS_BUSY, curses.COLOR_RED, -1)
    curses.init_pair(PAIR_SYSTEM, curses.COLOR_MAGENTA, -1)
    curses.init_pair(PAIR_ERROR, curses.COLOR_RED, -1)


def now_hms() -> str:
    return datetime.now().strftime("%H:%M:%S")


def recv_all(sock: socket.socket, n: int) -> bytes:
    data = bytearray()
    while len(data) < n:
        chunk = sock.recv(n - len(data))
        if not chunk:
            raise ConnectionError("socket closed")
        data.extend(chunk)
    return bytes(data)


def send_message(sock: socket.socket, mtype: int, payload: str) -> None:
    b = payload.encode("utf-8")
    header = struct.pack("!II", mtype, len(b))
    sock.sendall(header + b)


def recv_message(sock: socket.socket):
    h = recv_all(sock, 8)
    mtype, length = struct.unpack("!II", h)
    if length > MAX_PAYLOAD:
        raise ValueError("payload too large")
    payload = recv_all(sock, length).decode("utf-8", errors="replace") if length else ""
    return mtype, payload


class TuiState:
    def __init__(self, username: str):
        self.username = username
        self.channels = ["#broadcast"]
        self.channel_set = {"#broadcast"}
        self.selected_idx = 0
        self.messages = defaultdict(list)  # channel -> [(time, who, text)]
        self.online_users_text = ""
        self.input_line = ""
        self.status_line = "Connected. Type /help for commands."
        self.user_status = {}

    def add_channel(self, ch: str):
        if ch not in self.channel_set:
            self.channels.append(ch)
            self.channel_set.add(ch)

    def touch_channel_recent(self, ch: str):
        if ch == "#broadcast":
            return
        self.add_channel(ch)
        self.channels = ["#broadcast"] + [c for c in self.channels if c not in ("#broadcast", ch)] + [ch]
        # Move recently active contact near end; keeps arrows intuitive with newest at bottom.

    def append(self, ch: str, who: str, text: str, stamp: str = None):
        if stamp is None:
            stamp = now_hms()
        self.add_channel(ch)
        self.messages[ch].append((stamp, who, text))
        if len(self.messages[ch]) > 1000:
            self.messages[ch] = self.messages[ch][-1000:]

    def selected_channel(self) -> str:
        return self.channels[self.selected_idx]


def parse_from_payload(payload: str):
    parts = payload.split(" ", 1)
    if len(parts) == 1:
        return parts[0], ""
    return parts[0], parts[1]


def start_local_servers(root: str, mode: str, disc_port: int, chat_port: int):
    os.makedirs(os.path.join(root, "results", "ui_logs"), exist_ok=True)
    dlog = open(os.path.join(root, "results", "ui_logs", "discovery.log"), "a", buffering=1, encoding="utf-8")
    slog = open(os.path.join(root, "results", "ui_logs", f"chat_{mode}.log"), "a", buffering=1, encoding="utf-8")

    discovery_bin = os.path.join(root, "bin", "discovery_server")
    chat_bin = os.path.join(root, "bin", f"chat_server_{mode}")
    disc = subprocess.Popen([discovery_bin, str(disc_port)], stdout=dlog, stderr=dlog, cwd=root)
    time.sleep(0.3)
    chat = subprocess.Popen([chat_bin, str(chat_port)], stdout=slog, stderr=slog, cwd=root)
    time.sleep(0.5)
    return [disc, chat]


def register_with_discovery(disc_ip: str, disc_port: int, username: str, password: str, client_port: int):
    s = socket.create_connection((disc_ip, disc_port), timeout=4)
    try:
        send_message(s, MSG_DISC_REGISTER_REQ, f"{username} {password} {client_port}")
        mtype, payload = recv_message(s)
        if mtype != MSG_DISC_REGISTER_RESP:
            raise RuntimeError(f"discovery register failed: {payload}")
    finally:
        s.close()


def receiver_loop(sock: socket.socket, q: queue.Queue, stop_event: threading.Event):
    try:
        while not stop_event.is_set():
            try:
                mtype, payload = recv_message(sock)
                q.put((mtype, payload))
            except socket.timeout:
                continue
    except Exception as e:
        q.put(("__DISCONNECT__", str(e)))


def draw(stdscr, st: TuiState):
    h, w = stdscr.getmaxyx()
    left_w = max(24, min(32, w // 4))
    top_h = h - 3

    stdscr.erase()
    stdscr.addstr(
        0,
        0,
        f"User: {st.username} | Channel: {st.selected_channel()} | {st.status_line}"[: max(0, w - 1)],
        curses.color_pair(PAIR_HEADER),
    )

    # Vertical split
    for y in range(1, top_h):
        if left_w < w:
            stdscr.addch(y, left_w, "|")

    # Channels pane
    stdscr.addstr(1, 1, "Conversations", curses.color_pair(PAIR_HEADER))
    for i, ch in enumerate(st.channels[: max(0, top_h - 3)]):
        marker = ">" if i == st.selected_idx else " "
        if ch == "#broadcast":
            label_name = "Broadcast"
            color_attr = curses.A_NORMAL
        else:
            s = st.user_status.get(ch, "?")
            label_name = f"{ch} [{s}]"
            if s == "available":
                color_attr = curses.color_pair(PAIR_STATUS_OK)
            elif s == "away":
                color_attr = curses.color_pair(PAIR_STATUS_AWAY)
            elif s == "busy":
                color_attr = curses.color_pair(PAIR_STATUS_BUSY)
            else:
                color_attr = curses.A_NORMAL
        label = f"{marker} {label_name}"
        if i == st.selected_idx:
            color_attr |= curses.A_BOLD
        stdscr.addstr(2 + i, 1, label[: max(1, left_w - 2)], color_attr)

    # Messages pane
    msg_x = left_w + 2
    msg_w = max(10, w - msg_x - 1)
    ch = st.selected_channel()
    msgs = st.messages[ch]
    visible_h = max(1, top_h - 2)
    view = msgs[-visible_h:]
    for i, (tstamp, who, text) in enumerate(view):
        line = f"[{tstamp}] {who}: {text}"
        attr = curses.A_NORMAL
        if who == "system":
            attr = curses.color_pair(PAIR_SYSTEM)
        stdscr.addstr(1 + i, msg_x, line[:msg_w], attr)

    # Input/status lines
    stdscr.addstr(h - 2, 0, "-" * max(0, w - 1))
    stdscr.addstr(h - 1, 0, ("you> " + st.input_line)[: max(0, w - 1)])
    stdscr.move(h - 1, min(w - 1, len("you> ") + len(st.input_line)))
    stdscr.refresh()


def handle_history_payload(st: TuiState, payload: str):
    lines = [ln for ln in payload.splitlines() if ln.strip()]
    if not lines:
        st.status_line = "No history entries."
        return
    loaded = 0
    for ln in lines:
        try:
            obj = json.loads(ln)
        except Exception:
            continue
        et = obj.get("type", "")
        peer = obj.get("peer", "")
        text = obj.get("text", "")
        tstamp = obj.get("timestamp", now_hms())
        if et.startswith("broadcast_"):
            who = "me" if et == "broadcast_send" else peer
            st.append("#broadcast", who, text, stamp=tstamp)
            loaded += 1
        elif et.startswith("private_") and peer and peer != "-":
            ch = peer
            who = "me" if et == "private_send" else peer
            st.touch_channel_recent(ch)
            st.append(ch, who, text, stamp=tstamp)
            loaded += 1
    st.status_line = f"History loaded ({loaded} entries)."


def parse_online_statuses(st: TuiState, payload: str):
    st.online_users_text = payload
    new_map = {}
    for ln in payload.splitlines():
        line = ln.strip()
        if not line:
            continue
        # Expected server format: "username (status)"
        if " (" in line and line.endswith(")"):
            idx = line.rfind(" (")
            user = line[:idx].strip()
            status = line[idx + 2 : -1].strip()
            if user:
                new_map[user] = status
        else:
            parts = line.split()
            if parts:
                new_map[parts[0]] = "?"
    st.user_status.update(new_map)


def run_tui(stdscr, args, sock: socket.socket):
    init_colors()

    curses.curs_set(1)
    stdscr.keypad(True)
    stdscr.nodelay(True)
    stdscr.timeout(80)

    st = TuiState(args.username)
    q = queue.Queue()
    stop_event = threading.Event()
    th = threading.Thread(target=receiver_loop, args=(sock, q, stop_event), daemon=True)
    th.start()

    # Initial pulls for context
    send_message(sock, MSG_LIST_REQ, "")
    send_message(sock, MSG_HISTORY_REQ, "")

    while True:
        while True:
            try:
                evt = q.get_nowait()
            except queue.Empty:
                break

            if evt[0] == "__DISCONNECT__":
                st.status_line = f"Disconnected: {evt[1]}"
                draw(stdscr, st)
                time.sleep(1.5)
                return

            mtype, payload = evt
            if mtype == MSG_BROADCAST_DELIVER:
                who, text = parse_from_payload(payload)
                # Avoid duplicate self-echo in UI: we render self-broadcast on receipt only.
                st.append("#broadcast", who, text)
            elif mtype == MSG_PRIVATE_DELIVER:
                who, text = parse_from_payload(payload)
                if who == st.username:
                    continue
                st.touch_channel_recent(who)
                st.append(who, who, text)
            elif mtype == MSG_LIST_RESP:
                parse_online_statuses(st, payload)
                st.status_line = "Online users updated."
            elif mtype == MSG_HISTORY_RESP:
                handle_history_payload(st, payload)
            elif mtype == MSG_STATUS:
                st.user_status[st.username] = payload
                st.status_line = f"My status: {payload}"
            elif mtype == MSG_ACK:
                st.status_line = payload
            elif mtype == MSG_ERROR:
                st.status_line = f"Error: {payload}"
            else:
                st.status_line = f"Type {mtype}: {payload}"

        draw(stdscr, st)
        ch = stdscr.getch()
        if ch == -1:
            continue

        if ch in (curses.KEY_UP,):
            st.selected_idx = max(0, st.selected_idx - 1)
            continue
        if ch in (curses.KEY_DOWN,):
            st.selected_idx = min(len(st.channels) - 1, st.selected_idx + 1)
            continue
        if ch in (9,):  # tab
            st.selected_idx = (st.selected_idx + 1) % len(st.channels)
            continue
        if ch in (3,):  # Ctrl+C
            break
        if ch in (10, 13):
            line = st.input_line.strip()
            st.input_line = ""
            if not line:
                continue

            if line.startswith("/quit"):
                break
            if line.startswith("/help"):
                st.status_line = "Commands: /w user msg, /b msg, /list, /status s, /history, /online, /quit"
                continue
            if line.startswith("/list"):
                send_message(sock, MSG_LIST_REQ, "")
                continue
            if line.startswith("/online"):
                st.append("#broadcast", "system", st.online_users_text or "(no online users cached)")
                continue
            if line.startswith("/history"):
                send_message(sock, MSG_HISTORY_REQ, "")
                continue
            if line.startswith("/status "):
                send_message(sock, MSG_STATUS_SET_REQ, line[8:].strip())
                send_message(sock, MSG_LIST_REQ, "")
                continue
            if line.startswith("/w "):
                parts = line.split(" ", 2)
                if len(parts) < 3:
                    st.status_line = "Usage: /w <user> <message>"
                    continue
                to_user, msg = parts[1], parts[2]
                send_message(sock, MSG_PRIVATE_REQ, f"{to_user} {msg}")
                st.touch_channel_recent(to_user)
                st.append(to_user, "me", msg)
                st.selected_idx = st.channels.index(to_user)
                continue
            if line.startswith("/b "):
                msg = line[3:]
                send_message(sock, MSG_BROADCAST_REQ, msg)
                continue

            # Default send: selected channel decides route
            selected = st.selected_channel()
            if selected == "#broadcast":
                send_message(sock, MSG_BROADCAST_REQ, line)
            else:
                send_message(sock, MSG_PRIVATE_REQ, f"{selected} {line}")
                st.append(selected, "me", line)
            continue

        if ch in (curses.KEY_BACKSPACE, 127, 8):
            st.input_line = st.input_line[:-1]
            continue
        if 32 <= ch <= 126:
            st.input_line += chr(ch)

    stop_event.set()
    try:
        send_message(sock, MSG_DISCONNECT, "")
    except Exception:
        pass


def read_field(stdscr, y: int, x: int, prompt: str, initial: str = "", hidden: bool = False) -> str:
    val = list(initial)
    while True:
        stdscr.addstr(y, x, " " * 80)
        stdscr.addstr(y, x, prompt)
        shown = ("*" * len(val)) if hidden else "".join(val)
        stdscr.addstr(y, x + len(prompt), shown)
        stdscr.move(y, x + len(prompt) + len(shown))
        stdscr.refresh()
        ch = stdscr.getch()
        if ch in (10, 13):
            return "".join(val).strip()
        if ch in (27,):  # ESC clears field
            val = []
            continue
        if ch in (curses.KEY_BACKSPACE, 127, 8):
            if val:
                val.pop()
            continue
        if 32 <= ch <= 126:
            val.append(chr(ch))


def login_screen(stdscr, username_default: str, password_default: str, port_default: str):
    init_colors()
    curses.curs_set(1)
    stdscr.keypad(True)
    stdscr.nodelay(False)
    stdscr.timeout(-1)

    while True:
        stdscr.erase()
        stdscr.addstr(1, 2, "Chat TUI Login", curses.color_pair(PAIR_HEADER) | curses.A_BOLD)
        stdscr.addstr(2, 2, "Enter credentials (Press Enter to submit each field)")
        stdscr.addstr(3, 2, "Tip: ESC while editing a field clears it", curses.color_pair(PAIR_SYSTEM))

        username = read_field(stdscr, 5, 2, "Username: ", username_default, hidden=False)
        password = read_field(stdscr, 6, 2, "Password: ", password_default, hidden=True)
        client_port_s = read_field(stdscr, 7, 2, "Client port: ", port_default, hidden=False)

        err = None
        if not username:
            err = "Username cannot be empty."
        elif not password:
            err = "Password cannot be empty."
        else:
            try:
                cp = int(client_port_s)
                if cp <= 0 or cp > 65535:
                    err = "Client port must be between 1 and 65535."
            except Exception:
                err = "Client port must be a number."
                cp = 0

        if err is None:
            return username, password, cp

        stdscr.addstr(9, 2, f"Error: {err}", curses.color_pair(PAIR_ERROR) | curses.A_BOLD)
        stdscr.addstr(10, 2, "Press any key to retry...")
        stdscr.refresh()
        stdscr.getch()


def main():
    ap = argparse.ArgumentParser(description="Chat TUI with contacts + channel switching")
    ap.add_argument("--disc-ip", default="127.0.0.1")
    ap.add_argument("--disc-port", type=int, default=9000)
    ap.add_argument("--chat-ip", default="127.0.0.1")
    ap.add_argument("--chat-port", type=int, default=9100)
    ap.add_argument("--username")
    ap.add_argument("--password")
    ap.add_argument("--client-port", type=int)
    ap.add_argument("--start-local", action="store_true")
    ap.add_argument("--mode", choices=["thread", "fork", "select"], default="thread")
    ap.add_argument("--register", action="store_true", help="(legacy) Register credentials with discovery before login")
    ap.add_argument("--no-register", action="store_true", help="Disable auto-registration before login")
    args = ap.parse_args()

    root = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
    procs = []
    sock = None

    try:
        if args.start_local:
            procs = start_local_servers(root, args.mode, args.disc_port, args.chat_port)

        if not args.username or not args.password or args.client_port is None:
            udef = args.username or ""
            pdef = args.password or ""
            cdef = str(args.client_port) if args.client_port is not None else "10001"
            args.username, args.password, args.client_port = curses.wrapper(login_screen, udef, pdef, cdef)

        should_register = args.register or (not args.no_register)
        if should_register:
            try:
                register_with_discovery(args.disc_ip, args.disc_port, args.username, args.password, args.client_port)
            except Exception as e:
                # Existing username + wrong password should still fail at chat login.
                if "username exists; wrong password" not in str(e):
                    print(f"warning: discovery register issue: {e}")

        sock = socket.create_connection((args.chat_ip, args.chat_port), timeout=5)
        sock.settimeout(None)
        send_message(sock, MSG_LOGIN_REQ, f"{args.username} {args.password}")
        t, p = recv_message(sock)
        if t != MSG_LOGIN_RESP or not p.startswith("OK"):
            raise RuntimeError(f"login failed: {p}")

        curses.wrapper(run_tui, args, sock)
        return 0
    except Exception as e:
        print(f"chat_tui error: {e}")
        return 1
    finally:
        if sock is not None:
            try:
                sock.close()
            except Exception:
                pass
        for p in reversed(procs):
            if p.poll() is None:
                p.terminate()
        for p in reversed(procs):
            if p.poll() is None:
                try:
                    p.wait(timeout=2)
                except Exception:
                    p.kill()


if __name__ == "__main__":
    raise SystemExit(main())
