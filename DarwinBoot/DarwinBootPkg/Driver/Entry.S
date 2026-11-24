// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <AsmMacroLib.h>

ASM_FUNC(DarwinCurrentEl)
  mrs x0, CurrentEL
  ret

ASM_FUNC(DarwinMpidr)
  mrs x0, mpidr_el1
  ret

ASM_FUNC(DarwinCounter)
  isb
  mrs x0, cntpct_el0
  ret

// x0=PA, x1=size: write back and drop the data cache lines of a mapped range.
ASM_FUNC(DarwinFlush)
  cbz x1, 2f
  add x1, x0, x1
  mrs x2, ctr_el0
  ubfx x2, x2, #16, #4
  mov x3, #4
  lsl x2, x3, x2
  sub x3, x2, #1
  bic x0, x0, x3
1:
  dc civac, x0
  add x0, x0, x2
  cmp x0, x1
  b.lo 1b
  dsb sy
2:
  ret

// x0=boot_args PA, x1=entry PA, x2=owned PA, x3=owned size, x4=frame buffer
// PA (0 for none), x5=its size in bytes, x6=the vectors EL2 is left with (a
// copy of DarwinEl2Vectors outside the kernel's memory; used at EL2 only).
//
// At EL1 the kernel is entered at EL1 with the MMU off. At EL2 (the Pixel 8a's
// bootloader starts the firmware there) the kernel is entered at EL1 as well:
// the EL2 MMU and caches go off, EL2 is set up so that nothing the kernel does
// traps to it (DarwinEl2ToEl1), and an exception return drops to EL1h with
// every interrupt masked. Nothing of the loader stays at EL2 but the small
// vectors in x6.
ASM_FUNC(DarwinEnter)
  msr daifset, #15
  // Paint the top 1/16 of the framebuffer green (BGRA 0xFF00FF00)
  // before cache maintenance, MMU shutdown and the branch to the kernel.
  cbz x4, 2f
  lsr x9, x5, #4
  bic x9, x9, #7
  mov w10, #0xFF00
  movk w10, #0xFF00, lsl #16
  mov x11, x4
3:
  stp w10, w10, [x11], #8
  subs x9, x9, #8
  b.hi 3b
  dsb sy
2:
  add x5, x2, x3
  mrs x7, ctr_el0
  ubfx x7, x7, #16, #4
  mov x8, #4
  lsl x7, x8, x7
  sub x8, x7, #1
  bic x4, x2, x8
  dsb sy
1:
  dc civac, x4
  add x4, x4, x7
  cmp x4, x5
  b.lo 1b
  dsb sy
  ic iallu
  dsb sy
  isb
  mrs x4, CurrentEL
  cmp x4, #8
  b.eq 5f
  mrs x4, sctlr_el1
  bic x4, x4, #1
  bic x4, x4, #4
  bic x4, x4, #0x1000
  msr sctlr_el1, x4
  isb
  tlbi vmalle1
  dsb sy
  isb
  br x1
5:
  // EL2: the firmware's EL2 translation (an identity map) and caches off.
  mrs x4, sctlr_el2
  bic x4, x4, #1
  bic x4, x4, #4
  bic x4, x4, #0x1000
  msr sctlr_el2, x4
  isb
  tlbi alle2
  dsb sy
  isb
  msr vbar_el2, x6
  bl DarwinEl2ToEl1
  eret

// The kernel's other cores. The firmware (PSCI CPU_ON) starts them at EL2 when
// the loader itself ran at EL2; the kernel asks for this code instead of its
// reset vector (/chosen/el2-trampoline, XNU's psci_smp.c) and passes the reset
// vector as the context, x0. The block from here to DarwinEl2VectorsOffset is
// copied out of the loader's image into memory outside the kernel's window
// (the kernel never hands it out); it is position independent. A core that
// was started at EL1 goes straight on.
  .text
  .balign 64
ASM_GLOBAL ASM_PFX(DarwinEl2Trampoline)
ASM_PFX(DarwinEl2Trampoline):
  msr daifset, #0xf
  mov x19, x0
  mrs x1, CurrentEL
  cmp x1, #8
  b.ne 1f
  adr x1, DarwinEl2Vectors
  msr vbar_el2, x1
  mov x1, x19
  bl DarwinEl2ToEl1
  mov x0, #0
  eret
1:
  mov x0, #0
  br x19

// The EL2 set-up for a kernel at EL1 (the same as the old loader's, which ran
// this kernel on this phone): x1 = where EL1 begins. Uses x4 only and
// returns with ELR_EL2 and SPSR_EL2 set for the eret. HCR_EL2: EL1 is AArch64,
// no VHE, no stage 2, nothing trapped, pointer authentication at EL0/EL1 runs
// instead of trapping (arm64e user space); the physical counter and timer,
// floating point, the performance counters and the GIC's system registers are
// EL1's; EL1 starts with its MMU and caches off.
DarwinEl2ToEl1:
  mov x4, #(1 << 31)                  // HCR_EL2.RW
  orr x4, x4, #(3 << 40)              // HCR_EL2.API, APK
  msr hcr_el2, x4
  isb
  mov x4, #0x32ff                     // CPTR_EL2: its fixed bits; no trap of FP, SVE or tracing
  msr cptr_el2, x4
  mov x4, #3                          // CNTHCTL_EL2: EL1PCTEN, EL1PCEN
  msr cnthctl_el2, x4
  msr cntvoff_el2, xzr                // the virtual counter is the physical one
  mrs x4, midr_el1                    // what EL1 reads as MIDR_EL1 and MPIDR_EL1: this core's
  msr vpidr_el2, x4
  mrs x4, mpidr_el1
  msr vmpidr_el2, x4
  mrs x4, pmcr_el0                    // MDCR_EL2.HPMN: every counter EL1's; no debug traps
  ubfx x4, x4, #11, #5
  msr mdcr_el2, x4
  mrs x4, s3_4_c12_c9_5               // ICC_SRE_EL2: SRE, Enable
  orr x4, x4, #0x1
  orr x4, x4, #0x8
  msr s3_4_c12_c9_5, x4
  msr s3_4_c12_c11_0, xzr             // ICH_HCR_EL2: no virtual GIC interface
  msr vttbr_el2, xzr
  isb
  mov x4, #0x0800
  movk x4, #0x30d0, lsl #16           // SCTLR_EL1: its fixed bits (RES1); M, C, I clear
  msr sctlr_el1, x4
  mov x4, #(3 << 20)                  // CPACR_EL1: floating point allowed
  msr cpacr_el1, x4
  isb
  tlbi alle1
  dsb sy
  isb
  mov x4, #0x3c5                      // SPSR_EL2: EL1h, D A I F masked
  msr spsr_el2, x4
  msr elr_el2, x1
  ret

// A synchronous exception from EL1: an HVC with PSCI CPU_SUSPEND (SMC64) in x0
// is made here as the SMC it stands for and the answer handed back (the Pixel
// 8a's firmware refuses CPU_SUSPEND from EL1; a core that powers down comes
// back at the entry it gave, this trampoline). Another HVC: NOT_SUPPORTED (-1).
// Anything else switches the core off (PSCI CPU_OFF): no loader is left on it.
DarwinEl2Sync:
  mrs x9, esr_el2
  lsr x9, x9, #26
  cmp x9, #0x16                       // EC 0x16: HVC from AArch64
  b.ne DarwinEl2Off
  mov x9, #0x0001
  movk x9, #0xc400, lsl #16           // PSCI CPU_SUSPEND (SMC64)
  cmp x0, x9
  b.ne 3f
  smc #0
  eret
3:
  mov x0, #-1
  eret
DarwinEl2Off:
  mov x0, #0x0002
  movk x0, #0x8400, lsl #16           // PSCI CPU_OFF
  smc #0
2:
  wfi
  b 2b

  .balign 0x800
DarwinEl2Vectors:
  b DarwinEl2Off                      // current EL with SP0
  .balign 0x80
  b DarwinEl2Off
  .balign 0x80
  b DarwinEl2Off
  .balign 0x80
  b DarwinEl2Off
  .balign 0x80
  b DarwinEl2Off                      // current EL with SPx
  .balign 0x80
  b DarwinEl2Off
  .balign 0x80
  b DarwinEl2Off
  .balign 0x80
  b DarwinEl2Off
  .balign 0x80
  b DarwinEl2Sync                     // lower EL, AArch64: synchronous
  .balign 0x80
  b DarwinEl2Off
  .balign 0x80
  b DarwinEl2Off
  .balign 0x80
  b DarwinEl2Off
  .balign 0x80
  b DarwinEl2Off                      // lower EL, AArch32
  .balign 0x80
  b DarwinEl2Off
  .balign 0x80
  b DarwinEl2Off
  .balign 0x80
  b DarwinEl2Off
  .balign 0x80
// Where the vectors lie in the block, and the block's size.
ASM_GLOBAL ASM_PFX(DarwinEl2VectorsOffset)
ASM_PFX(DarwinEl2VectorsOffset):
  .quad DarwinEl2Vectors - DarwinEl2Trampoline
ASM_GLOBAL ASM_PFX(DarwinEl2TrampolineSize)
ASM_PFX(DarwinEl2TrampolineSize):
  .quad DarwinEl2VectorsOffset - DarwinEl2Trampoline
