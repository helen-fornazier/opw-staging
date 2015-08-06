/*
 * vimc-core.h Virtual Media Controller Driver
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

#ifndef _VIMC_CORE_H_
#define _VIMC_CORE_H_

#include <media/v4l2-device.h>

/* Struct which matches the MEDIA_BUS_FMT_ codes with the corresponding
 * V4L2_PIX_FMT_ fourcc pixelformat and its bytes per pixel (bpp) */
struct vimc_pix_map {
	unsigned int code;
	unsigned int bpp;
	u32 pixelformat;
};
extern const struct vimc_pix_map vimc_pix_map_list[];

struct vimc_ent_device {
	struct media_entity *ent;
	struct media_pad *pads;
	void (*destroy)(struct vimc_ent_device *);
	void (*process_frame)(struct vimc_ent_device *ved,
			      struct media_pad *sink, const void *frame);
};

int vimc_propagate_frame(struct device *dev,
			 struct media_pad *src, const void *frame);

/* Helper functions to allocate/initialize pads and free them */
struct media_pad *vimc_pads_init(u16 num_pads,
				 const unsigned long *pads_flag);
static inline void vimc_pads_cleanup(struct media_pad *pads)
{
	kfree(pads);
}

const struct vimc_pix_map *vimc_pix_map_by_code(u32 code);

const struct vimc_pix_map *vimc_pix_map_by_pixelformat(u32 pixelformat);

#endif
