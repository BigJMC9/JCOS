#!/usr/bin/env python3
"""Exercise the Windows capture script over TCP, without physical serial hardware."""
import argparse
import json
from pathlib import Path
import socket
import subprocess
import tempfile
import threading
import time

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--powershell", default="powershell.exe")
    parser.add_argument("--build-dir", type=Path, default=Path("build/host-tests"))
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    output = args.build_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    cases = Path(tempfile.mkdtemp(prefix="serial-capture-", dir=output))
    assert cases.resolve().is_relative_to(output)
    base = [args.powershell, "-NoProfile", "-File", str(root / "tools/capture_serial.ps1")]
    def invoke(path, port, extra=()):
        result = subprocess.run(base + ["-TcpHost", "127.0.0.1", "-TcpPort", str(port),
            "-OutputPath", str(path), "-NoInput", "-Quiet", *extra],
            capture_output=True, text=True, timeout=15)
        if result.returncode:
            print(result.stdout + result.stderr)
        return result
    failures = []
    def listen(worker):
        server = socket.socket()
        server.bind(("127.0.0.1", 0))
        server.listen(1)
        server.settimeout(12)
        port = server.getsockname()[1]
        def run():
            try:
                with server:
                    client, _ = server.accept()
                    with client:
                        client.settimeout(5)
                        worker(client)
            except Exception as exc:
                failures.append(exc)
        thread = threading.Thread(target=run)
        thread.start()
        return port, thread
    payload = b"\x1b[2JBOOT\r\n\x00\xffBODY\r\nJA:/> "
    def worker(client):
        received = b""
        while not received.endswith(b"\n"):
            received += client.recv(100)
        assert received == b"ping\n", received
        for offset in range(0, len(payload), 3):
            client.sendall(payload[offset:offset + 3])
            time.sleep(0.01)
    port, thread = listen(worker)
    path = cases / "exact.log"
    result = invoke(path, port, ["-SendLine", "ping"])
    thread.join(10)
    assert result.returncode == 0 and not failures, (result.stdout, result.stderr, failures)
    assert path.read_bytes() == payload
    meta = json.loads(Path(str(path) + ".json").read_text(encoding="utf-8"))
    assert meta["received_bytes"] == len(payload) and meta["sent_bytes"] == 5
    assert meta["stop_reason"] == "remote connection closed"

    port, thread = listen(lambda client: time.sleep(2))
    path = cases / "timeout.log"
    result = invoke(path, port, ["-DurationSeconds", "1"])
    thread.join(10)
    assert result.returncode == 0 and not failures
    meta = json.loads(Path(str(path) + ".json").read_text(encoding="utf-8"))
    assert not path.read_bytes() and meta["stop_reason"] == "duration elapsed"

    path = cases / "existing.log"
    path.write_bytes(b"KEEP RAW")
    Path(str(path) + ".json").write_bytes(b"KEEP META")
    result = invoke(path, 9)
    assert result.returncode != 0 and path.read_bytes() == b"KEEP RAW"
    assert Path(str(path) + ".json").read_bytes() == b"KEEP META"

    path = cases / "existing-meta.log"
    Path(str(path) + ".json").write_bytes(b"KEEP META")
    result = invoke(path, 9)
    assert result.returncode != 0 and Path(str(path) + ".json").read_bytes() == b"KEEP META"

    with socket.socket() as reserve:
        reserve.bind(("127.0.0.1", 0))
        closed_port = reserve.getsockname()[1]
    path = cases / "refused.log"
    result = invoke(path, closed_port)
    assert result.returncode != 0
    meta = json.loads(Path(str(path) + ".json").read_text(encoding="utf-8"))
    assert meta["stop_reason"] == "error or interruption" and meta["received_bytes"] == 0
    # Prove handles were closed on connection failure; retain all test artifacts.
    moved = cases / "refused-closed.log"
    path.rename(moved)
    moved.rename(path)
    print("SERIAL CAPTURE: PASS (5 TCP/file scenarios; byte preservation, LF send, timeout, no overwrite, error cleanup)")
    print("Artifacts: " + str(cases))

if __name__ == "__main__":
    main()
