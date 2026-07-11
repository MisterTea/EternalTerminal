"""TCP throttle proxy for E2E testing. Applies TCP backpressure to the server.

Reads from the server at a limited rate, causing the kernel TCP send buffer
to fill up. Client->server direction (keystrokes) forwarded at full speed.

Supports disconnect simulation:
  SIGUSR1 -> kill all connections, block new ones for 60s
  SIGUSR2 -> resume immediately

Usage: python3 throttle_proxy.py <listen_port> <target_port> [bytes_per_sec]
"""
import socket
import sys
import threading
import time
import signal
import select
from datetime import datetime

LISTEN_PORT, TARGET_PORT = int(sys.argv[1]), int(sys.argv[2])
RATE = int(sys.argv[3]) if len(sys.argv) > 3 else 100000

disconnect_until = 0
active_connections = []
lock = threading.Lock()


def log(msg):
    ts = datetime.now().strftime("%H:%M:%S.%f")
    print(f"[proxy {ts}] {msg}", flush=True)


def handle_disconnect(signum, frame):
    global disconnect_until
    disconnect_until = time.monotonic() + 60
    log("DISCONNECT: killing all connections for 60s")
    with lock:
        for cli, srv, stop in active_connections:
            stop.set()
            try:
                cli.close()
            except Exception:
                pass
            try:
                srv.close()
            except Exception:
                pass
        active_connections.clear()


def handle_reconnect(signum, frame):
    global disconnect_until
    disconnect_until = 0
    log("RECONNECT: accepting connections again")


signal.signal(signal.SIGUSR1, handle_disconnect)
signal.signal(signal.SIGUSR2, handle_reconnect)


def throttled_forward(name, src, dst, rate, stop):
    total = 0
    CHUNK = max(rate // 20, 64)
    last_t = time.time()
    try:
        while not stop.is_set():
            data = src.recv(CHUNK)
            if not data:
                break
            dst.sendall(data)
            total += len(data)
            time.sleep(len(data) / rate)
            now = time.time()
            if now - last_t > 2:
                log(f"{name}: sent={total:,}B")
                last_t = now
    except Exception:
        pass
    stop.set()


def fast_forward(name, src, dst, stop):
    try:
        while not stop.is_set():
            data = src.recv(4096)
            if not data:
                break
            dst.sendall(data)
    except Exception:
        pass
    stop.set()


def handle(cli, addr):
    log(f"connect {addr}")
    srv = None
    stop = threading.Event()
    try:
        srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        # Clamp the receive buffer BEFORE connect (window scaling is
        # negotiated at handshake). Without this, the proxy's kernel rcvbuf
        # autotunes to multiple MB and silently absorbs the server's output,
        # hiding buffer bloat that a real slow link would push back on.
        # 64KB ~= 0.64s of queue at the default 100KB/s rate.
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 64 * 1024)
        srv.connect(("127.0.0.1", TARGET_PORT))
        log(f"srv rcvbuf={srv.getsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF)}")
        with lock:
            active_connections.append((cli, srv, stop))
        t1 = threading.Thread(
            target=throttled_forward,
            args=("s->c", srv, cli, RATE, stop),
            daemon=True,
        )
        t2 = threading.Thread(
            target=fast_forward, args=("c->s", cli, srv, stop), daemon=True
        )
        t1.start()
        t2.start()
        t1.join()
        t2.join()
    except Exception as e:
        log(f"error: {e}")
    finally:
        for s in (cli, srv):
            try:
                s.close()
            except Exception:
                pass
        with lock:
            active_connections[:] = [
                (c, s, st) for c, s, st in active_connections if st is not stop
            ]
    log(f"disconnect {addr}")


# Dual-stack listener
s = socket.socket(socket.AF_INET6, socket.SOCK_STREAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_V6ONLY, 0)
s.bind(("::", LISTEN_PORT))
s.listen(5)
s.setblocking(False)
log(f"[::]:{LISTEN_PORT} -> 127.0.0.1:{TARGET_PORT} rate={RATE}B/s")

while True:
    try:
        r, _, _ = select.select([s], [], [], 1.0)
        for _ in r:
            c, a = s.accept()
            if time.monotonic() < disconnect_until:
                log(f"rejecting {a} (disconnected)")
                c.close()
                continue
            threading.Thread(target=handle, args=(c, a), daemon=True).start()
    except KeyboardInterrupt:
        break
    except Exception:
        pass
