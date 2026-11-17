"""Run the original win32kbase.sys ballistics code under Unicorn and compare
every result with `winmouse --simulate`.

usage: uv run --with unicorn python verify_against_windows.py WIN32KBASE.SYS WINMOUSE_BIN

The addresses below belong to win32kbase.sys with PDB GUID
7BD4193FFA935A0483172A97C84B455D1 (Windows 11, October 2026 build).
"""
import random
import struct
import subprocess
import sys

from unicorn import UC_ARCH_X86, UC_HOOK_CODE, UC_MODE_64, Uc
from unicorn.x86_const import (UC_X86_REG_EAX, UC_X86_REG_EBX, UC_X86_REG_ECX, UC_X86_REG_EDI,
                               UC_X86_REG_EDX, UC_X86_REG_ESI, UC_X86_REG_R8, UC_X86_REG_R9,
                               UC_X86_REG_R13, UC_X86_REG_R14, UC_X86_REG_RAX, UC_X86_REG_RBP,
                               UC_X86_REG_RCX, UC_X86_REG_RDX, UC_X86_REG_RIP, UC_X86_REG_RSI,
                               UC_X86_REG_RSP)

UPDATE_MOUSE_SENSITIVITY = 0x1401A6920
BUILD_CURVE = 0x1401B1A24
ACCEL_CORE_START = 0x14004EA91      # after GetDeviceSpecificAccelerationData returns
ACCEL_THREADINFO_CALL = 0x14004EAFA  # callq *[IAT] -> thread info (state at +0x4f98)
ACCEL_THREADINFO_RESUME = 0x14004EB06
ACCEL_CORE_END = 0x14004EBFD
LINEAR_START = 0x14004E81F           # right after GetNormalizedMouseSensitivityFactor
LINEAR_END = 0x14004E871
LINEAR_UNITY = 0x14004E8A4
DPI_SCALE_START = 0x14004ED83        # inside GetNormalizedMouseSensitivityFactor
DPI_SCALE_END = 0x14004EDE0

STACK, DATA, RET = 0x7000_0000, 0x7100_0000, 0x7200_0000
THIS, CURVE, STATE, VARS, MON, PROC = DATA, DATA + 0x1000, DATA + 0x2000, DATA + 0x3000, DATA + 0x4000, DATA + 0x5000

DEFAULT_X = [0, 0x6E15, 0x14000, 0x3DC29, 0x280000]
DEFAULT_Y = [0, 0x111FD, 0x42400, 0x12FC00, 0x1BBC000]


def load_image(uc, path):
    d = open(path, 'rb').read()
    pe = struct.unpack_from('<I', d, 0x3C)[0]
    opt = pe + 24
    base = struct.unpack_from('<Q', d, opt + 24)[0]
    size = struct.unpack_from('<I', d, opt + 56)[0]
    uc.mem_map(base, (size + 0xFFF) & ~0xFFF)
    uc.mem_write(base, d[:0x1000])
    nsec = struct.unpack_from('<H', d, pe + 6)[0]
    sec = opt + struct.unpack_from('<H', d, pe + 20)[0]
    for i in range(nsec):
        _, vsize, va, rsize, raw = struct.unpack_from('<8sIIII', d, sec + i * 40)
        uc.mem_write(base + va, d[raw:raw + min(vsize, rsize) if vsize else rsize])


class Windows:
    def __init__(self, sys_path):
        self.uc = Uc(UC_ARCH_X86, UC_MODE_64)
        load_image(self.uc, sys_path)
        for region in (STACK, DATA, RET):
            self.uc.mem_map(region, 0x10_0000)
        self.uc.hook_add(UC_HOOK_CODE, self._threadinfo, begin=ACCEL_THREADINFO_CALL, end=ACCEL_THREADINFO_CALL)
        self.uc.hook_add(UC_HOOK_CODE, lambda uc, *a: uc.emu_stop(), begin=LINEAR_UNITY, end=LINEAR_UNITY)

    def _threadinfo(self, uc, address, size, user):
        uc.reg_write(UC_X86_REG_RAX, STATE - 0x4F98)
        uc.reg_write(UC_X86_REG_RIP, ACCEL_THREADINFO_RESUME)

    def call(self, fn, regs):
        uc = self.uc
        rsp = STACK + 0x8_0000
        uc.mem_write(rsp, struct.pack('<Q', RET))
        uc.reg_write(UC_X86_REG_RSP, rsp)
        for reg, val in regs.items():
            uc.reg_write(reg, val)
        uc.emu_start(fn, RET)

    def linear_factor(self, sens):
        self.call(UPDATE_MOUSE_SENSITIVITY, {UC_X86_REG_RCX: THIS, UC_X86_REG_RDX: sens})
        return struct.unpack('<iI', self.uc.mem_read(THIS, 8))[1]

    def normalized_factor(self, factor, dpi):
        self.uc.mem_write(MON, b'\0' * 0x20)
        self.uc.mem_write(MON + 0x14, struct.pack('<H', dpi))
        self.uc.reg_write(UC_X86_REG_RSP, STACK + 0x8_0000)
        self.uc.reg_write(UC_X86_REG_RSI, MON)
        self.uc.reg_write(UC_X86_REG_EBX, factor)
        self.uc.emu_start(DPI_SCALE_START, DPI_SCALE_END)
        return struct.unpack('<i', struct.pack('<I', self.uc.reg_read(UC_X86_REG_EBX)))[0]

    def build_curve(self, sens, dpi, cx=DEFAULT_X, cy=DEFAULT_Y):
        self.uc.mem_write(THIS + 0x10, struct.pack('<5q', *cx))
        self.uc.mem_write(THIS + 0x38, struct.pack('<5q', *cy))
        self.call(BUILD_CURVE, {UC_X86_REG_RCX: THIS, UC_X86_REG_RDX: CURVE,
                                  UC_X86_REG_R8: dpi, UC_X86_REG_R9: sens})
        raw = struct.unpack('<18q', self.uc.mem_read(CURVE, 18 * 8))
        self.uc.mem_write(STATE, b'\0' * 0x18)
        return raw

    def accelerate(self, dx, dy):
        uc = self.uc
        rbp = STACK + 0x4_0000
        uc.mem_write(VARS, struct.pack('<ii', dx, dy))
        uc.mem_write(rbp + 0x48, struct.pack('<QQI', VARS, VARS + 4, 5))
        uc.mem_write(rbp - 0x30, struct.pack('<QQQ', CURVE, CURVE + 0x70, CURVE + 0x50))
        uc.reg_write(UC_X86_REG_RBP, rbp)
        uc.reg_write(UC_X86_REG_RSP, rbp - 0x70)
        uc.reg_write(UC_X86_REG_R13, VARS + 0x10)
        uc.reg_write(UC_X86_REG_ESI, 0)
        uc.emu_start(ACCEL_CORE_START, ACCEL_CORE_END)
        return struct.unpack('<ii', uc.mem_read(VARS, 8))

    def linear(self, factor, dx, dy, rem):
        uc = self.uc
        uc.mem_write(PROC + 0x24, struct.pack('<ii', *rem))
        uc.reg_write(UC_X86_REG_RSP, STACK + 0x8_0000)
        uc.reg_write(UC_X86_REG_RBP, STACK + 0x4_0000)
        uc.reg_write(UC_X86_REG_EAX, factor)
        uc.reg_write(UC_X86_REG_ESI, dx & 0xFFFFFFFF)
        uc.reg_write(UC_X86_REG_EDI, dy & 0xFFFFFFFF)
        uc.reg_write(UC_X86_REG_R14, PROC)
        uc.emu_start(LINEAR_START, LINEAR_END)
        if uc.reg_read(UC_X86_REG_RIP) == LINEAR_UNITY:
            return (dx, dy), rem
        s32 = lambda r: struct.unpack('<i', struct.pack('<I', uc.reg_read(r) & 0xFFFFFFFF))[0]
        return (s32(UC_X86_REG_ESI), s32(UC_X86_REG_EDI)), struct.unpack('<ii', uc.mem_read(PROC + 0x24, 8))


def packets(rng, n):
    out = []
    for _ in range(n):
        scale = rng.choice([1, 2, 4, 8, 16, 40, 120, 400])
        out.append((rng.randint(-scale, scale), rng.randint(-scale, scale)))
    return out


def simulate(binary, sens, epp, dpi, pkts):
    text = ''.join(f'{x} {y}\n' for x, y in pkts)
    res = subprocess.run([binary, '--simulate', str(sens), str(epp), str(dpi)], input=text,
                         capture_output=True, text=True, check=True, env={'WINMOUSE_PRINT_CURVE': '1'})
    lines = res.stdout.splitlines()
    curve = [int(v) for l in lines if l.startswith(('point', 'segment')) for v in l.split()[1:]]
    factor = next(int(l.split()[1]) for l in lines if l.startswith('factor'))
    moves = [tuple(map(int, l.split())) for l in lines if l[0] in '-0123456789']
    return curve, factor, moves


def main():
    win = Windows(sys.argv[1])
    binary = sys.argv[2]
    rng = random.Random(1234)
    checked = 0
    for sens in range(1, 21):
        assert win.linear_factor(sens) == simulate(binary, sens, 0, 96, [])[1], sens
    for dpi in (96, 120, 144, 168, 192):
        for sens in range(1, 21):
            pkts = packets(rng, 3000)
            # EPP on: curve tables and every accelerated packet.
            curve, _, moves = simulate(binary, sens, 1, dpi, pkts)
            raw = win.build_curve(sens, dpi)
            mine = curve[0:10:2] + curve[1:10:2] + curve[10::2] + curve[11::2]
            assert list(raw) == mine, (sens, dpi, raw, mine)
            for p, m in zip(pkts, moves):
                w = win.accelerate(*p) if p != (0, 0) else p
                assert w == m, (sens, dpi, p, w, m)
                checked += 1
            # EPP off: sensitivity factor, DPI normalisation and remainders.
            _, factor, moves = simulate(binary, sens, 0, dpi, pkts)
            wf = win.linear_factor(sens)
            if dpi > 96:
                wf = win.normalized_factor(wf, dpi)
            assert wf == factor, (sens, dpi, wf, factor)
            rem = (0, 0)
            for p, m in zip(pkts, moves):
                w, rem = win.linear(wf, *p, rem)
                assert tuple(w) == m, (sens, dpi, p, w, m)
                checked += 1
    print(f'OK: {checked} packets identical to win32kbase.sys across sensitivity 1-20, '
          f'EPP on/off, DPI 96/120/144/168/192')


if __name__ == '__main__':
    main()
