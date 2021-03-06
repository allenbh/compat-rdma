/*
 * Copyright 2013  Mellanox Technologies Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * Compatibility file for Linux RDMA for kernels 3.9.
 */

#include <linux/skbuff.h>
#include <linux/export.h>
#include <linux/ip.h>
#include <linux/ipv6.h>
#include <linux/if_vlan.h>
#include <net/ip.h>
#include <net/ipv6.h>
#include <linux/igmp.h>
#include <linux/icmp.h>
#include <linux/sctp.h>
#include <linux/dccp.h>
#include <linux/if_tunnel.h>
#include <linux/if_pppox.h>
#include <linux/ppp_defs.h>
#include <net/flow_keys.h>

#ifdef CONFIG_XPS
static u32 hashrnd __read_mostly;
#endif

#define get_xps_queue LINUX_BACKPORT(get_xps_queue)
static inline int get_xps_queue(struct net_device *dev, struct sk_buff *skb)
{
#ifdef CONFIG_XPS
	struct xps_dev_maps *dev_maps;
	struct xps_map *map;
	int queue_index = -1;

	rcu_read_lock();
	dev_maps = rcu_dereference(dev->xps_maps);
	if (dev_maps) {
		map = rcu_dereference(
		    dev_maps->cpu_map[raw_smp_processor_id()]);
		if (map) {
			if (map->len == 1)
				queue_index = map->queues[0];
			else {
				u32 hash;
				if (skb->sk && skb->sk->sk_hash)
					hash = skb->sk->sk_hash;
				else
					hash = (__force u16) skb->protocol ^
					    skb->rxhash;
				hash = jhash_1word(hash, hashrnd);
				queue_index = map->queues[
				    ((u64)hash * map->len) >> 32];
			}
			if (unlikely(queue_index >= dev->real_num_tx_queues))
				queue_index = -1;
		}
	}
	rcu_read_unlock();

	return queue_index;
#else
	return -1;
#endif
}

#define __netdev_pick_tx LINUX_BACKPORT(__netdev_pick_tx)
u16 __netdev_pick_tx(struct net_device *dev, struct sk_buff *skb)
{
	struct sock *sk = skb->sk;
	int queue_index = sk_tx_queue_get(sk);

	if (queue_index < 0 || skb->ooo_okay ||
	    queue_index >= dev->real_num_tx_queues) {
		int new_index = get_xps_queue(dev, skb);
		if (new_index < 0)
			new_index = skb_tx_hash(dev, skb);

		if (queue_index != new_index && sk &&
		    rcu_access_pointer(sk->sk_dst_cache))
			sk_tx_queue_set(sk, new_index);

		queue_index = new_index;
	}

	return queue_index;
}
EXPORT_SYMBOL(__netdev_pick_tx);

#define netif_set_xps_queue LINUX_BACKPORT(netif_set_xps_queue)
int netif_set_xps_queue(struct net_device *dev, struct cpumask *msk, u16 idx)
{
#ifdef HAVE_XPS_MAP
	int i, len, err;
	char buf[MAX_XPS_BUFFER_SIZE];
	struct attribute *attr = NULL;
	struct kobj_type *ktype = NULL;
	struct mlx4_en_netq_attribute *xps_attr = NULL;
	struct netdev_queue *txq = netdev_get_tx_queue(dev, idx);

#ifdef HAVE_NET_DEVICE_EXTENDED_TX_EXT
	struct netdev_tx_queue_extended *txq_ext =
					netdev_extended(dev)->_tx_ext + idx;
	ktype = txq_ext->kobj.ktype;
#else /* HAVE_NET_DEVICE_EXTENDED_TX_EXT */
	ktype = txq->kobj.ktype;
#endif /* HAVE_NET_DEVICE_EXTENDED_TX_EXT */
	if (!ktype)
		return -ENOMEM;

	for (i = 0; (attr = ktype->default_attrs[i]); i++) {
		if (!strcmp("xps_cpus", attr->name))
			break;
	}
	if (!attr)
		return -EINVAL;

	len = bitmap_scnprintf(buf, MAX_XPS_BUFFER_SIZE,
			       cpumask_bits(msk), MAX_XPS_CPUS);
	if (!len)
		return -ENOMEM;

	xps_attr = to_netq_attr(attr);
	err = xps_attr->store(txq, xps_attr, buf, len);
	if (err)
		return -EINVAL;

	return 0;
#else /* HAVE_XPS_MAP */
	return -1;
#endif /* HAVE_XPS_MAP */
}
EXPORT_SYMBOL(netif_set_xps_queue);
