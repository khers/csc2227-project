// SPDX-License-Identifier: GPL-2.0

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/inet.h>
#include <asm/string.h>

#include <kernkv/structures.h>

#include "hashring.h"

struct ring_state {
	/* We will store the ip network order, but process it in host */
	u32 *nodes;
	u32 chain_length;
};

static struct ring_state state;

int init_hashring(u32 chain_length)
{
	if (chain_length == 0) {
		printk(KERN_ERR "chain length must be greater than 0\n");
		return -1;
	}
	state.chain_length = chain_length;

	state.nodes = kzalloc(sizeof(u32) * RING_NODES, 0);
	if (!state.nodes) {
		printk(KERN_ERR "Failed to allocate node array\n");
		return -1;
	}

	return 0;
}

void cleanup_hashring(void)
{
	state.chain_length = 0;
	kfree(state.nodes);
}

int add_node(const char *ip_str)
{
	u32 ip = in_aton(ip_str);
	u8 node_id = RING_HASH(ntohl(ip));
	int checked = 0;

	while(state.nodes[node_id] != 0) {
		node_id = (node_id + 1) % RING_NODES;
		checked++;
		if (checked > RING_NODES) {
			printk(KERN_WARNING "Already have %d nodes in system, cannot add\n",
					RING_NODES);
			return -1;
		}
	}

	state.nodes[node_id] = ip;
	return 0;
}

int del_node(const char *ip_str)
{
	int checked = 0;
	u32 ip = in_aton(ip_str);
	u8 node_id = RING_HASH(ntohl(ip));

	while(state.nodes[node_id] != ip) {
		node_id = (node_id + 1) % RING_NODES;
		checked++;
		if (checked > RING_NODES) {
			printk(KERN_WARNING "Requested node for deletion not present\n");
			return -1;
		}
	}

	state.nodes[node_id] = 0;

	return 0;
}

u32 get_next_hop(u32 ip)
{
	u8 node_id = RING_HASH(ip);
	int checked = 0;

	while(state.nodes[node_id] != ip) {
		node_id = (node_id + 1) % RING_NODES;
		checked++;
		if (checked > RING_NODES) {
			printk(KERN_WARNING "Requested node for chain start not present\n");
			return 0;
		}
	}

	node_id++;
	while (state.nodes[node_id] == 0) {
		node_id = (node_id + 1) % RING_NODES;
	}

	if (state.nodes[node_id] == ip) {
		printk(KERN_WARNING "Search for next hop wrapped\n");
	}

	return state.nodes[node_id];
}

MODULE_AUTHOR("Eric B. Munson");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("In kernel distributed key/value store");

