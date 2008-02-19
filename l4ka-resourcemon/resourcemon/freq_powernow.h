/*
 *  (c) 2003-2006 Advanced Micro Devices, Inc.
 *  Your use of this code is subject to the terms and conditions of the
 *  GNU general public license version 2. See "COPYING" or
 *  http://www.gnu.org/licenses/gpl.html
 */

#ifndef __RESOURCEMON__FREQ_POWERNOW_H__
#define __RESOURCEMON__FREQ_POWERNOW_H__

#include "resourcemon/freq_acpi.h"

/* Model Specific Registers for p-state transitions. MSRs are 64-bit. For     */
/* writes (wrmsr - opcode 0f 30), the register number is placed in ecx, and   */
/* the value to write is placed in edx:eax. For reads (rdmsr - opcode 0f 32), */
/* the register number is placed in ecx, and the data is returned in edx:eax. */

/* Hardware Pstate _PSS and MSR definitions */
#define HW_PSTATE_MASK 			0x00000007
#define HW_PSTATE_VALID_MASK	0x80000000
#define HW_PSTATE_MAX_MASK		0x000000f0
#define HW_PSTATE_MAX_SHIFT		4
#define MSR_PSTATE_DEF_BASE 	0xc0010064 /* base of Pstate MSRs */
#define MSR_PSTATE_STATUS 		0xc0010063 /* pstate Status MSR */
#define MSR_PSTATE_CTRL 		0xc0010062 /* pstate control MSR */
#define MSR_PSTATE_CUR_LIMIT	0xc0010061 /* pstate current limit MSR */

#define MSR_COFVID_STATUS		0xc0010071 // limits and current values MSR
#define CUR_NBVID_MASK			0xfe000000
#define CUR_NBVID_SHIFT			25
#define CUR_CPUVID_MASK			0x0000fe00
#define CUR_CPUVID_SHIFT		9
#define MIN_VID_MASK			0x0001fc00
#define MIN_VID_SHIFT			10
#define MAX_VID_MASK			0x000003f8
#define MAX_VID_SHIFT			3

struct powernow_data_t {
	u32_t max_hw_pstate; // maximum legal hardware pstate
	u32_t min_freq;
	u32_t max_freq;
	unsigned int pstate_count;
	struct acpi_pss_t pstates[MAX_PSTATES]; // MANUEL: fixed array of size 10 for now
};

void powernow_init(L4_Word_t num_cpus);
int powernow_target(unsigned target_freq/*, unsigned relation*/);
u32_t powernow_get_freq(void); // get current core operating frequency
u32_t powernow_get_vdd(void); // get current voltage

#endif //!__RESOURCEMON__FREQ_POWERNOW_H__
