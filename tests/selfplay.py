"""Self-play test: two random players duel to the end through duel-engine.

  py tests/selfplay.py <duel-engine.exe> <cards.cdb> <CardScripts.zip> <deckA.json> <deckB.json> [games] [seed]

Decks: {"main":[codes], "extra":[codes]}. Every prompt gets a random answer that is usually legal (the engine
asks again on an illegal one). Each game is played twice with the same seed and answers: once with one view of
everything ("all") and once with per-player views ("split", as online). Fails when:
  - a message didn't decode exactly ("bad"), the engine reports an error, a duel doesn't finish, or a prompt is stuck;
  - the two runs differ (same seed + answers must give the same duel);
  - routing is wrong (a prompt sent to the other player, a player's private hint sent to the opponent);
  - a LEAK: a player saw a card code before it was ever public in the "all" run and it isn't in their own deck.
"""
import json, random, subprocess, sys, time
from collections import Counter

ENG, CDB, ZIP, DA, DB = sys.argv[1:6]
GAMES = int(sys.argv[6]) if len(sys.argv) > 6 else 5
SEED = int(sys.argv[7]) if len(sys.argv) > 7 else 1
decks = [json.load(open(DA)), json.load(open(DB))]

p = subprocess.Popen([ENG], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, encoding="utf-8", bufsize=1)
def send(o): p.stdin.write(json.dumps(o) + "\n"); p.stdin.flush()
def read():
    line = p.stdout.readline()
    if not line: raise SystemExit("engine exited")
    return json.loads(line)

def bits(mask, width): return [i for i in range(width) if mask >> i & 1]

def free_places(pr):
    me, flag, out = pr["player"], pr["flag"], []
    for side in (0, 1):
        con = me if side == 0 else 1 - me
        for seq in range(7):
            if not flag >> (seq + 16 * side) & 1: out.append({"con": con, "loc": 0x04, "seq": seq})
        for seq in range(8):
            if not flag >> (seq + 8 + 16 * side) & 1: out.append({"con": con, "loc": 0x08, "seq": seq})
    return out

def answer(rng, pr, st):
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
    if t == "select_position": return {"pos": 1 << rng.choice(bits(pr["positions"], 4))}
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
    if t == "announce_card": return {"code": rng.choice(decks[0]["main"] + decks[1]["main"])}
    if t == "rock_paper_scissors": return {"choice": rng.randint(1, 3)}
    raise SystemExit(f"no answer for prompt {t}")

def play(g, view, players, seed4):
    """Plays one duel; returns (events with the prompt count k each was seen at, result, stats)."""
    rng = random.Random(SEED * 1000 + g)
    send({"cmd": "start", "seed": seed4, "players": players, "view": view})
    st = {"tries": 0, "acts": 0}
    t0, k, retries, turns, out, problems = time.time(), 0, 0, 0, [], []
    while True:
        if time.time() - t0 > 120: problems.append("timeout"); return out, None, problems, (turns, k, retries)
        e = read()
        out.append((k, e))
        ev = e.get("ev")
        if ev == "msg" and e["msg"]["t"] == "new_turn" and e.get("to", 0) == 0: turns += 1; st["acts"] = 0
        elif ev in ("prompt", "retry"):
            pr = e["prompt"]
            if "to" in e and e["to"] != pr["player"]: problems.append(f"prompt for {pr['player']} sent to {e['to']}")
            if ev == "retry":
                retries += 1; st["tries"] += 1
                if st["tries"] == 50: problems.append(f"stuck on {pr['t']}: {json.dumps(pr)[:400]}")
            else: st["tries"] = 0; k += 1
            if turns > 60 or k > 6000:   # random players can stall; call it
                send({"cmd": "surrender", "player": pr["player"]}); continue
            send({"cmd": "respond", "player": pr["player"], "answer": answer(rng, pr, st)})
        elif ev == "end":
            if view == "split":   # the boards built from diffs must equal a fresh full state
                boards = [None, None]
                for _, x in out:
                    if x.get("ev") == "state": boards[x["to"]] = merge(boards[x["to"]], x)
                for v in (0, 1):
                    send({"cmd": "resync", "player": v}); full = read()
                    if full.get("ev") != "state": print("RESYNC GOT", json.dumps(full)[:400])
                    if full.get("ev") != "state" or full["state"] != boards[v]: problems.append(f"merged board for {v} differs from resync: " + "; ".join(
                            f"{k}: {json.dumps(boards[v].get(k))[:300]} vs {json.dumps(full['state'].get(k))[:300]}" if k != "players" else
                            "; ".join(f"p{p}.{pk}: {json.dumps(boards[v]['players'][p].get(pk))[:300]} vs {json.dumps(full['state']['players'][p].get(pk))[:300]}"
                                      for p in (0, 1) for pk in full['state']['players'][p] if boards[v]['players'][p].get(pk) != full['state']['players'][p].get(pk))
                            for k in full["state"] if boards[v] and boards[v].get(k) != full["state"].get(k)))
            return out, e, problems, (turns, k, retries)
        elif ev == "error": problems.append(e["error"])

FACEUP, FACEDOWN = 0x5, 0xA
def merge(cur, ev):
    """Applies a state event (full or diff) to a viewer's board."""
    if ev["full"] or cur is None: return json.loads(json.dumps(ev["state"]))
    for k, v in ev["state"].items():
        if k == "players":
            for p, d in v.items(): cur["players"][int(p)].update(d)
        else: cur[k] = v
    return cur

def public_codes(all_events):
    """For the "all" run: prompt count -> codes public by then (face-up on the field, GY, face-up banished /
    Extra Deck, public hand cards, activated, revealed)."""
    pub, by_k, cur = set(), {}, None
    for k, e in all_events:
        ev = e.get("ev")
        if ev == "state":
            cur = merge(cur, e)
            for pl in cur["players"]:
                for place in ("mzone", "szone", "grave", "removed", "extra", "hand"):
                    for c in pl[place]:
                        if not c: continue
                        if place == "grave" or c.get("public") or (place != "hand" and c.get("pos", 0) & FACEUP):
                            pub.add(c["code"])
                        for m in c.get("materials", []): pub.add(m)
        elif ev == "msg":
            m = e["msg"]
            if m["t"] in ("chaining", "summoning", "flipsummoning", "spsummoning", "deck_top", "match_kill"): pub.add(m.get("code"))
            if m["t"] in ("confirm_cards", "confirm_decktop", "confirm_extratop"):
                for c in m["cards"]: pub.add(c["code"])
            if m["t"] == "move" and (m["to"]["loc"] & 0x10 or (m["to"]["loc"] & 0x0c and m["to"]["pos"] & FACEUP)): pub.add(m["code"])
            if m["t"] == "pos_change" and m["pos"] & FACEUP: pub.add(m["code"])
        by_k[k] = set(pub)
    return by_k

def codes_in(e):
    """Every card code an event shows (not effect descriptions)."""
    out = []
    def walk(x, key=None):
        if isinstance(x, dict):
            for kk, v in x.items(): walk(v, kk)
        elif isinstance(x, list):
            for v in x: walk(v, key)
        elif key in ("code", "code1", "code2", "materials") and isinstance(x, int) and x: out.append(x)
    walk(e.get("msg") or e.get("state") or e.get("prompt") or {})
    return out

send({"cmd": "init", "cdb": CDB, "scripts": ZIP})
r = read()
assert r.get("ev") == "ready", r
print("ready:", r)

seeder = random.Random(SEED)
kinds, bad, problems, sizes = Counter(), {}, [], Counter()
for g in range(GAMES):
    order = [0, 1] if g % 2 == 0 else [1, 0]
    players = [decks[order[0]], decks[order[1]]]
    own = [set(players[i]["main"]) | set(players[i]["extra"]) for i in (0, 1)]
    seed4 = [seeder.getrandbits(63) for _ in range(4)]
    ev_all, res_all, pr_all, s_all = play(g, "all", players, seed4)
    ev_split, res_split, pr_split, s_split = play(g, "split", players, seed4)
    problems += [f"game {g} (all): {x}" for x in pr_all] + [f"game {g} (split): {x}" for x in pr_split]
    if res_all != res_split or s_all != s_split:
        problems.append(f"game {g}: runs differ: {res_all} {s_all} vs {res_split} {s_split}")
    pub = public_codes(ev_all)
    leaks = Counter()
    for k, e in ev_split:
        sizes[e.get("ev")] += len(json.dumps(e))
        if e.get("ev") == "msg":
            m = e["msg"]; kinds[m["t"]] += 1
            if m.get("bad"): bad.setdefault(m["t"], m)
            if m["t"] == "hint" and m["type"] in (1, 2, 3, 5) and e["to"] != m["player"]:
                problems.append(f"game {g}: private hint for {m['player']} sent to {e['to']}")
        if e.get("ev") in ("prompt", "retry"):
            kinds["?" + e["prompt"]["t"]] += 1
            if e["prompt"].get("bad"): bad.setdefault(e["prompt"]["t"], e["prompt"])
        v = e.get("to")
        if v is None: continue
        known = own[v] | pub.get(k, set()) | pub.get(k + 1, set())
        for c in codes_in(e):
            if c not in known: leaks[(e["ev"], (e.get("msg") or {}).get("t"), c)] += 1
    for (ev, t, c), n in leaks.items():
        problems.append(f"game {g}: LEAK to a player: {ev}/{t} shows {c} ({n}x) before it was public")
    print(f"game {g}: {res_split} turns={s_split[0]} prompts={s_split[1]} retries={s_split[2]} "
          f"split events={len(ev_split)} bytes={sum(len(json.dumps(e)) for _, e in ev_split)}")

send({"cmd": "quit"})
print("\nmessage / ?prompt counts (split runs):")
for k, v in sorted(kinds.items(), key=lambda x: -x[1]): print(f"  {k:28} {v}")
print("bytes by event (split runs):", dict(sizes))
for t, b in bad.items(): print("BAD LAYOUT:", t, b.get("raw"))
for x in problems[:60]: print("PROBLEM:", x)
sys.exit(1 if bad or problems else 0)
