Assembly
========

## Files
| Files      | Description                                                    |
-------------|----------------------------------------------------------------|
|tas.py      | Assembler driver for labels, `.equ`, `@(expr)`, data directives, `BRANCH()` and `CALL()` |
|yamp.py     | Experimental standalone macro preprocessor                     |
|assembler.inc | Instruction macros, registers, modifiers and immediate encoding |
|aliases.ymp | Optional pseudo-instructions such as `MV`, `JMP` and `SWP`     |
|lib/        | Reusable `.tas` include files such as `io.tas` and `math.tas`  |
|examples/   | Example `.tas` programs assembled into `out/*.trt`             |
|serial_load.py | Serializer for streaming a `.trt` image into the loader     |
|tests/      | Encoding and emulator smoke tests                              |

The Makefile runs `tas.py`, which invokes the system C preprocessor
(`gcc -E -xc`) and automatically includes `assembler.inc` and `aliases.ymp`.
`tas.py` adds symbolic labels, `.equ` constants, `@(expr)` operands,
`BRANCH(label)` fixups and `CALL(label)` fixups on top of the macro layer.
`#include "lib/io.tas"` is resolved recursively before the C preprocessor runs.
`yamp.py` is a partial standalone preprocessor replacement and is not used by
the default build.

## Build and test

```sh
make -C asm              # assemble asm/examples/*.tas into asm/out/*.trt
make -C asm test         # assemble and run encoding + emulator smoke tests
make -C asm clean        # remove asm/out
```

Run an assembled program with the emulator:

```sh
./emu/tern-cpu-emu asm/out/fib.trt
```

## Labels and expressions

`tas.py` performs two preprocessor passes.  The first pass measures labels and
branch locations; the second pass substitutes final addresses and expressions.

```asm
.equ COUNT, 10              // balanced ternary 3
.equ LIMIT, COUNT + 1       // 4

start:
ADDI( P01, ZERO, _NOP, 1);
BRANCH(done);               // skip the next instruction
ADDI( P01, ZERO, _NOP, 1n);
done:
ADDI( P02, ZERO, _NOP, @(LIMIT));
ADDI( P03, ZERO, _NOP, @(done - start));
```

- `name:` defines a label at the current tryte address.  Names made only of
  trit characters are still treated as numeric addresses (`nn:` is -4).
- `.equ NAME, expr` defines a constant.  Expressions use balanced ternary
  literals (`10`, `1n`, optional `0t` prefix), labels, parentheses and
  `+ - * / %`.
- `@(expr)` substitutes the value as a balanced ternary immediate.
- `BRANCH(label)` emits an unconditional relative branch.  The target must fit
  in the eight-trit ADDI immediate.
- `CALL(label)` pushes the return address on the stack and jumps to `label`.
  Use `RET` to return; `IO_INIT` sets the stack pointer.

## Data

Data directives emit trytes at the current address and work with the same
expressions and labels as `@(expr)`.

```asm
message:
.string "Hello, ternary!\n"  // characters plus a terminating zero
.tryte COUNT                 // one tryte
.triword 0                   // three zero trytes
.triword 1, 1, 1             // explicit low, middle and high trytes
```

`.string` uses the emulator's character encoding (`ascii - 0x1f`, with NUL kept
at zero).  `asm/examples/hello.tas` shows a complete program that prints the
string through `ADDR_UART` and then halts.

## I/O library

`asm/lib/io.tas` provides small UART routines:

| Routine | Arguments / result |
| --- | --- |
| `IO_INIT` | Initializes the stack pointer; call once before using `CALL`/`RET` |
| `IO_PUTC` | `P01` = character tryte to write |
| `IO_PUTS` | `P01` = address of a zero-terminated string; leaves `P01` at the terminator |
| `IO_GETC` | Returns a character in `P01` and status in `P02` (+1 ready, 0 empty, -1 EOF) |
| `IO_READ_TRYTE` | Reads nine UART trit bytes and returns one tryte in `P01` |

Include it with `#include "lib/io.tas"` and call routines with `CALL(label)`.

## Math library

`asm/lib/math.tas` provides integer helpers for nonnegative inputs:

| Routine | Arguments / result |
| --- | --- |
| `MATH_MOD` | `P01 = P01 % P02`; requires `P01 >= 0` and `P02 > 0` |
| `MATH_GCD` | `P01 = gcd(P01, P02)`; requires both arguments nonnegative |
| `MATH_MODPOW` | `P01 = P01^P02 mod P03`; requires `P02 >= 0` and `P03 > 1` |

All three routines return the result in `P01` and clobber scratch registers.
`MATH_GCD` and `MATH_MODPOW` use `CALL`/`RET`, so run `IO_INIT` once first.
Intermediate products must fit in the emulator's 27-trit word.
Include the library with `#include "lib/math.tas"`; see
`asm/examples/gcd.tas` and `asm/examples/modpow.tas`.

## Serial loader

`asm/examples/loader.tas` receives a serialized image over `ADDR_UART`, stores
it in RAM and jumps to the image start address.  `asm/serial_load.py` emits the
stream from a `.trt` image:

```sh
python3 asm/serial_load.py asm/out/hello.trt -o /tmp/hello.bin
printf 'g\n' > /tmp/loader-input
cat /tmp/hello.bin >> /tmp/loader-input
./emu/tern-cpu-emu asm/out/loader.trt < /tmp/loader-input
```

The first three trytes are the start address, the next three are the load base
address, and the remaining trytes are stored consecutively from that base.  Each
tryte is sent as nine bytes in the range `0x1e..0x20` (`-1/0/+1` trits).  EOF on
stdin terminates loading and transfers control to the loaded image.

Direct invocation is also supported:

```sh
python3 asm/tas.py --include assembler.inc --include aliases.ymp \
    asm/examples/labels.tas -o /tmp/labels.trt
```

## Code

The assemby code is formated to work well with an extended C-style preprocessor.
This allows pseudo instruction aliases to be easily introduced and seemlessly
integrated.


Instructions fall into one of 3 forms, matching the bit ordering options.
```
    INST1( R1, immediate );
    INST2( R1, R2+MOD, immediate );
    INST3( R1, R2+MOD, R3+MOD );
```
Multiple instructions can be placed on a single line using the semicolons as
seperators.

C-style comments are supported. being stripped out by the preprocessor.
The preprocessor supports a bunch of #directives and defines.


| Directive        | Description                                              |
|------------------|----------------------------------------------------------|
| `#include`       | Used to include other files contents                     |
| `#define`        | Defines a macro substitution object-like or function-like|
| `#undef`         | Removes a macro definition                               |
| `#if`            | Allows conditional inclusion of parts of file            |
| `#ifdef`         |                                                          |
| `#ifndef`        |                                                          |
| `#elif`          |                                                          |
| `#else`          |                                                          |
| `#endif`         |                                                          |
| `#pragma`        |                                                          |
| `#line`          | Sets line number for debugging / analysis purposes       |
| `#error`         | Emits an error and terminates if processed               |
| `#warning`       | Emits a warning and carries on if processed              |
| `#assert`        | Emits an error and terminates if clause is false         |
| `#for in`        | Allows repititive inclusion of parts of file             |
| `#endfor`        |                                                          |
| `#enum`          | Allows C-style enum to be treated as macros              |
| `#`              |                                                          |
| `##`             | Concatenate tokens                                       |
| `...`            | Variadic arguments to macro                              |
| `__FILE__`       |                                                          |
| `__LINE__`       |                                                          |
| `__DATE__`       |                                                          |
| `__TIME__`       |                                                          |
| `__TIMESTAMP__`  |                                                          |
| `SPLIT()`        | Splits token into multiple single character tokens       |
| `MERGE()`        | Merges list of tokens into 1 token                       |
| `defined()`      | true, false if is defined preprocessor symbol            |
| `//`             | Comment to end of line                                   |
| `/*`             | Begin block comment                                      |
| `*/`             | End block comment                                        |

| CLI              | Description                                              |
|------------------|----------------------------------------------------------|
| `-D`             | Add define at command line                               |
| `-I`             | Add include path at command line                         |
| `--include`      | Add include file at command line                         |
