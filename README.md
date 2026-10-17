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
{"cmd":"dev","op":...,"player":0|1,...}                                 -> {"ev":"reset"}, {"ev":"dev","ok":true}, then the
                                                                          duel again (only in duels started with "dev":true)
{"cmd":"quit"}
```

Player 0 goes first.

### Test layouts (local duels)

`start` may carry `"dev":true` (allows `dev` commands) and `"setup"`: a layout to start from instead of shuffled
decks and opening hands. It's built the way puzzles are (Debug.ReloadFieldBegin / AddCard / ReloadFieldEnd, with
DUEL_ATTACK_FIRST_TURN), so a layout can start in battle on turn 1:

```
{"turn":0|1, "players":[{"lp":8000, "deck":[...], "extra":[...], "hand":[...], "grave":[...], "removed":[...],
   "mzone":[null | {"card":"Name"|code, "seq":0-6, "pos":"atk"|"def"|"set"|POS_*, "materials":[...], "counters":[...],
   "owner":0|1}], "szone":[... same, seq 0-4, 5 = Field Zone; "equippedTo":{"con","seq"}]}, {...}],
 "actions":[{"do":"summon", "player", "card", "pos", "materials":[...], "how":"special"|"normal"},
   {"do":"draw", "player", "count"}, {"do":"damage"|"recover", "player", "amount"},
   {"do":"destroy"|"tograve"|"banish"|"tohand", "player", "card"}]}
```

Cards are codes or names (exact, else the only few names containing it; a vague name is an error listing matches).
Decks and Extra Decks left out come from `players`. Actions run for real (messages, chains, triggers) at the start of
the turn player's Main Phase 1; summons from the Extra Deck use their summon type (Fusion / Synchro / Xyz / Link) and
count as properly summoned. `"turn":1` plays player 0's turn through on default answers without showing it, so the
duel opens on player 1's turn 2. The two Extra Monster Zones are shared (player 0's 5 / 6 are player 1's 6 / 5).

`dev` rebuilds the running duel from the board as it is (Deck order, LP, materials, counters, equips kept; the turn
count and once-per-turn uses start over) with one change: `op` `spawn` (`card`, `to`: hand / mzone / emz / szone /
field / grave / removed / deck / extra, `pos`, `seq`, `materials`), `act` (`action`: one of the actions above),
`lp` (`value`), `remove` (`card`), `clear` (field to GY; `player` or both), `turn` (`turn`: whose turn it becomes),
`load` (`setup`: a whole layout). A failed one answers `{"ev":"dev","ok":false,"error":...}` and changes nothing.

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
