/*********************************************************************
 *
 * Copyright (C) 2007,  University of Karlsruhe
 *
 * File path:     afterburn-wedge/device/ide.cc
 * Description:   Front-end emulation for IDE(ATA) disk/controller
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

/* Emulated harddisks are specified at command line with parameter
 * hd#=m,n
 * whereas # ranges from 0 to 3 and m,n specifies the major,minor number
 *
 * see http://www.lanana.org/docs/device-list/devices-2.6+.txt for a list
 * of available major/minor numbers
*/


#include <device/ide.h>
#if defined(CONFIG_DEVICE_I82371AB)
#include <device/i82371ab.h>
#endif
#include INC_WEDGE(console.h)
#include <L4VMblock_idl_client.h>
#include INC_WEDGE(resourcemon.h)
#include INC_WEDGE(hthread.h)
#include INC_ARCH(intlogic.h)
#include INC_ARCH(sync.h)
#include <string.h>


static const u8_t debug_ide = 0;
static const u8_t debug_ddos = 1;

static unsigned char ide_irq_stack[KB(16)] ALIGNED(CONFIG_STACK_ALIGN);


static inline void ide_abort_command(ide_device_t *dev, u8_t error)
{
    dev->reg_status.raw = 0;
    dev->reg_status.x.err = 1;
    dev->reg_status.x.drdy = 1;
    dev->reg_alt_status = dev->reg_status.raw;
    dev->reg_error.raw = 0;
    dev->reg_error.x.abrt = 1;
}


static inline void ide_init_device(ide_device_t *dev)
{
    dev->reg_status.raw = 0;
    if(!dev->np)
	dev->reg_status.x.drdy = 1;
    dev->reg_alt_status = dev->reg_status.raw;
    dev->reg_error.raw = 0;

    dev->dma = 0;
    dev->udma_mode = 0;
}



// called from software reset
static inline void ide_set_signature(ide_device_t *dev)
{
    dev->reg_error.raw = 0x01;
    dev->reg_nsector = 0x01;
    dev->reg_lba_low = 0x01;
    dev->reg_lba_mid = 0x00;
    dev->reg_lba_high = 0x00;
    dev->reg_device.raw = 0x00;
}


static inline void ide_set_cmd_wait(ide_device_t *dev)
{
    dev->reg_status.raw = 0;
    dev->reg_status.x.drdy = 1;
    dev->reg_status.x.dsc = 1;
    dev->reg_error.raw = 0;
}


static inline void ide_set_data_wait( ide_device_t *dev )
{
    dev->reg_status.raw = 0;
    dev->reg_status.x.drq = 1;
    dev->reg_status.x.dsc = 1;
    dev->reg_error.raw = 0;
}


// calculates C/H/S value from number of sectors
static void calculate_chs( u32_t sector, u32_t &cyl, u32_t &head, u32_t &sec )
{
    u32_t max;
    head = 16;
    sec = 63;
    max = sector/1008; // 16*63
    if(max > 16383)  // if more than 16,514,064 sectors, always return 16,383
	cyl = 16383;
    else
	cyl = max;
}


// from client26.c
static void
L4VMblock_deliver_server_irq( IVMblock_client_shared_t *client )
{

    L4_ThreadId_t from_tid;
    L4_MsgTag_t msg_tag, result_tag;
    msg_tag.raw = 0; msg_tag.X.label = 0x100; msg_tag.X.u = 1;
    L4_Set_MsgTag( msg_tag );
    L4_LoadMR( 1, client->server_irq_no );
    result_tag = L4_Reply( client->server_irq_tid );
    if( L4_IpcFailed(result_tag) ) {
	//	con << "IDE irq NTO delivery failed\n";
	L4_Set_MsgTag( msg_tag );
	L4_Ipc( client->server_irq_tid, L4_nilthread,
		L4_Timeouts(L4_Never, L4_Never), &from_tid );
    }

}


static void ide_acquire_lock(volatile u32_t *lock)
{
    u32_t old, val;
    return;
    do {
	old = *lock;
	val = cmpxchg(lock, old, (u32_t)0x1);
    } while (val != old);

}

static void ide_release_lock(volatile u32_t *lock)
{
    *lock = 0x0;
}


static void ide_irq_thread (void* params, hthread_t *thread)
{
    ide_t *il = (ide_t*)params;
    il->ide_irq_loop();
    L4_KDB_Enter("IDE irq loop returned!");
}


void ide_t::ide_irq_loop()
{
    L4_ThreadId_t tid = L4_nilthread;
    volatile IVMblock_ring_descriptor_t *rdesc;

    for(;;) {
	L4_MsgTag_t tag = L4_Wait(&tid);
	ide_device_t *dev;
	//	ide_acquire_lock(&ring_info.lock);
	while( ring_info.start_dirty != ring_info.start_free ) {
	    // Get next transaction
	    rdesc = &client_shared->desc_ring[ ring_info.start_dirty ];
	    if( rdesc->status.X.server_owned)
		break;

	    if( rdesc->status.X.server_err ) {
		L4_KDB_Enter("error");
	    } else {
		dev = (ide_device_t*)rdesc->client_data;
		switch(dev->data_transfer) {
		case IDE_CMD_DATA_IN:
		    ide_set_data_wait(dev);
		    ide_raise_irq(dev);
		    break;
		case IDE_CMD_DATA_OUT:
		    if(dev->req_sectors>0)
			ide_set_data_wait(dev);
		    else
			ide_set_cmd_wait(dev);
		    ide_raise_irq(dev);
		    break;
		default:
		    L4_KDB_Enter("Unhandled transfer type");
		}

	    }
	    // Move to next 
	    ring_info.start_dirty = (ring_info.start_dirty + 1) % ring_info.cnt;
	} /* while */
	//	ide_release_lock(&ring_info.lock);
	client_shared->client_irq_pending=false;

    } /* for */
}


void ide_t::init(void)
{
    IVMblock_devid_t devid;
    CORBA_Environment ipc_env;
    IVMblock_devprobe_t probe_data;
    idl4_fpage_t client_mapping, server_mapping;
    L4_Fpage_t fpage;
    L4_Word_t shared_base = (L4_Word_t)&shared;
    L4_Word_t start, size;

    ipc_env = idl4_default_environment;

    con << "Initializing IDE emulation\n";

    // locate VMblock server
    IResourcemon_query_interface( L4_Pager(), UUID_IVMblock, &serverid, &ipc_env);
    if( ipc_env._major != CORBA_NO_EXCEPTION) {
	con << "unable to locate block server\n";
	return;
    }
    if(debug_ddos)
	con << "found blockserver with tid " << (void*)serverid.raw << "\n";

    IResourcemon_get_client_phys_range( L4_Pager(), &L4_Myself(), &start, &size, &ipc_env);
    if( ipc_env._major != CORBA_NO_EXCEPTION ) {
	CORBA_exception_free(&ipc_env);
	con << "error resolving address";
	return;
    }

    if(debug_ddos)
	con << "PhysStart: " << (void*) start << " size " << (void*)size << "\n";

    fpage = L4_Fpage(shared_base, 32768);

    if(debug_ddos)
	con << "Shared region at " << (void*)L4_Address(fpage) << " size " << L4_Size(fpage) << "\n";

    // Register with the server
    idl4_set_rcv_window( &ipc_env, fpage);
    IVMblock_Control_register(serverid, &handle, &client_mapping, &server_mapping, &ipc_env);
    if( ipc_env._major != CORBA_NO_EXCEPTION) {
	con << "unable to register with server\n";
	return;
    }

    if(debug_ddos) {
	con << "Registered with server\n";
	con << "client mapping " << (void*)idl4_fpage_get_base(client_mapping) << "\n";
	con << "server mapping " << (void*)idl4_fpage_get_base(server_mapping) << "\n";
    }

    client_shared = (IVMblock_client_shared_t*) (shared_base + idl4_fpage_get_base(client_mapping));
    server_shared = (IVMblock_server_shared_t*) (shared_base + idl4_fpage_get_base(server_mapping));

    // init stuff
    client_shared->client_irq_no = 15;
    ring_info.start_free = 0;
    ring_info.start_dirty = 0;
    ring_info.cnt = IVMblock_descriptor_ring_size;
    ring_info.lock = 0;
    con << "addr " << (void*)&ring_info.start_free << '\n';

    if(debug_ddos) {
	con << "Server: irq " << client_shared->server_irq_no << " irq_tid: " << (void*)(client_shared->server_irq_tid.raw)
	    << " main_tid: " << (void*)client_shared->server_main_tid.raw << "\n";
	con << "Client: irq " << client_shared->client_irq_no << " irq_tid: " << (void*)(client_shared->client_irq_tid.raw)
	    << " main_tid: " << (void*)client_shared->client_main_tid.raw << "\n";
    }

    /*    con << "Wedge_phys_offset " << (void*)resourcemon_shared.wedge_phys_offset << '\n';
	  con << "Wedge_virt_offset " << (void*)resourcemon_shared.wedge_virt_offset << '\n';*/

    // Connected to server, now probe all devices and attach
    char devname[8];
    char *optname = "hd0=";
    char *next;
    for(int i=0 ; i < IDE_MAX_DEVICES ; i++) {
	int ch = i / 2;
	int sl = i % 2;
	ide_device_t *dev = (sl ? &(channel[ch].slave) : &(channel[ch].master) );

	dev->ch = &channel[ch];
	dev->np=1;

	*(optname+2) = '0' + i;

	if( !cmdline_key_search( optname, devname, 8) )
	    continue;

	devid.major = strtoul(devname, &next, 0);
	devid.minor = strtoul( strstr(devname, ",")+1, &next, 0);

	// some sanity checks
	if(devid.major > 258 || devid.minor > 255) {
	    con << "Skipping suspicious device id !\n";
	    continue;
	}

	// save 'serial'
	strncpy((char*)dev->serial, devname, 20);
	for(int j=0;j<20;j++)
	    if(dev->serial[j] == ',')
		dev->serial[j] = '0';

	if(debug_ddos)
	    con << "Probing hd" << i << " with major " << devid.major  << " minor " << devid.minor << "\n";

	IVMblock_Control_probe( serverid, &devid, &probe_data, &ipc_env );
	if( ipc_env._major != CORBA_NO_EXCEPTION ) {
	    CORBA_exception_free(&ipc_env);
	    con << "error probing device\n";
	    continue;
	}

	if( probe_data.hardsect_size != 512 ) {
	    con << "Error: Unusual sector size " << probe_data.hardsect_size << '\n';
	    continue;
	}
	if(debug_ddos) {
	    con << "block size " << probe_data.block_size << "\n";
	    con << "hardsect size " << probe_data.hardsect_size << "\n";
	    con << "sectors " << probe_data.device_size << "\n";
	}
	dev->lba_sector = probe_data.device_size;

	// Attach to device
	IVMblock_Control_attach( serverid, handle, &devid, 3, dev->conhandle, &ipc_env);
	if( ipc_env._major != CORBA_NO_EXCEPTION ) {
	    CORBA_exception_free(&ipc_env);
	    con << "error attaching to device\n";
	    continue;
	}

	// calculate physical address

	dev->io_buffer_dma_addr = (u32_t)&dev->io_buffer - resourcemon_shared.wedge_virt_offset + resourcemon_shared.wedge_phys_offset;
	con << "IO buffer at " << (void*)dev->io_buffer_dma_addr << '\n';

	dev->np=0;
	dev->dev_num = i;
	ide_set_signature( dev );
	ide_init_device( dev );

	calculate_chs(dev->lba_sector, dev->cylinder, dev->head, dev->sector);
	con << "IDE hd" << i << " " << dev->lba_sector << " sectors (C/H/S: "
	    << dev->cylinder << "/" << dev->head << "/" << dev->sector << ")\n";
	}

    for(int i=0; i < (IDE_MAX_DEVICES/2) ; i++) {
	channel[i].cur_device = &channel[i].master;
	channel[i].irq = (i ? 15 : 14);
    }

    /*    intlogic_t &intlogic = get_intlogic();
    intlogic.set_irq_trace(14);
    intlogic.set_irq_trace(15);*/

    // start irq loop thread
    vcpu_t &vcpu = get_vcpu();
    con << "creating irq loop\n";
    hthread_t *irq_thread = 
	get_hthread_manager()->create_thread( &vcpu, (L4_Word_t)ide_irq_stack,
	     sizeof(ide_irq_stack), resourcemon_shared.prio, ide_irq_thread, 
					      L4_Pager(), this );
    if( !irq_thread ) {
	con << "Error creating ide irq thread\n";
	L4_KDB_Enter("irq thread");
	return;
    }
    client_shared->client_irq_tid = irq_thread->get_global_tid();
    client_shared->client_main_tid = irq_thread->get_global_tid();
    irq_thread->start();
    con << "IDE IRQ loop started with thread id " << (void*)irq_thread->get_global_tid().raw << '\n';
}


void ide_t::ide_write_register(ide_channel_t *ch, u16_t reg, u16_t value)
{
    if(debug_ide)
	con << "IDE write to register " << reg_to_str_write[reg] << " value " << (void*)value << "\n";
    switch(reg) {
    case 0: 
	ide_io_data_write( ch->cur_device, value);
	break;
    case 1:
	ch->master.reg_feature = value;
	ch->slave.reg_feature = value;
	break;
    case 2:
	ch->master.reg_nsector = value;
	ch->slave.reg_nsector = value;
	break;
    case 3:
	ch->master.reg_lba_low = value;
	ch->slave.reg_lba_low = value;
	break;
    case 4:
	ch->master.reg_lba_mid = value;
	ch->slave.reg_lba_mid = value;
	break;
    case 5:
	ch->master.reg_lba_high = value;
	ch->slave.reg_lba_high = value;
	break;
    case 6:
	ch->master.reg_device.raw = value;
	ch->slave.reg_device.raw = value;
	if(value & 0x10)
	    ch->cur_device = &ch->slave;
	else
	    ch->cur_device = &ch->master;
	break;
    case 7:
	ide_command( ch->cur_device, value );
	break;
    case 14:
	ch->master.reg_dev_control.raw = value;
	ch->slave.reg_dev_control.raw = value;
	if(value & 0x4)
	    ide_software_reset(ch);
	break;
    default:
	con << "IDE write to unknown register " << (void*)reg << "\n";
    }
}


u16_t ide_t::ide_read_register(ide_channel_t *ch, u16_t reg)
{
    ide_device_t *dev = ch->cur_device;

    if(dev->np)
	return 0;

    if(reg && debug_ide)
	con << "IDE read from register " << reg_to_str_read[reg] << "\n";

    switch(reg) {
    case 0: return ide_io_data_read(dev);
    case 1: return dev->reg_error.raw;
    case 2: return dev->reg_nsector;
    case 3: return dev->reg_lba_low;
    case 4: return dev->reg_lba_mid;
    case 5: return dev->reg_lba_high;
    case 6: return dev->reg_device.raw;
    case 7: return dev->reg_status.raw;
	//    case 14: return dev->reg_alt_status;
    case 14: return dev->reg_status.raw;
    default:
	con << "IDE read from unknown register " << (void*) reg << "\n";
    }
    return 0;
}


void ide_t::ide_raise_irq( ide_device_t *dev )
{
    if( !(dev->reg_dev_control.x.nien))
	 get_intlogic().raise_irq(dev->ch->irq);
}


void ide_t::ide_portio( u16_t port, u32_t & value, bool read )
{
    u16_t reg = port & 0xF;// + ((port & 0x200)>>8) 

    if(read)
	switch( port &~(0xf)) {
	case 0x1F0:
	    value = ide_read_register( &channel[0], reg);
	    break;
	case 0x3F0:
	    value = ide_read_register( &channel[0], reg+8);
	    break;
	case 0x170:
	    value = ide_read_register( &channel[1], reg);
	    break;
	case 0x370:
	    value = ide_read_register( &channel[1], reg+8);
	    break;
	default:
	    con << "IDE read from unknown port " << (void*) port << "\n";

	}
    else
	switch(port & ~(0xf)) {
	case 0x1F0:
	    ide_write_register( &channel[0], reg, value );
	    break;
	case 0x3F0:
	    ide_write_register( &channel[0], reg+8, value );
	    break;
	case 0x170:
	    ide_write_register( &channel[1], reg, value );
	    break;
	case 0x370:
	    ide_write_register( &channel[1], reg+8, value );
	    break;
	default:
	    con << "IDE write to unknown port " << (void*) port << "\n";
	}
}


u16_t ide_t::ide_io_data_read( ide_device_t *dev )
{
    if(dev->data_transfer != IDE_CMD_DATA_IN) {
	con << "IDE read with no data request\n";
	return 0xff;
    }
    u16_t dat = *((u16_t*)(dev->io_buffer+dev->io_buffer_index));
    dev->io_buffer_index += 2;

    if(dev->io_buffer_index >= dev->io_buffer_size) { // transfer complete
	if( dev->req_sectors > 0) {
	    u32_t n = dev->req_sectors;
	    if( n > IDE_IOBUFFER_BLOCKS)
		n = IDE_IOBUFFER_BLOCKS;
	    l4vm_transfer_block( dev->get_sector(), n*IDE_SECTOR_SIZE, (void*)dev->io_buffer_dma_addr, false, dev);
	    dev->req_sectors -= n;
	    dev->io_buffer_index = 0;
	    dev->io_buffer_size = n * IDE_SECTOR_SIZE;
	    dev->set_sector( dev->get_sector() + n);
	    }
	else {
	    dev->data_transfer = IDE_CMD_DATA_NONE;
	    ide_set_cmd_wait(dev);
	}
	return dat;
    }
    // TODO: check if raising the INT here is too early
    if( (dev->io_buffer_index % IDE_SECTOR_SIZE)==0) // next DRQ data block
	ide_raise_irq(dev);
    return dat;
}


void ide_t::ide_io_data_write( ide_device_t *dev, u16_t value )
{
    if(dev->data_transfer != IDE_CMD_DATA_OUT) { // do nothing
	con << "IDE write with no data request\n";
	return;
    }
    //    con << "IDE write value " <<(void*)value << '\n';
    *((u16_t*)(dev->io_buffer+dev->io_buffer_index)) = value;
    dev->io_buffer_index += 2;

    if(dev->io_buffer_index >= dev->io_buffer_size) {
	if( dev->req_sectors > 0 ) {
	    u32_t n = dev->req_sectors;
	    if( n > IDE_IOBUFFER_BLOCKS )
		n = IDE_IOBUFFER_BLOCKS;
	    l4vm_transfer_block( dev->get_sector(), n*IDE_SECTOR_SIZE, (void*)dev->io_buffer_dma_addr, true, dev);
	    dev->req_sectors -= n;
	    dev->io_buffer_index = 0;
	    dev->io_buffer_size = n * IDE_SECTOR_SIZE;
	    dev->set_sector( dev->get_sector() + n );
	}
	else {
	    l4vm_transfer_block( dev->get_sector(), dev->io_buffer_size, (void*)dev->io_buffer_dma_addr, true, dev);
	    ide_set_cmd_wait(dev);
	}
	return;
    }

    // TODO: see above
    if( (dev->io_buffer_index % IDE_SECTOR_SIZE)==0) // next DRQ data block
	ide_raise_irq(dev);
}


void ide_t::ide_command(ide_device_t *dev, u16_t cmd)
{
    // ignore commands to non existent devices
    if(dev->np) {
	con << "Ignoring command to np device\n";
	return;
    }

    if(debug_ide)
	con << "IDE command " << (void*)(cmd) << "\n";

    switch(cmd) {
    case IDE_CMD_CFA_ERASE_SECTOR:
    case IDE_CMD_CFA_REQUEST_EXTENDED_ERROR_CODE:
    case IDE_CMD_CFA_TRANSLATE_SECTOR:
    case IDE_CMD_CFA_WRITE_MULTIPLE_WITHOUT_ERASE:
    case IDE_CMD_CFA_WRITE_SECTORS_WITHOUT_ERASE:
    case IDE_CMD_CHECK_MEDIA_CARD_TYPE:
    case IDE_CMD_CHECK_POWER_MODE:
    case IDE_CMD_DEVICE_CONFIGURATION:
    case IDE_CMD_DEVICE_RESET:
    case IDE_CMD_DOWNLOAD_MICROCODE:

    case IDE_CMD_EXECUTE_DEVICE_DIAGNOSTIC:
    case IDE_CMD_FLUSH_CACHE:
    case IDE_CMD_FLUSH_CACHE_EXT:
    case IDE_CMD_GET_MEDIA_STATUS:
	break;

    case IDE_CMD_IDENTIFY_DEVICE:
	ide_identify(dev); 
	ide_raise_irq(dev);
	return;

    case IDE_CMD_IDENTIFY_PACKET_DEVICE:
    case IDE_CMD_IDLE:
    case IDE_CMD_IDLE_IMMEDIATE:
    case IDE_CMD_MEDIA_EJECT:
    case IDE_CMD_MEDIA_LOCK:

    case IDE_CMD_MEDIA_UNLOCK:
    case IDE_CMD_NOP:
    case IDE_CMD_PACKET:
    case IDE_CMD_READ_BUFFER:
	break;
    case IDE_CMD_READ_DMA:
	ide_read_dma(dev);
	return;
    case IDE_CMD_READ_DMA_EXT:
    case IDE_CMD_READ_DMA_QUEUED:

    case IDE_CMD_READ_DMA_QUEUED_EXT:
    case IDE_CMD_READ_LOG_EXT:
    case IDE_CMD_READ_MULTIPLE:

    case IDE_CMD_READ_MULTIPLE_EXT:
    case IDE_CMD_READ_NATIVE_MAX_ADDRESS:
    case IDE_CMD_READ_NATIVE_MAX_ADDRESS_EXT:
	break;
    case IDE_CMD_READ_SECTORS:
	ide_read_sectors(dev);
	return;
    case IDE_CMD_READ_SECTORS_EXT:
    case IDE_CMD_READ_VERIFY_SECTORS:
    case IDE_CMD_READ_VERIFY_SECTORS_EXT:
    case IDE_CMD_SECURITY_DISABLE_PASSWORD:
    case IDE_CMD_SECURITY_ERASE_PREPARE:
    case IDE_CMD_SECURITY_ERASE_UNIT:

    case IDE_CMD_SECURITY_FREEZE_LOCK:
    case IDE_CMD_SECURITY_SET_PASSWORD:
    case IDE_CMD_SECURITY_UNLOCK:
    case IDE_CMD_SEEK:
    case IDE_CMD_SERVICE:
	break;

    case IDE_CMD_SET_FEATURES:
	ide_set_features(dev);
	ide_raise_irq(dev);
	return;

    case IDE_CMD_SET_MAX:
    case IDE_CMD_SET_MAX_ADDRESS_EXT:
	break;

    case IDE_CMD_SET_MULTIPLE_MODE:
	// TODO: check for power of 2
	if(dev->reg_nsector > IDE_MAX_READ_WRITE_MULTIPLE)
	    ide_abort_command(dev, 0);
	else {
	    dev->mult_sectors = dev->reg_nsector;
	    ide_set_cmd_wait(dev);
	}
	ide_raise_irq(dev);
	return;

    case IDE_CMD_SLEEP:

    case IDE_CMD_SMART:
    case IDE_CMD_STANDBY:
    case IDE_CMD_STANDBY_IMMEDIATE:
    case IDE_CMD_WRITE_BUFFER:
	break;
    case IDE_CMD_WRITE_DMA:
	ide_write_dma(dev);
	return;
    case IDE_CMD_WRITE_DMA_EXT:
    case IDE_CMD_WRITE_DMA_QUEUED:
    case IDE_CMD_WRITE_DMA_QUEUED_EXT:
    case IDE_CMD_WRITE_LOG_EXT:
    case IDE_CMD_WRITE_MULTIPLE:

    case IDE_CMD_WRITE_MULTIPLE_EXT:
	break;
    case IDE_CMD_WRITE_SECTORS:
	ide_write_sectors(dev);
	return;
    case IDE_CMD_WRITE_SECTORS_EXT:
	break;

	// linux keeps calling this old stuff, simply do nothing
    case IDE_CMD_INITIALIZE_DEVICE_PARAMETERS:
    case IDE_CMD_RECALIBRATE:
	ide_set_cmd_wait(dev);
	ide_raise_irq(dev);
	return;

    default:
	con << "IDE unknown command " << (void*)cmd << "\n";
	ide_abort_command(dev, 0);
	ide_raise_irq(dev);
	return;
    }
    ide_abort_command(dev, 0);
    ide_raise_irq(dev);
}


// from qemu hw/ide.c
static void padstr(char *str, const char *src, u32_t len)
{
    int i, v;
    for(i = 0; i < len; i++) {
        if (*src)
            v = *src++;
        else
            v = ' ';
        *(char *)((long)str ^ 1) = v;
        str++;
    }
}


/* 8.15, p. 113 */
void ide_t::ide_identify( ide_device_t *dev )
{

    memset(dev->io_buffer, 0, 512);
    u16_t *buf = (u16_t*)dev->io_buffer;

    // fill buffer with identify information
    *(buf) = 0x0000;
    // obsolete since ata-6, but most OSes use it nevertheless
    *(buf+1) = dev->cylinder;
    *(buf+3) = dev->head;
    *(buf+6) = dev->sector;

    padstr( (char*)(buf+10), (char*)dev->serial, 20 );
    padstr( (char*)(buf+23), "l4ka 0.1", 8 );
    padstr( (char*)(buf+27), "L4KA AFTERBURNER HARDDISK", 40 );
    *(buf+47) = 0x8000 | IDE_MAX_READ_WRITE_MULTIPLE;
    *(buf+49) = 0x0300;
#if defined(CONFIG_DEVICE_I82371AB)
    *(buf+53) = 6;
#else
    *(buf+53) = 2;
#endif
    *(buf+59) = 0x0100 | 1 ;
    *(buf+60) = dev->lba_sector;
    *(buf+61) = dev->lba_sector >> 16;
    *(buf+64) = 3;	// pio mode 3 and 4
    *(buf+67) = 120;
    *(buf+68) = 120;
    *(buf+80) = 0x0070; // supports ata-6
    *(buf+81) = 0x0019; // ata-6 rev3a
    *(buf+82) = 0x4200; // support device reset and nop command
    *(buf+83) = (1 << 14);
    *(buf+84) = (1 << 14);
    *(buf+85) = 0;
    *(buf+86) = 0;
    *(buf+87) = (1 << 14);
    if(dev->dma)
	*(buf+88) = 7 | (1 << (dev->udma_mode + 8)); // udma support
    else
	*(buf+88) = 7; 
    *(buf+93) = (1 << 14) | 1;
    *(buf+100) = 0; // lba48

    ide_set_data_wait(dev);

    // setup data transfer
    dev->io_buffer_index = 0;
    dev->io_buffer_size = IDE_SECTOR_SIZE;
    dev->data_transfer = IDE_CMD_DATA_IN;
}



// 9.2, p. 315
void ide_t::ide_software_reset( ide_channel_t *ch)
{
    if(!ch->master.np) {
	ide_set_signature(&ch->master);
	ide_set_data_wait(&ch->master);
    }
    if(!ch->slave.np) {
	ide_set_signature(&ch->slave);
	ide_set_data_wait(&ch->slave);
    }
}


/* client code for the dd/os block server */
void ide_t::l4vm_transfer_block( u32_t block, u32_t size, void *data, bool write, ide_device_t* dev)
{
    volatile IVMblock_ring_descriptor_t *rdesc;
    ASSERT( size <= IDE_IOBUFFER_SIZE );

    // get next free ring descriptor
    ide_acquire_lock(&ring_info.lock);
    rdesc = &client_shared->desc_ring[ ring_info.start_free ];
    ASSERT( !rdesc->status.X.server_owned );
    ring_info.start_free = (ring_info.start_free+1) % ring_info.cnt;
    ide_release_lock(&ring_info.lock);

    rdesc->handle = handle;
    rdesc->size = size;
    rdesc->offset = block;
    rdesc->page = (void**)(data);
    rdesc->client_data = (void**)dev;
    rdesc->status.raw = 0;
    rdesc->status.X.do_write = write;
    rdesc->status.X.speculative = 0;
    rdesc->status.X.server_owned = 1;

    server_shared->irq_status |= 1; // was L4VMBLOCK_SERVER_IRQ_DISPATCH
    server_shared->irq_pending = true;

    L4VMblock_deliver_server_irq(client_shared);
}


// 8.34, p. 199
void ide_t::ide_read_sectors( ide_device_t *dev )
{
    u32_t sector, n;

    n = dev->req_sectors = ( dev->reg_nsector ? dev->reg_nsector : 256 );

    if( dev->reg_device.x.lba ) {
	sector = dev->get_sector();
	//	con << "Read sector " << sector << ' ' << n <<'\n';
    }
    else
	con << "IDE read non lba access\n";

    if( n > IDE_IOBUFFER_BLOCKS)
	n = IDE_IOBUFFER_BLOCKS;
    l4vm_transfer_block( sector, n*IDE_SECTOR_SIZE, (void*)dev->io_buffer_dma_addr, false, dev);
    dev->data_transfer = IDE_CMD_DATA_IN;
    dev->io_buffer_index = 0;
    dev->io_buffer_size = IDE_SECTOR_SIZE * n ;
    dev->req_sectors -= n;
    dev->set_sector( sector + n );
}


// 8.62, p. 303
void ide_t::ide_write_sectors( ide_device_t *dev )
{
    u32_t sector, n;

    n = dev->req_sectors = ( dev->reg_nsector ? dev->reg_nsector : 256 );

    if( dev->reg_device.x.lba ) {
	sector = dev->get_sector();
    }
    else
	con << "IDE write non lba access\n";

    if( n > IDE_IOBUFFER_BLOCKS )
	n = IDE_IOBUFFER_BLOCKS;

    dev->data_transfer = IDE_CMD_DATA_OUT;
    dev->io_buffer_size = IDE_SECTOR_SIZE * n;
    dev->io_buffer_index = 0;
    dev->req_sectors -= n;

    ide_set_data_wait(dev);
    dev->reg_status.x.drdy=1;
    ide_raise_irq(dev);
}


// 8.25, p. 166
void ide_t::ide_read_dma( ide_device_t *dev )
{
#if defined(CONFIG_DEVICE_I82371AB)
    i82371ab_t *dma = i82371ab_t::get_device(0);
    //    dma->get_prdt_entry(0,0);
    con << "IDE read DMA\n";
    L4_KDB_Enter("read dma");
#else 
    // no dma support, abort
    ide_abort_command(dev, 0);
    ide_raise_irq(dev);
#endif
}


void ide_t::ide_write_dma( ide_device_t *dev )
{
#if defined(CONFIG_DEVICE_I82371AB)
    con << "IDE write DMA\n";
    L4_KDB_Enter("write dma");
#else 
    // no dma support, abort
    ide_abort_command(dev, 0);
    ide_raise_irq(dev);
#endif
}


// 8.46, p. 224
void ide_t::ide_set_features( ide_device_t *dev )
{
    switch(dev->reg_feature) 
	{
	case 0x03 : // set transfer mode
	    switch(dev->reg_nsector >> 3) 
		{
		case 0x0: // pio default mode
		    dev->dma=0;
		    break;

		case 0x8: // udma mode
		    dev->dma=1;
		    dev->udma_mode = dev->reg_nsector & 0x7;
		    break;

		default:
		    ide_abort_command(dev, 0);
		    return;
		}
	    break;

	    
	default:
	    ide_abort_command(dev, 0);
	    return;
	}
    ide_set_cmd_wait(dev);
}


void ide_portio( u16_t port, u32_t & value, bool read )
{
#if defined(CONFIG_DEVICE_IDE)
extern ide_t ide;
    ide.ide_portio( port, value, read);
#endif
}

