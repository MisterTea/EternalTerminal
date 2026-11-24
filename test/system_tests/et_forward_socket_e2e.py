"""A forwarded Unix socket must see the live login terminal as its peer."""

import os
import select
import socket
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path


def wait_for(test, process, description):
    deadline = time.monotonic() + 10
    while not test():
        if process.poll() is not None or time.monotonic() >= deadline:
            raise RuntimeError(description)
        time.sleep(0.02)


def check_busy_listener(args, host, env, work):
    dest = work / "busy"
    source = work / "source"
    with socket.socket(socket.AF_UNIX) as listener, socket.socket(socket.AF_UNIX) as filler:
        listener.bind(str(dest))
        listener.listen(0)
        listener.settimeout(6)
        filler.connect(str(dest))
        client = subprocess.Popen(
            [*args, "--no-pty", "--command=cat", f"--tunnel={source}:{dest}", host],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, env=env,
        )
        try:
            wait_for(source.exists, client, "local tunnel did not open")
            with socket.socket(socket.AF_UNIX) as tunnel:
                tunnel.connect(str(source))
                tunnel.sendall(b"forward")
                # Send terminal input only after etserver has received the forward.
                wait_for(lambda: any(str(dest) in p.read_text() for p in work.glob("etserver-*.log")),
                         client, "etserver did not receive the forward")
                client.stdin.write(b"terminal\n")
                client.stdin.flush()
                assert select.select([client.stdout], [], [], 10)[0], "terminal stalled"
                assert client.stdout.readline() == b"terminal\n"
                listener.accept()[0].close()  # Make room for the pending forward.
                conn, _ = listener.accept()
                with conn:
                    conn.settimeout(6)
                    assert conn.recv(7, socket.MSG_WAITALL) == b"forward"
        finally:
            client.kill()
            client.wait()


def run(et, server_bin, terminal):
    with tempfile.TemporaryDirectory(prefix="et_forward_") as tmp:
        work = Path(tmp)
        router = work / "router"
        dest = work / "dest"
        ssh = work / "ssh"
        ssh.write_text('#!/bin/sh\nfor last; do :; done\nexec /bin/sh -c "$last"\n')
        ssh.chmod(0o700)
        env = dict(os.environ, PATH=f"{work}:{os.environ['PATH']}", ET_NO_TELEMETRY="YES", SHELL="/bin/sh")
        with socket.socket() as available:
            available.bind(("127.0.0.1", 0))
            port = available.getsockname()[1]
        common = [f"--serverfifo={router}", f"--logdir={work}"]
        args = [et, *common, "--no-ssh-config", f"--terminal-path={terminal}"]
        host = f"127.0.0.1:{port}"
        server = subprocess.Popen(
            [server_bin, *common, "--bindip=127.0.0.1", f"--port={port}"], env=env
        )
        try:
            wait_for(router.exists, server, "etserver did not create the router")
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as listener:
                listener.bind(str(dest))
                listener.listen()
                listener.settimeout(15)
                client = subprocess.Popen(
                    [*args, "-W", str(dest), host],
                    stdin=subprocess.PIPE,
                    stdout=subprocess.PIPE,
                    env=env,
                )
                try:
                    client.stdin.write(b"ping")
                    client.stdin.flush()
                    conn, _ = listener.accept()
                    with conn:
                        conn.settimeout(10)
                        cred = struct.Struct("3i")
                        pid, uid, _ = cred.unpack(
                            conn.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, cred.size)
                        )
                        peer = Path(f"/proc/{pid}")
                        assert uid == os.getuid() and (peer / "exe").samefile(terminal)
                        loginuid = Path("/proc/self/loginuid")
                        if loginuid.exists():
                            assert (peer / "loginuid").read_bytes() == loginuid.read_bytes()
                        assert conn.recv(4, socket.MSG_WAITALL) == b"ping"
                        conn.sendall(b"pong")
                    output, _ = client.communicate(timeout=10)
                    assert client.returncode == 0 and output == b"pong", output
                finally:
                    client.kill()
                    client.wait()
            check_busy_listener(args, host, env, work)
        finally:
            server.terminate()
            try:
                server.wait(timeout=5)
            except subprocess.TimeoutExpired:
                server.kill()
                server.wait()


if __name__ == "__main__":
    run(*(Path(arg).resolve() for arg in sys.argv[1:]))
