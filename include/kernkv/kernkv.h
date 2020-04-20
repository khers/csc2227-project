/* Copyright(c) 2020 Eric B Munson */

/*
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef KERN_KV_H
#define KERN_KV_H

#include <kernkv/structures.h>

int init_kernkv(const char *local_ip, unsigned int num_nodes, u16 chain_length, char **nodes);

int get(u64 key, struct value *value);
int put(u64 key, struct value *value);
int del(u64 key);

void shutdown_kernkv(void);

#endif

