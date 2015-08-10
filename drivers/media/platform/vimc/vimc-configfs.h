/*
 * vimc-configfs.h Virtual Media Controller Driver
 *
 * Copyright (C) 2018 Helen Fornazier <helen.fornazier@gmail.com>
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

#include <linux/configfs.h>

#define VIMC_CFS_SRC_PAD_NAME(n) "pad:source:" #n
#define VIMC_CFS_SINK_PAD_NAME(n) "pad:sink:" #n

struct vimc_cfs_drv {
	const char *name;
	void (*const configfs_cb)(struct config_group *group);
	struct list_head list;
};

int vimc_cfs_subsys_register(char *drv_name);

void vimc_cfs_subsys_unregister(void);

void vimc_cfs_drv_register(struct vimc_cfs_drv *c_drv);

void vimc_cfs_drv_unregister(struct vimc_cfs_drv *c_drv);

#endif
