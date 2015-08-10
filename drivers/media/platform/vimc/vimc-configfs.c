/*
 * vimc-configfs.h Virtual Media Controller Driver
 *
 * Copyright (C) 2018 Helen Mae Koike Fornazier <helen.fornazier@gmail.com>
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

#include <linux/component.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/platform_device.h>

#include "vimc-common.h"
#include "vimc-configfs.h"
#include "vimc-core.h"

#define CHAR_SEPARATOR ':'
#define LINK_SEPARATOR "->"

#define ci_err(ci, fmt, ...) \
	pr_err("vimc: %s: " pr_fmt(fmt), (ci)->ci_name, ##__VA_ARGS__)
#define cg_err(cg, ...) ci_err(&(cg)->cg_item, ##__VA_ARGS__)
#define ci_warn(ci, fmt, ...) \
	pr_warn("vimc: %s: " pr_fmt(fmt), (ci)->ci_name, ##__VA_ARGS__)
#define cg_warn(cg, ...) ci_warn(&(cg)->cg_item, ##__VA_ARGS__)
#define ci_dbg(ci, fmt, ...) \
	pr_debug("vimc: %s: " pr_fmt(fmt), (ci)->ci_name, ##__VA_ARGS__)
#define cg_dbg(cg, ...) ci_dbg(&(cg)->cg_item, ##__VA_ARGS__)

#define is_plugged(cfs) (!!(cfs)->pdev)

enum vimc_cfs_hotplug_state {
	VIMC_CFS_HOTPLUG_STATE_UNPLUGGED = 0,
	VIMC_CFS_HOTPLUG_STATE_PLUGGED = 1,
};

const static char *vimc_cfs_hotplug_values[2][3] = {
	[VIMC_CFS_HOTPLUG_STATE_UNPLUGGED] = {"unplugged\n", "unplug\n", "0\n"},
	[VIMC_CFS_HOTPLUG_STATE_PLUGGED] = {"plugged\n", "plug\n", "1\n"},
};

/* --------------------------------------------------------------------------
 * Pipeline structures
 */

static struct vimc_cfs_subsystem {
	struct configfs_subsystem subsys;
	struct list_head drvs;
} vimc_cfs_subsys;

/* Structure which describes the whole topology */
struct vimc_cfs_device {
	struct platform_device *pdev;
	struct vimc_platform_data_core pdata;
	struct list_head ents;
	struct list_head links;
	struct config_group gdev;
	struct config_group gents;
	struct config_group glinks;
};

/* Structure which describes individual configuration for each entity */
struct vimc_cfs_ent {
	struct platform_device *pdev;
	struct vimc_platform_data pdata;
	char drv[VIMC_MAX_NAME_LEN];
	struct list_head list;
	struct config_group cg;
};

/* Structure which describes links between entities */
struct vimc_cfs_link {
	struct vimc_platform_data_link pdata;
	char source_name[VIMC_MAX_NAME_LEN];
	char sink_name[VIMC_MAX_NAME_LEN];
	struct config_item ci;
};

void vimc_cfs_drv_register(struct vimc_cfs_drv *c_drv) {
	list_add(&c_drv->list, &vimc_cfs_subsys.drvs);
}
EXPORT_SYMBOL_GPL(vimc_cfs_drv_register);

void vimc_cfs_drv_unregister(struct vimc_cfs_drv *c_drv) {
	list_del(&c_drv->list);
}
EXPORT_SYMBOL_GPL(vimc_cfs_drv_unregister);

/* --------------------------------------------------------------------------
 * Platform Device builders
 */

static int vimc_cfs_link_get_entities(const struct vimc_cfs_device *cfs,
				       struct vimc_cfs_link *c_link)
{
	struct vimc_cfs_ent *c_ent;

	c_link->pdata.source = NULL;
	c_link->pdata.sink = NULL;
	list_for_each_entry(c_ent, &cfs->ents, list) {
		if (!c_link->pdata.source &&
		    !strcmp(c_ent->pdata.name, c_link->source_name))
			c_link->pdata.source = c_ent->pdev;
		if (!c_link->pdata.sink &&
		    !strcmp(c_ent->pdata.name, c_link->sink_name))
			c_link->pdata.sink = c_ent->pdev;
		if (c_link->pdata.source && c_link->pdata.sink) {
			return 0;
		}
	}
	return -EINVAL;
}

static int vimc_cfs_comp_compare(struct device *comp, void *data)
{
	dev_dbg(comp, "comp compare %p %p", comp, data);

	return comp == data;
}

static const struct component_master_ops vimc_cfs_comp_ops = {
	.bind = vimc_core_comp_bind,
	.unbind = vimc_core_comp_unbind,
};

static void vimc_cfs_device_unplug(struct vimc_cfs_device *cfs)
{
	struct vimc_cfs_ent *c_ent;

	dev_dbg(&cfs->pdev->dev, "Unplugging device");

	component_master_del(&cfs->pdev->dev, &vimc_cfs_comp_ops);
	list_for_each_entry(c_ent, &cfs->ents, list) {
		platform_device_unregister(c_ent->pdev);
		c_ent->pdev = NULL;
	}
	platform_device_unregister(cfs->pdev);
	cfs->pdev = NULL;
}

static int vimc_cfs_device_plug(struct vimc_cfs_device *cfs)
{
	struct component_match *match = NULL;
	struct vimc_cfs_ent *c_ent;
	struct vimc_cfs_link *c_link;
	int ret = 0;

	cg_dbg(&cfs->gdev, "Plugging device");

	if (list_empty(&cfs->ents)) {
		/* TODO: add support for a default topology */
		cg_err(&cfs->gdev,
			"At least an entity is required to plug the device");
		return -EINVAL;
	}

	cfs->pdev = platform_device_register_data(NULL, VIMC_CORE_PDEV_NAME,
						  PLATFORM_DEVID_AUTO,
						  &cfs->pdata,
						  sizeof(cfs->pdata));
	if (IS_ERR(cfs->pdev))
		return PTR_ERR(cfs->pdev);

	/* Add component_match for inner structure of the pipeline */
	list_for_each_entry(c_ent, &cfs->ents, list) {
		cg_dbg(&c_ent->cg, "registering entity %s:%s", c_ent->drv,
		       c_ent->pdata.name);
		if (c_ent->pdev)
			cg_err(&c_ent->cg, "pdev is not null");
		c_ent->pdev = platform_device_register_data(&cfs->pdev->dev,
							c_ent->drv,
							PLATFORM_DEVID_AUTO,
							&c_ent->pdata,
							sizeof(c_ent->pdata));
		if (IS_ERR(c_ent->pdev)) {
			ret = PTR_ERR(c_ent->pdev);
			goto unregister_ents;
		}
		component_match_add(&cfs->pdev->dev, &match,
				    vimc_cfs_comp_compare, &c_ent->pdev->dev);
	}
	list_for_each_entry(c_link, cfs->pdata.links, pdata.list) {
		ret = vimc_cfs_link_get_entities(cfs, c_link);
		if (ret) {
			ci_err(&c_link->ci, "could not validate link");
			goto unregister_ents;
		}
	}

	dev_dbg(&cfs->pdev->dev, "Adding master device");
	ret = component_master_add_with_match(&cfs->pdev->dev,
					      &vimc_cfs_comp_ops, match);
	if (ret)
		goto unregister_ents;

	return 0;

unregister_ents:
	list_for_each_entry_continue_reverse(c_ent, &cfs->ents, list) {
		platform_device_unregister(c_ent->pdev);
		c_ent->pdev = NULL;
	}

	platform_device_unregister(cfs->pdev);
	cfs->pdev = NULL;

	return ret;
}

/* --------------------------------------------------------------------------
 * Links
 */

static ssize_t vimc_cfs_links_attr_flags_show(struct config_item *item,
					      char *buf)
{
	struct vimc_cfs_link *c_link = container_of(item, struct vimc_cfs_link,
						    ci);

	sprintf(buf, "%d\n", c_link->pdata.flags);
	return strlen(buf);
}

static ssize_t vimc_cfs_links_attr_flags_store(struct config_item *item,
					       const char *buf, size_t size)
{
	struct vimc_cfs_link *c_link = container_of(item, struct vimc_cfs_link,
						    ci);

	if (kstrtou32(buf, 0, &c_link->pdata.flags))
		return -EINVAL;

	return size;
}

CONFIGFS_ATTR(vimc_cfs_links_attr_, flags);

static struct configfs_attribute *vimc_cfs_link_attrs[] = {
	&vimc_cfs_links_attr_attr_flags,
	NULL,
};

static void vimc_cfs_link_release(struct config_item *item)
{
	struct vimc_cfs_link *c_link = container_of(item, struct vimc_cfs_link,
						    ci);

	kfree(c_link);
}

static struct configfs_item_operations vimc_cfs_link_item_ops = {
	.release	= vimc_cfs_link_release,
};

static struct config_item_type vimc_cfs_link_type = {
	.ct_item_ops	= &vimc_cfs_link_item_ops,
	.ct_attrs	= vimc_cfs_link_attrs,
	.ct_owner	= THIS_MODULE,
};

static void vimc_cfs_link_drop_item(struct config_group *group,
				    struct config_item *item)
{
	struct vimc_cfs_link *link = container_of(item,
						  struct vimc_cfs_link, ci);
	struct vimc_cfs_device *cfs = container_of(group,
						   struct vimc_cfs_device,
						   glinks);

	if (is_plugged(cfs))
		vimc_cfs_device_unplug(cfs);
	list_del(&link->pdata.list);
}

static struct config_item *vimc_cfs_link_make_item(struct config_group *group,
						   const char *name)
{
	struct vimc_cfs_device *cfs = container_of(group,
						   struct vimc_cfs_device,
						   glinks);
	size_t src_pad_strlen, sink_pad_strlen, sink_namelen, source_namelen;
	const char *sep, *src_pad_str, *sink_pad_str, *sink_name,
	      *source_name = name;
	struct vimc_cfs_link *c_link;
	u16 source_pad, sink_pad;
	char tmp[4];

	cg_dbg(&cfs->gdev, "Creating link %s", name);

	if (is_plugged(cfs))
		vimc_cfs_device_unplug(cfs);

	/* Parse format "source_name:source_pad->sink_name:sink_pad" */
	sep = strchr(source_name, CHAR_SEPARATOR);
	if (!sep)
		goto syntax_error;
	source_namelen = (size_t)(sep - source_name);

	src_pad_str = &sep[1];
	sep = strstr(src_pad_str, LINK_SEPARATOR);
	if (!sep)
		goto syntax_error;
	src_pad_strlen = (size_t)(sep - src_pad_str);

	sink_name = &sep[strlen(LINK_SEPARATOR)];
	sep = strchr(sink_name, CHAR_SEPARATOR);
	if (!sep)
		goto syntax_error;
	sink_namelen = (size_t)(sep - sink_name);

	sink_pad_str = &sep[1];
	sink_pad_strlen = strlen(sink_pad_str);

	/* Validate sizes */
	if (!src_pad_strlen || !sink_pad_strlen ||
	    !sink_namelen || !source_namelen)
		goto syntax_error;

	/* we limit the size here so we don't need to allocate another buffer */
	if (src_pad_strlen >= sizeof(tmp) || sink_pad_strlen >= sizeof(tmp)) {
		cg_err(&cfs->gdev,
		       "Pad with more then %ld digits is not suported",
		       sizeof(tmp) - 1);
		goto syntax_error;
	}
	strscpy(tmp, src_pad_str, src_pad_strlen + 1);
	if (kstrtou16(tmp, 0, &source_pad)) {
		cg_err(&cfs->gdev, "Couldn't convert pad %s to number", tmp);
		goto syntax_error;
	}
	strscpy(tmp, sink_pad_str, sink_pad_strlen + 1);
	if (kstrtou16(tmp, 0, &sink_pad)) {
		cg_err(&cfs->gdev, "Couldn't convert pad %s to number", tmp);
		goto syntax_error;
	}

	c_link = kzalloc(sizeof(*c_link), GFP_KERNEL);
	if (!c_link)
		return ERR_PTR(-ENOMEM);

	c_link->pdata.source_pad = source_pad;
	c_link->pdata.sink_pad = sink_pad;
	strscpy(c_link->source_name, source_name, source_namelen + 1);
	strscpy(c_link->sink_name, sink_name, sink_namelen + 1);

	/* Configure group */
	list_add(&c_link->pdata.list, cfs->pdata.links);
	config_item_init_type_name(&c_link->ci, name, &vimc_cfs_link_type);

	return &c_link->ci;

syntax_error:
	cg_err(&cfs->gdev,
		"Couldn't create link %s, wrong syntax.", name);
	return ERR_PTR(-EINVAL);
}

/* --------------------------------------------------------------------------
 * Entities
 */

/* *TODO: add suport for hotplug in entity level */

static int vimc_cfs_drv_cb(const char *drv_name, struct config_group *group) {
	struct vimc_cfs_drv *c_drv = NULL;

	list_for_each_entry(c_drv, &vimc_cfs_subsys.drvs, list) {
		if (!strcmp(drv_name, c_drv->name))
			break;
	}
	if (!c_drv)
		return -EINVAL;

	if (c_drv->configfs_cb) {
		c_drv->configfs_cb(group);
	}

	return 0;
}

static void vimc_cfs_ent_release(struct config_item *item)
{
	struct vimc_cfs_ent *c_ent = container_of(item, struct vimc_cfs_ent,
						  cg.cg_item);

	kfree(c_ent);
}

static struct configfs_item_operations vimc_cfs_ent_item_ops = {
	.release	= vimc_cfs_ent_release,
};

static struct config_item_type vimc_cfs_ent_type = {
	.ct_item_ops	= &vimc_cfs_ent_item_ops,
	.ct_owner	= THIS_MODULE,
};

static void vimc_cfs_ent_drop_item(struct config_group *group,
				   struct config_item *item)
{
	struct vimc_cfs_ent *c_ent = container_of(item, struct vimc_cfs_ent,
						  cg.cg_item);
	struct vimc_cfs_device *cfs = container_of(group,
						   struct vimc_cfs_device,
						   gents);

	if (is_plugged(cfs))
		vimc_cfs_device_unplug(cfs);
	list_del(&c_ent->list);
}

static struct config_group *vimc_cfs_ent_make_group(struct config_group *group,
						    const char *name)
{
	struct vimc_cfs_device *cfs = container_of(group,
						   struct vimc_cfs_device,
						   gents);
	const char *drv_name = name;
	char *ent_name, *sep = strchr(drv_name, CHAR_SEPARATOR);
	struct vimc_cfs_ent *c_ent;
	size_t drv_namelen;

	if (is_plugged(cfs))
		vimc_cfs_device_unplug(cfs);

	/* Parse format "drv_name:ent_name" */
	if (!sep) {
		cg_err(&cfs->gdev,
			"Could not find separator '%c'", CHAR_SEPARATOR);
		goto syntax_error;
	}
	drv_namelen = (size_t)(sep - drv_name);
	ent_name = &sep[1];
	if (!*ent_name || !drv_namelen) {
		cg_err(&cfs->gdev,
			"%s: Driver name and entity name can't be empty.",
		       name);
		goto syntax_error;
	}
	if (drv_namelen >= sizeof(c_ent->drv)) {
		cg_err(&cfs->gdev,
		       "%s: Driver name length should be less then %ld.",
		       name, sizeof(c_ent->drv));
		goto syntax_error;
	}

	c_ent = kzalloc(sizeof(*c_ent), GFP_KERNEL);
	if (!c_ent)
		return ERR_PTR(-ENOMEM);

	/* Configure platform device */
	strscpy(c_ent->drv, drv_name, drv_namelen + 1);
	strscpy(c_ent->pdata.name, ent_name, sizeof(c_ent->pdata.name));
	c_ent->pdata.group = &c_ent->cg;

	cg_dbg(&cfs->gdev, "New entity %s:%s", c_ent->drv, c_ent->pdata.name);

	/* Configure group */
	config_group_init_type_name(&c_ent->cg, name, &vimc_cfs_ent_type);
	if (vimc_cfs_drv_cb(c_ent->drv, &c_ent->cg)) {
		cg_err(&c_ent->cg, "Module %s not found", c_ent->drv);
		kfree(c_ent);
		return ERR_PTR(-EINVAL);
	}
	list_add(&c_ent->list, &cfs->ents);

	return &c_ent->cg;

syntax_error:
	cg_err(&cfs->gdev,
		"Couldn't create entity %s, wrong syntax.", name);
	return ERR_PTR(-EINVAL);
}

/* --------------------------------------------------------------------------
 * Default group: Links
 */

static struct configfs_group_operations vimc_cfs_dlink_group_ops = {
	.make_item	= vimc_cfs_link_make_item,
	.drop_item	= vimc_cfs_link_drop_item,
};

static struct config_item_type vimc_cfs_dlink_type = {
	.ct_group_ops	= &vimc_cfs_dlink_group_ops,
	.ct_owner	= THIS_MODULE,
};

void vimc_cfs_dlink_add_default_group(struct vimc_cfs_device *cfs)
{
	config_group_init_type_name(&cfs->glinks, "links",
				    &vimc_cfs_dlink_type);
	configfs_add_default_group(&cfs->glinks, &cfs->gdev);
}

/* --------------------------------------------------------------------------
 * Default group: Entities
 */

static struct configfs_group_operations vimc_cfs_dent_group_ops = {
	.make_group	= vimc_cfs_ent_make_group,
	.drop_item	= vimc_cfs_ent_drop_item,
};

static struct config_item_type vimc_cfs_dent_type = {
	.ct_group_ops	= &vimc_cfs_dent_group_ops,
	.ct_owner	= THIS_MODULE,
};

void vimc_cfs_dent_add_default_group(struct vimc_cfs_device *cfs)
{
	config_group_init_type_name(&cfs->gents, "entities",
				    &vimc_cfs_dent_type);
	configfs_add_default_group(&cfs->gents, &cfs->gdev);
}

/* --------------------------------------------------------------------------
 * Device instance
 */

static int vimc_cfs_decode_state(const char *buf, size_t size)
{
	unsigned int i, j;

	for (i = 0; i < ARRAY_SIZE(vimc_cfs_hotplug_values); i++) {
		for (j = 0; j < ARRAY_SIZE(vimc_cfs_hotplug_values[0]); j++) {
			if (!strncmp(buf, vimc_cfs_hotplug_values[i][j], size))
				return i;
		}
	}
	return -EINVAL;
}

static ssize_t vimc_cfs_dev_attr_hotplug_show(struct config_item *item,
					      char *buf)
{
	struct vimc_cfs_device *cfs = container_of(item, struct vimc_cfs_device,
						   gdev.cg_item);

	strcpy(buf, vimc_cfs_hotplug_values[is_plugged(cfs)][0]);
	return strlen(buf);
}

static int vimc_cfs_hotplug_set(struct vimc_cfs_device *cfs,
				enum vimc_cfs_hotplug_state state)
{
	if (state == is_plugged(cfs)) {
		return 0;
	}
	else if (state == VIMC_CFS_HOTPLUG_STATE_UNPLUGGED) {
		vimc_cfs_device_unplug(cfs);
		return 0;
	}
	else if (state == VIMC_CFS_HOTPLUG_STATE_PLUGGED) {
		return vimc_cfs_device_plug(cfs);
	}
	return -EINVAL;
}

static ssize_t vimc_cfs_dev_attr_hotplug_store(struct config_item *item,
					       const char *buf, size_t size)
{
	struct vimc_cfs_device *cfs = container_of(item, struct vimc_cfs_device,
						   gdev.cg_item);
	int state = vimc_cfs_decode_state(buf, size);

	if (vimc_cfs_hotplug_set(cfs, state))
		return -EINVAL;
	return size;
}

CONFIGFS_ATTR(vimc_cfs_dev_attr_, hotplug);

static struct configfs_attribute *vimc_cfs_dev_attrs[] = {
	&vimc_cfs_dev_attr_attr_hotplug,
	NULL,
};

static void vimc_cfs_dev_release(struct config_item *item)
{
	struct vimc_cfs_device *cfs = container_of(item, struct vimc_cfs_device,
						   gdev.cg_item);

	kfree(cfs);
}

static struct configfs_item_operations vimc_cfs_dev_item_ops = {
	.release	= vimc_cfs_dev_release,
};

static struct config_item_type vimc_cfs_dev_type = {
	.ct_item_ops	= &vimc_cfs_dev_item_ops,
	.ct_attrs	= vimc_cfs_dev_attrs,
	.ct_owner	= THIS_MODULE,
};

static void vimc_cfs_dev_drop_item(struct config_group *group,
				   struct config_item *item)
{
	struct vimc_cfs_device *cfs = container_of(group,
						   struct vimc_cfs_device,
						   gdev);

	if (is_plugged(cfs))
		vimc_cfs_device_unplug(cfs);
}

static struct config_group *vimc_cfs_dev_make_group(
				struct config_group *group, const char *name)
{
	struct vimc_cfs_device *cfs = kzalloc(sizeof(*cfs), GFP_KERNEL);

	if (!cfs)
		return ERR_PTR(-ENOMEM);

	/* Configure pipeline */
	INIT_LIST_HEAD(&cfs->ents);
	INIT_LIST_HEAD(&cfs->links);

	/* Configure platform data */
	strscpy(cfs->pdata.data.name, name, sizeof(cfs->pdata.data.name));
	cfs->pdata.data.group = &cfs->gdev;
	cfs->pdata.links = &cfs->links;

	/* Configure configfs group */
	config_group_init_type_name(&cfs->gdev, name, &vimc_cfs_dev_type);
	vimc_cfs_dent_add_default_group(cfs);
	vimc_cfs_dlink_add_default_group(cfs);

	return &cfs->gdev;
}

/* --------------------------------------------------------------------------
 * Subsystem
 */

static struct configfs_group_operations vimc_cfs_subsys_group_ops = {
	/* Create vimc devices */
	.make_group	= vimc_cfs_dev_make_group,
	.drop_item	= vimc_cfs_dev_drop_item,
};

static struct config_item_type vimc_cfs_subsys_type = {
	.ct_group_ops	= &vimc_cfs_subsys_group_ops,
	.ct_owner	= THIS_MODULE,
};

int vimc_cfs_subsys_register(char *name)
{
	struct configfs_subsystem *subsys = &vimc_cfs_subsys.subsys;
	int ret;

	INIT_LIST_HEAD(&vimc_cfs_subsys.drvs);
	config_group_init_type_name(&subsys->su_group, name,
				    &vimc_cfs_subsys_type);
	mutex_init(&subsys->su_mutex);
	ret = configfs_register_subsystem(subsys);

	return ret;
}

void vimc_cfs_subsys_unregister(void)
{
	configfs_unregister_subsystem(&vimc_cfs_subsys.subsys);
}
