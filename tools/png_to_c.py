#!/usr/bin/env python3

import argparse
from pathlib import Path

from PIL import Image


def parse_rgb(value: str) -> tuple[int, int, int]:
    value = value.lstrip("#")

    if len(value) != 6:
        raise argparse.ArgumentTypeError(
            "background must be RRGGBB, for example 090E16"
        )

    try:
        return (
            int(value[0:2], 16),
            int(value[2:4], 16),
            int(value[4:6], 16),
        )
    except ValueError as exc:
        raise argparse.ArgumentTypeError(
            "background must contain hexadecimal digits"
        ) from exc


def composite_pixel(
    r: int,
    g: int,
    b: int,
    a: int,
    background: tuple[int, int, int],
) -> tuple[int, int, int]:
    if a == 255:
        return r, g, b

    br, bg, bb = background

    r = (r * a + br * (255 - a)) // 255
    g = (g * a + bg * (255 - a)) // 255
    b = (b * a + bb * (255 - a)) // 255

    return r, g, b


def encode_pixel(
    r: int,
    g: int,
    b: int,
    pixel_format: str,
) -> int:
    if pixel_format == "rgb":
        # 0x00RRGGBB
        return (r << 16) | (g << 8) | b

    if pixel_format == "bgr":
        # 0x00BBGGRR
        return (b << 16) | (g << 8) | r

    raise ValueError(f"unsupported format: {pixel_format}")


def write_header(
    output: Path,
    name: str,
    width: int,
    height: int,
) -> None:
    guard = f"JA_OS_{name.upper()}_H"

    text = f"""#ifndef {guard}
#define {guard}

#include "types.h"

#define BOOT_LOGO_WIDTH  {width}U
#define BOOT_LOGO_HEIGHT {height}U

extern const u32 {name}[BOOT_LOGO_WIDTH * BOOT_LOGO_HEIGHT]
    __attribute__((visibility("hidden")));

#endif
"""

    output.write_text(text, encoding="utf-8")


def write_source(
    output: Path,
    header_name: str,
    name: str,
    pixels: list[int],
    width: int,
) -> None:
    with output.open("w", encoding="utf-8", newline="\n") as f:
        f.write(f'#include "{header_name}"\n\n')

        f.write(
            f"const u32 {name}[BOOT_LOGO_WIDTH * BOOT_LOGO_HEIGHT]\n"
            '    __attribute__((visibility("hidden"))) = {\n'
        )

        for y in range(len(pixels) // width):
            f.write(f"    /* row {y} */\n    ")

            row_start = y * width

            for x in range(width):
                value = pixels[row_start + x]

                f.write(f"0x{value:08X}U")

                if row_start + x != len(pixels) - 1:
                    f.write(", ")

                if (x + 1) % 8 == 0 and x + 1 < width:
                    f.write("\n    ")

            f.write("\n")

        f.write("};\n")


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Convert a PNG image into a JA OS u32 pixel array."
    )

    parser.add_argument(
        "input",
        type=Path,
        help="Input PNG image",
    )

    parser.add_argument(
        "output_c",
        type=Path,
        help="Output C source file",
    )

    parser.add_argument(
        "output_h",
        type=Path,
        help="Output header file",
    )

    parser.add_argument(
        "--name",
        default="boot_logo",
        help="C array name (default: boot_logo)",
    )

    parser.add_argument(
        "--format",
        choices=("rgb", "bgr"),
        default="rgb",
        help="Pixel order (default: rgb)",
    )

    parser.add_argument(
        "--background",
        type=parse_rgb,
        default=parse_rgb("090E16"),
        help=(
            "Background used for transparent PNG pixels, "
            "as RRGGBB (default: 090E16)"
        ),
    )

    args = parser.parse_args()

    if not args.input.exists():
        raise SystemExit(f"input image does not exist: {args.input}")

    image = Image.open(args.input).convert("RGBA")

    width, height = image.size

    if width == 0 or height == 0:
        raise SystemExit("image has invalid dimensions")

    pixels: list[int] = []

    for r, g, b, a in image.getdata():
        r, g, b = composite_pixel(
            r,
            g,
            b,
            a,
            args.background,
        )

        pixels.append(
            encode_pixel(
                r,
                g,
                b,
                args.format,
            )
        )

    args.output_c.parent.mkdir(
        parents=True,
        exist_ok=True,
    )

    args.output_h.parent.mkdir(
        parents=True,
        exist_ok=True,
    )

    write_header(
        args.output_h,
        args.name,
        width,
        height,
    )

    write_source(
        args.output_c,
        args.output_h.name,
        args.name,
        pixels,
        width,
    )

    print(
        f"Converted {args.input} "
        f"({width}x{height})"
    )

    print(f"Wrote {args.output_c}")
    print(f"Wrote {args.output_h}")

    print(
        f"Pixels: {width * height:,}"
    )


if __name__ == "__main__":
    main()