"""Независимое воспроизведение событий сервера. Запуск: python3 tests/audit.py ЖУРНАЛ."""
import argparse
import ast
from pathlib import Path


def fib(n):
    a, b = 0, 1
    for _ in range(n):
        a, b = b, a + b
    return a


def audit(lines, initial=None):
    if not __debug__:
        raise RuntimeError("Запустите проверку без флага -O")
    assert lines, "Пустой журнал"
    if initial is None:
        fields = dict(x.split("=", 1) for x in lines[0].split()[4:])
        initial = ast.literal_eval(fields["db"])
    db = list(initial)
    assert db and db == sorted(db) and all(1 <= x <= 93 for x in db)
    active, waiting = {}, {}
    reads = writes = cancels = peak = 0
    last = 0
    used = set()
    progress = {}
    for line in lines:
        _, seq, evt, actor, *pairs = line.split()
        d = dict(item.split("=", 1) for item in pairs)
        token = int(d["ticket"])
        assert int(seq) == last + 1
        last += 1
        if last == 1:
            assert evt == "INIT" and d["db"] == str(db).replace(" ", "")
        if evt == "REQUEST":
            assert actor[0] in "RW" and 0 <= int(d["index"]) < len(db)
            assert all(a != actor for a, _ in (*waiting.values(), *active.values()))
            assert token not in used
            used.add(token)
            waiting[token] = (actor, int(d["index"]))
        elif evt in ("READ_BEGIN", "WRITE_BEGIN"):
            assert actor[0] == ("R" if evt == "READ_BEGIN" else "W")
            who, idx = waiting.pop(token)
            assert actor == who and db[idx] == int(d["old"])
            assert all(a.startswith("R") and actor.startswith("R") for a, _ in active.values())
            active[token] = (actor, idx)
        elif evt == "CANCEL":
            owner = active if d["began"] == "1" else waiting
            assert owner.pop(token)[0] == actor
            cancels += 1
        elif evt in ("READ_END", "WRITE_END"):
            assert actor[0] == ("R" if evt == "READ_END" else "W")
            progress[actor] = progress.get(actor, 0) + 1
            assert int(d["op"]) == progress[actor]
            who, idx = active.pop(token)
            assert who == actor and idx == int(d["index"])
            if evt == "READ_END":
                assert int(d["value"]) == db[idx] and int(d["fib"]) == fib(db[idx])
                reads += 1
            else:
                assert db[idx] == int(d["old"])
                assert 1 <= int(d["new"]) <= 93
                db[idx] = int(d["new"])
                db.sort()
                assert d["db"] == str(db).replace(" ", "")
                writes += 1
        elif evt == "SUMMARY":
            assert not active and not waiting and line == lines[-1]
            assert (reads, writes, cancels, peak) == tuple(
                int(d[k]) for k in ("reads", "writes", "cancelled", "peak_readers"))
            assert d["db"] == str(db).replace(" ", "")
        else:
            assert evt in ("INIT", "ONLINE", "OFFLINE")
        ar = sum(a.startswith("R") for a, _ in active.values())
        assert (ar, len(active)-ar) == (int(d["r"]), int(d["w"]))
        peak = max(peak, ar)
    assert last and " SUMMARY " in lines[-1]
    return reads, writes, cancels, peak


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    args = parser.parse_args()
    try:
        totals = audit(args.log.read_text().splitlines())
    except (AssertionError, KeyError, ValueError, SyntaxError) as exc:
        raise SystemExit(f"Журнал не прошёл проверку: {exc}")
    print("OK reads=%d writes=%d cancelled=%d peak_readers=%d" % totals)
