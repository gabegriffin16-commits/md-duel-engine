# duel-engine

The rules helper behind the md-duel app: it runs a two-player Yu-Gi-Oh! duel in the real rules engine and talks JSON
lines over stdin/stdout. The game runs it as a separate program.

- License: **AGPL-3.0-or-later** (see `LICENSE`). This repository is the complete source of the helper.
- Rules engine: [edo9300/ygopro-core](https://github.com/edo9300/ygopro-core) (AGPL-3.0-or-later), built in at commit
  `38d04c9feb1a26617407091380634c87262fe3f8`, with its bundled Lua 5.4.
- Card scripts: [ProjectIgnis/CardScripts](https://github.com/ProjectIgnis/CardScripts) (AGPL-3.0) and `cards.cdb` from
  [ProjectIgnis/BabelCDB](https://github.com/ProjectIgnis/BabelCDB). Not bundled; downloaded on first use.
- Also built in: SQLite (public domain), miniz (MIT), nlohmann/json (MIT).

## Build

```
cmake -B build -DOCGCORE_DIR=<ygopro-core checkout, with the lua/src submodule> -DDEPS_DIR=<folder with json.hpp, miniz/, sqlite/>
cmake --build build --config Release
```

`.github/workflows/build.yml` shows the exact downloads. `tests/selfplay.py` plays random duels to the end and fails on
any message it can't decode.

## Protocol

One JSON object per line each way.

```
{"cmd":"init","cdb":"cards.cdb","scripts":"CardScripts.zip"}          -> {"ev":"ready","cards":N,"scripts":N,"core":"11.0"}
{"cmd":"start","players":[{"main":[codes],"extra":[codes]}, {...}],    -> {"ev":"started"}, then events
  "seed":[4 x u64] (optional, random if left out), "lp":8000, "hand":5, "draw":1,
  "view":"split" (default: per-player events) | "all" (one view of everything, for local tests)}
{"cmd":"respond","player":0|1,"answer":{...}}                            answers the waiting prompt
{"cmd":"cards","codes":[codes]}                                        -> {"ev":"cards","cards":{"<code>":{name,desc,type,atk,def,
                                                                          level,link,linkMarker,attribute,race,lscale,rscale,setcode,alias,strings}}}
{"cmd":"search","text":"ash blo","limit":30}                            -> {"ev":"search","cards":[...]} (name search, for declaring a card)
{"cmd":"resync","player":0|1}                                           -> a full state for that player (+ their prompt)
{"cmd":"surrender","player":0|1}                                        -> {"ev":"end","winner":..,"reason":"surrender"}
{"cmd":"quit"}
```

Player 0 goes first.

### Views

With `"view":"split"` (online) every event for one player carries `"to":0|1`; events without `"to"` (`end`, `error`,
`started`...) are for both. The host sends each player only their events. What a player can't see is removed the way
the YGOPro / EDOPro servers do it: the other player's drawn / added / Set / face-down / Extra Deck cards, hand shuffles,
cards they look at in their own Deck and their "select a card to..." hints show code 0 or aren't sent; prompts go to
their player only (the other gets `{"ev":"waiting","player":p}`), with the other player's face-down cards as code 0.
`tests/selfplay.py` checks this independently: each game is played twice with the same seed (all / split) and any card
a player sees before it was ever public in the "all" run, and that isn't in their own deck, fails the test.

### Events

- `{"ev":"msg","msg":{"t":"move", ...}}`: everything that happens, decoded from the core's messages (field names follow
  `src/messages.cpp`). For animation and the duel log.
- `{"ev":"state","full":bool,"state":{...}}`: the board as that player sees it, sent before each prompt when it changed:
  `turn`, `turnPlayer`, `phase`, `chain` (links: code, at, player, desc, resolving / resolved / negated / disabled) and
  `players[0..1]`: `mzone` (7: 5 Main + 2 Extra Monster Zones), `szone` (8: 5 Spell/Trap, Field, 2 Pendulum),
  `hand`, `grave`, `removed`, `extra` (cards: code, pos, type, level, rank, attribute, race, atk, def, baseAtk, baseDef,
  link, linkMarker, lscale, rscale, materials, counters, owner, public, negated, equippedTo, targets; null = empty
  zone; `{"hidden":true,"pos":..}` = a card this player can't see), `deck` (count), `lp`. When `full` is false it's a
  diff: top-level fields that changed, and under `players` an object `{"0": {...}, "1": {...}}` with only the places
  that changed (each place is sent whole).
- `{"ev":"prompt","prompt":{"t":"select_card","type":15,"player":0, ...}}`: a player has to decide; the duel waits.
- `{"ev":"retry","prompt":{...}}`: the last answer wasn't legal; the same prompt again.
- `{"ev":"end","winner":0|1|2,"reason":...}`, `{"ev":"error","error":"..."}`, `{"ev":"warning",...}`, `{"ev":"log",...}`.

Answers per prompt are listed above `encode_response` in `src/messages.cpp`. A battle prompt with neither `end` nor
`main2` is a forced attack. Layouts not decoded yet (`raw`, not sent in split view except swap_grave_deck):
swap_grave_deck's card list, tag_swap, reload_field, show_hint.
