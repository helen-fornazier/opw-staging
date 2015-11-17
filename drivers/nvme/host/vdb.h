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

#ifndef _NVME_VDB_H
#define _NVME_VDB_H

#ifdef CONFIG_NVME_VDB

#define SQ_IDX(qid, stride) ((qid) * 2 * (stride))
#define CQ_IDX(qid, stride) (((qid) * 2 + 1) * (stride))

struct nvme_vdb_dev {
	u32 *db_mem;
	dma_addr_t doorbell;
	u32 *ei_mem;
	dma_addr_t eventidx;
};

struct nvme_vdb_queue {
	u32 *sq_doorbell_addr;
	u32 *sq_eventidx_addr;
	u32 *cq_doorbell_addr;
	u32 *cq_eventidx_addr;
};

int nvme_dma_alloc_doorbell_mem(struct device *dev,
				struct nvme_vdb_dev *vdb_d,
				u32 stride);

void nvme_dma_free_doorbell_mem(struct device *dev,
				struct nvme_vdb_dev *vdb_d,
				u32 stride);

void nvme_init_doorbell_mem(struct nvme_vdb_dev *vdb_d,
			    struct nvme_vdb_queue *vdb_q,
			    int qid, u32 stride);

void nvme_set_doorbell_memory(struct device *dev,
			      struct nvme_vdb_dev *vdb_d,
			      struct nvme_ctrl *ctrl,
			      u32 stride);

void nvme_write_doorbell(u16 value,
			 u32 __iomem *q_db,
			 u32 *db_addr,
			 volatile u32 *event_idx);

static inline void nvme_write_doorbell_cq(struct nvme_vdb_queue *vdb_q,
					  u16 value, u32 __iomem *q_db)
{
	nvme_write_doorbell(value, q_db,
			    vdb_q->cq_doorbell_addr,
			    vdb_q->cq_eventidx_addr);
}

static inline void nvme_write_doorbell_sq(struct nvme_vdb_queue *vdb_q,
					  u16 value, u32 __iomem *q_db)
{
	nvme_write_doorbell(value, q_db,
			    vdb_q->sq_doorbell_addr,
			    vdb_q->sq_eventidx_addr);
}

#else /* CONFIG_NVME_VDB */

struct nvme_vdb_dev {};

struct nvme_vdb_queue {};

static inline int nvme_dma_alloc_doorbell_mem(struct device *dev,
					      struct nvme_vdb_dev *vdb_d,
					      u32 stride)
{
	return 0;
}

static inline void nvme_dma_free_doorbell_mem(struct device *dev,
					      struct nvme_vdb_dev *vdb_d,
					      u32 stride)
{}

static inline void nvme_set_doorbell_memory(struct device *dev,
					    struct nvme_vdb_dev *vdb_d,
					    struct nvme_ctrl *ctrl,
					    u32 stride)
{}

static inline void nvme_init_doorbell_mem(struct nvme_vdb_dev *vdb_d,
					  struct nvme_vdb_queue *vdb_q,
					  int qid, u32 stride)
{}

static inline void nvme_write_doorbell_cq(struct nvme_vdb_queue *vdb_q,
					  u16 value, u32 __iomem *q_db)
{
	writel(value, q_db);
}

static inline void nvme_write_doorbell_sq(struct nvme_vdb_queue *vdb_q,
					  u16 value, u32 __iomem *q_db)
{
	writel(value, q_db);
}

#endif /* CONFIG_NVME_VDB */

#endif /* _NVME_VDB_H */
