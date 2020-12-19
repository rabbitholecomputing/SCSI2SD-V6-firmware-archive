//	Copyright (C) 2013 Michael McMaster <michael@codesrc.com>
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
#include "sdio.h"
#include "bsp_driver_sd.h"


#include "scsi.h"
#include "config.h"
#include "disk.h"
#include "sd.h"
#include "led.h"
#include "time.h"

#include "scsiPhy.h"

#include <string.h>

static void sd_earlyInit(S2S_Device* dev);
static const S2S_BoardCfg* sd_getBoardConfig(S2S_Device* dev);
static S2S_Target* sd_getTargets(S2S_Device* dev, int* count);
static uint32_t sd_getCapacity(S2S_Device* dev);
static int sd_pollMediaChange(S2S_Device* dev);
static void sd_saveConfig(S2S_Target* target);

// Global
SdCard sdCard S2S_DMA_ALIGN = {
		{
			sd_earlyInit,
			sd_getBoardConfig,
			sd_getTargets,
			sd_getCapacity,
			sd_pollMediaChange,
			sd_saveConfig
		}
};

S2S_Device* sdDevice = &(sdCard.dev);

static int sdCmdActive = 0; // TODO MOVE to sdCard

int
sdReadDMAPoll(uint32_t remainingSectors)
{
	// TODO DMA byte counting disabled for now as it's not
	// working.
	// We can ask the SDIO controller how many bytes have been
	// processed (SDIO_GetDataCounter()) but I'm not sure if that
	// means the data has been transfered via dma to memory yet.
//	uint32_t dmaBytesRemaining = __HAL_DMA_GET_COUNTER(hsd.hdmarx) * 4;

	if (hsd.DmaTransferCplt ||
		hsd.SdTransferCplt ||

//	if (dmaBytesRemaining == 0 ||
		(HAL_SD_ErrorTypedef)hsd.SdTransferErr != SD_OK)
	{
		HAL_SD_CheckReadOperation(&hsd, (uint32_t)SD_DATATIMEOUT);
		// DMA transfer is complete
		sdCmdActive = 0;
		return remainingSectors;
	}
/*	else
	{
		return remainingSectors - ((dmaBytesRemaining + (SD_SECTOR_SIZE - 1)) / SD_SECTOR_SIZE);
	}*/
	return 0;
}

void sdReadDMA(uint32_t lba, uint32_t sectors, uint8_t* outputBuffer)
{
	if (HAL_SD_ReadBlocks_DMA(
			&hsd, (uint32_t*)outputBuffer, lba * 512ll, 512, sectors
			) != SD_OK)
	{
		scsiDiskReset();

		scsiDev.status = CHECK_CONDITION;
		scsiDev.target->state.sense.code = HARDWARE_ERROR;
		scsiDev.target->state.sense.asc = LOGICAL_UNIT_COMMUNICATION_FAILURE;
		scsiDev.phase = STATUS;
	}
	else
	{
		sdCmdActive = 1;
	}
}

void sdCompleteTransfer()
{
	if (sdCmdActive)
	{
		HAL_SD_StopTransfer(&hsd);
		HAL_DMA_Abort(hsd.hdmarx);
		HAL_DMA_Abort(hsd.hdmatx);
		sdCmdActive = 0;
	}
}


static void sdInitDMA()
{
	// One-time init only.
	static uint8_t init = 0;
	if (init == 0)
	{
		init = 1;

		//TODO MM SEE STUPID SD_DMA_RxCplt that require the SD IRQs to preempt
		// Ie. priority must be geater than the SDIO_IRQn priority.
		// 4 bits preemption, NO sub priority.
		HAL_NVIC_SetPriorityGrouping(NVIC_PRIORITYGROUP_4);
		HAL_NVIC_SetPriority(DMA2_Stream3_IRQn, 8, 0);
		HAL_NVIC_EnableIRQ(DMA2_Stream3_IRQn);
		HAL_NVIC_SetPriorityGrouping(NVIC_PRIORITYGROUP_4);
		HAL_NVIC_SetPriority(DMA2_Stream6_IRQn, 8, 0);
		HAL_NVIC_EnableIRQ(DMA2_Stream6_IRQn);
	}
}

void sdTmpRead(uint8_t* data, uint32_t lba, int sectors)
{
	BSP_SD_ReadBlocks_DMA((uint32_t*) data, lba * 512ll, 512, sectors);
}

void sdTmpWrite(uint8_t* data, uint32_t lba, int sectors)
{
	BSP_SD_WriteBlocks_DMA((uint32_t*) data, lba * 512ll, 512, sectors);
}

static void sdClear()
{
	sdCard.version = 0;
	sdCard.capacity = 0;
	memset(sdCard.csd, 0, sizeof(sdCard.csd));
	memset(sdCard.cid, 0, sizeof(sdCard.cid));
}

static int sdDoInit()
{
	int result = 0;

	sdClear();


	int8_t error = BSP_SD_Init();
	if (error == MSD_OK)
	{
		memcpy(sdCard.csd, &SDCardInfo.SD_csd, sizeof(sdCard.csd));
		memcpy(sdCard.cid, &SDCardInfo.SD_cid, sizeof(sdCard.cid));
		sdCard.capacity = SDCardInfo.CardCapacity / SD_SECTOR_SIZE;
		sdCard.dev.mediaState |= MEDIA_PRESENT | MEDIA_INITIALISED;
		result = 1;

		// SD Benchmark code
		// Currently 10MB/s in high-speed mode.
		#ifdef SD_BENCHMARK
		if (HAL_SD_HighSpeed(&hsd) == SD_OK) // normally done in mainLoop()
		{
			s2s_setFastClock();
		}

		while(1)
		{
			s2s_ledOn();
			// 100MB
			int maxSectors = MAX_SECTOR_SIZE / SD_SECTOR_SIZE;
			for (
				int i = 0;
				i < (100LL * 1024 * 1024 / (maxSectors * SD_SECTOR_SIZE));
				++i)
			{
				sdTmpRead(&scsiDev.data[0], 0, maxSectors);
			}
			s2s_ledOff();

			for(int i = 0; i < 10; ++i) s2s_delay_ms(1000);
		}
		#endif

		goto out;
	}

//bad:
	sdCard.dev.mediaState &= ~(MEDIA_PRESENT | MEDIA_INITIALISED);

	sdCard.capacity = 0;

out:
	s2s_ledOff();
	return result;
}

int sdInit()
{
	// Check if there's an SD card present.
	int result = 0;

	static int firstInit = 1;

	if (firstInit)
	{
		sdCard.dev.mediaState &= ~(MEDIA_PRESENT | MEDIA_INITIALISED);
		sdClear();
		sdInitDMA();
	}

	if (firstInit || (scsiDev.phase == BUS_FREE))
	{
		uint8_t cs = HAL_GPIO_ReadPin(nSD_CD_GPIO_Port, nSD_CD_Pin) ? 0 : 1;
		uint8_t wp = HAL_GPIO_ReadPin(nSD_WP_GPIO_Port, nSD_WP_Pin) ? 0 : 1;

		if (cs && !(sdCard.dev.mediaState & MEDIA_PRESENT))
		{
			s2s_ledOn();

			// Debounce
			if (!firstInit)
			{
				s2s_delay_ms(250);
			}

			if (sdDoInit())
			{
				sdCard.dev.mediaState |= MEDIA_PRESENT | MEDIA_INITIALISED;

				if (wp)
				{
					sdCard.dev.mediaState |= MEDIA_WP;
				}
				else
				{
					sdCard.dev.mediaState &= ~MEDIA_WP;
				}

				// Always "start" the device. Many systems (eg. Apple System 7)
				// won't respond properly to
				// LOGICAL_UNIT_NOT_READY_INITIALIZING_COMMAND_REQUIRED sense
				// code, even if they stopped it first with
				// START STOP UNIT command.
				sdCard.dev.mediaState |= MEDIA_STARTED;

				result = 1;

				s2s_ledOff();
			}
			else
			{
				for (int i = 0; i < 10; ++i)
				{
					// visual indicator of SD error
					s2s_ledOff();
					s2s_delay_ms(50);
					s2s_ledOn();
					s2s_delay_ms(50);
				}
				s2s_ledOff();
			}
		}
		else if (!cs && (sdCard.dev.mediaState & MEDIA_PRESENT))
		{
			sdCard.capacity = 0;
			sdCard.dev.mediaState &= ~MEDIA_PRESENT;
			sdCard.dev.mediaState &= ~MEDIA_INITIALISED;
			int i;
			for (i = 0; i < S2S_MAX_TARGETS; ++i)
			{
				sdCard.targets[i].state.unitAttention = PARAMETERS_CHANGED;
			}

			HAL_SD_DeInit(&hsd);
		}
	}
	firstInit = 0;

	return result;
}

static void sd_earlyInit(S2S_Device* dev)
{
	SdCard* sdCardDevice = (SdCard*)dev;

	for (int i = 0; i < S2S_MAX_TARGETS; ++i)
	{
		sdCardDevice->targets[i].device = dev;
		sdCardDevice->targets[i].cfg = (S2S_TargetCfg*)
			(&(sdCardDevice->cfg[0]) + sizeof(S2S_BoardCfg) + (i * sizeof(S2S_TargetCfg)));
	}
	sdCardDevice->lastPollMediaTime = s2s_getTime_ms();

	// Don't require the host to send us a START STOP UNIT command
	sdCardDevice->dev.mediaState = MEDIA_STARTED;

}

static const S2S_BoardCfg* sd_getBoardConfig(S2S_Device* dev)
{
	SdCard* sdCardDevice = (SdCard*)dev;

	if (memcmp(__fixed_config, "BCFG", 4) == 0)
	{
		// Use hardcoded config if available. Hardcoded config always refers
		// to the SD card.
		memcpy(&(sdCardDevice->cfg[0]), __fixed_config, S2S_CFG_SIZE);
		return (S2S_BoardCfg*) &(sdCardDevice->cfg[0]);
	}
	else if ((sdCard.dev.mediaState & MEDIA_PRESENT) && sdCardDevice->capacity)
	{
		int cfgSectors = (S2S_CFG_SIZE + 511) / 512;
		BSP_SD_ReadBlocks_DMA(
			(uint32_t*) &(sdCardDevice->cfg[0]),
			(sdCardDevice->capacity - cfgSectors) * 512ll,
			512,
			cfgSectors);

		S2S_BoardCfg* cfg = (S2S_BoardCfg*) &(sdCardDevice->cfg[0]);
		if (memcmp(cfg->magic, "BCFG", 4))
		{
			// Set a default Target disk config
			memcpy(
				&(sdCardDevice->cfg[0]) + sizeof(S2S_BoardCfg),
				DEFAULT_TARGET_CONFIG,
				sizeof(DEFAULT_TARGET_CONFIG));

			return NULL;
		}
		else
		{
			return cfg;
		}
	}

	return NULL;
}

static S2S_Target* sd_getTargets(S2S_Device* dev, int* count)
{
	SdCard* sdCardDevice = (SdCard*)dev;
	*count = S2S_MAX_TARGETS;
	return sdCardDevice->targets;
}

static uint32_t sd_getCapacity(S2S_Device* dev)
{
	SdCard* sdCardDevice = (SdCard*)dev;
	return sdCardDevice->capacity;
}

static int sd_pollMediaChange(S2S_Device* dev)
{
	SdCard* sdCardDevice = (SdCard*)dev;
	if (s2s_elapsedTime_ms(sdCardDevice->lastPollMediaTime) > 200)
	{
		sdCardDevice->lastPollMediaTime = s2s_getTime_ms();
		return sdInit();
	}
	else
	{
		return 0;
	}
}

static void sd_saveConfig(S2S_Target* target)
{
	SdCard* sdCardDevice = (SdCard*)target->device;
	target->cfg->bytesPerSector = target->state.bytesPerSector;

	BSP_SD_WriteBlocks_DMA(
		(uint32_t*) (&(sdCardDevice->cfg[0])),
		(sdCardDevice->capacity - S2S_CFG_SIZE) * 512ll,
		512,
		(S2S_CFG_SIZE + 511) / 512);
}
