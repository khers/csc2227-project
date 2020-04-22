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
#include <stdlib.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <stdarg.h>
#include <stdio.h>
#include <sys/epoll.h>

#include <kernkv/kernkv.h>
#include <kernkv/structures.h>

struct system_state {
	u32 *nodes;
	u32 local_ip_net_order;
	int started;
	int epfd;
	u16 chain_length;
};

static struct system_state state;

static u64 request_ids;

void logit(const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	vfprintf(stderr, fmt, args);
	va_end(args);
}

static u32 convert_ip(const char *ip)
{
	u32 ret = 0;
	int err = inet_pton(AF_INET, ip, (void*)&ret);
	/* inet_pton does not follow the "standard" return value pattern and instead
	   returns 1 on success */
	if (err != 1) {
		return 0;
	}
	return ret;
}

static int add_node(const char *ip_str)
{
	u32 ip = convert_ip(ip_str);
	u8 node_id = RING_HASH(ntohl(ip));
	int checked = 0;

	if (ip == 0) {
		logit("Failed to convert ip '%s'.  %s\n", ip_str, strerror(errno));
		return -1;
	}

	while(state.nodes[node_id] != 0) {
		node_id = (node_id + 1) % RING_NODES;
		checked++;
		if (checked > RING_NODES) {
			logit("Already have %d nodes in system, cannot add\n", RING_NODES);
			return -1;
		}
	}

	state.nodes[node_id] = ip;
	return 0;
}

int init_kernkv(const char *local_ip, unsigned int num_nodes, u16 chain_length, char **nodes)
{
	int err_cache;

	state.chain_length = chain_length;

	if (num_nodes == 0) {
		logit("Must have at least 1 node in system\n");
		return -1;
	}

	state.nodes = malloc(sizeof(u32) * RING_NODES);
	if (!state.nodes) {
		logit("Failed to allocate space for nodes array\n");
		return -1;
	}
	memset(state.nodes, 0, sizeof(u32) * RING_NODES);

	for (unsigned int i = 0; i < num_nodes; i++) {
		if (add_node(nodes[i])) {
			err_cache = EINVAL;
			goto err_free;
		}
	}

	state.local_ip_net_order = convert_ip(local_ip);
	if (state.local_ip_net_order == 0) {
		err_cache = errno;
		logit("Failed to convert '%s' into ip address: %s\n", local_ip, strerror(errno));
		goto err_free;
	}

	state.epfd = epoll_create1(0);
	if (state.epfd < 0) {
		err_cache = errno;
		logit("epoll_create1 failed: %s\n", strerror(errno));
		goto err_free;
	}

	state.started = 1;
	return 0;
err_free:
	free(state.nodes);
	errno = err_cache;
	return -1;
}

void shutdown_kernkv(void)
{
	if (state.nodes) {
		free(state.nodes);
		state.nodes = NULL;
	}

	close(state.epfd);

	state.started = 0;
}

static int get_address(u64 key, int chain_index, struct sockaddr_in *addr)
{
	u8 node_id = RING_HASH(key);
	int checked = 0;

	while (checked < chain_index) {
		if (state.nodes[node_id] != 0) {
			checked++;
			if (checked == chain_index)
				break;
		}
		node_id = (node_id + 1) % RING_NODES;
	}

	/* We want the next valid node in the list */
	while (!state.nodes[node_id])
		node_id = (node_id + 1) % RING_NODES;

	addr->sin_addr.s_addr = state.nodes[node_id];
	addr->sin_family = AF_INET;
	addr->sin_port = htons(CHAIN_PORT);

	return 0;
}

static inline int get_response(u64 id, int sock, struct kv_response *resp)
{
	ssize_t ret;
	struct epoll_event event[2];
	int out = -1;
	int err_cache = 0;

	memset(resp, 0, sizeof(struct kv_response));

	memset(&event, 0, 2 * sizeof(struct epoll_event));
	event[0].events = EPOLLIN;
	if (epoll_ctl(state.epfd, EPOLL_CTL_ADD, sock, &event[0])) {
		err_cache = errno;
		logit("epoll_ctl failed: %s\n", strerror(errno));
		goto close;
	}

	memset(&event, 0, sizeof(struct epoll_event));
	while(event[0].data.fd != sock && event[1].data.fd != sock) {
		ret = epoll_wait(state.epfd, &event[0], 2, RESPONSE_TIMEOUT_MS);
		if (ret < 0) {
			logit("epoll_wait returned error: %s\n", strerror(errno));
			goto remove;
		} else if (ret == 0) {
			logit("Timeout while waiting for response\n");
			goto remove;
		}
	}

	ret = recv(sock, resp, sizeof (struct kv_response), 0);

	if (ret < (ssize_t)MIN_RESPONSE) {
		logit("Incoming repose too small: %s\n", strerror(errno));
		goto remove;
	}

	if (resp->hdr.version != STRUCTURES_VERSION) {
		logit("Version mismatch.  Got %d expected %d\n", resp->hdr.version,
				STRUCTURES_VERSION);
		goto remove;
	}

	if (resp->hdr.request_id != id) {
		logit("Wrong request id returned\n");
		goto remove;
	}

	if (resp->hdr.type != KV_VALUE && resp->hdr.type != KV_SUCCESS) {
		logit("Server returned non-success code\n");
		out = (int)resp->error_code;
		goto remove;
	}

	out = 0;
remove:
	memset(&event, 0, sizeof(struct epoll_event));
	event[0].events = EPOLLIN;
	epoll_ctl(state.epfd, EPOLL_CTL_DEL, sock, &event[0]);

close:
	close(sock);
	errno = err_cache;
	return out;
}

static int setup_response_socket(u64 id)
{
	int sock;
	int err_cache = 0;
	struct sockaddr_in addr;

	sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (sock < 0) {
		err_cache = errno;
		logit("Failed to create incoming socket: %s\n", strerror(errno));
		errno = err_cache;
		return -1;
	}

	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = htons(PORT_FOR_REQ(id));
	addr.sin_family = AF_INET;

	if (bind(sock, (struct sockaddr*)&addr, sizeof(struct sockaddr_in)) == -1) {
		err_cache = errno;
		logit("Failed to bind incoming socket: %s\n", strerror(errno));
		goto close;
	}

	return sock;
close:
	close(sock);
	errno = err_cache;
	return -1;
}

static inline u64 fill_request_header(struct kv_request *req, enum request_type type, u16 length)
{
	u64 req_id = __sync_fetch_and_add(&request_ids, 1);
	req->hdr.version = STRUCTURES_VERSION;
	req->hdr.request_id = req_id;
	req->hdr.type = type;
	req->hdr.client_ip = state.local_ip_net_order;
	req->hdr.hop = 1;
	req->hdr.length = length;
	req->hdr.client_port = htons(PORT_FOR_REQ(req_id));
	return req_id;
}

int get(u64 key, struct value *buf)
{
	u64 req_id;
	struct kv_request req;
	struct sockaddr_in addr;
	struct kv_response resp;
	ssize_t ret = 0;
	int sock;
	int resp_sock;
	int err_cache;

	if (!buf)
		return -1;

	if ((ret = get_address(key, CHAIN_LENGTH, &addr)))
		return -1;

	memset(&req, 0, sizeof(struct kv_request));
	req_id = fill_request_header(&req, KV_GET, 1);
	req.get.key = key;

	resp_sock = setup_response_socket(req_id);
	if (resp_sock < 0) {
		logit("Failed to create response socket: %s\n", strerror(errno));
		return -1;
	}

	sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (sock < 0) {
		logit("Failed to create socket for GET request: %s\n", strerror(errno));
		close(resp_sock);
		return -1;
	}

	ret = connect(sock, (struct sockaddr*)&addr, sizeof(struct sockaddr_in));
	if (ret) {
		err_cache = errno;
		logit("Failed to connect for GET request: %s\n", strerror(errno));
		goto out_close;
	}

	ret = sendto(sock, &req, GET_SIZE, 0,
			(struct sockaddr*)&addr, sizeof(struct sockaddr_in));
	if (ret < (ssize_t)GET_SIZE) {
		err_cache = errno;
		logit("Failed to send GET request: %s\n", strerror(errno));
		goto out_close;
	}

	ret = get_response(req_id, resp_sock, &resp);
	if (ret) {
		err_cache = errno;
		goto out_close;
	}

	close(sock);
	memcpy(buf, &resp.value, sizeof(struct value));
	return 0;
out_close:
	close(resp_sock);
	close(sock);
	errno = err_cache;
	return -1;
}

int put(u64 key, struct value *value)
{
	u64 req_id;
	struct kv_request req;
	struct sockaddr_in addr;
	struct kv_response resp;
	ssize_t ret = 0;
	int sock;
	int resp_sock;
	int err_cache;

	if (!value || !value->len || value->len > MAX_VALUE)
		return -1;

	if ((ret = get_address(key, 0, &addr)))
		return -1;

	memset(&req, 0, sizeof(struct kv_request));
	req_id = fill_request_header(&req, KV_PUT, state.chain_length);
	req.put.key = key;

	resp_sock = setup_response_socket(req_id);
	if (resp_sock < 0) {
		logit("Failed to create response socket: %s\n", strerror(errno));
		return -1;
	}

	req.put.value.len = value->len;
	memcpy((req.put.value.buf), value->buf, value->len);

	sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (sock < 0) {
		close(resp_sock);
		return -1;
	}

	ret = connect(sock, (struct sockaddr*)&addr, sizeof(struct sockaddr_in));
	if (ret) {
		err_cache = errno;
		goto out_close;
	}

	ret = send(sock, &req, PUT_SIZE(value->len), 0);
	if (ret < (ssize_t)PUT_SIZE(value->len)) {
		err_cache = errno;
		goto out_close;
	}

	close(sock);
	return get_response(req_id, resp_sock, &resp);
out_close:
	close(sock);
	errno = err_cache;
	return -1;
}

int del(u64 key)
{
	u64 req_id;
	struct kv_request req;
	struct sockaddr_in addr;
	struct kv_response resp;
	ssize_t ret = 0;
	int sock;
	int resp_sock;
	int err_cache;

	if ((ret = get_address(key, 0, &addr)))
		return -1;

	memset(&req, 0, sizeof(struct kv_request));
	req_id = fill_request_header(&req, KV_DELETE, state.chain_length);
	req.del.key = key;

	resp_sock = setup_response_socket(req_id);
	if (resp_sock < 0) {
		logit("Failed to create response socket: %s\n", strerror(errno));
		return -1;
	}

	sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (sock < 0) {
		close(resp_sock);
		return -1;
	}

	ret = connect(sock, (struct sockaddr*)&addr, sizeof(struct sockaddr_in));
	if (ret) {
		err_cache = errno;
		goto out_close;
	}

	ret = send(sock, &req, DEL_SIZE, 0);
	if (ret < (ssize_t)DEL_SIZE) {
		err_cache = errno;
		goto out_close;
	}

	close(sock);
	return get_response(req_id, resp_sock, &resp);
out_close:
	close(sock);
	close(resp_sock);
	errno = err_cache;
	return -1;
}
