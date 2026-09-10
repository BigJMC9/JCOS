#!/usr/bin/env python3

"""JCOS C/header formatter.

House style: readability-first K&R, a 120-column target, one ordinary
executable statement per line, compact single-statement control branches,
compact signatures when they fit, and predictable multiline wrapping for long
conditions/parameters.
Selective function and line-range formatting leaves unselected text untouched.
"""

import argparse
import re
from pathlib import Path


# JCOS readability-first style limits. 120 columns is the normal target;
# 140 is reserved as a hard ceiling for constructs that cannot be split safely
# (for example a single long string literal or hardware constant declaration).
LINE_LENGTH_TARGET = 120
LINE_LENGTH_HARD_LIMIT = 140
# Wrapped continuation lines aim slightly below the normal limit so grouped
# expressions keep some visual breathing room instead of hugging column 120.
WRAP_LINE_TARGET = 108
COMPACT_LINE_LIMIT = LINE_LENGTH_TARGET
COMMENT_LINE_LIMIT = LINE_LENGTH_TARGET


def visual_indent(line: str) -> int:
    prefix = line[:len(line) - len(line.lstrip())]
    return len(prefix.expandtabs(8))


def strip_trailing_block_comments(text: str) -> str:
    """
    foo(); /* comment */

    becomes, for checking purposes:

    foo();
    """
    text = text.rstrip()

    while text.endswith("*/"):
        start = text.rfind("/*")

        if start == -1:
            break

        text = text[:start].rstrip()

    return text


def count_top_level_semicolons(text: str) -> int:
    """Count semicolons outside strings/comments and nested (), [] pairs."""

    count = 0
    paren_depth = 0
    bracket_depth = 0
    state = "normal"
    i = 0

    while i < len(text):
        ch = text[i]
        nxt = text[i + 1] if i + 1 < len(text) else ""

        if state == "normal":
            if ch == '"':
                state = "string"
            elif ch == "'":
                state = "char"
            elif ch == "/" and nxt == "*":
                state = "comment"
                i += 1
            elif ch == "(":
                paren_depth += 1
            elif ch == ")":
                paren_depth = max(0, paren_depth - 1)
            elif ch == "[":
                bracket_depth += 1
            elif ch == "]":
                bracket_depth = max(0, bracket_depth - 1)
            elif ch == ";" and paren_depth == 0 and bracket_depth == 0:
                count += 1

        elif state == "string":
            if ch == "\\":
                i += 1
            elif ch == '"':
                state = "normal"

        elif state == "char":
            if ch == "\\":
                i += 1
            elif ch == "'":
                state = "normal"

        elif state == "comment":
            if ch == "*" and nxt == "/":
                state = "normal"
                i += 1

        i += 1

    return count


def is_single_statement(text: str) -> bool:
    """
    Return True for things such as:

        foo();
        x = 10;
        return;
        return foo();

    Be conservative about declarations and control statements.
    """

    code = strip_trailing_block_comments(text.strip())

    if not code:
        return False

    if not code.endswith(";"):
        return False

    if count_top_level_semicolons(code) != 1:
        return False

    if "{" in code or "}" in code:
        return False

    if code.startswith("#"):
        return False

    # Don't flatten nested control structures.
    blocked = (
        "if ",
        "if(",
        "for ",
        "for(",
        "while ",
        "while(",
        "switch ",
        "switch(",
        "else",
        "do ",
    )

    if code.startswith(blocked):
        return False

    # Don't turn:
    #
    # if (x) {
    #     int y = 10;
    # }
    #
    # into:
    #
    # if (x) int y = 10;
    #
    # because declarations can't be used directly as the
    # body of an if without braces.
    declarations = (
        "auto ",
        "char ",
        "const ",
        "double ",
        "enum ",
        "extern ",
        "float ",
        "int ",
        "long ",
        "register ",
        "short ",
        "signed ",
        "static ",
        "struct ",
        "typedef ",
        "union ",
        "unsigned ",
        "void ",
        "volatile ",
        "_Bool ",
        "_Atomic ",
    )

    if code.startswith(declarations):
        return False

    return True


def is_asm_statement_start(line: str) -> bool:
    """Return True when a physical line starts a GNU-style asm statement."""

    stripped = line.lstrip()

    for keyword in ("__asm__", "__asm", "asm"):
        if not stripped.startswith(keyword):
            continue

        end = len(keyword)

        if end < len(stripped) and (
            stripped[end].isalnum()
            or stripped[end] == "_"
        ):
            continue

        return True

    return False


def protect_asm_blocks(text: str) -> tuple[str, dict[str, str]]:
    """
    Replace GNU-style asm statements with inert marker comments before any
    formatting pass runs. The exact original asm text is restored afterward.

    This deliberately protects both basic and extended asm forms, including
    qualifiers such as `volatile`, `inline`, and `goto`::

        __asm__ volatile (
            "mfence"
            :
            :
            : "memory"
        );

    The formatter therefore cannot collapse the parentheses, rewrite comments,
    pack lines, or otherwise disturb operand/clobber alignment inside asm.
    """

    lines = text.splitlines(keepends=True)
    result = []
    preserved: dict[str, str] = {}
    i = 0

    while i < len(lines):
        line = lines[i]

        if not is_asm_statement_start(line):
            result.append(line)
            i += 1
            continue

        block = []
        cursor = i
        complete = False

        while cursor < len(lines):
            block.append(lines[cursor])
            candidate = "".join(block)

            if is_single_statement(candidate):
                complete = True
                cursor += 1
                break

            cursor += 1

        # If the asm statement is malformed/incomplete, leave it alone here
        # rather than swallowing the rest of the file into a marker.
        if not complete:
            result.append(line)
            i += 1
            continue

        block_text = "".join(block)
        indent = line[:len(line) - len(line.lstrip())]
        marker = f"/*__CODE_FORMAT_PRESERVED_ASM_{len(preserved):04d}__*/"

        # Keep the marker on exactly one physical line. Its indentation remains
        # outside the replacement so restoring the marker reproduces the first
        # asm line at the same indentation level.
        if block_text.endswith("\r\n"):
            line_ending = "\r\n"
            body = block_text[len(indent):-2]
        elif block_text.endswith("\n") or block_text.endswith("\r"):
            line_ending = block_text[-1]
            body = block_text[len(indent):-1]
        else:
            line_ending = ""
            body = block_text[len(indent):]

        preserved[marker] = body
        result.append(indent + marker + line_ending)
        i = cursor

    return "".join(result), preserved


def restore_asm_blocks(text: str, preserved: dict[str, str]) -> str:
    """Restore exact asm statement text previously hidden by protect_asm_blocks."""

    for marker, original in preserved.items():
        text = text.replace(marker, original)

    return text


def wrap_long_standalone_block_comments(text: str) -> str:
    """Wrap overlong standalone one-line block comments into normal prose blocks."""

    result: list[str] = []

    for line in text.splitlines():
        stripped = line.strip()
        if (
            len(line.expandtabs(4)) <= COMMENT_LINE_LIMIT
            or not stripped.startswith("/*")
            or not stripped.endswith("*/")
            or stripped == "/**/"
        ):
            result.append(line)
            continue

        indent = line[:len(line) - len(line.lstrip())]
        body = stripped[2:-2].strip()
        if not body or "/*" in body or "*/" in body:
            result.append(line)
            continue

        words = body.split()
        wrapped: list[str] = []
        current = ""
        width = max(40, COMMENT_LINE_LIMIT - len((indent + " * ").expandtabs(4)))

        for word in words:
            candidate = word if not current else current + " " + word
            if current and len(candidate) > width:
                wrapped.append(current)
                current = word
            else:
                current = candidate

        if current:
            wrapped.append(current)

        if not wrapped:
            result.append(line)
            continue

        result.append(indent + "/*")
        result.extend(indent + " * " + part for part in wrapped)
        result.append(indent + " */")

    ending = "\r\n" if "\r\n" in text else "\n"
    joined = ending.join(result)
    if text.endswith(("\n", "\r")):
        joined += ending
    return joined


def protect_multiline_block_comments(text: str) -> tuple[str, dict[str, str]]:
    """
    Hide remaining standalone multiline block comments from line-oriented
    formatting passes.

    Simple prose comments have already had a chance to collapse to one line
    before this function runs. Anything still written as::

        /*
         * ...
         */

    is treated as intentionally structured and replaced with a preprocessor-like
    marker. This is important because comment body text can contain tokens such
    as '=', '(', ')', or ';' that would otherwise look like C code to passes
    that operate one physical line at a time.
    """

    lines = text.splitlines(keepends=True)
    result = []
    preserved: dict[str, str] = {}
    i = 0

    while i < len(lines):
        line = lines[i]

        # Only protect standalone multiline block comments. Inline one-line
        # comments remain available to the normal formatter.
        if line.strip() != "/*":
            result.append(line)
            i += 1
            continue

        block = [line]
        cursor = i + 1
        complete = False

        while cursor < len(lines):
            block.append(lines[cursor])

            if lines[cursor].strip() == "*/":
                complete = True
                cursor += 1
                break

            cursor += 1

        if not complete:
            result.append(line)
            i += 1
            continue

        block_text = "".join(block)
        indent = line[:len(line) - len(line.lstrip())]
        marker = f"#__CODE_FORMAT_PRESERVED_COMMENT_{len(preserved):04d}__"

        if block_text.endswith("\r\n"):
            line_ending = "\r\n"
            body = block_text[len(indent):-2]
        elif block_text.endswith("\n") or block_text.endswith("\r"):
            line_ending = block_text[-1]
            body = block_text[len(indent):-1]
        else:
            line_ending = ""
            body = block_text[len(indent):]

        preserved[marker] = body
        result.append(indent + marker + line_ending)
        i = cursor

    return "".join(result), preserved


def restore_multiline_block_comments(
    text: str,
    preserved: dict[str, str],
) -> str:
    """Restore multiline block comments hidden before line-based formatting."""

    for marker, original in preserved.items():
        text = text.replace(marker, original)

    return text


def find_matching_paren(
    text: str,
    open_index: int,
) -> int | None:
    """
    Find the ')' matching text[open_index] == '('.

    Handles nested parentheses and ignores parentheses inside
    strings, character literals and block comments.
    """

    depth = 0
    state = "normal"
    i = open_index

    while i < len(text):
        ch = text[i]
        nxt = text[i + 1] if i + 1 < len(text) else ""

        if state == "normal":
            if ch == '"':
                state = "string"

            elif ch == "'":
                state = "char"

            elif ch == "/" and nxt == "*":
                state = "comment"
                i += 1

            elif ch == "(":
                depth += 1

            elif ch == ")":
                depth -= 1

                if depth == 0:
                    return i

        elif state == "string":
            if ch == "\\":
                i += 1

            elif ch == '"':
                state = "normal"

        elif state == "char":
            if ch == "\\":
                i += 1

            elif ch == "'":
                state = "normal"

        elif state == "comment":
            if ch == "*" and nxt == "/":
                state = "normal"
                i += 1

        i += 1

    return None


def parse_parenthesized_header(
    text: str,
    keyword: str,
):
    """
    Parse:

        if (...)
        if (...) {
        if (...) foo();

    and likewise for `for`.
    """

    if not text.startswith(keyword):
        return None

    pos = len(keyword)

    # Avoid matching things such as "ifdef".
    if pos < len(text):
        if text[pos].isalnum() or text[pos] == "_":
            return None

    while pos < len(text) and text[pos].isspace():
        pos += 1

    if pos >= len(text) or text[pos] != "(":
        return None

    close = find_matching_paren(text, pos)

    if close is None:
        return None

    header = text[:close + 1]
    remainder = text[close + 1:].strip()

    if remainder == "":
        return header, "next", None

    if remainder == "{":
        return header, "brace", None

    if is_single_statement(remainder):
        return header, "inline", remainder

    return None


def parse_control_line(line: str):
    """
    Parse an if, else-if, else, for or while line.

    Returns:

        kind
        indentation
        header
        mode
        inline statement
    """

    stripped = line.lstrip()
    indent = line[:len(line) - len(stripped)]

    for keyword in ("if", "for", "while"):
        parsed = parse_parenthesized_header(
            stripped,
            keyword,
        )

        if parsed is None:
            continue

        header, mode, statement = parsed

        # A do/while tail such as `while (x);` is not a loop body
        # that should be rewritten as `while (x) ;`.
        if keyword == "while" and mode == "inline" and statement == ";":
            continue

        return (
            keyword,
            indent,
            header,
            mode,
            statement,
        )

    if stripped.startswith("else"):
        pos = 4

        if pos < len(stripped):
            if stripped[pos].isalnum() or stripped[pos] == "_":
                return None

        remainder = stripped[pos:].lstrip()

        # else if (...)
        if remainder.startswith("if"):
            parsed = parse_parenthesized_header(
                remainder,
                "if",
            )

            if parsed is None:
                return None

            if_header, mode, statement = parsed

            return (
                "else_if",
                indent,
                f"else {if_header}",
                mode,
                statement,
            )

        # else
        if remainder == "":
            return (
                "else",
                indent,
                "else",
                "next",
                None,
            )

        # else {
        if remainder == "{":
            return (
                "else",
                indent,
                "else",
                "brace",
                None,
            )

        # else foo();
        if is_single_statement(remainder):
            return (
                "else",
                indent,
                "else",
                "inline",
                remainder,
            )

    return None


def parse_closing_brace(line: str):
    """
    Recognise:

        }

    and:

        } else ...
    """

    stripped = line.lstrip()
    indent = line[:len(line) - len(stripped)]

    if not stripped.startswith("}"):
        return None

    suffix = stripped[1:].strip()

    if suffix and not suffix.startswith("else"):
        return None

    return indent, suffix


def scan_parens(
    text: str,
    depth: int = 0,
    state: str = "normal",
) -> tuple[int, str]:
    """
    Track parenthesis depth while ignoring parentheses
    inside strings, chars and block comments.
    """

    i = 0

    while i < len(text):
        ch = text[i]
        nxt = text[i + 1] if i + 1 < len(text) else ""

        if state == "normal":
            if ch == '"':
                state = "string"

            elif ch == "'":
                state = "char"

            elif ch == "/" and nxt == "*":
                state = "comment"
                i += 1

            elif ch == "(":
                depth += 1

            elif ch == ")":
                depth -= 1

        elif state == "string":
            if ch == "\\":
                i += 1

            elif ch == '"':
                state = "normal"

        elif state == "char":
            if ch == "\\":
                i += 1

            elif ch == "'":
                state = "normal"

        elif state == "comment":
            if ch == "*" and nxt == "/":
                state = "normal"
                i += 1

        i += 1

    return depth, state


def normalize_joined_code(text: str) -> str:
    """
    Normalize whitespace outside strings/comments.

    Example:

        terminal_write( "hello" );

    becomes:

        terminal_write("hello");
    """

    out = []
    i = 0
    state = "normal"

    while i < len(text):
        ch = text[i]
        nxt = text[i + 1] if i + 1 < len(text) else ""

        if state == "normal":
            if ch == '"':
                out.append(ch)
                state = "string"
                i += 1
                continue

            if ch == "'":
                out.append(ch)
                state = "char"
                i += 1
                continue

            if ch == "/" and nxt == "*":
                out.append("/*")
                state = "comment"
                i += 2
                continue

            if ch.isspace():
                # Find next non-whitespace character.
                j = i

                while j < len(text) and text[j].isspace():
                    j += 1

                previous = out[-1] if out else ""
                following = text[j] if j < len(text) else ""

                # Don't put whitespace:
                #
                # foo( x )  -> foo(x)
                # foo( x, y ); -> foo(x, y);
                if (
                    previous not in {"", "(", "["}
                    and following not in {")", "]", ",", ";"}
                ):
                    if previous != " ":
                        out.append(" ")

                i = j
                continue

            out.append(ch)
            i += 1
            continue

        if state == "string":
            out.append(ch)

            if ch == "\\" and i + 1 < len(text):
                out.append(text[i + 1])
                i += 2
                continue

            if ch == '"':
                state = "normal"

            i += 1
            continue

        if state == "char":
            out.append(ch)

            if ch == "\\" and i + 1 < len(text):
                out.append(text[i + 1])
                i += 2
                continue

            if ch == "'":
                state = "normal"

            i += 1
            continue

        if state == "comment":
            out.append(ch)

            if ch == "*" and nxt == "/":
                out.append("/")
                state = "normal"
                i += 2
                continue

            i += 1

    return "".join(out).strip()


def collapse_multiline_parentheses(
    lines: list[str],
) -> list[str]:
    """
    Collapse expressions/calls whose parentheses span
    multiple lines.

    Example:

        terminal_write(
            "hello"
        );

    becomes:

        terminal_write("hello");

    Also:

        if (
            foo == 1 &&
            bar == 2
        ) {

    becomes:

        if (foo == 1 && bar == 2) {
    """

    result = []
    i = 0

    while i < len(lines):
        line = lines[i]

        # Leave preprocessor directives alone.
        if line.lstrip().startswith("#"):
            result.append(line)
            i += 1
            continue

        indent = line[:len(line) - len(line.lstrip())]

        depth, state = scan_parens(line)

        # Nothing continues onto another line.
        if depth <= 0:
            result.append(line)
            i += 1
            continue

        parts = [line.strip()]
        i += 1

        while i < len(lines) and depth > 0:
            part = lines[i]

            parts.append(part.strip())

            depth, state = scan_parens(
                part,
                depth,
                state,
            )

            i += 1

        # Unbalanced parentheses: don't try to modify it.
        if depth != 0:
            result.extend(parts)
            continue

        joined = " ".join(
            part
            for part in parts
            if part
        )

        joined = normalize_joined_code(joined)
        candidate = indent + joined

        # Keep compact declarations/calls when they fit the normal line target,
        # but do not destroy an already readable multiline condition/signature
        # just to make it horizontally dense.
        if (
            len(candidate.expandtabs(4)) <= LINE_LENGTH_TARGET
            or can_rewrap_parenthesized_candidate(candidate)
        ):
            result.append(candidate)
        else:
            result.extend(lines[i - len(parts):i])

    return result


def has_assignment_operator(text: str) -> bool:
    """
    Return True if text contains a C assignment operator outside
    strings, character literals and block comments.

    This recognises = as well as compound assignments such as +=,
    -=, &=, <<=, and so on, while ignoring ==, !=, <= and >=.
    """

    state = "normal"
    i = 0

    while i < len(text):
        ch = text[i]
        prev = text[i - 1] if i > 0 else ""
        nxt = text[i + 1] if i + 1 < len(text) else ""

        if state == "normal":
            if ch == '"':
                state = "string"

            elif ch == "'":
                state = "char"

            elif ch == "/" and nxt == "*":
                state = "comment"
                i += 1

            elif ch == "=":
                if nxt != "=" and prev not in {"=", "!", "<", ">"}:
                    return True

        elif state == "string":
            if ch == "\\":
                i += 1
            elif ch == '"':
                state = "normal"

        elif state == "char":
            if ch == "\\":
                i += 1
            elif ch == "'":
                state = "normal"

        elif state == "comment":
            if ch == "*" and nxt == "/":
                state = "normal"
                i += 1

        i += 1

    return False


def collapse_multiline_assignments(
    lines: list[str],
) -> list[str]:
    """
    Collapse assignments split across physical lines.

    Example:

        state->regs->is =
            0xFFFFFFFFU;

    becomes:

        state->regs->is = 0xFFFFFFFFU;

    Multiline parenthesized calls should be collapsed before this pass.
    """

    result = []
    i = 0

    while i < len(lines):
        line = lines[i]
        stripped = line.strip()

        if (
            not stripped
            or stripped.startswith("#")
            or stripped.startswith("/*")
            or not has_assignment_operator(line)
            or strip_trailing_block_comments(stripped).endswith(";")
            or "{" in stripped
            or "}" in stripped
        ):
            result.append(line)
            i += 1
            continue

        indent = line[:len(line) - len(line.lstrip())]
        parts = [stripped]
        cursor = i + 1
        complete = False

        while cursor < len(lines):
            part = lines[cursor]
            part_stripped = part.strip()

            if not part_stripped:
                break

            if part_stripped.startswith("#"):
                break

            if "{" in part_stripped or "}" in part_stripped:
                break

            parts.append(part_stripped)

            joined = normalize_joined_code(" ".join(parts))

            if strip_trailing_block_comments(joined).endswith(";"):
                complete = True
                cursor += 1
                break

            cursor += 1

        if not complete:
            result.append(line)
            i += 1
            continue

        joined = normalize_joined_code(" ".join(parts))
        candidate = indent + joined

        if len(candidate.expandtabs(4)) <= LINE_LENGTH_TARGET:
            result.append(candidate)
            i = cursor
            continue

        # The joined assignment would be harder to scan than the original.
        # Preserve the source layout rather than creating an overlong line.
        result.append(line)
        i += 1

    return result



def find_assignment_index(text: str) -> int | None:
    """
    Return the index of a C assignment '=' outside strings, character
    literals and block comments. Equality/comparison operators are ignored.
    """

    state = "normal"
    i = 0

    while i < len(text):
        ch = text[i]
        prev = text[i - 1] if i > 0 else ""
        nxt = text[i + 1] if i + 1 < len(text) else ""

        if state == "normal":
            if ch == '"':
                state = "string"

            elif ch == "'":
                state = "char"

            elif ch == "/" and nxt == "*":
                state = "comment"
                i += 1

            elif ch == "=":
                if nxt != "=" and prev not in {"=", "!", "<", ">"}:
                    return i

        elif state == "string":
            if ch == "\\":
                i += 1
            elif ch == '"':
                state = "normal"

        elif state == "char":
            if ch == "\\":
                i += 1
            elif ch == "'":
                state = "normal"

        elif state == "comment":
            if ch == "*" and nxt == "/":
                state = "normal"
                i += 1

        i += 1

    return None


def simple_statement_group_key(line: str) -> str | None:
    """
    Return a grouping key for simple statements that should be packed
    together when separated only by blank lines.

    Examples that share keys:

        k_memset(...);
        k_memset(...);

    and:

        state->regs->is = ...;
        state->regs->serr = ...;

    Unrelated statements get different keys, so intentional spacing between
    different groups is preserved.
    """

    if not is_single_statement(line):
        return None

    code = strip_trailing_block_comments(line.strip())

    if code.startswith((
        "return ",
        "return;",
        "break;",
        "continue;",
        "goto ",
    )):
        return None

    assignment_index = find_assignment_index(code)

    if assignment_index is not None:
        lhs = code[:assignment_index].rstrip()

        # Compound assignments such as += have the operator character
        # immediately before '='. Remove it from the LHS for grouping.
        if lhs and lhs[-1] in "+-*/%&|^":
            lhs = lhs[:-1].rstrip()

        if lhs.endswith("<<") or lhs.endswith(">>"):
            lhs = lhs[:-2].rstrip()

        if "->" in lhs:
            base = lhs.rsplit("->", 1)[0] + "->"
            return f"assign:{base}"

        if "." in lhs:
            base = lhs.rsplit(".", 1)[0] + "."
            return f"assign:{base}"

        if "[" in lhs:
            base = lhs.split("[", 1)[0].strip()
            return f"assign-array:{base}"

        return f"assign:{lhs}"

    # Direct function/macro call such as k_memset(...);
    open_paren = code.find("(")

    if open_paren > 0:
        callee = code[:open_paren].strip()

        if callee and all(
            ch.isalnum() or ch in "_>-."
            for ch in callee
        ):
            return f"call:{callee}"

    return None


def statement_indent(line: str) -> str:
    stripped = line.lstrip()
    return line[:len(line) - len(stripped)]


def bunch_simple_statement_runs(
    lines: list[str],
) -> list[str]:
    """
    Remove blank lines between matching runs of ordinary statements, then
    normalise indentation inside each run.

    This packs runs such as repeated k_memset() calls or assignments to
    fields on the same object, without removing blank lines between unrelated
    statement groups.
    """

    packed = []

    for i, line in enumerate(lines):
        if line.strip() != "":
            packed.append(line)
            continue

        previous = packed[-1] if packed else None
        next_line = None

        for j in range(i + 1, len(lines)):
            if lines[j].strip() != "":
                next_line = lines[j]
                break

        if previous is not None and next_line is not None:
            previous_key = simple_statement_group_key(previous)
            next_key = simple_statement_group_key(next_line)

            if previous_key is not None and previous_key == next_key:
                continue

        packed.append(line)

    # Normalise indentation for contiguous members of the same group. This
    # fixes cases where the first statement accidentally has no indentation
    # while the rest of the run are indented consistently.
    result = []
    i = 0

    while i < len(packed):
        key = simple_statement_group_key(packed[i])

        if key is None:
            result.append(packed[i])
            i += 1
            continue

        run = [packed[i]]
        cursor = i + 1

        while cursor < len(packed):
            if simple_statement_group_key(packed[cursor]) != key:
                break

            run.append(packed[cursor])
            cursor += 1

        if len(run) == 1:
            result.append(run[0])
            i += 1
            continue

        counts = {}

        for statement in run:
            indent = statement_indent(statement)
            counts[indent] = counts.get(indent, 0) + 1

        common_indent = max(
            counts,
            key=lambda indent: (
                counts[indent],
                len(indent.expandtabs(8)),
            ),
        )

        for statement in run:
            result.append(common_indent + statement.lstrip())

        i = cursor

    return result

def is_declaration_like_line(line: str) -> bool:
    """Recognize ordinary local declarations for conservative vertical compaction."""

    stripped = strip_trailing_block_comments(line.strip())

    if not stripped.endswith(";") or count_top_level_semicolons(stripped) != 1:
        return False
    if stripped.startswith(("return ", "return;", "goto ", "break;", "continue;")):
        return False
    if parse_control_line(stripped) is not None:
        return False

    declaration = re.compile(
        r"^(?:(?:const|volatile|static|extern|register)\s+)*"
        r"(?:(?:struct|union|enum)\s+[A-Za-z_]\w*|[A-Za-z_]\w*)"
        r"\s+\**\s*[A-Za-z_]\w*"
        r"(?:\s*\[[^\]]*\])?"
        r"(?:\s*=.*)?;$"
    )
    return declaration.match(stripped) is not None


def bunch_local_declarations(lines: list[str]) -> list[str]:
    """Remove blank lines that split a contiguous run of local declarations."""

    result: list[str] = []

    for i, line in enumerate(lines):
        if line.strip():
            result.append(line)
            continue

        previous = result[-1] if result else None
        next_line = None

        for j in range(i + 1, len(lines)):
            if lines[j].strip():
                next_line = lines[j]
                break

        if previous is not None and next_line is not None:
            previous_indent = statement_indent(previous)
            next_indent = statement_indent(next_line)

            if (
                previous_indent
                and previous_indent == next_indent
                and is_declaration_like_line(previous)
                and is_declaration_like_line(next_line)
            ):
                continue

        result.append(line)

    return result


def is_function_prototype_line(line: str) -> bool:
    """Return True for an ordinary top-level C function prototype."""

    if statement_indent(line):
        return False

    stripped = strip_trailing_block_comments(line.strip())
    if not stripped.endswith(";") or "{" in stripped or "}" in stripped:
        return False
    if stripped.startswith(("typedef ", "#")):
        return False

    return function_name_from_header(stripped[:-1].rstrip()) is not None


def bunch_top_level_prototypes(lines: list[str]) -> list[str]:
    """Remove blank lines between adjacent prototypes while preserving comments/groups."""

    result: list[str] = []

    for i, line in enumerate(lines):
        if line.strip():
            result.append(line)
            continue

        previous = result[-1] if result else None
        next_line = None

        for j in range(i + 1, len(lines)):
            if lines[j].strip():
                next_line = lines[j]
                break

        if (
            previous is not None
            and next_line is not None
            and is_function_prototype_line(previous)
            and is_function_prototype_line(next_line)
        ):
            continue

        result.append(line)

    return result


def ensure_top_level_function_spacing(lines: list[str]) -> list[str]:
    """Ensure top-level function definitions are separated by one blank line."""

    result: list[str] = []

    for i, line in enumerate(lines):
        result.append(line)

        if line != "}":
            continue
        if i + 1 >= len(lines) or not lines[i + 1].strip():
            continue

        # A bare column-zero closing brace in this C codebase is a function end.
        # Struct/enum/initializer endings normally carry a trailing ';' or name.
        result.append("")

    return result


def collapse_blank_lines(
    lines: list[str],
) -> list[str]:
    """
    Collapse multiple blank lines into one blank line.
    """

    result = []
    previous_blank = False

    for line in lines:
        blank = line.strip() == ""

        if blank:
            if not previous_blank:
                result.append("")

            previous_blank = True

        else:
            result.append(line)
            previous_blank = False

    return result


def starts_compact_control(line: str) -> bool:
    """
    Return True if a line begins with an if, for or while statement.
    """

    stripped = line.lstrip()

    for keyword in ("if", "for", "while"):
        if not stripped.startswith(keyword):
            continue

        pos = len(keyword)

        if pos >= len(stripped):
            continue

        if not (
            stripped[pos].isalnum()
            or stripped[pos] == "_"
        ):
            return True

    return False


def bunch_compact_controls(
    lines: list[str],
) -> list[str]:
    """
    Remove blank lines between consecutive compact
    if/for/while statements.

    Example:

        if (x) foo();

        if (y) bar();

    becomes:

        if (x) foo();
        if (y) bar();
    """

    result = []

    for i, line in enumerate(lines):
        if line.strip() != "":
            result.append(line)
            continue

        previous = (
            result[-1]
            if result
            else None
        )

        next_line = None

        for j in range(i + 1, len(lines)):
            if lines[j].strip() != "":
                next_line = lines[j]
                break

        if (
            previous is not None
            and next_line is not None
            and starts_compact_control(previous)
            and starts_compact_control(next_line)
        ):
            continue

        result.append(line)

    return result


def can_attach_opening_brace(line: str) -> bool:
    """Return True when a standalone `{` should be attached K&R-style."""

    stripped = line.strip()

    if not stripped or stripped.startswith("#"):
        return False

    if stripped.endswith((";", "{", "}", "\\")):
        return False

    if stripped.startswith("/*") and stripped.endswith("*/"):
        return False

    if stripped in {"else", "do"}:
        return True

    if stripped.endswith(")"):
        return True

    if stripped.startswith(("struct ", "union ", "enum ")):
        return True

    # Initializers split as `foo =` followed by `{` are also naturally K&R.
    if stripped.endswith("="):
        return True

    return False


def attach_opening_braces(lines: list[str]) -> list[str]:
    """Convert Allman-style standalone opening braces to K&R placement."""

    result = []

    for line in lines:
        if line.strip() == "{" and result:
            previous = result[-1]

            if can_attach_opening_brace(previous):
                result[-1] = previous.rstrip() + " {"
                continue

        result.append(line)

    return result


def attach_else_lines(lines: list[str]) -> list[str]:
    """Convert a separate `else` line to `} else ...` when indentation matches."""

    result = []

    for line in lines:
        stripped = line.lstrip()

        if stripped.startswith("else") and result:
            previous = result[-1]
            previous_stripped = previous.rstrip()

            if previous_stripped.endswith("}"):
                previous_indent = statement_indent(previous)
                current_indent = statement_indent(line)

                if previous_indent.expandtabs(8) == current_indent.expandtabs(8):
                    result[-1] = previous_stripped + " " + stripped
                    continue

        result.append(line)

    return result


def split_top_level_statements(text: str) -> list[str] | None:
    """Split a physical line at top-level C statement semicolons."""

    parts = []
    start = 0
    paren_depth = 0
    bracket_depth = 0
    state = "normal"
    i = 0

    while i < len(text):
        ch = text[i]
        nxt = text[i + 1] if i + 1 < len(text) else ""

        if state == "normal":
            if ch == '"':
                state = "string"
            elif ch == "'":
                state = "char"
            elif ch == "/" and nxt == "*":
                state = "comment"
                i += 1
            elif ch == "(":
                paren_depth += 1
            elif ch == ")":
                paren_depth = max(0, paren_depth - 1)
            elif ch == "[":
                bracket_depth += 1
            elif ch == "]":
                bracket_depth = max(0, bracket_depth - 1)
            elif ch == ";" and paren_depth == 0 and bracket_depth == 0:
                parts.append(text[start:i + 1].strip())
                start = i + 1

        elif state == "string":
            if ch == "\\":
                i += 1
            elif ch == '"':
                state = "normal"

        elif state == "char":
            if ch == "\\":
                i += 1
            elif ch == "'":
                state = "normal"

        elif state == "comment":
            if ch == "*" and nxt == "/":
                state = "normal"
                i += 1

        i += 1

    remainder = text[start:].strip()

    if remainder:
        # A trailing comment or non-statement fragment makes the line ambiguous;
        # leave it alone rather than risking a semantic/comment placement change.
        return None

    return [part for part in parts if part]



def _normalize_code_segment(segment: str) -> str:
    """Normalize conservative C whitespace inside a non-literal code segment."""

    # Pointer member access is visually atomic. The decrement/comparison edge
    # case `x-->0` is normalized separately below.
    segment = re.sub(r"\s*->\s*", "->", segment)

    # Comparisons, boolean operators and assignments are easier to scan with
    # one space on each side. Arithmetic operators are intentionally excluded
    # because '*' and '&' are ambiguous with pointer declarators.
    operators = r"(<<=|>>=|\+=|-=|\*=|/=|%=|&=|\|=|\^=|==|!=|<=|>=|&&|\|\||=)"
    segment = re.sub(rf"[ \t]*{operators}[ \t]*", r" \1 ", segment)

    # Normal comma and delimiter spacing.
    segment = re.sub(r"[ \t]*,[ \t]*", ", ", segment)
    segment = re.sub(r"[ \t]*;[ \t]*", "; ", segment)
    segment = re.sub(r"[ \t]*(<<|>>)(?!=)[ \t]*", r" \1 ", segment)
    segment = re.sub(r"([^<\s])[ \t]*<[ \t]*([^<=\s])", r"\1 < \2", segment)
    segment = re.sub(r"([^->=\s])[ \t]*>[ \t]*([^=>\s])", r"\1 > \2", segment)
    segment = re.sub(r"--[ \t]*>[ \t]*", "-- > ", segment)
    segment = re.sub(r"\([ \t]+", "(", segment)
    segment = re.sub(r"[ \t]+\)", ")", segment)
    segment = re.sub(r"\[[ \t]+", "[", segment)
    segment = re.sub(r"[ \t]+\]", "]", segment)

    # C control keywords keep a space before '('. Function calls do not.
    segment = re.sub(r"\b(if|for|while|switch)\s*\(", r"\1 (", segment)
    segment = re.sub(r"\breturn\s*\(", "return (", segment)

    segment = re.sub(r"[ \t]+$", " ", segment)
    return segment


def normalize_c_line_spacing(line: str) -> str:
    """Normalize safe whitespace while preserving strings and comments exactly."""

    stripped = line.lstrip()

    if not stripped or stripped.startswith("#"):
        return line.rstrip()

    # Protected asm blocks are marker lines. Inline asm is intentionally left
    # alone too because GNU asm punctuation is whitespace-sensitive to humans.
    if "__asm__" in line or "__asm " in line or re.search(r"\basm\s*\(", line):
        return line.rstrip()

    indent = line[:len(line) - len(stripped)]
    text = stripped.rstrip()
    out: list[str] = []
    code: list[str] = []
    state = "normal"
    i = 0

    def flush_code() -> None:
        if code:
            out.append(_normalize_code_segment("".join(code)))
            code.clear()

    while i < len(text):
        ch = text[i]
        nxt = text[i + 1] if i + 1 < len(text) else ""

        if state == "normal":
            if ch == '"':
                flush_code()
                out.append(ch)
                state = "string"
            elif ch == "'":
                flush_code()
                out.append(ch)
                state = "char"
            elif ch == "/" and nxt == "*":
                flush_code()
                out.append("/*")
                state = "comment"
                i += 1
            else:
                code.append(ch)

        elif state == "string":
            out.append(ch)
            if ch == "\\" and i + 1 < len(text):
                out.append(text[i + 1])
                i += 1
            elif ch == '"':
                state = "normal"

        elif state == "char":
            out.append(ch)
            if ch == "\\" and i + 1 < len(text):
                out.append(text[i + 1])
                i += 1
            elif ch == "'":
                state = "normal"

        elif state == "comment":
            out.append(ch)
            if ch == "*" and nxt == "/":
                out.append("/")
                state = "normal"
                i += 1

        i += 1

    flush_code()
    return indent + "".join(out).rstrip()


def normalize_c_spacing(lines: list[str]) -> list[str]:
    """Apply conservative whitespace cleanup to physical C source lines."""

    return [normalize_c_line_spacing(line) for line in lines]


def is_guard_statement(text: str) -> bool:
    """Return True for a short control-flow exit suitable for an inline guard."""

    code = strip_trailing_block_comments(text.strip())
    return code == "return;" or code.startswith((
        "return ",
        "break;",
        "continue;",
        "goto ",
    ))


def is_plain_statement_fragment(text: str) -> bool:
    """Return True for one non-control semicolon-terminated statement/declaration."""

    code = strip_trailing_block_comments(text.strip())

    if not code or not code.endswith(";"):
        return False
    if count_top_level_semicolons(code) != 1:
        return False
    if "{" in code or "}" in code or code.startswith("#"):
        return False
    if code.startswith(("case ", "default:")):
        return False

    return parse_control_line(code) is None


def is_inline_control_statement(text: str) -> bool:
    """Return True for a statement that is safe/readable as an inline body.

    This is intentionally narrower than ``is_single_statement`` when removing
    braces. In particular, it avoids turning a typedef-based declaration such
    as ``Widget value;`` into the invalid ``if (x) Widget value;``.
    """

    raw = text.strip()
    if re.fullmatch(r"/\*__CODE_FORMAT_PRESERVED_ASM_\d+__\*/", raw):
        return True

    if not is_single_statement(text):
        return False

    code = strip_trailing_block_comments(raw)

    if is_guard_statement(code):
        return True

    if code.startswith(("__asm__", "__asm ", "asm ")):
        return True

    # Ordinary direct/member call expression, including function-like macros.
    # A leading `(void)` cast is also common for deliberately ignored cleanup
    # return values and is still one obvious action.
    expression = code[:-1].strip()
    if expression.startswith("(void)"):
        expression = expression[6:].lstrip()

    open_paren = expression.find("(")
    if open_paren > 0:
        callee = expression[:open_paren].strip()
        if (
            re.fullmatch(
                r"[A-Za-z_][A-Za-z0-9_]*(?:(?:->|\.)[A-Za-z_][A-Za-z0-9_]*)*",
                callee,
            )
            and find_matching_paren(expression, open_paren) == len(expression) - 1
        ):
            return True

    if find_assignment_index(code) is not None:
        return True

    # Prefix/postfix increment/decrement are ordinary expression statements.
    if re.fullmatch(r"(?:\+\+|--)?[A-Za-z_][A-Za-z0-9_]*(?:\+\+|--);", code):
        return True

    return False


def enforce_one_statement_per_line(lines: list[str]) -> list[str]:
    """Split horizontally packed ordinary statements without changing semantics."""

    result: list[str] = []

    for line in lines:
        stripped = line.strip()

        if not stripped or "{" in stripped or "}" in stripped:
            result.append(line)
            continue

        parts = split_top_level_statements(stripped)

        if parts is None or len(parts) < 2:
            result.append(line)
            continue

        valid = True

        for part in parts:
            parsed = parse_control_line(part)
            if parsed is not None and parsed[3] == "inline":
                continue
            if not is_plain_statement_fragment(part):
                valid = False
                break

        if not valid:
            result.append(line)
            continue

        indent = statement_indent(line)
        result.extend(indent + part for part in parts)

    return result


def expand_one_line_function_definitions(lines: list[str]) -> list[str]:
    """Expand compact function definitions so the body has its own source lines."""

    result: list[str] = []

    for line in lines:
        stripped = line.lstrip()
        indent = line[:len(line) - len(stripped)]

        if "{" not in stripped or "}" not in stripped or stripped.startswith("#"):
            result.append(line)
            continue

        open_brace = stripped.find("{")
        close_brace = stripped.rfind("}")

        if open_brace <= 0 or close_brace <= open_brace:
            result.append(line)
            continue

        header = stripped[:open_brace].rstrip()
        trailer = stripped[close_brace + 1:].strip()
        body = stripped[open_brace + 1:close_brace].strip()

        if trailer or function_name_from_header(header) is None:
            result.append(line)
            continue

        if not body:
            result.append(f"{indent}{header} {{")
            result.append(f"{indent}}}")
            continue

        if "{" in body or "}" in body:
            result.append(line)
            continue

        statements = split_top_level_statements(body)
        if statements is None or not statements:
            result.append(line)
            continue
        if not all(is_plain_statement_fragment(statement) for statement in statements):
            result.append(line)
            continue

        result.append(f"{indent}{header} {{")
        result.extend(f"{indent}    {statement}" for statement in statements)
        result.append(f"{indent}}}")

    return result


def split_attached_else_lines(lines: list[str]) -> list[str]:
    """Temporarily split `} else ...` so the else body can be normalized."""

    result: list[str] = []

    for line in lines:
        stripped = line.lstrip()
        indent = line[:len(line) - len(stripped)]

        if stripped.startswith("} else"):
            result.append(indent + "}")
            result.append(indent + stripped[2:].lstrip())
        else:
            result.append(line)

    return result


def _compact_braced_control(line: str):
    """Parse a one-line simple control body: `if (x) { foo(); bar(); }`."""

    stripped = line.lstrip()
    indent = line[:len(line) - len(stripped)]
    header = None
    remainder = None

    if stripped.startswith("else"):
        rest = stripped[4:].lstrip()
        if rest.startswith("if"):
            parsed = parse_parenthesized_header(rest, "if")
            if parsed is not None and parsed[1] == "next":
                # `parse_parenthesized_header` cannot parse the braced inline
                # body itself, so handle `else if` below using raw offsets.
                pass

    for prefix in ("else if", "if", "for", "while", "switch"):
        if not stripped.startswith(prefix):
            continue

        pos = len(prefix)
        if pos < len(stripped) and (stripped[pos].isalnum() or stripped[pos] == "_"):
            continue

        while pos < len(stripped) and stripped[pos].isspace():
            pos += 1
        if pos >= len(stripped) or stripped[pos] != "(":
            continue

        close = find_matching_paren(stripped, pos)
        if close is None:
            continue

        header = stripped[:close + 1]
        remainder = stripped[close + 1:].strip()
        break

    if header is None and stripped.startswith("else"):
        after = stripped[4:].strip()
        if after.startswith("{"):
            header = "else"
            remainder = after

    if header is None or remainder is None:
        return None
    if not remainder.startswith("{") or not remainder.endswith("}"):
        return None

    body = remainder[1:-1].strip()
    if not body or "{" in body or "}" in body:
        return None

    statements = split_top_level_statements(body)
    if statements is None or not statements:
        return None
    if not all(is_plain_statement_fragment(statement) for statement in statements):
        return None

    return indent, header, statements


def expand_compact_braced_controls(lines: list[str]) -> list[str]:
    """Expand one-line braced control bodies so body statements scan vertically."""

    result: list[str] = []

    for line in lines:
        parsed = _compact_braced_control(line)

        if parsed is None:
            result.append(line)
            continue

        indent, header, statements = parsed
        result.append(f"{indent}{header} {{")
        result.extend(f"{indent}    {statement}" for statement in statements)
        result.append(f"{indent}}}")

    return result


def collapse_single_statement_control_blocks(lines: list[str]) -> list[str]:
    """Collapse a simple three-line control body when it stays easy to scan.

    This deliberately preserves compact dispatch/table-like code such as::

        if (matches("help")) command_help();
        else if (matches("clear")) terminal_clear();

    A branch with more than one statement remains braced and vertical.
    Declarations are also left braced because C does not permit a declaration
    as the direct body of an unbraced control statement.
    """

    result: list[str] = []
    i = 0

    while i < len(lines):
        if i + 2 >= len(lines):
            result.append(lines[i])
            i += 1
            continue

        parsed = parse_control_line(lines[i])
        body = lines[i + 1]
        close = lines[i + 2]

        if (
            parsed is None
            or parsed[3] != "brace"
            or parsed[0] not in {"if", "else_if", "else", "for", "while"}
            or not is_inline_control_statement(body)
            or close.strip() != "}"
            or statement_indent(close).expandtabs(8) != parsed[1].expandtabs(8)
            or visual_indent(body) <= visual_indent(lines[i])
        ):
            result.append(lines[i])
            i += 1
            continue

        candidate = f"{parsed[1]}{parsed[2]} {body.strip()}"

        if len(candidate.expandtabs(4)) > LINE_LENGTH_TARGET:
            result.append(lines[i])
            i += 1
            continue

        result.append(candidate)
        i += 3

    return result


def expand_inline_control_bodies(lines: list[str]) -> list[str]:
    """Keep short single-statement control bodies inline; brace longer bodies."""

    result: list[str] = []

    for line in lines:
        parsed = parse_control_line(line)

        if parsed is None or parsed[3] != "inline" or parsed[4] is None:
            result.append(line)
            continue

        kind, indent, header, _, statement = parsed
        candidate = f"{indent}{header} {statement}"

        if (
            kind in {"if", "else_if", "else", "for", "while"}
            and len(candidate.expandtabs(4)) <= LINE_LENGTH_TARGET
        ):
            result.append(candidate)
            continue

        result.append(f"{indent}{header} {{")
        result.append(f"{indent}    {statement}")
        result.append(f"{indent}}}")

    return result

def collapse_unbraced_guard_clauses(lines: list[str]) -> list[str]:
    """Collapse a short unbraced ``if`` guard onto one physical line.

    Example::

        if (!space)
            return false;

    becomes::

        if (!space) return false;

    Only exit-style guard statements are handled here. Ordinary one-statement
    bodies continue to follow the normal compact-control rules.
    """

    result: list[str] = []
    i = 0

    while i < len(lines):
        parsed = parse_control_line(lines[i])

        if parsed is None or parsed[0] != "if" or parsed[3] != "next":
            result.append(lines[i])
            i += 1
            continue

        body_index = i + 1
        while body_index < len(lines) and not lines[body_index].strip():
            body_index += 1

        if body_index >= len(lines):
            result.append(lines[i])
            i += 1
            continue

        body = lines[body_index]
        if (
            not is_guard_statement(body)
            or visual_indent(body) <= visual_indent(lines[i])
        ):
            result.append(lines[i])
            i += 1
            continue

        candidate = f"{parsed[1]}{parsed[2]} {body.strip()}"
        if len(candidate.expandtabs(4)) > LINE_LENGTH_TARGET:
            result.append(lines[i])
            i += 1
            continue

        result.append(candidate)
        i = body_index + 1

    return result


def collapse_braced_guard_clauses(lines: list[str]) -> list[str]:
    """Collapse a three-line braced guard only when the result stays short."""

    result: list[str] = []
    i = 0

    while i < len(lines):
        if i + 2 >= len(lines):
            result.append(lines[i])
            i += 1
            continue

        parsed = parse_control_line(lines[i])
        body = lines[i + 1]
        close = lines[i + 2]

        if (
            parsed is None
            or parsed[0] != "if"
            or parsed[3] != "brace"
            or not is_guard_statement(body)
            or close.strip() != "}"
            or statement_indent(close).expandtabs(8) != parsed[1].expandtabs(8)
            or visual_indent(body) <= visual_indent(lines[i])
        ):
            result.append(lines[i])
            i += 1
            continue

        # Do not collapse the head of an if/else chain.
        next_index = i + 3
        while next_index < len(lines) and not lines[next_index].strip():
            next_index += 1
        if next_index < len(lines) and lines[next_index].lstrip().startswith("else"):
            result.append(lines[i])
            i += 1
            continue

        candidate = f"{parsed[1]}{parsed[2]} {body.strip()}"
        if len(candidate.expandtabs(4)) > LINE_LENGTH_TARGET:
            result.append(lines[i])
            i += 1
            continue

        result.append(candidate)
        i += 3

    return result


def _split_top_level_tokens(text: str, tokens: tuple[str, ...]) -> list[tuple[str, str | None]]:
    """Split text at selected top-level tokens while ignoring nested/literal text."""

    parts: list[tuple[str, str | None]] = []
    start = 0
    paren_depth = 0
    bracket_depth = 0
    state = "normal"
    i = 0

    while i < len(text):
        ch = text[i]
        nxt = text[i + 1] if i + 1 < len(text) else ""

        if state == "normal":
            if ch == '"':
                state = "string"
            elif ch == "'":
                state = "char"
            elif ch == "/" and nxt == "*":
                state = "comment"
                i += 1
            elif ch == "(":
                paren_depth += 1
            elif ch == ")":
                paren_depth = max(0, paren_depth - 1)
            elif ch == "[":
                bracket_depth += 1
            elif ch == "]":
                bracket_depth = max(0, bracket_depth - 1)
            elif paren_depth == 0 and bracket_depth == 0:
                matched = next((token for token in tokens if text.startswith(token, i)), None)
                if matched is not None:
                    parts.append((text[start:i].strip(), matched))
                    i += len(matched)
                    start = i
                    continue

        elif state == "string":
            if ch == "\\":
                i += 1
            elif ch == '"':
                state = "normal"

        elif state == "char":
            if ch == "\\":
                i += 1
            elif ch == "'":
                state = "normal"

        elif state == "comment":
            if ch == "*" and nxt == "/":
                state = "normal"
                i += 1

        i += 1

    parts.append((text[start:].strip(), None))
    return parts


def split_top_level_logical_terms(expression: str) -> list[str]:
    """Split a boolean expression into one top-level predicate per line."""

    raw = _split_top_level_tokens(expression, ("&&", "||"))
    terms: list[str] = []

    for part, operator in raw:
        if not part:
            return [expression.strip()]
        terms.append(f"{part} {operator}" if operator else part)

    return terms


def split_top_level_commas(expression: str) -> list[str]:
    """Split a parenthesized argument/parameter list at top-level commas."""

    raw = _split_top_level_tokens(expression, (",",))
    return [part for part, _ in raw if part]


def can_rewrap_parenthesized_candidate(line: str) -> bool:
    """Return True if an overlong joined parenthesized line can be split safely later."""

    if parse_control_line(line) is not None:
        return True

    stripped = line.lstrip()
    open_paren = stripped.find("(")
    if open_paren <= 0:
        return False

    close_paren = find_matching_paren(stripped, open_paren)
    if close_paren is None:
        return False

    arguments = stripped[open_paren + 1:close_paren].strip()
    return len(split_top_level_commas(arguments)) >= 2


def _pack_wrapped_fragments(
    first_prefix: str,
    continuation_prefix: str,
    fragments: list[str],
    final_suffix: str = "",
    *,
    target: int = WRAP_LINE_TARGET,
) -> list[str]:
    """Greedily pack already-safe fragments into readable continuation lines."""

    if not fragments:
        return [first_prefix.rstrip() + final_suffix]

    result: list[str] = []
    prefix = first_prefix
    current = prefix

    for fragment in fragments:
        separator = "" if current == prefix else " "
        candidate = current + separator + fragment

        if current != prefix and len(candidate.expandtabs(4)) > target:
            result.append(current.rstrip())
            prefix = continuation_prefix
            current = prefix + fragment
        elif current == prefix and len(candidate.expandtabs(4)) > target:
            # If the very first fragment cannot fit beside its prefix, keep the
            # prefix as a natural opening line and move the fragment below it.
            result.append(current.rstrip())
            prefix = continuation_prefix
            current = prefix + fragment
        else:
            current = candidate

    if final_suffix:
        candidate = current.rstrip() + final_suffix
        # Closing punctuation must never be stranded on a source line by itself.
        # Prefer the normal target, but allow the hard limit for tiny suffixes
        # such as `;`, `);`, or `) {`.
        limit = LINE_LENGTH_HARD_LIMIT if len(final_suffix) <= 4 else LINE_LENGTH_TARGET
        if len(candidate.expandtabs(4)) <= limit or len(final_suffix) <= 2:
            current = candidate
        else:
            result.append(current.rstrip())
            current = continuation_prefix + final_suffix.lstrip()

    result.append(current.rstrip())
    return result


def wrap_long_control_conditions(lines: list[str]) -> list[str]:
    """Wrap long boolean controls, packing several predicates per line when safe."""

    result: list[str] = []

    for line in lines:
        if len(line.expandtabs(4)) <= LINE_LENGTH_TARGET:
            result.append(line)
            continue

        parsed = parse_control_line(line)
        if parsed is None or parsed[0] not in {"if", "else_if", "while"}:
            result.append(line)
            continue

        _, indent, header, mode, statement = parsed
        if mode == "inline" or statement is not None:
            result.append(line)
            continue

        open_paren = header.find("(")
        close_paren = find_matching_paren(header, open_paren) if open_paren >= 0 else None
        if close_paren is None:
            result.append(line)
            continue

        condition = header[open_paren + 1:close_paren].strip()
        terms = split_top_level_logical_terms(condition)
        suffix = ")" + (" {" if mode == "brace" else "")

        if len(terms) < 2:
            # A single long function-call predicate can still wrap compactly by
            # grouping its arguments rather than putting the whole condition on
            # a 120+ column line. Handles forms such as `!foo(a, b, c, d)`.
            call_open = condition.find("(")
            call_close = find_matching_paren(condition, call_open) if call_open > 0 else None
            if call_close == len(condition) - 1:
                arguments = condition[call_open + 1:call_close].strip()
                parts = split_top_level_commas(arguments)
                if parts:
                    fragments = [
                        part + ("," if i + 1 < len(parts) else "")
                        for i, part in enumerate(parts)
                    ]
                    result.extend(
                        _pack_wrapped_fragments(
                            indent + header[:open_paren + 1] + condition[:call_open + 1],
                            indent + "        ",
                            fragments,
                            ")" + suffix,
                        )
                    )
                    continue

            result.append(indent + header[:open_paren + 1])
            result.append(indent + "    " + condition)
            result.append(indent + suffix)
            continue

        result.extend(
            _pack_wrapped_fragments(
                indent + header[:open_paren + 1],
                indent + "    ",
                terms,
                suffix,
            )
        )

    return result


def _logical_assignment_parts(line: str) -> tuple[str, list[str]] | None:
    """Return assignment prefix and top-level logical RHS terms for a long statement."""

    stripped = line.lstrip()
    if not stripped.endswith(";") or "/*" in stripped or stripped.startswith("#"):
        return None

    assignment = find_assignment_index(stripped)
    if assignment is None:
        return None

    # Only ordinary '=' assignments are handled here. Compound assignments are
    # deliberately left to the generic formatter.
    if assignment > 0 and stripped[assignment - 1] in "+-*/%&|^":
        return None

    rhs = stripped[assignment + 1:-1].strip()
    terms = split_top_level_logical_terms(rhs)
    if len(terms) < 2:
        return None

    return stripped[:assignment + 1].rstrip(), terms


def wrap_long_logical_assignments(lines: list[str]) -> list[str]:
    """Wrap overlong boolean assignments without using one predicate per line."""

    result: list[str] = []

    for line in lines:
        if len(line.expandtabs(4)) <= LINE_LENGTH_TARGET:
            result.append(line)
            continue

        parsed = _logical_assignment_parts(line)
        if parsed is None:
            result.append(line)
            continue

        prefix, terms = parsed
        indent = statement_indent(line)
        result.extend(
            _pack_wrapped_fragments(
                indent + prefix + " ",
                indent + "    ",
                terms,
                ";",
            )
        )

    return result


def _collect_multiline_logical_assignment(
    lines: list[str],
    index: int,
) -> tuple[str, int] | None:
    """Join one simple multiline assignment so it can be rewrapped consistently."""

    first = lines[index]
    stripped = first.strip()
    if not has_assignment_operator(first) or strip_trailing_block_comments(stripped).endswith(";"):
        return None
    if stripped.startswith(("#", "/*")) or "{" in stripped or "}" in stripped:
        return None

    indent = statement_indent(first)
    parts = [stripped]
    cursor = index + 1
    paren_depth, state = scan_parens(first)

    while cursor < len(lines):
        part = lines[cursor]
        piece = part.strip()

        if not piece or piece.startswith(("#", "/*")) or "{" in piece or "}" in piece:
            return None

        parts.append(piece)
        paren_depth, state = scan_parens(part, paren_depth, state)

        joined = normalize_joined_code(" ".join(parts))
        if paren_depth == 0 and strip_trailing_block_comments(joined).endswith(";"):
            return indent + joined, cursor + 1

        cursor += 1

    return None


def compact_multiline_logical_assignments(lines: list[str]) -> list[str]:
    """Repack multiline boolean assignments into fewer, bounded-width lines."""

    result: list[str] = []
    i = 0

    while i < len(lines):
        collected = _collect_multiline_logical_assignment(lines, i)
        if collected is None:
            result.append(lines[i])
            i += 1
            continue

        joined, next_index = collected
        # Parentheses around the entire RHS are kept. The generic logical
        # splitter can still find top-level operators inside those parentheses
        # only after peeling that one redundant presentation layer.
        stripped = joined.lstrip()
        assignment = find_assignment_index(stripped)
        if assignment is None:
            result.append(lines[i])
            i += 1
            continue

        prefix = stripped[:assignment + 1].rstrip()
        rhs = stripped[assignment + 1:-1].strip()
        wrapped = False

        if rhs.startswith("(") and find_matching_paren(rhs, 0) == len(rhs) - 1:
            inner = rhs[1:-1].strip()
            terms = split_top_level_logical_terms(inner)
            if len(terms) >= 2:
                indent = statement_indent(joined)
                packed = _pack_wrapped_fragments(
                    indent + prefix + " (",
                    indent + "    ",
                    terms,
                    ");",
                )
                result.extend(packed)
                wrapped = True
        else:
            terms = split_top_level_logical_terms(rhs)
            if len(terms) >= 2:
                indent = statement_indent(joined)
                result.extend(
                    _pack_wrapped_fragments(
                        indent + prefix + " ",
                        indent + "    ",
                        terms,
                        ";",
                    )
                )
                wrapped = True

        if wrapped:
            i = next_index
        else:
            result.append(lines[i])
            i += 1

    return result


def wrap_long_parenthesized_lines(lines: list[str]) -> list[str]:
    """Wrap long calls/declarations, packing multiple arguments per continuation line."""

    result: list[str] = []

    for line in lines:
        if len(line.expandtabs(4)) <= LINE_LENGTH_TARGET:
            result.append(line)
            continue

        stripped = line.lstrip()
        indent = line[:len(line) - len(stripped)]

        if (
            not stripped
            or stripped.startswith(("#", "/*", "*", "//"))
            or parse_control_line(line) is not None
            or "__asm__" in line
        ):
            result.append(line)
            continue

        open_paren = stripped.find("(")
        if open_paren <= 0:
            result.append(line)
            continue

        close_paren = find_matching_paren(stripped, open_paren)
        if close_paren is None:
            result.append(line)
            continue

        arguments = stripped[open_paren + 1:close_paren].strip()
        parts = split_top_level_commas(arguments)
        if not parts:
            result.append(line)
            continue
        # Even a single complex argument should wrap once the line exceeds the
        # normal target; the hard limit is reserved for literals/comments that
        # cannot be decomposed safely.

        fragments = [part + ("," if i + 1 < len(parts) else "") for i, part in enumerate(parts)]
        suffix = ")" + stripped[close_paren + 1:].rstrip()

        result.extend(
            _pack_wrapped_fragments(
                indent + stripped[:open_paren + 1],
                indent + "    ",
                fragments,
                suffix,
            )
        )

    return result


def is_terminal_output_statement(line: str) -> bool:
    """Return True for a plain terminal_* output/configuration statement."""

    stripped = strip_trailing_block_comments(line.strip())
    if not is_single_statement(stripped):
        return False
    return re.match(r"^terminal_[A-Za-z0-9_]+\s*\(", stripped) is not None


def compact_internal_blank_lines(lines: list[str]) -> list[str]:
    """Remove low-information blank lines while preserving phase boundaries.

    Blank lines still separate comments, nontrivial control-flow phases and
    independent test steps. They are removed when they merely split an opening
    brace from its body, a computed value from its terminal report, or cleanup
    code from the exit it belongs to.
    """

    result: list[str] = []

    for i, line in enumerate(lines):
        if line.strip():
            result.append(line)
            continue

        previous = result[-1] if result else None
        next_line = None
        for j in range(i + 1, len(lines)):
            if lines[j].strip():
                next_line = lines[j]
                break

        if previous is None or next_line is None:
            result.append(line)
            continue

        previous_stripped = previous.strip()
        next_stripped = next_line.strip()

        if previous_stripped.endswith("{") or next_stripped == "}":
            continue

        same_indent = statement_indent(previous) == statement_indent(next_line)
        previous_is_comment = previous_stripped.startswith(("/*", "*", "//", "#"))

        # Keep a result/calculation visually attached to the output that reports
        # it. The blank *after* that report is retained as the phase boundary.
        if (
            same_indent
            and is_terminal_output_statement(next_line)
            and not previous_is_comment
            and previous_stripped != "}"
            and parse_control_line(previous) is None
        ):
            continue

        # Cleanup followed by return/break/continue is one compact exit phase.
        if same_indent and is_guard_statement(next_line) and not previous_is_comment and previous_stripped != "}":
            continue

        result.append(line)

    return result


def compact_multiline_block_comments(text: str) -> str:
    """
    Collapse standalone wrapped prose comments into compact one-line comments.

    Example:

        /*
         * Poll until hardware clears CI bit zero.
         */

    becomes:

        /* Poll until hardware clears CI bit zero. */

    Blank-line-separated comments, list-like comments, and comments that would
    exceed COMMENT_LINE_LIMIT are left alone. Leading indentation is preserved.
    """
    lines = text.splitlines()
    result = []
    i = 0

    while i < len(lines):
        line = lines[i]

        if line.strip() != "/*":
            result.append(line)
            i += 1
            continue

        indent = line[:len(line) - len(line.lstrip())]
        content = []
        cursor = i + 1
        valid = True
        close_index = None

        while cursor < len(lines):
            stripped = lines[cursor].strip()

            if stripped == "*/":
                close_index = cursor
                break

            # A blank line usually indicates a real paragraph boundary, so
            # preserve the original multiline layout.
            if not stripped:
                valid = False
                break

            if not stripped.startswith("*"):
                valid = False
                break

            part = stripped[1:].strip()

            if not part:
                valid = False
                break

            # Preserve comments that look like lists, annotations, or other
            # intentionally structured material instead of flattening them.
            if (
                part.startswith(("- ", "+ ", "@", "```"))
                or (len(part) >= 2 and part[0].isdigit() and part[1] in ".)")
            ):
                valid = False
                break

            content.append(part)
            cursor += 1

        if not valid or close_index is None or not content:
            result.append(line)
            i += 1
            continue

        body = " ".join(" ".join(content).split())
        compact = f"{indent}/* {body} */"

        if len(compact.expandtabs(4)) > COMMENT_LINE_LIMIT:
            result.append(line)
            i += 1
            continue

        result.append(compact)
        i = close_index + 1

    return "\n".join(result)

def convert_line_comments(text: str) -> str:
    """
    Convert:

        // hello

    into:

        /* hello */

    Also handles:

        foo(); // hello

    becoming:

        foo(); /* hello */

    Strings such as:

        "https://example.com"

    are left alone.
    """

    out = []
    i = 0
    state = "normal"

    while i < len(text):
        ch = text[i]

        nxt = (
            text[i + 1]
            if i + 1 < len(text)
            else ""
        )

        if state == "normal":
            if ch == '"':
                out.append(ch)
                state = "string"
                i += 1
                continue

            if ch == "'":
                out.append(ch)
                state = "char"
                i += 1
                continue

            if ch == "/" and nxt == "*":
                out.append("/*")
                state = "block_comment"
                i += 2
                continue

            if ch == "/" and nxt == "/":
                end = i + 2

                while (
                    end < len(text)
                    and text[end] not in "\r\n"
                ):
                    end += 1

                comment = text[i + 2:end].strip()

                # Prevent a */ inside the // comment from
                # accidentally closing our new block comment.
                comment = comment.replace(
                    "*/",
                    "* /",
                )

                if comment:
                    out.append(
                        f"/* {comment} */"
                    )
                else:
                    out.append("/* */")

                i = end
                continue

            out.append(ch)
            i += 1
            continue

        if state == "string":
            out.append(ch)

            if (
                ch == "\\"
                and i + 1 < len(text)
            ):
                out.append(text[i + 1])
                i += 2
                continue

            if ch == '"':
                state = "normal"

            i += 1
            continue

        if state == "char":
            out.append(ch)

            if (
                ch == "\\"
                and i + 1 < len(text)
            ):
                out.append(text[i + 1])
                i += 2
                continue

            if ch == "'":
                state = "normal"

            i += 1
            continue

        if state == "block_comment":
            out.append(ch)

            if ch == "*" and nxt == "/":
                out.append("/")
                state = "normal"
                i += 2
                continue

            i += 1

    return "".join(out)


def format_text(original: str) -> str:
    """Format C source text using JCOS's readability-first house style."""

    newline = "\r\n" if "\r\n" in original else "\n"
    had_final_newline = original.endswith(("\n", "\r"))

    # Inline assembly and deliberately structured block comments are protected
    # from all structural/whitespace passes and restored byte-for-byte later.
    text, preserved_asm = protect_asm_blocks(original)
    text = convert_line_comments(text)
    text = compact_multiline_block_comments(text)
    text = wrap_long_standalone_block_comments(text)
    text, preserved_comments = protect_multiline_block_comments(text)

    lines = text.splitlines()

    # First normalize safe whitespace. This repairs visual noise such as
    # `value =true`, `thread-> member`, `if(`, and missing spaces after commas
    # without touching strings, comments, preprocessor directives, or asm.
    lines = normalize_c_spacing(lines)

    # Compact multiline calls/declarations/assignments only when the result
    # fits the normal 120-column target. Longer expressions keep their readable
    # multiline source shape instead of being forced horizontal.
    lines = collapse_multiline_parentheses(lines)
    lines = collapse_multiline_assignments(lines)
    lines = normalize_c_spacing(lines)

    # K&R braces remain the structural baseline.
    lines = attach_opening_braces(lines)
    lines = split_attached_else_lines(lines)

    # Repair legacy formatter output before deciding which controls may stay
    # compact. Horizontally packed ordinary statements are split, but a control
    # branch containing exactly one simple statement may remain on one line when
    # it fits the 120-column target. Multi-statement branches stay braced.
    lines = expand_one_line_function_definitions(lines)
    lines = enforce_one_statement_per_line(lines)
    lines = expand_compact_braced_controls(lines)
    lines = collapse_single_statement_control_blocks(lines)
    lines = expand_inline_control_bodies(lines)
    lines = collapse_single_statement_control_blocks(lines)
    lines = collapse_unbraced_guard_clauses(lines)
    lines = collapse_braced_guard_clauses(lines)
    lines = attach_else_lines(lines)

    # Long boolean expressions are packed into bounded-width continuation
    # lines. Several related predicates/arguments may share a line; this keeps
    # the source compact without recreating the old 200+ column lines.
    lines = compact_multiline_logical_assignments(lines)
    lines = wrap_long_control_conditions(lines)
    lines = wrap_long_logical_assignments(lines)
    lines = wrap_long_parenthesized_lines(lines)

    # Blank lines are paragraph separators: collapse repeats, remove accidental
    # gaps inside exact statement groups, and bunch consecutive compact guards.
    # We deliberately do NOT use the old same-root horizontal/spacing packers.
    lines = collapse_blank_lines(lines)
    lines = compact_internal_blank_lines(lines)
    lines = bunch_local_declarations(lines)
    lines = bunch_top_level_prototypes(lines)
    lines = bunch_simple_statement_runs(lines)
    lines = bunch_compact_controls(lines)
    lines = ensure_top_level_function_spacing(lines)

    # A final conservative spacing pass catches constructs exposed by wrapping.
    lines = normalize_c_spacing(lines)
    lines = collapse_unbraced_guard_clauses(lines)

    new_text = newline.join(lines)
    if had_final_newline:
        new_text += newline

    new_text = restore_multiline_block_comments(new_text, preserved_comments)
    new_text = restore_asm_blocks(new_text, preserved_asm)
    return new_text

def line_range_offsets(
    text: str,
    start_line: int,
    end_line: int,
) -> tuple[int, int]:
    """Return byte-string offsets for an inclusive 1-based physical line range."""

    lines = text.splitlines(keepends=True)

    if not lines:
        raise ValueError("Cannot select lines from an empty file")

    if start_line < 1 or end_line < 1:
        raise ValueError("Line numbers must be >= 1")

    if start_line > end_line:
        raise ValueError(
            f"Start line {start_line} is after end line {end_line}"
        )

    if end_line > len(lines):
        raise ValueError(
            f"Line range {start_line}-{end_line} exceeds file length "
            f"({len(lines)} lines)"
        )

    start_offset = sum(
        len(line)
        for line in lines[:start_line - 1]
    )
    end_offset = start_offset + sum(
        len(line)
        for line in lines[start_line - 1:end_line]
    )

    return start_offset, end_offset


def format_line_range(
    original: str,
    start_line: int,
    end_line: int,
) -> str:
    """
    Format only an inclusive line range.

    Everything before and after the selection is copied back byte-for-byte.
    """

    start_offset, end_offset = line_range_offsets(
        original,
        start_line,
        end_line,
    )

    selected = original[start_offset:end_offset]
    formatted = format_text(selected)

    return (
        original[:start_offset]
        + formatted
        + original[end_offset:]
    )


def mask_preprocessor_lines(text: str) -> str:
    """Blank preprocessor directives while preserving offsets and newlines."""

    result = []
    continuing = False

    for line in text.splitlines(keepends=True):
        if line.endswith("\r\n"):
            ending = "\r\n"
            body = line[:-2]
        elif line.endswith(("\n", "\r")):
            ending = line[-1]
            body = line[:-1]
        else:
            ending = ""
            body = line

        is_directive = continuing or body.lstrip().startswith("#")

        if is_directive:
            result.append(" " * len(body) + ending)
            continuing = body.rstrip().endswith("\\")
        else:
            result.append(line)
            continuing = False

    return "".join(result)


def c_scan_view(text: str) -> str:
    """
    Return a same-length C source view with comments/strings/directives blanked.

    Newlines and real C punctuation remain in place so offsets and brace nesting
    can be inspected without being confused by braces inside comments or strings.
    """

    text = mask_preprocessor_lines(text)
    out = list(text)
    state = "normal"
    i = 0

    while i < len(text):
        ch = text[i]
        nxt = text[i + 1] if i + 1 < len(text) else ""

        if state == "normal":
            if ch == "/" and nxt == "/":
                out[i] = " "
                out[i + 1] = " "
                state = "line_comment"
                i += 2
                continue

            if ch == "/" and nxt == "*":
                out[i] = " "
                out[i + 1] = " "
                state = "block_comment"
                i += 2
                continue

            if ch == '"':
                out[i] = " "
                state = "string"
                i += 1
                continue

            if ch == "'":
                out[i] = " "
                state = "char"
                i += 1
                continue

            i += 1
            continue

        if state == "line_comment":
            if ch in "\r\n":
                state = "normal"
            else:
                out[i] = " "

            i += 1
            continue

        if state == "block_comment":
            if ch == "*" and nxt == "/":
                out[i] = " "
                out[i + 1] = " "
                state = "normal"
                i += 2
                continue

            if ch not in "\r\n":
                out[i] = " "

            i += 1
            continue

        if state in {"string", "char"}:
            quote = '"' if state == "string" else "'"

            if ch == "\\" and i + 1 < len(text):
                if ch not in "\r\n":
                    out[i] = " "

                if text[i + 1] not in "\r\n":
                    out[i + 1] = " "

                i += 2
                continue

            if ch == quote:
                out[i] = " "
                state = "normal"
                i += 1
                continue

            if ch not in "\r\n":
                out[i] = " "

            i += 1

    return "".join(out)


def function_name_from_header(header: str) -> str | None:
    """Return a likely C function name for a top-level header before `{`."""

    candidates = []
    paren_depth = 0
    bracket_depth = 0
    i = 0

    while i < len(header):
        ch = header[i]

        if ch == "(" and bracket_depth == 0:
            paren_depth += 1
            i += 1
            continue

        if ch == ")" and bracket_depth == 0:
            paren_depth = max(0, paren_depth - 1)
            i += 1
            continue

        if ch == "[":
            bracket_depth += 1
            i += 1
            continue

        if ch == "]":
            bracket_depth = max(0, bracket_depth - 1)
            i += 1
            continue

        # A top-level assignment strongly indicates an initializer rather than a
        # function definition. C function parameter defaults do not exist.
        if ch == "=" and paren_depth == 0 and bracket_depth == 0:
            return None

        if (
            paren_depth == 0
            and bracket_depth == 0
            and (ch.isalpha() or ch == "_")
        ):
            start = i
            i += 1

            while i < len(header) and (
                header[i].isalnum()
                or header[i] == "_"
            ):
                i += 1

            name = header[start:i]
            cursor = i

            while cursor < len(header) and header[cursor].isspace():
                cursor += 1

            if cursor < len(header) and header[cursor] == "(":
                candidates.append(name)

            continue

        i += 1

    blocked = {
        "if",
        "for",
        "while",
        "switch",
        "sizeof",
        "_Alignof",
        "typeof",
        "__typeof__",
        "__attribute__",
        "__declspec",
        "asm",
        "__asm",
        "__asm__",
    }

    plausible = [
        name
        for name in candidates
        if name not in blocked
    ]

    if not plausible:
        return None

    # Post-signature annotations are commonly uppercase macros or __attributes.
    # Prefer the last ordinary identifier, falling back to the last candidate.
    ordinary = [
        name
        for name in plausible
        if not name.isupper()
        and not name.startswith("__")
    ]

    return ordinary[-1] if ordinary else plausible[-1]


def find_c_functions(text: str) -> list[dict[str, int | str]]:
    """
    Find ordinary top-level C function definitions.

    The scanner is deliberately conservative and dependency-free. It ignores
    strings, comments, and preprocessor directives, then matches top-level brace
    blocks whose header looks like a function declarator.
    """

    scan = c_scan_view(text)
    functions = []
    brace_depth = 0
    boundary = 0
    active = None
    i = 0

    while i < len(scan):
        ch = scan[i]

        if ch == "{" and brace_depth == 0:
            segment = scan[boundary:i]
            leading = len(segment) - len(segment.lstrip())
            header_start = boundary + leading
            header = scan[header_start:i]
            name = function_name_from_header(header)

            if name is not None:
                active = {
                    "name": name,
                    "start_offset": header_start,
                    "open_offset": i,
                }

            brace_depth = 1
            i += 1
            continue

        if ch == "{" and brace_depth > 0:
            brace_depth += 1
            i += 1
            continue

        if ch == "}" and brace_depth > 0:
            brace_depth -= 1

            if brace_depth == 0:
                if active is not None:
                    start_offset = int(active["start_offset"])
                    functions.append({
                        "name": str(active["name"]),
                        "start_line": text.count("\n", 0, start_offset) + 1,
                        "end_line": text.count("\n", 0, i) + 1,
                    })
                    active = None

                boundary = i + 1

            i += 1
            continue

        if ch == ";" and brace_depth == 0:
            boundary = i + 1

        i += 1

    return functions


def select_function_range(
    text: str,
    function_name: str | None = None,
    function_line: int | None = None,
) -> tuple[int, int, str]:
    """Resolve a function selector to inclusive start/end lines and its name."""

    functions = find_c_functions(text)

    if function_name is not None:
        matches = [
            function
            for function in functions
            if function["name"] == function_name
        ]

        if not matches:
            raise ValueError(
                f"Function not found: {function_name}"
            )

        if len(matches) > 1:
            starts = ", ".join(
                str(function["start_line"])
                for function in matches
            )
            raise ValueError(
                f"Function name {function_name!r} is ambiguous; "
                f"definitions start on lines {starts}"
            )

        selected = matches[0]

    elif function_line is not None:
        if function_line < 1:
            raise ValueError("Line numbers must be >= 1")

        matches = [
            function
            for function in functions
            if (
                int(function["start_line"])
                <= function_line
                <= int(function["end_line"])
            )
        ]

        if not matches:
            raise ValueError(
                f"No function contains line {function_line}"
            )

        selected = matches[0]

    else:
        raise ValueError("A function name or function line is required")

    return (
        int(selected["start_line"]),
        int(selected["end_line"]),
        str(selected["name"]),
    )


def format_file(
    path: Path,
    *,
    line_range: tuple[int, int] | None = None,
    function_name: str | None = None,
    function_line: int | None = None,
) -> bool:
    """Format a whole file or one selected region; return True if it changed."""

    # newline="" prevents Python from automatically converting line endings.
    with path.open(
        "r",
        encoding="utf-8",
        newline="",
    ) as file:
        original = file.read()

    if line_range is not None:
        start_line, end_line = line_range
        new_text = format_line_range(
            original,
            start_line,
            end_line,
        )

    elif function_name is not None or function_line is not None:
        start_line, end_line, resolved_name = select_function_range(
            original,
            function_name=function_name,
            function_line=function_line,
        )
        new_text = format_line_range(
            original,
            start_line,
            end_line,
        )

    else:
        new_text = format_text(original)

    if new_text == original:
        return False

    with path.open(
        "w",
        encoding="utf-8",
        newline="",
    ) as file:
        file.write(new_text)

    return True


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Format .c and .h files, functions, or selected line ranges",
        epilog=(
            "Examples:\n"
            "  code_format.py source.c\n"
            "  code_format.py source.c --function init_device\n"
            "  code_format.py source.c --function-at-line 240\n"
            "  code_format.py source.c --lines 240 310"
        ),
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )

    parser.add_argument(
        "path",
        type=Path,
        help="File or directory to format",
    )

    selection = parser.add_mutually_exclusive_group()

    selection.add_argument(
        "-f",
        "--function",
        dest="function_name",
        metavar="NAME",
        help="Format only the named C function definition",
    )

    selection.add_argument(
        "--function-at-line",
        "--function-line",
        "--function-start-line",
        "--at-line",
        dest="function_line",
        type=int,
        metavar="LINE",
        help=(
            "Format the entire function containing LINE; LINE may be the "
            "function's starting line or any line inside it"
        ),
    )

    selection.add_argument(
        "--lines",
        "--line-range",
        dest="line_range",
        type=int,
        nargs=2,
        metavar=("START", "END"),
        help="Format only inclusive physical lines START through END",
    )

    args = parser.parse_args()

    path = args.path

    if not path.exists():
        raise SystemExit(
            f"File or directory does not exist: {path}"
        )

    selective = (
        args.function_name is not None
        or args.function_line is not None
        or args.line_range is not None
    )

    # --------------------------------------
    # Single file
    # --------------------------------------
    if path.is_file():
        if path.suffix.lower() not in {".c", ".h"}:
            raise SystemExit(
                f"Not a .c or .h file: {path}"
            )

        files = [path]

    # --------------------------------------
    # Directory
    # --------------------------------------
    elif path.is_dir():
        if selective:
            raise SystemExit(
                "--function, --function-at-line, and --lines require a single "
                ".c or .h file, not a directory"
            )

        files = sorted(
            file
            for file in path.rglob("*")
            if (
                file.is_file()
                and file.suffix.lower() in {".c", ".h"}
            )
        )

    else:
        raise SystemExit(
            f"Unsupported path: {path}"
        )

    if not selective:
        print(
            f"Found {len(files)} C/header file(s)"
        )

    changed = 0

    for file in files:
        selected = None

        try:
            if args.line_range is not None:
                start_line, end_line = args.line_range
                # Validate before formatting so the status description is only
                # printed for a real, in-range selection.
                with file.open(
                    "r",
                    encoding="utf-8",
                    newline="",
                ) as source:
                    original = source.read()

                line_range_offsets(original, start_line, end_line)
                selected = f"lines {start_line}-{end_line}"

            elif args.function_name is not None or args.function_line is not None:
                with file.open(
                    "r",
                    encoding="utf-8",
                    newline="",
                ) as source:
                    original = source.read()

                start_line, end_line, resolved_name = select_function_range(
                    original,
                    function_name=args.function_name,
                    function_line=args.function_line,
                )
                selected = (
                    f"function {resolved_name} (lines {start_line}-{end_line})"
                )

            did_change = format_file(
                file,
                line_range=(
                    tuple(args.line_range)
                    if args.line_range is not None
                    else None
                ),
                function_name=args.function_name,
                function_line=args.function_line,
            )
        except ValueError as exc:
            raise SystemExit(str(exc)) from exc

        if selected is not None:
            action = "Formatted" if did_change else "Already formatted"
            print(f"{action}: {file} [{selected}]")
        elif did_change:
            print(f"Formatted: {file}")

        if did_change:
            changed += 1

    print(
        f"Changed {changed} file(s)"
    )


if __name__ == "__main__":
    main()
