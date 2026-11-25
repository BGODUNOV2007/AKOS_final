"""Проверки через настоящий IPv4/TCP. Зависимости — стандартная библиотека Python."""
import argparse
import contextlib
import re
import signal
import socket
import subprocess
import tempfile
import time
from pathlib import Path

from audit import audit, fib

ROOT = Path(__file__).resolve().parents[1]
BIN = ROOT / "bin"
IP = "127.0.0.1"
EVIDENCE = None
DEFAULT = dict(db="2,3,5", readers=2, writers=1, reader_ops=1, writer_ops=1,
               index="0-2", value="1-93", gap_ms="0-0", read_ms="0-0",
               write_ms="0-0", policy="fair", seed=42)


def wait_for(predicate):
    deadline = time.monotonic() + 4
    while not predicate():
        if time.monotonic() >= deadline:
            raise AssertionError("Превышено время ожидания события")
        time.sleep(.01)


class Wire:
    def __init__(self, port):
        self.sock = socket.create_connection((IP, port), timeout=4)
        self.file = self.sock.makefile("rb")

    def send(self, line):
        self.sock.sendall((line + "\n").encode())

    def read(self, prefix=""):
        line = self.file.readline().decode().rstrip()
        assert line and line.startswith(prefix), (prefix, line)
        return line

    def close(self):
        self.file.close()
        self.sock.close()


class Run:
    def __init__(self, folder, record_as=None, **changes):
        self.record_as = record_as
        folder = Path(folder)
        cfg = dict(DEFAULT, **changes)
        self.initial = sorted(map(int, str(cfg["db"]).split(",")))
        self.cfg = folder / "case.conf"
        self.cfg.write_text("".join(f"{k}={v}\n" for k, v in cfg.items()))
        with socket.socket() as probe:
            probe.bind((IP, 0))
            self.port = probe.getsockname()[1]
        self.log = folder / "events.log"
        self.children = []
        self.wires = []
        self.server = self.start("server", self.cfg, self.log)
        assert self.server.stdout.readline().startswith("LISTEN")

    def start(self, prog, *args):
        p = subprocess.Popen([BIN / prog, IP, str(self.port), *map(str, args)],
                             stdout=subprocess.PIPE, text=True)
        self.children.append(p)
        return p

    def wire(self, hello=None):
        w = Wire(self.port)
        self.wires.append(w)
        if hello:
            w.send(hello)
            w.read("READY")
        return w

    def ctl(self, *command):
        proc = self.start("control", *command)
        out, _ = proc.communicate(timeout=4)
        assert proc.returncode == 0, (command, out)
        return out

    def text(self):
        return self.log.read_text()

    def seen(self, event, actor, count=1):
        wait_for(lambda: self.text().count(f" {event} {actor} ") >= count)

    def end(self, reason):
        out, _ = self.server.communicate(timeout=5)
        assert self.server.returncode == 0 and f"reason={reason}" in out, out
        totals = audit(self.text().splitlines(), self.initial)
        if EVIDENCE and self.record_as:
            (EVIDENCE / (self.record_as + ".log")).write_text(self.text())
        return totals

    def close(self):
        for w in self.wires:
            w.close()
        for p in self.children:
            if p.poll() is None:
                p.kill()
            p.wait()
            p.stdout.close()


@contextlib.contextmanager
def run(**cfg):
    with tempfile.TemporaryDirectory() as d:
        r = Run(d, **cfg)
        try:
            yield r
        finally:
            r.close()


def complete(p, marker):
    out, _ = p.communicate(timeout=8)
    assert p.returncode == 0 and marker in out, (p.returncode, out)
    return out


def grant(w):
    _, ticket, value = w.read("GRANT").split()
    return ticket, int(value)


def end_op(w, role, granted):
    ticket, value = granted
    w.send(f"END {ticket}" + (f" {fib(value)}" if role == "R" else ""))
    return w.read("RESULT")


def clients_and_observers():
    with run(reader_ops="3,2", writer_ops=2, read_ms="30-30", write_ms="10-10") as r:
        mons = [r.start("monitor") for _ in range(2)]
        r.seen("ONLINE", "M2")
        ps = [r.start("reader", 1), r.start("reader", 2), r.start("writer", 1)]
        for p in ps:
            complete(p, "BYE quota")
        views = [complete(p, "STOP complete") for p in mons]
        events = [[s for s in view.splitlines() if " ticket=" in s and s.startswith("EVENT") and
                   " ONLINE " not in s] for view in views]
        assert events[0] == events[1]
        assert r.end("complete")[:2] == (5, 2)


def policy(kind):
    # Держим первого писателя в критической секции, ставим R1,W2,R2 в точном порядке.
    with run(writers=2, policy=kind, record_as="policy_" + kind) as r:
        w = r.wire("HELLO W 1")
        w.send("REQ 0 9")
        first = grant(w)
        actors = {}
        for name, req in [("R1","REQ 0"), ("W2","REQ 1 7"), ("R2","REQ 2")]:
            peer = r.wire(f"HELLO {name[0]} {name[1]}")
            actors[name] = peer
            peer.send(req)
            r.seen("REQUEST", name)
        end_op(w, "W", first)
        order = {"fair":["R1","W2","R2"], "readers":["R1","R2","W2"], "writers":["W2","R1","R2"]}[kind]
        if kind == "readers":
            r.seen("READ_BEGIN","R2")  # Оба допущены до окончания первого чтения.
        for name in order:
            end_op(actors[name], name[0], grant(actors[name]))
        totals = r.end("complete")
        begins = re.findall(r"(?:READ|WRITE)_BEGIN (\w+)",r.text())
        assert begins == ["W1", *order] and totals[:2] == (2,2)
        if kind == "readers":
            assert totals[3] == 2


def reconnect():
    with run(record_as="restart_after_kill_and_drop", readers=0, writer_ops=2, write_ms="450-450") as r:
        mon = r.start("monitor")
        r.seen("ONLINE","M1")
        first = r.start("writer",1)
        r.seen("WRITE_BEGIN","W1",2)
        first.kill()
        first.wait()
        r.seen("CANCEL","W1")
        assert "PROGRESS W1 done=1 quota=2" in r.ctl("list")
        assert "OK" in r.ctl("drop","M",1)
        complete(mon,"STOP dropped")
        back = r.start("monitor")
        r.seen("ONLINE","M2")
        second = r.start("writer",1)
        r.seen("WRITE_BEGIN","W1",3)
        r.ctl("drop","W",1)
        complete(second,"STOP dropped")
        restored = complete(r.start("writer",1),"BYE quota")
        assert "READY 1 2 " in restored
        view = complete(back,"STOP complete")
        assert "SNAP " in view and "PROGRESS W1 done=1" in view
        assert r.end("complete")[:3] == (0,2,2)


def network_and_invalid():
    with run(db="93", index="0-0", writers=0) as r:
        first = r.wire()
        first.sock.sendall(b"HEL")
        first.sock.sendall(b"LO R 1\nREQ 0\n")
        first.read("READY")
        ticket, value = grant(first)
        dup = r.wire()
        dup.send("HELLO R 1")
        dup.read("ERROR id-in-use")
        first.send(f"END {ticket} 0")
        first.read("ERROR invalid-finish")
        assert value == 93
        for payload in (b"HELLO R 3\n", b"x"*270+b"\n", b"HELLO\x00R 1\n"):
            bad = r.wire()
            bad.sock.sendall(payload)
            bad.read("ERROR")
        for i in (1,2):
            assert "fib=12200160415121876738" in complete(r.start("reader",i),"BYE quota")
        assert r.end("complete")[:3] == (2,0,1)


def stop_all(mode):
    with run(record_as=mode + "_cancels_all", reader_ops=0, writer_ops=0, read_ms="60000-60000") as r:
        mon = r.start("monitor")
        reader = r.start("reader",1)
        r.seen("READ_BEGIN","R1")
        writer = r.start("writer",1)
        r.seen("REQUEST","W1")
        again = r.start("reader",2)
        r.seen("REQUEST","R2")
        view = r.ctl("list")
        assert "R1 active" in view and "W1 queued" in view and "R2 queued" in view
        if mode == "signal":
            r.server.send_signal(signal.SIGINT)
        else:
            r.ctl("stop")
        for p in (reader,writer,again,mon):
            complete(p,"STOP " + mode)
        assert r.end(mode)[:3] == (0,0,3)


def reader_disconnect_and_resume():
    with run(read_ms="60000-60000", reader_ops=1, writer_ops=1) as r:
        busy = r.start("reader",1)
        r.seen("READ_BEGIN","R1")
        writer = r.start("writer",1)
        r.seen("REQUEST","W1")
        queued = r.start("reader",2)
        r.seen("REQUEST","R2")
        mon = r.start("monitor")
        r.seen("ONLINE","M1")
        r.ctl("drop","R",2)
        complete(queued,"STOP dropped")
        busy.kill()
        busy.wait()
        complete(writer,"BYE quota")
        # Сеансы тестового клиента вправе закончить работу раньше модельной задержки.
        for i in (1,2):
            w = r.wire(f"HELLO R {i}")
            w.send("REQ 0")
            end_op(w,"R",grant(w))
        view = complete(mon,"STOP complete")
        assert "PEER R1 active" in view and "PEER W1 queued" in view
        assert r.end("complete")[:3] == (2,1,2)


def committed_without_ack():
    with run(readers=1, writers=1, writer_ops=1) as r:
        w = r.wire("HELLO W 1")
        w.send("REQ 1 17")
        ticket, _ = grant(w)
        w.send(f"END {ticket}")
        r.seen("WRITE_END","W1")
        w.close()  # RESULT ещё не прочитан, но запись уже опубликована.
        resumed = r.wire()
        resumed.send("HELLO W 1")
        resumed.read("BYE quota")
        complete(r.start("reader",1),"BYE quota")
        assert r.end("complete")[:3] == (1,1,0)


def seed_and_boundaries():
    choices = []
    for _ in range(2):
        with run(readers=0, writer_ops=10, db="93,1,2", value="90-93") as r:
            output = complete(r.start("writer",1),"BYE quota")
            choices.append(re.findall(r"RESULT W1 op=\d+ index=(\d+) old=\d+ new=(\d+)",output))
            assert r.end("complete")[:2] == (0,10)
    assert len(choices[0]) == 10 and choices[0] == choices[1]


def invalid_configs():
    with tempfile.TemporaryDirectory() as d:
        path = Path(d) / "test.conf"
        for changes in ({"db":"0"}, {"db":"94"}, {"index":"0-3"}, {"policy":"typo"},
                        {"readers":0,"writers":0}, {"reader_ops":"1,2,3"}, {"gap_ms":"9-1"}):
            path.write_text("".join(f"{k}={v}\n" for k,v in dict(DEFAULT,**changes).items()))
            proc = subprocess.run([BIN/"server",IP,"5000",path],capture_output=True,timeout=2)
            assert proc.returncode == 2


def load_test():
    with run(readers=12, writers=6, reader_ops=8, writer_ops=8, read_ms="3-7",write_ms="1-3") as r:
        ps = [r.start(prog,i+1) for prog,n in [("reader",12),("writer",6)] for i in range(n)]
        for p in ps:
            complete(p,"BYE quota")
        assert r.end("complete")[:3] == (96,48,0)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--evidence", type=Path, help="Сохранить протоколы отдельных сценариев")
    options = parser.parse_args()
    EVIDENCE = options.evidence
    if EVIDENCE:
        EVIDENCE.mkdir(parents=True, exist_ok=True)
    cases = [("clients_two_monitors",clients_and_observers),
             *((f"policy_{k}",lambda k=k: policy(k)) for k in ("fair","readers","writers")),
             ("restart_after_kill_and_drop",reconnect), ("framing_and_fib93",network_and_invalid),
             ("signal_cancels_all",lambda: stop_all("signal")),
             ("control_cancels_all",lambda: stop_all("control")),
             ("reader_disconnect_and_resume",reader_disconnect_and_resume),
             ("commit_before_lost_ack",committed_without_ack), ("seed_replay",seed_and_boundaries), ("invalid_configs",invalid_configs),
             ("18_processes_144_operations",load_test)]
    for name, test in cases:
        test()
        print("PASS", name, flush=True)
    print(f"PASS {len(cases)}/{len(cases)}")
