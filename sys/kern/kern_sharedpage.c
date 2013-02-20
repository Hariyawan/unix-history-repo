/*-
 * Copyright (c) 2010, 2012 Konstantin Belousov <kib@FreeBSD.org>
 * All rights reserved.
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
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include "opt_compat.h"
#include "opt_vm.h"

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/rwlock.h>
#include <sys/sysent.h>
#include <sys/sysctl.h>
#include <sys/vdso.h>

#include <vm/vm.h>
#include <vm/vm_param.h>
#include <vm/pmap.h>
#include <vm/vm_extern.h>
#include <vm/vm_kern.h>
#include <vm/vm_map.h>
#include <vm/vm_object.h>
#include <vm/vm_page.h>
#include <vm/vm_pager.h>

static struct sx shared_page_alloc_sx;
static vm_object_t shared_page_obj;
static int shared_page_free;
char *shared_page_mapping;

void
shared_page_write(int base, int size, const void *data)
{

	bcopy(data, shared_page_mapping + base, size);
}

static int
shared_page_alloc_locked(int size, int align)
{
	int res;

	res = roundup(shared_page_free, align);
	if (res + size >= IDX_TO_OFF(shared_page_obj->size))
		res = -1;
	else
		shared_page_free = res + size;
	return (res);
}

int
shared_page_alloc(int size, int align)
{
	int res;

	sx_xlock(&shared_page_alloc_sx);
	res = shared_page_alloc_locked(size, align);
	sx_xunlock(&shared_page_alloc_sx);
	return (res);
}

int
shared_page_fill(int size, int align, const void *data)
{
	int res;

	sx_xlock(&shared_page_alloc_sx);
	res = shared_page_alloc_locked(size, align);
	if (res != -1)
		shared_page_write(res, size, data);
	sx_xunlock(&shared_page_alloc_sx);
	return (res);
}

static void
shared_page_init(void *dummy __unused)
{
	vm_page_t m;
	vm_offset_t addr;

	sx_init(&shared_page_alloc_sx, "shpsx");
	shared_page_obj = vm_pager_allocate(OBJT_PHYS, 0, PAGE_SIZE,
	    VM_PROT_DEFAULT, 0, NULL);
	VM_OBJECT_LOCK(shared_page_obj);
	m = vm_page_grab(shared_page_obj, 0, VM_ALLOC_RETRY | VM_ALLOC_NOBUSY |
	    VM_ALLOC_ZERO);
	m->valid = VM_PAGE_BITS_ALL;
	VM_OBJECT_UNLOCK(shared_page_obj);
	addr = kmem_alloc_nofault(kernel_map, PAGE_SIZE);
	pmap_qenter(addr, &m, 1);
	shared_page_mapping = (char *)addr;
}

SYSINIT(shp, SI_SUB_EXEC, SI_ORDER_FIRST, (sysinit_cfunc_t)shared_page_init,
    NULL);

static void
timehands_update(struct sysentvec *sv)
{
	struct vdso_timehands th;
	struct vdso_timekeep *tk;
	uint32_t enabled, idx;

	enabled = tc_fill_vdso_timehands(&th);
	tk = (struct vdso_timekeep *)(shared_page_mapping +
	    sv->sv_timekeep_off);
	idx = sv->sv_timekeep_curr;
	atomic_store_rel_32(&tk->tk_th[idx].th_gen, 0);
	if (++idx >= VDSO_TH_NUM)
		idx = 0;
	sv->sv_timekeep_curr = idx;
	if (++sv->sv_timekeep_gen == 0)
		sv->sv_timekeep_gen = 1;
	th.th_gen = 0;
	if (enabled)
		tk->tk_th[idx] = th;
	tk->tk_enabled = enabled;
	atomic_store_rel_32(&tk->tk_th[idx].th_gen, sv->sv_timekeep_gen);
	tk->tk_current = idx;
}

#ifdef COMPAT_FREEBSD32
static void
timehands_update32(struct sysentvec *sv)
{
	struct vdso_timekeep32 *tk;
	struct vdso_timehands32 th;
	uint32_t enabled, idx;

	enabled = tc_fill_vdso_timehands32(&th);
	tk = (struct vdso_timekeep32 *)(shared_page_mapping +
	    sv->sv_timekeep_off);
	idx = sv->sv_timekeep_curr;
	atomic_store_rel_32(&tk->tk_th[idx].th_gen, 0);
	if (++idx >= VDSO_TH_NUM)
		idx = 0;
	sv->sv_timekeep_curr = idx;
	if (++sv->sv_timekeep_gen == 0)
		sv->sv_timekeep_gen = 1;
	th.th_gen = 0;
	if (enabled)
		tk->tk_th[idx] = th;
	tk->tk_enabled = enabled;
	atomic_store_rel_32(&tk->tk_th[idx].th_gen, sv->sv_timekeep_gen);
	tk->tk_current = idx;
}
#endif

/*
 * This is hackish, but easiest way to avoid creating list structures
 * that needs to be iterated over from the hardclock interrupt
 * context.
 */
static struct sysentvec *host_sysentvec;
#ifdef COMPAT_FREEBSD32
static struct sysentvec *compat32_sysentvec;
#endif

void
timekeep_push_vdso(void)
{

	if (host_sysentvec != NULL && host_sysentvec->sv_timekeep_base != 0)
		timehands_update(host_sysentvec);
#ifdef COMPAT_FREEBSD32
	if (compat32_sysentvec != NULL &&
	    compat32_sysentvec->sv_timekeep_base != 0)
		timehands_update32(compat32_sysentvec);
#endif
}

void
exec_sysvec_init(void *param)
{
	struct sysentvec *sv;
	int tk_base;
	uint32_t tk_ver;

	sv = (struct sysentvec *)param;

	if ((sv->sv_flags & SV_SHP) == 0)
		return;
	sv->sv_shared_page_obj = shared_page_obj;
	sv->sv_sigcode_base = sv->sv_shared_page_base +
	    shared_page_fill(*(sv->sv_szsigcode), 16, sv->sv_sigcode);
	if ((sv->sv_flags & SV_ABI_MASK) != SV_ABI_FREEBSD)
		return;
	tk_ver = VDSO_TK_VER_CURR;
#ifdef COMPAT_FREEBSD32
	if ((sv->sv_flags & SV_ILP32) != 0) {
		tk_base = shared_page_alloc(sizeof(struct vdso_timekeep32) +
		    sizeof(struct vdso_timehands32) * VDSO_TH_NUM, 16);
		KASSERT(tk_base != -1, ("tk_base -1 for 32bit"));
		shared_page_write(tk_base + offsetof(struct vdso_timekeep32,
		    tk_ver), sizeof(uint32_t), &tk_ver);
		KASSERT(compat32_sysentvec == 0,
		    ("Native compat32 already registered"));
		compat32_sysentvec = sv;
	} else {
#endif
		tk_base = shared_page_alloc(sizeof(struct vdso_timekeep) +
		    sizeof(struct vdso_timehands) * VDSO_TH_NUM, 16);
		KASSERT(tk_base != -1, ("tk_base -1 for native"));
		shared_page_write(tk_base + offsetof(struct vdso_timekeep,
		    tk_ver), sizeof(uint32_t), &tk_ver);
		KASSERT(host_sysentvec == 0, ("Native already registered"));
		host_sysentvec = sv;
#ifdef COMPAT_FREEBSD32
	}
#endif
	sv->sv_timekeep_base = sv->sv_shared_page_base + tk_base;
	sv->sv_timekeep_off = tk_base;
	timekeep_push_vdso();
}
