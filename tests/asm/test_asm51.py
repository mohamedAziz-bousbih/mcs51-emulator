"""Tests for tools/asm51.py.

The encodings in INSTRUCTION_CASES are written out by hand from the opcode
tables of the Intel MCS-51 instruction set (opcode byte, then operand bytes
in instruction order), independently of the assembler's own table.
"""

from __future__ import annotations

import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "tests" / "vectors"))

import asm51  # noqa: E402
import gen_all_opcodes  # noqa: E402


def encode(line: str, org: int = 0) -> bytes:
    """Assemble one instruction placed at org."""
    program = asm51.assemble(f"        ORG   0{org:04X}h\n        {line}\n")
    return program.image(org)


def assemble(source: str) -> asm51.Program:
    return asm51.assemble(source)


def errors_of(source: str) -> list[str]:
    with pytest.raises(asm51.AsmError) as info:
        asm51.assemble(source)
    return info.value.messages


# ---------------------------------------------------------------------------
# Instruction encodings, one or more per instruction class and addressing mode
# ---------------------------------------------------------------------------

INSTRUCTION_CASES = [
    # data transfer: immediate, direct, register, register indirect
    ("MOV A,#0FFh", "74 FF"),
    ("MOV A,30h", "E5 30"),
    ("MOV A,@R0", "E6"),
    ("MOV A,R1", "E9"),
    ("MOV 30h,A", "F5 30"),
    ("MOV @R0,A", "F6"),
    ("MOV R7,A", "FF"),
    ("MOV 30h,#12h", "75 30 12"),
    ("MOV @R1,#5", "77 05"),
    ("MOV R4,#0", "7C 00"),
    ("MOV 31h,30h", "85 30 31"),      # source address is encoded first
    ("MOV 30h,@R0", "86 30"),
    ("MOV P1,R7", "8F 90"),
    ("MOV @R0,30h", "A6 30"),
    ("MOV R2,P1", "AA 90"),
    ("MOV DPTR,#1234h", "90 12 34"),
    ("MOV C,P1.7", "A2 97"),
    ("MOV P1.0,C", "92 90"),
    ("MOVC A,@A+DPTR", "93"),
    ("MOVC A,@A+PC", "83"),
    ("MOVX A,@DPTR", "E0"),
    ("MOVX A,@R0", "E2"),
    ("MOVX A,@R1", "E3"),
    ("MOVX @DPTR,A", "F0"),
    ("MOVX @R0,A", "F2"),
    ("MOVX @R1,A", "F3"),
    ("PUSH ACC", "C0 E0"),
    ("POP PSW", "D0 D0"),
    ("XCH A,30h", "C5 30"),
    ("XCH A,@R1", "C7"),
    ("XCH A,R3", "CB"),
    ("XCHD A,@R0", "D6"),
    # arithmetic
    ("ADD A,#12h", "24 12"),
    ("ADD A,30h", "25 30"),
    ("ADD A,@R1", "27"),
    ("ADD A,R2", "2A"),
    ("ADDC A,#1", "34 01"),
    ("ADDC A,B", "35 F0"),
    ("ADDC A,@R0", "36"),
    ("ADDC A,R7", "3F"),
    ("SUBB A,#1", "94 01"),
    ("SUBB A,30h", "95 30"),
    ("SUBB A,@R0", "96"),
    ("SUBB A,R0", "98"),
    ("INC A", "04"),
    ("INC 30h", "05 30"),
    ("INC @R0", "06"),
    ("INC @R1", "07"),
    ("INC R0", "08"),
    ("INC R7", "0F"),
    ("INC DPTR", "A3"),
    ("DEC A", "14"),
    ("DEC 30h", "15 30"),
    ("DEC @R1", "17"),
    ("DEC R3", "1B"),
    ("MUL AB", "A4"),
    ("DIV AB", "84"),
    ("DA A", "D4"),
    # logic
    ("ANL 30h,A", "52 30"),
    ("ANL 30h,#7Fh", "53 30 7F"),
    ("ANL A,#55h", "54 55"),
    ("ANL A,ACC", "55 E0"),
    ("ANL A,@R0", "56"),
    ("ANL A,R1", "59"),
    ("ORL 30h,A", "42 30"),
    ("ORL P1,#0Fh", "43 90 0F"),
    ("ORL A,#0F0h", "44 F0"),
    ("ORL A,30h", "45 30"),
    ("ORL A,@R1", "47"),
    ("ORL A,R5", "4D"),
    ("XRL 30h,A", "62 30"),
    ("XRL 30h,#0FFh", "63 30 FF"),
    ("XRL A,#1", "64 01"),
    ("XRL A,30h", "65 30"),
    ("XRL A,@R1", "67"),
    ("XRL A,R6", "6E"),
    ("CLR A", "E4"),
    ("CPL A", "F4"),
    ("RL A", "23"),
    ("RLC A", "33"),
    ("RR A", "03"),
    ("RRC A", "13"),
    ("SWAP A", "C4"),
    # boolean
    ("CLR C", "C3"),
    ("SETB C", "D3"),
    ("CPL C", "B3"),
    ("CLR P1.0", "C2 90"),
    ("SETB EA", "D2 AF"),
    ("CPL P1.1", "B2 91"),
    ("ANL C,P3.2", "82 B2"),
    ("ANL C,/TF0", "B0 8D"),
    ("ORL C,ACC.7", "72 E7"),
    ("ORL C,/ACC.0", "A0 E0"),
    # program branching (relative targets written with $)
    ("SJMP $", "80 FE"),
    ("SJMP $+2", "80 00"),
    ("SJMP $+129", "80 7F"),
    ("SJMP $-126", "80 80"),
    ("JC $", "40 FE"),
    ("JNC $", "50 FE"),
    ("JZ $", "60 FE"),
    ("JNZ $", "70 FE"),
    ("JB P1.0,$", "20 90 FD"),
    ("JNB TI,$", "30 99 FD"),
    ("JBC 20h.1,$", "10 01 FD"),
    ("CJNE A,#10h,$", "B4 10 FD"),
    ("CJNE A,30h,$", "B5 30 FD"),
    ("CJNE @R1,#3,$", "B7 03 FD"),
    ("CJNE R7,#0,$", "BF 00 FD"),
    ("DJNZ 30h,$", "D5 30 FD"),
    ("DJNZ R7,$", "DF FE"),
    ("LJMP 1234h", "02 12 34"),
    ("LCALL 1234h", "12 12 34"),
    ("JMP 1234h", "02 12 34"),        # generic jump/call: always the long form
    ("CALL 1234h", "12 12 34"),
    ("JMP @A+DPTR", "73"),
    ("AJMP 0123h", "21 23"),          # A10-A8 = 001 in opcode bits 7-5
    ("ACALL 0456h", "91 56"),         # A10-A8 = 100
    ("AJMP 07FFh", "E1 FF"),
    ("RET", "22"),
    ("RETI", "32"),
    ("NOP", "00"),
]


@pytest.mark.parametrize("line, expected", INSTRUCTION_CASES)
def test_instruction_encoding(line: str, expected: str) -> None:
    assert encode(line).hex(" ").upper() == expected


def test_instruction_cases_cover_every_mnemonic() -> None:
    covered = {line.split()[0] for line, _ in INSTRUCTION_CASES}
    assert covered == set(asm51.INSTRUCTIONS)


BIT_CASES = [
    ("20h.0", 0x00), ("2Fh.7", 0x7F), ("21h.2", 0x0A), ("0", 0x00), ("7Fh", 0x7F),
    ("P0.0", 0x80), ("TCON.4", 0x8C), ("P1.7", 0x97), ("SCON.1", 0x99),
    ("ACC.3", 0xE3), ("B.0", 0xF0), ("PSW.7", 0xD7),
    ("IT0", 0x88), ("IE0", 0x89), ("TR0", 0x8C), ("TF0", 0x8D), ("TF1", 0x8F),
    ("RI", 0x98), ("TI", 0x99), ("REN", 0x9C), ("SM0", 0x9F),
    ("EX0", 0xA8), ("ES", 0xAC), ("EA", 0xAF),
    ("RXD", 0xB0), ("INT0", 0xB2), ("T1", 0xB5), ("RD", 0xB7),
    ("PX0", 0xB8), ("PS", 0xBC),
    ("P", 0xD0), ("OV", 0xD2), ("RS0", 0xD3), ("RS1", 0xD4), ("F0", 0xD5), ("AC", 0xD6), ("CY", 0xD7),
]


@pytest.mark.parametrize("operand, address", BIT_CASES)
def test_bit_addresses(operand: str, address: int) -> None:
    assert encode(f"SETB {operand}") == bytes([0xD2, address])


def test_sfr_addresses() -> None:
    expected = {"P0": 0x80, "SP": 0x81, "DPL": 0x82, "DPH": 0x83, "PCON": 0x87,
                "TCON": 0x88, "TMOD": 0x89, "TL0": 0x8A, "TL1": 0x8B, "TH0": 0x8C,
                "TH1": 0x8D, "P1": 0x90, "SCON": 0x98, "SBUF": 0x99, "P2": 0xA0,
                "IE": 0xA8, "P3": 0xB0, "IP": 0xB8, "PSW": 0xD0, "ACC": 0xE0, "B": 0xF0}
    for name, address in expected.items():
        assert encode(f"MOV {name},A") == bytes([0xF5, address]), name


def test_absolute_jump_uses_page_of_next_instruction() -> None:
    # The AJMP occupies 07FEh-07FFh; the following instruction starts the
    # page 0800h-0FFFh, which is the page the target must lie in.
    assert encode("AJMP 0F23h", org=0x07FE) == bytes([0xE1, 0x23])
    messages = errors_of("        ORG 07FEh\n        AJMP 0723h\n")
    assert "2 KiB page" in messages[0]


def test_relative_range_limits() -> None:
    assert encode("SJMP $+129")[1] == 0x7F
    assert encode("SJMP $-126")[1] == 0x80
    assert "out of range" in errors_of("        SJMP $+130\n")[0]
    assert "out of range" in errors_of("        SJMP $-127\n")[0]
    # three-byte branches count from the end of the instruction
    assert encode("CJNE A,#0,$+3") == bytes([0xB4, 0x00, 0x00])


# ---------------------------------------------------------------------------
# All 256 opcodes, cross-checked with the C disassembler's test vector
# ---------------------------------------------------------------------------

VECTORS = ROOT / "tests" / "vectors"


def test_all_opcodes_vector_is_current() -> None:
    assert (VECTORS / "all_opcodes.a51").read_text() == gen_all_opcodes.generate()


def test_all_opcodes_assemble_to_every_opcode_in_order() -> None:
    program = asm51.assemble_file(VECTORS / "all_opcodes.a51")
    opcodes = [st.data[0] for st in program.statements if st.data]
    assert opcodes == list(range(256))
    for st in program.statements:
        if st.data and st.op != "DB":
            assert len(st.data) == st.size
    assert asm51.to_ihex(program) == (VECTORS / "all_opcodes.hex").read_text()


# ---------------------------------------------------------------------------
# Directives, symbols and expressions
# ---------------------------------------------------------------------------


def test_labels_forward_references_and_org() -> None:
    program = assemble(
        "        ORG   0100h\n"
        "START:  LJMP  LATER\n"
        "        SJMP  START\n"
        "LATER:  NOP\n"
        "        ORG   0200h\n"
        "TABLE:  DW    START, LATER, TABLE\n"
    )
    assert program.symbols["START"].value == 0x100
    assert program.symbols["LATER"].value == 0x105
    assert program.image(0x100, 0x106) == bytes([0x02, 0x01, 0x05, 0x80, 0xFB, 0x00])
    assert program.image(0x200, 0x206) == bytes([0x01, 0x00, 0x01, 0x05, 0x02, 0x00])
    assert program.segments() == [(0x100, program.image(0x100, 0x106)), (0x200, program.image(0x200, 0x206))]


def test_db_dw_ds_and_strings() -> None:
    program = assemble(
        "        DB    'AB', 0, 'C', -1, 'it''s', \"x,y;z\"\n"
        "        DS    3\n"
        "WORDS:  DW    1234h, 'AB'\n"
    )
    data = b"AB\x00C\xffit's" + b"x,y;z"
    assert program.image(0, len(data)) == data
    assert program.symbols["WORDS"].value == len(data) + 3
    assert program.image(len(data), len(data) + 3) == b"\xff\xff\xff"  # DS emits nothing
    assert program.image(len(data) + 3) == bytes([0x12, 0x34, 0x41, 0x42])


def test_db_expression_with_character_constants() -> None:
    # An item that starts and ends with a quote is not necessarily a string:
    # 'A' + 'B' is an expression (41h + 42h), "'0' OR 80h" too.
    program = assemble("        DB    'A' + 'B', '0' OR 80h, 'x'\n")
    assert program.image() == bytes([0x83, 0xB0, ord("x")])


def test_symbol_directives() -> None:
    program = assemble(
        "COUNT   EQU   10\n"
        "BUFFER  DATA  30h\n"
        "FLAG    BIT   20h.3\n"
        "LED     BIT   P1.0\n"
        "TABLE   CODE  0800h\n"
        "XBUF    XDATA 8000h\n"
        "STACK   IDATA 80h\n"
        "        MOV   R7, #COUNT\n"
        "        MOV   BUFFER, #COUNT*2\n"
        "        SETB  FLAG\n"
        "        CPL   LED\n"
        "        MOV   DPTR, #XBUF\n"
        "        MOV   SP, #STACK-1\n"
        "        LJMP  TABLE\n"
    )
    assert program.image(0) == bytes.fromhex("7F0A 753014 D203 B290 908000 75817F 020800")
    assert program.symbols["FLAG"].kind == "bit"
    assert program.symbols["BUFFER"].kind == "data"


@pytest.mark.parametrize("expression, value", [
    ("1 + 2 * 3", 7), ("(1 + 2) * 3", 9), ("10 MOD 3", 1), ("10 % 4", 2), ("7 / 2", 3),
    ("1 SHL 4", 16), ("80h SHR 3", 16), ("1 << 2", 4), ("0F0h AND 3Ch", 0x30),
    ("0F0h OR 0Fh", 0xFF), ("0FFh XOR 0Fh", 0xF0), ("0F0h & 3Ch | 1", 0x31),
    ("HIGH 1234h", 0x12), ("LOW(1234h)", 0x34), ("HIGH(1234h) + 1", 0x13),
    ("NOT 0 AND 0FFh", 0xFF), ("~1 & 0FFh", 0xFE), ("-(-5)", 5), ("'A' + 1", 0x42),
    ("1010b", 10), ("17o", 15), ("17q", 15), ("0x1F", 31), ("123d", 123), ("0Bh", 11),
    ("'AB'", 0x4142), ("$ + 4", 4),
])
def test_expressions(expression: str, value: int) -> None:
    program = assemble(f"X EQU {expression}\n")
    assert program.symbols["X"].value == value


def test_names_are_case_insensitive() -> None:
    assert encode("mov a, #lOw(1234H)") == bytes([0x74, 0x34])
    assert encode("setb p1.0") == bytes([0xD2, 0x90])


# ---------------------------------------------------------------------------
# Error reporting
# ---------------------------------------------------------------------------


@pytest.mark.parametrize("source, message", [
    ("        MOV A, MISSING\n", "undefined symbol 'MISSING'"),
    ("L:      NOP\nL:      NOP\n", "already defined"),
    ("A:      NOP\n", "reserved word"),
    ("        SETB P1\n", "byte address"),
    ("        INC TR0\n", "bit address"),
    ("        MOV A,#256\n", "does not fit in 8 bits"),
    ("        MOV DPTR,#10000h\n", "does not fit in 16 bits"),
    ("        SETB 20h.8\n", "out of range 0-7"),
    ("        SETB 30h.0\n", "not bit-addressable"),
    ("        FOO A\n", "unknown instruction"),
    ("        MOV @R2,A\n", "invalid indirect operand '@R2'"),
    ("        MOV A,#'x\n", "unexpected character"),
    ("        MOV A,@DPTR\n", "invalid operands"),
    ("        MOVX A,30h\n", "invalid operands"),
    ("        DB\n", "at least one item"),
    ("        MOV A,#(1\n", "missing ')'"),
    ("        MOV A,#1/0\n", "division by zero"),
    ("        NOP\n        ORG 0\n        NOP\n", "overlaps"),
    ("        ORG 10000h\n", "outside 0000h-FFFFh"),
])
def test_errors(source: str, message: str) -> None:
    messages = errors_of(source)
    assert any(message in m for m in messages), messages


def test_all_errors_are_reported_with_line_numbers() -> None:
    messages = errors_of("        NOP\n        MOV A,#300\n        SJMP NOWHERE\n")
    assert len(messages) == 2
    assert messages[0].startswith("<source>:2: error:")
    assert messages[1].startswith("<source>:3: error:")


# ---------------------------------------------------------------------------
# Intel HEX output, listing and the committed example images
# ---------------------------------------------------------------------------


def parse_ihex(text: str) -> dict[int, int]:
    memory: dict[int, int] = {}
    lines = text.splitlines()
    assert lines[-1] == ":00000001FF"
    for line in lines[:-1]:
        record = bytes.fromhex(line[1:])
        assert sum(record) & 0xFF == 0, line
        count, address, rtype = record[0], record[1] << 8 | record[2], record[3]
        assert rtype == 0 and len(record) == count + 5 and count <= 16
        for i, byte in enumerate(record[4:4 + count]):
            memory[address + i] = byte
    return memory


def test_ihex_round_trip() -> None:
    program = assemble("        ORG 0FFF0h\n        DB " + ", ".join(str(i) for i in range(16)) +
                       "\n        ORG 0\n        LJMP 0FFF0h\n")
    assert parse_ihex(asm51.to_ihex(program)) == program.memory
    assert asm51.to_ihex(assemble("")) == ":00000001FF\n"


EXAMPLES = sorted((ROOT / "examples").glob("*.a51"))


@pytest.mark.parametrize("source", EXAMPLES, ids=[p.stem for p in EXAMPLES])
def test_committed_example_hex_is_current(source: Path) -> None:
    program = asm51.assemble_file(source)
    assert asm51.to_ihex(program) == source.with_suffix(".hex").read_text()


def test_command_line(tmp_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
    source = tmp_path / "prog.a51"
    source.write_text("        ORG 0\nLOOP:   SJMP LOOP\n")
    assert asm51.main([str(source), "-l", str(tmp_path / "prog.lst")]) == 0
    assert (tmp_path / "prog.hex").read_text() == ":0200000080FE80\n:00000001FF\n"
    listing = (tmp_path / "prog.lst").read_text()
    assert "0000  80 FE" in listing and "LOOP" in listing
    assert "2 bytes" in capsys.readouterr().out

    bad = tmp_path / "bad.a51"
    bad.write_text("        SJMP NOWHERE\n")
    assert asm51.main([str(bad)]) == 1
    assert "undefined symbol" in capsys.readouterr().err
    assert not (tmp_path / "bad.hex").exists()
