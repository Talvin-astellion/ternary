# Ternary CPU Emulator

A small emulator for the balanced-ternary CPU.  The core is dependency-free C;
the analysis tools are optional Python 3 scripts.

## Files

| File | Description |
| --- | --- |
| `cpu.c`, `cpu.h` | CPU state, instruction execution, register helpers |
| `mem.c`, `mem.h` | RAM and memory-mapped I/O bus |
| `ternary.c`, `ternary.h` | Balanced-ternary arithmetic and parsing |
| `disassem.c`, `disassem.h` | Instruction decoding and disassembly |
| `main.c` | Command-line monitor |
| `tml.c`, `tml.h` | Ternary Moral Logic (TML) runtime and state machine |
| `tml_policy.c`, `tml_policy.h` | Policy defaults and `key=value` policy loader |
| `tml_log.c`, `tml_log.h` | Append-only JSONL Always Memory with hash chaining |
| `sha256.c`, `sha256.h` | SHA-256 used by Always Memory |
| `trace.c`, `trace.h` | Instruction / taint / memory JSONL tracing |
| `taint.c`, `taint.h` | Register and memory taint propagation |
| `symbolic.c`, `symbolic.h` | Lightweight collection of conditional constraints |
| `tests/` | Unit tests run by `make test` |

## Build and test

```sh
make -C emu                 # builds emu/tern-cpu-emu
make -C emu test            # core, TML and trace/taint/symbolic tests
make -C emu sanitize        # same tests with ASan and UBSan
```

`make -C emu all` additionally builds Doxygen documentation and therefore
requires `doxygen`; plain `make` does not.

## Monitor

```sh
./emu/tern-cpu-emu [options] [program.tas]
```

| Option | Description |
| --- | --- |
| `--tml` | Enable the TML runtime |
| `--tml-log FILE` | Always Memory JSONL path (default `tml_memory.jsonl`) |
| `--tml-policy FILE` | Load policy thresholds from a file |
| `--trace FILE` | Write an instruction trace JSONL |
| `--taint` | Enable taint tracking |
| `--symbolic FILE` | Write conditional constraints as JSONL |

`HALT` stops the CPU until `c` resets it.  Invalid opcodes and malformed
register fields latch a trap and stop the CPU instead of executing as NOPs.
`REG_CLOCK` counts executed instructions; the flags register tracks sign,
carry, overflow and trit parity.

Interactive commands:

| Command | Action |
| --- | --- |
| `<Enter>` | Step one instruction |
| `g [N]` | Run up to `N` instructions (default 100000), stopping on FREEZE |
| `s` | Print CPU state |
| `r <ADDR>` / `w <ADDR> <VAL>` | Peek / poke a tryte |
| `c` / `m` | Reset CPU / memory |
| `f <FILE>` | Load a file into memory |
| `t` | Print TML status |
| `l` | Print the tail of the Always Memory log |
| `o +|-|0` | Human oversight override: PROCEED / REFUSE / SACRED_ZERO |
| `u` | Recover from FREEZE with the built-in unfreeze token |
| `x` | Print taint state (`--taint`) |
| `y` | Print collected symbolic constraints |
| `q` | Quit |

Addresses and values are entered in balanced ternary (`-`, `0`, `1`).

## TML runtime

The runtime implements the fail-closed TML state machine:

```
BOOT -> ACTIVE -> SACRED_PAUSE -> FREEZE -> RECOVERY
```

* `PROCEED` (`+1`), `SACRED_ZERO` (`0`), `REFUSE` (`-1`).
* `SACRED_PAUSE` blocks external actuation while evidence is collected.
* `FREEZE` is latched; only the unfreeze token can enter `RECOVERY`.
* A Type-0 anomaly (actuation without a logged decision) triggers `FREEZE`.
* Every boot, decision, override, freeze and recovery is appended to Always
  Memory as JSONL with `prev_hash` / `hash` SHA-256 chaining.

### MMIO

The TML registers are memory-mapped at negative ternary addresses:

| Address | Name | Access | Meaning |
| --- | --- | --- | --- |
| `-26` | `ADDR_UART_STATUS` | read | +1 input ready, 0 empty, -1 EOF |
| `-17` | `ADDR_TML_STATUS` | read | FSM state: 0 BOOT, 1 ACTIVE, 2 SACRED_PAUSE, 3 FREEZE, 4 RECOVERY |
| `-18` | `ADDR_TML_ACTION` | read/write | Writing submits an action id and evaluates it |
| `-19` | `ADDR_TML_ARG0` | read/write | Action argument 0 |
| `-20` | `ADDR_TML_ARG1` | read/write | Action argument 1 |
| `-21` | `ADDR_TML_ARG2` | read/write | Action argument 2 |
| `-22` | `ADDR_TML_VERDICT` | read/write | Last verdict or human override |
| `-23` | `ADDR_TML_RISK` | read/write | Risk score 0..100 |
| `-24` | `ADDR_TML_LOG_HEAD` | read | Tryte derived from the current log head |
| `-25` | `ADDR_TML_UNFREEZE` | write | Unfreeze token (4242) |

The symbolic constants are defined in `mem.h`.  Standard input supplies UART
input to programs, and `ADDR_UART_STATUS` reports when a byte is ready, empty or
EOF.  A read from `ADDR_UART` returns 0 when no character is ready, so programs
should poll `ADDR_UART_STATUS` first.  The monitor uses the same input stream,
so serialized input should follow a run command such as `g\n`.

### Policy file

`--tml-policy` accepts `key=value` lines; `#` starts a comment.

```ini
system_id=ternary-emu
log=tml_memory.jsonl
tau_pause=60
tau_refuse=85
require_decision=1
taint_penalty=25
taint_freeze=0
```

`tau_pause` / `tau_refuse` are inclusive risk thresholds.  When taint is
enabled, a tainted action adds `taint_penalty` to its risk score; with
`taint_freeze=1` a tainted action at or above `tau_refuse` enters `FREEZE`.

## Analysis tools

```sh
python3 tools/tml_analyze.py tml_memory.jsonl trace.jsonl
python3 tools/tml_report.py tml_memory.jsonl -o report.md
```

`tml_analyze.py` summarizes trace events, opcodes, verdicts, risk and verifies
the Always Memory hash chain.  `tml_report.py` emits a Markdown report with a
timeline, freeze evidence and chain status.  The trace records instruction
fields and taint changes; `--symbolic` adds conditional constraints from `ADS`
and `ADSI` instructions.
