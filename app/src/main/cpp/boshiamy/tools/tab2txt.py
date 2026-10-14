#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Convert Boshiamy liu-uni.tab binary table into plain-text boshiamy.txt."""

import math
import struct
import sys


def convert_liu_unitab(tab_filename: str, out_filename: str) -> int:
    idx_to_key = " abcdefghijklmnopqrstuvwxyz,.'[]"
    with open(tab_filename, "rb") as f:
        key_table = struct.unpack("<1025H", f.read(1025 * 2))
        n_words = key_table[-1]

        high_bits = f.read(math.ceil(n_words * 2 / 8))
        _unknown_bits = f.read(math.ceil(n_words / 8))
        _is_simple_bits = f.read(math.ceil(n_words / 8))

        words = []
        for i in range(n_words):
            word_data = f.read(3)
            if len(word_data) < 3:
                break
            val24 = (word_data[0] << 16) | (word_data[1] << 8) | word_data[2]
            key3 = (val24 >> 19) & 0x1F
            key4 = (val24 >> 14) & 0x1F
            low_word = val24 & 0x3FFF

            i_byte, start_bit = divmod(i * 2, 8)
            high_word = (high_bits[i_byte] >> (6 - start_bit)) & 0x03

            codepoint = (high_word << 14) | low_word
            word = chr(codepoint) if codepoint else ""
            words.append((key3, key4, word))
        words.append((0, 0, None))

    for n_skip in range(0, min(32, len(words))):
        word = words[n_skip][2]
        if word and word in "對夺对対":
            if n_skip:
                words = words[n_skip:]
            break

    count = 0
    with open(out_filename, "w", encoding="utf-8") as out:
        for i_key in range(32, len(key_table) - 1):
            key1, key2 = divmod(i_key, 32)
            i_start_word = key_table[i_key]
            i_next_word = key_table[i_key + 1]
            for i in range(i_start_word, i_next_word):
                key3, key4, word = words[i]
                if not word:
                    continue
                code = "".join(
                    idx_to_key[k] for k in (key1, key2, key3, key4) if k != 0
                )
                out.write(f"{code} {word}\n")
                count += 1
    return count


def main() -> None:
    if len(sys.argv) < 3:
        print(f"Usage: {sys.argv[0]} <liu-uni.tab> <boshiamy.txt>", file=sys.stderr)
        sys.exit(1)
    total = convert_liu_unitab(sys.argv[1], sys.argv[2])
    print(f"Converted {total} entries to {sys.argv[2]}")


if __name__ == "__main__":
    main()
