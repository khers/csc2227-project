// SPDX-License-Identifier: GPL-2.0

#ifndef KV_HASHRING_H
#define KV_HASHRING_H

#include <kernkv/structures.h>

int init_hashring(u32 chain_length);

void cleanup_hashring(void);

int add_node(const char *ip_str);

int del_node(const char *ip_str);

u32 get_next_hop(u32 my_ip);

#endif
