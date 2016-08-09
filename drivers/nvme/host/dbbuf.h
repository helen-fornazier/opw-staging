/*
 * NVM Express device driver
 * Copyright (C) 2015-2017, Google, Inc.
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

#ifndef _NVME_DBBUF_H
#define _NVME_DBBUF_H

#define SQ_IDX(qid, stride) ((qid) * 2 * (stride))
#define CQ_IDX(qid, stride) (((qid) * 2 + 1) * (stride))

#ifdef CONFIG_NVME_DBBUF

struct nvme_dbbuf_dev {
	u32 *db_mem;
	dma_addr_t doorbell;
	u32 *ei_mem;
	dma_addr_t eventidx;
};

struct nvme_dbbuf_queue {
	u32 *sq_doorbell_addr;
	u32 *sq_eventidx_addr;
	u32 *cq_doorbell_addr;
	u32 *cq_eventidx_addr;
};

int nvme_dma_alloc_dbbuf(struct device *dev,
			 struct nvme_dbbuf_dev *dbbuf_d,
			 u32 stride);

void nvme_dma_free_dbbuf(struct device *dev,
			 struct nvme_dbbuf_dev *dbbuf_d,
			 u32 stride);

void nvme_init_dbbuf(struct nvme_dbbuf_dev *dbbuf_d,
		     struct nvme_dbbuf_queue *dbbuf_q,
		     int qid, u32 stride);

void nvme_set_dbbuf(struct device *dev,
		    struct nvme_dbbuf_dev *dbbuf_d,
		    struct nvme_ctrl *ctrl,
		    u32 stride);

void nvme_write_doorbell(u16 value,
			 u32 __iomem *q_db,
			 u32 *db_addr,
			 volatile u32 *event_idx);

static inline void nvme_write_doorbell_cq(struct nvme_dbbuf_queue *dbbuf_q,
					  u16 value, u32 __iomem *q_db)
{
	nvme_write_doorbell(value, q_db,
			    dbbuf_q->cq_doorbell_addr,
			    dbbuf_q->cq_eventidx_addr);
}

static inline void nvme_write_doorbell_sq(struct nvme_dbbuf_queue *dbbuf_q,
					  u16 value, u32 __iomem *q_db)
{
	nvme_write_doorbell(value, q_db,
			    dbbuf_q->sq_doorbell_addr,
			    dbbuf_q->sq_eventidx_addr);
}

#else /* CONFIG_NVME_DBBUF */

struct nvme_dbbuf_dev {};

struct nvme_dbbuf_queue {};

static inline int nvme_dma_alloc_dbbuf(struct device *dev,
				       struct nvme_dbbuf_dev *dbbuf_d,
				       u32 stride)
{
	return 0;
}

static inline void nvme_dma_free_dbbuf(struct device *dev,
				       struct nvme_dbbuf_dev *dbbuf_d,
				       u32 stride)
{}

static inline void nvme_set_dbbuf(struct device *dev,
				  struct nvme_dbbuf_dev *dbbuf_d,
				  struct nvme_ctrl *ctrl,
				  u32 stride)
{}

static inline void nvme_init_dbbuf(struct nvme_dbbuf_dev *dbbuf_d,
				   struct nvme_dbbuf_queue *dbbuf_q,
				   int qid, u32 stride)
{}

static inline void nvme_write_doorbell_cq(struct nvme_dbbuf_queue *dbbuf_q,
					  u16 value, u32 __iomem *q_db)
{
	writel(value, q_db);
}

static inline void nvme_write_doorbell_sq(struct nvme_dbbuf_queue *dbbuf_q,
					  u16 value, u32 __iomem *q_db)
{
	writel(value, q_db);
}

#endif /* CONFIG_NVME_DBBUF */

#endif /* _NVME_DBBUF_H */
