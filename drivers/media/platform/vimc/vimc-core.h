/*
 * vimc-core.h Virtual Media Controller Driver
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

#ifndef _VIMC_CORE_H_
#define _VIMC_CORE_H_

#define VIMC_CORE_PDEV_NAME "vimc-core"

int vimc_core_comp_bind(struct device *master);

void vimc_core_comp_unbind(struct device *master);

#endif
