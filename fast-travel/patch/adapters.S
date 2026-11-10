// SPDX-License-Identifier: MIT
// Copyright (c) HanSoloBlaster, afiaka87

.section .text.patch_start,"ax"
.global patch_start
patch_start:
    stp x0, x1, [sp, #-32]!
    stp x29, x30, [sp, #16]
    bl patch_initialize
    ldp x29, x30, [sp, #16]
    ldp x0, x1, [sp], #32
    b patch_nnmain_body

.section .text.patch_main_base,"ax"
.global patch_main_base
patch_main_base:
    adrp x0, patch_image_base
    ret

.section .text.patch_debug_routine,"ax"
.global patch_debug_routine
patch_debug_routine:
    adrp x0, patch_tick_slot
    ldr x0, [x0, :lo12:patch_tick_slot]
    add x0, x0, #DEBUG_DELTA_PAGES, lsl #12
    add x0, x0, #DEBUG_DELTA_LOW
    ret

.macro hook name, bit, first_word, plt
    .section .text.patch_gate_\name,"ax"
    .global patch_gate_\name
patch_gate_\name:
    str x16, [sp, #-16]!
    adrp x16, patch_hook_mask
    add x16, x16, :lo12:patch_hook_mask
    ldar x16, [x16]
    tbz x16, #\bit, 1f
    ldr x16, [sp], #16
    b patch_callback_\name
1:
    ldr x16, [sp], #16
    // The closed gate and the callback's Orig call use the same replay.
    b patch_original_\name

    .section .text.patch_original_\name,"ax"
    .global patch_original_\name
patch_original_\name:
    .if \plt
        adrp x16, patch_plt_page_\name
    .else
        .inst \first_word
    .endif
    b patch_resume_\name
.endm

#include "patch_adapters.inc"
