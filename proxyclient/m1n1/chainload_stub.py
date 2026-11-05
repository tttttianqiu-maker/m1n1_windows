# SPDX-License-Identifier: MIT
"""Pre-encoded chainload relocation stub, so chainload needs no host AArch64 toolchain.

Equivalent to:
    1:  ldp x4, x5, [x1], #16
        stp x4, x5, [x2]
        dc cvau, x2
        ic ivau, x2
        add x2, x2, #16
        sub x3, x3, #16
        cbnz x3, 1b
        ldr x1, 2f
        br x1
        nop             // keeps the literal 8-byte aligned
    2:  .quad entry
"""
import struct

STUB_WORDS = (
    0xA8C11424, 0xA9001444, 0xD50B7B22, 0xD50B7522, 0x91004042,
    0xD1004063, 0xB5FFFF43, 0x58000061, 0xD61F0020, 0xD503201F,
)


class ChainloadStub:
    def __init__(self, entry: int, addr: int):
        if addr % 8:
            raise ValueError(f"stub address {addr:#x} must be 8-byte aligned")
        self.addr = addr
        self.data = struct.pack("<10IQ", *STUB_WORDS, entry)
        self.len = len(self.data)
