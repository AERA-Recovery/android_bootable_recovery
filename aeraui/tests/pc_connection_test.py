#!/usr/bin/env python3
"""Exercise the real HTTPS service against a fake installer, never a device."""
import hashlib
import http.client
import json
import pathlib
import socket
import ssl
import subprocess
import sys
import tempfile
import time
import unittest


class PcConnectionTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix="pc-storage-", dir=sys.argv[2])
        cls.root = pathlib.Path(cls.temporary.name)
        with socket.socket() as sock:
            sock.bind(("127.0.0.1", 0))
            cls.port = sock.getsockname()[1]
        cls.process = subprocess.Popen([sys.argv[1], str(cls.root), str(cls.port)],
                                       stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        assert cls.process.stdout.readline().strip() == "READY"
        cls.command("forget")
        cls.context = ssl._create_unverified_context()  # Fixture certificate only.
        cls.key = "a" * 64
        cls.token = ""

    @classmethod
    def tearDownClass(cls):
        if cls.process.poll() is None:
            cls.command("quit")
            cls.process.wait(timeout=10)
        cls.process.stdin.close()
        cls.process.stdout.close()
        cls.temporary.cleanup()

    @classmethod
    def command(cls, text):
        cls.process.stdin.write(text + "\n")
        cls.process.stdin.flush()
        if text == "quit":
            return
        while True:
            line = cls.process.stdout.readline().strip()
            if line == "OK " + text:
                return
            if not line:
                raise AssertionError("Host fixture unexpectedly stopped")

    @classmethod
    def request(cls, path, body=None, method=None, token=None, raw=False):
        raw_output = raw
        headers = {}
        active_token = cls.token if token is None else token
        if active_token:
            headers["Authorization"] = "Bearer " + active_token
        if isinstance(body, dict):
            body = json.dumps(body).encode()
            headers["Content-Type"] = "application/json"
        connection = http.client.HTTPSConnection("127.0.0.1", cls.port, context=cls.context, timeout=8)
        connection.request(method or ("POST" if body is not None else "GET"), path, body, headers)
        response = connection.getresponse()
        raw = response.read()
        status = response.status
        connection.close()
        return status, raw if raw_output else json.loads(raw) if raw else {}

    def wait_phase(self, phase):
        until = time.monotonic() + 8
        while time.monotonic() < until:
            status, result = self.request("/api/status")
            self.assertEqual(status, 200)
            if result["job"]["phase"] == phase:
                return result["job"]
            time.sleep(.05)
        self.fail(f"Did not reach {phase}: {result}")

    def prepare(self, data, **extra):
        body = dict(name="test.zip", bytes=len(data), kind="zip", storage=str(self.root),
                    sha256=hashlib.sha256(data).hexdigest())
        body.update(extra)
        return self.request("/api/prepare", body)

    def test_01_approval_is_required(self):
        self.assertEqual(self.request("/api/status", token="")[0], 401)
        self.assertEqual(self.request("/api/install", {"id": "x", "confirm": True}, token="")[0], 401)
        status, connection = self.request("/api/connect", {"key": self.key, "name": "Test computer"}, token="")
        self.assertEqual((status, connection["state"]), (202, "pending"))
        self.command("remember")
        status, approved = self.request("/api/connect/status", {"key": self.key, "request": connection["request"]}, token="")
        self.assertEqual((status, approved["state"]), (200, "allowed"))
        type(self).token = approved["token"]
        self.assertTrue(self.request("/api/status")[1]["remembered"])

    def test_02_partial_or_bad_upload_cannot_install(self):
        data = b"fixture-data" * 8192
        status, job = self.prepare(data, sha256="0" * 64)
        self.assertEqual(status, 200)
        self.assertEqual(self.request("/api/install", {"id": job["id"], "confirm": True})[0], 409)
        self.assertEqual(self.request("/api/upload/" + job["id"], data, "PUT")[0], 409)
        self.wait_phase("failed")
        self.assertFalse(list(self.root.rglob("payload*")))
        status, job = self.prepare(data)
        self.assertEqual(status, 200)
        connection = http.client.HTTPSConnection("127.0.0.1", self.port, context=self.context, timeout=8)
        connection.connect()
        connection.putrequest("PUT", "/api/upload/" + job["id"])
        connection.putheader("Authorization", "Bearer " + self.token)
        connection.putheader("Content-Length", str(len(data)))
        connection.endheaders()
        connection.send(data[:4096])
        connection.close()
        self.wait_phase("failed")
        self.assertEqual(self.request("/api/install", {"id": job["id"], "confirm": True})[0], 409)
        self.assertFalse(list(self.root.rglob("payload*")))

    def test_03_verified_install_survives_disconnect(self):
        data = b"simulated ZIP, not a flashable package" * 32768
        status, job = self.prepare(data)
        self.assertEqual(status, 200)
        status, verified = self.request("/api/upload/" + job["id"], data, "PUT")
        self.assertEqual((status, verified["phase"]), (200, "ready"))
        self.assertEqual(next(self.root.rglob("payload.zip")).read_bytes(), data)
        self.assertEqual(self.prepare(data)[0], 409)
        self.assertEqual(self.request("/api/install", {"id": job["id"], "confirm": False})[0], 409)
        self.assertEqual(self.request("/api/install", {"id": job["id"], "confirm": True})[0], 200)
        self.command("run")
        self.wait_phase("installing")
        self.assertEqual(self.request("/api/cancel", {"id": job["id"]})[0], 409)
        self.command("presentation")
        presentation = self.request("/api/status")[1]["job"]["presentation"]
        self.assertEqual(presentation["package"], "Simulated payload ZIP")
        self.assertEqual((presentation["stage"], presentation["stage_count"]), (2, 5))
        self.command("prompt")
        self.assertEqual(self.request("/api/prompt", {"id": job["id"], "prompt": "wrong", "accepted": True})[0], 409)
        self.assertEqual(self.request("/api/prompt", {"id": job["id"], "prompt": "test-question", "accepted": True})[0], 200)
        # Stop the complete network service, not just a browser tab.
        self.command("off")
        time.sleep(3.1)
        self.assertFalse(list(self.root.rglob("payload*")))
        self.command("on")
        _, approved = self.request("/api/connect", {"key": self.key, "name": "Test computer"}, token="")
        self.assertEqual(approved["state"], "allowed")
        type(self).token = approved["token"]
        self.wait_phase("completed")
        self.assertFalse(list(self.root.rglob("payload*")))
        self.command("hold")

    def test_04_image_targets_and_storage_are_validated(self):
        data = b"image fixture" * 8192
        self.assertEqual(self.prepare(data, storage="/tmp")[0], 409)
        self.assertEqual(self.prepare(data, kind="img", partition="/bad", both_slots=False)[0], 400)
        self.assertEqual(self.prepare(data, kind="img", partition="/system", both_slots=True)[0], 400)
        status, job = self.prepare(data, name="boot.img", kind="img", partition="/boot", both_slots=True)
        self.assertEqual(status, 200)
        self.assertEqual(job["partition"], "/boot")
        self.assertTrue(job["both_slots"])
        self.assertEqual(self.request("/api/upload/" + job["id"], data, "PUT")[0], 200)
        self.assertEqual(self.request("/api/cancel", {"id": job["id"]})[0], 200)
        self.assertFalse(list(self.root.rglob("payload*")))

    def test_05_other_computer_cannot_install_or_discard(self):
        data = b"owned fixture"
        status, job = self.prepare(data)
        self.assertEqual(status, 200)
        self.assertEqual(self.request("/api/upload/" + job["id"], data, "PUT")[0], 200)
        _, pending = self.request("/api/connect", {"key": "b" * 64, "name": "Second computer"})
        self.command("approve")
        _, approved = self.request("/api/connect/status", {"key": "b" * 64, "request": pending["request"]})
        second = approved["token"]
        self.assertFalse(self.request("/api/status", token=second)[1]["remembered"])
        for _ in range(12):
            status, resumed = self.request("/api/connect", {"key": "b" * 64, "name": "Second computer"}, token="")
            self.assertEqual((status, resumed["state"], resumed["token"]), (200, "allowed", second))
        self.assertEqual(self.request("/api/status")[0], 200)
        self.assertFalse(self.request("/api/status", token=second)[1]["job"]["owned"])
        self.assertEqual(self.request("/api/install", {"id": job["id"], "confirm": True}, token=second)[0], 403)
        self.assertEqual(self.request("/api/cancel", {"id": job["id"]}, token=second)[0], 403)
        self.assertEqual(self.request("/api/cancel", {"id": job["id"]})[0], 200)
        self.assertEqual(self.request("/api/forget", {}, token=second)[0], 200)
        self.assertEqual(self.request("/api/status", token=second)[0], 401)
        _, pending = self.request("/api/connect", {"key": "b" * 64, "name": "Second computer"}, token="")
        self.assertEqual(pending["state"], "pending")
        self.command("deny")

    def test_05a_file_browser_and_verified_file_save(self):
        folder = self.root / "browser-files"
        status, _ = self.request("/api/files/mkdir", {"path": str(self.root), "name": folder.name, "confirm": True})
        self.assertEqual(status, 200)
        self.assertEqual(self.request("/api/files/list", {"path": "/proc"})[0], 409)
        self.assertEqual(self.request("/api/files/mkdir", {"path": str(folder), "name": "../escape", "confirm": True})[0], 409)
        data = b"plain file data, not an installer" * 4096
        status, job = self.prepare(data, kind="file", name="notes.txt", folder=str(folder))
        self.assertEqual(status, 200)
        self.assertEqual(self.request("/api/upload/" + job["id"], data, "PUT")[0], 200)
        self.assertFalse((folder / "notes.txt").exists())
        self.assertEqual(self.request("/api/install", {"id": job["id"], "confirm": True})[1]["phase"], "completed")
        self.assertEqual((folder / "notes.txt").read_bytes(), data)
        status, empty = self.prepare(b"", kind="file", name="empty.txt", folder=str(folder))
        self.assertEqual(status, 200)
        self.assertEqual(self.request("/api/upload/" + empty["id"], b"", "PUT")[0], 200)
        self.assertEqual(self.request("/api/install", {"id": empty["id"], "confirm": True})[1]["phase"], "completed")
        self.assertEqual((folder / "empty.txt").read_bytes(), b"")
        self.assertEqual(self.request("/api/files/delete", {"path": str(folder / "empty.txt"), "confirm": True})[0], 200)
        self.assertEqual(self.prepare(data, kind="file", name="notes.txt", folder=str(folder))[0], 409)
        status, listing = self.request("/api/files/list", {"path": str(folder)})
        self.assertEqual((status, listing["entries"][0]["name"]), (200, "notes.txt"))
        _, ticket = self.request("/api/files/download", {"path": str(folder / "notes.txt")})
        self.assertEqual(self.request(ticket["url"], token="", raw=True), (200, data))
        self.assertEqual(self.request(ticket["url"], token="")[0], 403)
        self.assertEqual(self.request("/api/files/delete", {"path": str(folder), "confirm": True})[0], 409)
        self.command("busy")
        self.assertEqual(self.request("/api/files/delete", {"path": str(folder / "notes.txt"), "confirm": True})[0], 409)
        self.assertEqual(self.request("/api/reboot", {"target": "system", "confirm": True})[0], 409)
        self.command("idle")
        self.assertEqual(self.request("/api/files/rename", {"path": str(folder / "notes.txt"), "name": "renamed.txt", "confirm": True})[0], 200)
        self.assertEqual(self.request("/api/files/delete", {"path": str(folder / "renamed.txt"), "confirm": True})[0], 200)
        self.assertEqual(self.request("/api/files/delete", {"path": str(folder), "confirm": True})[0], 200)

    def test_05b_certificate_is_public_stable_and_valid_for_ip(self):
        for path in ("/api/log/logcat", "/api/log/kernel"):
            self.assertEqual(self.request(path, token="")[0], 401)
            status, output = self.request(path, raw=True)
            self.assertEqual(status, 200)
            self.assertIn(b"Simulated diagnostic snapshot", output)
        _, hello = self.request("/api/hello", token="")
        status, certificate = self.request("/api/certificate", raw=True)
        self.assertEqual(status, 200)
        self.assertIn(b"BEGIN CERTIFICATE", certificate)
        self.assertNotIn(b"PRIVATE KEY", certificate)
        der = ssl.PEM_cert_to_DER_cert(certificate.decode())
        self.assertEqual(hashlib.sha256(der).hexdigest(), hello["certificate"])
        trusted = ssl.create_default_context(cadata=certificate.decode())
        connection = http.client.HTTPSConnection("127.0.0.1", self.port, context=trusted, timeout=8)
        connection.request("GET", "/api/hello")
        self.assertEqual(connection.getresponse().status, 200)
        connection.close()
        self.command("restart")
        self.assertEqual(self.request("/api/hello", token="")[1]["certificate"], hello["certificate"])
        _, approved = self.request("/api/connect", {"key": self.key, "name": "Test computer"}, token="")
        type(self).token = approved["token"]

    def test_05c_root_and_reboot_are_confirmed_queued_jobs(self):
        self.command("run")
        self.assertEqual(self.request("/api/root", {"action": "patch", "provider": "next", "slot": "b"})[0], 400)
        self.assertEqual(self.request("/api/root", {"action": "patch", "provider": "next", "slot": "bad", "confirm": True})[0], 400)
        status, job = self.request("/api/root", {"action": "inspect", "provider": "next", "slot": "b", "confirm": True})
        self.assertEqual((status, job["root_action"], job["root_slot"]), (200, "inspect", "b"))
        self.wait_phase("completed")
        self.assertEqual(self.request("/api/reboot", {"target": "system"})[0], 400)
        self.assertEqual(self.request("/api/reboot", {"target": "bad", "confirm": True})[0], 400)
        self.assertEqual(self.request("/api/reboot", {"target": "recovery", "confirm": True})[0], 200)
        self.assertTrue(self.request("/api/status")[1]["busy"])
        time.sleep(1.1)
        self.command("reboot")
        self.assertFalse(self.request("/api/status")[1]["busy"])

    def test_05d_payload_review_and_arb_confirmation(self):
        self.command("hold")
        for mode in ("same", "upgrade", "downgrade", "malformed"):
            data = ("AERA-test-OTA\n" + mode + "\n").encode()
            status, job = self.prepare(data, name="Infinity-X-fixture.zip")
            self.assertEqual(status, 200)
            status, uploaded = self.request("/api/upload/" + job["id"], data, "PUT")
            self.assertEqual(status, 200)
            info = uploaded["package_info"]
            self.assertTrue(info["is_payload"])
            self.assertEqual(info["build"], "Infinity-X Test")
            self.assertEqual(info["android"], "Android 16")
            self.assertEqual(info["security_patch"], "2026-10-01")
            self.assertEqual([p["name"] for p in info["partitions"]], ["system", "vendor", "xbl_config"])
            body = {"id": job["id"], "confirm": True}
            if mode == "same":
                self.assertEqual(self.request("/api/install", body)[0], 200)
            else:
                self.assertEqual(self.request("/api/install", body)[0], 409)
                body["arb_acknowledged"] = True
                self.assertEqual(self.request("/api/install", body)[0], 200 if mode == "upgrade" else 409)
            self.assertEqual(self.request("/api/cancel", {"id": job["id"]})[0], 200)

    def test_05e_existing_packages_preserve_original_files(self):
        self.command("hold")
        package = self.root / "existing-ota.zip"
        original = b"AERA-test-OTA\nsame\n"
        package.write_bytes(original)
        self.assertEqual(self.request("/api/files/prepare", {"path": "/proc/version"})[0], 409)
        status, job = self.request("/api/files/prepare", {"path": str(package)})
        self.assertEqual((status, job["phase"], job["source_path"]), (200, "ready", str(package)))
        self.assertTrue(job["package_info"]["is_payload"])
        self.assertEqual(self.request("/api/upload/" + job["id"], original, "PUT")[0], 409)
        self.assertEqual(self.request("/api/cancel", {"id": job["id"]})[0], 200)
        self.assertEqual(package.read_bytes(), original)
        image = self.root / "modem.img"
        image.write_bytes(b"simulated firmware image")
        self.assertEqual(self.request("/api/files/prepare", {"path": str(image)})[0], 400)
        for partition, both_slots in (("/system", True), ("/block/splash", True)):
            self.assertEqual(self.request("/api/files/prepare", {"path": str(image), "partition": partition, "both_slots": both_slots})[0], 400)
        status, job = self.request("/api/files/prepare", {"path": str(image), "partition": "/block/modem", "both_slots": True})
        self.assertEqual((status, job["partition"], job["both_slots"]), (200, "/block/modem", True))
        self.assertEqual(self.request("/api/cancel", {"id": job["id"]})[0], 200)
        self.assertTrue(image.exists())
        _, job = self.request("/api/files/prepare", {"path": str(package)})
        self.assertEqual(self.request("/api/install", {"id": job["id"], "confirm": True})[0], 200)
        self.command("run")
        self.wait_phase("completed")
        self.assertEqual(package.read_bytes(), original)
        self.command("hold")
        _, job = self.request("/api/files/prepare", {"path": str(image), "partition": "/block/modem", "both_slots": False})
        image.write_bytes(b"changed!! firmware image")
        self.assertEqual(self.request("/api/install", {"id": job["id"], "confirm": True})[0], 200)
        self.command("run")
        self.wait_phase("failed")
        self.assertTrue(image.exists())
        self.assertTrue(package.exists())
        self.command("hold")

    def test_06_remember_restart_and_forget(self):
        self.command("restart")
        self.assertEqual(self.request("/api/status")[0], 401)
        _, approved = self.request("/api/connect", {"key": self.key, "name": "Test computer"}, token="")
        self.assertEqual(approved["state"], "allowed")
        type(self).token = approved["token"]
        self.assertEqual(self.request("/api/forget", {})[0], 200)
        self.assertEqual(self.request("/api/status")[0], 401)
        _, pending = self.request("/api/connect", {"key": self.key, "name": "Test computer"}, token="")
        self.assertEqual(pending["state"], "pending")
        self.command("deny")
        self.assertEqual(self.request("/api/connect/status", {"key": self.key, "request": pending["request"]}, token="")[1]["state"], "denied")


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]], verbosity=2)
