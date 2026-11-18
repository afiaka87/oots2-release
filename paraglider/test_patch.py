"""Run the compiled Paraglider code in a CPU emulator, with every game routine it calls modelled in Python.

Checks the frame-by-frame behaviour and that the hooks leave the caller's registers and stack alone.
It does not test the game's side of any call.

  uv run --no-project --with unicorn python test_patch.py
"""

from __future__ import annotations

import struct
import sys
from pathlib import Path

from unicorn import UC_ARCH_ARM64, UC_HOOK_CODE, UC_HOOK_INTR, UC_MODE_ARM, Uc
from unicorn.arm64_const import (UC_ARM64_REG_CPACR_EL1, UC_ARM64_REG_LR, UC_ARM64_REG_PC, UC_ARM64_REG_SP,
                                 UC_ARM64_REG_X0, UC_ARM64_REG_X19, UC_ARM64_REG_X29)

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import build_patch as bp  # noqa: E402

RET = struct.pack("<I", 0xD65F03C0)
HEAP, STACK, STOP, VSTUB = 0x10000000, 0x20000000, 0x30000000, 0x31000000
NONE, JUMP, FALL, LAND, LIFT, DRAWN, GLIDE = 0, 13, 33, 32, 43, 57, 82


class World:
    def __init__(self):
        root = bp.main_checkout()
        blob, self.sym = bp.compile_code(root, root / "work/switch2/oot-remake/build/paraglider/obj")
        mu = self.mu = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        mu.reg_write(UC_ARM64_REG_CPACR_EL1, 0x300000)  # floating point on
        self.pages: set[int] = set()
        self.page(bp.CODE, len(blob))
        mu.mem_write(bp.CODE, blob)
        for base, size in ((HEAP, 0x10000), (STACK - 0x10000, 0x10000), (STOP, 0x1000), (VSTUB, 0x1000)):
            mu.mem_map(base, size)
        self.page(bp.STATE, bp.STATE_SIZE)
        self.calls: list[tuple] = []
        self.log: list[str] = []
        self.handlers = {}
        for name, (addr, _) in bp.GAME.items():
            self.page(addr, 8)
            if hasattr(self, name):
                mu.mem_write(addr, RET)
                self.handlers[addr] = getattr(self, name)
        self.q(bp.GAME["game_lift_param_slot"][0], 0x4981930)
        self.q(bp.GAME["game_wake_vtable_slot"][0], HEAP + 0x8000)
        self.q(bp.GAME["game_wake_vec_slot"][0], HEAP + 0x8100)
        mu.mem_write(HEAP + 0x8100, struct.pack("<3f", 1.0, 1.0, 1.0))
        self.q(bp.GAME["game_gmd_slot"][0], HEAP + 0x8200)
        self.q(HEAP + 0x8200, HEAP + 0x8300)  # the manager
        h = HEAP
        self.node, a, b = h + 0x000, h + 0x100, h + 0x200
        self.actor, self.comps, self.player = h + 0x400, h + 0x800, h + 0x1000
        self.lifter, self.holder, bsa = h + 0x2000, h + 0x2400, h + 0x2800
        self.main_rec, self.arms_rec = h + 0x2900, h + 0x2A00
        self.slot, self.held = h + 0x3000, h + 0x3400
        self.q(self.node + 8, a), self.q(a + 8, b), self.q(b + 0x1F0, self.actor)
        self.q(self.node, bp.GAME["game_sub_main_vtable"][0])  # the node is SelectorPlayerSubSchedulerMain
        self.child_node = h + 0x7000  # the active upper-body selector
        self.q(self.child_node, HEAP + 0x3900), self.q(self.child_node + 8, a)
        self.q(self.actor + 0x270, self.comps)
        mu.mem_write(self.actor + 0x31C, struct.pack("<3f", 10.0, 20.0, 30.0))
        self.q(self.comps + 0x358, self.player), self.q(self.comps + 0x2C8, self.lifter)
        self.q(self.comps + 0x110, self.holder), self.q(self.comps + 0x188, bsa)
        self.q(bsa + 0x1C0, self.main_rec), self.q(bsa + 0x1C8, self.arms_rec)
        mu.mem_write(self.node + 0x23, b"\x05")  # Lift child index of the empty-arms selector
        mu.mem_write(self.node + 0x2D, b"\x07")  # Lift child index of the sword-out selector
        vt = h + 0x3800
        self.q(self.slot, vt)
        for off, fn in ((0x18, self.slot_wake), (0x40, self.slot_alive), (0x50, self.slot_create),
                        (0x78, self.slot_release), (0x90, self.slot_actor)):
            self.q(vt + off, VSTUB + off)
            mu.mem_write(VSTUB + off, RET)
            self.handlers[VSTUB + off] = fn
        # the slot (no request, no actor) and the holder: owner Link, one used entry of four
        self.q(self.slot + 0x18, 0), self.q(self.slot + 0x30, 0), self.q(self.slot + 0x38, 0xFFFFFFFF)
        cells = h + 0x5000
        self.q(self.holder + 0x18, self.actor), self.q(self.holder + 0x28, cells)
        self.w32(self.holder + 0x30, 1), self.w32(self.holder + 0x34, 4)
        self.w32(cells, 0x1234), self.w32(cells + 0x10, 1), self.q(cells + 0x18, cells + 0x100)
        self.q(cells + 0x100, self.slot)
        # a second Link (a cutscene double, or the next area's Link), standing, hands empty
        o = h + 0x4000
        self.other_node = o
        self.q(o, bp.GAME["game_sub_main_vtable"][0])
        self.q(o + 8, o + 0x100), self.q(o + 0x108, o + 0x200), self.q(o + 0x3F0, o + 0x400)
        self.q(o + 0x400 + 0x270, o + 0x800)
        for off, target in ((0x358, o + 0x1000), (0x2C8, o + 0x2000), (0x110, self.holder), (0x188, o + 0x2800)):
            self.q(o + 0x800 + off, target)
        self.q(o + 0x2800 + 0x1C0, o + 0x2900), self.q(o + 0x2800 + 0x1C8, o + 0x2A00)
        self.w32(o + 0x2900 + 0x28, 40)
        mu.hook_add(UC_HOOK_CODE, self.on_code)
        mu.hook_add(UC_HOOK_INTR, self.on_svc)
        self.owned = self.chest = self.pressed = False
        self.held_life = 3   # waiting
        self.wake_delay = 1
        self.waking = None
        self.create_delay = 10 ** 6
        self.creating = None
        self.lift_ok = True
        self.select_works = True

    def page(self, addr: int, size: int) -> None:
        for p in range(addr & ~0xFFF, (addr + size + 0xFFF) & ~0xFFF, 0x1000):
            if p not in self.pages:
                self.mu.mem_map(p, 0x1000)
                self.pages.add(p)

    def q(self, addr: int, value: int) -> None:
        self.mu.mem_write(addr, struct.pack("<Q", value))

    def u32(self, addr: int) -> int:
        return struct.unpack("<I", self.mu.mem_read(addr, 4))[0]

    def w32(self, addr: int, value: int) -> None:
        self.mu.mem_write(addr, struct.pack("<I", value))

    def x(self, n: int) -> int:
        return self.mu.reg_read(UC_ARM64_REG_X0 + n)

    def ret(self, value: int) -> None:
        self.mu.reg_write(UC_ARM64_REG_X0, value)

    def on_code(self, mu, addr, size, user) -> None:
        fn = self.handlers.get(addr)
        if fn:
            fn()

    def on_svc(self, mu, intno, user) -> None:
        self.log.append(bytes(mu.mem_read(self.x(0), self.x(1))).decode())

    def game_sub_pass_body(self):
        self.calls.append(("pass", self.x(0), self.x(1)))
        self.ret(0x1234)

    def game_key_pressed(self):
        self.ret(int(self.pressed and self.x(1) == 0))

    def game_gmd_get_bool(self):
        want = {bp.murmur3_32(bp.OWN_FLAG.encode()): self.owned, bp.murmur3_32(bp.CHEST_FLAG.encode()): self.chest}
        assert self.x(0) == HEAP + 0x8300 and self.x(2) in want
        self.mu.mem_write(self.x(1), bytes([want[self.x(2)]]))
        self.ret(1)

    def game_holder_slot_by_name(self):
        name = struct.unpack("<Q", self.mu.mem_read(self.x(1), 8))[0]
        text = bytes(self.mu.mem_read(name, 15))
        assert self.x(0) == self.holder and text == b"Obj_Paraglider\0" and self.x(2) == 0, text
        self.calls.append(("slot",))
        self.ret(self.slot)

    def slot_alive(self):
        self.ret(int(self.held_life not in (0, 7)))

    def slot_actor(self):
        raise AssertionError("slot +0x90 called")  # returns nothing while the actor waits (boot 5)

    def sync_slot(self) -> None:
        entry = HEAP + 0x5800
        self.q(self.slot + 0x30, entry)
        self.q(self.slot + 0x38, 0xFFFFFFFF if self.held_life in (0, 7) else 5)
        self.w32(entry + 0x48, 5), self.q(entry + 0x40, self.held)
        self.w32(self.held + 0x2C, self.held_life)

    def slot_wake(self):
        arg = bytes(self.mu.mem_read(self.x(1), 0x58))
        assert self.x(0) == self.slot and self.x(2) == self.actor
        assert self.held_life in (3, 6)
        self.calls.append(("wake", arg))
        self.waking = self.wake_delay
        self.ret(1)

    def slot_create(self):
        assert self.x(0) == self.slot and self.x(1) == self.actor and self.x(2) == 0
        ok = self.held_life in (0, 7) and self.creating is None
        if ok:
            self.creating = self.create_delay
        self.calls.append(("create", ok))
        self.ret(int(ok))

    def slot_release(self):
        self.calls.append(("release",))
        self.held_life = 0

    def game_link_set(self):
        assert self.x(1) == self.held and self.x(2) == 0
        vt, zero, ident = struct.unpack("<3Q", self.mu.mem_read(self.x(0), 24))
        assert (vt, zero, ident) == (bp.GAME["game_link_vtable"][0], 0, 0x1FFFFFFFF)
        self.calls.append(("link",))

    def game_link_destroy(self):
        assert struct.unpack("<Q", self.mu.mem_read(self.x(0), 8))[0] == bp.GAME["game_link_dead_vtable"][0]
        self.calls.append(("unlink",))

    def game_lift_start(self):
        assert self.x(0) == self.lifter and self.x(2) == 0x4981930
        ok = self.lift_ok and self.u32(self.lifter + 0x2B0) == 0 and self.held_life in (4, 5)
        if ok:
            self.w32(self.lifter + 0x2B0, 1)
        self.calls.append(("start", ok))
        self.ret(int(ok))

    def game_lift_bind(self):
        assert (self.x(1), self.x(2)) == (1, 1) and self.u32(self.lifter + 0x2B0) == 1
        self.w32(self.lifter + 0x2B0, 3)
        self.calls.append(("bind",))
        self.ret(1)

    def game_link_valid(self):
        assert self.x(0) == self.lifter + 0x1A8
        self.ret(int(self.u32(self.lifter + 0x2B0) in (2, 3) and self.held_life in (4, 5)))

    def game_drop(self):
        assert self.x(0) == self.actor and self.x(1) == 0
        self.w32(self.lifter + 0x2B0, 0)
        self.calls.append(("drop",))

    def game_lift_cancel(self):
        self.calls.append(("cancel",))

    def game_select_child(self):
        self.calls.append(("select", self.x(1)))
        if self.select_works:
            self.w32(self.arms_rec + 0x28, LIFT)
        self.ret(1)

    def game_sub_none_body(self):
        self.calls.append(("none-check",))
        self.ret(0)

    def game_sub_drawn_body(self):
        self.calls.append(("drawn-check",))
        self.ret(0)

    def call(self, name: str, *args: int) -> int:
        mu = self.mu
        saved = {r: 0x1111000000000000 + r for r in range(UC_ARM64_REG_X19, UC_ARM64_REG_X29 + 1)}
        for r, v in saved.items():
            mu.reg_write(r, v)
        for i, v in enumerate(args):
            mu.reg_write(UC_ARM64_REG_X0 + i, v)
        mu.reg_write(UC_ARM64_REG_SP, STACK - 0x100)
        mu.reg_write(UC_ARM64_REG_LR, STOP)
        mu.emu_start(self.sym[name], STOP, count=200000)
        assert mu.reg_read(UC_ARM64_REG_PC) == STOP, "did not return"
        assert mu.reg_read(UC_ARM64_REG_SP) == STACK - 0x100, "stack not restored"
        for r, v in saved.items():
            assert mu.reg_read(r) == v, "callee-saved register changed"
        return self.x(0)

    def frame(self, main: int, pressed: bool = False) -> list[tuple]:
        """The upper-body pass for Main, then for the active selector, then that selector's check."""
        self.calls = []
        self.pressed = pressed
        if self.waking is not None:
            self.waking -= 1
            if self.waking < 0:
                self.held_life, self.waking = 4, None
        if self.creating is not None:
            self.creating -= 1
            if self.creating < 0:
                self.held_life, self.creating = 3, None
        self.sync_slot()
        self.w32(self.main_rec + 0x28, main)
        assert self.call("hook_sub_pass", self.node, 0x77) == 0x1234
        assert self.calls[-1] == ("pass", self.node, 0x77)
        mine = self.calls[:-1]
        self.calls = []
        assert self.call("hook_sub_pass", self.child_node, 0x55) == 0x1234
        assert self.calls == [("pass", self.child_node, 0x55)], "the frame step ran for a selector other than Main"
        self.calls = mine
        arms = self.u32(self.arms_rec + 0x28)
        if arms == NONE:
            self.call("hook_sub_none", self.node, 1, 2, 3)
        elif arms == DRAWN:
            self.call("hook_sub_drawn", self.node, 1, 2, 3)
        return [c for c in self.calls if c[0] not in ("none-check", "drawn-check")]

    def other_frame(self, main: int = 40, pressed: bool = False) -> list[tuple]:
        self.calls = []
        self.pressed = pressed
        self.w32(self.other_node + 0x2900 + 0x28, main)
        self.call("hook_sub_pass", self.other_node, 0x77)
        assert self.calls[-1] == ("pass", self.other_node, 0x77)
        return self.calls[:-1]

    def names(self, calls: list[tuple]) -> list[str]:
        return [c[0] for c in calls]

    def phase(self) -> int:
        return self.u32(bp.STATE + 0x10)

    def reset(self, arms: int = NONE) -> None:
        self.mu.mem_write(bp.STATE, bytes(bp.STATE_SIZE))
        self.w32(self.lifter + 0x2B0, 0)
        self.w32(self.arms_rec + 0x28, arms)
        self.w32(self.player + 0xE40, 0)
        self.w32(self.player + 0xE38, 0)
        self.held_life, self.waking, self.lift_ok, self.select_works = 3, None, True, True
        self.creating, self.create_delay = None, 10 ** 6
        self.owned = self.chest = False


def open_glider(w: World) -> None:
    for _ in range(5):
        assert w.frame(JUMP) == []
    assert w.names(w.frame(JUMP, pressed=True)) == ["slot"] and w.phase() == 1
    calls = w.frame(JUMP)
    assert w.names(calls) == ["wake"], calls
    arg = calls[0][1]
    assert struct.unpack_from("<Q", arg, 0)[0] == HEAP + 0x8000 + 0x10
    assert struct.unpack_from("<3f", arg, 8) == (10.0, 21.0, 30.0)
    assert struct.unpack_from("<9f", arg, 0x14) == (1, 0, 0, 0, 1, 0, 0, 0, 1)
    assert struct.unpack_from("<3f", arg, 0x38) == (1.0, 1.0, 1.0) and arg[0x48:0x50] == bytes(8) and arg[0x50] == 3
    assert w.frame(JUMP) == []  # still waking
    calls = w.frame(JUMP)
    assert calls[:4] == [("link",), ("start", True), ("bind",), ("unlink",)], calls
    assert w.phase() == 2


def main() -> None:
    w = World()

    # 1. Not owned: nothing happens. Owned but pressed too early: nothing happens.
    w.reset()
    for _ in range(6):
        w.frame(JUMP)
    assert w.frame(JUMP, pressed=True) == [] and w.phase() == 0
    w.reset()
    w.owned = True
    w.frame(JUMP), w.frame(JUMP)
    assert w.frame(JUMP, pressed=True) == [] and w.phase() == 0
    assert w.frame(LAND, pressed=True) == []  # on the ground

    # 2. Owned, empty arms: open, arms switch, float, land, put away.
    w.reset()
    w.owned = True
    open_glider(w)
    assert ("select", 5) in w.calls and w.u32(w.player + 0xE40) & 2
    assert w.u32(w.arms_rec + 0x28) == LIFT
    for _ in range(20):
        assert w.frame(GLIDE) == [] and w.phase() == 2
    assert w.frame(LAND) == []
    assert w.names(w.frame(LAND)) == ["drop", "cancel", "release"] and w.phase() == 0

    # 3. Chest flag only, sword out: the sword-out selector switches the arms.
    w.reset(arms=DRAWN)
    w.chest = True
    open_glider(w)
    assert ("select", 7) in w.calls and w.u32(w.player + 0xE38) & 8 and w.u32(w.player + 0xE40) & 2

    # 4. A second press while floating puts it away, but not within the first frames.
    for _ in range(12):
        w.frame(GLIDE)
    assert w.names(w.frame(GLIDE, pressed=True)) == ["drop", "cancel", "release"] and w.phase() == 0

    # 5. The actor never wakes: give up after the timeout without dropping anything.
    w.reset()
    w.owned = True
    w.wake_delay = 10 ** 6
    for _ in range(5):
        w.frame(FALL)
    w.frame(FALL, pressed=True)
    seen = []
    for _ in range(60):
        seen += w.names(w.frame(FALL))
    assert seen == ["wake", "cancel", "release"] and w.phase() == 0, seen
    w.wake_delay = 1

    # 5b. The holder has no paraglider and none arrives: ask for one once, give up and touch nothing.
    w.reset()
    w.owned = True
    w.held_life = 0
    for _ in range(5):
        w.frame(FALL)
    w.frame(FALL, pressed=True)
    seen = []
    for _ in range(60):
        seen += w.names(w.frame(FALL))
    assert seen == ["create", "cancel"] and w.phase() == 0, seen
    # the next attempt asks again and is refused while the first request is in flight
    for _ in range(5):
        w.frame(FALL)
    w.frame(FALL, pressed=True)
    seen = []
    for _ in range(60):
        seen += [c for c in w.frame(FALL) if c[0] == "create"]
    assert seen == [("create", False)], seen

    # 5c. The holder has no paraglider; the one it is asked for arrives in mid-air and opens.
    w.reset()
    w.owned = True
    w.held_life = 0
    w.create_delay = 3
    for _ in range(5):
        w.frame(FALL)
    assert w.names(w.frame(FALL, pressed=True)) == ["slot"]
    seen = []
    for _ in range(8):
        seen += w.names(w.frame(FALL))
    assert seen[:2] == ["create", "wake"] and "bind" in seen and w.phase() == 2, seen

    # 6. The lift is refused three times: put away.
    w.reset()
    w.owned = True
    w.lift_ok = False
    for _ in range(5):
        w.frame(JUMP)
    w.frame(JUMP, pressed=True)
    seen = []
    for _ in range(8):
        seen += [c for c in w.frame(JUMP) if c[0] in ("start", "release")]
    assert seen == [("start", False)] * 3 + [("release",)], seen

    # 7. Something else took it out of Link's hands (the game dropped it): delete it.
    w.reset()
    w.owned = True
    open_glider(w)
    w.frame(GLIDE)
    w.w32(w.lifter + 0x2B0, 0)
    assert w.names(w.frame(GLIDE)) == ["cancel", "release"] and w.phase() == 0

    # 8. The arms never reach the carry state: give up after 30 frames.
    w.reset()
    w.owned = True
    w.select_works = False  # the selector's switch has no effect
    open_glider(w)
    seen = []
    for _ in range(40):
        seen += [n for n in w.names(w.frame(JUMP)) if n != "select"]
    assert seen == ["drop", "cancel", "release"], seen

    # 8b. Arms busy with something else (bow, hookshot): the press does nothing.
    w.reset(arms=59)
    w.owned = True
    for _ in range(5):
        w.frame(JUMP)
    assert w.frame(JUMP, pressed=True) == [] and w.phase() == 0

    # 9. Carrying something already: the press does nothing.
    w.reset()
    w.owned = True
    w.w32(w.lifter + 0x2B0, 3)
    for _ in range(5):
        w.frame(JUMP)
    assert w.frame(JUMP, pressed=True) == [] and w.phase() == 0

    # 10. A second Link's frames between Link's own do not disturb the count, the hold or the put-away.
    w.reset()
    w.owned = True
    for _ in range(5):
        assert w.frame(JUMP) == [] and w.other_frame() == []
    assert w.names(w.frame(JUMP, pressed=True)) == ["slot"] and w.other_frame() == []
    assert w.names(w.frame(JUMP)) == ["wake"] and w.other_frame() == []
    w.frame(JUMP), w.other_frame()
    assert w.names(w.frame(JUMP))[:3] == ["link", "start", "bind"] and w.phase() == 2
    for _ in range(10):
        assert w.other_frame() == [] and w.frame(GLIDE) == [] and w.phase() == 2
    w.frame(LAND), w.other_frame()
    assert w.names(w.frame(LAND)) == ["drop", "cancel", "release"]

    # 11. Link leaves for another area while holding it: the old Link's objects are never touched again,
    # and the new Link can open the paraglider.
    w.reset()
    w.owned = True
    open_glider(w)
    for _ in range(125):
        assert w.other_frame() == []
    assert w.phase() == 0
    w.w32(w.lifter + 0x2B0, 0)  # the next area's Link: empty hands, a fresh waiting paraglider
    w.w32(w.arms_rec + 0x28, NONE)
    w.held_life = 3
    open_glider(w)

    assert w.log == [], w.log  # no supervisor calls
    print("all cases passed")


if __name__ == "__main__":
    main()
