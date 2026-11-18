// SPDX-License-Identifier: MIT
// Copyright (c) HanSoloBlaster, afiaka87

// Each trampoline runs the game instruction its hook replaced, then continues in the game's routine.
.section .text.game_trampolines,"ax"
.global game_sub_pass
game_sub_pass:
    .inst 0xD503237F            // pacibsp, the first instruction of 0x2B41580
    b game_sub_pass_body

.global game_sub_none_check
game_sub_none_check:
    .inst 0xF9400408            // ldr x8, [x0, #8], the first instruction of 0x2B43870
    b game_sub_none_body

.global game_sub_drawn_check
game_sub_drawn_check:
    .inst 0xD503237F            // pacibsp, the first instruction of 0x499B70
    b game_sub_drawn_body
