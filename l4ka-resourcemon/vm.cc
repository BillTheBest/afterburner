/*********************************************************************
 *
 * Copyright (C) 2003-2004,  Karlsruhe University
 *
 * File path:     vm.cc
 * Description:   The virtual machine abstraction.
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
 ********************************************************************/

#include <l4/types.h>
#include <l4/sigma0.h>
#include <l4/schedule.h>
#include <l4/arch.h>

#include <vm.h>
#include <module_manager.h>
#include <elf.h>
#include <basics.h>
#include <debug.h>
#include <string.h>
#include <schedule.h>
#include <earm.h>


L4_Word_t tid_space_t::base_tid;

vm_t * vm_allocator_t::allocate_vm()
{
    // Return the first available vm, and change its status to allocated.
    for( L4_Word_t i = 0; i < MAX_VM; i++ )
	if( this->vm_list[i].allocated == false )
	{
	    vm_t *vm = &this->vm_list[i];
	    vm->allocated = true;
	    return vm;
	}

    return NULL;
}

void vm_allocator_t::init()
{
   
    // Init all of the vm instances.
    for( L4_Word_t i = 0; i < MAX_VM; i++ )
	this->vm_list[i].init( i );

    // Reserve the first vm for the resourcemon.
    vm_t *vm = this->get_resourcemon_vm();
    vm->allocated = true;
    vm->init_mm( get_resourcemon_end_addr(), MB(0), false, 0, 0 );
}

void vm_t::init( L4_Word_t new_space_id )
{
    this->allocated = false;
    this->device_access_enabled = false;
    this->space_id = new_space_id;
    this->haddr_base = 0;
    this->paddr_len = 0;
    this->vaddr_offset = 0;
    this->vcpu_count = 1;
    this->pcpu_count = 1;
    this->prio = 102;
    //this->client_shared = (IResourcemon_shared_t *) this->client_shared_remap_area;
}

bool vm_t::init_mm( L4_Word_t size, L4_Word_t new_vaddr_offset, bool shadow_special, 
		    L4_Word_t init_wedge_size, L4_Word_t init_wedge_paddr )
{
    size = round_up( size, SUPER_PAGE_SIZE );

    // Look for a block of contiguous memory to hold our VM, unless
    // it is for the resourcemon (space_id == 0), and then take the requested
    // size at addr 0.
    L4_Word_t start_addr = 0, end_addr;
    bool found = (get_space_id() == 0);
    while( !found )
    {
	found = true;
	end_addr = start_addr + size;

	// Look for conflicts with other virtual machines.
	for( L4_Word_t id = 0; id < MAX_VM; id++ )
	{
	    vm_t *vm = get_vm_allocator()->space_id_to_vm(id);
	    if( vm->get_space_size() == 0 )
		continue;
	    if( (end_addr <= vm->get_haddr_base()) ||
		    (start_addr >= (vm->get_haddr_base() + vm->get_space_size())) )
		continue;
	    // Start looking in the memory following this vm.
	    found = false;
	    start_addr = vm->get_haddr_base() + vm->get_space_size();
	    start_addr = round_up( start_addr, MB(4) );
	    break;
	}

	// Look for conflicts with KIP memory descriptors.
	if( found )
	{
	    L4_Word_t next;
	    while( kip_conflict(start_addr, size, &next) )
	    {
		start_addr = round_up( next, MB(4) );
		found = false;		// validate against the VM list.
		if( start_addr == 0 )
		    return false;	// wrap-around
	    }
	}
    }

    if( !found )
	return false; // Unable to find a contiguous block of memory.

    if( (start_addr + size - 1) >= get_max_phys_addr() )
	return false; // Not enough memory.

    this->paddr_len = size;
    this->vaddr_offset = new_vaddr_offset;
    this->haddr_base = start_addr;

    this->wedge_paddr = init_wedge_paddr;
    this->wedge_vaddr = 0;

    printf("Creating VM id %d at %x size %d MBytes\n",
	    this->get_space_id(), this->haddr_base, this->paddr_len / (1024 * 1024));
    dprintf(debug_startup,  "\tThread space: first TID %t, number of threads: %d\n",
	    this->get_first_tid(), tid_space_t::get_tid_space_size());

    if( shadow_special )
    {
	this->shadow_special_memory();
    }
    

    return true;
}

void vm_t::shadow_special_memory()
{
    L4_Word_t tot = 0;

    memset( (void *)this->get_haddr_base(), 0, this->get_space_size() );
    return;

    // If we execute this copy after the resourcemon has grabbed all physical
    // memory, then the destination areas can still use large page
    // sizes.
    for( L4_Word_t page = 0; page < SPECIAL_MEM; page += PAGE_SIZE )
    {
	L4_Word_t haddr;
	L4_Fpage_t req_fp = L4_FpageLog2( page, PAGE_BITS );
	L4_Fpage_t rcv_fp = L4_Sigma0_GetPage( L4_Pager(), req_fp, req_fp );
	if( !client_paddr_to_haddr(page, &haddr) )
	    continue;

	if( L4_IsNilFpage(rcv_fp) )
	    memset( (void *)haddr, 0, PAGE_SIZE );
	else
	{
	    memcpy( (void *)haddr, (void *)page, PAGE_SIZE );
	    tot++;
	}
    }

    if( tot == 0 )
    {
	dprintf(debug_startup,  "Error: something broken, no special platform memory!\n");
#if defined(__i386__)
	L4_KDB_Enter( "notice me" );
#endif
    }
}

bool vm_t::client_vaddr_to_haddr( L4_Word_t vaddr, L4_Word_t *haddr )
{
    if( wedge_paddr && (vaddr >= wedge_vaddr) )
    {
	return this->client_paddr_to_haddr( vaddr - wedge_vaddr + wedge_paddr, haddr );
    }
    
    return this->client_paddr_to_haddr( vaddr - this->vaddr_offset, haddr );
}

bool vm_t::client_vaddr_to_paddr( L4_Word_t vaddr, L4_Word_t *paddr )
{
    if( vaddr >= this->vaddr_offset )
    {
	*paddr = vaddr - this->vaddr_offset;
	return true;
    }
    return false;
}

bool vm_t::elf_section_describe( 
	L4_Word_t file_start, 
	const char *search_name, 
	L4_Word_t *addr,
	L4_Word_t *size )
{
    elf_ehdr_t *ehdr = (elf_ehdr_t *)file_start;
    elf_shdr_t *shdr, *shdr_list, *str_hdr;
    L4_Word_t s_cnt;
    char *section_name;

    shdr_list = (elf_shdr_t *)(file_start + ehdr->shoff);
    str_hdr = &shdr_list[ ehdr->shstrndx ];
    for( s_cnt = 0; s_cnt < ehdr->shnum; s_cnt++ )
    {
	shdr = &shdr_list[ s_cnt ];
	section_name = (char *)
	    (shdr->name + file_start + str_hdr->offset);

	if( !strcmp(section_name, search_name) )
	{
	    *addr = shdr->addr;
	    *size = shdr->size;
	    return true;
	}
    }

    return false;
}

/**
 * ELF-load an ELF memory image
 *
 * @param file_start    First byte of source ELF image in memory
 * @param memory_start  Pointer to address of first byte of loaded image
 * @param memory_end    Pointer to address of first byte behind loaded image
 * @param entry         Pointer to address of entry point
 *
 * This function ELF-loads an ELF image that is located in memory.  It
 * interprets the program header table and copies all segments marked
 * as load. It stores the lowest and highest+1 address used by the
 * loaded image as well as the entry point into caller-provided memory
 * locations.
 *
 * @returns false if ELF loading failed, true otherwise.
 */
bool vm_t::elf_load( L4_Word_t file_start )
{
    // Pointer to ELF file header
    elf_ehdr_t* eh = (elf_ehdr_t*) file_start;

    if( !elf_is_valid(file_start) || (eh->type != 2) )
    {
	printf( "Error: invalid ELF binary.\n");
	return false;
    }

    // Is the address of the PHDR table valid?
    if (eh->phoff == 0)
    {
        // No. Bail out
	printf( "Error: ELF binary has wrong PHDR table offset.\n");
        return false;
    }

    if( wedge_paddr ) 
    {
	wedge_vaddr = eh->entry - (eh->entry & (MB(64)-1));
	dprintf(debug_startup,  "\tWedge virt offset %x phys offset %x\n", wedge_vaddr, wedge_paddr);
    }

    dprintf(debug_startup,  "\tELF entry virtual address: %x\n", eh->entry);

    // Locals to find the enclosing memory range of the loaded file
    L4_Word_t max_addr =  0UL;
    L4_Word_t min_addr = ~0UL;

    // Walk the program header table
    for (L4_Word_t i = 0; i < eh->phnum; i++)
    {
        // Locate the entry in the program header table
        elf_phdr_t* ph = (elf_phdr_t*) 
	    (file_start + eh->phoff + eh->phentsize * i);
        
        // Is it to be loaded?
        if (ph->type == PT_LOAD)
        {
	    L4_Word_t haddr, haddr2;
	    if( !client_vaddr_to_haddr(ph->vaddr, &haddr) ||
		    !client_vaddr_to_haddr(ph->vaddr+ph->msize-1, &haddr2) )
	    {
		printf( "Error: ELF file doesn't fit!\n");
		return false;
	    }
	    dprintf(debug_startup,  "\t  Source %x size %08d --> resourcemon address %x, VM address %x\n",
		    (file_start + ph->offset), ph->fsize, haddr, ph->vaddr);
	    
            // Copy bytes from "file" to memory - load address
            memcpy((void *)haddr,
		    (void *)(file_start + ph->offset), ph->fsize);
	    // Zero "non-file" bytes
	    memset ((void *)(haddr + ph->fsize), 
		    0, ph->msize - ph->fsize);
            // Update min and max
            min_addr = min(min_addr, ph->vaddr);
            max_addr = max(max_addr, ph->vaddr + max(ph->msize, ph->fsize));
        }
        
    }
    // Write back the values into the caller-provided locations
    this->binary_start_vaddr = min_addr;
    this->binary_end_vaddr = max_addr;
    this->binary_entry_vaddr = this->elf_entry_vaddr = eh->entry;
    this->binary_stack_vaddr = 0;

    // Signal successful loading
    return true;
}

bool vm_t::install_memory_regions(vm_t *source_vm)
{
    // install the KIP
    // kip_start and size are originally stored in the ELF image,
    // just move them over from the source
    L4_Fpage_t source_kip_fp = source_vm->get_kip_fp();
    L4_Word_t kip_start = L4_Address(source_kip_fp);
    L4_Word_t kip_size = L4_Size(source_kip_fp);
    dprintf(debug_startup,  " KIP start at %x, size %d, end %x, binary VM vaddr start %x\n ",
	    kip_start, kip_start, (kip_start + kip_size), this->binary_start_vaddr);

    this->kip_fp = L4_Fpage(kip_start, kip_size);
    if (L4_Address(this->kip_fp) != kip_start)
    {
        printf( "Error: the KIP is misaligned\n");
        return false;
    }
    // install the UTCB
    // utcb_start and utcb_size are originally stored in the ELF image
    L4_Fpage_t source_utcb_fp = source_vm->get_utcb_fp();
    L4_Word_t utcb_start = L4_Address(source_utcb_fp);
    L4_Word_t utcb_size = L4_Size(source_utcb_fp);
    this->utcb_fp = L4_Fpage(utcb_start, utcb_size);
    if (L4_Address(this->utcb_fp) != utcb_start)
    {
        printf( "Error: UTCB is misaligned\n");
        return false;
    }

    this->wedge_vaddr = source_vm->get_wedge_vaddr();
    this->binary_stack_vaddr = source_vm->get_binary_stack_vaddr();
    this->elf_entry_vaddr = this->binary_entry_vaddr;

    // client shared page vaddr from ELF
    this->client_shared_vaddr = source_vm->get_client_shared_vaddr();
    bool result = client_vaddr_to_haddr(this->client_shared_vaddr,
					(L4_Word_t *) &this->client_shared);
    if (!result)
    {
	printf( "Error: client shared does not fit within VM\n");
	return false;
    }

    return true;
}

bool vm_t::install_elf_binary( L4_Word_t elf_start )
{
    // Install the elf binary.
    if( !elf_load(elf_start) )
	return false;

    /* Figure out where to install the kip.
     */
    L4_Word_t kip_start, kip_size;
    if( !elf_section_describe(elf_start, ".l4kip", &kip_start, &kip_size) )
    {
	kip_start = KIP_LOCATION;
	kip_size = 1 << KIP_SIZE_BITS;
	if( (kip_start + kip_size) > this->binary_start_vaddr )
	{
	    printf( "Error: unable to find a decent KIP location.\n");
	    printf( " KIP start at %x, size %d, end %x, binary VM vaddr start %x\n ",
		    kip_start, kip_start, (kip_start + kip_size), this->binary_start_vaddr);
	    return false;
	}
    }

    this->kip_fp = L4_Fpage(kip_start, kip_size);

    if( L4_Address(this->kip_fp) != kip_start )
    {
	printf( "Error: the KIP area is misaligned, aborting.\n");
	return false;
    }

    /* Figure out where to install the utcb.
     */
    L4_Word_t utcb_start, utcb_size;
    if( !elf_section_describe(elf_start, ".l4utcb", &utcb_start, &utcb_size) )
    {
	utcb_start = UTCB_LOCATION;
	utcb_size = 1UL << UTCB_SIZE_BITS;
	if( (utcb_start + utcb_size) > this->binary_start_vaddr )
	{
	    printf( "Error: unable to find a decent UTCB location.\n");
	    return false;
	}
    }

    this->utcb_fp = L4_Fpage(utcb_start, utcb_size);

    if( L4_Address(this->utcb_fp) != utcb_start )
    {
	printf( "Error: the UTCB section is misaligned, aborting.\n");
	printf( "  UTCB start: %x size: %d\n", utcb_start, utcb_size);
	return false;
    }

    /* Locate the client shared data structure.
     */
    L4_Word_t shared_start, shared_size;
    if( elf_section_describe(elf_start, ".resourcemon", &shared_start, &shared_size) )
    {
	if( shared_size < sizeof(IResourcemon_shared_t) )
	{
	    printf( "Error: the .resourcemon ELF section is too small; please "
		"ensure that the binary is compatible with this resourcemon.\n");
	    return false;
	}

	L4_Word_t end_addr;
	bool result = client_vaddr_to_haddr( shared_start, (L4_Word_t *) &this->client_shared);
	result |= client_vaddr_to_haddr( (shared_start + shared_size - 1), &end_addr );
	if( !result )
	{
	    printf( "Error: the .resourcemon elf section doesn't fit within"
		    " allocated memory for the virtual machine, aborting.\n");
	    return false;
	}
	
	if (shared_size != sizeof(IResourcemon_shared_t))
	{
	    printf( "Error: the .resourcemon elf section size is wrong\n");
	    return false;
	}
	

	dprintf(debug_startup,  "\tResourcemon shared page at VM address %x size %d remap to %x\n",
		shared_start, shared_size, client_shared);


    }
    else
	this->client_shared = NULL;
    
	
    /* Look for an alternate start address.
     */
    L4_Word_t alternate_start, alternate_size;
    if( elf_section_describe(elf_start, ".resourcemon.startup", &alternate_start, &alternate_size) )
    {
	if( alternate_size < sizeof(IResourcemon_startup_config_t) ) {
	    printf( "Error: version mismatch for the .resourcemon.startup "
		    "ELF section.\nPlease ensure that the binary is compatible "
		    "with this version of the resourcemon.\n");
	    return false;
	}

	IResourcemon_startup_config_t *alternate;
	L4_Word_t end_addr;
	bool result = client_vaddr_to_haddr( alternate_start,
		(L4_Word_t *)&alternate );
	result |= client_vaddr_to_haddr( (alternate_start + alternate_size - 1),
		&end_addr );
	if( !result )
	{
	    printf( "Error: the .resourcemon.startup ELF section doesn't fit "
		    "within allocated memory for the virtual machine, "
		    "aborting.\n");
	    return false;
	}

	this->binary_entry_vaddr = alternate->start_ip;
	this->binary_stack_vaddr = alternate->start_sp;

	dprintf(debug_startup,  "\tEntry override, IP %x, SP %x\n",
		this->binary_entry_vaddr, this->binary_stack_vaddr);
    }

    return true;
}

//
// copy the shared memory data from the source vm_t object to this vm_t object
//
void vm_t::copy_client_shared(vm_t *source_vm)
{
    memcpy((void *)this->client_shared, (void *)source_vm->get_client_shared(),
	   this->client_shared_size);
    dprintf(debug_startup,  "copy client shared from "
	, source_vm->get_client_shared()
	," to ", this->client_shared
	," size ", this->client_shared_size
	,"\n");
}

bool vm_t::init_client_shared( const char *cmdline )
{
    if( !client_shared )
    {
	printf( "Error: unable to locate the VM's shared resourcemon page.\n");
	return false;
    }

    if( client_shared->version != IResourcemon_version )
    {
	printf( "Error: the binary uses a different version of the "
	        "resourcemons's IDL file:\n\tresourcemon's version: %dm binary's version: %d\n",
		IResourcemon_version ,client_shared->version);
	return false;
    }
    if( client_shared->cpu_cnt > IResourcemon_max_cpus )
    {
	printf( "Error: the binary expects %d CPUS, but the resourcemon's IDL file supports %d ",
		client_shared->cpu_cnt, IResourcemon_max_cpus);
	return false;
    }
    
    if( pcpu_count > IResourcemon_max_cpus )
    {
	printf( "Error: ",pcpu_count," L4 CPUs exceeds the "
		,IResourcemon_max_cpus," CPUs supported by the resourcemon.\n");
	return false;
    }
    

    client_shared->thread_server_tid = L4_Myself();
    client_shared->locator_tid = L4_Myself();
    client_shared->resourcemon_tid = L4_Myself();

    for( L4_Word_t cpu = 0; cpu < pcpu_count; cpu++ )
	client_shared->cpu[cpu].time_balloon = 0;

    client_shared->vcpu_count = this->vcpu_count;
    client_shared->pcpu_count = this->pcpu_count;
    
    client_shared->module_count = 0;

    client_shared->thread_space_start =
	tid_space_t::get_tid_start( this->get_space_id() );
    client_shared->thread_space_len =
	tid_space_t::get_tid_space_size();

    client_shared->utcb_fpage = this->get_utcb_fp();
    client_shared->kip_fpage  = this->get_kip_fp();

    if (l4_iommu_enabled)
	client_shared->phys_offset = 0;
    else
	client_shared->phys_offset = this->get_haddr_base();

    client_shared->phys_size = this->get_space_size();
    
    if( this->has_client_dma_access() )
	client_shared->phys_end = get_max_phys_addr();
    else
        client_shared->phys_end = this->get_space_size() - 1;

    

    client_shared->link_vaddr = this->vaddr_offset;
    client_shared->entry_ip = this->elf_entry_vaddr;
    client_shared->entry_sp = 0;

    client_shared->prio = this->get_prio();
    client_shared->mem_balloon = 0;

    client_shared->wedge_virt_offset = this->wedge_vaddr;
    client_shared->wedge_phys_offset = this->wedge_paddr;

    
    client_shared->max_logid_in_use = max_logid_in_use;
    for( L4_Word_t cpu = 0; cpu < IResourcemon_max_cpus; cpu++ )
    {

	for( L4_Word_t p = 0; p < ILogging_max_load_parms; p++ )
	{
	    this->get_load_parm(cpu, p)->threshold = 0;
	    this->get_load_parm(cpu, p)->logfile_selector = 0;
	    this->get_load_parm(cpu, p)->initial_threshold = 0;
	    this->get_load_parm(cpu, p)->padding = 0;
	}

	for( L4_Word_t d = 0; d < ILogging_max_logids; d++ )
	{
	    for (L4_Word_t r = 0; r < ILogging_max_resources; r++)
	    {
		this->get_resource(cpu, d, r)->threshold = 0;
		this->get_resource(cpu, d, r)->logfile_selector = 0;
		this->get_resource(cpu, d, r)->initial_threshold = 0;
		this->get_resource(cpu, d, r)->padding = 0;
	    }
	}
	
	for( L4_Word_t i = 0; i < ILogging_log_buffer_size; i++ )
	    this->get_log_buffer(cpu)[i] = 0;
    }
    

    if( cmdline )
    {
	unsigned cmdlinelen = strlen(cmdline);
	if( (cmdlinelen) >= sizeof(client_shared->cmdline) )
	{
	    printf( "Error: the command line for the VM is too long (%d / %d).\n", cmdlinelen, sizeof(client_shared->cmdline));
	    return false;
	}
	if (!install_ipcmdline(cmdline, client_shared->cmdline))
	    return false;
    }

    else
	*client_shared->cmdline = '\0';
    
   
    L4_Word_t d=0;

    if (this->has_client_dma_access())
    {
	// Declare all of machine memory, so that it has a representation in
	// the page map, but reserved.
	
	// Below phys_offset 
#if 0
	if (client_shared->phys_offset > 0x100000)
	{
	    client_shared->devices[d].low = 0x100000;
	    client_shared->devices[d].high = client_shared->phys_offset - 1;
	    client_shared->devices[d].type = L4_ReservedMemoryType;
	    dprintf(debug_startup, "\tregistering dmamem %08x-%08x type %d\n", client_shared->devices[d].low,
		   client_shared->devices[d].high, client_shared->devices[d].type);
	    d++;
	}
#endif	
	// Above phys_offset + phys_size
	if (client_shared->phys_offset + client_shared->phys_size < client_shared->phys_end)
	{
	    client_shared->devices[d].low = client_shared->phys_offset + client_shared->phys_size;
	    client_shared->devices[d].high = client_shared->phys_end;
	    client_shared->devices[d].type = L4_ReservedMemoryType;
	    dprintf(debug_startup, "\tregistering dmamem %08x-%08x type %d\n", client_shared->devices[d].low,
		   client_shared->devices[d].high, client_shared->devices[d].type);
	    d++;
	}
    }
    
    for( L4_Word_t i = 0; i < L4_NumMemoryDescriptors(l4_kip); i++)
    {
	L4_MemoryDesc_t *mdesc = L4_MemoryDesc(l4_kip, i);
	
	if (((L4_Type(mdesc) & 0xF) == L4_ArchitectureSpecificMemoryType) &&
	    this->has_device_access())
	{
	    if (d >= IResourcemon_max_devices)
	    {
		printf("Could not register all device memory regions cur %x max %d", 
		       d, IResourcemon_max_devices);
		break;
	    }

	    client_shared->devices[d].low = L4_Low(mdesc);
	    client_shared->devices[d].high = L4_High(mdesc);
	    client_shared->devices[d].type = L4_Type(mdesc);
	    dprintf(debug_startup, "\tregistering devmem %08x-%08x type %d\n", client_shared->devices[d].low,
		    client_shared->devices[d].high, client_shared->devices[d].type);
	    d++;
	}
	else if (((L4_Type(mdesc) & 0xF) == L4_SharedMemoryType) &&
		 (L4_Low(mdesc) == 0xa0000 && L4_High(mdesc) == 0xbffff))
	{
	    // Give passthrough access to VGA memory to everybody for now
	    if (d >= IResourcemon_max_devices)
	    {
		printf("Could not register all device memory regions cur %x max %d", 
		       d, IResourcemon_max_devices);
		break;
	    }

	    client_shared->devices[d].low = L4_Low(mdesc);
	    client_shared->devices[d].high = L4_High(mdesc);
	    client_shared->devices[d].type = L4_Type(mdesc);
	    dprintf(debug_startup, "\tregistering shared mem %08x-%08x type %d\n", client_shared->devices[d].low,
		    client_shared->devices[d].high, client_shared->devices[d].type);
	    d++;
	}

    }	
	
	
    if (d < IResourcemon_max_devices)
    {
	client_shared->devices[d].low = get_max_phys_addr();
	client_shared->devices[d].high = 0xffffffff;
    }
    
    return true;
}

bool vm_t::start_vm()
{
    L4_ThreadId_t tid, scheduler, pager;
    L4_Word_t result;
    tid = this->get_first_tid();
    scheduler = this->get_scheduler_tid(); 
    
    pager = L4_Myself();
    dprintf(debug_startup,  "\tVM %d\t   KIP at %x, size %d", 
	    get_space_id(), L4_Address(this->kip_fp), L4_Size(this->kip_fp));
    dprintf(debug_startup,  "\t  UTCB at %x, size %d\n",
	    L4_Address(this->utcb_fp), L4_Size(this->utcb_fp));
    dprintf(debug_startup,  "\t  Scheduler TID: %t",scheduler);
    dprintf(debug_startup,  "\tTID: %t\n", tid);

    result = L4_ThreadControl(tid, tid, L4_Myself(), L4_nilthread, (void *)L4_Address(utcb_fp));
    
    if (!result)
    {
	printf( "Error: failure creating first thread, TID %t, scheduler TID %t L4 error code\n",
		tid, scheduler, L4_ErrorCode());
	return false;
    }
    L4_Word_t dummy, control = 0;

    if (this->has_device_access() && l4_iommu_enabled)
    {
	static int num;
	if (num++ == 0){
	    control = (1<<30 | L4_TimePeriod(10 * 1000).raw);
	}
	else {
	    control = (1<<30 | L4_TimePeriod(10 * 1000).raw);
	}
    }
    
    result = L4_SpaceControl(tid, control, this->kip_fp, this->utcb_fp, L4_nilthread, &dummy);
    if (!result)
    {
	printf( "Error: failure creating space, TID %t, L4 error code %d\n", 
		tid, L4_ErrorCode());
	goto err_space_control;
    }

   // Priority, domain, etc
    // Set the thread's priority.
    if( !L4_Set_Priority(tid, this->get_prio()) )
    {
        printf( "Error: unable to set a thread's prio to %x, L4 error: ", this->get_prio(), L4_ErrorCode());
	goto err_priority;
    }
    
    // Set the thread's logid.
    if (l4_logging_enabled)
    {
        L4_Word_t logid = space_id + VM_LOGID_OFFSET;
        if( !L4_Set_Logid(tid, logid) )
        {
            printf("Error: failure setting VM's logid to %d TID %t, errcode %d\n",
                   logid, tid, L4_ErrorCode());
            goto err_priority;
        }
        set_max_logid_in_use(logid);
    }

    
    if (l4_pmsched_enabled)
    {
        // Set the thread's timeslice to never
        if( !L4_Set_Timeslice(tid, L4_Never, L4_Never) )
        {
            printf( "Error: failure setting VM's timeslice to %x, TID %t, L4 error code %d\n", 
                    L4_Never.raw, tid, L4_ErrorCode());
            goto err_priority;
        }
        schedule_setup_thread_faults(tid);
    }

    // Make the thread valid.
    result = L4_ThreadControl(tid, tid, scheduler, pager, (void *)-1UL);
    
    if(!result )
    {
	printf( "Error: failure starting thread, TID %t, L4 error code %d\n", 
		tid, L4_ErrorCode());
	goto err_valid;
    }

    if (l4_hsched_enabled)
    {
	L4_Word_t sched_control = 1, result = 0;
    
	//New Subqueue below me
 	printf("\t  HSCHED: scheduling subqueue below %t for %t\n", L4_Myself(), tid);
	result = L4_HS_Schedule(tid, sched_control, L4_Myself(), 98, 100, &sched_control);
	
        if (!result)
        {
            printf("Error: failure creating scheduling subqueue for VM, TID %t result %d errcode %d\n",
                   tid, result, L4_ErrorCode());
            goto err_valid;
        }	
        
	//Restride
	L4_Word_t stride = INIT_CPU_STRIDE;
	sched_control = 16;
        
	printf("\t  HSCHED: Set stride for subqeue to %d\n", stride);
	result = L4_HS_Schedule(tid, sched_control, tid, 0, stride, &sched_control);
        if (!result)
        {
            printf("Error: failure setting stride for subqueue, TID %t result %d errcode %d\n",
                   tid, result, L4_ErrorCode());
            goto err_valid;
        }	
        
    }

    // Enable smallspaces for device drivers.
    if( l4_smallspaces_enabled )
    {
	L4_Word_t control;
	if( this->has_device_access() )
	    control = (1 << 31) | L4_SmallSpace(0,64);
	else
	    control = (1 << 31) | L4_SmallSpace(1,256);
	if( !L4_SpaceControl(tid, control, L4_Nilpage, L4_Nilpage, L4_nilthread, &dummy) )
	    printf( "Warning: unable to activate a smallspace.\n");
	else
	    printf( "\t  Small space establishing.\n");
	L4_KDB_Enter("small");
    }

    if (!this->activate())
        goto err_activate;

    return true;

err_priority:
err_space_control:
err_valid:
err_activate:
    // Delete the thread and space.
    L4_ThreadControl(tid, L4_nilthread, L4_nilthread, L4_nilthread, (void *)-1UL);
    return false;
}

bool vm_t::activate_thread()
{
    //L4_KDB_Enter( "starting VM" );

    L4_Msg_t msg;
    L4_Clear( &msg );
    L4_Append( &msg, this->binary_entry_vaddr );		// ip
    L4_Append( &msg, this->binary_stack_vaddr );		// sp
    L4_Load( &msg );


    L4_MsgTag_t tag = L4_Reply( this->get_first_tid() );
    return L4_IpcSucceeded(tag);
}


bool vm_t::install_ramdisk( L4_Word_t rd_start, L4_Word_t rd_end )
{
    // Too big?
    L4_Word_t size = rd_end - rd_start;
    if( size > this->get_space_size() )
	return false;

    // Too big?
    L4_Word_t target_paddr = this->get_space_size() - size;
    if( target_paddr < PAGE_SIZE )
	return false;
    target_paddr = (target_paddr - PAGE_SIZE) & PAGE_MASK;

    // Convert to virtual
    L4_Word_t target_vaddr;
    if( !client_paddr_to_vaddr(target_paddr, &target_vaddr) )
	return false;

    // Overlaps with the elf binary?
    if( is_region_conflict(target_vaddr, target_vaddr+size,
		this->binary_start_vaddr, this->binary_end_vaddr) )
	return false;

    // Overlaps with the kip or utcb?
    if( is_fpage_conflict(this->kip_fp, target_vaddr, target_vaddr+size) )
	return false;
    if( is_fpage_conflict(this->utcb_fp, target_vaddr, target_vaddr+size) )
	return false;

    // Can we translate the addresses?
    L4_Word_t haddr, haddr2;
    if( !client_paddr_to_haddr(target_paddr, &haddr) ||
	    !client_paddr_to_haddr(target_paddr + size - 1, &haddr2) )
	return false;

    printf("\tInstalling ramdisk at VM address %p size %d\n", target_vaddr, size);

    memcpy( (void *)haddr, (void *)rd_start, size );

    // Update the client shared information.
    if( this->client_shared )
    {
	this->client_shared->ramdisk_start = target_vaddr;
	this->client_shared->ramdisk_size = size;
    }

    return true;
}

bool vm_t::install_ipcmdline( const char *cmdline, char *ipcmdline )
{
    unsigned cmdlinelen = strlen(cmdline);
    char ipsubstr[64], *prefixend, *suffixstart;
    int ipsubstrlen, prefixlen, suffixlen;
	
    module_manager_t *mm = get_module_manager();
	    
    if (mm->cmdline_has_grubdhcp( cmdline, &prefixend, &suffixstart))
    {
    
	//<client-ip>:<server-ip>:<gw-ip>:<netmask>:<ovr>
	char *dst = ipsubstr;
	char *src = mm->dhcp_info.ip;
	int len = strlen(src);
	strncpy( dst, src, len);
	dst += len;
	*dst++ = ':';
	src = mm->dhcp_info.server;
	len = strlen(src);
	strncpy( dst, src, len);
	dst += len; 
	*dst++ = ':';
	src = mm->dhcp_info.gateway;
	len = strlen(src);
	strncpy( dst, src, len);
	dst += len;
	*dst++ = ':';
	src = mm->dhcp_info.mask;
	len = strlen(src);
	strncpy( dst, src, len);
	dst += len;
	*dst = 0;
	    
	dprintf(debug_startup,  "\tPatching IP info %s from grub dhcp into cmdline\n",ipsubstr);
	    
	ipsubstrlen = dst - ipsubstr;
	// leave ip= in place
	prefixend += 3;
	    
	prefixlen = prefixend - cmdline;
	suffixlen = cmdline + cmdlinelen - suffixstart;

    }
    else 
    {
	prefixlen = cmdlinelen;
	ipsubstrlen = suffixlen =  0;
    }
	
    if( (cmdlinelen + ipsubstrlen) >= sizeof(client_shared->cmdline) )
    {
	printf( "Error: the command line for the VM is too long (%d / %d).\n", cmdlinelen + ipsubstrlen, sizeof(client_shared->cmdline));
	return false;
    }

    strncpy( ipcmdline, cmdline, prefixlen);
    strncpy( ipcmdline + prefixlen, ipsubstr, ipsubstrlen); 
    strncpy( ipcmdline + prefixlen + ipsubstrlen, suffixstart, suffixlen);
	
    return true;
}

bool vm_t::install_module( L4_Word_t ceiling, L4_Word_t haddr_start, L4_Word_t haddr_end, const char *cmdline )
{
    // Too big?
    L4_Word_t size = haddr_end - haddr_start;
    if( size > ceiling ) {
	printf( "Module too big, size %d, ceiling %x ", size, ceiling);
	return false;
    }

    L4_Word_t target_paddr = (ceiling - size) & PAGE_MASK;

    // Convert to virtual
    L4_Word_t target_vaddr;
    if( !client_paddr_to_vaddr(target_paddr, &target_vaddr) )
	return false;

    // Overlaps with the elf binary?
    if( is_region_conflict(target_vaddr, target_vaddr+size,
		this->binary_start_vaddr, this->binary_end_vaddr) )
    {
	printf( "Module overlaps with the ELF binary.\n");
	return false;
    }

    // Overlaps with the kip or utcb?
    if( is_fpage_conflict(this->kip_fp, target_vaddr, target_vaddr+size) ) {
	printf( "Module overlaps with the KIP.\n");
	return false;
    }
    if( is_fpage_conflict(this->utcb_fp, target_vaddr, target_vaddr+size) ) {
	printf( "Module overlaps with the UTCB area.\n");
	return false;
    }

    // Can we translate the addresses?
    L4_Word_t haddr, haddr2;
    if( !client_paddr_to_haddr(target_paddr, &haddr) ||
	    !client_paddr_to_haddr(target_paddr + size - 1, &haddr2) )
	return false;

    dprintf(debug_startup,  "\tInstalling module at VM phys address %x, size %d\n", target_paddr, size);

    memcpy( (void *)haddr, (void *)haddr_start, size );

    if( this->client_shared ) 
    {
	L4_Word_t idx = this->client_shared->module_count;
	if( idx >= IResourcemon_max_modules ) {
	    printf( "Too many modules (hard coded limit).\n");
	    return false;
	}
	this->client_shared->modules[ idx ].vm_offset = target_paddr;
	this->client_shared->modules[ idx ].size = size;
	

	if (! this->install_ipcmdline(cmdline, this->client_shared->modules[idx].cmdline))
	    return false;
	
	this->client_shared->module_count++;
    }

    return true;
}

void vm_t::dump_vm()
{
    printf( "########################################################\n");
    printf( "                    VM ",this->space_id," DUMP\n");
    printf( "allocated: %x\n", this->allocated);
    printf( "device_access_enabled: %x\n", this->device_access_enabled);
    printf( "client_dma_enabled: %x\n", this->client_dma_enabled);
    printf( "space_id: %x\n", this->space_id);
    printf( "haddr_base: %x\n", this->haddr_base);
    printf( "paddr_len: %x\n", this->paddr_len);
    printf( "vaddr_offset: %x\n", this->vaddr_offset);
    printf( "prio: %x\n", this->prio);
    printf( "wedge_paddr: %x\n", this->wedge_paddr);
    printf( "wedge_vaddr: %x\n", this->wedge_vaddr);
    printf( "kip: %x\n", L4_Address(this->kip_fp));
    printf( "utcb: %x\n", L4_Address(this->utcb_fp));
    printf( "vcpu_count: %x\n", this->vcpu_count);
    printf( "pcpu_count: %x\n", this->pcpu_count);
    printf( "client_shared: %x\n", this->client_shared);
    printf( "client_shared_size: %x\n", this->client_shared_size);
    printf( "client_shared_vaddr: %x\n", this->client_shared_vaddr);
    printf( "client_shared: %x\n", "\n");
    printf( "\tversion: %x\n", this->client_shared->version);
    printf( "\tcpu_cnt: %x\n", this->client_shared->cpu_cnt);
    printf( "\tprio: %x\n", this->client_shared->prio);
    printf( "\tthread_space_start: %x\n", this->client_shared->thread_space_start);
    printf( "\tthread_space_len: %x\n", this->client_shared->thread_space_len);
    printf( "\tutcb_fpage: %x\n", L4_Address(this->client_shared->utcb_fpage));
    printf( "\tkip_fpage: %x\n", L4_Address(this->client_shared->kip_fpage));
    printf( "\tlink_vaddr: %x\n", this->client_shared->link_vaddr);
    printf( "\tentry_ip: %x\n", this->client_shared->entry_ip);
    printf( "\tentry_sp: %x\n", this->client_shared->entry_sp);
    printf( "\tphys_offset: %x\n", this->client_shared->phys_offset);
    printf( "\tphys_size: %x\n", this->client_shared->phys_size);
    printf( "\tphys_end: %x\n", this->client_shared->phys_end);
    printf( "\twedge_phys_size: %x\n", this->client_shared->wedge_phys_size);
    printf( "\twedge_virt_size",this->client_shared->wedge_phys_size);
    printf( "\tvcpu_count: %x\n", this->client_shared->vcpu_count);
    printf( "\tpcpu_count: %x\n", this->client_shared->pcpu_count);
    printf( "\tmem_balloon: %x\n", this->client_shared->mem_balloon);
    printf( "\tcmdline: %x\n", this->client_shared->cmdline);

    printf( "binary_entry_vaddr: %x\n", this->binary_entry_vaddr);
    printf( "binary_start_vaddr: %x\n", this->binary_start_vaddr);
    printf( "binary_end_vaddr: %x\n", this->binary_end_vaddr);
    printf( "binary_stack_vaddr: %x\n", this->binary_stack_vaddr);
    printf( "elf_entry_vaddr: %x\n", this->elf_entry_vaddr);
    printf( "########################################################\n");
}

L4_Word_t max_logid_in_use = 1;
void set_max_logid_in_use(L4_Word_t logid)
{
   L4_Word_t id = 0;
   if (logid > max_logid_in_use)
     {
	max_logid_in_use = logid;
	for (;;)
	{
	    vm_t * v = get_vm_allocator()->space_id_to_vm( id++ );
	    if (v)
	    {
	       v->set_max_logid_in_use(logid);
	       //hout << "propagating max_logid_in_use " << logid
		 //<< " to " << id << "\n";
	    }
	    else 
		break;
	}
     }
}

