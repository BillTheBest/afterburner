/*********************************************************************
 *
 * Copyright (C) 2005,  University of Karlsruhe
 *
 * File path:     afterburn-wedge/amd64/config.h
 * Description:   
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * $Id: config.h,v 1.4 2005/04/14 19:51:16 joshua Exp $
 *
 ********************************************************************/
#ifndef __AMD64__CONFIG_H__
#define __AMD64__CONFIG_H__

#ifndef CONFIG_ARCH_AMD64
#define CONFIG_ARCH_AMD64
#endif

#define CONFIG_ARCH		amd64
#define CONFIG_BITWIDTH		64
#define CONFIG_PAGEBITS		12

/* XXX What do these mean? */
#define CONFIG_MOREPAGEBITS	22
#define CONFIG_NUM_MOREPAGEBITS	1

#define CONFIG_STACK_ALIGN	(16UL)
#define CONFIG_STACK_SAFETY	(4UL)

#define CONFIG_MONITOR_STACK_SIZE	(KB(16))

#if !defined(CONFIG_VSMP)
#define CONFIG_NR_VCPUS		1
#endif
#if !defined(CONFIG_SMP)
#define CONFIG_NR_CPUS		1
#endif
#endif	/* __AMD64__CONFIG_H__ */
