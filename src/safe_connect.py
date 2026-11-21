"""Safe Connect 1.0: keeps a jailbroken PS5 from downloading system updates.

Based on the Update blocker in Prospero Sender Turbo.

The PS5 downloads a system update into /update, as PS5UPDATE.PUP.net.temp
and then PS5UPDATE.PUP. A folder already standing at each of those names
(with a read-only file inside, so it is never empty) leaves the download
nowhere to write. A downloaded update waiting there is deleted first.

Everything goes over the PS5's FTP payload (usually port 2121). Standard
library only: tkinter, ftplib, threading.
"""
from __future__ import annotations

import ftplib
import json
import math
import os
import queue
import socket
import sys
import threading
import time
import tkinter as tk
from pathlib import Path
from tkinter import messagebox
from typing import Callable

APP_NAME = "Safe Connect"
VERSION = "1.0"
FTP_TIMEOUT = 8

# --- the blocker ----------------------------------------
UPDATE_DIRS = ("/update",)
UPDATE_NAMES = ("PS5UPDATE.PUP", "PS5UPDATE.PUP.net.temp")
UPDATE_DUMMY = "prospero-win-update-blocker"     # the name Prospero Sender Turbo uses too
UPDATE_DUMMY_TEXT = (b"Made by Safe Connect so the PS5 can't save a system update here.\n"
                     b"Undo it in the app: Remove blocker.\n")


class Remote:
    """The PS5's FTP server: MLSD lists the current directory only."""

    def __init__(self, host: str, port: int):
        self.host, self.port = host, port
        self.ftp = ftplib.FTP()
        self.ftp.connect(host, port, FTP_TIMEOUT)
        self.ftp.login()
        try:
            self.ftp.voidcmd("TYPE I")
        except ftplib.all_errors:
            pass

    def listdir(self, path: str) -> dict[str, tuple[str, int]]:
        self.ftp.cwd(path)
        out = {}
        for name, facts in self.ftp.mlsd():
            if name in (".", "..") or facts.get("type") in ("cdir", "pdir"):
                continue
            try:
                size = int(facts.get("size", 0))
            except ValueError:
                size = 0
            out[name] = (facts.get("type", "file").lower(), size)
        return out

    def write(self, path: str, data: bytes) -> None:
        import io
        self.ftp.storbinary(f"STOR {path}", io.BytesIO(data))

    def delete(self, path: str) -> None:
        # the PS5's ftpsrv confirms a delete with 226, which ftplib's delete() refuses
        self.ftp.sendcmd(f"DELE {path}")

    def chmod(self, path: str, mode: str) -> bool:
        try:
            self.ftp.sendcmd(f"SITE CHMOD {mode} {path}")
            return True
        except ftplib.all_errors:
            return False

    def close(self) -> None:
        try:
            self.ftp.quit()
        except (OSError, EOFError, *ftplib.all_errors):
            try:
                self.ftp.close()
            except OSError:
                pass


class BlockerError(Exception):
    pass


def connect(host: str, port: int) -> Remote:
    try:
        return Remote(host, port)
    except (OSError, EOFError, *ftplib.all_errors) as error:
        raise BlockerError(f"no FTP server answers at {host}:{port} ({error}). Check the address "
                           "and that the PS5's FTP payload is running.") from error


def _gone(remote: Remote, path: str) -> bool:
    try:
        remote.delete(path)
        return True
    except ftplib.all_errors:
        return False


def _kind(remote: Remote, folder: str, name: str) -> str | None:
    """'dir', 'file' or None (nothing there)."""
    try:
        entries = remote.listdir(folder)
    except ftplib.error_perm:
        return None
    entry = entries.get(name)
    return None if entry is None else ("dir" if entry[0] == "dir" else "file")


def _update_names(remote: Remote, folder: str) -> list[str]:
    """The names the update downloads to, and any other PS5UPDATE* folder there
    (one block_updates() made from a download it deleted)."""
    names = list(UPDATE_NAMES)
    try:
        for name, (kind, _size) in remote.listdir(folder).items():
            if kind == "dir" and name.upper().startswith("PS5UPDATE") and name not in names:
                names.append(name)
    except ftplib.error_perm:
        pass
    return names


def update_blocker_state(remote: Remote) -> list[tuple[str, str]]:
    """(path, 'blocked' / 'update file' / 'open' / 'no folder') for each name."""
    states = []
    for folder in UPDATE_DIRS:
        try:
            remote.listdir(folder)
        except ftplib.error_perm:
            states += [(f"{folder}/{name}", "no folder") for name in UPDATE_NAMES]
            continue
        for name in _update_names(remote, folder):
            kind = _kind(remote, folder, name)
            states.append((f"{folder}/{name}", {"dir": "blocked", "file": "update file"}.get(kind, "open")))
    return states


def block_updates(remote: Remote, say: Callable[[str], None] = print) -> list[tuple[str, str]]:
    """Puts a folder holding a read-only file at each name the update downloads
    to. Returns update_blocker_state() afterwards."""
    for folder in UPDATE_DIRS:
        try:
            entries = remote.listdir(folder)
        except ftplib.error_perm:
            say(f"{folder} can't be opened over FTP")
            continue
        # a finished or half-finished download: gone, so it can't install.
        # Whatever name it had gets blocked too.
        names = list(UPDATE_NAMES)
        for name, (kind, size) in entries.items():
            if kind != "dir" and name.upper().startswith("PS5UPDATE"):
                say(f"Deleting the downloaded update {folder}/{name} ({size:,} bytes)")
                _gone(remote, f"{folder}/{name}")
                if name not in names:
                    names.append(name)
        for name in names:
            path = f"{folder}/{name}"
            say(f"Blocking {path}")
            if _kind(remote, folder, name) != "dir":
                try:
                    remote.ftp.mkd(path)
                except ftplib.all_errors:
                    pass
            dummy = f"{path}/{UPDATE_DUMMY}"
            try:
                remote.write(dummy, UPDATE_DUMMY_TEXT)
            except ftplib.all_errors:
                pass
            remote.chmod(dummy, "444")
            remote.chmod(path, "555")
    try:
        remote.ftp.cwd("/")
    except ftplib.all_errors:
        pass
    return update_blocker_state(remote)


def unblock_updates(remote: Remote, say: Callable[[str], None] = print) -> list[tuple[str, str]]:
    """Takes away what block_updates() made. Folders holding anything else stay."""
    for folder in UPDATE_DIRS:
        for name in _update_names(remote, folder):
            path = f"{folder}/{name}"
            if _kind(remote, folder, name) != "dir":
                continue
            say(f"Removing {path}")
            remote.chmod(path, "777")
            remote.chmod(f"{path}/{UPDATE_DUMMY}", "666")
            _gone(remote, f"{path}/{UPDATE_DUMMY}")
            try:
                remote.ftp.cwd("/")
                remote.ftp.sendcmd(f"RMD {path}")
            except ftplib.all_errors:
                say(f"{path} was left (not empty, or the server refused)")
    return update_blocker_state(remote)


def is_blocked(states: list[tuple[str, str]]) -> bool:
    return bool(states) and all(state == "blocked" for _path, state in states)


def needs_block(states: list[tuple[str, str]]) -> bool:
    return any(state in ("open", "update file") for _path, state in states)


# --- settings ---------------------------------------------------------------------
STATE_DIR = Path(os.environ.get("LOCALAPPDATA") or Path.home() / ".local" / "state") / "safe-connect"
SETTINGS = STATE_DIR / "settings.json"


def load_settings() -> dict:
    """What was typed last time. First launch: 0.0.0.0 and port 2121, the port
    most PS5 FTP payloads use."""
    try:
        values = json.loads(SETTINGS.read_text())
        if isinstance(values, dict):
            return values
    except (OSError, ValueError):
        pass
    return {"host": "0.0.0.0", "port": "2121"}


def host_set(host: str) -> bool:
    return bool(host) and host != "0.0.0.0"


def save_settings(values: dict) -> None:
    try:
        STATE_DIR.mkdir(parents=True, exist_ok=True)
        SETTINGS.write_text(json.dumps(values, indent=1))
    except OSError:
        pass


def resource(name: str) -> Path:
    base = Path(getattr(sys, "_MEIPASS", Path(__file__).resolve().parent))
    return base / name


# --- the window -------------------------------------------------------------------
BG = "#070B14"
PANEL = "#0E1526"
EDGE = "#1C2A48"
TEXT = "#DCE6FF"
DIM = "#7083AD"
CYAN = "#29D3FF"
GREEN = "#2BFF88"
RED = "#FF3B5C"
AMBER = "#FFB020"
FONT = "Segoe UI"
MONO = "Consolas"

SCENE_HEIGHT = 280
ANIM_LENGTH = 5.0

LOOK = {"blocked": ("BLOCKED", GREEN), "open": ("OPEN: an update can download", RED),
        "update file": ("AN UPDATE IS DOWNLOADED HERE", RED),
        "no folder": ("folder not reachable over FTP", AMBER)}
ICON_PNG = "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"
INTERVALS = {"30 s": 30, "1 min": 60, "5 min": 300, "15 min": 900}


class App:
    def __init__(self, root: tk.Tk):
        self.root = root
        settings = load_settings()
        root.title(f"{APP_NAME} {VERSION}")
        root.configure(bg=BG)
        try:
            self.icon = tk.PhotoImage(data=ICON_PNG)
            root.iconphoto(True, self.icon)
        except tk.TclError:
            pass
        root.minsize(720, 640)
        root.geometry("760x700")
        self.host = tk.StringVar(value=settings.get("host") or "0.0.0.0")
        self.port = tk.StringVar(value=str(settings.get("port") or "2121"))
        self.watch = tk.BooleanVar(value=settings.get("watch", True))
        self.auto_block = tk.BooleanVar(value=settings.get("auto_block", True))
        self.interval = tk.StringVar(value=settings.get("interval", "1 min"))
        self.events: queue.Queue = queue.Queue()
        self.busy = False
        self.mode = "idle"            # idle / busy / blocked / open / error
        self.phase = 0.0
        self.next_check = 0.0
        self.last_states: list[tuple[str, str]] = []
        self.shown: str | None = None     # last result: "blocked" / "open"
        self.anim_start: float | None = None
        self.build()
        self.root.protocol("WM_DELETE_WINDOW", self.quit)
        self.root.after(30, self.animate)
        self.root.after(100, self.pump)
        self.root.after(300, self.tick)
        if host_set(self.host.get().strip()):
            self.root.after(600, lambda: self.run("check", quiet=True))
        else:
            self.log("Type the PS5's IP address and press CHECK.", DIM)

    # layout
    def build(self) -> None:
        top = tk.Frame(self.root, bg=BG)
        top.pack(fill="x", padx=18, pady=(14, 4))
        tk.Label(top, text="SAFE CONNECT", bg=BG, fg=CYAN, font=(FONT, 20, "bold")).pack(side="left")
        tk.Label(top, text="PS5 update blocker", bg=BG, fg=DIM, font=(FONT, 10)).pack(side="left", padx=(10, 0), pady=(10, 0))
        tk.Label(top, text=f"v{VERSION}", bg=BG, fg=DIM, font=(FONT, 10)).pack(side="left", padx=(8, 0), pady=(10, 0))

        conn = tk.Frame(self.root, bg=PANEL, highlightbackground=EDGE, highlightthickness=1)
        conn.pack(fill="x", padx=18, pady=6)
        tk.Label(conn, text="PS5 IP", bg=PANEL, fg=DIM, font=(FONT, 10, "bold")).grid(row=0, column=0, padx=(12, 6), pady=10)
        host = tk.Entry(conn, textvariable=self.host, width=18, bg=BG, fg=TEXT, insertbackground=CYAN,
                        relief="flat", font=(MONO, 13), highlightthickness=1, highlightcolor=CYAN,
                        highlightbackground=EDGE)
        host.grid(row=0, column=1, pady=10, ipady=3)
        host.bind("<Return>", lambda _e: self.run("check"))
        tk.Label(conn, text="FTP PORT", bg=PANEL, fg=DIM, font=(FONT, 10, "bold")).grid(row=0, column=2, padx=(16, 6))
        port = tk.Entry(conn, textvariable=self.port, width=6, bg=BG, fg=TEXT, insertbackground=CYAN,
                        relief="flat", font=(MONO, 13), highlightthickness=1, highlightcolor=CYAN,
                        highlightbackground=EDGE)
        port.grid(row=0, column=3, ipady=3)
        port.bind("<Return>", lambda _e: self.run("check"))
        self.check_button = self.button(conn, "CHECK", CYAN, lambda: self.run("check"))
        self.check_button.grid(row=0, column=4, padx=(16, 12))
        conn.grid_columnconfigure(5, weight=1)

        self.canvas = tk.Canvas(self.root, height=SCENE_HEIGHT, bg=BG, highlightthickness=0)
        self.canvas.pack(fill="x", padx=18, pady=(6, 0))

        actions = tk.Frame(self.root, bg=BG)
        actions.pack(pady=(4, 6))
        self.block_button = self.button(actions, "BLOCK UPDATES", GREEN, lambda: self.run("block"), big=True)
        self.block_button.pack(side="left", padx=6)
        self.remove_button = self.button(actions, "REMOVE BLOCKER", RED, self.ask_remove, big=True)
        self.remove_button.pack(side="left", padx=6)

        auto = tk.Frame(self.root, bg=PANEL, highlightbackground=EDGE, highlightthickness=1)
        auto.pack(fill="x", padx=18, pady=4)
        self.check(auto, "Keep checking the PS5 every", self.watch).grid(row=0, column=0, padx=(10, 4), pady=6, sticky="w")
        menu = tk.OptionMenu(auto, self.interval, *INTERVALS, command=lambda _v: self.saved())
        menu.configure(bg=BG, fg=TEXT, activebackground=EDGE, activeforeground=TEXT, relief="flat",
                       highlightthickness=0, font=(FONT, 9))
        menu["menu"].configure(bg=PANEL, fg=TEXT)
        menu.grid(row=0, column=1, sticky="w")
        self.countdown = tk.Label(auto, text="", bg=PANEL, fg=DIM, font=(FONT, 9))
        self.countdown.grid(row=0, column=2, padx=10, sticky="w")
        self.check(auto, "Put the blocker back automatically if the PS5 clears it "
                         "(a hard crash or a factory reset empties /update)",
                   self.auto_block).grid(row=1, column=0, columnspan=3, padx=10, pady=(0, 6), sticky="w")

        self.rows = tk.Frame(self.root, bg=PANEL, highlightbackground=EDGE, highlightthickness=1)
        self.rows.pack(fill="x", padx=18, pady=4)
        tk.Label(self.rows, text="Not checked yet.", bg=PANEL, fg=DIM, font=(FONT, 10)).pack(anchor="w", padx=12, pady=8)

        logbox = tk.Frame(self.root, bg=PANEL, highlightbackground=EDGE, highlightthickness=1)
        logbox.pack(fill="both", expand=True, padx=18, pady=(4, 6))
        self.logtext = tk.Text(logbox, height=6, bg=PANEL, fg=TEXT, relief="flat", font=(MONO, 9),
                               state="disabled", wrap="word", padx=10, pady=6)
        self.logtext.pack(fill="both", expand=True)
        for name, colour in (("dim", DIM), ("ok", GREEN), ("bad", RED), ("warn", AMBER), ("info", CYAN)):
            self.logtext.tag_configure(name, foreground=colour)

        tk.Label(self.root, bg=BG, fg=DIM, font=(FONT, 8), wraplength=720, justify="left",
                 text="Also turn off Settings > System > System Software > System Software Update and Settings > "
                      "Download Update Files Automatically / Install Update Files Automatically. A manual USB or "
                      "recovery-mode update ignores the blocker.").pack(fill="x", padx=18, pady=(0, 10))

    def button(self, parent, text, colour, command, big=False) -> tk.Label:
        widget = tk.Label(parent, text=text, bg=BG, fg=colour, cursor="hand2",
                          font=(FONT, 12 if big else 10, "bold"), padx=18 if big else 12, pady=8 if big else 4,
                          highlightbackground=colour, highlightthickness=2)
        widget.bind("<Button-1>", lambda _e: None if widget.cget("state") == "disabled" else command())
        widget.bind("<Enter>", lambda _e: widget.configure(bg=colour, fg=BG) if widget.cget("state") != "disabled" else None)
        widget.bind("<Leave>", lambda _e: widget.configure(bg=BG, fg=colour))
        return widget

    def check(self, parent, text, variable) -> tk.Checkbutton:
        return tk.Checkbutton(parent, text=text, variable=variable, command=self.saved, bg=PANEL, fg=TEXT,
                              selectcolor=BG, activebackground=PANEL, activeforeground=CYAN,
                              font=(FONT, 9), anchor="w", justify="left")

    def saved(self) -> None:
        save_settings({"host": self.host.get().strip(), "port": self.port.get().strip(),
                       "watch": self.watch.get(), "auto_block": self.auto_block.get(),
                       "interval": self.interval.get()})
        self.next_check = time.time() + INTERVALS.get(self.interval.get(), 60)

    def log(self, line: str, colour: str = TEXT) -> None:
        tag = {DIM: "dim", GREEN: "ok", RED: "bad", AMBER: "warn", CYAN: "info"}.get(colour, "")
        self.logtext.configure(state="normal")
        self.logtext.insert("end", time.strftime("%H:%M:%S  "), "dim")
        self.logtext.insert("end", line + "\n", tag)
        self.logtext.see("end")
        self.logtext.configure(state="disabled")

    # work
    def address(self) -> tuple[str, int]:
        host = self.host.get().strip()
        if not host_set(host):
            raise BlockerError("type the PS5's IP address first (0.0.0.0 is only a placeholder)")
        try:
            port = int(self.port.get().strip() or 2121)
        except ValueError:
            raise BlockerError(f"{self.port.get()!r} is not a port number (the PS5's FTP payload usually uses 2121)")
        return host, port

    def ask_remove(self) -> None:
        if messagebox.askyesno(APP_NAME, "Let the PS5 download system updates again?\n\n"
                               "An update can end the jailbreak for good.", icon="warning", parent=self.root):
            if self.auto_block.get():
                self.auto_block.set(False)       # or the next check would put it straight back
                self.log("Turned off automatic put-back, so the blocker stays removed.", AMBER)
            self.run("unblock")

    def run(self, action: str, quiet: bool = False) -> None:
        if self.busy:
            return
        try:
            host, port = self.address()
        except BlockerError as error:
            if not quiet:
                messagebox.showerror(APP_NAME, str(error), parent=self.root)
            return
        self.saved()
        self.busy = True
        self.mode = "busy"
        for widget in (self.check_button, self.block_button, self.remove_button):
            widget.configure(state="disabled")
        verb = {"check": "Checking", "block": "Blocking updates on", "unblock": "Removing the blocker on",
                "auto": "Checking"}[action]
        self.log(f"{verb} {host}:{port}…", CYAN)
        auto = self.auto_block.get()
        threading.Thread(target=self.work, args=(action, host, port, auto), daemon=True).start()

    def work(self, action: str, host: str, port: int, auto: bool) -> None:
        say = lambda line: self.events.put(("log", line, TEXT))   # noqa: E731
        try:
            remote = connect(host, port)
            try:
                if action == "block":
                    states = block_updates(remote, say)
                elif action == "unblock":
                    states = unblock_updates(remote, say)
                else:
                    states = update_blocker_state(remote)
                    if action == "auto" and auto and needs_block(states):
                        self.events.put(("log", "The PS5 cleared the blocker. Putting it back…", AMBER))
                        states = block_updates(remote, say)
                        action = "reblock"
            finally:
                remote.close()
            self.events.put(("done", action, states))
        except BlockerError as error:
            self.events.put(("fail", str(error)))
        except (OSError, EOFError, *ftplib.all_errors) as error:
            self.events.put(("fail", f"the FTP connection broke ({error})"))

    def pump(self) -> None:
        try:
            while True:
                event = self.events.get_nowait()
                if event[0] == "log":
                    self.log(event[1], event[2])
                elif event[0] == "done":
                    self.finished(event[1], event[2])
                elif event[0] == "fail":
                    self.failed(event[1])
        except queue.Empty:
            pass
        self.root.after(100, self.pump)

    def ready(self) -> None:
        self.busy = False
        for widget in (self.check_button, self.block_button, self.remove_button):
            widget.configure(state="normal")
        self.next_check = time.time() + INTERVALS.get(self.interval.get(), 60)

    def finished(self, action: str, states: list[tuple[str, str]]) -> None:
        self.ready()
        was_blocked = is_blocked(self.last_states)
        self.last_states = states
        blocked = is_blocked(states)
        self.mode = "blocked" if blocked else "open"
        if blocked and self.shown != "blocked":
            self.anim_start = time.time()      # the protection animation
        elif not blocked:
            self.anim_start = None
        self.shown = self.mode
        self.show_rows(states)
        if action in ("block", "reblock"):
            if blocked:
                self.log("Blocker ON. The PS5 can't download system updates.", GREEN)
            else:
                self.log("Some places could not be blocked (see the list above).", RED)
        elif action == "unblock":
            self.log("Blocker removed. The PS5 can download system updates again.", AMBER)
        else:
            if blocked:
                self.log("Blocker is on.", GREEN)
            elif any(state == "update file" for _p, state in states):
                self.log("An update is already DOWNLOADED on the PS5. Press BLOCK UPDATES to delete it and block.", RED)
            elif all(state == "no folder" for _p, state in states):
                self.log("/update can't be opened over FTP. The FTP payload may not have full access.", AMBER)
            else:
                self.log("Blocker is OFF. The PS5 can download system updates. Press BLOCK UPDATES.", RED)

    def failed(self, text: str) -> None:
        self.ready()
        self.mode = "error"
        self.log(f"Couldn't reach the PS5: {text}", AMBER)

    def show_rows(self, states: list[tuple[str, str]]) -> None:
        for child in self.rows.winfo_children():
            child.destroy()
        for row, (path, state) in enumerate(states):
            text, colour = LOOK.get(state, (state, TEXT))
            tk.Label(self.rows, text="●", bg=PANEL, fg=colour, font=(FONT, 12)).grid(row=row, column=0, padx=(12, 6), pady=2)
            tk.Label(self.rows, text=path, bg=PANEL, fg=TEXT, font=(MONO, 10)).grid(row=row, column=1, sticky="w")
            tk.Label(self.rows, text=text, bg=PANEL, fg=colour, font=(FONT, 10, "bold")).grid(row=row, column=2, sticky="w", padx=16)

    def tick(self) -> None:
        if self.watch.get() and not self.busy and host_set(self.host.get().strip()):
            left = int(self.next_check - time.time())
            if left <= 0:
                self.run("auto", quiet=True)
                self.countdown.configure(text="checking…")
            else:
                self.countdown.configure(text=f"next check in {left // 60}:{left % 60:02d}"
                                         + ("  ·  auto put-back on" if self.auto_block.get() else ""))
        elif not self.watch.get():
            self.countdown.configure(text="(watching is off)")
        self.root.after(500, self.tick)

    def animate(self) -> None:
        self.phase += 0.06
        self.draw_ps5()
        self.root.after(30, self.animate)

    # --- the picture: a PS5 with its network cable ------------------------------
    @staticmethod
    def ease(x: float) -> float:
        x = min(max(x, 0.0), 1.0)
        return x * x * (3 - 2 * x)

    def tone(self, red: str, green: str, g: float) -> str:
        return self.mix(red, green, g)

    def draw_ps5(self) -> None:
        c = self.canvas
        c.delete("all")
        w = max(c.winfo_width(), 400)
        h = SCENE_HEIGHT
        cx = w // 2
        t = None if self.anim_start is None else time.time() - self.anim_start
        ease = self.ease
        dx, g, roll, ring, wrapper, tear, drop = 0.0, 0.0, 0.0, None, None, 0.0, 0.0
        if self.shown == "blocked":
            if t is None or t > ANIM_LENGTH:
                g, roll = 1.0, 1.0
            else:
                dx = 70 * ease(t / 0.8) if t < 3.4 else 70 * (1 - ease((t - 3.4) / 0.8))
                g = ease((t - 4.2) / 0.8)
                if 0.8 <= t < 3.4:
                    wrapper = ease((t - 0.8) / 0.4)
                    tear = ease((t - 1.3) / 0.5)
                    drop = ease((t - 2.6) / 0.8)
                if 1.8 <= t < 2.6:
                    ring = ease((t - 1.8) / 0.8)
                roll = ease((t - 2.6) / 0.8)
        pulse = (math.sin(self.phase * (2.4 if g < 0.5 else 1.0)) + 1) / 2
        light = self.tone("#FF8A9A", "#9CFFC4", g)
        mid = self.tone("#C0263E", "#1FA35C", g)
        dark = self.tone("#3A0A12", "#08361E", g)
        edge = self.tone(RED, GREEN, g)
        grid = self.tone("#1A0A10", "#0A1A12", g)
        for x in range(0, w, 28):
            c.create_line(x, 0, x, h, fill=grid)
        for y in range(0, h, 28):
            c.create_line(0, y, w, y, fill=grid)
        x0, top, k = cx - 100, 8, 0.25
        P = lambda pts: [v for x, y in pts for v in (x0 + (x - 855) * k, top + (y - 60) * k)]   # noqa: E731
        # (points traced from a photo of the console standing up, 1028 px tall)
        # glow
        for i in range(3):
            r = 104 + i * 14 + pulse * 5
            gx, gy = x0, top + 440 * k
            c.create_oval(gx - r * 0.7, gy - r, gx + r * 0.7, gy + r, outline=self.mix(edge, BG, 0.35 + i * 0.22), width=2)
        # stand
        c.create_polygon(P([(700, 900), (1010, 900), (1022, 958), (690, 958)]), fill=self.mix(dark, "#000000", 0.3),
                         outline=self.mix(edge, BG, 0.5), width=1, smooth=True)
        # black core
        core = [(780, 95), (910, 95), (920, 250), (916, 450), (898, 700), (882, 940), (786, 940), (790, 700),
                (806, 450), (792, 250)]
        c.create_polygon(P(core), fill=dark, outline=self.mix(edge, BG, 0.4), width=1)
        for side in ((792, 250, 806, 450, 790, 700, 786, 900), (920, 250, 916, 450, 898, 700, 884, 900)):
            c.create_line(P(list(zip(side[0::2], side[1::2]))), fill=self.tone("#7A1A34", "#1A7A4A", g), width=2, smooth=True)
        # front USB ports
        c.create_rectangle(P([(843, 522), (857, 552)]), fill="#05070C", outline=mid)
        c.create_oval(P([(845, 575), (855, 598)]), fill="#05070C", outline=mid)
        # the two white side plates
        left = [(730, 56), (752, 150), (774, 300), (786, 450), (770, 600), (746, 750), (728, 880), (730, 942),
                (794, 942), (798, 700), (814, 450), (800, 250), (790, 140), (788, 104), (768, 78)]
        right = [(952, 56), (936, 150), (926, 300), (934, 450), (958, 600), (990, 750), (1015, 880), (1018, 942),
                 (874, 942), (890, 700), (908, 450), (912, 250), (904, 130), (900, 104), (922, 78)]
        for plate in (left, right):
            c.create_polygon(P(plate), fill=light, outline=edge, width=2, smooth=True)
        # disc slot on the right plate
        c.create_line(P([(924, 612), (936, 905)]), fill=mid, width=3)
        # power light, blinks while talking to the PS5
        blink = self.mode == "busy" and int(self.phase * 4) % 2 == 0
        c.create_line(P([(800, 470), (793, 690)]), fill=CYAN if blink else self.mix(edge, "#FFFFFF", 0.3), width=2)
        # the network port, at the back: the cable comes out past the right plate
        port_x, port_y = x0 + (968 - 855) * k, top + (640 - 60) * k
        c.create_rectangle(port_x - 4, port_y - 9, port_x + 4, port_y + 9, fill="#05070C", outline=mid)
        # the cable and its RJ45 plug
        px = port_x + 2 + dx
        cable = self.tone("#D0304A", "#22C46E", g)
        c.create_line(px + 30, port_y, px + 70, port_y, cx + 90, port_y + 20, cx + 140, h - 20, w + 10, h - 6,
                      fill=self.mix(cable, BG, 0.45), width=10, smooth=True, capstyle="round")
        c.create_line(px + 30, port_y, px + 70, port_y, cx + 90, port_y + 20, cx + 140, h - 20, w + 10, h - 6,
                      fill=cable, width=6, smooth=True, capstyle="round")
        c.create_rectangle(px, port_y - 8, px + 30, port_y + 8, fill=self.tone("#FFC2CB", "#C8FFE0", g), outline=edge, width=2)
        c.create_rectangle(px + 6, port_y - 13, px + 20, port_y - 8, fill=self.tone("#FFC2CB", "#C8FFE0", g), outline=edge)
        for k in range(4):
            c.create_line(px + 3, port_y - 5 + k * 3.4, px + 9, port_y - 5 + k * 3.4, fill="#D4A017", width=1)
        # the protection, rolled onto the plug
        latex = self.tone("#FFB3C8", "#7DFFB0", g)
        if roll > 0:
            length = 46 * roll
            tip = px - 4
            c.create_polygon(tip, port_y - 11, tip + length, port_y - 11, tip + length, port_y + 11, tip, port_y + 11,
                             fill=latex, stipple="gray50", outline=latex, width=2)
            c.create_oval(tip - 8, port_y - 5, tip + 2, port_y + 5, fill=latex, stipple="gray50", outline=latex, width=2)
            c.create_oval(tip + length - 4, port_y - 13, tip + length + 4, port_y + 13, outline=latex, width=3)
        # the wrapper, torn open, then dropped
        if wrapper is not None:
            size = 66 * (0.4 + 0.6 * wrapper)
            wx, wy = cx + 110, 46 + drop * 220
            foil = self.mix("#E8C860", "#9AA4B8", 0.3)
            pts = []
            n = 7
            for i in range(n + 1):
                pts += [wx - size / 2 + size * i / n, wy - size / 2 + (4 if i % 2 else 0) + 12]
            for i in range(n + 1):
                pts += [wx + size / 2 - size * i / n, wy + size / 2 - (4 if i % 2 else 0)]
            c.create_polygon(pts, fill=foil, outline="#8A6D1A", width=2)
            if ring is None and drop < 0.05:
                c.create_oval(wx - size / 3, wy - size / 3 + 8, wx + size / 3, wy + size / 3 + 4, outline="#8A6D1A", width=2)
            strip = [wx - size / 2, wy - size / 2, wx + size / 2, wy - size / 2, wx + size / 2, wy - size / 2 + 12,
                     wx - size / 2, wy - size / 2 + 12]
            lift = tear * 60
            c.create_polygon([v + (lift * 1.2 if i % 2 == 0 else -lift) for i, v in enumerate(strip)],
                             fill=foil, outline="#8A6D1A", width=2)
            c.create_text(wx, wy + 14, text="SAFE\nCONNECT", fill="#5A4210", font=(FONT, 7, "bold"), justify="center")
        # the ring travelling from the wrapper to the plug
        if ring is not None:
            sx, sy = cx + 110, 52
            ex, ey = px - 4, port_y
            rx = sx + (ex - sx) * ring
            ry = sy + (ey - sy) * ring - math.sin(ring * math.pi) * 40
            half_w = 22 + (5 - 22) * ring
            half_h = 8 + (14 - 8) * ring
            c.create_oval(rx - half_w, ry - half_h, rx + half_w, ry + half_h, outline=latex, width=4)
        # sparkles once it's on
        if self.shown == "blocked" and t is not None and ANIM_LENGTH - 1.0 < t < ANIM_LENGTH + 2.0:
            k = t - (ANIM_LENGTH - 1.0)
            for i in range(7):
                a = i * 0.9 + k
                sx = port_x + 30 + math.cos(a) * (30 + k * 18)
                sy = port_y - 10 - k * 26 - i * 6
                c.create_text(sx, sy, text="✦" if i % 2 else "♥", fill=GREEN, font=(FONT, 10 + i % 3 * 2))
        if self.shown == "blocked":
            title = "PROTECTED: SYSTEM UPDATES BLOCKED" if (t is None or t > 4.2) else "PUTTING PROTECTION ON…"
            colour = GREEN if (t is None or t > 4.2) else CYAN
        elif self.shown == "open":
            title, colour = "UNPROTECTED: SYSTEM UPDATES CAN DOWNLOAD", RED
        else:
            title, colour = "NOT CHECKED YET", DIM
        if self.mode == "busy":
            title, colour = "TALKING TO THE PS5…", CYAN
        elif self.mode == "error":
            title, colour = "PS5 NOT REACHABLE", AMBER
        c.create_text(cx, h - 14, text=title, fill=colour, font=(FONT, 15, "bold"))

    @staticmethod
    def mix(a: str, b: str, t: float) -> str:
        """a blended toward b by t (0 = a, 1 = b)."""
        pa = [int(a[i:i + 2], 16) for i in (1, 3, 5)]
        pb = [int(b[i:i + 2], 16) for i in (1, 3, 5)]
        return "#" + "".join(f"{round(x + (y - x) * t):02x}" for x, y in zip(pa, pb))

    def quit(self) -> None:
        self.saved()
        self.root.destroy()


def cli(argv: list[str]) -> int:
    """safe-connect.exe --check|--block|--unblock HOST [PORT] (no window)."""
    action, host = argv[0].lstrip("-"), argv[1]
    port = int(argv[2]) if len(argv) > 2 else 2121
    remote = connect(host, port)
    try:
        if action == "check":
            states = update_blocker_state(remote)
        else:
            states = {"block": block_updates, "unblock": unblock_updates}[action](remote, print)
    finally:
        remote.close()
    for path, state in states:
        print(f"{path}: {state}")
    print("BLOCKED" if is_blocked(states) else "NOT BLOCKED")
    return 0 if is_blocked(states) else 1


def main() -> None:
    if len(sys.argv) >= 3 and sys.argv[1] in ("--check", "--block", "--unblock"):
        sys.exit(cli(sys.argv[1:]))
    try:
        from ctypes import windll
        windll.shcore.SetProcessDpiAwareness(1)
    except Exception:  # noqa: BLE001
        pass
    root = tk.Tk()
    App(root)
    root.mainloop()


if __name__ == "__main__":
    main()
