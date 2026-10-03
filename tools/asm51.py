#!/usr/bin/env python3
"""asm51 - a two-pass assembler for the Intel MCS-51 (8051).

Usage:
    python tools/asm51.py program.a51 [-o program.hex] [-l program.lst]

Source syntax (a subset of Intel ASM51):

    [label:] [mnemonic [operand {, operand}]] [; comment]

    name EQU expr      numeric constant
    name DATA expr     direct address (internal RAM 00h-7Fh or SFR 80h-FFh)
    name IDATA expr    indirectly addressed internal RAM (constant)
    name XDATA expr    external data memory address (constant)
    name CODE expr     program memory address
    name BIT bit       bit address: a number, a bit name or byte.bit
    ORG expr           set the location counter
    DB item {, item}   bytes; an item is an expression or a quoted string
    DW expr {, expr}   16-bit words, high byte first (the 8051 byte order)
    DS expr            reserve bytes without emitting code
    END                end of the source text (optional)

Operands: A, AB, C, DPTR, @DPTR, @A+DPTR, @A+PC, R0-R7, @R0, @R1, #expr,
direct/bit/code-address expressions, byte.bit (P1.0, ACC.7, 20h.3) and
/bit (complemented bit for ANL/ORL C). All SFRs and SFR bits of the 8051
are predefined (P0-P3, SP, DPL, DPH, PCON, TCON, TMOD, TL0, TL1, TH0, TH1,
SCON, SBUF, IE, IP, PSW, ACC, B; IT0 ... TF1, RI ... SM0, EX0 ... EA,
PX0 ... PS, RXD ... RD, P, OV, RS0, RS1, F0, AC, CY).

Numbers: 123, 123d, 0FFh, 0x1F, 1010b, 17o, 17q, 'A' and 'AB' (16 bit).
Operators, from high to low precedence:
    unary + - NOT ~ HIGH LOW;  * / MOD % SHL SHR << >>;  + -;
    AND &;  XOR ^;  OR |
$ is the address of the current instruction. Names are case-insensitive.

The generic JMP/CALL with an address operand assemble as LJMP/LCALL, so
their size never depends on the distance to the target.

Symbols carry a type: a bare DATA symbol is rejected where a bit is
expected and a bare BIT symbol where a byte address is expected (SETB P1 is
an error, SETB P1.0 is meant).
"""

from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

__all__ = ["AsmError", "Program", "assemble", "assemble_file", "to_ihex", "main"]


class AsmError(Exception):
    """Assembly failed; str(error) lists every message as file:line: text."""

    def __init__(self, messages: list[str]):
        super().__init__("\n".join(messages))
        self.messages = messages


class _LineError(Exception):
    pass


# --------------------------------------------------------------------------
# Predefined symbols
# --------------------------------------------------------------------------

SFRS = {
    "P0": 0x80, "SP": 0x81, "DPL": 0x82, "DPH": 0x83, "PCON": 0x87,
    "TCON": 0x88, "TMOD": 0x89, "TL0": 0x8A, "TL1": 0x8B, "TH0": 0x8C,
    "TH1": 0x8D, "P1": 0x90, "SCON": 0x98, "SBUF": 0x99, "P2": 0xA0,
    "IE": 0xA8, "P3": 0xB0, "IP": 0xB8, "PSW": 0xD0, "ACC": 0xE0, "B": 0xF0,
}


def _bits(base: int, names: str) -> dict[str, int]:
    return {name: base + i for i, name in enumerate(names.split()) if name != "-"}


SFR_BITS = {
    **_bits(0x88, "IT0 IE0 IT1 IE1 TR0 TF0 TR1 TF1"),
    **_bits(0x98, "RI TI RB8 TB8 REN SM2 SM1 SM0"),
    **_bits(0xA8, "EX0 ET0 EX1 ET1 ES - - EA"),
    **_bits(0xB0, "RXD TXD INT0 INT1 T0 T1 WR RD"),
    **_bits(0xB8, "PX0 PT0 PX1 PT1 PS"),
    **_bits(0xD0, "P - OV RS0 RS1 F0 AC CY"),
}

RESERVED = {
    "A", "AB", "C", "DPTR", "PC", "R0", "R1", "R2", "R3", "R4", "R5", "R6", "R7",
    "AND", "OR", "XOR", "NOT", "MOD", "SHL", "SHR", "HIGH", "LOW",
}

SYMBOL_DIRECTIVES = {"EQU": "num", "DATA": "data", "IDATA": "num",
                     "XDATA": "num", "CODE": "code", "BIT": "bit"}
DIRECTIVES = {"ORG", "DB", "DW", "DS", "END", *SYMBOL_DIRECTIVES}

# --------------------------------------------------------------------------
# Instruction set: mnemonic -> [(operand pattern, opcode)]
#
# Pattern elements: A AB C DPTR @DPTR @A+DPTR @A+PC (fixed operands),
# @Ri Rn (register number added to the opcode), # (8-bit immediate),
# #16 (16-bit immediate), dir, bit, /bit, rel (8-bit PC-relative),
# a11 (AJMP/ACALL page address), a16 (16-bit code address).
# --------------------------------------------------------------------------


def _alu(base: int) -> list:
    return [(("A", "#"), base + 4), (("A", "dir"), base + 5),
            (("A", "@Ri"), base + 6), (("A", "Rn"), base + 8)]


def _logic(base: int) -> list:
    return [(("dir", "A"), base + 2), (("dir", "#"), base + 3)] + _alu(base)


INSTRUCTIONS: dict[str, list[tuple[tuple[str, ...], int]]] = {
    "NOP": [((), 0x00)],
    "AJMP": [(("a11",), 0x01)],
    "LJMP": [(("a16",), 0x02)],
    "RR": [(("A",), 0x03)],
    "INC": [(("A",), 0x04), (("dir",), 0x05), (("@Ri",), 0x06), (("Rn",), 0x08),
            (("DPTR",), 0xA3)],
    "JBC": [(("bit", "rel"), 0x10)],
    "ACALL": [(("a11",), 0x11)],
    "LCALL": [(("a16",), 0x12)],
    "RRC": [(("A",), 0x13)],
    "DEC": [(("A",), 0x14), (("dir",), 0x15), (("@Ri",), 0x16), (("Rn",), 0x18)],
    "JB": [(("bit", "rel"), 0x20)],
    "RET": [((), 0x22)],
    "RL": [(("A",), 0x23)],
    "ADD": _alu(0x20),
    "JNB": [(("bit", "rel"), 0x30)],
    "RETI": [((), 0x32)],
    "RLC": [(("A",), 0x33)],
    "ADDC": _alu(0x30),
    "JC": [(("rel",), 0x40)],
    "ORL": _logic(0x40) + [(("C", "bit"), 0x72), (("C", "/bit"), 0xA0)],
    "JNC": [(("rel",), 0x50)],
    "ANL": _logic(0x50) + [(("C", "bit"), 0x82), (("C", "/bit"), 0xB0)],
    "JZ": [(("rel",), 0x60)],
    "XRL": _logic(0x60),
    "JNZ": [(("rel",), 0x70)],
    "JMP": [(("@A+DPTR",), 0x73), (("a16",), 0x02)],
    "CALL": [(("a16",), 0x12)],
    "MOV": [
        (("A", "#"), 0x74), (("dir", "#"), 0x75), (("@Ri", "#"), 0x76), (("Rn", "#"), 0x78),
        (("dir", "dir"), 0x85), (("dir", "@Ri"), 0x86), (("dir", "Rn"), 0x88),
        (("DPTR", "#16"), 0x90), (("bit", "C"), 0x92), (("C", "bit"), 0xA2),
        (("@Ri", "dir"), 0xA6), (("Rn", "dir"), 0xA8),
        (("A", "dir"), 0xE5), (("A", "@Ri"), 0xE6), (("A", "Rn"), 0xE8),
        (("dir", "A"), 0xF5), (("@Ri", "A"), 0xF6), (("Rn", "A"), 0xF8),
    ],
    "SJMP": [(("rel",), 0x80)],
    "MOVC": [(("A", "@A+PC"), 0x83), (("A", "@A+DPTR"), 0x93)],
    "DIV": [(("AB",), 0x84)],
    "SUBB": _alu(0x90),
    "MUL": [(("AB",), 0xA4)],
    "CPL": [(("A",), 0xF4), (("C",), 0xB3), (("bit",), 0xB2)],
    "CJNE": [(("A", "#", "rel"), 0xB4), (("A", "dir", "rel"), 0xB5),
             (("@Ri", "#", "rel"), 0xB6), (("Rn", "#", "rel"), 0xB8)],
    "PUSH": [(("dir",), 0xC0)],
    "CLR": [(("A",), 0xE4), (("C",), 0xC3), (("bit",), 0xC2)],
    "SWAP": [(("A",), 0xC4)],
    "XCH": [(("A", "dir"), 0xC5), (("A", "@Ri"), 0xC6), (("A", "Rn"), 0xC8)],
    "POP": [(("dir",), 0xD0)],
    "SETB": [(("C",), 0xD3), (("bit",), 0xD2)],
    "DA": [(("A",), 0xD4)],
    "DJNZ": [(("dir", "rel"), 0xD5), (("Rn", "rel"), 0xD8)],
    "XCHD": [(("A", "@Ri"), 0xD6)],
    "MOVX": [(("A", "@DPTR"), 0xE0), (("A", "@Ri"), 0xE2),
             (("@DPTR", "A"), 0xF0), (("@Ri", "A"), 0xF2)],
}

OPERAND_SIZE = {"#": 1, "#16": 2, "dir": 1, "bit": 1, "/bit": 1, "rel": 1, "a11": 1, "a16": 2}
FIXED_OPERANDS = {"A", "AB", "C", "DPTR", "@DPTR", "@A+DPTR", "@A+PC"}

# --------------------------------------------------------------------------
# Expressions
# --------------------------------------------------------------------------

_TOKEN = re.compile(r"""
    \s*(?:
        (?P<num>[0-9][0-9A-Za-z]*)
      | (?P<str>'(?:[^']|'')*'|"(?:[^"]|"")*")
      | (?P<id>[A-Za-z_?][A-Za-z0-9_?]*)
      | (?P<op><<|>>|[-+*/%()&|^~$])
    )""", re.X)

_BINARY_LEVELS = [
    {"OR": "|", "|": "|"},
    {"XOR": "^", "^": "^"},
    {"AND": "&", "&": "&"},
    {"+": "+", "-": "-"},
    {"*": "*", "/": "/", "MOD": "%", "%": "%", "SHL": "<<", "SHR": ">>", "<<": "<<", ">>": ">>"},
]
_UNARY = {"+": "+", "-": "-", "NOT": "~", "~": "~", "HIGH": "HIGH", "LOW": "LOW"}


def parse_number(text: str) -> int:
    t = text.upper()
    try:
        if t.startswith("0X"):
            return int(t[2:], 16)
        if t.endswith("H"):
            return int(t[:-1], 16)
        if t.endswith("B") and t[:-1] and set(t[:-1]) <= {"0", "1"}:
            return int(t[:-1], 2)
        if t.endswith(("O", "Q")):
            return int(t[:-1], 8)
        if t.endswith("D") and t[:-1].isdigit():
            return int(t[:-1], 10)
        if t.isdigit():
            return int(t, 10)
    except ValueError:
        pass
    raise _LineError(f"invalid number '{text}'")


def _unquote(token: str) -> str:
    quote = token[0]
    return token[1:-1].replace(quote * 2, quote)


def _tokenize(text: str) -> list[tuple[str, str]]:
    tokens, pos = [], 0
    text = text.rstrip()
    while pos < len(text):
        m = _TOKEN.match(text, pos)
        if not m or m.end() == pos:
            raise _LineError(f"unexpected character in expression: '{text[pos:].strip()}'")
        kind = m.lastgroup
        value = m.group(kind)
        tokens.append((kind, value.upper() if kind == "id" else value))
        pos = m.end()
    return tokens


class _ExprParser:
    def __init__(self, text: str):
        self.text = text
        self.tokens = _tokenize(text)
        self.pos = 0

    def parse(self):
        if not self.tokens:
            raise _LineError("missing expression")
        node = self._binary(0)
        if self.pos != len(self.tokens):
            raise _LineError(f"unexpected '{self.tokens[self.pos][1]}' in expression '{self.text.strip()}'")
        return node

    def _peek(self):
        return self.tokens[self.pos] if self.pos < len(self.tokens) else (None, None)

    def _binary(self, level: int):
        if level == len(_BINARY_LEVELS):
            return self._unary()
        node = self._binary(level + 1)
        while True:
            kind, value = self._peek()
            if kind in ("op", "id") and value in _BINARY_LEVELS[level]:
                self.pos += 1
                node = ("bin", _BINARY_LEVELS[level][value], node, self._binary(level + 1))
            else:
                return node

    def _unary(self):
        kind, value = self._peek()
        if kind in ("op", "id") and value in _UNARY:
            self.pos += 1
            return ("un", _UNARY[value], self._unary())
        return self._primary()

    def _primary(self):
        kind, value = self._peek()
        if kind is None:
            raise _LineError(f"incomplete expression '{self.text.strip()}'")
        self.pos += 1
        if kind == "num":
            return ("num", parse_number(value))
        if kind == "str":
            chars = _unquote(value)
            if not 1 <= len(chars) <= 2:
                raise _LineError(f"character constant {value} must have 1 or 2 characters")
            number = 0
            for ch in chars:
                number = number << 8 | _char_code(ch)
            return ("num", number)
        if kind == "id":
            if value in RESERVED:
                raise _LineError(f"'{value}' cannot be used in an expression")
            return ("sym", value)
        if value == "$":
            return ("pc",)
        if value == "(":
            node = self._binary(0)
            if self._peek()[1] != ")":
                raise _LineError(f"missing ')' in '{self.text.strip()}'")
            self.pos += 1
            return node
        raise _LineError(f"unexpected '{value}' in expression '{self.text.strip()}'")


def _char_code(ch: str) -> int:
    code = ord(ch)
    if code > 0xFF:
        raise _LineError(f"character '{ch}' is not an 8-bit character")
    return code


def parse_expression(text: str):
    return _ExprParser(text).parse()


def _split_outside_quotes(text: str, sep: str) -> list[str]:
    """Split at sep characters that are not inside quotes or parentheses."""
    parts, current, quote, depth = [], [], None, 0
    for ch in text:
        if quote:
            if ch == quote:
                quote = None
        elif ch in "'\"":
            quote = ch
        elif ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
        elif ch == sep and depth == 0:
            parts.append("".join(current))
            current = []
            continue
        current.append(ch)
    parts.append("".join(current))
    return parts


def _strip_comment(line: str) -> str:
    quote = None
    for i, ch in enumerate(line):
        if quote:
            if ch == quote:
                quote = None
        elif ch in "'\"":
            quote = ch
        elif ch == ";":
            return line[:i]
    return line


# --------------------------------------------------------------------------
# Operands
# --------------------------------------------------------------------------


@dataclass
class Operand:
    kind: str           # fixed name, '@Ri', 'Rn', '#', '/bit', 'expr' or 'bitdot'
    value: object = None
    text: str = ""


def parse_operand(text: str) -> Operand:
    raw = text.strip()
    if not raw:
        raise _LineError("empty operand")
    compact = re.sub(r"\s+", "", raw).upper()
    if compact in FIXED_OPERANDS:
        return Operand(compact, text=raw)
    if re.fullmatch(r"@R[01]", compact):
        return Operand("@Ri", int(compact[2]), raw)
    if re.fullmatch(r"R[0-7]", compact):
        return Operand("Rn", int(compact[1]), raw)
    if compact.startswith("@"):
        raise _LineError(f"invalid indirect operand '{raw}' (use @R0, @R1, @DPTR, @A+DPTR or @A+PC)")
    if raw.startswith("#"):
        return Operand("#", parse_expression(raw[1:]), raw)
    if raw.startswith("/"):
        inner = parse_operand(raw[1:])
        if inner.kind not in ("expr", "bitdot"):
            raise _LineError(f"invalid bit operand '{raw}'")
        return Operand("/bit", inner, raw)
    halves = _split_outside_quotes(raw, ".")
    if len(halves) == 2:
        return Operand("bitdot", (parse_expression(halves[0]), parse_expression(halves[1])), raw)
    if len(halves) > 2:
        raise _LineError(f"invalid operand '{raw}'")
    return Operand("expr", parse_expression(raw), raw)


def _matches(pattern: tuple[str, ...], operands: list[Operand]) -> bool:
    if len(pattern) != len(operands):
        return False
    for slot, op in zip(pattern, operands):
        if slot in FIXED_OPERANDS or slot in ("@Ri", "Rn", "/bit"):
            ok = op.kind == slot
        elif slot in ("#", "#16"):
            ok = op.kind == "#"
        elif slot == "bit":
            ok = op.kind in ("expr", "bitdot")
        else:  # dir, rel, a11, a16
            ok = op.kind == "expr"
        if not ok:
            return False
    return True


# --------------------------------------------------------------------------
# Assembler
# --------------------------------------------------------------------------


@dataclass
class Symbol:
    value: int
    kind: str           # 'num', 'data', 'bit', 'code'
    line: int = 0       # 0 for predefined symbols


@dataclass
class Statement:
    line: int
    source: str
    label: str | None = None
    name: str | None = None         # symbol defined by EQU/DATA/BIT/...
    op: str | None = None
    operands: list[str] = field(default_factory=list)
    address: int = 0
    size: int = 0
    pattern: tuple[str, ...] = ()
    opcode: int = 0
    parsed: list[Operand] = field(default_factory=list)
    data: bytes = b""


@dataclass
class Program:
    memory: dict[int, int]
    symbols: dict[str, Symbol]
    statements: list[Statement]
    filename: str

    def segments(self) -> list[tuple[int, bytes]]:
        """Contiguous (start address, bytes) runs, sorted by address."""
        runs: list[tuple[int, bytearray]] = []
        for addr in sorted(self.memory):
            if runs and runs[-1][0] + len(runs[-1][1]) == addr:
                runs[-1][1].append(self.memory[addr])
            else:
                runs.append((addr, bytearray([self.memory[addr]])))
        return [(start, bytes(data)) for start, data in runs]

    def image(self, start: int = 0, end: int | None = None, fill: int = 0xFF) -> bytes:
        if end is None:
            end = max(self.memory) + 1 if self.memory else start
        return bytes(self.memory.get(a, fill) for a in range(start, end))


_LABEL = re.compile(r"\s*([A-Za-z_?][A-Za-z0-9_?]*)\s*:(?!=)")
_WORD = re.compile(r"\s*([A-Za-z_?][A-Za-z0-9_?]*)")


class _Assembler:
    def __init__(self, filename: str):
        self.filename = filename
        self.errors: list[str] = []
        self.symbols: dict[str, Symbol] = {}
        for name, addr in SFRS.items():
            self.symbols[name] = Symbol(addr, "data")
        for name, addr in SFR_BITS.items():
            self.symbols[name] = Symbol(addr, "bit")

    def error(self, line: int, message: str) -> None:
        self.errors.append(f"{self.filename}:{line}: error: {message}")

    def check(self) -> None:
        if self.errors:
            raise AsmError(self.errors)

    # -- parsing ------------------------------------------------------------

    def parse_line(self, number: int, source: str) -> Statement:
        st = Statement(number, source.rstrip("\r\n"))
        text = _strip_comment(st.source)
        m = _LABEL.match(text)
        if m:
            st.label = m.group(1).upper()
            text = text[m.end():]
        if not text.strip():
            return st
        m = _WORD.match(text)
        if not m:
            raise _LineError(f"cannot parse '{text.strip()}'")
        first = m.group(1).upper()
        rest = text[m.end():]
        m2 = _WORD.match(rest)
        if m2 and m2.group(1).upper() in SYMBOL_DIRECTIVES and st.label is None:
            st.name = first
            st.op = m2.group(1).upper()
            rest = rest[m2.end():]
        else:
            st.op = first
        rest = rest.strip()
        st.operands = [part.strip() for part in _split_outside_quotes(rest, ",")] if rest else []
        return st

    # -- symbols ------------------------------------------------------------

    def define(self, line: int, name: str, value: int, kind: str) -> None:
        if name in RESERVED or name in INSTRUCTIONS or name in DIRECTIVES:
            raise _LineError(f"'{name}' is a reserved word")
        old = self.symbols.get(name)
        if old is not None:
            where = f"line {old.line}" if old.line else "predefined"
            raise _LineError(f"symbol '{name}' already defined ({where})")
        self.symbols[name] = Symbol(value, kind, line)

    def evaluate(self, node, pc: int) -> tuple[int, str | None]:
        """Value of an expression and, for a bare symbol, its type."""
        tag = node[0]
        if tag == "num":
            return node[1], None
        if tag == "pc":
            return pc, None
        if tag == "sym":
            sym = self.symbols.get(node[1])
            if sym is None:
                raise _LineError(f"undefined symbol '{node[1]}'")
            return sym.value, sym.kind
        if tag == "un":
            value, _ = self.evaluate(node[2], pc)
            op = node[1]
            if op == "-":
                return -value, None
            if op == "~":
                return ~value, None
            if op == "HIGH":
                return (value >> 8) & 0xFF, None
            if op == "LOW":
                return value & 0xFF, None
            return value, None
        left, _ = self.evaluate(node[2], pc)
        right, _ = self.evaluate(node[3], pc)
        op = node[1]
        if op in ("/", "%") and right == 0:
            raise _LineError("division by zero")
        if op in ("<<", ">>") and right < 0:
            raise _LineError("negative shift count")
        return {
            "+": lambda: left + right, "-": lambda: left - right,
            "*": lambda: left * right, "/": lambda: _div(left, right),
            "%": lambda: left - _div(left, right) * right,
            "<<": lambda: left << right, ">>": lambda: left >> right,
            "&": lambda: left & right, "|": lambda: left | right, "^": lambda: left ^ right,
        }[op](), None

    def value(self, text_or_node, pc: int) -> int:
        node = parse_expression(text_or_node) if isinstance(text_or_node, str) else text_or_node
        return self.evaluate(node, pc)[0]

    def bit_address(self, op: Operand, pc: int) -> int:
        if op.kind == "bitdot":
            base_node, bit_node = op.value
            base, kind = self.evaluate(base_node, pc)
            if kind == "bit":
                raise _LineError(f"'{op.text}': the part before '.' must be a byte address")
            bit = self.value(bit_node, pc)
            if not 0 <= bit <= 7:
                raise _LineError(f"bit number {bit} out of range 0-7 in '{op.text}'")
            if 0x20 <= base <= 0x2F:
                return (base - 0x20) * 8 + bit
            if 0x80 <= base <= 0xFF and base % 8 == 0:
                return base + bit
            raise _LineError(f"byte {base:02X}h in '{op.text}' is not bit-addressable")
        value, kind = self.evaluate(op.value, pc)
        if kind == "data":
            raise _LineError(f"'{op.text}' is a byte address; name a bit (for example {op.text}.0)")
        if not 0 <= value <= 0xFF:
            raise _LineError(f"bit address {value} out of range 0-255")
        return value

    # -- pass 1: addresses and labels --------------------------------------

    def pass1(self, statements: list[Statement]) -> None:
        pc = 0
        for st in statements:
            try:
                if st.op == "ORG":
                    self._expect_operands(st, 1)
                    pc = self.value(st.operands[0], pc)
                    if not 0 <= pc <= 0xFFFF:
                        raise _LineError(f"ORG address {pc:X}h outside 0000h-FFFFh")
                st.address = pc
                if st.label:
                    self.define(st.line, st.label, pc, "code")
                if st.op is None or st.op == "ORG":
                    continue
                if st.op == "END":
                    self._expect_operands(st, 0)
                    break
                if st.name is not None:
                    self._define_directive(st, pc)
                    continue
                st.size = self._size(st, pc)
                pc += st.size
                if pc > 0x10000:
                    raise _LineError("code beyond the end of the 64 KiB program memory")
            except _LineError as e:
                self.error(st.line, str(e))

    def _expect_operands(self, st: Statement, count: int) -> None:
        if len(st.operands) != count:
            raise _LineError(f"{st.op} expects {count} operand{'s' if count != 1 else ''}")

    def _define_directive(self, st: Statement, pc: int) -> None:
        self._expect_operands(st, 1)
        kind = SYMBOL_DIRECTIVES[st.op]
        if st.op == "BIT":
            value = self.bit_address(parse_operand(st.operands[0]), pc)
        else:
            value = self.value(st.operands[0], pc)
            if st.op == "DATA" and not 0 <= value <= 0xFF:
                raise _LineError(f"DATA address {value} out of range 00h-FFh")
        self.define(st.line, st.name, value, kind)

    def _size(self, st: Statement, pc: int) -> int:
        if st.op == "DB":
            if not st.operands:
                raise _LineError("DB needs at least one item")
            return sum(len(_unquote(item)) if _is_string(item) else 1 for item in st.operands)
        if st.op == "DW":
            if not st.operands:
                raise _LineError("DW needs at least one item")
            return 2 * len(st.operands)
        if st.op == "DS":
            self._expect_operands(st, 1)
            count = self.value(st.operands[0], pc)
            if count < 0:
                raise _LineError("DS count must not be negative")
            return count
        forms = INSTRUCTIONS.get(st.op)
        if forms is None:
            raise _LineError(f"unknown instruction or directive '{st.op}'")
        st.parsed = [parse_operand(text) for text in st.operands]
        for pattern, opcode in forms:
            if _matches(pattern, st.parsed):
                st.pattern, st.opcode = pattern, opcode
                return 1 + sum(OPERAND_SIZE.get(slot, 0) for slot in pattern)
        raise _LineError(f"invalid operands for {st.op}: {', '.join(st.operands) or '(none)'}")

    # -- pass 2: code generation --------------------------------------------

    def pass2(self, statements: list[Statement]) -> dict[int, int]:
        memory: dict[int, int] = {}
        owner: dict[int, int] = {}
        for st in statements:
            if st.op == "END":
                break
            if st.size == 0 or st.op == "DS":
                continue
            try:
                st.data = self._encode(st)
            except _LineError as e:
                self.error(st.line, str(e))
                continue
            for i, byte in enumerate(st.data):
                addr = st.address + i
                if addr in memory:
                    self.error(st.line, f"code at {addr:04X}h overlaps line {owner[addr]}")
                    break
                memory[addr] = byte
                owner[addr] = st.line
        return memory

    def _encode(self, st: Statement) -> bytes:
        pc = st.address
        if st.op == "DB":
            out = bytearray()
            for item in st.operands:
                if _is_string(item):
                    out.extend(_char_code(ch) for ch in _unquote(item))
                else:
                    out.append(_byte(self.value(item, pc), "DB value"))
            return bytes(out)
        if st.op == "DW":
            out = bytearray()
            for item in st.operands:
                word = _word(self.value(item, pc), "DW value")
                out += bytes([word >> 8, word & 0xFF])
            return bytes(out)

        opcode = st.opcode
        operand_bytes = bytearray()
        next_pc = pc + st.size
        for slot, op in zip(st.pattern, st.parsed):
            if slot in ("@Ri", "Rn"):
                opcode += op.value
            elif slot == "#":
                operand_bytes.append(_byte(self.value(op.value, pc), "immediate value"))
            elif slot == "#16":
                word = _word(self.value(op.value, pc), "16-bit immediate")
                operand_bytes += bytes([word >> 8, word & 0xFF])
            elif slot == "dir":
                value, kind = self.evaluate(op.value, pc)
                if kind == "bit":
                    raise _LineError(f"'{op.text}' is a bit address, a byte address is required")
                if not 0 <= value <= 0xFF:
                    raise _LineError(f"direct address {value} out of range 00h-FFh")
                operand_bytes.append(value)
            elif slot == "bit":
                operand_bytes.append(self.bit_address(op, pc))
            elif slot == "/bit":
                operand_bytes.append(self.bit_address(op.value, pc))
            elif slot == "rel":
                target = self.value(op.value, pc)
                offset = target - next_pc
                if not -128 <= offset <= 127:
                    raise _LineError(f"relative jump to {target & 0xFFFF:04X}h out of range "
                                     f"({offset:+d} bytes, allowed -128..+127)")
                operand_bytes.append(offset & 0xFF)
            elif slot == "a11":
                target = self.value(op.value, pc)
                if not 0 <= target <= 0xFFFF or (target & 0xF800) != (next_pc & 0xF800):
                    raise _LineError(f"{st.op} target {target & 0xFFFF:04X}h is not in the 2 KiB "
                                     f"page of the next instruction ({next_pc & 0xF800:04X}h)")
                opcode |= (target >> 3) & 0xE0
                operand_bytes.append(target & 0xFF)
            elif slot == "a16":
                target = self.value(op.value, pc)
                if not 0 <= target <= 0xFFFF:
                    raise _LineError(f"code address {target} out of range")
                operand_bytes += bytes([target >> 8, target & 0xFF])
        if st.opcode == 0x85:  # MOV dest,src is encoded as 85h src dest
            operand_bytes = operand_bytes[1:2] + operand_bytes[0:1]
        return bytes([opcode]) + bytes(operand_bytes)


def _div(left: int, right: int) -> int:
    """Integer division truncating toward zero, as in ASM51 and C."""
    quotient = abs(left) // abs(right)
    return quotient if (left < 0) == (right < 0) else -quotient


_STRING = re.compile(r"'(?:[^']|'')*'|\"(?:[^\"]|\"\")*\"")


def _is_string(item: str) -> bool:
    """True for a DB item that is exactly one quoted string ('A' + 1 is not)."""
    return _STRING.fullmatch(item.strip()) is not None


def _byte(value: int, what: str) -> int:
    if not -128 <= value <= 255:
        raise _LineError(f"{what} {value} does not fit in 8 bits")
    return value & 0xFF


def _word(value: int, what: str) -> int:
    if not -32768 <= value <= 0xFFFF:
        raise _LineError(f"{what} {value} does not fit in 16 bits")
    return value & 0xFFFF


def assemble(source: str, filename: str = "<source>") -> Program:
    """Assemble source text; raises AsmError listing every error found."""
    asm = _Assembler(filename)
    statements = []
    for number, line in enumerate(source.splitlines(), start=1):
        try:
            statements.append(asm.parse_line(number, line))
        except _LineError as e:
            asm.error(number, str(e))
    asm.check()
    asm.pass1(statements)
    asm.check()
    memory = asm.pass2(statements)
    asm.check()
    return Program(memory, asm.symbols, statements, filename)


def assemble_file(path: str | Path) -> Program:
    path = Path(path)
    return assemble(path.read_text(encoding="utf-8"), str(path))


# --------------------------------------------------------------------------
# Output
# --------------------------------------------------------------------------


def _ihex_record(address: int, rtype: int, data: bytes) -> str:
    record = bytes([len(data), address >> 8, address & 0xFF, rtype]) + data
    checksum = (-sum(record)) & 0xFF
    return ":" + (record + bytes([checksum])).hex().upper()


def to_ihex(program: Program, record_size: int = 16) -> str:
    lines = []
    for start, data in program.segments():
        for offset in range(0, len(data), record_size):
            lines.append(_ihex_record(start + offset, 0x00, data[offset:offset + record_size]))
    lines.append(_ihex_record(0, 0x01, b""))
    return "\n".join(lines) + "\n"


def to_listing(program: Program) -> str:
    out = [f"asm51 listing of {program.filename}", "",
           " LOC  OBJ               LINE  SOURCE"]
    for st in program.statements:
        data = st.data
        loc = f"{st.address:04X}" if (st.op or st.label) and st.name is None else "    "
        if st.name is not None:
            sym = program.symbols.get(st.name)
            loc = f"={sym.value:04X}" if sym else "    "
        out.append(f"{loc:5} {data[:6].hex(' ').upper():17} {st.line:5}  {st.source}".rstrip())
        for offset in range(6, len(data), 6):
            out.append(f"{st.address + offset:04X}  {data[offset:offset + 6].hex(' ').upper()}")
    user = {n: s for n, s in program.symbols.items() if s.line}
    if user:
        out += ["", "SYMBOLS", f"{'NAME':20} {'TYPE':5} VALUE"]
        for name in sorted(user):
            sym = user[name]
            out.append(f"{name:20} {sym.kind.upper():5} {sym.value & 0xFFFF:04X}h")
    return "\n".join(out) + "\n"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Two-pass assembler for the Intel MCS-51 (8051).")
    parser.add_argument("source", help="assembly source file (.a51)")
    parser.add_argument("-o", "--output", help="Intel HEX output file (default: source name with .hex)")
    parser.add_argument("-l", "--listing", help="also write a listing with addresses and code bytes")
    args = parser.parse_args(argv)

    try:
        program = assemble_file(args.source)
    except AsmError as e:
        print(e, file=sys.stderr)
        return 1
    except OSError as e:
        print(f"asm51: {e}", file=sys.stderr)
        return 1

    output = Path(args.output) if args.output else Path(args.source).with_suffix(".hex")
    output.write_text(to_ihex(program), encoding="ascii", newline="\n")
    if args.listing:
        Path(args.listing).write_text(to_listing(program), encoding="utf-8", newline="\n")
    size = len(program.memory)
    span = f", {min(program.memory):04X}h-{max(program.memory):04X}h" if size else ""
    print(f"{args.source}: {size} bytes{span} -> {output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
