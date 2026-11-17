#!/usr/bin/env python3
"""Regression tests for tools/cst8xx_wake_patch.py (issue #325).

Covers:
  1. CLI state machine (--check, patch, idempotent re-patch, --revert,
     --no-backup, malformed inputs, and coexistence in either order with
     cst8xx_multitouch_patch.py).
  2. ELF integrity on the bundled TempoTec V1 1.3 cst8xx_touch.ko: identical
     file size, headers, sections, symbols and relocations; byte-for-byte
     restore on --revert; only the parse loop in .text changes.
  3. MIPS32r2 execution of the module's real hyn_irq_handler machine code
     paired with Linux 4.4's input_handle_event() deduplication, comparing the
     stock module against the patched one across wake, taps (including (0, 0)
     and panel corners), slides, mid-touch blanks, missed TOUCH_UP, gesture-only
     frames, and two-finger multitouch.
"""

import hashlib
import io
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import zipfile

TESTS_DIR = os.path.dirname(os.path.abspath(__file__))
PLAYER_DIR = os.path.dirname(TESTS_DIR)
REPO_DIR = os.path.dirname(PLAYER_DIR)
WAKE_TOOL = os.path.join(PLAYER_DIR, "tools", "cst8xx_wake_patch.py")
MT_TOOL = os.path.join(PLAYER_DIR, "tools", "cst8xx_multitouch_patch.py")
STOCK_PART1 = os.path.join(REPO_DIR, "sonix-packer", "firmware-input", "v1.zip.part1")
STOCK_PART2 = os.path.join(REPO_DIR, "sonix-packer", "firmware-input", "v1.zip.part2")

sys.path.insert(0, os.path.join(PLAYER_DIR, "tools"))
import cst8xx_wake_patch as wake_mod  # noqa: E402

# Linux input event constants.
EV_SYN = 0
EV_KEY = 1
EV_ABS = 3
SYN_REPORT = 0
SYN_MT_REPORT = 2
BTN_TOUCH = 330
ABS_MT_TOUCH_MAJOR = 48
ABS_MT_POSITION_X = 53
ABS_MT_POSITION_Y = 54
ABS_MT_TRACKING_ID = 57
ABS_MT_PRESSURE = 58


def _lzo1x_decompress(src):
    """Minimal LZO1X-1 decompressor for SquashFS 4.0 LZO metadata and blocks."""
    out = bytearray()
    ip = 0
    t = 0
    if src[ip] > 17:
        t = src[ip] - 17
        ip += 1
        if t < 4:
            state = "match_next"
        else:
            out.extend(src[ip:ip + t])
            ip += t
            state = "first_literal_run"
    else:
        state = "top"

    while True:
        if state == "top":
            t = src[ip]
            ip += 1
            if t >= 16:
                state = "match"
                continue
            if t == 0:
                while src[ip] == 0:
                    t += 255
                    ip += 1
                t += 15 + src[ip]
                ip += 1
            out.extend(src[ip:ip + t + 3])
            ip += t + 3
            state = "first_literal_run"
        if state == "first_literal_run":
            t = src[ip]
            ip += 1
            if t >= 16:
                state = "match"
                continue
            m_pos = len(out) - (1 + 0x0800) - (t >> 2) - (src[ip] << 2)
            ip += 1
            for _ in range(3):
                out.append(out[m_pos])
                m_pos += 1
            state = "match_done"
        if state == "match":
            if t >= 64:
                m_pos = len(out) - 1 - ((t >> 2) & 7) - (src[ip] << 3)
                ip += 1
                m_len = (t >> 5) + 1
            elif t >= 32:
                m_len = t & 31
                if m_len == 0:
                    while src[ip] == 0:
                        m_len += 255
                        ip += 1
                    m_len += 31 + src[ip]
                    ip += 1
                m_len += 2
                v = src[ip] | (src[ip + 1] << 8)
                ip += 2
                m_pos = len(out) - 1 - (v >> 2)
            elif t >= 16:
                m_pos = len(out) - ((t & 8) << 11)
                m_len = t & 7
                if m_len == 0:
                    while src[ip] == 0:
                        m_len += 255
                        ip += 1
                    m_len += 7 + src[ip]
                    ip += 1
                v = src[ip] | (src[ip + 1] << 8)
                ip += 2
                m_pos -= v >> 2
                if m_pos == len(out):
                    return bytes(out)
                m_pos -= 0x4000
                m_len += 2
            else:
                m_pos = len(out) - 1 - (t >> 2) - (src[ip] << 2)
                ip += 1
                m_len = 2
            for _ in range(m_len):
                out.append(out[m_pos])
                m_pos += 1
            state = "match_done"
        if state == "match_done":
            t = src[ip - 2] & 3
            if t == 0:
                state = "top"
                continue
            state = "match_next"
        if state == "match_next":
            out.extend(src[ip:ip + t])
            ip += t
            t = src[ip]
            ip += 1
            state = "match"


def _iso_list_dir(iso, extent, size):
    entries = {}
    pos = extent * 2048
    end = pos + size
    while pos < end:
        rec_len = iso[pos]
        if rec_len == 0:
            pos = ((pos // 2048) + 1) * 2048
            continue
        ext, = struct.unpack_from("<I", iso, pos + 2)
        dlen, = struct.unpack_from("<I", iso, pos + 10)
        nlen = iso[pos + 32]
        raw_name = iso[pos + 33:pos + 33 + nlen]
        su = pos + 33 + nlen + (1 if nlen % 2 == 0 else 0)
        nm = []
        while su + 4 <= pos + rec_len:
            sig, slen = iso[su:su + 2], iso[su + 2]
            if slen < 4:
                break
            if sig == b"NM":
                nm.append(iso[su + 5:su + slen].decode("latin1"))
            su += slen
        name = "".join(nm) if nm else raw_name.decode("latin1").split(";")[0]
        if raw_name not in (b"\x00", b"\x01"):
            entries[name] = (ext, dlen)
        pos += rec_len
    return entries


def _squashfs_read_files(sqfs, paths):
    hdr = struct.unpack_from("<IIIIIHHHHHHQQQQQQQQ", sqfs, 0)
    magic, block_size, comp = hdr[0], hdr[3], hdr[5]
    root_inode, inode_table, dir_table, frag_table = hdr[11], hdr[15], hdr[16], hdr[17]
    if magic != 0x73717368 or comp != 3:  # 'hsqs', LZO
        return None

    meta_cache = {}

    def meta_block(off):
        if off not in meta_cache:
            h, = struct.unpack_from("<H", sqfs, off)
            clen = h & 0x7FFF
            raw = sqfs[off + 2:off + 2 + clen]
            meta_cache[off] = (raw if (h & 0x8000) else _lzo1x_decompress(raw), off + 2 + clen)
        return meta_cache[off]

    def meta_read(base, blk_rel, in_off, n):
        off, pos = base + blk_rel, in_off
        out = bytearray()
        while len(out) < n:
            d, nxt = meta_block(off)
            take = min(len(d) - pos, n - len(out))
            out.extend(d[pos:pos + take])
            pos += take
            if pos == len(d):
                off, pos = nxt, 0
        return bytes(out)

    def list_dir(ref):
        itype, = struct.unpack_from("<H", meta_read(inode_table, ref >> 16, ref & 0xFFFF, 2), 0)
        if itype == 1:
            raw = meta_read(inode_table, ref >> 16, ref & 0xFFFF, 32)
            sblk, _, fsize, doff, _ = struct.unpack_from("<IIHHI", raw, 16)
        else:
            raw = meta_read(inode_table, ref >> 16, ref & 0xFFFF, 40)
            _, fsize, sblk, _, _, doff, _ = struct.unpack_from("<IIIIHHI", raw, 16)
        stream = meta_read(dir_table, sblk, doff, fsize - 3)
        p, entries = 0, {}
        while p < len(stream):
            count, d_start, _ = struct.unpack_from("<III", stream, p)
            p += 12
            for _ in range(count + 1):
                e_off, _, _, sz = struct.unpack_from("<HhHH", stream, p)
                p += 8
                name = stream[p:p + sz + 1].decode("utf-8")
                p += sz + 1
                entries[name] = (d_start << 16) | e_off
        return entries

    def read_one(rel_path):
        ref = root_inode
        for part in rel_path.strip("/").split("/"):
            ref = list_dir(ref)[part]
        itype, = struct.unpack_from("<H", meta_read(inode_table, ref >> 16, ref & 0xFFFF, 2), 0)
        if itype == 2:
            raw = meta_read(inode_table, ref >> 16, ref & 0xFFFF, 32)
            bstart, frag, foff, fsize = struct.unpack_from("<IIII", raw, 16)
            hlen = 32
        else:
            raw = meta_read(inode_table, ref >> 16, ref & 0xFFFF, 56)
            bstart, fsize, _, _, frag, foff, _ = struct.unpack_from("<QQQIIII", raw, 16)
            hlen = 56
        nblk = (fsize // block_size) if frag != 0xFFFFFFFF else ((fsize + block_size - 1) // block_size)
        full = meta_read(inode_table, ref >> 16, ref & 0xFFFF, hlen + 4 * nblk)
        bsizes = struct.unpack_from("<%dI" % nblk, full, hlen)
        out, pos = bytearray(), bstart
        for bs in bsizes:
            if bs == 0:
                out.extend(b"\x00" * block_size)
            else:
                chunk = sqfs[pos:pos + (bs & 0xFFFFFF)]
                pos += bs & 0xFFFFFF
                out.extend(chunk if (bs & (1 << 24)) else _lzo1x_decompress(chunk))
        if frag != 0xFFFFFFFF:
            mblk, = struct.unpack_from("<Q", sqfs, frag_table + (frag // 512) * 8)
            fd, _ = meta_block(mblk)
            fstart, fsz, _ = struct.unpack_from("<QII", fd, (frag % 512) * 16)
            raw = sqfs[fstart:fstart + (fsz & 0xFFFFFF)]
            fb = raw if (fsz & (1 << 24)) else _lzo1x_decompress(raw)
            out.extend(fb[foff:foff + (fsize - len(out))])
        return bytes(out[:fsize])

    return [read_one(p) for p in paths]


def _extract_stock_module_from_upt():
    """Extracts cst8xx_touch.ko and cst8xx_touch.sh directly from the bundled
    TempoTec V1 1.3 split zip in pure Python (no 7z or unsquashfs needed)."""
    if not (os.path.isfile(STOCK_PART1) and os.path.isfile(STOCK_PART2)):
        return None
    buf = io.BytesIO()
    for part in (STOCK_PART1, STOCK_PART2):
        with open(part, "rb") as src:
            shutil.copyfileobj(src, buf)
    buf.seek(0)
    with zipfile.ZipFile(buf) as zf:
        upt_names = [n for n in zf.namelist() if n.lower().endswith(".upt")]
        if not upt_names:
            return None
        iso = zf.read(upt_names[0])
    rext, = struct.unpack_from("<I", iso, 16 * 2048 + 158)
    rlen, = struct.unpack_from("<I", iso, 16 * 2048 + 166)
    root = _iso_list_dir(iso, rext, rlen)
    ota_key = next(k for k in root if k.lower() == "ota_v0")
    ota = _iso_list_dir(iso, *root[ota_key])
    chunks = sorted(k for k in ota if k.lower().startswith("rootfs.squashfs."))
    sqfs = b"".join(iso[ota[k][0] * 2048:ota[k][0] * 2048 + ota[k][1]] for k in chunks)
    got = _squashfs_read_files(
        sqfs, ["module_driver/cst8xx_touch.ko", "module_driver/cst8xx_touch.sh"]
    )
    if not got:
        return None
    return got[0], got[1].decode("utf-8")


def _run_tool(script, *args):
    return subprocess.run(
        [sys.executable, script, *args],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )


def _parse_elf32(data):
    e_shoff, = struct.unpack_from("<I", data, 32)
    e_shentsize, e_shnum, e_shstrndx = struct.unpack_from("<HHH", data, 46)

    def shdr(i):
        return struct.unpack_from("<10I", data, e_shoff + i * e_shentsize)

    shstr_off = shdr(e_shstrndx)[4]

    def cstr(buf, off):
        end = buf.index(b"\x00", off)
        return buf[off:end].decode("utf-8", "replace")

    secs = {}
    for i in range(e_shnum):
        s = shdr(i)
        secs[cstr(data, shstr_off + s[0])] = s

    sym_s = secs[".symtab"]
    str_s = secs[".strtab"]
    syms = []
    for i in range(sym_s[5] // 16):
        st_name, st_val, st_size, st_info, st_other, st_shndx = struct.unpack_from(
            "<IIIBBH", data, sym_s[4] + i * 16
        )
        syms.append((cstr(data, str_s[4] + st_name), st_val, st_size, st_info, st_shndx))
    return secs, syms


class HynIrqHarness:
    """Minimal MIPS32r2 LE interpreter running cst8xx_touch.ko's hyn_irq_handler
    with stubbed kernel helpers (hyn_i2c_read, input_event, kfree, etc.) and
    Linux 4.4's exact input_handle_event() deduplication for EV_KEY vs MT-A."""

    TEXT_BASE = 0x10000000
    STUB_BASE = 0x20000000
    DATA_BASE = 0x30000000
    STACK_TOP = 0x30080000
    MEM_SIZE = 0x00100000

    # Struct layout verified from cst8xx_touch.ko disassembly.
    TS_OFF = 0x1000       # struct hyn_ts_data (s0)
    PDATA_OFF = 0x2000    # struct hyn_ts_platform_data
    INPUT_OFF = 0x3000    # struct input_dev
    CLIENT_OFF = 0x3400   # struct i2c_client
    EVENTS_OFF = 0x4000   # struct ts_event[max_touch] (28 B each; cnt at +24)
    PNTBUF_OFF = 0x5000   # u8 pnt_buf[6 * max_touch + 4]
    TMP_OFF = 0x6000      # scratch for stubbed calls

    def __init__(self, ko_bytes, max_touch=None):
        secs, syms = _parse_elf32(ko_bytes)
        text_s = secs[".text"]
        self.text = bytearray(ko_bytes[text_s[4]:text_s[4] + text_s[5]])
        self.mem = bytearray(self.MEM_SIZE)

        # Detect max_touch_number from .init.text (`addiu v0, zero, N; sw v0, 0x58(s1)`).
        if max_touch is None:
            init_s = secs[".init.text"]
            init_code = ko_bytes[init_s[4]:init_s[4] + init_s[5]]
            max_touch = 1
            for off in range(0, len(init_code) - 4, 4):
                w0, w1 = struct.unpack_from("<II", init_code, off)
                if (w0 & 0xFFFF0000) == 0x24020000 and w1 == 0xAE220058:
                    max_touch = w0 & 0xFFFF
                    break
        self.max_touch = max_touch

        # Relocate external calls in .text to distinct stub addresses.
        self.stubs = {}
        next_stub = self.STUB_BASE
        sym_addr = {}
        rel_s = secs[".rel.text"]
        rels = [
            struct.unpack_from("<II", ko_bytes, rel_s[4] + i * 8)
            for i in range(rel_s[5] // 8)
        ]

        # Find hyn_i2c_read's .text address so we intercept it cleanly.
        self.i2c_read_addr = None
        for name, val, _sz, _info, shndx in syms:
            if name == "hyn_i2c_read":
                self.i2c_read_addr = self.TEXT_BASE + val

        i = 0
        while i < len(rels):
            r_off, r_info = rels[i]
            r_type = r_info & 0xFF
            sym_idx = r_info >> 8
            sym_name, sym_val, _sz, _info, shndx = syms[sym_idx]
            key = sym_name if sym_name else ("sec%d" % shndx)

            if key == "hyn_data":
                addr = self.DATA_BASE + 0x0800
            elif sym_name == "" and shndx == 2:
                # Internal .text reference: addend is in the R_MIPS_LO16 word.
                addr = self.TEXT_BASE
            else:
                if key not in sym_addr:
                    sym_addr[key] = next_stub
                    self.stubs[next_stub] = key
                    next_stub += 0x10
                addr = sym_addr[key]

            if r_type == 5:  # R_MIPS_HI16 paired with R_MIPS_LO16
                hi_ins, = struct.unpack_from("<I", self.text, r_off)
                # Find the matching LO16 for the same symbol.
                lo_addend = 0
                for j in range(i + 1, min(i + 8, len(rels))):
                    lo_off, lo_info = rels[j]
                    if (lo_info & 0xFF) == 6 and (lo_info >> 8) == sym_idx:
                        lo_ins, = struct.unpack_from("<I", self.text, lo_off)
                        lo_addend = lo_ins & 0xFFFF
                        if lo_addend & 0x8000:
                            lo_addend -= 0x10000
                        break
                full = (addr + lo_addend) & 0xFFFFFFFF
                hi = ((full + 0x8000) >> 16) & 0xFFFF
                struct.pack_into("<I", self.text, r_off, (hi_ins & 0xFFFF0000) | hi)
            elif r_type == 6:  # R_MIPS_LO16
                lo_ins, = struct.unpack_from("<I", self.text, r_off)
                lo_addend = lo_ins & 0xFFFF
                if lo_addend & 0x8000:
                    lo_addend -= 0x10000
                full = (addr + lo_addend) & 0xFFFFFFFF
                struct.pack_into("<I", self.text, r_off, (lo_ins & 0xFFFF0000) | (full & 0xFFFF))
            i += 1

        # Static helper hyn_i2c_read sits at .text + 0x04b0.
        if self.i2c_read_addr is None:
            self.i2c_read_addr = self.TEXT_BASE + 0x04B0

        # Populate hyn_ts_data and pdata just as hyn_ts_init leaves them.
        ts = self.DATA_BASE + self.TS_OFF
        pdata = self.DATA_BASE + self.PDATA_OFF
        idev = self.DATA_BASE + self.INPUT_OFF
        client = self.DATA_BASE + self.CLIENT_OFF
        events = self.DATA_BASE + self.EVENTS_OFF
        pntbuf = self.DATA_BASE + self.PNTBUF_OFF

        self._sw(self.DATA_BASE + 0x0800, ts)   # hyn_data = ts
        self._sw(ts + 0, client)                 # ts->client
        self._sw(ts + 12, idev)                  # ts->input_dev
        self._sw(ts + 16, pdata)                 # ts->pdata
        self._sw(ts + 264, events)               # ts->events
        self._sw(ts + 272, pntbuf)               # ts->pnt_buf
        self._sw(ts + 276, 6 * max_touch + 3)    # ts->pnt_buf_size
        self._sw(pdata + 88, max_touch)          # pdata->max_touch_number

        # Kernel input_dev key state (for input_handle_event EV_KEY dedup).
        self.key_down = set()
        self.next_i2c_frame = b"\x00" * (6 * max_touch + 3)
        self.emitted = []

    def _off(self, addr):
        o = addr - self.DATA_BASE
        if not (0 <= o < self.MEM_SIZE):
            raise RuntimeError("bad data address 0x%08x" % addr)
        return o

    def _lw(self, addr):
        return struct.unpack_from("<I", self.mem, self._off(addr))[0]

    def _lbu(self, addr):
        return self.mem[self._off(addr)]

    def _sw(self, addr, val):
        struct.pack_into("<I", self.mem, self._off(addr), val & 0xFFFFFFFF)

    def _sb(self, addr, val):
        self.mem[self._off(addr)] = val & 0xFF

    @staticmethod
    def _s32(v):
        v &= 0xFFFFFFFF
        return v - 0x100000000 if (v & 0x80000000) else v

    def event_cnt(self, idx=0):
        return self._lw(self.DATA_BASE + self.EVENTS_OFF + 28 * idx + 24)

    def set_event_cnt(self, idx, val):
        self._sw(self.DATA_BASE + self.EVENTS_OFF + 28 * idx + 24, val)

    def touchs(self):
        return self._lw(self.DATA_BASE + self.TS_OFF + 280)

    def simulate_resume_prefix(self):
        """What hyn_resume_work does right before enable_irq() triggers the
        replayed PA16 edge: input_mt_sync, BTN_TOUCH 0, SYN_REPORT, zero touchs
        and key_state (leaving events[i].cnt untouched)."""
        out = []
        self._kernel_input_event(out, EV_SYN, SYN_MT_REPORT, 0)
        self._kernel_input_event(out, EV_KEY, BTN_TOUCH, 0)
        self._kernel_input_event(out, EV_SYN, SYN_REPORT, 0)
        ts = self.DATA_BASE + self.TS_OFF
        self._sw(ts + 280, 0)
        self._sw(ts + 284, 0)
        return out

    def _kernel_input_event(self, log, ev_type, code, value):
        """Mirrors vmlinux input_handle_event() on Linux 4.4.94+:
        EV_KEY is suppressed when unchanged; MT-A EV_ABS and EV_SYN pass."""
        value = self._s32(value)
        if ev_type == EV_KEY:
            is_down = value != 0
            was_down = code in self.key_down
            if is_down == was_down:
                return  # deduplicated by input_handle_event()
            if is_down:
                self.key_down.add(code)
            else:
                self.key_down.discard(code)
        log.append((ev_type, code, value))

    def fire_irq(self, frame_bytes):
        """Runs hyn_irq_handler (.text + 0x0540) to return and yields the input
        events that reach evdev."""
        need = 6 * self.max_touch + 3
        if len(frame_bytes) < need:
            frame_bytes = frame_bytes + b"\xff" * (need - len(frame_bytes))
        self.next_i2c_frame = bytes(frame_bytes[:need])
        self.emitted = []

        regs = [0] * 32
        regs[4] = 99  # irq
        regs[5] = self.DATA_BASE + self.TS_OFF  # dev_id = ts_data
        regs[29] = self.STACK_TOP  # sp
        sentinel = 0x2FFF0000
        regs[31] = sentinel  # ra

        pc = self.TEXT_BASE + 0x0540
        next_pc = None
        steps = 0

        while pc != sentinel:
            steps += 1
            if steps > 10000:
                raise RuntimeError("hyn_irq_handler exceeded step limit at 0x%08x" % pc)

            if pc == self.i2c_read_addr:
                # static int hyn_read_bytes(wbuf, rbuf, rlen)
                rbuf = regs[5]
                rlen = regs[6]
                for k in range(rlen):
                    b = self.next_i2c_frame[k] if k < len(self.next_i2c_frame) else 0
                    self._sb(rbuf + k, b)
                regs[2] = rlen
                pc = regs[31]
                continue

            if pc in self.stubs:
                name = self.stubs[pc]
                if name == "input_event":
                    self._kernel_input_event(self.emitted, regs[5], regs[6], regs[7])
                    regs[2] = 0
                elif name == "memset":
                    dst, val, n = regs[4], regs[5] & 0xFF, regs[6]
                    for k in range(n):
                        self._sb(dst + k, val)
                    regs[2] = dst
                elif name in ("__kmalloc", "kmem_cache_alloc"):
                    regs[2] = self.DATA_BASE + self.TMP_OFF
                else:
                    regs[2] = 0
                pc = regs[31]
                continue

            t_off = pc - self.TEXT_BASE
            if not (0 <= t_off <= len(self.text) - 4):
                raise RuntimeError("PC out of .text: 0x%08x" % pc)
            ins, = struct.unpack_from("<I", self.text, t_off)

            fall = (pc + 4) & 0xFFFFFFFF if next_pc is None else next_pc
            next_pc = None
            branch_target = None

            op = ins >> 26
            rs = (ins >> 21) & 31
            rt = (ins >> 16) & 31
            rd = (ins >> 11) & 31
            sh = (ins >> 6) & 31
            fn = ins & 63
            imm = ins & 0xFFFF
            simm = imm - 0x10000 if (imm & 0x8000) else imm

            if ins == 0:
                pass
            elif op == 0:
                if fn == 0x00:    # sll
                    regs[rd] = (regs[rt] << sh) & 0xFFFFFFFF
                elif fn == 0x02:  # srl
                    regs[rd] = (regs[rt] & 0xFFFFFFFF) >> sh
                elif fn == 0x03:  # sra
                    regs[rd] = (self._s32(regs[rt]) >> sh) & 0xFFFFFFFF
                elif fn == 0x08:  # jr
                    branch_target = regs[rs] & 0xFFFFFFFF
                elif fn == 0x09:  # jalr
                    target = regs[rs] & 0xFFFFFFFF
                    regs[rd] = (pc + 8) & 0xFFFFFFFF
                    branch_target = target
                elif fn == 0x0A:  # movz
                    if regs[rt] == 0:
                        regs[rd] = regs[rs]
                elif fn == 0x0B:  # movn
                    if regs[rt] != 0:
                        regs[rd] = regs[rs]
                elif fn == 0x21:  # addu
                    regs[rd] = (regs[rs] + regs[rt]) & 0xFFFFFFFF
                elif fn == 0x23:  # subu
                    regs[rd] = (regs[rs] - regs[rt]) & 0xFFFFFFFF
                elif fn == 0x24:  # and
                    regs[rd] = regs[rs] & regs[rt]
                elif fn == 0x25:  # or
                    regs[rd] = regs[rs] | regs[rt]
                elif fn == 0x26:  # xor
                    regs[rd] = regs[rs] ^ regs[rt]
                elif fn == 0x27:  # nor
                    regs[rd] = (~(regs[rs] | regs[rt])) & 0xFFFFFFFF
                elif fn == 0x2A:  # slt
                    regs[rd] = 1 if self._s32(regs[rs]) < self._s32(regs[rt]) else 0
                elif fn == 0x2B:  # sltu
                    regs[rd] = 1 if (regs[rs] & 0xFFFFFFFF) < (regs[rt] & 0xFFFFFFFF) else 0
                else:
                    raise RuntimeError("unhandled SPECIAL fn=0x%x at 0x%08x" % (fn, pc))
            elif op == 28 and fn == 0x02:  # mul (MIPS32r2 SPECIAL2)
                regs[rd] = (self._s32(regs[rs]) * self._s32(regs[rt])) & 0xFFFFFFFF
            elif op == 1:  # REGIMM: bltz / bgez
                sv = self._s32(regs[rs])
                take = (sv < 0) if (rt == 0) else (sv >= 0)
                if take:
                    branch_target = (pc + 4 + (simm << 2)) & 0xFFFFFFFF
                else:
                    branch_target = (pc + 8) & 0xFFFFFFFF
            elif op == 4:  # beq
                branch_target = (pc + 4 + (simm << 2)) & 0xFFFFFFFF if regs[rs] == regs[rt] else (pc + 8) & 0xFFFFFFFF
            elif op == 5:  # bne
                branch_target = (pc + 4 + (simm << 2)) & 0xFFFFFFFF if regs[rs] != regs[rt] else (pc + 8) & 0xFFFFFFFF
            elif op == 6:  # blez
                branch_target = (pc + 4 + (simm << 2)) & 0xFFFFFFFF if self._s32(regs[rs]) <= 0 else (pc + 8) & 0xFFFFFFFF
            elif op == 7:  # bgtz
                branch_target = (pc + 4 + (simm << 2)) & 0xFFFFFFFF if self._s32(regs[rs]) > 0 else (pc + 8) & 0xFFFFFFFF
            elif op == 9:   # addiu
                regs[rt] = (regs[rs] + simm) & 0xFFFFFFFF
            elif op == 10:  # slti
                regs[rt] = 1 if self._s32(regs[rs]) < simm else 0
            elif op == 11:  # sltiu
                regs[rt] = 1 if (regs[rs] & 0xFFFFFFFF) < (simm & 0xFFFFFFFF) else 0
            elif op == 12:  # andi
                regs[rt] = regs[rs] & imm
            elif op == 13:  # ori
                regs[rt] = regs[rs] | imm
            elif op == 14:  # xori
                regs[rt] = regs[rs] ^ imm
            elif op == 15:  # lui
                regs[rt] = (imm << 16) & 0xFFFFFFFF
            elif op == 35:  # lw
                regs[rt] = self._lw((regs[rs] + simm) & 0xFFFFFFFF)
            elif op == 36:  # lbu
                regs[rt] = self._lbu((regs[rs] + simm) & 0xFFFFFFFF)
            elif op == 40:  # sb
                self._sb((regs[rs] + simm) & 0xFFFFFFFF, regs[rt])
            elif op == 43:  # sw
                self._sw((regs[rs] + simm) & 0xFFFFFFFF, regs[rt])
            else:
                raise RuntimeError("unhandled op=%d at 0x%08x (0x%08x)" % (op, pc, ins))

            regs[0] = 0
            if branch_target is not None:
                next_pc = branch_target
                pc = (pc + 4) & 0xFFFFFFFF  # execute delay slot next
            else:
                pc = fall

        self.assert_irq_handled = (regs[2] == 1)
        return list(self.emitted)


def make_contact(flag, cid, x, y, pressure=0, area=0):
    """Packs one 6-byte CST8xx contact slot (flag: 0=DOWN, 1=UP, 2=CONTACT)."""
    return bytes([
        ((flag & 0x3) << 6) | ((x >> 8) & 0x0F),
        x & 0xFF,
        ((cid & 0x0F) << 4) | ((y >> 8) & 0x0F),
        y & 0xFF,
        pressure & 0xFF,
        (area & 0x0F) << 4,
    ])


def make_frame(point_num, contacts, gesture=0):
    body = b"".join(contacts)
    return bytes([0x00, gesture & 0xFF, point_num & 0x0F]) + body


STOCK_BUNDLE = _extract_stock_module_from_upt()


class Cst8xxWakePatchCliTests(unittest.TestCase):
    def _synthetic_elf(self, payload):
        return b"\x7fELF\x01\x01\x01" + b"\x00" * 57 + payload + b"\x00" * 32

    def test_cli_patch_check_idempotent_and_revert(self):
        with tempfile.TemporaryDirectory() as td:
            mod = os.path.join(td, "cst8xx_touch.ko")
            stock = self._synthetic_elf(wake_mod.ANCHOR)
            with open(mod, "wb") as f:
                f.write(stock)

            self.assertEqual(_run_tool(WAKE_TOOL, "--check", td).returncode, 1)

            r = _run_tool(WAKE_TOOL, td)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertTrue(os.path.isfile(mod + ".orig"))
            self.assertEqual(_run_tool(WAKE_TOOL, "--check", td).returncode, 0)

            # Second run is a no-op and preserves the original backup.
            r2 = _run_tool(WAKE_TOOL, td)
            self.assertEqual(r2.returncode, 0, r2.stderr)
            self.assertIn("already patched", r2.stdout)
            with open(mod + ".orig", "rb") as f:
                self.assertEqual(f.read(), stock)

            # Revert restores the exact stock bytes.
            r3 = _run_tool(WAKE_TOOL, "--revert", td)
            self.assertEqual(r3.returncode, 0, r3.stderr)
            with open(mod, "rb") as f:
                self.assertEqual(f.read(), stock)
            self.assertEqual(_run_tool(WAKE_TOOL, "--check", td).returncode, 1)

    def test_no_backup_leaves_no_orig_behind(self):
        with tempfile.TemporaryDirectory() as td:
            mod = os.path.join(td, "cst8xx_touch.ko")
            with open(mod, "wb") as f:
                f.write(self._synthetic_elf(wake_mod.ANCHOR))
            r = _run_tool(WAKE_TOOL, "--no-backup", td)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertFalse(os.path.exists(mod + ".orig"))
            self.assertEqual(_run_tool(WAKE_TOOL, "--check", td).returncode, 0)

    def test_unrecognised_and_duplicate_inputs_return_exit_2(self):
        with tempfile.TemporaryDirectory() as td:
            mod = os.path.join(td, "cst8xx_touch.ko")
            with open(mod, "wb") as f:
                f.write(self._synthetic_elf(b"\x12\x34\x56\x78" * 30))
            self.assertEqual(_run_tool(WAKE_TOOL, "--check", td).returncode, 2)
            self.assertEqual(_run_tool(WAKE_TOOL, td).returncode, 2)

            with open(mod, "wb") as f:
                f.write(self._synthetic_elf(wake_mod.ANCHOR + wake_mod.ANCHOR))
            self.assertEqual(_run_tool(WAKE_TOOL, "--check", td).returncode, 2)
            self.assertEqual(_run_tool(WAKE_TOOL, td).returncode, 2)

            with open(mod, "wb") as f:
                f.write(b"not an elf")
            self.assertEqual(_run_tool(WAKE_TOOL, "--check", td).returncode, 2)


@unittest.skipIf(STOCK_BUNDLE is None, "stock V1 cst8xx_touch.ko unavailable (install 7z + squashfs-tools)")
class Cst8xxStockModuleTests(unittest.TestCase):
    def _patched_bytes(self, also_multitouch=False, mt_first=False):
        ko, sh = STOCK_BUNDLE
        with tempfile.TemporaryDirectory() as td:
            mod = os.path.join(td, "cst8xx_touch.ko")
            scr = os.path.join(td, "cst8xx_touch.sh")
            with open(mod, "wb") as f:
                f.write(ko)
            with open(scr, "w", encoding="utf-8") as f:
                f.write(sh)

            if also_multitouch and mt_first:
                self.assertEqual(_run_tool(MT_TOOL, td).returncode, 0)
            self.assertEqual(_run_tool(WAKE_TOOL, "--no-backup", td).returncode, 0)
            if also_multitouch and not mt_first:
                self.assertEqual(_run_tool(MT_TOOL, td).returncode, 0)

            with open(mod, "rb") as f:
                return f.read()

    def test_elf_structure_and_exact_revert(self):
        ko, sh = STOCK_BUNDLE
        patched = self._patched_bytes()
        self.assertEqual(len(patched), len(ko))
        self.assertNotEqual(patched, ko)

        secs_orig, syms_orig = _parse_elf32(ko)
        secs_new, syms_new = _parse_elf32(patched)
        self.assertEqual(secs_orig, secs_new)
        self.assertEqual(syms_orig, syms_new)

        # Every byte outside .text + 0x06d8..0x073b is untouched.
        text_off = secs_orig[".text"][4]
        lo = text_off + 0x06D8
        hi = lo + len(wake_mod.ANCHOR)
        self.assertEqual(patched[:lo], ko[:lo])
        self.assertEqual(patched[hi:], ko[hi:])

        # Coexists with cst8xx_multitouch_patch.py in either order and reverts cleanly.
        both_a = self._patched_bytes(also_multitouch=True, mt_first=False)
        both_b = self._patched_bytes(also_multitouch=True, mt_first=True)
        self.assertEqual(both_a, both_b)

        with tempfile.TemporaryDirectory() as td:
            mod = os.path.join(td, "cst8xx_touch.ko")
            scr = os.path.join(td, "cst8xx_touch.sh")
            with open(mod, "wb") as f:
                f.write(both_a)
            with open(scr, "w", encoding="utf-8") as f:
                f.write(sh)
            self.assertEqual(_run_tool(WAKE_TOOL, "--check", td).returncode, 0)
            self.assertEqual(_run_tool(MT_TOOL, "--check", td).returncode, 0)
            self.assertEqual(_run_tool(WAKE_TOOL, "--revert", td).returncode, 0)
            self.assertEqual(_run_tool(MT_TOOL, "--revert", td).returncode, 0)
            with open(mod, "rb") as f:
                self.assertEqual(
                    hashlib.sha256(f.read()).hexdigest(),
                    hashlib.sha256(ko).hexdigest(),
                )

    def test_stock_module_reproduces_spurious_wake_contact(self):
        ko, _ = STOCK_BUNDLE
        h = HynIrqHarness(ko)
        h.simulate_resume_prefix()

        # Replayed PA16 falling edge reads nine zero bytes.
        ev = h.fire_irq(b"\x00" * 9)
        self.assertTrue(h.assert_irq_handled)
        self.assertEqual(ev, [
            (EV_ABS, ABS_MT_TRACKING_ID, 0),
            (EV_ABS, ABS_MT_PRESSURE, 63),
            (EV_ABS, ABS_MT_TOUCH_MAJOR, 9),
            (EV_ABS, ABS_MT_POSITION_X, 0),
            (EV_ABS, ABS_MT_POSITION_Y, 0),
            (EV_SYN, SYN_MT_REPORT, 0),
            (EV_SYN, SYN_MT_REPORT, 0),
            (EV_SYN, SYN_REPORT, 0),
        ])
        # Toggle stuck at 1 and BTN_TOUCH still 0 in the kernel.
        self.assertEqual(h.event_cnt(0), 1)
        self.assertEqual(h.touchs(), 1)
        self.assertNotIn(BTN_TOUCH, h.key_down)

        # Scan 0 of the user's real touch has its coordinates dropped by cnt == 1.
        scan0 = h.fire_irq(make_frame(1, [make_contact(0, 0, 120, 160)]))
        self.assertEqual(scan0, [
            (EV_KEY, BTN_TOUCH, 1),
            (EV_SYN, SYN_REPORT, 0),
        ])

    def test_patched_module_suppresses_wake_glitch_and_reports_first_tap(self):
        h = HynIrqHarness(self._patched_bytes())
        h.simulate_resume_prefix()

        # Replayed PA16 IRQ after wake: zero ABS_MT_* and zero EV_KEY events.
        wake_ev = h.fire_irq(b"\x00" * 9)
        self.assertTrue(h.assert_irq_handled)
        self.assertEqual(wake_ev, [
            (EV_SYN, SYN_MT_REPORT, 0),
            (EV_SYN, SYN_REPORT, 0),
        ])
        self.assertEqual(h.event_cnt(0), 0)
        self.assertEqual(h.touchs(), 0)
        self.assertNotIn(BTN_TOUCH, h.key_down)

        # First real tap after wake lands on scan 0 with full coordinates.
        down = h.fire_irq(make_frame(1, [make_contact(0, 0, 120, 160)]))
        self.assertEqual(down, [
            (EV_ABS, ABS_MT_TRACKING_ID, 0),
            (EV_ABS, ABS_MT_PRESSURE, 63),
            (EV_ABS, ABS_MT_TOUCH_MAJOR, 9),
            (EV_ABS, ABS_MT_POSITION_X, 120),
            (EV_ABS, ABS_MT_POSITION_Y, 160),
            (EV_SYN, SYN_MT_REPORT, 0),
            (EV_KEY, BTN_TOUCH, 1),
            (EV_SYN, SYN_REPORT, 0),
        ])

        up = h.fire_irq(make_frame(0, [make_contact(1, 0, 120, 160)]))
        self.assertEqual(up, [
            (EV_KEY, BTN_TOUCH, 0),
            (EV_SYN, SYN_MT_REPORT, 0),
            (EV_SYN, SYN_REPORT, 0),
        ])
        self.assertEqual(h.event_cnt(0), 0)
        self.assertEqual(h.touchs(), 0)

    def test_patched_corners_slide_midtouch_blank_missed_up_and_gesture(self):
        h = HynIrqHarness(self._patched_bytes())

        # Genuine taps at (0, 0), (239, 319) and (479, 799) all report cleanly.
        for x, y in ((0, 0), (239, 319), (479, 799)):
            h.simulate_resume_prefix()
            h.fire_irq(b"\x00" * 9)
            down = h.fire_irq(make_frame(1, [make_contact(0, 0, x, y, pressure=40, area=5)]))
            self.assertIn((EV_ABS, ABS_MT_POSITION_X, x), down)
            self.assertIn((EV_ABS, ABS_MT_POSITION_Y, y), down)
            self.assertIn((EV_ABS, ABS_MT_PRESSURE, 40), down)
            self.assertIn((EV_ABS, ABS_MT_TOUCH_MAJOR, 5), down)
            self.assertIn((EV_KEY, BTN_TOUCH, 1), down)
            up = h.fire_irq(make_frame(0, [make_contact(1, 0, x, y)]))
            self.assertIn((EV_KEY, BTN_TOUCH, 0), up)

        # Slide after wake: scan 0 DOWN, scans 1..3 CONTACT, scan 4 UP.
        h.simulate_resume_prefix()
        h.fire_irq(b"\x00" * 9)
        s0 = h.fire_irq(make_frame(1, [make_contact(0, 0, 100, 200)]))
        s1 = h.fire_irq(make_frame(1, [make_contact(2, 0, 100, 180)]))
        s2 = h.fire_irq(make_frame(1, [make_contact(2, 0, 100, 150)]))
        s3 = h.fire_irq(make_frame(1, [make_contact(2, 0, 100, 120)]))
        s4 = h.fire_irq(make_frame(0, [make_contact(1, 0, 100, 100)]))
        self.assertIn((EV_ABS, ABS_MT_POSITION_Y, 200), s0)
        self.assertIn((EV_KEY, BTN_TOUCH, 1), s0)
        self.assertEqual(s1, [(EV_SYN, SYN_REPORT, 0)])  # driver's odd-scan toggle
        self.assertIn((EV_ABS, ABS_MT_POSITION_Y, 150), s2)
        self.assertEqual(s3, [(EV_SYN, SYN_REPORT, 0)])
        self.assertIn((EV_KEY, BTN_TOUCH, 0), s4)

        # Screen blanked mid-touch (cnt left at 1): wake clears cnt back to 0.
        h.fire_irq(make_frame(1, [make_contact(0, 0, 50, 60)]))
        self.assertEqual(h.event_cnt(0), 1)
        rel = h.simulate_resume_prefix()
        self.assertIn((EV_KEY, BTN_TOUCH, 0), rel)
        wake = h.fire_irq(b"\x00" * 9)
        self.assertFalse(any(t == EV_ABS for t, _, _ in wake))
        self.assertEqual(h.event_cnt(0), 0)
        nxt = h.fire_irq(make_frame(1, [make_contact(0, 0, 80, 90)]))
        self.assertIn((EV_ABS, ABS_MT_POSITION_X, 80), nxt)
        self.assertIn((EV_KEY, BTN_TOUCH, 1), nxt)

        # Missed TOUCH_UP (an all-zero idle frame while BTN_TOUCH is 1) still releases.
        missed = h.fire_irq(b"\x00" * 9)
        self.assertEqual(missed, [
            (EV_KEY, BTN_TOUCH, 0),
            (EV_SYN, SYN_MT_REPORT, 0),
            (EV_SYN, SYN_REPORT, 0),
        ])
        self.assertNotIn(BTN_TOUCH, h.key_down)

        # Gesture-only frame (point_num == 0, gesture id 0x05, slot 0 zeroed).
        gest = h.fire_irq(make_frame(0, [b"\x00" * 6], gesture=0x05))
        self.assertFalse(any(t == EV_ABS for t, _, _ in gest))
        self.assertFalse(any(t == EV_KEY for t, _, _ in gest))

        # Over-limit point_num is still rejected outright.
        over = h.fire_irq(make_frame(2, [make_contact(0, 0, 10, 20)]))
        self.assertEqual(over, [])

    def test_patched_with_multitouch_handles_two_contacts_and_partial_lift(self):
        h = HynIrqHarness(self._patched_bytes(also_multitouch=True))
        self.assertEqual(h.max_touch, 2)

        h.simulate_resume_prefix()
        # CST836U idle buffer: buf[0..8] = 0x00, buf[9..14] = 0xFF.
        wake = h.fire_irq(b"\x00" * 9 + b"\xff" * 6)
        self.assertFalse(any(t == EV_ABS for t, _, _ in wake))
        self.assertEqual(h.event_cnt(0), 0)
        self.assertEqual(h.event_cnt(1), 0)

        # Two fingers down together.
        both = h.fire_irq(make_frame(2, [
            make_contact(0, 0, 40, 260),
            make_contact(0, 1, 200, 260),
        ]))
        xs = [v for t, c, v in both if t == EV_ABS and c == ABS_MT_POSITION_X]
        ids = [v for t, c, v in both if t == EV_ABS and c == ABS_MT_TRACKING_ID]
        self.assertEqual(ids, [0, 1])
        self.assertEqual(xs, [40, 200])
        self.assertIn((EV_KEY, BTN_TOUCH, 1), both)

        # Next scan clears toggle; then finger 0 lifts (flag=1) while finger 1 holds (flag=2).
        h.fire_irq(make_frame(2, [
            make_contact(2, 0, 40, 260),
            make_contact(2, 1, 200, 260),
        ]))
        partial = h.fire_irq(make_frame(1, [
            make_contact(1, 0, 40, 260),
            make_contact(2, 1, 202, 261),
        ]))
        ids = [v for t, c, v in partial if t == EV_ABS and c == ABS_MT_TRACKING_ID]
        xs = [v for t, c, v in partial if t == EV_ABS and c == ABS_MT_POSITION_X]
        self.assertEqual(ids, [1])
        self.assertEqual(xs, [202])
        self.assertIn(BTN_TOUCH, h.key_down)
        self.assertEqual(h.event_cnt(0), 0)

        # Finger 1 lifts: BTN_TOUCH goes back to 0.
        last_up = h.fire_irq(make_frame(0, [make_contact(1, 1, 202, 261)]))
        self.assertIn((EV_KEY, BTN_TOUCH, 0), last_up)
        self.assertEqual(h.touchs(), 0)


if __name__ == "__main__":
    unittest.main()
