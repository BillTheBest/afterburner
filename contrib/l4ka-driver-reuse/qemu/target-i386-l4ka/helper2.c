/*
 *  i386 helpers (without register variable usage)
 *
 *  Copyright (c) 2003 Fabrice Bellard
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

/*
 * Main cpu loop for handling I/O requests coming from a virtual machine
 * Copyright � 2004, Intel Corporation.
 * Copyright � 2005, International Business Machines Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU Lesser General Public License,
 * version 2.1, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307 USA.
 */
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <signal.h>
#include <assert.h>

#include <limits.h>
#include <fcntl.h>

#ifdef CONFIG_L4KA
#include "ioreq.h"
#else
#include <xenctrl.h>
#include <xen/hvm/ioreq.h>
#endif /* CONFIG_L4KA with #else */

#include "cpu.h"
#include "exec-all.h"

//#define DEBUG_MMU

#ifdef USE_CODE_COPY
#include <asm/ldt.h>
#include <linux/unistd.h>
#include <linux/version.h>

_syscall3(int, modify_ldt, int, func, void *, ptr, unsigned long, bytecount)

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 5, 66)
#define modify_ldt_ldt_s user_desc
#endif
#endif /* USE_CODE_COPY */

#include "vl.h"

int domid = -1;
int vcpus = 1;

extern int xc_handle;

long time_offset = 0;

shared_iopage_t *shared_page = NULL;

/* the evtchn fd for polling */
int xce_handle = -1;

/* which vcpu we are serving */
int send_vcpu = 0;

#ifndef CONFIG_L4KA
//the evtchn port for polling the notification,
#define NR_CPUS 32
evtchn_port_t ioreq_local_port[NR_CPUS];
#endif /* !CONFIG_L4KA */

CPUX86State *cpu_x86_init(void)
{
    CPUX86State *env;
    static int inited;

    env = qemu_mallocz(sizeof(CPUX86State));
    if (!env)
        return NULL;
    cpu_exec_init(env);

    /* init various static tables */
    if (!inited) {
        inited = 1;

        cpu_single_env = env;

#ifndef CONFIG_L4KA
        xce_handle = xc_evtchn_open();
        if (xce_handle == -1) {
            perror("open");
            return NULL;
        }

        /* FIXME: how about if we overflow the page here? */
        for (i = 0; i < vcpus; i++) {
            rc = xc_evtchn_bind_interdomain(
                xce_handle, domid, shared_page->vcpu_iodata[i].vp_eport);
            if (rc == -1) {
                fprintf(logfile, "bind interdomain ioctl error %d\n", errno);
                return NULL;
            }
            ioreq_local_port[i] = rc;
        }
#endif /* !CONFIG_L4KA */
    }

    return env;
}

/* called from main_cpu_reset */
void cpu_reset(CPUX86State *env)
{
#ifndef CONFIG_L4KA
    int xcHandle;
    int sts;

    xcHandle = xc_interface_open();
    if (xcHandle < 0)
        fprintf(logfile, "Cannot acquire xenctrl handle\n");
    else {
        xc_domain_shutdown_hook(xcHandle, domid);
        sts = xc_domain_shutdown(xcHandle, domid, SHUTDOWN_reboot);
        if (sts != 0)
            fprintf(logfile,
                    "? xc_domain_shutdown failed to issue reboot, sts %d\n",
                    sts);
        else
            fprintf(logfile, "Issued domain %d reboot\n", domid);
        xc_interface_close(xcHandle);
    }
#endif /* CONFIG_L4KA */
}

void cpu_x86_close(CPUX86State *env)
{
    free(env);
}


void cpu_dump_state(CPUState *env, FILE *f,
                    int (*cpu_fprintf)(FILE *f, const char *fmt, ...),
                    int flags)
{
}

/***********************************************************/
/* x86 mmu */
/* XXX: add PGE support */

void cpu_x86_set_a20(CPUX86State *env, int a20_state)
{
    a20_state = (a20_state != 0);
    if (a20_state != ((env->a20_mask >> 20) & 1)) {
#if defined(DEBUG_MMU)
        fprintf(logfile,"A20 update: a20=%d\n", a20_state);
#endif
        env->a20_mask = 0xffefffff | (a20_state << 20);
    }
}

target_ulong cpu_get_phys_page_debug(CPUState *env, target_ulong addr)
{
        return addr;
}

//some functions to handle the io req packet
void sp_info()
{
#ifndef CONFIG_L4KA
    ioreq_t *req;
    int i;

    for (i = 0; i < vcpus; i++) {
        req = &(shared_page->vcpu_iodata[i].vp_ioreq);
        term_printf("vcpu %d: event port %d\n", i, ioreq_local_port[i]);
        term_printf("  req state: %x, ptr: %x, addr: %"PRIx64", "
                    "data: %"PRIx64", count: %"PRIx64", size: %"PRIx64"\n",
                    req->state, req->data_is_ptr, req->addr,
                    req->data, req->count, req->size);
        term_printf("  IO totally occurred on this vcpu: %"PRIx64"\n",
                    req->io_count);
    }
#endif /* !CONFIG_L4KA */
}

//get the ioreq packets from share mem
static ioreq_t *__cpu_get_ioreq(int vcpu)
{
    ioreq_t *req;

    req = &(shared_page->vcpu_iodata[vcpu].vp_ioreq);

    if (req->state != STATE_IOREQ_READY) {
        fprintf(logfile,"I/O request not ready: "
                "%x, ptr: %x, port: %llx, "
                "data: %llx, count: %llx, size: %llx\n",
                req->state, req->data_is_ptr, req->addr,
                req->data, req->count, req->size);
        return NULL;
	}

    rmb(); /* see IOREQ_READY /then/ read contents of ioreq */

    req->state = STATE_IOREQ_INPROCESS;

    return req;
}

//use poll to get the port notification
//ioreq_vec--out,the
//retval--the number of ioreq packet
static ioreq_t *cpu_get_ioreq(void)
{
#ifdef CONFIG_L4KA
    send_vcpu = 0;
    return __cpu_get_ioreq(0);
#else
    int i;
    evtchn_port_t port;

    port = xc_evtchn_pending(xce_handle);
    if (port != -1) {
        for ( i = 0; i < vcpus; i++ )
            if ( ioreq_local_port[i] == port )
                break;

        if ( i == vcpus ) {
            fprintf(logfile,"Fatal error while trying to get io event!\n");
            exit(1);
        }

        // unmask the wanted port again
        xc_evtchn_unmask(xce_handle, port);

        //get the io packet from shared memory
        send_vcpu = i;
        return __cpu_get_ioreq(i);
    }

    //read error or read nothing
    return NULL;
#endif /* CONFIG_L4KA with #else */
}

unsigned long do_inp(CPUState *env, unsigned long addr, unsigned long size)
{
    switch(size) {
    case 1:
        return cpu_inb(env, addr);
    case 2:
        return cpu_inw(env, addr);
    case 4:
        return cpu_inl(env, addr);
    default:
        fprintf(logfile,"inp: bad size: %lx %lx\n", addr, size);
        exit(-1);
    }
}

void do_outp(CPUState *env, unsigned long addr,
             unsigned long size, unsigned long val)
{
    switch(size) {
    case 1:
        return cpu_outb(env, addr, val);
    case 2:
        return cpu_outw(env, addr, val);
    case 4:
        return cpu_outl(env, addr, val);
    default:
        fprintf(logfile,"outp: bad size: %lx %lx\n", addr, size);
        exit(-1);
    }
}

extern void cpu_physical_memory_rw(target_phys_addr_t addr, uint8_t *buf,
                                   int len, int is_write);

static inline void read_physical(uint64_t addr, unsigned long size, void *val)
{
    return cpu_physical_memory_rw((target_phys_addr_t)addr, val, size, 0);
}

static inline void write_physical(uint64_t addr, unsigned long size, void *val)
{
    return cpu_physical_memory_rw((target_phys_addr_t)addr, val, size, 1);
}

void cpu_ioreq_pio(CPUState *env, ioreq_t *req)
{
    int i, sign;

    sign = req->df ? -1 : 1;

    if (req->dir == IOREQ_READ) {
        if (!req->data_is_ptr) {
            req->data = do_inp(env, req->addr, req->size);
        } else {
            unsigned long tmp;

            for (i = 0; i < req->count; i++) {
                tmp = do_inp(env, req->addr, req->size);
                write_physical((target_phys_addr_t) req->data
                  + (sign * i * req->size),
                  req->size, &tmp);
            }
        }
    } else if (req->dir == IOREQ_WRITE) {
        if (!req->data_is_ptr) {
            do_outp(env, req->addr, req->size, req->data);
        } else {
            for (i = 0; i < req->count; i++) {
                unsigned long tmp = 0;

                read_physical((target_phys_addr_t) req->data
                  + (sign * i * req->size),
                  req->size, &tmp);
                do_outp(env, req->addr, req->size, tmp);
            }
        }
    }
}

void cpu_ioreq_move(CPUState *env, ioreq_t *req)
{
    int i, sign;

    sign = req->df ? -1 : 1;

    if (!req->data_is_ptr) {
        if (req->dir == IOREQ_READ) {
            for (i = 0; i < req->count; i++) {
                read_physical(req->addr
                  + (sign * i * req->size),
                  req->size, &req->data);
            }
        } else if (req->dir == IOREQ_WRITE) {
            for (i = 0; i < req->count; i++) {
                write_physical(req->addr
                  + (sign * i * req->size),
                  req->size, &req->data);
            }
        }
    } else {
        target_ulong tmp;

        if (req->dir == IOREQ_READ) {
            for (i = 0; i < req->count; i++) {
                read_physical(req->addr
                  + (sign * i * req->size),
                  req->size, &tmp);
                write_physical((target_phys_addr_t )req->data
                  + (sign * i * req->size),
                  req->size, &tmp);
            }
        } else if (req->dir == IOREQ_WRITE) {
            for (i = 0; i < req->count; i++) {
                read_physical((target_phys_addr_t) req->data
                  + (sign * i * req->size),
                  req->size, &tmp);
                write_physical(req->addr
                  + (sign * i * req->size),
                  req->size, &tmp);
            }
        }
    }
}

void cpu_ioreq_and(CPUState *env, ioreq_t *req)
{
    target_ulong tmp1, tmp2;

    if (req->data_is_ptr != 0)
        hw_error("expected scalar value");

    read_physical(req->addr, req->size, &tmp1);
    if (req->dir == IOREQ_WRITE) {
        tmp2 = tmp1 & (target_ulong) req->data;
        write_physical(req->addr, req->size, &tmp2);
    }
    req->data = tmp1;
}

void cpu_ioreq_add(CPUState *env, ioreq_t *req)
{
    target_ulong tmp1, tmp2;

    if (req->data_is_ptr != 0)
        hw_error("expected scalar value");

    read_physical(req->addr, req->size, &tmp1);
    if (req->dir == IOREQ_WRITE) {
        tmp2 = tmp1 + (target_ulong) req->data;
        write_physical(req->addr, req->size, &tmp2);
    }
    req->data = tmp1;
}

void cpu_ioreq_sub(CPUState *env, ioreq_t *req)
{
    target_ulong tmp1, tmp2;

    if (req->data_is_ptr != 0)
        hw_error("expected scalar value");

    read_physical(req->addr, req->size, &tmp1);
    if (req->dir == IOREQ_WRITE) {
        tmp2 = tmp1 - (target_ulong) req->data;
        write_physical(req->addr, req->size, &tmp2);
    }
    req->data = tmp1;
}

void cpu_ioreq_or(CPUState *env, ioreq_t *req)
{
    target_ulong tmp1, tmp2;

    if (req->data_is_ptr != 0)
        hw_error("expected scalar value");

    read_physical(req->addr, req->size, &tmp1);
    if (req->dir == IOREQ_WRITE) {
        tmp2 = tmp1 | (target_ulong) req->data;
        write_physical(req->addr, req->size, &tmp2);
    }
    req->data = tmp1;
}

void cpu_ioreq_xor(CPUState *env, ioreq_t *req)
{
    target_ulong tmp1, tmp2;

    if (req->data_is_ptr != 0)
        hw_error("expected scalar value");

    read_physical(req->addr, req->size, &tmp1);
    if (req->dir == IOREQ_WRITE) {
        tmp2 = tmp1 ^ (target_ulong) req->data;
        write_physical(req->addr, req->size, &tmp2);
    }
    req->data = tmp1;
}

void timeoffset_get()
{
#ifndef CONFIG_L4KA
    char *p;

    p = xenstore_vm_read(domid, "rtc/timeoffset", NULL);
    if (!p)
	return;

    if (sscanf(p, "%ld", &time_offset) == 1)
	fprintf(logfile,"Time offset set %ld\n", time_offset);
    else
	time_offset = 0;

    xc_domain_set_time_offset(xc_handle, domid, time_offset);

    free(p);
#endif /* !CONFIG_L4KA */
}

void cpu_ioreq_timeoffset(CPUState *env, ioreq_t *req)
{
    time_offset += (ulong)req->data;

    fprintf(logfile,"Time offset set %ld, added offset %lld\n", time_offset, req->data);
#ifndef CONFIG_L4KA
    sprintf(b, "%ld", time_offset);
    xenstore_vm_write(domid, "rtc/timeoffset", b);
#endif /* CONFIG_L4KA */
}

void cpu_ioreq_xchg(CPUState *env, ioreq_t *req)
{
    unsigned long tmp1;

    if (req->data_is_ptr != 0)
        hw_error("expected scalar value");

    read_physical(req->addr, req->size, &tmp1);
    write_physical(req->addr, req->size, &req->data);
    req->data = tmp1;
}

void __handle_ioreq(CPUState *env, ioreq_t *req)
{
    if (!req->data_is_ptr && (req->dir == IOREQ_WRITE) &&
        (req->size < sizeof(target_ulong)))
        req->data &= ((target_ulong)1 << (8 * req->size)) - 1;

    switch (req->type) {
    case IOREQ_TYPE_PIO:
        cpu_ioreq_pio(env, req);
        break;
    case IOREQ_TYPE_COPY:
        cpu_ioreq_move(env, req);
        break;
    case IOREQ_TYPE_AND:
        cpu_ioreq_and(env, req);
        break;
    case IOREQ_TYPE_ADD:
        cpu_ioreq_add(env, req);
        break;
    case IOREQ_TYPE_SUB:
        cpu_ioreq_sub(env, req);
        break;
    case IOREQ_TYPE_OR:
        cpu_ioreq_or(env, req);
        break;
    case IOREQ_TYPE_XOR:
        cpu_ioreq_xor(env, req);
        break;
    case IOREQ_TYPE_XCHG:
        cpu_ioreq_xchg(env, req);
        break;
    case IOREQ_TYPE_TIMEOFFSET:
        cpu_ioreq_timeoffset(env, req);
        break;
    case IOREQ_TYPE_INVALIDATE:
        qemu_invalidate_map_cache();
        break;
    default:
        hw_error("Invalid ioreq type 0x%x\n", req->type);
    }
}

void cpu_handle_ioreq(void *opaque)
{
    extern int vm_running;
    extern int shutdown_requested;
    CPUState *env = opaque;
    ioreq_t *req = cpu_get_ioreq();

    if (req) {
        __handle_ioreq(env, req);

        if (req->state != STATE_IOREQ_INPROCESS) {
            fprintf(logfile,"Badness in I/O request ... not in service?!: "
                    "%x, ptr: %x, port: %"PRIx64", "
                    "data: %"PRIx64", count: %"PRIx64", size: %"PRIx64"\n",
                    req->state, req->data_is_ptr, req->addr,
                    req->data, req->count, req->size);
            destroy_hvm_domain();
            return;
        }

        wmb(); /* Update ioreq contents /then/ update state. */

	/*
         * We do this before we send the response so that the tools
         * have the opportunity to pick up on the reset before the
         * guest resumes and does a hlt with interrupts disabled which
         * causes Xen to powerdown the domain.
         */
        if (vm_running) {
            if (shutdown_requested) {
		fprintf(logfile,"shutdown requested in cpu_handle_ioreq\n");
		destroy_hvm_domain();
	    }
	    if (reset_requested) {
		fprintf(logfile,"reset requested in cpu_handle_ioreq.\n");
#ifdef CONFIG_QUIT_ON_REBOOT_REQUEST
		destroy_hvm_domain();
#endif
		qemu_system_reset();
		reset_requested = 0;
	    }
	}

        req->state = STATE_IORESP_READY;

#ifndef CONFIG_L4KA
        xc_evtchn_notify(xce_handle, ioreq_local_port[send_vcpu]);
#endif /* CONFIG_L4KA */
    }
}

int main_loop(void)
{
    extern int vm_running;
    extern int shutdown_requested;
    extern int suspend_requested;
    CPUState *env = cpu_single_env;

#ifndef CONFIG_L4KA
    int evtchn_fd = xce_handle == -1 ? -1 : xc_evtchn_fd(xce_handle);
    char qemu_file[PATH_MAX];
    fd_set fds;

    if (evtchn_fd != -1)
        qemu_set_fd_handler(evtchn_fd, cpu_handle_ioreq, NULL, env);

    xenstore_record_dm_state("running");
#endif /* CONFIG_L4KA */
    while (1) {
        while (!(vm_running && suspend_requested))
	{
            /* Wait up to 10 msec. */
            main_loop_wait(10);
	}

        fprintf(logfile,"device model saving state\n");

        /* Pull all outstanding ioreqs through the system */
        main_loop_wait(1); /* For the select() on events */

#ifndef CONFIG_L4KA
        /* Save the device state */
        snprintf(qemu_file, sizeof(qemu_file), 
                 "/var/lib/xen/qemu-save.%d", domid);
        do_savevm(qemu_file);
        xenstore_record_dm_state("paused");

        /* Wait to be allowed to continue */
        while (suspend_requested) {
            FD_ZERO(&fds);
            FD_SET(xenstore_fd(), &fds);
            if (select(xenstore_fd() + 1, &fds, NULL, NULL, NULL) > 0)
                xenstore_process_event(NULL);
        }

        xenstore_record_dm_state("running");
#endif /* CONFIG_L4KA */
    }

    return 0;
}



void destroy_hvm_domain(void)
{
#ifdef CONFIG_L4KA
    fprintf(logfile,"Shutdown Requested. Exit qemu-l4ka\n");
    exit(0);
#else
    int xcHandle;
    int sts;
 
    xcHandle = xc_interface_open();
    if (xcHandle < 0)
        fprintf(logfile,"Cannot acquire xenctrl handle\n");
    else {
        sts = xc_domain_shutdown(xcHandle, domid, SHUTDOWN_poweroff);
        if (sts != 0)
            fprintf(logfile,"? xc_domain_shutdown failed to issue poweroff, "
                    "sts %d, errno %d\n", sts, errno);
        else
            fprintf(logfile,"Issued domain %d poweroff\n", domid);
        xc_interface_close(xcHandle);
    }
#endif /* CONFIG_L4KA with else */

}


