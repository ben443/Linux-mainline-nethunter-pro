#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only

import argparse
import re
from pathlib import Path


NUMBER = re.compile(r"0[xX][0-9a-fA-F]+|[0-9]+")
NODE = re.compile(
    r"(?m)^[ \t]*(?:[\w-]+:\s*)?([A-Za-z0-9,._+-]+(?:@[^\s{]+)?)\s*\{"
)


def strip_comments(text):
    result = []
    i = 0
    quoted = False

    while i < len(text):
        if quoted:
            result.append(text[i])
            if text[i] == "\\" and i + 1 < len(text):
                i += 1
                result.append(text[i])
            elif text[i] == '"':
                quoted = False
            i += 1
        elif text[i] == '"':
            quoted = True
            result.append(text[i])
            i += 1
        elif text.startswith("/*", i):
            end = text.find("*/", i + 2)
            if end < 0:
                raise ValueError("unterminated block comment")
            result.append("\n" * text[i:end + 2].count("\n"))
            i = end + 2
        elif text.startswith("//", i):
            end = text.find("\n", i + 2)
            if end < 0:
                break
            result.append("\n")
            i = end + 1
        else:
            result.append(text[i])
            i += 1

    return "".join(result)


def matching_brace(text, opening):
    depth = 0
    quoted = False
    escaped = False

    for i in range(opening, len(text)):
        char = text[i]
        if quoted:
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == '"':
                quoted = False
        elif char == '"':
            quoted = True
        elif char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return i

    raise ValueError("unterminated device-tree node")


def property_cell_count(body, name):
    match = re.search(
        rf"{re.escape(name)}\s*=\s*<\s*(0[xX][0-9a-fA-F]+|[0-9]+)\s*>;",
        body,
    )
    if not match:
        raise ValueError(f"missing {name} in reserved-memory")
    return int(match.group(1), 0)


def combine_cells(cells):
    value = 0
    for cell in cells:
        value = (value << 32) | cell
    return value


def reserved_regions(text):
    text = strip_comments(text)
    match = re.search(
        r"(?m)^[ \t]*(?:[\w-]+:\s*)?reserved-memory\s*\{", text
    )
    if not match:
        raise ValueError("no reserved-memory node found")

    opening = text.index("{", match.start())
    closing = matching_brace(text, opening)
    body = text[opening + 1:closing]
    address_cells = property_cell_count(body, "#address-cells")
    size_cells = property_cell_count(body, "#size-cells")
    stride = address_cells + size_cells
    if not address_cells or not size_cells or not stride:
        raise ValueError("reserved-memory cell counts must be non-zero")

    regions = []
    position = 0
    while node := NODE.search(body, position):
        node_open = body.index("{", node.start())
        node_close = matching_brace(body, node_open)
        node_body = body[node_open + 1:node_close]
        reg = re.search(r"\breg\s*=\s*([^;]+);", node_body, re.S)
        if reg:
            values = [
                int(value, 0)
                for group in re.findall(r"<([^>]*)>", reg.group(1))
                for value in NUMBER.findall(group)
            ]
            if not values or len(values) % stride:
                raise ValueError(f"unexpected reg cell count in {node.group(1)}")

            for offset in range(0, len(values), stride):
                start = combine_cells(values[offset:offset + address_cells])
                size = combine_cells(
                    values[offset + address_cells:offset + stride]
                )
                if size:
                    regions.append((node.group(1), start, start + size))

        position = node_close + 1

    return regions


def main():
    parser = argparse.ArgumentParser(
        description="Find overlapping reserved-memory reg ranges in a decompiled DTS"
    )
    parser.add_argument("dts", type=Path, help="decompiled DTS file")
    args = parser.parse_args()

    try:
        regions = reserved_regions(args.dts.read_text())
    except (OSError, ValueError) as error:
        parser.error(str(error))

    overlaps = 0
    for i, (name_a, start_a, end_a) in enumerate(regions):
        for name_b, start_b, end_b in regions[i + 1:]:
            start = max(start_a, start_b)
            end = min(end_a, end_b)
            if start < end:
                print(f"{name_a} overlaps {name_b}: [{start:#x}, {end:#x})")
                overlaps += 1

    if not overlaps:
        print("No reserved-memory overlaps found")
    return bool(overlaps)


if __name__ == "__main__":
    raise SystemExit(main())
