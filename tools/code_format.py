#!/usr/bin/env python3

import argparse
from pathlib import Path


COMPACT_LINE_LIMIT = 180
PREFIX_PACK_LINE_LIMIT = 280
COMMENT_LINE_LIMIT = 280


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


def direct_call_callee(line: str) -> str | None:
    """
    Return the callee for a plain call statement such as:

        terminal_write("hello");

    Declarations, assignments, control statements and compound expressions are
    deliberately rejected. This keeps horizontal packing conservative.
    """

    if not is_single_statement(line):
        return None

    code = strip_trailing_block_comments(line.strip())

    if "/*" in line or has_assignment_operator(code):
        return None

    open_paren = code.find("(")

    if open_paren <= 0:
        return None

    callee = code[:open_paren].strip()

    if not callee:
        return None

    if not all(ch.isalnum() or ch in "_>." for ch in callee):
        return None

    return callee



def is_terminal_writeln_statement(text: str) -> bool:
    """Return True only for a direct `terminal_writeln(...);` statement."""

    return direct_call_callee(text) == "terminal_writeln"


def compact_call_block(
    indent: str,
    header: str,
    statements: list[str],
) -> str | None:
    """Build `if (...) { foo(); bar(); }` when the body is safely compactable."""

    if len(statements) < 2:
        return None

    if any(direct_call_callee(statement) is None for statement in statements):
        return None

    # terminal_writeln() is a hard physical-line boundary. Never fold it into
    # a compact multi-call control body with neighbouring statements.
    if any(is_terminal_writeln_statement(statement) for statement in statements):
        return None

    body = " ".join(statement.strip() for statement in statements)
    candidate = f"{indent}{header} {{ {body} }}"

    if len(candidate.expandtabs(4)) > COMPACT_LINE_LIMIT:
        return None

    return candidate


def consume_clause(
    lines: list[str],
    header_text: str,
    next_index: int,
):
    """
    Convert one if/else/for/while clause into one line where safe.

    Single-statement bodies lose unnecessary braces. Braced bodies containing
    two or more plain call statements may instead become a compact one-line
    block, preserving braces so semantics stay unchanged.
    """

    parsed = parse_control_line(header_text)

    if parsed is None:
        return None

    kind, indent, header, mode, statement = parsed

    # Already one-line.
    if mode == "inline":
        return {
            "kind": kind,
            "text": f"{indent}{header} {statement}",
            "next_index": next_index,
            "tail": None,
        }

    # Skip blank lines before the body.
    body_index = next_index

    while (
        body_index < len(lines)
        and lines[body_index].strip() == ""
    ):
        body_index += 1

    if body_index >= len(lines):
        return None

    # --------------------------------------
    # No braces
    #
    # if (x)
    #     foo();
    # --------------------------------------
    if mode == "next":
        body = lines[body_index]

        if not is_single_statement(body):
            return None

        if visual_indent(body) <= visual_indent(header_text):
            return None

        return {
            "kind": kind,
            "text": f"{indent}{header} {body.strip()}",
            "next_index": body_index + 1,
            "tail": None,
        }

    # --------------------------------------
    # Braces
    # --------------------------------------
    statements = []
    cursor = body_index
    close_indent = None
    suffix = ""

    while cursor < len(lines):
        current = lines[cursor]

        if current.strip() == "":
            cursor += 1
            continue

        close = parse_closing_brace(current)

        if close is not None:
            close_indent, suffix = close
            break

        if visual_indent(current) <= visual_indent(header_text):
            return None

        if not is_single_statement(current):
            return None

        statements.append(current.strip())
        cursor += 1

    if close_indent is None or not statements:
        return None

    # Closing brace must line up with control statement.
    if close_indent.expandtabs(8) != indent.expandtabs(8):
        return None

    tail = f"{close_indent}{suffix}" if suffix else None

    if len(statements) == 1:
        text = f"{indent}{header} {statements[0]}"
    else:
        text = compact_call_block(indent, header, statements)

        if text is None:
            return None

    return {
        "kind": kind,
        "text": text,
        "next_index": cursor + 1,
        "tail": tail,
    }

def try_format_if_chain(
    lines: list[str],
    index: int,
):
    """
    Convert:

        if (x)
            foo();

        else if (y) {
            bar();
        }

        else
            baz();

    into:

        if (x) foo();
        else if (y) bar();
        else baz();
    """

    first = consume_clause(
        lines,
        lines[index],
        index + 1,
    )

    if first is None:
        return None

    if first["kind"] != "if":
        return None

    clauses = [first["text"]]

    cursor = first["next_index"]
    tail = first["tail"]

    while True:
        if tail is not None:
            # Handles:
            #
            # } else if (...) {
            #
            header_text = tail
            next_index = cursor

        else:
            # Look over blank lines without consuming them
            # unless we actually find an else.
            candidate = cursor

            while (
                candidate < len(lines)
                and lines[candidate].strip() == ""
            ):
                candidate += 1

            if candidate >= len(lines):
                break

            parsed = parse_control_line(
                lines[candidate]
            )

            if parsed is None:
                break

            if parsed[0] not in {
                "else_if",
                "else",
            }:
                break

            header_text = lines[candidate]
            next_index = candidate + 1

        clause = consume_clause(
            lines,
            header_text,
            next_index,
        )

        if clause is None:
            return None

        if clause["kind"] not in {
            "else_if",
            "else",
        }:
            return None

        clauses.append(clause["text"])

        cursor = clause["next_index"]
        tail = clause["tail"]

        if clause["kind"] == "else":
            if tail is not None:
                return None

            break

    return clauses, cursor

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

        result.append(indent + joined)

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
        result.append(indent + joined)
        i = cursor

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

def format_controls(
    lines: list[str],
) -> list[str]:
    """
    Format simple if/else chains, for loops and while loops.
    """

    result = []
    i = 0

    while i < len(lines):
        parsed = parse_control_line(lines[i])

        if parsed is None:
            result.append(lines[i])
            i += 1
            continue

        kind = parsed[0]

        # --------------------------------------
        # if / else-if / else
        # --------------------------------------
        if kind == "if":
            chain = try_format_if_chain(
                lines,
                i,
            )

            if chain is not None:
                formatted_lines, next_index = chain

                result.extend(formatted_lines)
                i = next_index
                continue

        # --------------------------------------
        # for (...) / while (...)
        # --------------------------------------
        elif kind in {"for", "while"}:
            clause = consume_clause(
                lines,
                lines[i],
                i + 1,
            )

            if (
                clause is not None
                and clause["tail"] is None
            ):
                result.append(
                    clause["text"]
                )

                i = clause["next_index"]
                continue

        # Couldn't safely transform it.
        result.append(lines[i])
        i += 1

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


def leading_statement_root(line: str) -> str | None:
    """
    Return the leading family/root of a simple statement.

    The root is the first identifier, shortened at the first underscore. This
    makes related forms share a key:

        terminal_write(...)      -> terminal
        terminal_putchar(...)    -> terminal
        state->regs->is = ...    -> state
        device.address.bus = ... -> device
        buffer[index] = ...      -> buffer

    Declarations, controls, returns, comments and compound/multi-statement
    lines are deliberately ignored.
    """

    if not is_single_statement(line):
        return None

    code = strip_trailing_block_comments(line.strip())

    if not code:
        return None

    i = 0

    if not (code[i].isalpha() or code[i] == "_"):
        return None

    i += 1

    while i < len(code) and (code[i].isalnum() or code[i] == "_"):
        i += 1

    identifier = code[:i]
    remainder = code[i:]

    # An underscore inside the leading identifier defines an API/name family:
    # terminal_write -> terminal, pci_read_bar -> pci. Keep a leading
    # underscore as part of the identifier rather than producing an empty root.
    separator = identifier.find("_", 1)

    if separator > 0:
        return identifier[:separator]

    # Member/index/call syntax also defines a useful expression root. Plain
    # whitespace after the first identifier does not: this deliberately keeps
    # custom-type declarations such as `u64 a = 0;` or `Foo value;` out of
    # horizontal packing without needing to know every project typedef.
    if remainder.startswith("->") or remainder.startswith("."):
        return identifier

    if remainder.startswith("[") or remainder.startswith("("):
        return identifier

    return None


def compact_line_root(line: str) -> str | None:
    """
    Return the root represented by a physical line.

    This also recognises inline controls such as:

        if (x) terminal_write("YES");

    so blank-line cleanup can treat them as part of the same family without
    ever joining the control statement horizontally with surrounding code.
    """

    root = leading_statement_root(line)

    if root is not None:
        return root

    parsed = parse_control_line(line)

    if parsed is not None and parsed[3] == "inline" and parsed[4] is not None:
        return leading_statement_root(parsed[4])

    # A line already packed by this formatter contains multiple top-level
    # semicolons. Inspect only its first statement for spacing/grouping.
    stripped = line.strip()
    state = "normal"
    paren_depth = 0
    bracket_depth = 0
    i = 0

    while i < len(stripped):
        ch = stripped[i]
        nxt = stripped[i + 1] if i + 1 < len(stripped) else ""

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
                first = stripped[:i + 1]
                return leading_statement_root(first)

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


def pack_same_root_statements(lines: list[str]) -> list[str]:
    """
    Horizontally pack adjacent simple statements that share a leading root.

    Blank lines are hard boundaries during this pass. That means an existing
    blank-separated layout acts as a useful hint for where one compact
    physical line should end. A later pass can remove the blank *between*
    two same-root compact lines without merging the lines themselves.

    Example:

        terminal_write("  ABAR: ");
        terminal_write_hex(info->abar);
        terminal_putchar('\\n');

        terminal_write("  CAP: ");
        terminal_write_hex(info->capabilities);
        terminal_putchar('\\n');

    becomes:

        terminal_write("  ABAR: "); terminal_write_hex(info->abar); terminal_putchar('\\n');
        terminal_write("  CAP: "); terminal_write_hex(info->capabilities); terminal_putchar('\\n');
    """

    result = []
    i = 0

    while i < len(lines):
        line = lines[i]
        root = leading_statement_root(line)

        # Assignments are intentionally vertical. The root-family packer exists
        # for call/API families such as terminal_write/terminal_putchar, not for
        # adjacent member writes such as header->ctba = ...; header->ctbau = ...;.
        if (
            root is None
            or is_terminal_writeln_statement(line)
            or has_assignment_operator(strip_trailing_block_comments(line.strip()))
        ):
            result.append(line)
            i += 1
            continue

        indent = statement_indent(line)
        parts = [line.strip()]
        cursor = i + 1

        while cursor < len(lines):
            candidate = lines[cursor]

            if candidate.strip() == "":
                break

            if statement_indent(candidate) != indent:
                break

            if leading_statement_root(candidate) != root:
                break

            if is_terminal_writeln_statement(candidate):
                break

            joined = indent + " ".join(parts + [candidate.strip()])

            if len(joined.expandtabs(4)) > PREFIX_PACK_LINE_LIMIT:
                break

            parts.append(candidate.strip())
            cursor += 1

        if len(parts) == 1:
            result.append(line)
            i += 1
            continue

        result.append(indent + " ".join(parts))
        i = cursor

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


def is_assignment_statement(text: str) -> bool:
    """Return True for one ordinary assignment statement."""

    if not is_single_statement(text):
        return False

    code = strip_trailing_block_comments(text.strip())
    return find_assignment_index(code) is not None


def enforce_assignment_statement_lines(lines: list[str]) -> list[str]:
    """
    Keep runs of ordinary assignments one statement per physical source line.

    This also repairs output produced by older formatter versions::

        header->prdt_length = 1U; header->ctba = value; header->ctbau = high;

    becomes::

        header->prdt_length = 1U;
        header->ctba = value;
        header->ctbau = high;

    Compact braced control bodies are left alone; this pass only splits plain
    physical lines whose top-level statements are all assignments.
    """

    result = []

    for line in lines:
        stripped = line.strip()

        if not stripped or "{" in stripped or "}" in stripped:
            result.append(line)
            continue

        parts = split_top_level_statements(stripped)

        if parts is None or len(parts) < 2:
            result.append(line)
            continue

        if not all(is_assignment_statement(part) for part in parts):
            result.append(line)
            continue

        indent = statement_indent(line)
        result.extend(indent + part for part in parts)

    return result


def wrap_inline_multi_statement_ifs(lines: list[str]) -> list[str]:
    """
    Recover compact multi-statement `if` bodies by restoring braces.

    Example:

        if (!any) foo(); bar(); baz();

    becomes:

        if (!any) { foo(); bar(); baz(); }

    This rule is intentionally narrow: the physical line must begin with an
    `if (...)` header and the entire remainder must consist of two or more
    ordinary simple statements. Declarations, comments, nested controls and
    ambiguous trailing text are left untouched.

    `terminal_writeln(...)` remains a hard physical-line boundary. If one of
    the recovered statements is a terminal_writeln call, use a multiline
    braced body instead of putting that call beside neighbouring statements.
    """

    result = []

    for line in lines:
        stripped = line.lstrip()
        indent = line[:len(line) - len(stripped)]

        if not stripped.startswith("if"):
            result.append(line)
            continue

        pos = 2

        # Avoid matching identifiers such as `ifdef` or `if_state`.
        if pos < len(stripped) and (stripped[pos].isalnum() or stripped[pos] == "_"):
            result.append(line)
            continue

        while pos < len(stripped) and stripped[pos].isspace():
            pos += 1

        if pos >= len(stripped) or stripped[pos] != "(":
            result.append(line)
            continue

        close = find_matching_paren(stripped, pos)

        if close is None:
            result.append(line)
            continue

        header = stripped[:close + 1]
        remainder = stripped[close + 1:].strip()

        # Already braced, empty, or otherwise not the malformed compact form
        # this pass is responsible for.
        if not remainder or remainder.startswith("{"):
            result.append(line)
            continue

        statements = split_top_level_statements(remainder)

        if statements is None or len(statements) < 2:
            result.append(line)
            continue

        if not all(is_single_statement(statement) for statement in statements):
            result.append(line)
            continue

        if any(is_terminal_writeln_statement(statement) for statement in statements):
            result.append(f"{indent}{header} {{")

            for statement in statements:
                result.append(f"{indent}    {statement}")

            result.append(f"{indent}}}")
            continue

        body = " ".join(statement.strip() for statement in statements)
        compact = f"{indent}{header} {{ {body} }}"

        if len(compact.expandtabs(4)) <= COMPACT_LINE_LIMIT:
            result.append(compact)
            continue

        # Preserve the recovered semantics even when the compact line would be
        # too long; just fall back to a normal braced body.
        result.append(f"{indent}{header} {{")

        for statement in statements:
            result.append(f"{indent}    {statement}")

        result.append(f"{indent}}}")

    return result

def enforce_terminal_writeln_lines(lines: list[str]) -> list[str]:
    """
    Ensure terminal_writeln(...) occupies its own physical source line.

    This is an enforcement pass, not merely a packing preference, so previously
    compacted input such as:

        terminal_write("A"); terminal_writeln("B"); terminal_write("C");

    becomes:

        terminal_write("A");
        terminal_writeln("B");
        terminal_write("C");
    """

    result = []

    for line in lines:
        stripped = line.strip()

        if "terminal_writeln" not in stripped:
            result.append(line)
            continue

        # A compact control such as `if (x) terminal_writeln(...);` still puts
        # terminal_writeln on a shared physical line. Expand it to a small
        # braced block so the call itself is truly standalone.
        parsed = parse_control_line(line)

        if parsed is not None and parsed[3] == "inline" and parsed[4] is not None:
            kind, indent, header, _, statement = parsed

            if is_terminal_writeln_statement(statement):
                result.append(f"{indent}{header} {{")
                result.append(f"{indent}    {statement}")
                result.append(f"{indent}}}")
                continue

        if count_top_level_semicolons(stripped) < 2:
            result.append(line)
            continue

        statements = split_top_level_statements(stripped)

        if statements is None or not statements:
            result.append(line)
            continue

        # Only rewrite lines made entirely from ordinary simple statements.
        if not all(is_single_statement(statement) for statement in statements):
            result.append(line)
            continue

        if not any(is_terminal_writeln_statement(statement) for statement in statements):
            result.append(line)
            continue

        indent = statement_indent(line)
        pending = []

        def flush_pending() -> None:
            if pending:
                result.append(indent + " ".join(pending))
                pending.clear()

        for statement in statements:
            if is_terminal_writeln_statement(statement):
                flush_pending()
                result.append(indent + statement)
            else:
                pending.append(statement)

        flush_pending()

    return result


def bunch_same_root_spacing(lines: list[str]) -> list[str]:
    """Remove blank lines that only separate two lines from the same root."""

    result = []

    for i, line in enumerate(lines):
        if line.strip() != "":
            result.append(line)
            continue

        previous = result[-1] if result else None
        next_line = None

        for j in range(i + 1, len(lines)):
            if lines[j].strip() != "":
                next_line = lines[j]
                break

        if previous is not None and next_line is not None:
            previous_root = compact_line_root(previous)
            next_root = compact_line_root(next_line)

            if previous_root is not None and previous_root == next_root:
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

        if visual_indent(compact) > COMMENT_LINE_LIMIT:
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


def format_file(path: Path) -> bool:
    """
    Format one .c or .h file.

    Returns True if the file changed.
    """

    # newline="" prevents Python from automatically
    # converting line endings.
    with path.open(
        "r",
        encoding="utf-8",
        newline="",
    ) as file:
        original = file.read()

    # Preserve the existing line-ending style.
    newline = (
        "\r\n"
        if "\r\n" in original
        else "\n"
    )

    had_final_newline = original.endswith(
        ("\n", "\r")
    )

    # Protect inline assembly before any formatter pass. Extended GNU asm uses
    # punctuation-heavy multiline syntax where whitespace and line layout are
    # often intentionally kept readable; preserve those statements verbatim.
    text, preserved_asm = protect_asm_blocks(original)

    # Convert // comments before processing lines. Protected asm is represented
    # only by inert marker comments here, so comments inside asm stay untouched.
    text = convert_line_comments(text)

    # Collapse ordinary wrapped prose block comments such as:
    #
    #     /*
    #      * Poll until hardware clears CI bit zero.
    #      */
    #
    # into a single compact comment while preserving indentation.
    text = compact_multiline_block_comments(text)

    # Any multiline block comment that remains is intentionally structured.
    # Hide it before line-oriented passes so text such as "DET=3" cannot be
    # mistaken for an assignment and joined with the following C statement.
    text, preserved_comments = protect_multiline_block_comments(text)

    lines = text.splitlines()

    # Collapse multiline function calls and conditions first.
    lines = collapse_multiline_parentheses(lines)

    # Collapse split assignments such as:
    #
    #     value =
    #         123;
    #
    # into:
    #
    #     value = 123;
    lines = collapse_multiline_assignments(lines)

    # Attach opening braces before compacting control structures so both
    # Allman and K&R input converge on the same style.
    lines = attach_opening_braces(lines)
    lines = attach_else_lines(lines)

    # Then perform the normal formatting.
    lines = collapse_blank_lines(lines)
    lines = format_controls(lines)
    lines = attach_else_lines(lines)
    lines = bunch_compact_controls(lines)

    # Pack statements by their leading family/root before generic whitespace
    # cleanup. Existing blank lines deliberately define compact-line boundaries;
    # then redundant blanks between lines from the same family are removed
    # without merging those compact lines again.
    lines = pack_same_root_statements(lines)
    lines = bunch_same_root_spacing(lines)

    # Finally remove blank lines between any remaining exact statement groups.
    # Running this after prefix packing prevents it from erasing the boundaries
    # that tell the horizontal compactor where one physical line should end.
    lines = bunch_simple_statement_runs(lines)

    # If a compact/malformed inline if contains several simple statements,
    # restore braces around the whole sequence before enforcing hard output
    # line boundaries.
    lines = wrap_inline_multi_statement_ifs(lines)

    # terminal_writeln() is intentionally special: it always occupies its own
    # physical source line, even if the input was already horizontally packed.
    lines = enforce_terminal_writeln_lines(lines)

    # Assignments remain vertically aligned even when they share an object/root.
    # This also repairs assignment runs packed by older formatter versions.
    lines = enforce_assignment_statement_lines(lines)

    new_text = newline.join(lines)

    if had_final_newline:
        new_text += newline

    # Restore intentionally structured multiline comments before restoring asm.
    # Both were hidden from all line-oriented formatting transformations.
    new_text = restore_multiline_block_comments(new_text, preserved_comments)

    # Put every protected asm statement back byte-for-byte after all formatting
    # transformations have finished.
    new_text = restore_asm_blocks(new_text, preserved_asm)

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
        description="Format .c and .h files",
    )

    parser.add_argument(
        "path",
        type=Path,
        help="File or directory to format",
    )

    args = parser.parse_args()

    path = args.path

    if not path.exists():
        raise SystemExit(
            f"File or directory does not exist: {path}"
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

    print(
        f"Found {len(files)} C/header file(s)"
    )

    changed = 0

    for file in files:
        if format_file(file):
            print(f"Formatted: {file}")
            changed += 1

    print(
        f"Changed {changed} file(s)"
    )


if __name__ == "__main__":
    main()