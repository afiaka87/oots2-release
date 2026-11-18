"""NVN / Tegra block-linear address math.

A block-linear surface is stored as 64-byte x 8-row *GOBs*. ``block_height`` GOBs are
stacked vertically into one "block"; blocks run left to right, then top to bottom.

Everything below counts in ELEMENTS, where an element is one compressed block for BC/ASTC
formats and one pixel for uncompressed ones. Callers convert with
``bytes_per_element`` and the format's element footprint.

Verified against the shipped ``nvn_font_jis1.ntx``: 320 elements across x 8 B = 2560 B =
40 GOBs; 40 x 48 GOB-rows x 512 B = 983,040 B, which is exactly that file's payload and
forces ``block_height = 16``. Decoding with these parameters reproduced the font's ASCII
rows legibly, which is the proof the math is right.
"""

from __future__ import annotations

GOB_WIDTH_BYTES = 64
GOB_HEIGHT_ROWS = 8
GOB_BYTES = GOB_WIDTH_BYTES * GOB_HEIGHT_ROWS


def gob_offset(x_byte: int, y_row: int) -> int:
    """Byte offset of ``(x_byte, y_row)`` inside a single 64x8 GOB."""
    return (
        ((x_byte % 64) // 32) * 256
        + ((y_row % 8) // 2) * 64
        + ((x_byte % 32) // 16) * 32
        + (y_row % 2) * 16
        + (x_byte % 16)
    )


def gobs_per_row(width_elements: int, bytes_per_element: int) -> int:
    """How many GOBs one row of elements occupies, rounded up."""
    row_bytes = width_elements * bytes_per_element
    return (row_bytes + GOB_WIDTH_BYTES - 1) // GOB_WIDTH_BYTES


def gob_rows(height_elements: int, block_height: int) -> int:
    """Total GOB rows, rounded up to a whole number of blocks."""
    rows = (height_elements + GOB_HEIGHT_ROWS - 1) // GOB_HEIGHT_ROWS
    return (rows + block_height - 1) // block_height * block_height


def swizzled_size(
    width_elements: int, height_elements: int, bytes_per_element: int, block_height: int
) -> int:
    """Size in bytes of the padded, swizzled surface."""
    return (
        gobs_per_row(width_elements, bytes_per_element)
        * gob_rows(height_elements, block_height)
        * GOB_BYTES
    )


def element_offset(
    x_element: int,
    y_element: int,
    width_elements: int,
    bytes_per_element: int,
    block_height: int,
) -> int:
    """Byte offset of one element in the swizzled surface."""
    x_byte = x_element * bytes_per_element
    per_row = gobs_per_row(width_elements, bytes_per_element)
    gob_x = x_byte // GOB_WIDTH_BYTES
    block_y, gob_in_block = divmod(y_element // GOB_HEIGHT_ROWS, block_height)
    return (
        block_y * per_row * block_height * GOB_BYTES
        + gob_x * block_height * GOB_BYTES
        + gob_in_block * GOB_BYTES
        + gob_offset(x_byte, y_element)
    )


def deswizzle(
    data: bytes,
    width_elements: int,
    height_elements: int,
    bytes_per_element: int,
    block_height: int,
) -> bytearray:
    """Swizzled surface -> tightly packed row-major elements."""
    out = bytearray(width_elements * height_elements * bytes_per_element)
    limit = len(data)
    for y in range(height_elements):
        row = y * width_elements * bytes_per_element
        for x in range(width_elements):
            src = element_offset(x, y, width_elements, bytes_per_element, block_height)
            if src + bytes_per_element > limit:
                continue
            dst = row + x * bytes_per_element
            out[dst : dst + bytes_per_element] = data[src : src + bytes_per_element]
    return out


def swizzle(
    linear: bytes,
    width_elements: int,
    height_elements: int,
    bytes_per_element: int,
    block_height: int,
) -> bytearray:
    """Tightly packed row-major elements -> swizzled surface (inverse of :func:`deswizzle`)."""
    out = bytearray(
        swizzled_size(width_elements, height_elements, bytes_per_element, block_height)
    )
    for y in range(height_elements):
        row = y * width_elements * bytes_per_element
        for x in range(width_elements):
            dst = element_offset(x, y, width_elements, bytes_per_element, block_height)
            src = row + x * bytes_per_element
            out[dst : dst + bytes_per_element] = linear[src : src + bytes_per_element]
    return out
