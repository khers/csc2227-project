/* Copyright (c) 2020 Eric B Munson */

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

#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <stddef.h>
#include <string.h>

#include <kernkv/kernkv.h>
#include <kernkv/structures.h>

struct system_state {
	char *hostname;
	int socket;
};

static struct system_state state;

static u64 request_ids;

int init_kernkv(const char *hostname)
{
        state.hostname = strndup(hostname, 253);
        if (!state.hostname)
          return -1;

	state.socket = socket(AF_INET, SOCK_DGRAM, 0);
	if (state.socket < 0)
		return -1;

	return 0;
}

static int get_addr(const char *hostname, struct sockaddr_in *addr)
{
	struct addrinfo *info;
	int ret;

	if (!addr)
		return -1;

	if ((ret = getaddrinfo(hostname, NULL, NULL, &info)) != 0) {
		return ret;
	}

	addr->sin_family = AF_INET;
	addr->sin_port = htons(CHAIN_PORT);
	return 0;
}

int get(u64 key)
{
	u64 req_id = __sync_fetch_and_add(&request_ids, 1);
	struct kv_request req;

	req.request_id = req_id;
	req.type = KV_GET;
	req.get.key = key;
	return -ENOTSUP;
}

int put(u64 key, struct value *value)
{
	u64 req_id = __sync_fetch_and_add(&request_ids, 1);
	return -ENOTSUP;
}

int del(u64 key)
{
	u64 req_id = __sync_fetch_and_add(&request_ids, 1);
	return -ENOTSUP;
}
