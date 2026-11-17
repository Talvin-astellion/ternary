#!/usr/bin/env python3
"""Assembler encoding and emulator smoke tests."""

import os
import re
import shutil
import subprocess
import sys
import tempfile

ASMDIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REPO = os.path.dirname(ASMDIR)
EMU = os.path.join(REPO, "emu", "tern-cpu-emu")
CPU_H = os.path.join(REPO, "emu", "cpu.h")
OUTDIR = os.path.join(ASMDIR, "out")

TWO_REG = [
    "LD1", "LD2", "LD3", "ST1", "ST2", "ST3", "POP", "PSH",
    "ADCI", "ADDI", "MULI", "RTLI", "TANDI", "TORI", "TMAJI",
    "TADDI", "TMULI", "TMINI", "TMAXI",
]
THREE_REG = [
    "ADC", "ADD", "ADS", "MUL", "MULU", "RTL", "TAND", "TOR",
    "TMAJ", "TADD", "TMUL", "TMIN", "TMAX",
]

CASES = [(name, name, "P01, P02, _NOP, 0") for name in TWO_REG]
CASES += [(name, name, "P01, P02, _NOP, P03, _NOP") for name in THREE_REG]
CASES += [("ADSI", "ADSI", "P01, 0, 0")]
CASES += [("NOP", "NOP", None)]
CASES += [
    ("MV", "ADDI", "P01, P02"),
    ("MVI", "ADDI", "P01, 0"),
    ("JMP", "ADDI", "P01"),
    ("JMPI", "ADDI", "0"),
    ("NEG", "ADDI", "P01, P02"),
    ("SWP", "RTLI", "P01, P02"),
    ("BRA", "ADDI", "0"),
]


def trit_string(value):
    """Render the low 12-bit opcode field as six trits."""
    symbols = {0: "0", 1: "+", 3: "-"}
    out = []
    for shift in range(10, -1, -2):
        out.append(symbols[(value >> shift) & 3])
    return "".join(out)


def opcode_table():
    text = open(CPU_H, "r", encoding="utf-8").read()
    table = {}
    for match in re.finditer(r"OPCODE_(\w+)\s*=\s*0b([01]+)", text):
        table[match.group(1)] = trit_string(int(match.group(2), 2))
    return table


def preprocess(source_path):
    cmd = [
        "gcc", "-E", "-xc",
        "-I", ASMDIR,
        "--include", "assembler.inc",
        "--include", "aliases.ymp",
        source_path,
    ]
    result = subprocess.run(cmd, text=True, capture_output=True)
    if result.returncode != 0:
        sys.stderr.write(result.stderr)
        raise RuntimeError("preprocessor failed")
    return result.stdout


def instruction_trytes(preprocessed):
    """Return the first tryte of each assembled instruction."""
    instructions = []
    for line in preprocessed.splitlines():
        if line.startswith("#") or ";" not in line:
            continue
        parts = [part.strip() for part in line.split(";") if part.strip()]
        if len(parts) != 3:
            raise AssertionError(
                "expected 3 trytes per instruction, got %d: %r"
                % (len(parts), line)
            )
        instructions.append("".join(parts[0].split()))
    return instructions


def test_encodings():
    expected = opcode_table()
    covered = {opcode for _name, opcode, _args in CASES}
    if covered != set(expected):
        missing = sorted(set(expected) - covered)
        extra = sorted(covered - set(expected))
        raise AssertionError(
            "opcode coverage mismatch; missing=%r extra=%r" % (missing, extra)
        )
    source = "\n".join(
        ("%s;" % name) if args is None else "%s(%s);" % (name, args)
        for name, _op, args in CASES
    )
    with tempfile.NamedTemporaryFile("w", suffix=".tas", delete=False) as tmp:
        tmp.write(source + "\n")
        path = tmp.name
    try:
        text = preprocess(path)
    finally:
        os.unlink(path)

    instructions = instruction_trytes(text)
    if len(instructions) != len(CASES):
        raise AssertionError(
            "expected %d instructions, got %d" % (len(CASES), len(instructions))
        )

    failures = []
    for (name, opcode, _args), tryte in zip(CASES, instructions):
        if len(tryte) != 9 or any(ch not in "-0+" for ch in tryte):
            failures.append("%s: malformed tryte %r" % (name, tryte))
            continue
        actual = tryte[-6:]
        if actual != expected[opcode]:
            failures.append(
                "%s: opcode %s, expected %s" % (name, actual, expected[opcode])
            )

    if failures:
        raise AssertionError("\n".join(failures))


def run_emulator(program, commands):
    result = subprocess.run(
        [EMU, program],
        input=commands,
        text=True,
        capture_output=True,
        timeout=20,
    )
    output = result.stdout + result.stderr
    if result.returncode != 0:
        raise AssertionError("%s exited %d\n%s" % (program, result.returncode, output))
    if "ERROR" in output or "Unexpected" in output:
        raise AssertionError("%s reported a load/run error\n%s" % (program, output))
    return output


def assemble_source(source):
    tmpdir = tempfile.mkdtemp(prefix="tern-asm-")
    src = os.path.join(tmpdir, "test.tas")
    out = os.path.join(tmpdir, "test.trt")
    with open(src, "w", encoding="utf-8") as handle:
        handle.write(source + "\n")
    text = preprocess(src)
    with open(out, "w", encoding="utf-8") as handle:
        handle.write(text)
    return tmpdir, out


def test_runtime_macros():
    source = """--: 10-;
10-:
ADDI( P01, ZERO, _NOP, 10);
ADDI( P02, ZERO, _NOP, 1);
SWP( P01, P02);
MUL( P04, P01, _NOP, P02, _NOP);
MULI( P05, P04, _NOP, 10);
NEG( P06, P02);
MV( P07, P01);
MVI( P08, 10);
ADDI( PC, PC, _NOP, 0);
"""
    tmpdir, program = assemble_source(source)
    try:
        output = run_emulator(program, "g\nq\n")
    finally:
        shutil.rmtree(tmpdir)
    actual = {}
    for register in ("GP01", "GP02", "GP04", "GP05", "GP06", "GP07", "GP08"):
        values = register_values(output, register)
        if not values:
            raise AssertionError("unable to read %s\n%s" % (register, output))
        actual[register] = values[-1]
    expected = {
        "GP01": 1,
        "GP02": 3,
        "GP04": 3,
        "GP05": 9,
        "GP06": -3,
        "GP07": 1,
        "GP08": 3,
    }
    if actual != expected:
        raise AssertionError("runtime macro result %r, expected %r" % (actual, expected))


def register_values(output, register):
    pattern = r"%s\s+\d+\s*:\s*[^(\n]*\((-?\d+)\)" % register
    return [int(value) for value in re.findall(pattern, output)]


def test_examples():
    examples = sorted(
        name[:-4] for name in os.listdir(os.path.join(ASMDIR, "examples"))
        if name.endswith(".tas")
    )
    for name in examples:
        program = os.path.join(OUTDIR, name + ".trt")
        if not os.path.exists(program):
            raise AssertionError("missing assembled program %s" % program)
        run_emulator(program, "g\nq\n")

    fib = os.path.join(OUTDIR, "fib.trt")
    output = run_emulator(fib, "g\nq\n")
    values = register_values(output, "GP01")
    values2 = register_values(output, "GP02")
    if not values or not values2:
        raise AssertionError("unable to read fib registers\n%s" % output)
    expected = (1548008755920, 2504730781961)  # F60, F61
    actual = (values[-1], values2[-1])
    if actual != expected:
        raise AssertionError("fib result %r, expected %r" % (actual, expected))

    labels = os.path.join(OUTDIR, "labels.trt")
    output = run_emulator(labels, "g\nq\n")
    expected = {"GP01": 1, "GP02": 4, "GP03": 6, "GP04": 5, "GP05": 9}
    actual = {}
    for register in expected:
        values = register_values(output, register)
        if not values:
            raise AssertionError(
                "unable to read labels register %s\n%s" % (register, output)
            )
        actual[register] = values[-1]
    if actual != expected:
        raise AssertionError(
            "labels result %r, expected %r" % (actual, expected)
        )


def expect_tas_error(source, expected):
    tmpdir = tempfile.mkdtemp(prefix="tern-tas-error-")
    src = os.path.join(tmpdir, "error.tas")
    out = os.path.join(tmpdir, "error.trt")
    try:
        with open(src, "w", encoding="utf-8") as handle:
            handle.write(source + "\n")
        result = subprocess.run(
            [
                sys.executable, os.path.join(ASMDIR, "tas.py"),
                "--include", "assembler.inc",
                "--include", "aliases.ymp",
                src, "-o", out,
            ],
            text=True,
            capture_output=True,
            timeout=20,
        )
        output = result.stdout + result.stderr
        if result.returncode == 0:
            raise AssertionError("expected tas.py to reject: %s" % source)
        if "Traceback" in output:
            raise AssertionError("tas.py crashed instead of reporting an error\n%s" % output)
        if expected not in output:
            raise AssertionError("expected %r in tas.py output\n%s" % (expected, output))
    finally:
        shutil.rmtree(tmpdir)


def test_tas_errors():
    expect_tas_error(
        """.equ A, MISSING + 1
ADDI( P01, ZERO, _NOP, @(A));
ADDI( PC, PC, _NOP, 0);
""",
        "undefined symbol 'MISSING'",
    )
    expect_tas_error(
        """.equ A, B + 1
.equ B, A + 1
ADDI( P01, ZERO, _NOP, @(A));
ADDI( PC, PC, _NOP, 0);
""",
        "constant cycle involving",
    )
    expect_tas_error(
        """BRANCH(missing);
ADDI( PC, PC, _NOP, 0);
""",
        "undefined branch target 'missing'",
    )
    expect_tas_error(
        """x:
x:
ADDI( PC, PC, _NOP, 0);
""",
        "duplicate label 'x'",
    )


def main():
    test_encodings()
    test_runtime_macros()
    test_examples()
    test_tas_errors()
    print("assembler encoding and emulator smoke tests passed")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (AssertionError, RuntimeError, subprocess.TimeoutExpired) as exc:
        print("FAIL: %s" % exc, file=sys.stderr)
        sys.exit(1)
