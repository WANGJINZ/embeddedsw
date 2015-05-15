/******************************************************************************
*
* Copyright (C) 2015 Xilinx, Inc.  All rights reserved.
*
* Permission is hereby granted, free of charge, to any person obtaining a copy
* of this software and associated documentation files (the "Software"), to deal
* in the Software without restriction, including without limitation the rights
* to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
* copies of the Software, and to permit persons to whom the Software is
* furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* Use of the Software is limited solely to applications:
* (a) running on a Xilinx device, or
* (b) that interact with a Xilinx device through a bus or interconnect.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
* XILINX  BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
* WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF
* OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
* SOFTWARE.
*
* Except as contained in this notice, the name of the Xilinx shall not be used
* in advertising or otherwise to promote the sale, use or other dealings in
* this Software without prior written authorization from Xilinx.
*
******************************************************************************/

/*
 *
 * CONTENT
 * Assumptions: only PROCESSOR core is executing this code,
 * other cores in PROCESSOR subsystem are already powered down.
 * 1) PROCESSOR configures timer0 peripheral to generate interrupts.
 * 2) PROCESSOR waits for few interrupts to be generated by the timer and
 *    then initiates self suspend. Before calling pm_self_suspend APU
 *    has saved its context (which is in this case only tick_count
 *    variable value) in CONTEXT memory. Suspending of the PROCESSOR is followed
 *    by CONTEXT retention.
 * 3) Timer is still counting while PROCESSOR is suspended and the next timer
 *    interrupt causes CONTEXT to be woken up by PMU.
 * 4) Processor resumes its execution, meaning that it restores value of
 *    tick_count from CONTEXT MEM and does not configure timer again because it
 *    is already configured. PROCESSOR enables interrupts at the processor
 *    level (CPSR) and handle timer interrupt that caused wake-up.
 * 5) PROCESSOR waits for few more timer interrupts and repeats the suspend
 *    procedure.
 */

#include <xil_exception.h>
#include <xil_printf.h>
#include <xil_io.h>
#include <xil_cache.h>
#include <xstatus.h>
#include <sleep.h>
#include "pm_api_sys.h"
#include "timer.h"
#include "pm_client.h"

extern void *_vector_table;

#ifdef __aarch64__
	/* Use OCM for saving context */
	#define CONTEXT_MEM_BASE	0xFFFC0000U
#else
	/* Use TCM for saving context */
	#define CONTEXT_MEM_BASE	0x8000U
#endif

/* The below sections will be saved during suspend */
extern u8 __data_start;
extern u8 __bss_start__;
extern u8 __data_end;
extern u8 __bss_end__;

/**
 * SaveContext() - called to save context of bss and data sections in OCM
 */
static void SaveContext(void)
{
	u8 *MemPtr;
	u8 *ContextMemPtr = (u8 *)CONTEXT_MEM_BASE;

	for (MemPtr = &__data_start; MemPtr < &__data_end; MemPtr++, ContextMemPtr++) {
		*ContextMemPtr = *MemPtr;
	}

	for (MemPtr = &__bss_start__; MemPtr < &__bss_end__; MemPtr++, ContextMemPtr++) {
		*ContextMemPtr = *MemPtr;
	}

	pm_dbg("Saved context (tick_count = %d)\n", TickCount);
}

/**
 * RestoreContext() - called to restore context of bss and data sections from OCM
 */
static void RestoreContext(void)
{
	u8 *MemPtr;
	u8 *ContextMemPtr = (u8 *)CONTEXT_MEM_BASE;

	for (MemPtr = &__data_start; MemPtr < &__data_end; MemPtr++, ContextMemPtr++) {
		*MemPtr = *ContextMemPtr;
	}

	for (MemPtr = &__bss_start__; MemPtr < &__bss_end__; MemPtr++, ContextMemPtr++) {
		*MemPtr = *ContextMemPtr;
	}

	pm_dbg("Restored context (tick_count = %d)\n", TickCount);
}

static u32 GetCpuId(void)
{
#ifdef __aarch64__
	u64 id;

	__asm__ volatile("mrs	%0, MPIDR_EL1\n"
			: "=r"(id)
	);
#else
	u32 id;

	__asm__ volatile("mrc	p15, 0, %0, c0, c0, 5\n"
			: "=r"(id)
	);
#endif

	return id & 0xff;
}

/**
 * PrepareSuspend() - save context and request suspend
 */
static void PrepareSuspend(void)
{
	SaveContext();
/* usleep is used to prevents UART prints from overlapping */
#ifdef __aarch64__
	u64 rvbar;
	u64 vector_base = (u64)&_vector_table;

	/* APU */
	XPm_SelfSuspend(NODE_APU_0, MAX_LATENCY, 0);
	usleep(100000);
	XPm_SetRequirement(NODE_OCM_BANK_0, PM_CAP_CONTEXT, 0, REQ_ACK_NO);
	usleep(100000);
	XPm_SetRequirement(NODE_OCM_BANK_1, PM_CAP_CONTEXT, 0, REQ_ACK_NO);
	usleep(100000);
	XPm_SetRequirement(NODE_OCM_BANK_2, PM_CAP_CONTEXT, 0, REQ_ACK_NO);
	usleep(100000);
	XPm_SetRequirement(NODE_OCM_BANK_3, PM_CAP_CONTEXT, 0, REQ_ACK_NO);
	usleep(100000);

	/*
	 * Set RVBAR to ensure we resume at the expected address
	 * FIXME: This should be communicated to FW which has to set this.
	 */
	rvbar = APU_RVBARADDR0L;
	rvbar += 8 * GetCpuId();
	Xil_Out32(rvbar, vector_base & 0xffffffff);
	rvbar += 4;
	Xil_Out32(rvbar, vector_base >> 32);
#else
	u32 reg, rpuctrl;
	u32 vector_base = (u32)&_vector_table;

	/* RPU */
	XPm_SelfSuspend(NODE_RPU_0, MAX_LATENCY, 0);
	usleep(100000);
	XPm_SetRequirement(NODE_TCM_0_A, PM_CAP_CONTEXT, 0, REQ_ACK_NO);
	usleep(100000);
	XPm_SetRequirement(NODE_TCM_0_B, PM_CAP_CONTEXT, 0, REQ_ACK_NO);
	usleep(100000);
	XPm_SetRequirement(NODE_TCM_1_A, PM_CAP_CONTEXT, 0, REQ_ACK_NO);
	usleep(100000);
	XPm_SetRequirement(NODE_TCM_1_B, PM_CAP_CONTEXT, 0, REQ_ACK_NO);
	usleep(100000);

	/*
	 * Set VINITH to ensure we resume at the expected address
	 * FIXME: This should be communicated to FW which has to set this.
	 */
	if (GetCpuId() == 0U) {
		rpuctrl = RPU_RPU_0_CFG;
	} else {
		rpuctrl = RPU_RPU_1_CFG;
	}

	reg = Xil_In32(rpuctrl);
	if (vector_base == 0) {
		reg &= ~RPU_RPU_0_CFG_VINITHI_MASK;
	} else {
		reg |= RPU_RPU_0_CFG_VINITHI_MASK;
	}
	Xil_Out32(rpuctrl, reg);
#endif /* __aarch64__ */
}

/**
 * InitApp() - initialize interrupts and context
 */
static u32 InitApp(void)
{
	enum XPmBootStatus status = XPm_GetBootStatus();

	pm_dbg("Main\n");
	if (PM_INITIAL_BOOT == status) {
		pm_dbg("INITIAL BOOT\n");
		/* Configure timer, if configuration fails return from main */
		if (XST_FAILURE == TimerConfigure(TIMER_PERIOD)) {
			pm_dbg("Exiting main...\n");
			return XST_FAILURE;
		}
	} else if (PM_RESUME == status) {
		pm_dbg("RESUMED\n");
		RestoreContext();
		/* Timer is already counting, just enable interrupts */
		Xil_ExceptionEnable();
	} else {
		pm_dbg("ERROR cannot identify boot reason\n");
	}

	return XST_SUCCESS;
}

int main(void)
{
	Xil_DCacheDisable();
	u32 Status = InitApp();

	if (XST_SUCCESS != Status) {
		return XST_FAILURE;
	}

	pm_dbg("Waiting for ticks...\n");
	/* Wait for 3 timer ticks */
	while ((TickCount + 1) % 4);

	PrepareSuspend();
	pm_dbg("Going to WFI...\n");
	__asm__("wfi");

	/*
	 * Can execute code below only if interrupt is generated between calling
	 * the PrepareSuspend and executing wfi. Shouldn't happen.
	 */
	pm_dbg("Error! WFI exit...\n");

	return XST_FAILURE;
}
