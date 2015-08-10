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

#include <linux/configfs.h>
#include <linux/module.h>
#include <linux/slab.h>

#include "vimc-configfs.h"

#define VIMC_CFG_ROLE_SENSOR_STR	"sensor"
#define VIMC_CFG_ROLE_CAPTURE_STR	"capture"
#define VIMC_CFG_ROLE_INPUT_STR		"input"
#define VIMC_CFG_ROLE_DEBAYER_STR	"debayer"
#define VIMC_CFG_ROLE_SCALER_STR	"scaler"

enum vimc_cfg_status {
	VIMC_CFG_DEPLOYED,
	VIMC_CFG_UNDEPLOYED,
	VIMC_CFG_ERROR,
};

/* Structure which describes a pad within a entity */
struct vimc_cfg_pad {
	struct config_group group;
	struct list_head list;

	int direction;
};

/* Structure which describes individual configuration for each entity */
struct vimc_cfg_ent {
	struct config_group group;
	struct list_head list;
	struct list_head pads;
	size_t num_pads;
	enum vimc_cfg_role role;
	const char *name;
};

/* Structure which describes links between entities */
struct vimc_cfg_link {
	struct config_item item;
	struct list_head list;
	unsigned long flags;
	struct vimc_cfg_pad *src_pad;
	struct vimc_cfg_pad *sink_pad;
};

/* Structure which describes the whole topology */
struct vimc_cfg_pipeline {
	/* List of vimc_cfg_ent structures */
	struct list_head ents;
	/* List of vimc_cfg_link structures */
	struct list_head links;
};

/* Structure which describes a platform device instance */
struct vimc_cfg_pdev {
	struct config_group group;
	struct vimc_cfg_pipeline pipe;
	struct platform_device pdev;
};

/* Struct which describes the subsystem */
struct vimc_cfg_subsys {
	struct configfs_subsystem subsys;
	unsigned int pdev_id;
};

/* --------------------------------------------------------------------------
 * Link
 */

static struct configfs_attribute vimc_cfg_lnk_attr_flags = {
	.ca_owner = THIS_MODULE,
	.ca_name = "flags",
	.ca_mode = S_IRUGO,
};

static int vimc_cfg_lnk_allow_link(struct config_item *src,
				   struct config_item *target)
{
	pr_info("Link %s -> %s\n", src->ci_name, target->ci_name);
	return 0;
}

static ssize_t vimc_cfg_lnk_show_attr(struct config_item *item,
				      struct configfs_attribute *attr,
				      char *page)
{
	/* TODO: take the according action dependending on the attribute being
	 * shown */
	return sprintf(page, attr->ca_name);
}

static void vimc_cfg_lnk_release_item(struct config_item *item)
{
	struct vimc_cfg_link *link = container_of(item,
						  struct vimc_cfg_link, item);

	/* Remove link item from the dev list */
	list_del(&link->list);

	kfree(link);
}

static struct configfs_item_operations vimc_cfg_lnk_item_ops = {
	.show_attribute	= vimc_cfg_lnk_show_attr,
	.allow_link	= vimc_cfg_lnk_allow_link,
	.release	= vimc_cfg_lnk_release_item,
};

static struct configfs_attribute *vimc_cfg_lnk_attrs[] = {
	&vimc_cfg_lnk_attr_flags,
	NULL,
};

static struct config_item_type vimc_cfg_lnk_type = {
	.ct_item_ops	= &vimc_cfg_lnk_item_ops,
	.ct_attrs	= vimc_cfg_lnk_attrs,
	.ct_owner	= THIS_MODULE,
};

static struct config_item *vimc_cfg_lnk_make_item(struct config_group *group,
						  const char *name)
{
	struct vimc_cfg_link *link;
	struct vimc_cfg_ent *ent;
	struct vimc_cfg_pad *pad;
	struct vimc_cfg_pdev *pd;

	link = kzalloc(sizeof(struct vimc_cfg_link), GFP_KERNEL);
	if (!link)
		return ERR_PTR(-ENOMEM);

	config_item_init_type_name(&link->item, name, &vimc_cfg_lnk_type);

	/* Save parent pad as a source pad */
	pad = container_of(group, struct vimc_cfg_pad, group);
	link->src_pad = pad;

	/* Add item to the dev list */
	ent = container_of(pad->group.cg_item.ci_parent,
			   struct vimc_cfg_ent, group.cg_item);
	pd = container_of(ent->group.cg_item.ci_parent,
			   struct vimc_cfg_pdev, group.cg_item);
	list_add_tail(&link->list, &pd->pipe.links);

	return &link->item;
}

/* --------------------------------------------------------------------------
 * Pad
 */

static struct configfs_attribute vimc_cfg_pad_attr_dir = {
	.ca_owner = THIS_MODULE,
	.ca_name = "direction",
	.ca_mode = S_IRUGO,
};

static int vimc_cfg_pad_allow_link(struct config_item *src,
				   struct config_item *target)
{
	pr_info("Link %s -> %s\n", src->ci_name, target->ci_name);
	return 0;
}

static ssize_t vimc_cfg_pad_show_attr(struct config_item *item,
				      struct configfs_attribute *attr,
				      char *page)
{
	/* TODO: take the according action dependending on the attribute being
	 * shown */
	return sprintf(page, attr->ca_name);
}

static void vimc_cfg_pad_release_item(struct config_item *item)
{
	struct vimc_cfg_pad *pad = container_of(item,
						struct vimc_cfg_pad,
						group.cg_item);

	/* Remove this entity from the parent's list */
	list_del(&pad->list);

	kfree(pad);
}

static struct configfs_item_operations vimc_cfg_pad_item_ops = {
	.show_attribute	= vimc_cfg_pad_show_attr,
	.allow_link	= vimc_cfg_pad_allow_link,
	.release	= vimc_cfg_pad_release_item,
};

static struct configfs_attribute *vimc_cfg_pad_attrs[] = {
	&vimc_cfg_pad_attr_dir,
	NULL,
};

static struct configfs_group_operations vimc_cfg_pad_group_ops = {
	/* Create link children */
	.make_item	= vimc_cfg_lnk_make_item,
};

static struct config_item_type vimc_cfg_pad_type = {
	.ct_item_ops	= &vimc_cfg_pad_item_ops,
	.ct_group_ops	= &vimc_cfg_pad_group_ops,
	.ct_attrs	= vimc_cfg_pad_attrs,
	.ct_owner	= THIS_MODULE,
};

static struct config_group *vimc_cfg_pad_make_group(struct config_group *group,
						    const char *name)
{
	struct vimc_cfg_pad *pad;
	struct vimc_cfg_ent *ent;

	pad = kzalloc(sizeof(struct vimc_cfg_pad), GFP_KERNEL);
	if (!pad)
		return ERR_PTR(-ENOMEM);

	config_group_init_type_name(&pad->group, name, &vimc_cfg_pad_type);

	/* Add this entity in the parent's list */
	ent = container_of(group, struct vimc_cfg_ent, group);
	list_add_tail(&pad->list, &ent->pads);
	ent->num_pads++;

	return &pad->group;
}

/* --------------------------------------------------------------------------
 * Entity
 */

static struct configfs_attribute vimc_cfg_ent_attr_name = {
	.ca_owner = THIS_MODULE,
	.ca_name = "name",
	.ca_mode = S_IRUGO,
};

static struct configfs_attribute vimc_cfg_ent_attr_role = {
	.ca_owner = THIS_MODULE,
	.ca_name = "role",
	.ca_mode = S_IRUGO,
};

static ssize_t vimc_cfg_ent_show_attr(struct config_item *item,
				      struct configfs_attribute *attr,
				      char *page)
{
	return sprintf(page, attr->ca_name);
}

static void vimc_cfg_ent_release_item(struct config_item *item)
{
	struct vimc_cfg_ent *ent = container_of(item,
						struct vimc_cfg_ent,
						group.cg_item);

	pr_info("ent_release: ent %p\n", ent);
	/* Remove this entity from the parent's list */
	list_del(&ent->list);

	kfree(ent);
}

static struct configfs_item_operations vimc_cfg_ent_item_ops = {
	.show_attribute	= vimc_cfg_ent_show_attr,
	.release	= vimc_cfg_ent_release_item,
};

static struct configfs_attribute *vimc_cfg_ent_attrs[] = {
	&vimc_cfg_ent_attr_name,
	&vimc_cfg_ent_attr_role,
	NULL,
};

static struct configfs_group_operations vimc_cfg_ent_group_ops = {
	/* Create pad children */
	.make_group	= vimc_cfg_pad_make_group,
};

static struct config_item_type vimc_cfg_ent_type = {
	.ct_item_ops	= &vimc_cfg_ent_item_ops,
	.ct_group_ops	= &vimc_cfg_ent_group_ops,
	.ct_attrs	= vimc_cfg_ent_attrs,
	.ct_owner	= THIS_MODULE,
};

static struct config_group *vimc_cfg_ent_make_group(struct config_group *group,
						    const char *name)
{
	struct vimc_cfg_pdev *pd;
	struct vimc_cfg_ent *ent;

	ent = kzalloc(sizeof(struct vimc_cfg_ent), GFP_KERNEL);
	if (!ent)
		return ERR_PTR(-ENOMEM);

	pr_info("ent_make_group: ent %p\n", ent);

	INIT_LIST_HEAD(&ent->pads);
	config_group_init_type_name(&ent->group, name, &vimc_cfg_ent_type);

	/* Add this entity in the parent's list */
	pd = container_of(group, struct vimc_cfg_pdev, group);
	pr_info("ent_make_group: pdev %p\n", pd);
	pr_info("ent_make_group: list %p prev %p next %p\n", &pd->pipe.ents,
		pd->pipe.ents.prev, pd->pipe.ents.next);

	pr_info("ent_make_group: ent_list %p prev %p next %p\n", &ent->list,
		ent->list.prev, ent->list.next);
	list_add_tail(&ent->list, &pd->pipe.ents);

	return &ent->group;
}

/* --------------------------------------------------------------------------
 * Device instance
 */

static struct configfs_attribute vimc_cfg_pdev_attr_deploy = {
	.ca_owner = THIS_MODULE,
	.ca_name = "deploy",
	.ca_mode = S_IRUGO,
};

static void vimc_cfg_pdev_release_item(struct config_item *item)
{
	struct vimc_cfg_pdev *cfg_pdev = container_of(item,
						      struct vimc_cfg_pdev,
						      group.cg_item);

	/* TODO: check how we prohibit rmdir if the dev is in use */
	vimc_pdev_unregister(&cfg_pdev->pdev);
	/* Check if we can free the pdev here */
	kfree(cfg_pdev);
}

static ssize_t vimc_cfg_pdev_store_attr(struct config_item *item,
					struct configfs_attribute *attr,
					const char *page, size_t count)
{
	/* TODO */
	return 0;
}

static ssize_t vimc_cfg_pdev_show_attr(struct config_item *item,
				       struct configfs_attribute *attr,
				       char *page)
{
	/* TODO */
	return sprintf(page, attr->ca_name);
}

static struct configfs_item_operations vimc_cfg_pdev_item_ops = {
	.show_attribute		= vimc_cfg_pdev_show_attr,
	.store_attribute	= vimc_cfg_pdev_store_attr,
	.release		= vimc_cfg_pdev_release_item,
};

static struct configfs_attribute *vimc_cfg_pdev_attrs[] = {
	&vimc_cfg_pdev_attr_deploy,
	NULL,
};

static struct configfs_group_operations vimc_cfg_pdev_group_ops = {
	/* Create pad children */
	.make_group	= vimc_cfg_ent_make_group,
};

static struct config_item_type vimc_cfg_pdev_type = {
	.ct_item_ops	= &vimc_cfg_pdev_item_ops,
	.ct_group_ops	= &vimc_cfg_pdev_group_ops,
	.ct_attrs	= vimc_cfg_pdev_attrs,
	.ct_owner	= THIS_MODULE,
};

static void vimc_cfg_pdev_release(struct device *dev)
{}

static struct config_group *vimc_cfg_pdev_make_group(struct config_group *group,
						    const char *name)
{
	struct vimc_cfg_subsys *cfg_subsys;
	struct vimc_cfg_pdev *pd;
	int ret;

	pd = kzalloc(sizeof(struct vimc_cfg_pdev), GFP_KERNEL);
	if (!pd)
		return ERR_PTR(-ENOMEM);

	pr_info("pdev_make_group: pdev %p\n", pd);

	/* Get the parent subsystem */
	cfg_subsys = container_of(group->cg_subsys,
				  struct vimc_cfg_subsys, subsys);

	pd->pdev.name = group->cg_item.ci_name;
	pd->pdev.dev.release = vimc_cfg_pdev_release;
	/* TODO: release the ids? Or can we assume unsigned
	 * int is big enough? */
	pd->pdev.id = cfg_subsys->pdev_id++;
	ret = vimc_pdev_register(&pd->pdev);
	if (ret) {
		kfree(pd);
		return ERR_PTR(ret);
	}

	INIT_LIST_HEAD(&pd->pipe.ents);
	INIT_LIST_HEAD(&pd->pipe.links);

	pr_info("pdev_make_group: list %p prev %p next %p\n", &pd->pipe.ents,
		pd->pipe.ents.prev, pd->pipe.ents.next);

	config_group_init_type_name(&pd->group, name, &vimc_cfg_pdev_type);

	return &pd->group;
}

/* --------------------------------------------------------------------------
 * Subsystem
 */

static struct configfs_group_operations vimc_cfg_subsys_group_ops = {
	/* Create entity children */
	.make_group	= vimc_cfg_pdev_make_group,
};

static struct config_item_type vimc_cfg_subsys_type = {
	.ct_group_ops	= &vimc_cfg_subsys_group_ops,
	.ct_owner	= THIS_MODULE,
};

static struct vimc_cfg_subsys vimc_cfg_subsys = {
	.subsys = {
		.su_group = {
			.cg_item = {
				.ci_type = &vimc_cfg_subsys_type,
			},
		},
	},
};

/* --------------------------------------------------------------------------
 * Functions
 */

int vimc_cfg_register(char *name)
{
	struct configfs_subsystem *subsys;
	int ret;

	BUG_ON(!name);

	subsys = &vimc_cfg_subsys.subsys;
	subsys->su_group.cg_item.ci_name = name;
	config_group_init(&subsys->su_group);
	mutex_init(&subsys->su_mutex);
	ret = configfs_register_subsystem(subsys);

	return ret;
}

void vimc_cfg_unregister(void)
{
	configfs_unregister_subsystem(&vimc_cfg_subsys.subsys);
}
