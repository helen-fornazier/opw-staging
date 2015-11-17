/*
 * NVM Express device driver
 * Copyright (C) 2015-2016, Google, Inc.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 */

#include "nvme.h"
#include "vdb.h"

static inline unsigned int nvme_doorbell_memory_size(u32 stride)
{
	return ((num_possible_cpus() + 1) * 8 * stride);
}

int nvme_dma_alloc_doorbell_mem(struct device *dev,
				struct nvme_vdb_dev *vdb_d,
				u32 stride)
{
	unsigned int mem_size = nvme_doorbell_memory_size(stride);

	vdb_d->db_mem = dma_alloc_coherent(dev, mem_size, &vdb_d->doorbell,
					   GFP_KERNEL);
	if (!vdb_d->db_mem)
		return -ENOMEM;
	vdb_d->ei_mem = dma_alloc_coherent(dev, mem_size, &vdb_d->eventidx,
					   GFP_KERNEL);
	if (!vdb_d->ei_mem) {
		dma_free_coherent(dev, mem_size,
				  vdb_d->db_mem, vdb_d->doorbell);
		vdb_d->db_mem = NULL;
		return -ENOMEM;
	}

	return 0;
}

void nvme_dma_free_doorbell_mem(struct device *dev,
				struct nvme_vdb_dev *vdb_d,
				u32 stride)
{
	unsigned int mem_size = nvme_doorbell_memory_size(stride);

	if (vdb_d->db_mem) {
		dma_free_coherent(dev, mem_size,
				  vdb_d->db_mem, vdb_d->doorbell);
		vdb_d->db_mem = NULL;
	}
	if (vdb_d->ei_mem) {
		dma_free_coherent(dev, mem_size,
				  vdb_d->ei_mem, vdb_d->eventidx);
		vdb_d->ei_mem = NULL;
	}
}

void nvme_init_doorbell_mem(struct nvme_vdb_dev *vdb_d,
			    struct nvme_vdb_queue *vdb_q,
			    int qid, u32 stride)
{
	if (!vdb_d->db_mem || !qid)
		return;

	vdb_q->sq_doorbell_addr = &vdb_d->db_mem[SQ_IDX(qid, stride)];
	vdb_q->cq_doorbell_addr = &vdb_d->db_mem[CQ_IDX(qid, stride)];
	vdb_q->sq_eventidx_addr = &vdb_d->ei_mem[SQ_IDX(qid, stride)];
	vdb_q->cq_eventidx_addr = &vdb_d->ei_mem[CQ_IDX(qid, stride)];
}

void nvme_set_doorbell_memory(struct device *dev,
			      struct nvme_vdb_dev *vdb_d,
			      struct nvme_ctrl *ctrl,
			      u32 stride)
{
	struct nvme_command c;

	if (!vdb_d->db_mem)
		return;

	memset(&c, 0, sizeof(c));
	c.doorbell_memory.opcode = nvme_admin_doorbell_memory;
	c.doorbell_memory.prp1 = cpu_to_le64(vdb_d->doorbell);
	c.doorbell_memory.prp2 = cpu_to_le64(vdb_d->eventidx);

	if (nvme_submit_sync_cmd(ctrl->admin_q, &c, NULL, 0))
		/* Free memory and continue on */
		nvme_dma_free_doorbell_mem(dev, vdb_d, stride);
}

static inline int nvme_ext_need_event(u16 event_idx, u16 new_idx, u16 old)
{
	/* Borrowed from vring_need_event */
	return (u16)(new_idx - event_idx - 1) < (u16)(new_idx - old);
}

void nvme_write_doorbell(u16 value,
			 u32 __iomem *q_db,
			 u32 *db_addr,
			 volatile u32 *event_idx)
{
	u16 old_value;

	if (!db_addr) {
		writel(value, q_db);
		return;
	}

	/*
	 * Ensure that the queue is written before updating
	 * the doorbell in memory
	 */
	wmb();

	old_value = *db_addr;
	*db_addr = value;

	if (nvme_ext_need_event(*event_idx, value, old_value))
		writel(value, q_db);
}
