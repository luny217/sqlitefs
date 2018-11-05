/*
 * win32io - Win32 Direct IO Driver for fat32lib and smlib
 *
 */

#include <stdlib.h>
#include <stdio.h>
#include <windows.h>
#include "win32io.h"
#include "av_log.h"

//HANDLE h;
DWORD last_offset = 0;


LPSTR ConvertErrorCodeToString(DWORD ErrorCode)
{
	HLOCAL LocalAddress = NULL;
	FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_IGNORE_INSERTS | FORMAT_MESSAGE_FROM_SYSTEM,
		NULL, ErrorCode, 0, (PTSTR)&LocalAddress, 0, NULL);
	return (LPSTR)LocalAddress;
}

int xopen_win32(TCHAR* physical_drive)
{
	DWORD ioctl_result;
	WIN32IO_DEVICE* dev;
	DISK_GEOMETRY geo;
	DWORD bytes_returned;
	HANDLE hnd;

	dev = malloc(sizeof(WIN32IO_DEVICE));
	memset(dev, 0x00, sizeof(WIN32IO_DEVICE));
	dev->physical_drive = physical_drive;

	hnd = CreateFile((TCHAR*)physical_drive, GENERIC_READ | GENERIC_WRITE,
		FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

	DeviceIoControl(
		hnd,              // handle to device
		IOCTL_DISK_GET_DRIVE_GEOMETRY, // dwIoControlCode
		NULL,                          // lpInBuffer
		0,                             // nInBufferSize
		(LPVOID)&geo,          // output buffer
		(DWORD) sizeof(DISK_GEOMETRY),        // size of output buffer
		(LPDWORD)&bytes_returned,     // number of bytes returned
		NULL
	);

	dev->bytes_per_sector = geo.BytesPerSector;
	dev->total_sectors = geo.SectorsPerTrack * geo.TracksPerCylinder * geo.Cylinders.LowPart;
	if (!DeviceIoControl(hnd, FSCTL_LOCK_VOLUME, NULL, 0, NULL, 0, &ioctl_result, NULL))
	{
		printf("win32io: could not lock drive.\r");
	}
	if (!DeviceIoControl(hnd, FSCTL_DISMOUNT_VOLUME, NULL, 0, NULL, 0, &ioctl_result, NULL))
	{
		printf("Could not dismount volume.\r");
	}
	if (!DeviceIoControl(
		(HANDLE)hnd,              // handle to device
		FSCTL_ALLOW_EXTENDED_DASD_IO,  // dwIoControlCode
		NULL,                          // lpInBuffer
		0,                             // nInBufferSize
		NULL,                          // lpOutBuffer
		0,                             // nOutBufferSize
		&ioctl_result,     // number of bytes returned
		NULL    // OVERLAPPED structure
	))
	{
		printf("FSCTL_ALL_EXTENDED_DASD_IO failed.\r");

	}

	return (int32_t)hnd;
}

void xclose_win32(int32_t fd)
{
	HANDLE hnd = (HANDLE)fd;
	DWORD ioctl_result;
	DeviceIoControl(hnd, FSCTL_UNLOCK_VOLUME, NULL, 0, NULL, 0, &ioctl_result, NULL);
	CloseHandle(hnd);
}

//
// reads a sector from the storage device using Win32 raw I/O
//
int xread_win32(int32_t fd, uint8_t * buf, int size, int64_t offset)
{
	DWORD bytes_read = 0;
	HANDLE hnd = (HANDLE)fd;

	//if (offset != (last_offset + offset))
	{
		SetFilePointer(hnd, offset, NULL, FILE_BEGIN);
	}
	last_offset = offset;

	if (!ReadFile(hnd, buf, size, &bytes_read, NULL))
	{
		DWORD saved_error = GetLastError();
		ConvertErrorCodeToString(saved_error);
		av_log(AV_LOG_ERROR, "ReadFile error! %d\n", saved_error);
		return STORAGE_COMMUNICATION_ERROR;
	}
		

	if (bytes_read < size)
		return STORAGE_COMMUNICATION_ERROR;
	return STORAGE_SUCCESS;
}

//
// writes a sector to the storage device using Win32 I/O
//
int xwrite_win32(int32_t fd, uint8_t * buf, int size, int64_t offset)
{
	DWORD bytes_written = 0;
	HANDLE h = (HANDLE)fd;	

	if (offset != (last_offset + size))
	{
		SetFilePointer(h, offset, NULL, FILE_BEGIN);
	}
	last_offset = offset;
	
	WriteFile(h, buf, offset, &bytes_written, NULL);

	if (bytes_written < size)
	{
		printf("win32io: Write operation to %x failed!\n", offset);
		return STORAGE_COMMUNICATION_ERROR;
	}

	return STORAGE_SUCCESS;
}