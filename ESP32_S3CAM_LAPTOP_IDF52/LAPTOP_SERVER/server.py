
"""ESP32 Face AI HTTP server: website enrollment authorized by ESP32 MAIN.

Compatible with the original FaceEngine, gallery and FCR1 protocol.
Uses Python 3.11; runs on the laptop LAN address bound in config.json.
"""
from __future__ import annotations

import json
import re
import secrets
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import urlsplit

from face_engine import FaceEngine
from protocol import response, STATUS_ERROR

ROOT = Path(__file__).resolve().parent
ID_RE = re.compile(r"[1-9][0-9]{0,4}")
TOKEN_RE = re.compile(r"[0-9a-f]{8}")
PENDING_TTL = 90.0


def cfg_load():
    cfg = json.loads((ROOT / "config.json").read_text(encoding="utf-8"))

    for key in ("device_key", "admin_key"):
        if len(str(cfg.get(key, ""))) < 12:
            raise RuntimeError(
                f"config.json needs {key} (>=12 chars)"
            )

    if cfg["admin_key"] == cfg["device_key"]:
        raise RuntimeError(
            "admin_key and device_key must differ"
        )

    cfg.setdefault("bind", "0.0.0.0")
    cfg.setdefault("port", 5000)

    return cfg


def employee_id(value):
    s = str(value).strip()

    if (
        not s.isascii()
        or not s.isdecimal()
        or not 1 <= int(s) <= 65535
    ):
        raise ValueError(
            "Employee ID must be 1..65535"
        )

    return str(int(s))


class FaceHTTP(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True

    def __init__(self, address, config, engine):
        super().__init__(address, FaceRequest)

        self.config = config
        self.engine = engine
        self.lock = threading.RLock()

        self.pending = None
        self.pending_error = ""
        self.completed = None

    def expired(self):
        if (
            self.pending
            and time.monotonic() -
                self.pending["created"] > PENDING_TTL
        ):
            self.pending_error = (
                "ESP32 MAIN không xác minh ID kịp thời."
            )

            self.pending = None


class FaceRequest(BaseHTTPRequestHandler):
    server: FaceHTTP
    server_version = "ESP32FaceAI/2"

    def log_message(self, fmt, *args):
        print(
            "[WEB]",
            self.address_string(),
            fmt % args,
            flush=True
        )

    def send_data(
        self,
        code,
        data: bytes,
        mime="application/json; charset=utf-8"
    ):
        self.send_response(code)

        self.send_header(
            "Content-Type",
            mime
        )

        self.send_header(
            "Content-Length",
            str(len(data))
        )

        self.send_header(
            "Cache-Control",
            "no-store"
        )

        self.send_header(
            "X-Content-Type-Options",
            "nosniff"
        )

        self.send_header(
            "X-Frame-Options",
            "DENY"
        )

        self.end_headers()

        try:
            self.wfile.write(data)

        except (
            BrokenPipeError,
            ConnectionResetError
        ):
            pass

    def json(self, code, obj):
        self.send_data(
            code,
            json.dumps(
                obj,
                ensure_ascii=False
            ).encode("utf-8")
        )

    def text(self, code, msg):
        self.send_data(
            code,
            msg.encode("ascii"),
            "text/plain; charset=ascii"
        )

    def authenticated(self, role):
        header = (
            "X-Admin-Key"
            if role == "admin"
            else "X-Device-Key"
        )

        key = self.server.config[
            "admin_key"
            if role == "admin"
            else "device_key"
        ]

        if not secrets.compare_digest(
            self.headers.get(header, ""),
            key
        ):
            self.json(
                403,
                {
                    "error":
                        "Invalid " + role + " credential"
                }
            )

            return False

        return True

    def request_body(self, limit):
        try:
            n = int(
                self.headers.get(
                    "Content-Length",
                    "0"
                )
            )

        except ValueError as exc:
            raise ValueError(
                "Invalid Content-Length"
            ) from exc

        if not 0 < n <= limit:
            raise ValueError(
                "Body missing or too large"
            )

        body = self.rfile.read(n)

        if len(body) != n:
            raise ValueError(
                "Incomplete request"
            )

        return body

    def json_body(self, limit=2048):
        return json.loads(
            self.request_body(limit).decode("utf-8")
        )

    def do_GET(self):
        path = urlsplit(self.path).path

        # ----------------------------------------------------
        # Website home page
        # ----------------------------------------------------

        if path == "/":
            page = ROOT / "templates" / "index.html"

            if not page.is_file():
                self.json(
                    500,
                    {
                        "error":
                            "Missing templates/index.html"
                    }
                )

            else:
                self.send_data(
                    200,
                    page.read_bytes(),
                    "text/html; charset=utf-8"
                )

            return

        # ----------------------------------------------------
        # Health check
        # ----------------------------------------------------

        if path == "/api/health":
            self.json(
                200,
                {"ready": True}
            )

            return

        # ----------------------------------------------------
        # Website pending employee registration
        #
        # S3 polls this endpoint and asks MAIN whether the
        # employee already has RFID and fingerprint data.
        # ----------------------------------------------------

        if path == "/api/device/enroll/pending":
            if not self.authenticated("device"):
                return

            with self.server.lock:
                self.server.expired()

                p = self.server.pending

                self.text(
                    200,
                    (
                        f"CHECK:{p['id']}:{p['token']}\n"
                        if p
                        else "NONE\n"
                    )
                )

            return

        # ----------------------------------------------------
        # Enrollment capture mode
        #
        # While capturing 30 images, S3 must continue
        # uploading camera frames even without radar presence.
        # ----------------------------------------------------

        if path == "/api/device/enroll/mode":
            if not self.authenticated("device"):
                return

            with self.server.engine.lock:
                active = bool(
                    self.server.engine.enrollment
                )

            self.text(
                200,
                (
                    "ACTIVE:1\n"
                    if active
                    else "ACTIVE:0\n"
                )
            )

            return

        # ----------------------------------------------------
        # Enrolled face roster
        #
        # Example:
        # IDS:101,102,103
        #
        # S3 transfers this to MAIN using UART STATUS frames.
        # ----------------------------------------------------

        if path == "/api/device/face/roster":
            if not self.authenticated("device"):
                return

            with self.server.engine.lock:
                ids = sorted({
                    int(k)
                    for k
                    in self.server.engine.gallery.people
                })

            if (
                len(ids) > 512
                or any(
                    not 0 < i <= 65535
                    for i in ids
                )
            ):
                self.json(
                    503,
                    {
                        "error":
                            "Face roster exceeds bounds"
                    }
                )

                return

            roster = (
                "IDS:" +
                ",".join(map(str, ids)) +
                "\n"
            )

            if len(roster) >= 4096:
                self.json(
                    503,
                    {
                        "error":
                            "Face roster too large"
                    }
                )

                return

            self.text(200, roster)
            return

        # ----------------------------------------------------
        # Admin-only APIs
        # ----------------------------------------------------

        if path not in (
            "/api/preview",
            "/api/status",
            "/api/people"
        ):
            self.json(
                404,
                {"error": "Not found"}
            )

            return

        if not self.authenticated("admin"):
            return

        engine = self.server.engine

        with self.server.lock:
            self.server.expired()

            pending = self.server.pending

            pending_status = (
                {
                    "id": pending["id"],
                    "state": "WAITING_MAIN"
                }
                if pending
                else None
            )

            pending_error = (
                self.server.pending_error
            )

        with engine.lock:

            # Live camera preview

            if path == "/api/preview":
                preview = engine.last_preview

                if preview:
                    self.send_data(
                        200,
                        preview,
                        "image/jpeg"
                    )

                else:
                    self.json(
                        404,
                        {
                            "error":
                                "No camera frame"
                        }
                    )

            # System status and registration progress

            elif path == "/api/status":
                self.json(
                    200,
                    {
                        "status":
                            engine.last_status,

                        "score":
                            round(
                                float(engine.last_score),
                                3
                            ),

                        "face_count":
                            int(engine.last_face_count),

                        "enrollment":
                            engine.enroll_status(),

                        "pending":
                            pending_status,

                        "pending_error":
                            pending_error,

                        "people_count":
                            len(
                                engine.gallery.people
                            ),

                        "camera_frame_received":
                            bool(engine.last_preview)
                    }
                )

            # Registered employees

            else:
                result = []

                for pid, name in (
                    engine.gallery.people.items()
                ):
                    result.append(
                        {
                            "id": str(pid),
                            "name": name,
                            "images": int(
                                sum(
                                    engine.gallery.ids == pid
                                )
                            )
                        }
                    )

                self.json(
                    200,
                    {"people": result}
                )

    def do_POST(self):
        path = urlsplit(self.path).path

        try:

            # ------------------------------------------------
            # Camera JPEG upload
            #
            # S3 -> Python AI -> FCR1 binary response
            # ------------------------------------------------

            if path == "/api/frame":
                if not self.authenticated("device"):
                    return

                content_type = (
                    self.headers.get(
                        "Content-Type",
                        ""
                    ).split(";")[0].strip()
                )

                if content_type != "image/jpeg":
                    self.send_data(
                        415,
                        b"",
                        "application/octet-stream"
                    )

                    return

                img = self.request_body(180000)

                try:
                    out = (
                        self.server.engine.process(img)
                    )

                except Exception as exc:
                    print(
                        "[AI] process failed:",
                        repr(exc),
                        flush=True
                    )

                    out = response(
                        STATUS_ERROR
                    )

                self.send_data(
                    200,
                    out,
                    "application/octet-stream"
                )

                return

            # ------------------------------------------------
            # S3 submits MAIN's employee verification result
            # ------------------------------------------------

            if path == "/api/device/enroll/decision":
                if not self.authenticated("device"):
                    return

                p = self.json_body(512)

                eid = employee_id(
                    p.get("id")
                )

                token = str(
                    p.get("token", "")
                )

                exists = p.get("exists")

                if (
                    type(exists) is not bool
                    or not TOKEN_RE.fullmatch(token)
                ):
                    raise ValueError(
                        "Invalid decision"
                    )

                with self.server.lock:
                    self.server.expired()

                    pending = self.server.pending

                    if (
                        not pending
                        or pending["token"] != token
                        or pending["id"] != eid
                    ):
                        self.json(
                            200,
                            {
                                "ok": False,
                                "state": "IGNORED"
                            }
                        )

                        return

                    self.server.pending = None

                    if not exists:
                        self.server.pending_error = (
                            "ID chưa có đủ RFID và vân tay "
                            "trên ESP32 MAIN."
                        )

                        self.json(
                            200,
                            {
                                "ok": False,
                                "state": "DENIED"
                            }
                        )

                        return

                    try:
                        with self.server.engine.lock:

                            if (
                                self.server.engine.enrollment
                            ):
                                raise ValueError(
                                    "Another enrollment is active"
                                )

                            if any(
                                int(k) == int(eid)
                                for k
                                in self.server.engine.gallery.people
                            ):
                                raise ValueError(
                                    "This employee already has a face"
                                )

                            self.server.engine.start_enroll(
                                eid,
                                pending["name"]
                            )

                    except ValueError as exc:
                        self.server.pending_error = str(exc)

                        self.json(
                            409,
                            {"error": str(exc)}
                        )

                        return

                    self.server.pending_error = ""

                self.json(
                    200,
                    {
                        "ok": True,
                        "state": "CAPTURING"
                    }
                )

                return

            # ------------------------------------------------
            # MAIN requests face deletion through S3
            # ------------------------------------------------

            if path == "/api/device/face/delete":
                if not self.authenticated("device"):
                    return

                p = self.json_body(512)

                eid = employee_id(
                    p.get("id")
                )

                with (
                    self.server.lock,
                    self.server.engine.lock
                ):
                    if (
                        self.server.engine.enrollment
                        and
                        self.server.engine.enrollment["id"]
                        == eid
                    ):
                        self.server.engine.cancel_enroll()

                    self.server.engine.gallery.delete(eid)

                self.text(
                    200,
                    f"DELETED:{eid}"
                )

                return

            # ------------------------------------------------
            # Admin-only commands
            # ------------------------------------------------

            if path not in (
                "/api/enroll/start",
                "/api/enroll/cancel",
                "/api/people/delete"
            ):
                self.json(
                    404,
                    {"error": "Not found"}
                )

                return

            if not self.authenticated("admin"):
                return

            p = self.json_body(2048)

            # ------------------------------------------------
            # Begin face enrollment through website
            #
            # Important:
            # This does NOT immediately start taking images.
            # MAIN must verify the employee first.
            # ------------------------------------------------

            if path == "/api/enroll/start":
                eid = employee_id(
                    p.get("id")
                )

                name = str(
                    p.get("name", "")
                ).strip()

                if not 1 <= len(name) <= 64:
                    raise ValueError(
                        "Employee name must be 1..64 characters"
                    )

                with self.server.lock:
                    self.server.expired()

                    if self.server.pending:
                        raise ValueError(
                            "Another employee is waiting for MAIN"
                        )

                    with self.server.engine.lock:

                        if self.server.engine.enrollment:
                            raise ValueError(
                                "Another enrollment is active"
                            )

                        if any(
                            int(k) == int(eid)
                            for k
                            in self.server.engine.gallery.people
                        ):
                            raise ValueError(
                                "ID already has a face. Delete first."
                            )

                    self.server.pending = {
                        "id": eid,
                        "name": name,
                        "token": secrets.token_hex(4),
                        "created": time.monotonic()
                    }

                    self.server.pending_error = ""

                self.json(
                    200,
                    {
                        "ok": True,
                        "pending": True
                    }
                )

                return

            # ------------------------------------------------
            # Cancel pending or active enrollment
            # ------------------------------------------------

            if path == "/api/enroll/cancel":
                with self.server.lock:
                    self.server.pending = None
                    self.server.pending_error = ""

                self.server.engine.cancel_enroll()

                self.json(
                    200,
                    {"ok": True}
                )

                return

            # ------------------------------------------------
            # Delete one registered face through website
            #
            # RFID and fingerprint are not deleted.
            # Updated roster automatically removes FACE
            # permission from MAIN after synchronization.
            # ------------------------------------------------

            if path == "/api/people/delete":
                eid = employee_id(
                    p.get("id")
                )

                with (
                    self.server.lock,
                    self.server.engine.lock
                ):
                    if self.server.engine.enrollment:
                        raise ValueError(
                            "Cancel active enrollment before delete"
                        )

                    removed = (
                        self.server.engine.gallery.delete(eid)
                    )

                self.json(
                    200,
                    {"ok": bool(removed)}
                )

                return

        except (
            ValueError,
            TypeError,
            KeyError,
            json.JSONDecodeError
        ) as exc:
            self.json(
                400,
                {"error": str(exc)}
            )

        except Exception as exc:
            print(
                "[HTTP] unexpected error:",
                repr(exc),
                flush=True
            )

            self.json(
                500,
                {
                    "error":
                        "Internal server error"
                }
            )


def main():
    config = cfg_load()

    engine = FaceEngine(config)

    server = FaceHTTP(
        (
            str(config["bind"]),
            int(config["port"])
        ),
        config,
        engine
    )

    print(
        f"AI server listening on "
        f"{config['bind']}:{config['port']}",
        flush=True
    )

    try:
        server.serve_forever(
            poll_interval=0.3
        )

    except KeyboardInterrupt:
        print(
            "Stopping server",
            flush=True
        )

    finally:
        server.server_close()


if __name__ == "__main__":
    main()
