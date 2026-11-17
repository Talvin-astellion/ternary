Assembly
========

## Files
| Files      | Description                                                    |
-------------|----------------------------------------------------------------|
|tas.py      | Assembler driver for labels, `.equ`, `@(expr)` and `BRANCH()`  |
|yamp.py     | Experimental standalone macro preprocessor                     |
|assembler.inc | Instruction macros, registers, modifiers and immediate encoding |
|aliases.ymp | Optional pseudo-instructions such as `MV`, `JMP` and `SWP`     |
|examples/   | Example `.tas` programs assembled into `out/*.trt`             |
|tests/      | Encoding and emulator smoke tests                              |

The Makefile runs `tas.py`, which invokes the system C preprocessor
(`gcc -E -xc`) and automatically includes `assembler.inc` and `aliases.ymp`.
`tas.py` adds symbolic labels, `.equ` constants, `@(expr)` operands and
`BRANCH(label)` fixups on top of the macro layer.  `yamp.py` is a partial
standalone preprocessor replacement and is not used by the default build.

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
