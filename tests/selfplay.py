"""Self-play test: two random players duel to the end through duel-engine.

  py tests/selfplay.py <duel-engine.exe> <cards.cdb> <CardScripts.zip> <deckA.json> <deckB.json> [games] [seed]

Decks: {"main":[codes], "extra":[codes]}. Every prompt gets a random answer that is usually legal (the engine
asks again on an illegal one). Fails when a message didn't decode exactly ("bad"), the engine reports an error,
or a duel doesn't finish. Prints message type counts so new layouts are easy to spot.
"""
import json, random, subprocess, sys, time
from collections import Counter

ENG, CDB, ZIP, DA, DB = sys.argv[1:6]
GAMES = int(sys.argv[6]) if len(sys.argv) > 6 else 5
rng = random.Random(int(sys.argv[7]) if len(sys.argv) > 7 else 1)
decks = [json.load(open(DA)), json.load(open(DB))]

p = subprocess.Popen([ENG], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, encoding="utf-8", bufsize=1)
def send(o): p.stdin.write(json.dumps(o) + "\n"); p.stdin.flush()
def read():
    line = p.stdout.readline()
    if not line: raise SystemExit("engine exited")
    return json.loads(line)

def bits(mask, width):
    return [i for i in range(width) if mask >> i & 1]

def free_places(pr):
    me, flag, out = pr["player"], pr["flag"], []
    for side in (0, 1):
        con = me if side == 0 else 1 - me
        for seq in range(7):
            if not flag >> (seq + 16 * side) & 1: out.append({"con": con, "loc": 0x04, "seq": seq})
        for seq in range(8):
            if not flag >> (seq + 8 + 16 * side) & 1: out.append({"con": con, "loc": 0x08, "seq": seq})
    return out

def answer(pr, st):
    t, n = pr["t"], st["tries"]
    if t == "select_idlecmd":
        st["acts"] += 1
        opts = [(a, i) for a in ("activate", "summon", "spsummon", "mset", "sset") for i in range(len(pr[a]))]
        if opts and st["acts"] < 25 and rng.random() < 0.85 and n < 3:
            a, i = rng.choice(opts); return {"act": a, "i": i}
        if pr["battle"] and rng.random() < 0.7: return {"act": "battle"}
        return {"act": "end"}
    if t == "select_battlecmd":
        if pr["attack"] and rng.random() < 0.8 and n < 3: return {"act": "attack", "i": rng.randrange(len(pr["attack"]))}
        if pr["activate"] and rng.random() < 0.3 and n < 3: return {"act": "activate", "i": rng.randrange(len(pr["activate"]))}
        if not pr["end"] and not pr["main2"] and pr["attack"]:   # a forced attack: ending the battle isn't offered
            return {"act": "attack", "i": rng.randrange(len(pr["attack"]))}
        return {"act": "main2" if pr["main2"] and (rng.random() < 0.3 or not pr["end"]) else "end"}
    if t in ("select_effectyn", "select_yesno"): return {"yes": rng.random() < 0.7}
    if t == "select_option": return {"i": rng.randrange(len(pr["options"]))}
    if t == "announce_number": return {"i": rng.randrange(len(pr["options"]))}
    if t == "select_chain":
        k = len(pr["chains"])
        if pr["forced"]: return {"i": rng.randrange(k)}
        return {"i": rng.randrange(k) if k and rng.random() < 0.5 and n < 3 else -1}
    if t in ("select_card", "select_tribute"):
        k = len(pr["cards"])
        if pr["cancelable"] and n > 6: return {"cancel": True}
        c = rng.randint(pr["min"], min(pr["max"], k)) if k else 0
        return {"picks": rng.sample(range(k), c)}
    if t == "select_sum":
        must = sum(m["param"] & 0xffff for m in pr["must"])
        k, acc = len(pr["cards"]), pr["acc"]
        for _ in range(2000):
            idx = rng.sample(range(k), rng.randint(1, k)) if k else []
            vals = [c["param"] for c in (pr["cards"][i] for i in idx)]
            s = must + sum(v & 0xffff for v in vals)
            s2 = must + sum((v >> 16) or (v & 0xffff) for v in vals)
            if (pr["mode"] == 0 and acc in (s, s2)) or (pr["mode"] == 1 and max(s, s2) >= acc):
                return {"picks": idx}
        return {"picks": list(range(k))}
    if t == "select_unselect_card":
        if pr["finishable"] and (rng.random() < 0.4 or n > 4): return {"done": True}
        if pr["select"]: return {"i": rng.randrange(len(pr["select"]))}
        return {"done": True}
    if t in ("select_place", "select_disfield"):
        free = free_places(pr); rng.shuffle(free)
        return {"places": free[:pr["count"]]}
    if t == "select_position":
        return {"pos": 1 << rng.choice(bits(pr["positions"], 4))}
    if t == "select_counter":
        left, counts = pr["count"], []
        for c in pr["cards"]:
            v = min(left, c["have"]); counts.append(v); left -= v
        return {"counts": counts}
    if t in ("sort_card", "sort_chain"): return {}
    if t == "announce_race":
        av = bits(pr["available"], 64); rng.shuffle(av)
        return {"value": sum(1 << b for b in av[:pr["count"]])}
    if t == "announce_attrib":
        av = bits(pr["available"], 32); rng.shuffle(av)
        return {"value": sum(1 << b for b in av[:pr["count"]])}
    if t == "announce_card":
        return {"code": rng.choice(decks[0]["main"] + decks[1]["main"])}
    if t == "rock_paper_scissors": return {"choice": rng.randint(1, 3)}
    raise SystemExit(f"no answer for prompt {t}")

send({"cmd": "init", "cdb": CDB, "scripts": ZIP})
r = read()
assert r.get("ev") == "ready", r
print("ready:", r)

kinds, bad, problems = Counter(), [], []
for g in range(GAMES):
    order = [0, 1] if g % 2 == 0 else [1, 0]
    send({"cmd": "start", "seed": [rng.getrandbits(63) for _ in range(4)], "players": [decks[order[0]], decks[order[1]]]})
    st = {"tries": 0, "acts": 0}
    t0, prompts, retries, turns, done = time.time(), 0, 0, 0, None
    while done is None:
        if time.time() - t0 > 120: problems.append(f"game {g}: timeout"); break
        e = read()
        ev = e.get("ev")
        if ev == "msg":
            m = e["msg"]; kinds[m["t"]] += 1
            if m.get("bad"): bad.append(m)
            if m["t"] == "new_turn": turns += 1; st["acts"] = 0
        elif ev in ("prompt", "retry"):
            pr = e["prompt"]; kinds["?" + pr["t"]] += 1
            if pr.get("bad"): bad.append(pr)
            if ev == "retry":
                retries += 1; st["tries"] += 1
                if st["tries"] == 50: problems.append(f"game {g}: stuck on {pr['t']}: {json.dumps(pr)[:600]}")
            else: st["tries"] = 0; prompts += 1
            if turns > 60 or prompts > 6000:   # random players can stall; call it a draw
                send({"cmd": "surrender", "player": pr["player"]}); continue
            send({"cmd": "respond", "player": pr["player"], "answer": answer(pr, st)})
        elif ev == "end": done = e
        elif ev == "error": problems.append(f"game {g}: {e['error']}")
        elif ev == "warning": print("warning:", e)
    print(f"game {g}: {done} turns={turns} prompts={prompts} retries={retries} {time.time() - t0:.1f}s")

send({"cmd": "quit"})
print("\nmessage / ?prompt counts:")
for k, v in sorted(kinds.items(), key=lambda x: -x[1]): print(f"  {k:28} {v}")
seen = {}
for b in bad: seen.setdefault(b["t"], b)
for t, b in seen.items(): print("BAD LAYOUT:", t, b.get("raw"))
for x in problems: print("PROBLEM:", x)
sys.exit(1 if seen or problems else 0)
