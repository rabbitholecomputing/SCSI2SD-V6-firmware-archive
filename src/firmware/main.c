//	Copyright (C) 2014 Michael McMaster <michael@codesrc.com>
//
//	This file is part of SCSI2SD.
//
//	SCSI2SD is free software: you can redistribute it and/or modify
//	it under the terms of the GNU General Public License as published by
//	the Free Software Foundation, either version 3 of the License, or
//	(at your option) any later version.
//
//	SCSI2SD is distributed in the hope that it will be useful,
//	but WITHOUT ANY WARRANTY; without even the implied warranty of
//	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//	GNU General Public License for more details.
//
//	You should have received a copy of the GNU General Public License
//	along with SCSI2SD.  If not, see <http://www.gnu.org/licenses/>.

#include "stm32f2xx.h"

#include "config.h"
#include "bsp.h"
#include "disk.h"
#include "fpga.h"
#include "hwversion.h"
#include "led.h"
#include "sd.h"
#include "scsi.h"
#include "scsiPhy.h"
#include "time.h"
#include "sdio.h"
#include "usb_device/usb_device.h"
#include "usb_device/usbd_composite.h"
#include "usb_device/usbd_msc_storage_sd.h"

const char* Notice = "Copyright (C) 2020 Michael McMaster <michael@codesrc.com>";

static int isUsbStarted;

void mainEarlyInit()
{
	// USB device is initialised before mainInit is called
	s2s_initUsbDeviceStorage();
}

void mainInit()
{
	s2s_timeInit();
	s2s_checkHwVersion();

	// DISable the ULPI chip
	HAL_GPIO_WritePin(nULPI_RESET_GPIO_Port, nULPI_RESET_Pin, GPIO_PIN_RESET);

	s2s_ledInit();
	s2s_fpgaInit();
	s2s_deviceEarlyInit();
	scsiPhyInit();

	scsiDiskInit();
	sdInit();
	s2s_configInit(&scsiDev.boardCfg);
	scsiPhyConfig();
	scsiInit();

	MX_USB_DEVICE_Init(); // USB lun config now available.
	isUsbStarted = 1;

	// Optional bootup delay
	int delaySeconds = 0;
	while (delaySeconds < scsiDev.boardCfg.startupDelay) {
		// Keep the USB connection working, otherwise it's very hard to revert
		// silly extra-long startup delay settings.
		int i;
		for (i = 0; i < 200; i++) {
			s2s_delay_ms(5);
			scsiDev.watchdogTick++;
			s2s_configPoll();
		}
		++delaySeconds;
	}

}

void mainLoop()
{
	scsiDev.watchdogTick++;

	scsiPoll();
	scsiDiskPoll();
	s2s_configPoll();
	s2s_usbDevicePoll();

	if (unlikely(scsiDev.phase == BUS_FREE))
	{
		if (s2s_pollMediaChange())
		{
			s2s_configInit(&scsiDev.boardCfg);
			scsiPhyConfig();
			scsiInit();
		}
	}
#warning MOVE THIS CODE TO SD READ/WRITE METHODS
#if 0
	else if ((scsiDev.phase >= 0) && (blockDev.state & DISK_PRESENT))
	{
		// don't waste time scanning SD cards while we're doing disk IO
		lastSDPoll = s2s_getTime_ms();
	}
#endif
}

