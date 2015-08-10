/*
 * vimc-core.c Virtual Media Controller Driver
 *
 * Copyright (C) 2015-2017 Helen Koike <helen.fornazier@gmail.com>
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
#include <linux/init.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <media/media-device.h>
#include <media/v4l2-device.h>

#include "vimc-common.h"
#include "vimc-configfs.h"

struct vimc_device {
	/* The Associated media_device parent */
	struct media_device mdev;

	/* Internal v4l2 parent device*/
	struct v4l2_device v4l2_dev;
};

static int vimc_core_links_create(const struct device *master)
{
	struct vimc_platform_data_core *pdata = master->platform_data;
	struct vimc_platform_data_link *plink;
	struct vimc_ent_device *ved_src, *ved_sink;
	int ret;

	list_for_each_entry(plink, pdata->links, list) {
		ved_src = platform_get_drvdata(plink->source);
		ved_sink = platform_get_drvdata(plink->sink);

		ret = media_create_pad_link(ved_src->ent, plink->source_pad,
					    ved_sink->ent, plink->sink_pad,
					    plink->flags);
		if (ret)
			return ret;
	}

	return 0;
}

int vimc_core_comp_bind(struct device *master)
{
	struct platform_device *pdev = to_platform_device(master);
	struct vimc_device *vimc = platform_get_drvdata(pdev);
	int ret;

	dev_dbg(master, "bind");

	/* Register the v4l2 struct */
	ret = v4l2_device_register(vimc->mdev.dev, &vimc->v4l2_dev);
	if (ret) {
		dev_err(vimc->mdev.dev,
			"v4l2 device register failed (err=%d)\n", ret);
		return ret;
	}

	/* Bind subdevices */
	ret = component_bind_all(master, &vimc->v4l2_dev);
	if (ret)
		goto err_v4l2_unregister;
	ret = vimc_core_links_create(master);
	if (ret)
		goto err_comp_unbind_all;

	/* Register the media device */
	ret = media_device_register(&vimc->mdev);
	if (ret) {
		dev_err(vimc->mdev.dev,
			"media device register failed (err=%d)\n", ret);
		goto err_comp_unbind_all;
	}

	/* Expose all subdev's nodes*/
	ret = v4l2_device_register_subdev_nodes(&vimc->v4l2_dev);
	if (ret) {
		dev_err(vimc->mdev.dev,
			"vimc subdev nodes registration failed (err=%d)\n",
			ret);
		goto err_mdev_unregister;
	}

	return 0;

err_mdev_unregister:
	media_device_unregister(&vimc->mdev);
err_comp_unbind_all:
	component_unbind_all(master, NULL);
err_v4l2_unregister:
	v4l2_device_unregister(&vimc->v4l2_dev);

	return ret;
}
EXPORT_SYMBOL_GPL(vimc_core_comp_bind);

void vimc_core_comp_unbind(struct device *master)
{
	struct vimc_device *vimc = platform_get_drvdata(to_platform_device(master));

	dev_dbg(master, "unbind");

	media_device_unregister(&vimc->mdev);
	component_unbind_all(master, NULL);
	v4l2_device_unregister(&vimc->v4l2_dev);
}
EXPORT_SYMBOL_GPL(vimc_core_comp_unbind);

static int vimc_probe(struct platform_device *pdev)
{
	const struct vimc_platform_data_core *pdata = pdev->dev.platform_data;
	struct vimc_device *vimc = devm_kzalloc(&pdev->dev, sizeof(*vimc),
						GFP_KERNEL);

	dev_dbg(&pdev->dev, "probe");

	/* Initialize media device */
	strscpy(vimc->mdev.model, pdata->data.name, sizeof(vimc->mdev.model));
	vimc->mdev.dev = &pdev->dev;
	media_device_init(&vimc->mdev);
	vimc->v4l2_dev.mdev = &vimc->mdev;

	platform_set_drvdata(pdev, vimc);

	return 0;
}

static int vimc_remove(struct platform_device *pdev)
{
	struct vimc_device *vimc = platform_get_drvdata(pdev);

	dev_dbg(&pdev->dev, "remove");

	media_device_cleanup(&vimc->mdev);

	return 0;
}

static struct platform_driver vimc_pdrv = {
	.probe		= vimc_probe,
	.remove		= vimc_remove,
	.driver		= {
		.name	= "vimc-core",
	}
};

static int __init vimc_init(void)
{
	int ret;

	ret = platform_driver_register(&vimc_pdrv);
	if (ret)
		return ret;

	ret = vimc_cfs_subsys_register("vimc");
	if (ret) {
		platform_driver_unregister(&vimc_pdrv);
		return ret;
	}

	return 0;
}

static void __exit vimc_exit(void)
{
	vimc_cfs_subsys_unregister();

	platform_driver_unregister(&vimc_pdrv);
}

module_init(vimc_init);
module_exit(vimc_exit);

MODULE_DESCRIPTION("Virtual Media Controller Driver (VIMC)");
MODULE_AUTHOR("Helen Fornazier <helen.fornazier@gmail.com>");
MODULE_LICENSE("GPL");
