// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * OpenAppFilter per-application QoS classifier.
 *
 * The existing OAF module identifies applications and stores the app id in
 * conntrack mark bits 0..15. This module consumes that state and assigns a
 * dedicated QoS class to skb->mark. Actual rate limiting is performed by tc.
 *
 * PREROUTING classifies LAN-originated traffic by source MAC, while
 * POSTROUTING classifies WAN-originated traffic by destination MAC.
 */
#include <linux/etherdevice.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/netfilter.h>
#include <linux/netfilter_ipv4.h>
#include <linux/proc_fs.h>
#include <linux/skbuff.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>
#include <linux/if_ether.h>
#include <net/netfilter/nf_conntrack.h>

#include "fwx_qos.h"

#define FWX_QOS_PROC_NAME "oaf_qos_rules"

static struct fwx_qos_rule g_qos_rules[FWX_QOS_MAX_RULES];
static unsigned int g_qos_rule_count;
static DEFINE_SPINLOCK(g_qos_lock);
static struct proc_dir_entry *g_qos_proc;

static inline u32 fwx_qos_mark_encode(u16 class_id)
{
	return ((u32)class_id << FWX_QOS_CLASS_SHIFT) & FWX_QOS_CLASS_MASK;
}

static int fwx_qos_parse_mac(const char *text, u8 *mac)
{
	unsigned int octets[ETH_ALEN];
	int i;

	if (!text || !mac)
		return -EINVAL;
	if (sscanf(text, "%2x:%2x:%2x:%2x:%2x:%2x",
		   &octets[0], &octets[1], &octets[2], &octets[3],
		   &octets[4], &octets[5]) != ETH_ALEN)
		return -EINVAL;

	for (i = 0; i < ETH_ALEN; i++) {
		if (octets[i] > 0xff)
			return -EINVAL;
		mac[i] = (u8)octets[i];
	}
	return 0;
}

static int fwx_qos_parse_line(char *line, struct fwx_qos_rule *out)
{
	char mac_buf[ETH_ALEN * 3] = {0};
	unsigned int class_id = 0;
	unsigned int app_id = 0;
	char *p;

	if (!line || !out)
		return -EINVAL;

	p = strim(line);
	if (!*p || *p == '#')
		return 1;
	if (!strcmp(p, "clear"))
		return 2;

	if (sscanf(p, "%u %u %17s", &class_id, &app_id, mac_buf) != 3)
		return -EINVAL;
	if (class_id == 0 || class_id > FWX_QOS_CLASS_MAX)
		return -EINVAL;
	if (app_id == 0 || app_id > 32000)
		return -EINVAL;

	memset(out, 0, sizeof(*out));
	out->app_id = app_id;
	out->class_id = (u16)class_id;
	if (!strcmp(mac_buf, "*")) {
		out->any_mac = 1;
		return 0;
	}

	if (fwx_qos_parse_mac(mac_buf, out->mac) < 0)
		return -EINVAL;
	return 0;
}

static ssize_t fwx_qos_proc_write(struct file *file, const char __user *buf,
				  size_t count, loff_t *ppos)
{
	char *input;
	char *cursor;
	char *line;
	struct fwx_qos_rule *new_rules;
	unsigned int new_count = 0;
	int ret;

	if (count == 0)
		return 0;
	if (count > FWX_QOS_MAX_WRITE)
		return -E2BIG;

	input = memdup_user_nul(buf, count);
	if (IS_ERR(input))
		return PTR_ERR(input);

	new_rules = kcalloc(FWX_QOS_MAX_RULES, sizeof(*new_rules), GFP_KERNEL);
	if (!new_rules) {
		kfree(input);
		return -ENOMEM;
	}

	cursor = input;
	while ((line = strsep(&cursor, "\n")) != NULL) {
		if (new_count >= FWX_QOS_MAX_RULES) {
			kfree(new_rules);
			kfree(input);
			return -ENOSPC;
		}

		ret = fwx_qos_parse_line(line, &new_rules[new_count]);
		if (ret == 1)
			continue;
		if (ret == 2) {
			new_count = 0;
			continue;
		}
		if (ret < 0) {
			pr_err("oaf_qos: invalid rule line: %s\n", line);
			kfree(new_rules);
			kfree(input);
			return ret;
		}
		new_count++;
	}

	spin_lock_bh(&g_qos_lock);
	memcpy(g_qos_rules, new_rules, new_count * sizeof(*new_rules));
	g_qos_rule_count = new_count;
	spin_unlock_bh(&g_qos_lock);

	kfree(new_rules);
	kfree(input);
	pr_info("oaf_qos: loaded %u active classifiers\n", new_count);
	return count;
}

static const struct proc_ops fwx_qos_proc_ops = {
	.proc_write = fwx_qos_proc_write,
};

static u16 fwx_qos_lookup_class(u32 app_id, const u8 *mac)
{
	unsigned int i;
	u16 class_id = 0;

	if (!app_id)
		return 0;

	spin_lock_bh(&g_qos_lock);
	for (i = 0; i < g_qos_rule_count; i++) {
		const struct fwx_qos_rule *rule = &g_qos_rules[i];

		if (rule->app_id != app_id)
			continue;
		if (!rule->any_mac && (!mac || !ether_addr_equal(rule->mac, mac)))
			continue;
		class_id = rule->class_id;
		break;
	}
	spin_unlock_bh(&g_qos_lock);
	return class_id;
}

static unsigned int fwx_qos_apply(struct sk_buff *skb, const u8 *client_mac)
{
	enum ip_conntrack_info ctinfo;
	struct nf_conn *ct;
	u32 app_id;
	u16 class_id;
	u32 mark;

	if (!skb)
		return NF_ACCEPT;

	ct = nf_ct_get(skb, &ctinfo);
	if (!ct)
		return NF_ACCEPT;

	app_id = ct->mark & 0x0000FFFFU;
	if (app_id == 0 || app_id > 32000)
		return NF_ACCEPT;

	class_id = fwx_qos_lookup_class(app_id, client_mac);
	if (!class_id)
		return NF_ACCEPT;

	mark = skb->mark & ~FWX_QOS_SKB_MARK_MASK;
	skb->mark = mark | fwx_qos_mark_encode(class_id);
	return NF_ACCEPT;
}

static unsigned int fwx_qos_prerouting(void *priv, struct sk_buff *skb,
					       const struct nf_hook_state *state)
{
	const struct ethhdr *eth;

	if (!skb)
		return NF_ACCEPT;
	eth = eth_hdr(skb);
	return fwx_qos_apply(skb, eth ? eth->h_source : NULL);
}

static unsigned int fwx_qos_postrouting(void *priv, struct sk_buff *skb,
						const struct nf_hook_state *state)
{
	const struct ethhdr *eth;

	if (!skb)
		return NF_ACCEPT;
	eth = eth_hdr(skb);
	return fwx_qos_apply(skb, eth ? eth->h_dest : NULL);
}

static struct nf_hook_ops fwx_qos_ops[] __read_mostly = {
	{
		.hook = fwx_qos_prerouting,
		.pf = NFPROTO_INET,
		.hooknum = NF_INET_PRE_ROUTING,
		.priority = NF_IP_PRI_CONNTRACK + 2,
	},
	{
		.hook = fwx_qos_postrouting,
		.pf = NFPROTO_INET,
		.hooknum = NF_INET_POST_ROUTING,
		.priority = NF_IP_PRI_CONNTRACK + 2,
	},
};

static int __init fwx_qos_init(void)
{
	int ret;

	g_qos_rule_count = 0;
	g_qos_proc = proc_create(FWX_QOS_PROC_NAME, 0200, NULL, &fwx_qos_proc_ops);
	if (!g_qos_proc) {
		pr_err("oaf_qos: failed to create /proc/%s\n", FWX_QOS_PROC_NAME);
		return -ENOMEM;
	}

	ret = nf_register_net_hooks(&init_net, fwx_qos_ops,
					ARRAY_SIZE(fwx_qos_ops));
	if (ret) {
		proc_remove(g_qos_proc);
		g_qos_proc = NULL;
		pr_err("oaf_qos: failed to register netfilter hooks: %d\n", ret);
		return ret;
	}

	pr_info("oaf_qos: classifier initialized\n");
	return 0;
}

static void __exit fwx_qos_exit(void)
{
	nf_unregister_net_hooks(&init_net, fwx_qos_ops,
				ARRAY_SIZE(fwx_qos_ops));
	if (g_qos_proc) {
		proc_remove(g_qos_proc);
		g_qos_proc = NULL;
	}
	spin_lock_bh(&g_qos_lock);
	g_qos_rule_count = 0;
	spin_unlock_bh(&g_qos_lock);
	pr_info("oaf_qos: classifier exited\n");
}

module_init(fwx_qos_init);
module_exit(fwx_qos_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("OpenAppFilter contributors");
MODULE_DESCRIPTION("OpenAppFilter per-application QoS classifier");
