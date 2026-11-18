// SPDX-License-Identifier: MIT
// Copyright (c) HanSoloBlaster, afiaka87

// Paraglider for the Ocarina of Time remake (Switch 2), main E0F3CCFA (2.0.0). A second jump press in mid-air
// puts the paraglider in Link's hands the way the game hands him a bomb, so the game's own cucco float takes over.

typedef unsigned char u8;
typedef unsigned int u32;
typedef unsigned long u64;

#define AT(type, base, off) (*(type *)((u8 *)(base) + (off)))
#define VCALL(type, obj, off) ((type)AT(void *, AT(void *, obj, 0), off))

enum { VERB_NONE = 0, VERB_JUMP = 13, VERB_FALL = 33, VERB_LIFT = 43, VERB_DRAWN = 57, VERB_GLIDE = 82 };
enum { KEY_JUMP = 0 };
enum { IDLE, WAKING, HELD };
enum {
    MIN_AIR_FRAMES = 4,     // ignore the press that started the jump
    WAKE_TIMEOUT = 45,
    LIFT_TIMEOUT = 30,
    REPRESS_FRAMES = 10,    // frames before a second press puts it away
    GROUND_FRAMES = 2,
    ABSENT_FRAMES = 120,    // frames without the holding Link before his state is forgotten
};

struct ActorLink {
    void *vtable;
    u64 zero;
    u64 id;
};

struct State {
    void *actor;      // the Link this state belongs to
    void *slot;
    u32 phase;
    u32 air;          // frames in Jump or Fall
    u32 timer;        // frames in the current phase
    u32 ground;
    u8 woke;
    u8 tries;
    u8 asked;         // the holder was asked for a paraglider this attempt
    u32 absent;
    void *air_actor;  // the Link the air count belongs to
};

// Fixed addresses, supplied by build_patch.py.
extern struct State glider_state;
extern u8 game_link_vtable[];
extern u8 game_link_dead_vtable[];
extern void *game_lift_param_slot;
extern u8 *game_wake_vtable_slot;
extern const u32 *game_wake_vec_slot;
extern void **game_gmd_slot;

extern u8 game_sub_main_vtable[];
u64 game_sub_pass(void *node, void *arg, void *x2, void *x3);
u32 game_sub_none_check(void *node, void *a, void *b, void *c);
u32 game_sub_drawn_check(void *node, void *a, void *b, void *c);
u32 game_select_child(void *node, u32 child);
u32 game_key_pressed(void *player, u32 key);
void *game_holder_slot_by_name(void *holder, const char **name, u32 index);
u32 game_lift_start(void *lifter, struct ActorLink *link, void *param);
u32 game_lift_bind(void *lifter, u32 release_on_fail, u32 immediate);
void game_lift_cancel(void *lifter);
void game_drop(void *actor, u32 mode);
void game_link_set(struct ActorLink *link, void *actor, u64 zero);
void game_link_destroy(struct ActorLink *link);
u32 game_link_valid(void *link);
u32 game_gmd_get_bool(void *manager, u8 *out, u32 hash);

typedef u32 (*SlotBool)(void *slot);
typedef void (*SlotVoid)(void *slot);
typedef u32 (*SlotWake)(void *slot, void *arg, void *owner);
typedef u32 (*SlotCreate)(void *slot, void *owner, u64 zero);

static void *node_actor(void *node)
{
    void *p = AT(void *, node, 8);
    if (!p) return 0;
    p = AT(void *, p, 8);
    return p ? AT(void *, p, 0x1F0) : 0;
}

static u32 verb(void *comps, u32 record)
{
    void *bsa = AT(void *, comps, 0x188);
    void *rec = bsa ? AT(void *, bsa, record) : 0;
    if (!rec) return 0;
    u32 v = AT(u32, rec, 0x28);
    return v == 282 ? AT(u32, rec, 0x4C) : v;
}

static u32 carrying(void *lifter)
{
    return (AT(u32, lifter, 0x2B0) & ~1u) == 2 && (game_link_valid((u8 *)lifter + 0x1A8) & 1);
}

static u32 flag(void *manager, u32 hash)
{
    u8 value = 0;
    return (game_gmd_get_bool(manager, &value, hash) & 1) ? (value & 1) : 0;
}

// Link has the item, or has at least opened its chest (if the pouch did not take it).
static u32 owned(void)
{
    void **global = game_gmd_slot;
    void *manager = global ? *global : 0;
    if (!manager) return 0;
    return flag(manager, GLIDER_OWN_HASH) | (flag(manager, GLIDER_CHEST_HASH) << 1);
}

// Read the handle directly: slot +0x90 returns nothing while the actor is waiting (boot 5).
static void *slot_actor(void *slot)
{
    if (!(VCALL(SlotBool, slot, 0x40)(slot) & 1)) return 0;
    u32 id = AT(u32, slot, 0x38);
    void *entry = AT(void *, slot, 0x30);
    if (id == 0xFFFFFFFFu || !entry || AT(u32, entry, 0x48) != id) return 0;
    return AT(void *, entry, 0x40);
}

// Same call the holder makes for each entry when Link is created; refused while the slot has an actor or request.
static void ask(void *slot, void *holder, void *actor)
{
    void *owner = AT(void *, holder, 0x18);
    VCALL(SlotCreate, slot, 0x50)(slot, owner ? owner : actor, 0);
}

// Wake the waiting actor above Link, as the bomb pull-out does (0x27E2A60).
static void wake(void *slot, void *actor)
{
    u8 arg[0x58];
    for (u32 i = 0; i < sizeof arg; i++) arg[i] = 0;
    const float *at = &AT(float, actor, 0x31C);
    const u32 *vec = game_wake_vec_slot;
    AT(void *, arg, 0x00) = game_wake_vtable_slot + 0x10;
    AT(float, arg, 0x08) = at[0];
    AT(float, arg, 0x0C) = at[1] + 1.0f;
    AT(float, arg, 0x10) = at[2];
    AT(float, arg, 0x14) = 1.0f;
    AT(float, arg, 0x24) = 1.0f;
    AT(float, arg, 0x34) = 1.0f;
    AT(u32, arg, 0x38) = vec[0];
    AT(u32, arg, 0x3C) = vec[1];
    AT(u32, arg, 0x40) = vec[2];
    arg[0x50] = 3;
    VCALL(SlotWake, slot, 0x18)(slot, arg, actor);
}

// Put the actor in Link's hands at once, as the bomb pull-out does (0x27E2C50).
static u32 attach(void *lifter, void *held)
{
    struct ActorLink link;
    link.vtable = game_link_vtable;
    link.zero = 0;
    link.id = 0x1FFFFFFFFul;
    game_link_set(&link, held, 0);
    u32 started = game_lift_start(lifter, &link, game_lift_param_slot) & 1;
    u32 bound = started ? game_lift_bind(lifter, 1, 1) & 1 : 0;
    link.vtable = game_link_dead_vtable;
    game_link_destroy(&link);
    return bound;
}

// Drop and delete the paraglider; the holder makes a fresh waiting one.
static void stow(struct State *g, void *actor, void *lifter)
{
    if (carrying(lifter)) game_drop(actor, 0);
    game_lift_cancel(lifter);
    void *slot = g->slot;
    u32 used = g->phase == HELD || g->woke;  // an actor that never woke stays waiting in the holder
    if (slot && used && (VCALL(SlotBool, slot, 0x40)(slot) & 1)) VCALL(SlotVoid, slot, 0x78)(slot);
    g->phase = IDLE;
    g->slot = 0;
    g->timer = 0;
    g->ground = 0;
    g->woke = 0;
    g->tries = 0;
}

// Once a frame, for SelectorPlayerSubSchedulerMain. Each press is seen once.
void glider_frame(void *node)
{
    struct State *g = &glider_state;
    void *actor = node_actor(node);
    void *comps = actor ? AT(void *, actor, 0x270) : 0;
    u8 *player = comps ? AT(u8 *, comps, 0x358) : 0;
    void *lifter = comps ? AT(void *, comps, 0x2C8) : 0;
    void *holder = comps ? AT(void *, comps, 0x110) : 0;
    if (!player || !lifter || !holder) return;

    if (g->phase != IDLE && actor != g->actor) {
        // another Link's frame; if the holding Link stays away (new area, age change) forget him untouched
        if (++g->absent > ABSENT_FRAMES) {
            g->phase = IDLE;
            g->slot = 0;
            g->actor = 0;
        }
        return;
    }
    g->absent = 0;

    u32 main_verb = verb(comps, 0x1C0);
    u32 arms_verb = verb(comps, 0x1C8);
    u32 falling = main_verb == VERB_JUMP || main_verb == VERB_FALL;
    u32 gliding = main_verb == VERB_GLIDE;
    u32 pressed = game_key_pressed(player, KEY_JUMP) & 1;

    if (falling) {
        if (g->air_actor != actor) {
            g->air_actor = actor;
            g->air = 0;
        }
        g->air++;
    } else if (!gliding && g->air_actor == actor) {
        g->air = 0;
    }

    if (g->phase == IDLE) {
        if (!falling || !pressed || g->air < MIN_AIR_FRAMES || g->air_actor != actor) return;
        if (!owned() || AT(u32, lifter, 0x2B0) != 0 || (arms_verb != VERB_NONE && arms_verb != VERB_DRAWN)) return;
        const char *name = "Obj_Paraglider";
        void *slot = game_holder_slot_by_name(holder, &name, 0);
        if (!slot) return;
        g->actor = actor;
        g->slot = slot;
        g->phase = WAKING;
        g->timer = 0;
        g->woke = 0;
        g->tries = 0;
        g->asked = 0;
        return;
    }

    g->timer++;

    if (g->phase == WAKING) {
        if (!(falling || gliding) || g->timer > WAKE_TIMEOUT) {
            stow(g, actor, lifter);
            return;
        }
        void *held = slot_actor(g->slot);
        if (!held) {
            // One that arrives after Link lands stays waiting for the next jump.
            if (!g->asked) {
                g->asked = 1;
                ask(g->slot, holder, actor);
            }
            return;
        }
        u32 life = AT(u32, held, 0x2C);
        if ((life & ~1u) == 4) {
            if (attach(lifter, held)) {
                g->phase = HELD;
                g->timer = 0;
                g->ground = 0;
            } else if (++g->tries >= 3) {
                stow(g, actor, lifter);
            }
        } else if ((life == 3 || life == 6) && !g->woke) {
            g->woke = 1;
            wake(g->slot, actor);
        }
        return;
    }

    // HELD
    if (!carrying(lifter)) {
        stow(g, actor, lifter);
        return;
    }
    if (arms_verb != VERB_LIFT) {
        AT(u32, player, 0xE40) |= 2;  // ask the carry action to start in its holding mode
        if (g->timer > LIFT_TIMEOUT) {
            stow(g, actor, lifter);
            return;
        }
    }
    g->ground = (falling || gliding) ? 0 : g->ground + 1;
    if (g->ground >= GROUND_FRAMES || (pressed && g->timer > REPRESS_FRAMES))
        stow(g, actor, lifter);
}

// True while the paraglider is in Link's hands and his arms should switch to carrying.
static u8 *wants_lift(void *node)
{
    struct State *g = &glider_state;
    void *actor = node_actor(node);
    if (g->phase != HELD || !actor || actor != g->actor) return 0;
    void *comps = AT(void *, actor, 0x270);
    void *lifter = comps ? AT(void *, comps, 0x2C8) : 0;
    if (!lifter || !carrying(lifter)) return 0;
    return AT(u8 *, comps, 0x358);
}

// The upper-body selectors' shared pass (0x2B41580) runs for Main, then for the active selector.
u64 hook_sub_pass(void *node, void *arg, void *x2, void *x3)
{
    if (AT(void *, node, 0) == (void *)game_sub_main_vtable) glider_frame(node);
    return game_sub_pass(node, arg, x2, x3);
}

// Replaces SelectorPlayerSubSchedulerNone's priority check (0x2B43870).
u32 hook_sub_none(void *node, void *a, void *b, void *c)
{
    u8 *player = wants_lift(node);
    if (!player) return game_sub_none_check(node, a, b, c);
    AT(u32, player, 0xE40) |= 2;
    return game_select_child(node, AT(u8, node, 0x23));
}

// Replaces SelectorPlayerSubSchedulerDrawn's priority check (0x499B70).
u32 hook_sub_drawn(void *node, void *a, void *b, void *c)
{
    u8 *player = wants_lift(node);
    if (!player) return game_sub_drawn_check(node, a, b, c);
    AT(u32, player, 0xE38) |= 8;
    AT(u32, player, 0xE40) |= 2;
    return game_select_child(node, AT(u8, node, 0x2D));
}
