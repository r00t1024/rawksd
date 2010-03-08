#include "launcher.h"
#include "menu.h"
#include "wdvd.h"
#include "riivolution.h"

#include <stdio.h>
#include <ogc/lwp_watchdog.h>

#include <files.h>

typedef void (*AppReport) (const char*, ...);
typedef void (*AppEnter) (AppReport);
typedef int  (*AppLoad) (void**, s32*, s32*);
typedef void* (*AppExit) ();
typedef void (*AppStart) (AppEnter*, AppLoad*, AppExit*);

typedef struct {
	u8 revision[16];
	AppStart start;
	u32 loader_size;
	u32 data_size;
	u8 unused[4];
} app_info;

static union {
	u32 partition_info[24];
	app_info app;
} ATTRIBUTE_ALIGN(32);

static char GameName[0x100];

#define PART_OFFSET			0x00040000
#define APP_INFO_OFFSET		0x2440
#define APP_DATA_OFFSET		(APP_INFO_OFFSET + 0x20)

#define min(a,b) ((a)>(b) ? (b) : (a))
#define max(a,b) ((a)>(b) ? (a) : (b))

static int nullprintf(const char* fmt, ...)
{
	return 0;
}

const char* Launcher_GetGameName()
{
	return GameName;
}

LauncherStatus::Enum Launcher_Init()
{
	int fd = WDVD_Init();
	if (fd < 0)
		return LauncherStatus::IosError;

	WDVD_Reset();

	return LauncherStatus::OK;
}

LauncherStatus::Enum Launcher_ReadDisc()
{
	u32 i;
	
	if (!WDVD_CheckCover())
		return LauncherStatus::NoDisc;
	
	WDVD_Reset();
	
	WDVD_LowReadDiskId();
	WDVD_LowUnencryptedRead(MEM_BASE, 0x20, 0x00000000); // Just to make sure...
	
	WDVD_LowUnencryptedRead(partition_info, 0x20, PART_OFFSET);
	// make sure there is at least one primary partition
	if (partition_info[0] == 0)
		return LauncherStatus::ReadError;
	
	// read primary partition table
	WDVD_LowUnencryptedRead(partition_info + 8, max(4, min(8, partition_info[0])) * 8, partition_info[1] << 2);
	for (i = 0; i < partition_info[0]; i++)
		if (partition_info[i * 2 + 8 + 1] == 0)
			break;
	
	// make sure we found a game partition
	if (i >= partition_info[0])
		return LauncherStatus::ReadError;
	
	if (WDVD_LowOpenPartition((u64)partition_info[i * 2 + 8] << 2) != 1)
		return LauncherStatus::ReadError;

	// TODO: Read Game title from opening.bnr on primary partition
	WDVD_LowUnencryptedRead(GameName, 0x40, 0x20);

	return LauncherStatus::OK;
}

static u32 fstdata[0x10] ATTRIBUTE_ALIGN(32);

LauncherStatus::Enum Launcher_RVL()
{
	if (WDVD_LowRead(fstdata, 0x40, 0x420) < 0)
		return LauncherStatus::ReadError;
	fstdata[2] <<= 2;
	if (WDVD_LowRead(SYS_GetArena2Lo(), fstdata[2], (u64)fstdata[1] << 2) < 0) // MEM2
		return LauncherStatus::ReadError;
	RVL_SetFST(SYS_GetArena2Lo(), fstdata[2]);

	return LauncherStatus::OK;
}

LauncherStatus::Enum Launcher_CommitRVL(bool dip)
{
	if (dip) {
		File_CreateDir("/riivolution/temp");
		File_CreateFile("/riivolution/temp/fst");
		int tmpfd = File_Open("/riivolution/temp/fst", O_WRONLY | O_TRUNC);
		if (tmpfd < 0)
			return LauncherStatus::IosError;
		File_Write(tmpfd, SYS_GetArena2Lo(), fstdata[2]);
		File_Close(tmpfd);
		fstdata[1] = (u32)(RVL_GetShiftOffset(fstdata[2]) >> 2);
		RVL_AddPatch(RVL_AddFile("/riivolution/temp/fst"), 0, (u64)fstdata[1] << 2, fstdata[2]);
		fstdata[2] >>= 2;
		fstdata[3] = fstdata[2]; // NOTE: Not really sure why
		File_CreateFile("/riivolution/temp/fst.header");
		tmpfd = File_Open("/riivolution/temp/fst.header", O_WRONLY | O_TRUNC);
		if (tmpfd < 0)
			return LauncherStatus::IosError;
		File_Write(tmpfd, fstdata, 0x40);
		File_Close(tmpfd);
		RVL_AddPatch(RVL_AddFile("/riivolution/temp/fst.header"), 0, 0x420, 0x40);
	} else
		memcpy(*MEM_FSTADDRESS, SYS_GetArena2Lo(), fstdata[2]); // TODO: Account for fstdata changing

	return LauncherStatus::OK;
}

static void* app_address = NULL;

static void* FindInBuffer(void* buffer, s32 size, const void* tofind, s32 findsize)
{
	for (int i = 0; i < size - findsize; i++, buffer = (u8*)buffer + 1) {
		if (!memcmp(buffer, tofind, findsize))
			return buffer;
	}

	return NULL;
}

static void ApplyBinaryPatches(s32 app_section_size)
{
	void* found;

	// DIP
	while ((found = FindInBuffer(app_address, app_section_size, "/dev/di", 7))) {
		memcpy(found, "/dev/do", 7);
	}
}

LauncherStatus::Enum Launcher_RunApploader()
{
	AppEnter app_enter = NULL;
	AppLoad app_loader = NULL;
	AppExit app_exit = NULL;
	s32 app_section_size = 0;
	s32 app_disc_offset = 0;
	
	settime(secs_to_ticks(time(NULL) - 946684800));
	
	// Avoid a flash of green
	VIDEO_SetBlack(true); VIDEO_Flush(); VIDEO_WaitVSync(); VIDEO_WaitVSync();
	
	// put crap in memory to keep the apploader/dol happy
	*MEM_VIRTUALSIZE = 0x01800000;
	*MEM_PHYSICALSIZE = 0x01800000;
	*MEM_BI2 = 0;
	*MEM_BOOTCODE = 0x0D15EA5E;
	*MEM_VERSION = 1;
	*MEM_ARENA1LOW = 0;
	*MEM_BUSSPEED = 0x0E7BE2C0;
	*MEM_CPUSPEED = 0x2B73A840;
	*MEM_GAMEIDADDRESS = MEM_BASE;
	memcpy(MEM_GAMEONLINE, MEM_BASE, 4);
	//*MEM_GAMEONLINE = (u32)MEM_BASE;
	DCFlushRange(MEM_BASE, 0x3F00);
	
	// read the apploader info
	WDVD_LowRead(&app, sizeof(app_info), APP_INFO_OFFSET);
	DCFlushRange(&app, sizeof(app_info));
	// read the apploader into memory
	WDVD_LowRead(MEM_APPLOADER, app.loader_size + app.data_size, APP_DATA_OFFSET);
	DCFlushRange(MEM_APPLOADER, app.loader_size + app.data_size);
	app.start(&app_enter, &app_loader, &app_exit);
	app_enter((AppReport)nullprintf);
	
	while (app_loader(&app_address, &app_section_size, &app_disc_offset)) {
		WDVD_LowRead(app_address, app_section_size, (u64)app_disc_offset << 2);
		DCFlushRange(app_address, app_section_size);
		ApplyBinaryPatches(app_section_size);
		app_address = NULL;
		app_section_size = 0;
		app_disc_offset = 0;
	}
	
	// copy the IOS version over the expected IOS version
	memcpy(MEM_IOSEXPECTED, MEM_IOSVERSION, 4);
	
	app_address = app_exit();

	return LauncherStatus::OK;
}

LauncherStatus::Enum Launcher_Launch()
{
	if (app_address) {
		WDVD_Close();
		DCFlushRange(MEM_BASE, 0x17FFFFFF);
		
		SYS_ResetSystem(SYS_SHUTDOWN, 0, 0);
		__asm__ __volatile__ (
			"mtlr %0;"
			"blr"
			:
			: "r" (app_address)
		);
	}

	// We shouldn't get to here
	VIDEO_SetBlack(false);
	VIDEO_Flush();
	VIDEO_WaitVSync(); VIDEO_WaitVSync();
	return LauncherStatus::ReadError;
}
