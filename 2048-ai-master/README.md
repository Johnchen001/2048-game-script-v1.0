# 2048 AI

An AI for the [2048 game](http://gabrielecirulli.github.io/2048/), based on
**expectimax search** with a highly efficient **bitboard representation** —
the 4×4 board packs into 80 bits (5 bits per tile, two 64-bit words), letting
the engine evaluate **tens of millions of moves per second** on modern hardware.

The AI reliably merges tiles up to **2048** and routinely reaches **4096 / 8192**
or higher. With the 5-bit tile fields the engine supports tiles up to **2^31**
(rank 31), i.e. **65536 / 131072 and beyond** are fully mergeable — no more
32768 ceiling. A Chinese usage guide (使用规范 / 冲分攻略) is available at
[`docs/使用指南.md`](docs/使用指南.md), and the field report of a real
2048verse.com session (integration recipe, pitfalls, a 32768 run) at
[`docs/实战经验.md`](docs/实战经验.md).

Algorithm details: [StackOverflow answer](https://stackoverflow.com/a/22498940/1204143).

---

## Repository layout

| File | Purpose |
| --- | --- |
| `2048.cpp`, `2048.h` | C++ expectimax engine (bitboard + transposition table + heuristics) |
| `ailib.py` | `ctypes` bindings that load `bin/2048.{so,dll,dylib}` and convert boards |
| `2048.py` | Main script: drives a browser game, or gives move hints |
| `gamectrl.py` | Game controllers: `fast` / `keyboard` / `hybrid` / `play2048co` |
| `chromectrl.py`, `ffctrl.py` | Chrome DevTools / Firefox remote-debugging transports |
| `manualctrl.py` | Interactive "coach" mode (`-b manual`) |
| `platdefs.h`, `config.h*`, `configure.ac`, `Makefile.in`, `make-msvc.bat` | Build system |

---

## Building

### Unix / Linux / macOS

```sh
./configure
make
```

Any reasonably recent C++ compiler works. There is **no `make install`** —
the program is meant to run from this directory (it loads `bin/2048.so`).

> Quick build without autotools (macOS/Linux, g++ or clang++):
>
> ```sh
> mkdir -p bin
> g++ -O3 -Wall -Wextra -fPIC -shared -o bin/2048.so 2048.cpp   # shared lib for Python
> g++ -O3 -o bin/2048 2048.cpp                                  # standalone CLI
> ```

### Windows

- **Cygwin only**: follow the Unix instructions above. The resulting DLL works
  only with Cygwin programs, so use Cygwin Python for browser control.
  Step-by-step guide by Tamas Szell: [CygwinStepByStep.pdf](https://github.com/nneonneo/2048-ai/wiki/CygwinStepByStep.pdf).
- **Cygwin + MinGW** (64-bit DLL by default; works with non-Cygwin programs):

  ```sh
  CXX=x86_64-w64-mingw32-g++ CXXFLAGS='-static-libstdc++ -static-libgcc -D_WINDLL -D_GNU_SOURCE=1' ./configure ; make
  ```

  Use `CXX=i686-w64-mingw32-g++` for a 32-bit DLL.
- **Visual Studio**: open a "Native Tools Command Prompt" of the desired bitness
  and run `make-msvc.bat`.

> **Bitness must match**: the Python interpreter and the DLL must be the same
> bitness (32 vs 64), or loading fails with `%1 is not a valid Win32 application`.

---

## Usage

### 1. Command-line autoplay

```sh
bin/2048
```

The AI plays itself and prints every move, search statistics, and the final
score / highest tile. This is the fastest way to watch (and verify) the engine.

### 2. Browser autoplay (proof of concept)

`2048.py` can drive a real 2048 tab in Firefox or Chrome via remote debugging.

**Firefox**: enable `devtools.debugger.remote-enabled` and
`devtools.chrome.enabled` in `about:config`, restart with
`--start-debugger-server 32000`, open the game in a tab, then:

```sh
python3 2048.py -b firefox -p 32000
```

**Chrome**: restart Chrome with

```sh
google-chrome --remote-debugging-port=9222 --remote-allow-origins=http://localhost:9222 --user-data-dir=chrome.tmp
```

open the game in a tab, then:

```sh
python3 2048.py -b chrome -p 9222
```

Game variants via `-k` (default `hybrid`):

| `-k` mode | Notes |
| --- | --- |
| `hybrid` | Default; original 2048 or compatible clones |
| `fast` | Faster, less compatible with clones |
| `keyboard` | Slower, works with some clones |
| `play2048co` | For the 2025 play2048.co version |

Multithreading (default 4 worker threads) can be tuned with the environment
variable `2048_THREADS` (e.g. `2048_THREADS=8 python3 2048.py -b chrome`).

### 3. Interactive hints ("coach mode")

```sh
python3 2048.py -b manual
```

Enter your current board (one row per line, values separated by spaces), and the
AI recommends the next move. After each move, tell it the newly spawned tile as
`r,c,n` (1-indexed row/column, value `n`), e.g. `3,1,4`. Multiple updates go on
one line: `1,1,4 1,3,2`. This works on any platform (phones included) and is the
recommended way to learn the strategy — see
[`docs/使用指南.md`](docs/使用指南.md).

Sample session:

```
Row 1: 16 128 256 1024
Row 2: 16 8 2 0
Row 3: 8 2 0 0
Row 4: 0 4 0 0
...
005.030340: Score 0, Move 1: up
EXECUTE MOVE: up
...
Enter update(s) in the form r,c,n ... : 3,1,4
```

---

## How it works (short version)

- **Bitboard**: 16 tiles × 4 bits in one `uint64_t`; `execute_move` is a handful
  of table lookups + XORs (precomputed 65536-entry move tables per row).
- **Search**: expectimax over (move → tile-spawn) nodes; a transposition table
  caches node values, `CPROB_THRESH_BASE = 1e-4` prunes unlikely branches, and
  depth adapts: `depth_limit = max(3, #distinct_tiles − 2)`.
- **Heuristics**: bonuses for empty squares and imminent merges, penalties for
  losing and for non-monotonic layouts (weights in `2048.cpp`:
  `SCORE_*_WEIGHT`). This encodes the classic *corner strategy*.

**Representation**: a tile stores its exponent (rank) in **5 bits**, so the
board needs 80 bits (two `uint64_t` words); the largest representable tile is
**2^31** (rank 31) — 65536 (rank 16) and 131072 (rank 17) merge normally.
Two rank-31 tiles do not merge (representational limit).

**Speed knobs** (environment variables, read once at startup):
- `2048_DEPTH=<int>`: added to the adaptive depth limit
  (`max(3, #distinct_tiles − 2 + adj)`). Negative = faster/weaker
  (e.g. `2048_DEPTH=-3` ≈ 5× faster), positive = slower/stronger.
- `2048_CPROB=<float>`: search pruning threshold (default `1e-4`).
  `2048_CPROB=0.001` ≈ 8× faster with mild strength loss; combine with
  `2048_DEPTH=-3` for up to ~16×.
- `2048_KEY_DELAY=<seconds>`: browser keyboard settle delay (default `0.05`).
- `2048_THREADS=<n>`: browser-control worker threads (default `4`).

---

## Troubleshooting

| Symptom | Fix |
| --- | --- |
| `找不到 2048 核心库 / Couldn't find 2048 library bin/2048.so` | Build first: `./configure && make` (or the quick g++ one-liner above) |
| Chrome: `websocket-client library not available` | `pip install websocket-client` |
| Chrome: no pages listed / connection refused | Launch Chrome with `--remote-debugging-port` and `--remote-allow-origins` as shown above |
| `%1 is not a valid Win32 application` | Bitness mismatch between Python and the DLL — rebuild with matching bitness |
| AI never reaches 2048 | Rare; the AI is probabilistic. Rerun, or use `-b manual` coach mode to learn the corner strategy |

---

## License

MIT — see [LICENSE](LICENSE).
