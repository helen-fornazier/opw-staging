/*
 * vimc-configfs.h Virtual Media Controller Driver
 *
 * Copyright (C) 2015 Helen Fornazier <helen.fornazier@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#ifndef _VIMC_CONFIGFS_H_
#define _VIMC_CONFIGFS_H_

#include "vimc-core.h"

/**
 * enum vimc_cfg_role - Select the functionality of a node in the topology
 * @VIMC_CFG_ROLE_SENSOR:	A node with role SENSOR simulates a camera sensor
 *				generating internal images in bayer format and
 *				propagating those images through the pipeline
 * @VIMC_CFG_ROLE_CAPTURE:	A node with role CAPTURE is a v4l2 video_device
 *				that exposes the received image from the
 *				pipeline to the user space
 * @VIMC_CFG_ROLE_INPUT:	A node with role INPUT is a v4l2 video_device that
 *				receives images from the user space and
 *				propagates them through the pipeline
 * @VIMC_CFG_ROLE_DEBAYER:	A node with role DEBAYER expects to receive a frame
 *				in bayer format converts it to RGB
 * @VIMC_CFG_ROLE_SCALER:	A node with role SCALER scales the received image
 *				by a given multiplier
 *
 * This enum is used in the entity configuration struct to allow the definition
 * of a custom topology specifying the role of each role on it.
 */
enum vimc_cfg_role {
	VIMC_CFG_ROLE_SENSOR,
	VIMC_CFG_ROLE_CAPTURE,
	VIMC_CFG_ROLE_INPUT,
	VIMC_CFG_ROLE_DEBAYER,
	VIMC_CFG_ROLE_SCALER,
};

int vimc_cfg_register(char *drv_name);

void vimc_cfg_unregister(void);

#endif
