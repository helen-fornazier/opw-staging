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
	kfree(item);
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
	struct config_item *item = kzalloc(sizeof(struct config_item), GFP_KERNEL);
	if (!item)
		return ERR_PTR(-ENOMEM);

	config_item_init_type_name(item, name, &vimc_cfg_lnk_type);

	return item;
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
	kfree(item);
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

static struct config_group *vimc_cfg_pad_make_group(struct config_group *pgroup,
						    const char *name)
{
	struct config_group *group = kzalloc(sizeof(struct config_group), GFP_KERNEL);
	if (!group)
		return ERR_PTR(-ENOMEM);

	config_group_init_type_name(group, name, &vimc_cfg_pad_type);

	return group;
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
	/* TODO: take the according action dependending on the attribute being
	 * shown */
	return sprintf(page, attr->ca_name);
}

static void vimc_cfg_ent_release_item(struct config_item *item)
{
	struct config_group *group = container_of(item, struct config_group,
						  cg_item);
	kfree(group);
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

static struct config_group *vimc_cfg_ent_make_group(struct config_group *pgroup,
						    const char *name)
{
	struct config_group *group = kzalloc(sizeof(struct config_group), GFP_KERNEL);
	if (!group)
		return ERR_PTR(-ENOMEM);

	config_group_init_type_name(group, name, &vimc_cfg_ent_type);

	return group;
}

/* --------------------------------------------------------------------------
 * Subsystem
 */

static struct configfs_attribute vimc_cfg_subsys_attr_deploy = {
	.ca_owner = THIS_MODULE,
	.ca_name = "deploy_topology",
	.ca_mode = S_IRUGO,
};

static struct configfs_attribute vimc_cfg_subsys_attr_status = {
	.ca_owner = THIS_MODULE,
	.ca_name = "status",
	.ca_mode = S_IRUGO,
};

static ssize_t vimc_cfg_subsys_show_attr(struct config_item *item,
					 struct configfs_attribute *attr,
					 char *page)
{
	/* TODO: take the according action dependending on the attribute being
	 * shown */
	return sprintf(page, attr->ca_name);
}

static struct configfs_item_operations vimc_cfg_subsys_item_ops = {
	.show_attribute	= vimc_cfg_subsys_show_attr,
};

static struct configfs_group_operations vimc_cfg_subsys_group_ops = {
	/* Create entity children */
	.make_group	= vimc_cfg_ent_make_group,
};

static struct configfs_attribute *vimc_cfg_subsys_attrs[] = {
	&vimc_cfg_subsys_attr_deploy,
	&vimc_cfg_subsys_attr_status,
	NULL,
};

static struct config_item_type vimc_cfg_subsys_type = {
	.ct_item_ops	= &vimc_cfg_subsys_item_ops,
	.ct_group_ops	= &vimc_cfg_subsys_group_ops,
	.ct_attrs	= vimc_cfg_subsys_attrs,
	.ct_owner	= THIS_MODULE,
};

static struct configfs_subsystem vimc_cfg_subsys = {
	.su_group = {
		.cg_item = {
			.ci_namebuf = "vimc",
			.ci_type = &vimc_cfg_subsys_type,
		},
	},
};

/* --------------------------------------------------------------------------
 * Functions
 */

int vimc_cfg_register(void)
{
	struct configfs_subsystem *subsys;
	int ret = 0;

	subsys = &vimc_cfg_subsys;
	config_group_init(&subsys->su_group);
	mutex_init(&subsys->su_mutex);
	ret = configfs_register_subsystem(subsys);
	if (ret) {
		printk(KERN_ERR "Error %d while registering subsystem %s\n",
		       ret,
		       subsys->su_group.cg_item.ci_namebuf);
	}

	return ret;
}

void vimc_cfg_unregister(void)
{
	configfs_unregister_subsystem(&vimc_cfg_subsys);
}
