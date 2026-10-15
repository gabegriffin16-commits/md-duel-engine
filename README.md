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
{"cmd":"start","players":[{"main":[codes],"extra":[codes]}, {...}],    -> {"ev":"started"}, then messages
  "seed":[4 x u64] (optional, random if left out), "lp":8000, "hand":5, "draw":1}
{"cmd":"respond","player":0|1,"answer":{...}}                            answers the waiting prompt
{"cmd":"surrender","player":0|1}                                        -> {"ev":"end","winner":..,"reason":"surrender"}
{"cmd":"quit"}
```

Player 0 goes first. Out:

- `{"ev":"msg","msg":{"t":"move", ...}}`: everything that happens, decoded from the core's messages (field names follow
  `src/messages.cpp`). A layout the helper doesn't know yet comes with `"raw"` (hex).
- `{"ev":"prompt","prompt":{"t":"select_card","type":15,"player":0, ...}}`: a player has to decide; the duel waits.
- `{"ev":"retry","prompt":{...}}`: the last answer wasn't legal; the same prompt again.
- `{"ev":"end","winner":0|1|2,"reason":...}`, `{"ev":"error","error":"..."}`, `{"ev":"warning",...}`, `{"ev":"log",...}`.

Answers per prompt are listed above `encode_response` in `src/messages.cpp`.

Not filtered yet: every message is sent as the core writes it, including hidden cards (hands, Deck order, face-down
cards). The host must filter per player before sending anything to the other side (planned here next).
