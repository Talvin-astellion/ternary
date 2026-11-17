#!/usr/bin/env python3
"""Two-pass assembler driver for Ternary Assembly (.tas).

The existing C-preprocessor macros still encode instructions.  This driver adds
symbolic labels, .equ constants, @(expr) operands and relative BRANCH(label)
fixups on top of that macro layer.
"""

import argparse
import os
import re
import shlex
import subprocess
import sys
import tempfile
from dataclasses import dataclass

ASMDIR = os.path.dirname(os.path.abspath(__file__))
TRIT_CHARS = set("nN0zZ1pP+-_=!")
TRIT_VALUE = {
    "n": -1, "N": -1, "-": -1, "_": -1,
    "0": 0, "z": 0, "Z": 0,
    "1": 1, "p": 1, "P": 1, "+": 1, "=": 1, "!": 1,
}
IDENT_RE = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
EQU_RE = re.compile(r"^\s*\.equ\s+([A-Za-z_][A-Za-z0-9_]*)\s*,\s*(.*?)\s*$")
MAX_IMMEDIATE = (3 ** 8 - 1) // 2

LABEL_PREFIX = "__tas_label_"
BRANCH_PREFIX = "__tas_branch_"
EXPR_PREFIX = "__tas_expr_"
UNRESOLVED_BRANCH_PREFIX = "__tas_ubranch_"
UNRESOLVED_EXPR_PREFIX = "__tas_uexpr_"


class AsmError(Exception):
    def __init__(self, path, line, message):
        super().__init__(message)
        self.path = path
        self.line = line
        self.message = message

    def __str__(self):
        if self.line > 0:
            return "%s:%d: error: %s" % (self.path, self.line, self.message)
        return "%s: error: %s" % (self.path, self.message)


class UnresolvedError(Exception):
    pass


@dataclass
class Occurrence:
    kind: str
    start: int
    end: int
    line: int
    data: object
    index: int


@dataclass
class SourceModel:
    base: str
    occurrences: list
    labels: list
    label_lines: list
    constants: dict
    constant_lines: dict


def strip_comments(text):
    out = []
    i = 0
    block = False
    while i < len(text):
        if block:
            end = text.find("*/", i)
            if end < 0:
                break
            i = end + 2
            block = False
        elif text.startswith("//", i):
            break
        elif text.startswith("/*", i):
            block = True
            i += 2
        else:
            out.append(text[i])
            i += 1
    return "".join(out)


def find_matching(text, open_pos, opening, closing):
    depth = 0
    for pos in range(open_pos, len(text)):
        char = text[pos]
        if char == opening:
            depth += 1
        elif char == closing:
            depth -= 1
            if depth == 0:
                return pos
    return -1


def prepare_source(text, path):
    out = []
    out_len = 0
    occurrences = []
    labels = []
    label_lines = []
    label_index = {}
    constants = {}
    constants_lines = {}

    def append(value):
        nonlocal out_len
        out.append(value)
        out_len += len(value)

    def add_constant(raw, line):
        clean = strip_comments(raw)
        match = EQU_RE.match(clean)
        if not match:
            raise AsmError(path, line, "expected .equ NAME, expression")
        name = match.group(1)
        if name in constants:
            raise AsmError(path, line, "duplicate constant '%s'" % name)
        constants[name] = match.group(2)
        constants_lines[name] = line

    i = 0
    line = 1
    at_line_start = True
    in_block = False
    in_line = False
    in_directive = False

    while i < len(text):
        char = text[i]

        if char == "\n":
            append(char)
            i += 1
            line += 1
            in_line = False
            if in_directive:
                back = i - 2
                if back >= 0 and text[back] == "\r":
                    back -= 1
                continued = back >= 0 and text[back] == "\\"
                in_directive = continued
                at_line_start = not continued
            else:
                at_line_start = True
            continue

        if in_line:
            append(char)
            i += 1
            continue

        if in_block:
            if text.startswith("*/", i):
                append("*/")
                i += 2
                in_block = False
            else:
                append(char)
                i += 1
            continue

        if in_directive:
            append(char)
            i += 1
            continue

        if at_line_start:
            if char in " \t\r":
                append(char)
                i += 1
                continue
            if char == "#":
                in_directive = True
                append(char)
                i += 1
                at_line_start = False
                continue
            if text.startswith(".equ", i):
                after = i + 4
                if after == len(text) or not (text[after].isalnum() or text[after] == "_"):
                    end = text.find("\n", i)
                    if end < 0:
                        end = len(text)
                    add_constant(text[i:end], line)
                    i = end
                    at_line_start = False
                    continue
            at_line_start = False

        if text.startswith("//", i):
            in_line = True
            append("//")
            i += 2
            continue

        if text.startswith("/*", i):
            in_block = True
            append("/*")
            i += 2
            continue

        previous_is_word = i > 0 and (text[i - 1].isalnum() or text[i - 1] == "_")
        if (char.isalpha() or char == "_") and not previous_is_word:
            match = IDENT_RE.match(text, i)
            if match:
                name = match.group(0)
                end = match.end()
                after = end
                while after < len(text) and text[after] in " \t":
                    after += 1
                if after < len(text) and text[after] == ":":
                    if is_trit_token(name):
                        append(name)
                        i = end
                        continue
                    if name in label_index:
                        raise AsmError(path, line, "duplicate label '%s'" % name)
                    index = len(labels)
                    labels.append(name)
                    label_lines.append(line)
                    label_index[name] = index
                    append("%s%d" % (LABEL_PREFIX, index))
                    i = end
                    continue
                if name == "BRANCH":
                    if after < len(text) and text[after] == "(":
                        close = find_matching(text, after, "(", ")")
                        if close < 0:
                            raise AsmError(path, line, "unclosed BRANCH(")
                        label = text[after + 1:close].strip()
                        if not IDENT_RE.fullmatch(label):
                            raise AsmError(path, line, "BRANCH target must be a label")
                        index = len(occurrences)
                        start = out_len
                        append("%s%d__" % (BRANCH_PREFIX, index))
                        occurrences.append(Occurrence(
                            "branch", start, out_len, line, label, index))
                        i = close + 1
                        continue
                append(name)
                i = end
                continue

        if char == "@" and i + 1 < len(text) and text[i + 1] == "(":
            close = find_matching(text, i + 1, "(", ")")
            if close < 0:
                raise AsmError(path, line, "unclosed @(")
            expression = text[i + 2:close]
            index = len(occurrences)
            start = out_len
            append("%s%d__" % (EXPR_PREFIX, index))
            occurrences.append(Occurrence(
                "expr", start, out_len, line, expression, index))
            i = close + 1
            continue

        append(char)
        i += 1

    for name, constant_line in constants_lines.items():
        if name in label_index:
            raise AsmError(path, constant_line,
                           "constant '%s' conflicts with a label" % name)

    return SourceModel("".join(out), occurrences, labels, label_lines,
                       constants, constants_lines)


def balanced_to_int(value):
    result = 0
    for char in value:
        if char not in TRIT_VALUE:
            raise ValueError(char)
        result = result * 3 + TRIT_VALUE[char]
    return result


def int_to_balanced(value):
    if value == 0:
        return "0"
    digits = []
    while value:
        value, remainder = divmod(value, 3)
        if remainder == 2:
            remainder = -1
            value += 1
        digits.append({-1: "n", 0: "0", 1: "1"}[remainder])
    return "".join(reversed(digits))


def is_trit_token(token):
    return bool(token) and all(char in TRIT_CHARS for char in token)


def render(model, mode, expr_values=None, branch_offsets=None):
    parts = []
    last = 0
    for occurrence in model.occurrences:
        parts.append(model.base[last:occurrence.start])
        if occurrence.kind == "expr":
            if mode == 1:
                parts.append("0")
            else:
                value = (expr_values.get(occurrence.index)
                         if expr_values is not None else None)
                if value is None:
                    parts.append("%s%d__" % (UNRESOLVED_EXPR_PREFIX, occurrence.index))
                else:
                    parts.append(int_to_balanced(value))
        else:
            if mode == 1:
                parts.append("%s%d__;0;0" % (BRANCH_PREFIX, occurrence.index))
            else:
                offset = branch_offsets.get(occurrence.index) if branch_offsets else None
                if offset is None:
                    parts.append("%s%d__" % (UNRESOLVED_BRANCH_PREFIX, occurrence.index))
                else:
                    parts.append("ADDI( PC, PC, _NOP, %s )" % int_to_balanced(offset))
        last = occurrence.end
    parts.append(model.base[last:])
    return "".join(parts)


def run_preprocessor(source, pp, include_dirs, includes, defines, path):
    command = shlex.split(pp)
    for directory in [ASMDIR] + include_dirs:
        command.extend(["-I", directory])
    for include in includes:
        command.extend(["--include", include])
    for define in defines:
        command.append("-D" + define)
    command.append("-")
    escaped_path = path.replace("\\", "\\\\").replace('"', '\\"')
    source = '#line 1 "%s"\n%s' % (escaped_path, source)
    result = subprocess.run(
        command,
        input=source,
        text=True,
        capture_output=True,
        cwd=ASMDIR,
    )
    if result.returncode != 0:
        message = result.stderr.strip() or "preprocessor failed"
        raise AsmError(path, 0, message)
    return result.stdout


def parse_preprocessed(text, model, path):
    label_address = {}
    branch_address = {}
    address = 0
    current_file = path
    current_line = 0

    for raw_line in text.splitlines():
        line = raw_line.rstrip("\r")
        stripped = line.lstrip()
        if stripped.startswith("#"):
            match = re.match(r'#\s*(?:line\s+)?(\d+)\s*(?:"([^"]*)")?', stripped)
            if match:
                current_line = int(match.group(1))
                current_file = match.group(2) or current_file
            continue
        if not stripped or stripped.startswith(".equ"):
            continue

        accum = ""
        for char in line:
            if char.isspace():
                continue
            if char == ":":
                token = accum
                accum = ""
                match = re.fullmatch(re.escape(LABEL_PREFIX) + r"(\d+)", token)
                if match:
                    index = int(match.group(1))
                    name = model.labels[index]
                    if name in label_address:
                        raise AsmError(path, model.label_lines[index],
                                       "duplicate label '%s'" % name)
                    label_address[name] = address
                elif is_trit_token(token):
                    address = balanced_to_int(token)
                else:
                    raise AsmError(current_file, current_line,
                                   "invalid address or label '%s'" % token)
                continue
            if char == ";":
                token = accum
                accum = ""
                match = re.fullmatch(re.escape(BRANCH_PREFIX) + r"(\d+)__", token)
                if match:
                    index = int(match.group(1))
                    branch_address[index] = address
                elif not is_trit_token(token):
                    raise AsmError(current_file, current_line,
                                   "invalid tryte '%s'" % token)
                address += 1
                continue
            accum += char

        if accum.strip():
            raise AsmError(current_file, current_line,
                           "unexpected text '%s'" % accum.strip())

    return label_address, branch_address


def trunc_div(left, right, path, line):
    if right == 0:
        raise AsmError(path, line, "division by zero")
    quotient = abs(left) // abs(right)
    return -quotient if (left < 0) != (right < 0) else quotient


class ExpressionParser:
    def __init__(self, text, path, line, resolver, known=()):
        self.text = text
        self.path = path
        self.line = line
        self.resolver = resolver
        self.known = set(known)
        self.pos = 0
        self.token = None
        self.next()

    def error(self, message):
        raise AsmError(self.path, self.line, message)

    def next(self):
        while self.pos < len(self.text) and self.text[self.pos].isspace():
            self.pos += 1
        if self.pos >= len(self.text):
            self.token = ("eof", None)
            return
        char = self.text[self.pos]
        if char in "+-*/%()":
            self.token = (char, char)
            self.pos += 1
            return
        if self.text.startswith("0t", self.pos):
            start = self.pos
            self.pos += 2
            while self.pos < len(self.text) and self.text[self.pos] in "nN0zZ1pP":
                self.pos += 1
            self.token = ("number", self.text[start:self.pos])
            return
        if char.isalpha() or char == "_":
            match = IDENT_RE.match(self.text, self.pos)
            word = match.group(0)
            if word in self.known or not all(ch in TRIT_CHARS for ch in word):
                self.token = ("ident", word)
            else:
                self.token = ("number", word)
            self.pos = match.end()
            return
        if char in "nN0zZ1pP":
            start = self.pos
            while self.pos < len(self.text) and self.text[self.pos] in "nN0zZ1pP":
                self.pos += 1
            self.token = ("number", self.text[start:self.pos])
            return
        self.error("unexpected character '%s'" % char)

    def expect(self, kind):
        if self.token[0] != kind:
            self.error("expected '%s'" % kind)
        token = self.token
        self.next()
        return token

    def parse(self):
        value = self.parse_sum()
        if self.token[0] != "eof":
            self.error("unexpected token '%s'" % self.token[1])
        return value

    def parse_sum(self):
        value = self.parse_product()
        while self.token[0] in ("+", "-"):
            operator = self.token[0]
            self.next()
            right = self.parse_product()
            value = value + right if operator == "+" else value - right
        return value

    def parse_product(self):
        value = self.parse_unary()
        while self.token[0] in ("*", "/", "%"):
            operator = self.token[0]
            self.next()
            right = self.parse_unary()
            if operator == "*":
                value *= right
            elif operator == "/":
                value = trunc_div(value, right, self.path, self.line)
            else:
                value -= trunc_div(value, right, self.path, self.line) * right
        return value

    def parse_unary(self):
        if self.token[0] == "+":
            self.next()
            return self.parse_unary()
        if self.token[0] == "-":
            self.next()
            return -self.parse_unary()
        return self.parse_primary()

    def parse_primary(self):
        if self.token[0] == "number":
            token = self.token[1]
            self.next()
            if token.startswith("0t"):
                token = token[2:]
            return balanced_to_int(token)
        if self.token[0] == "ident":
            name = self.token[1]
            self.next()
            return self.resolver(name)
        if self.token[0] == "(":
            self.next()
            value = self.parse_sum()
            self.expect(")")
            return value
        self.error("expected an expression")


def evaluate_expressions(model, label_address, path):
    constant_values = {}
    constant_state = {}
    constant_errors = {}
    known = set(model.constants) | set(label_address)

    def resolve_constant(name, line):
        if name in label_address:
            return label_address[name]
        if name not in model.constants:
            raise UnresolvedError("undefined symbol '%s'" % name)
        if name in constant_values:
            if name in constant_errors:
                raise UnresolvedError(constant_errors[name])
            return constant_values[name]
        if constant_state.get(name) == "evaluating":
            raise AsmError(path, line, "constant cycle involving '%s'" % name)
        constant_state[name] = "evaluating"
        try:
            value = ExpressionParser(
                model.constants[name], path, line,
                lambda symbol: resolve_constant(symbol, line), known).parse()
        except UnresolvedError as error:
            constant_errors[name] = str(error)
            constant_values[name] = None
            raise
        finally:
            constant_state.pop(name, None)
        constant_values[name] = value
        return value

    for name, expression in model.constants.items():
        line = model.constant_lines.get(name, 0)
        try:
            ExpressionParser(
                expression, path, line,
                lambda symbol: resolve_constant(symbol, line), known).parse()
        except UnresolvedError:
            pass

    values = {}
    errors = {}
    for occurrence in model.occurrences:
        if occurrence.kind != "expr":
            continue
        try:
            values[occurrence.index] = ExpressionParser(
                occurrence.data, path, occurrence.line,
                lambda symbol: resolve_constant(symbol, occurrence.line),
                known).parse()
        except UnresolvedError as error:
            values[occurrence.index] = None
            errors[occurrence.index] = str(error)

    return values, errors


def finalize_output(text, model, path, expr_errors=None):
    text = re.sub(re.escape(LABEL_PREFIX) + r"\d+\s*:", "", text)

    unresolved = re.search(
        r"(%s|%s)(\d+)__" % (re.escape(UNRESOLVED_BRANCH_PREFIX),
                             re.escape(UNRESOLVED_EXPR_PREFIX)), text)
    if unresolved:
        prefix, raw_index = unresolved.groups()
        index = int(raw_index)
        if prefix == UNRESOLVED_BRANCH_PREFIX:
            occurrence = model.occurrences[index]
            raise AsmError(path, occurrence.line,
                           "undefined branch target '%s'" % occurrence.data)
        occurrence = model.occurrences[index]
        message = "undefined expression in '@(%s)'" % occurrence.data
        reason = expr_errors.get(index) if expr_errors else None
        if reason:
            message += ": " + reason
        raise AsmError(path, occurrence.line, message)

    if re.search(r"__tas_(label|branch|expr|ubranch|uexpr)_", text):
        raise AsmError(path, 0, "unresolved assembler placeholder")

    address = 0
    line_number = 0
    for raw_line in text.splitlines():
        line_number += 1
        line = raw_line.rstrip("\r")
        if line.lstrip().startswith("#"):
            continue
        accum = ""
        for char in line:
            if char.isspace():
                continue
            if char == ":":
                if not is_trit_token(accum):
                    raise AsmError(path, line_number,
                                   "invalid address '%s'" % accum)
                address = balanced_to_int(accum)
                accum = ""
            elif char == ";":
                if not is_trit_token(accum) or len(accum) > 9:
                    raise AsmError(path, line_number,
                                   "invalid tryte '%s'" % accum)
                address += 1
                accum = ""
            elif char in TRIT_CHARS:
                accum += char
            else:
                raise AsmError(path, line_number,
                               "invalid character '%s' in output" % char)
        if accum:
            raise AsmError(path, line_number,
                           "line ends with incomplete tryte '%s'" % accum)

    return text


def write_output(text, output):
    if output == "-":
        sys.stdout.write(text)
        return
    directory = os.path.dirname(os.path.abspath(output))
    os.makedirs(directory, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=".tas-", dir=directory)
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as handle:
            handle.write(text)
        os.replace(temporary, output)
    except Exception:
        try:
            os.unlink(temporary)
        except OSError:
            pass
        raise


def assemble(path, output, pp, include_dirs, includes, defines):
    try:
        with open(path, "r", encoding="utf-8") as handle:
            source = handle.read()
    except OSError as error:
        raise AsmError(path, 0, str(error))

    model = prepare_source(source, path)
    pass1_source = render(model, 1)
    pass1_text = run_preprocessor(
        pass1_source, pp, include_dirs, includes, defines, path)
    label_address, branch_address = parse_preprocessed(pass1_text, model, path)

    expr_values, expr_errors = evaluate_expressions(model, label_address, path)

    branch_offsets = {}
    for occurrence in model.occurrences:
        if occurrence.kind != "branch":
            continue
        target = occurrence.data
        if target not in label_address or occurrence.index not in branch_address:
            continue
        offset = label_address[target] - (branch_address[occurrence.index] + 3)
        if abs(offset) > MAX_IMMEDIATE:
            raise AsmError(path, occurrence.line,
                           "branch offset does not fit in an immediate")
        branch_offsets[occurrence.index] = offset

    pass2_source = render(model, 2, expr_values, branch_offsets)
    pass2_text = run_preprocessor(
        pass2_source, pp, include_dirs, includes, defines, path)
    final_text = finalize_output(pass2_text, model, path, expr_errors)
    write_output(final_text, output)


def main(argv=None):
    parser = argparse.ArgumentParser(description="Ternary assembler driver")
    parser.add_argument("input", help="input .tas file")
    parser.add_argument("-o", "--output", default="-", help="output .trt file")
    parser.add_argument("--pp", default="gcc -E -xc", help="preprocessor command")
    parser.add_argument("-I", "--include-dir", action="append", default=[],
                        help="additional include directory")
    parser.add_argument("--include", action="append", default=[],
                        help="include file before the source")
    parser.add_argument("-D", "--define", action="append", default=[],
                        help="preprocessor definition")
    args = parser.parse_args(argv)

    try:
        assemble(args.input, args.output, args.pp, args.include_dir,
                 args.include, args.define)
    except AsmError as error:
        print(error, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
