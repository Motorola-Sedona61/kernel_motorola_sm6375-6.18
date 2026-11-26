// SPDX-License-Identifier: GPL-2.0-only
/*
 * Qualcomm Technologies, Inc. CX iPeak peak-current coordination driver
 */

#include <asm/barrier.h>
#include <linux/err.h>
#include <linux/export.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <soc/qcom/cx_ipeak.h>

#define CXIP_BYPASS_OFFSET		0x4
#define CXIP_VOTE_CLEAR_OFFSET		0x8
#define CXIP_VOTE_SET_OFFSET		0xc
#define CXIP_FEATURE_EN_OFFSET		0x10
#define CXIP_DANGER_OFFSET		0x24

#define CXIP_V2_CLIENT_STRIDE		0x1000
#define CXIP_V2_CLIENT_HIGH_GAP		0x3000
#define CXIP_V2_CLIENT_HIGH_ID		9
#define CXIP_V2_THROTTLE_VOTE_OFFSET	0xb000

#define CXIP_MAX_CLIENT_ID		31
#define CXIP_POLL_TIMEOUT_NS		(50 * NSEC_PER_MSEC)
#define CXIP_MAX_VICTIMS		3

struct cx_ipeak_device;

struct cx_ipeak_ops {
	int (*update)(struct cx_ipeak_client *client, bool vote);
	struct cx_ipeak_client *(*alloc_client)(int client_id);
};

struct cx_ipeak_client {
	unsigned int vote_cnt;
	u32 reg_val;
	int client_id;
	bool victim_throttled;
	struct cx_ipeak_device *dev;
};

struct cx_ipeak_victim {
	u32 client_id;
	u32 victim_id;
	u32 freq_limit;
	void *data;
	cx_ipeak_victim_fn cb;
	struct cx_ipeak_client *client;
};

struct cx_ipeak_device {
	struct platform_device *pdev;
	struct mutex lock;
	struct mutex victim_lock;
	void __iomem *base;
	const struct cx_ipeak_ops *ops;
	struct cx_ipeak_victim victims[CXIP_MAX_VICTIMS];
	unsigned int victim_cnt;
	int danger_irq;
	int safe_irq;
};

static struct cx_ipeak_device cx_ipeak_dev = {
	.lock = __MUTEX_INITIALIZER(cx_ipeak_dev.lock),
	.victim_lock = __MUTEX_INITIALIZER(cx_ipeak_dev.victim_lock),
	.danger_irq = -EINVAL,
	.safe_irq = -EINVAL,
};

static int cx_ipeak_v1_update(struct cx_ipeak_client *client, bool vote)
{
	void __iomem *base = client->dev->base;
	u64 deadline;
	int ret = 0;

	mutex_lock(&client->dev->lock);
	if (vote) {
		if (client->vote_cnt == 0) {
			writel_relaxed(client->reg_val,
				       base + CXIP_VOTE_SET_OFFSET);
			deadline = ktime_get_ns() + CXIP_POLL_TIMEOUT_NS;
			while (readl(base + CXIP_DANGER_OFFSET)) {
				if (ktime_get_ns() > deadline)
					break;
			}
			if (readl(base + CXIP_DANGER_OFFSET)) {
				writel_relaxed(client->reg_val,
					       base + CXIP_VOTE_CLEAR_OFFSET);
				ret = -ETIMEDOUT;
				goto out;
			}
		}
		client->vote_cnt++;
	} else {
		if (client->vote_cnt == 0) {
			ret = -EINVAL;
			goto out;
		}
		if (--client->vote_cnt == 0)
			writel_relaxed(client->reg_val,
				       base + CXIP_VOTE_CLEAR_OFFSET);
	}

out:
	mutex_unlock(&client->dev->lock);
	return ret;
}

static struct cx_ipeak_client *cx_ipeak_v1_alloc_client(int client_id)
{
	void __iomem *base = cx_ipeak_dev.base;
	struct cx_ipeak_client *client;
	u32 mask = BIT(client_id);

	if (!readl_relaxed(base + CXIP_FEATURE_EN_OFFSET))
		return NULL;
	if (readl_relaxed(base + CXIP_BYPASS_OFFSET) & mask)
		return NULL;

	client = kzalloc(sizeof(*client), GFP_KERNEL);
	if (!client)
		return ERR_PTR(-ENOMEM);

	client->reg_val = mask;
	client->dev = &cx_ipeak_dev;
	return client;
}

static int cx_ipeak_v2_update(struct cx_ipeak_client *client, bool vote)
{
	void __iomem *base = client->dev->base;
	u64 deadline;
	int ret = 0;

	mutex_lock(&client->dev->lock);
	if (vote) {
		if (client->vote_cnt == 0) {
			writel_relaxed(1, base + client->reg_val);
			deadline = ktime_get_ns() + CXIP_POLL_TIMEOUT_NS;
			while (readl(base + CXIP_DANGER_OFFSET)) {
				if (client->victim_throttled ||
				    ktime_get_ns() > deadline)
					break;
			}
			if (readl(base + CXIP_DANGER_OFFSET) &&
			    !client->victim_throttled) {
				writel_relaxed(0, base + client->reg_val);
				ret = -ETIMEDOUT;
				goto out;
			}
			if (client->victim_throttled) {
				writel_relaxed(0, base + client->reg_val);
				ret = -ETIMEDOUT;
				goto out;
			}
		}
		client->vote_cnt++;
	} else {
		if (client->vote_cnt == 0) {
			ret = -EINVAL;
			goto out;
		}
		if (--client->vote_cnt == 0)
			writel_relaxed(0, base + client->reg_val);
	}

out:
	mutex_unlock(&client->dev->lock);
	return ret;
}

static struct cx_ipeak_client *cx_ipeak_v2_alloc_client(int client_id)
{
	void __iomem *base = cx_ipeak_dev.base;
	struct cx_ipeak_client *client;
	u32 offset = 0;

	if (client_id >= 0)
		offset = (u32)(client_id + 1) * CXIP_V2_CLIENT_STRIDE;
	if (client_id > CXIP_V2_CLIENT_HIGH_ID)
		offset += CXIP_V2_CLIENT_HIGH_GAP;

	if (!readl_relaxed(base + CXIP_FEATURE_EN_OFFSET))
		return NULL;
	if (readl_relaxed(base + offset + CXIP_BYPASS_OFFSET) & BIT(0))
		return NULL;

	client = kzalloc(sizeof(*client), GFP_KERNEL);
	if (!client)
		return ERR_PTR(-ENOMEM);

	client->reg_val = offset;
	client->dev = &cx_ipeak_dev;
	return client;
}

static const struct cx_ipeak_ops cx_ipeak_v1_ops = {
	.update = cx_ipeak_v1_update,
	.alloc_client = cx_ipeak_v1_alloc_client,
};

static const struct cx_ipeak_ops cx_ipeak_v2_ops = {
	.update = cx_ipeak_v2_update,
	.alloc_client = cx_ipeak_v2_alloc_client,
};

struct cx_ipeak_client *cx_ipeak_register(struct device_node *dev_node,
					  const char *client_name)
{
	struct of_phandle_args spec = {};
	struct cx_ipeak_client *client;
	int ret;

	if (!dev_node || !client_name)
		return ERR_PTR(-EINVAL);

	ret = of_parse_phandle_with_args(dev_node, client_name,
					 "#size-cells", 0, &spec);
	if (ret)
		return ERR_PTR(-EINVAL);

	if (!of_device_is_available(spec.np)) {
		of_node_put(spec.np);
		return NULL;
	}
	of_node_put(spec.np);

	if (!smp_load_acquire(&cx_ipeak_dev.base))
		return ERR_PTR(-EPROBE_DEFER);

	if (spec.args_count < 1 || spec.args[0] > CXIP_MAX_CLIENT_ID)
		return ERR_PTR(-EINVAL);

	if (!cx_ipeak_dev.ops || !cx_ipeak_dev.ops->alloc_client)
		return NULL;

	client = cx_ipeak_dev.ops->alloc_client(spec.args[0]);
	if (IS_ERR_OR_NULL(client))
		return client;

	client->client_id = spec.args[0];
	return client;
}
EXPORT_SYMBOL(cx_ipeak_register);

void cx_ipeak_unregister(struct cx_ipeak_client *client)
{
	if (IS_ERR_OR_NULL(client))
		return;

	cx_ipeak_victim_unregister(client);
	kfree(client);
}
EXPORT_SYMBOL(cx_ipeak_unregister);

int cx_ipeak_update(struct cx_ipeak_client *client, bool vote)
{
	if (!client)
		return 0;
	if (IS_ERR(client) || !client->dev || !client->dev->ops ||
	    !client->dev->base)
		return -EINVAL;

	return client->dev->ops->update(client, vote);
}
EXPORT_SYMBOL(cx_ipeak_update);

int cx_ipeak_victim_register(struct cx_ipeak_client *client,
			     cx_ipeak_victim_fn victim_cb, void *data)
{
	unsigned int i;

	if (!client || IS_ERR(client) || !victim_cb)
		return -EINVAL;

	mutex_lock(&cx_ipeak_dev.victim_lock);
	for (i = 0; i < cx_ipeak_dev.victim_cnt; i++) {
		if (cx_ipeak_dev.victims[i].client_id ==
		    (u32)client->client_id) {
			cx_ipeak_dev.victims[i].cb = victim_cb;
			cx_ipeak_dev.victims[i].data = data;
			cx_ipeak_dev.victims[i].client = client;
			mutex_unlock(&cx_ipeak_dev.victim_lock);
			return 0;
		}
	}
	mutex_unlock(&cx_ipeak_dev.victim_lock);
	return -ENOENT;
}
EXPORT_SYMBOL(cx_ipeak_victim_register);

void cx_ipeak_victim_unregister(struct cx_ipeak_client *client)
{
	unsigned int i;

	if (IS_ERR_OR_NULL(client))
		return;

	mutex_lock(&cx_ipeak_dev.victim_lock);
	for (i = 0; i < cx_ipeak_dev.victim_cnt; i++) {
		if (cx_ipeak_dev.victims[i].client_id ==
		    (u32)client->client_id) {
			cx_ipeak_dev.victims[i].cb = NULL;
			cx_ipeak_dev.victims[i].data = NULL;
			cx_ipeak_dev.victims[i].client = NULL;
		}
	}
	mutex_unlock(&cx_ipeak_dev.victim_lock);
}
EXPORT_SYMBOL(cx_ipeak_victim_unregister);

static irqreturn_t cx_ipeak_irq_thread(int irq, void *dev_id)
{
	unsigned int i;

	mutex_lock(&cx_ipeak_dev.victim_lock);
	for (i = 0; i < cx_ipeak_dev.victim_cnt; i++) {
		struct cx_ipeak_victim *v = &cx_ipeak_dev.victims[i];
		u32 vote_off;

		if (!v->cb || !v->client || !cx_ipeak_dev.base)
			continue;

		vote_off = (v->victim_id << 12) + CXIP_V2_THROTTLE_VOTE_OFFSET;
		if (irq == cx_ipeak_dev.danger_irq) {
			v->client->victim_throttled = true;
			if (!v->cb(v->data, v->freq_limit))
				writel_relaxed(1, cx_ipeak_dev.base + vote_off);
		} else if (irq == cx_ipeak_dev.safe_irq) {
			v->client->victim_throttled = false;
			v->cb(v->data, 0);
			writel_relaxed(0, cx_ipeak_dev.base + vote_off);
		}
	}
	mutex_unlock(&cx_ipeak_dev.victim_lock);

	return IRQ_HANDLED;
}

static int cx_ipeak_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	const struct cx_ipeak_ops *ops;
	void __iomem *base;
	struct resource *res;
	int count, i, ret;
	int danger_irq, safe_irq;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -ENODEV;

	base = devm_ioremap(&pdev->dev, res->start, resource_size(res));
	if (!base)
		return -ENOMEM;

	if (of_device_is_compatible(np, "qcom,cx-ipeak-v1"))
		ops = &cx_ipeak_v1_ops;
	else if (of_device_is_compatible(np, "qcom,cx-ipeak-v2"))
		ops = &cx_ipeak_v2_ops;
	else
		ops = NULL;

	cx_ipeak_dev.victim_cnt = 0;
	if (of_find_property(np, "victims_table", NULL)) {
		count = of_property_count_elems_of_size(np, "victims_table",
							sizeof(u32));
		if (count < 0 || (count % 3) != 0 ||
		    (count / 3) > CXIP_MAX_VICTIMS)
			return -EINVAL;

		for (i = 0; i < count / 3; i++) {
			ret = of_property_read_u32_index(np, "victims_table",
				i * 3, &cx_ipeak_dev.victims[i].client_id);
			if (ret)
				return ret;
			ret = of_property_read_u32_index(np, "victims_table",
				i * 3 + 1, &cx_ipeak_dev.victims[i].victim_id);
			if (ret)
				return ret;
			ret = of_property_read_u32_index(np, "victims_table",
				i * 3 + 2, &cx_ipeak_dev.victims[i].freq_limit);
			if (ret)
				return ret;
			cx_ipeak_dev.victim_cnt++;
		}
	}

	danger_irq = platform_get_irq_byname_optional(pdev, "cx_ipeak_danger");
	if (danger_irq > 0) {
		ret = devm_request_threaded_irq(&pdev->dev,
			danger_irq, NULL, cx_ipeak_irq_thread,
			IRQF_ONESHOT | IRQF_TRIGGER_RISING,
			"cx_ipeak_danger", NULL);
		if (ret)
			return ret;
	}

	safe_irq = platform_get_irq_byname_optional(pdev, "cx_ipeak_safe");
	if (safe_irq > 0) {
		ret = devm_request_threaded_irq(&pdev->dev,
			safe_irq, NULL, cx_ipeak_irq_thread,
			IRQF_ONESHOT | IRQF_TRIGGER_RISING,
			"cx_ipeak_safe", NULL);
		if (ret)
			return ret;
	}

	cx_ipeak_dev.danger_irq = danger_irq;
	cx_ipeak_dev.safe_irq = safe_irq;
	cx_ipeak_dev.ops = ops;
	cx_ipeak_dev.pdev = pdev;
	/* Pairs with smp_load_acquire() in cx_ipeak_register(). */
	smp_store_release(&cx_ipeak_dev.base, base);
	return 0;
}

static const struct of_device_id cx_ipeak_match_table[] = {
	{ .compatible = "qcom,cx-ipeak-v1" },
	{ .compatible = "qcom,cx-ipeak-v2" },
	{ .compatible = "qcom,cx-ipeak" },
	{}
};
MODULE_DEVICE_TABLE(of, cx_ipeak_match_table);

static struct platform_driver cx_ipeak_platform_driver = {
	.probe = cx_ipeak_probe,
	.driver = {
		.name = "cx_ipeak",
		.of_match_table = cx_ipeak_match_table,
	},
};

static int __init cx_ipeak_init(void)
{
	return platform_driver_register(&cx_ipeak_platform_driver);
}
arch_initcall(cx_ipeak_init);

MODULE_DESCRIPTION("Qualcomm Technologies, Inc. CX iPeak Driver");
MODULE_LICENSE("GPL");
