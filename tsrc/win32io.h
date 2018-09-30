/*
 * win32io - Win32 Direct IO Driver for fat32lib and smlib
 *
 */

#ifndef WIN32IO_H
#define WIN32IO_H

#include "storage_device.h"

typedef struct
{
	void* physical_drive;
	uint32_t bytes_per_sector;
	uint32_t total_sectors;
}
WIN32IO_DEVICE;

//
// struct for passing arameters of async calls to new thread
//
typedef struct
{
	void* device;
	uint32_t sector_address;
	unsigned char* buffer;
	uint16_t* async_state;
	PSTORAGE_CALLBACK_INFO callback_info;
	char write;
}
WIN32IO_ASYNC_PARAMS;



typedef struct WIN32IO_MULTI_BLOCK_CONTEXT
{
	void* device;
	uint32_t sector_address;
	unsigned char* buffer;
	uint16_t* async_state;
	STORAGE_CALLBACK_INFO_EX callback_info;
	STORAGE_CALLBACK_INFO cinfo;

}
WIN32IO_MULTI_BLOCK_CONTEXT;

static void win32io_write_multiple_blocks_callback(WIN32IO_MULTI_BLOCK_CONTEXT* context, uint16_t* result);
static uint16_t win32io_write_multiple_blocks(
	void* device, uint32_t sector_address, unsigned char* buffer, uint16_t* async_state, STORAGE_CALLBACK_INFO_EX* callback_info);

//
// gets the interface for I/O access to a physical drive on this
// computer
//
// Arguments:
//	physical drive - the UTF16 path to the physical drive. ie. \\.\E:
//	device - a STORAGE_DEVICE structure pointer to initialize
//
// WARNING! - if you create an interface to a hard drive there's always the
// chance that it will be corrupted so be safe and use removable storage.
//
void win32io_get_storage_device(TCHAR* physical_drive, STORAGE_DEVICE* device);
void win32io_release_storage_device();

//
// STORAGE_DEVICE interface functions
//
uint16_t win32io_read_sector(void* device, uint32_t sector_address, unsigned char* buffer);
uint16_t win32io_read_sector_async(void* device, uint32_t sector_address, unsigned char* buffer, uint16_t* async_state, PSTORAGE_CALLBACK_INFO callback_info);
uint16_t win32io_write_sector(void* device, uint32_t sector_address, unsigned char* buffer);
uint16_t win32io_write_sector_async(void* device, uint32_t sector_address, unsigned char* buffer, uint16_t* async_state, PSTORAGE_CALLBACK_INFO callback_info);
uint16_t win32io_get_sector_size(void* device);
uint32_t win32io_get_sector_count(void* device);

//
// async call worker
//
static void win32io_async_worker();

#endif