#!/usr/bin/env python3
"""
Stops cst8xx_touch.ko reporting a phantom contact after every screen wake.

On the TempoTec V1 (and HiBy R1), blanking fb0 calls disable_irq_nosync() on
the touch interrupt (PA16, falling edge) and then powers down the CST8xx rail
and drives reset low. The X1600 pinctrl irq_chip uses lazy IRQ disable, so the
falling edge on PA16 as the controller loses power latches IRQS_PENDING. When
hyn_resume_work resets the controller and calls enable_irq(), check_irq_resend
immediately runs hyn_irq_handler against the freshly cleared register block
(nine zero bytes: point_num = 0, buf[3..8] = 0).

hyn_irq_handler was adapted from FocalTech's driver, where an unused contact
slot reads back as 0xFF (flag = 3, id = 15) and stops the parse loop. The
CST8xx firmware only 0xFF-fills slots 1 and up; slot 0 stays zero, which in
that encoding is id = 0, flag = 0 (FTS_TOUCH_DOWN) at (0, 0). Because the loop
never checks point_num == 0, every wake emits ABS_MT_TRACKING_ID 0 and position
(0, 0) while the input core suppresses BTN_TOUCH 0 as unchanged -- latching a
phantom press in userspace that swallows the entire first tap or slide until
the user lifts a real finger, and leaving the driver's per-contact scan toggle
out of step as well.

When point_num is zero there are no contacts down: a genuine release frame has
flag = 1 (FTS_TOUCH_UP), and an all-zero slot has no contact at all. Setting
bit 0 of event->flag whenever point_num == 0 sends 0 -> 1 (UP) and 2 -> 3
(NO_EVENT) while leaving 1 and 3 untouched, so a spurious zero frame clears the
per-contact toggle, clears touchs, emits no ABS_MT_* events, and still releases
BTN_TOUCH if one was held. Any frame with point_num >= 1 is unchanged.

    python3 tools/cst8xx_wake_patch.py [directory]
    python3 tools/cst8xx_wake_patch.py --check     [directory]
    python3 tools/cst8xx_wake_patch.py --revert    [directory]
    python3 tools/cst8xx_wake_patch.py --no-backup [directory]

Coexists with cst8xx_multitouch_patch.py in either order: the two patches touch
different sections (.text vs .init.text). See PATCHES.md for the full account.
"""

import argparse
import os
import shutil
import struct
import sys

MODULE = "cst8xx_touch.ko"
BACKUP_SUFFIX = ".orig"

# The contact parse loop in hyn_irq_handler (.text + 0x06d8..0x0738).
#
# GCC reloads buf[3] (-3(a0)) and buf[5] (-1(a0)) a second time to shift out
# `flag` and `id`, and leaves the loop-exit branch delay slot empty. Holding
# buf[3] in a3 and buf[5] in t0 (both dead caller-saved temporaries across the
# loop) plus loading ts_data->point_num (0x124(s0)) in the delay slot frees the
# exact two words needed for `if (point_num == 0) flag |= 1`, with no branch
# target, relocation or section size changed.
ANCHOR = struct.pack(
    "<25I",
    0x00000000,  # nop                  (delay slot of beq v1, zero, done)
    0x90830005,  # lbu   v1, 5(a0)
    0x2C630020,  # sltiu v1, v1, 32
    0x1060001E,  # beq   v1, zero, done
    0x24840006,  # addiu a0, a0, 6
    0x9083FFFD,  # lbu   v1, -3(a0)     buf[3]
    0x9086FFFE,  # lbu   a2, -2(a0)     buf[4]
    0x24A50001,  # addiu a1, a1, 1      i++
    0x3063000F,  # andi  v1, v1, 0xf
    0x00031A00,  # sll   v1, v1, 8
    0x00661821,  # addu  v1, v1, a2
    0xAC430000,  # sw    v1, 0(v0)      event->x
    0x9083FFFF,  # lbu   v1, -1(a0)     buf[5]
    0x90860000,  # lbu   a2, 0(a0)      buf[6]
    0x2442001C,  # addiu v0, v0, 28
    0x3063000F,  # andi  v1, v1, 0xf
    0x00031A00,  # sll   v1, v1, 8
    0x00661821,  # addu  v1, v1, a2
    0xAC43FFE8,  # sw    v1, -24(v0)    event->y
    0x9083FFFD,  # lbu   v1, -3(a0)     buf[3] (again)
    0x00031982,  # srl   v1, v1, 6      flag = buf[3] >> 6
    0xAC43FFF0,  # sw    v1, -16(v0)    event->flag
    0x9083FFFF,  # lbu   v1, -1(a0)     buf[5] (again)
    0x00031902,  # srl   v1, v1, 4      id = buf[5] >> 4
    0xAC43FFF4,  # sw    v1, -12(v0)    event->id
)

ANCHOR_PATCHED = struct.pack(
    "<25I",
    0x8E090124,  # lw    t1, 0x124(s0)  t1 = ts_data->point_num
    0x90830005,  # lbu   v1, 5(a0)
    0x2C630020,  # sltiu v1, v1, 32
    0x1060001E,  # beq   v1, zero, done
    0x24840006,  # addiu a0, a0, 6
    0x9087FFFD,  # lbu   a3, -3(a0)     a3 = buf[3]
    0x9086FFFE,  # lbu   a2, -2(a0)     buf[4]
    0x24A50001,  # addiu a1, a1, 1      i++
    0x30E3000F,  # andi  v1, a3, 0xf
    0x00031A00,  # sll   v1, v1, 8
    0x00661821,  # addu  v1, v1, a2
    0xAC430000,  # sw    v1, 0(v0)      event->x
    0x9088FFFF,  # lbu   t0, -1(a0)     t0 = buf[5]
    0x90860000,  # lbu   a2, 0(a0)      buf[6]
    0x2442001C,  # addiu v0, v0, 28
    0x3103000F,  # andi  v1, t0, 0xf
    0x00031A00,  # sll   v1, v1, 8
    0x00661821,  # addu  v1, v1, a2
    0xAC43FFE8,  # sw    v1, -24(v0)    event->y
    0x00071982,  # srl   v1, a3, 6      flag = buf[3] >> 6
    0x2D260001,  # sltiu a2, t1, 1      a2 = (point_num == 0)
    0x00661825,  # or    v1, v1, a2     if (point_num == 0) flag |= 1
    0xAC43FFF0,  # sw    v1, -16(v0)    event->flag
    0x00081902,  # srl   v1, t0, 4      id = buf[5] >> 4
    0xAC43FFF4,  # sw    v1, -12(v0)    event->id
)


def fail(message, code=1):
    sys.stderr.write("cst8xx_wake_patch: " + message + "\n")
    sys.exit(code)


def find_once(data, pattern, what):
    """The single offset of `pattern`, or None. More than one is an error."""
    hits = []
    start = 0
    while True:
        i = data.find(pattern, start)
        if i < 0:
            break
        hits.append(i)
        start = i + 1
    if len(hits) > 1:
        fail("%s found %d times, expected once -- this is not the module this "
             "patch was written for" % (what, len(hits)), code=2)
    return hits[0] if hits else None


def module_state(path):
    """'stock', 'patched', or a reason it is neither."""
    with open(path, "rb") as f:
        data = f.read()

    if data[:4] != b"\x7fELF":
        return data, "not an ELF file"

    has_stock = find_once(data, ANCHOR, "the parse loop") is not None
    has_patched = find_once(data, ANCHOR_PATCHED, "the patched parse loop") is not None
    if has_stock and has_patched:
        return data, "both stock and patched sequences are present"
    if has_stock:
        return data, "stock"
    if has_patched:
        return data, "patched"
    return data, "neither the stock sequence nor a patched one is present"


def backup(path):
    """Keeps the first version seen. A second run must not overwrite it."""
    dest = path + BACKUP_SUFFIX
    if not os.path.exists(dest):
        shutil.copy2(path, dest)
        print("  backup: %s" % os.path.basename(dest))


def patch_module(path, revert, keep_backup):
    data, state = module_state(path)

    want = "patched" if revert else "stock"
    done = "stock" if revert else "patched"
    if state == done:
        print("  %s: already %s" % (MODULE, done))
        return False
    if state != want:
        fail("%s: %s" % (MODULE, state), code=2)

    src = ANCHOR_PATCHED if revert else ANCHOR
    dst = ANCHOR if revert else ANCHOR_PATCHED
    at = find_once(data, src, "the sequence")

    if keep_backup:
        backup(path)
    mode = os.stat(path).st_mode
    patched = data[:at] + dst + data[at + len(dst):]
    with open(path, "wb") as f:
        f.write(patched)
    os.chmod(path, mode)

    action = "reverted point_num==0 guard" if revert else "guarded point_num==0 contact parse"
    print("  %s: %s at file offset 0x%x" % (MODULE, action, at))
    return True


def check(module_path):
    _, state = module_state(module_path)
    print("  %s: %s" % (MODULE, state))

    if state == "patched":
        print("wake-touch fix: in place")
        return 0
    if state == "stock":
        print("wake-touch fix: NOT in place -- run this script without --check")
        return 1
    return 2


def main():
    ap = argparse.ArgumentParser(
        description="Stop cst8xx_touch.ko emitting a phantom touch on screen wake."
    )
    ap.add_argument("directory", nargs="?",
                    default=os.path.dirname(os.path.abspath(__file__)),
                    help="directory holding %s (default: beside this script)" % MODULE)
    group = ap.add_mutually_exclusive_group()
    group.add_argument("--check", action="store_true",
                       help="report the state and change nothing (0=patched, 1=stock, 2=unrecognised)")
    group.add_argument("--revert", action="store_true",
                       help="put the stock behaviour back")
    ap.add_argument("--no-backup", action="store_true",
                    help="patch in place without leaving a %s copy behind" % BACKUP_SUFFIX)
    args = ap.parse_args()

    module_path = (
        args.directory if os.path.isfile(args.directory)
        else os.path.join(args.directory, MODULE)
    )
    if not os.path.isfile(module_path):
        fail("cannot find %s" % module_path)

    if args.check:
        sys.exit(check(module_path))

    changed = patch_module(module_path, args.revert, keep_backup=not args.no_backup)
    if not changed:
        print("nothing to do")


if __name__ == "__main__":
    main()
