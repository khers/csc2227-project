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
	char *hostname;
	u32 local_ip_net_order;
	int listen_sock;
	int started;
	int epfd;
};

static struct system_state state;

static u64 request_ids;

static void logit(const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	fprintf(stderr, fmt, args);
	va_end(args);
}

static u32 convert_ip(const char *ip)
{
	u32 ret = 0;
	int err = inet_pton(AF_INET, ip, (void*)&ret);
	/* inet_pton does not follow the "standard" return value pattern and return 1 on success */
	if (err != 1) {
		return 0;
	}
	return ret;
}

int init_kernkv(const char *local_ip, const char *hostname)
{
	struct sockaddr_in addr;
	int err_cache;
	struct epoll_event event;

	state.hostname = strndup(hostname, 253);
	if (!state.hostname) {
		logit("Failed to allocate memory for hostname\n");
		return -1;
	}

	state.local_ip_net_order = convert_ip(local_ip);
	if (state.local_ip_net_order == 0) {
		err_cache = errno;
		logit("Failed to convert '%s' into ip address: %s\n", local_ip, strerror(errno));
		goto err_free;
	}

	state.listen_sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (state.listen_sock < 0) {
		err_cache = errno;
		logit("Failed to create incoming socket: %s\n", strerror(errno));
		goto err_free;
	}

	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = htons(CLIENT_PORT);
	addr.sin_family = AF_INET;

	if (bind(state.listen_sock, (struct sockaddr*)&addr, sizeof(struct sockaddr_in)) == -1) {
		err_cache = errno;
		logit("Failed to bind incoming socket: %s\n", strerror(errno));
		goto err_close;
	}

	state.epfd = epoll_create1(0);
	if (state.epfd < 0) {
		err_cache = errno;
		logit("epoll_create1 failed: %s\n", strerror(errno));
		goto err_close;
	}

	memset(&event, 0, sizeof(struct epoll_event));
	event.events = EPOLLIN;
	if (epoll_ctl(state.epfd, EPOLL_CTL_ADD, state.listen_sock, &event)) {
		err_cache = errno;
		logit("epoll_ctl failed: %s\n", strerror(errno));
		goto err_ep_close;
	}

	state.started = 1;
	return 0;
err_ep_close:
	close(state.epfd);
err_close:
	close(state.listen_sock);
err_free:
	free(state.hostname);
	errno = err_cache;
	return -1;
}

void shutdown_kernkv(void)
{
	if (state.hostname) {
		free(state.hostname);
		state.hostname = NULL;
	}

	close(state.listen_sock);

	state.started = 0;
}

static int resolve_addr(const char *hostname, struct sockaddr_in *addr)
{
	struct addrinfo *info;
	struct addrinfo hints;
	int ret;

	if (!addr)
		return -1;

	memset(&hints, 0, sizeof(struct addrinfo));
	hints.ai_family = AF_INET;
	if ((ret = getaddrinfo(hostname, NULL, &hints, &info)) != 0) {
		return ret;
	}

	memcpy(&(info->ai_addr), addr, info->ai_addrlen);
	addr->sin_family = AF_INET;
	addr->sin_port = htons(CHAIN_PORT);

	return 0;
}

static int get_address(struct sockaddr_in *addr)
{
	u32 ip;
	int ret = 0;

	if ((ip = convert_ip(state.hostname)) == 0) {
		if ((ret = resolve_addr(state.hostname, addr)) != 0)
			return ret;
		if (addr->sin_addr.s_addr ==0)
			return 1;
	} else {
		addr->sin_addr.s_addr = ip;
		addr->sin_family = AF_INET;
		addr->sin_port = htons(CHAIN_PORT);
	}
	return ret;
}

static inline int get_response(u64 id, struct kv_response *resp)
{
	ssize_t ret;
	struct epoll_event event;

	memset(resp, 0, sizeof(struct kv_response));

	memset(&event, 0, sizeof(struct epoll_event));
	ret = epoll_wait(state.epfd, &event, 1, RESPONSE_TIMEOUT_MS);
	if (ret < 0) {
		logit("epoll_wait returned error: %s\n", strerror(errno));
		return -1;
	} else if (ret == 0) {
		logit("Timeout while waiting for response\n");
		return -1;
	}

	ret = recv(state.listen_sock, resp, sizeof (struct kv_response), 0);

	if (ret < (ssize_t)MIN_RESPONSE) {
		logit("Incoming repose too small: %s\n", strerror(errno));
		return -1;
	}

	if (resp->version != STRUCTURES_VERSION) {
		logit("Version mismatch.  Got %d expected %d\n", resp->version,
				STRUCTURES_VERSION);
		return -1;
	}

	if (resp->request_id != id) {
		logit("Wrong request id returned\n");
		return -1;
	}

	if (resp->type != KV_SUCCESS) {
		logit("Server returned non-success code\n");
		return (int)resp->error_code;
	}

	return 0;
}

int get(u64 key, struct value *buf)
{
	u64 req_id = __sync_fetch_and_add(&request_ids, 1);
	struct kv_request req;
	struct sockaddr_in addr;
	struct kv_response resp;
	ssize_t ret = 0;
	int sock;
	int err_cache;

	if (!buf)
		return -1;

	if ((ret = get_address(&addr)))
		return -1;

	memset(&req, 0, sizeof(struct kv_request));
	req.version = STRUCTURES_VERSION;
	req.request_id = req_id;
	req.type = KV_GET;
	req.client_ip = state.local_ip_net_order;
	req.get.key = key;

	sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (sock < 0) {
		logit("Failed to create socket for GET request: %s\n", strerror(errno));
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
	if (ret < 0 || ret != GET_SIZE) {
		err_cache = errno;
		logit("Failed to send GET request: %s\n", strerror(errno));
		goto out_close;
	}

	ret = get_response(req_id, &resp);
	if (ret) {
		err_cache = errno;
		goto out_close;
	}

	close(sock);
	memcpy(buf, &resp.value, sizeof(struct value));
	return 0;
out_close:
	close(sock);
	errno = err_cache;
	return -1;
}

int put(u64 key, struct value *value)
{
	u64 req_id = __sync_fetch_and_add(&request_ids, 1);
	struct kv_request req;
	struct sockaddr_in addr;
	struct kv_response resp;
	ssize_t ret = 0;
	int sock;
	int err_cache;

	if (!value || !value->len || value->len > MAX_VALUE)
		return -1;

	if ((ret = get_address(&addr)))
		return -1;

	memset(&req, 0, sizeof(struct kv_request));
	req.version = STRUCTURES_VERSION;
	req.request_id = req_id;
	req.type = KV_PUT;
	req.client_ip = state.local_ip_net_order;
	req.put.key = key;

	req.put.value.len = value->len;
	memcpy((req.put.value.buf), value->buf, value->len);

	sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (sock < 0)
		return -1;

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
	return get_response(req_id, &resp);
out_close:
	close(sock);
	errno = err_cache;
	return -1;
}

int del(u64 key)
{
	u64 req_id = __sync_fetch_and_add(&request_ids, 1);
	struct kv_request req;
	struct sockaddr_in addr;
	struct kv_response resp;
	ssize_t ret = 0;
	int sock;
	int err_cache;

	if ((ret = get_address(&addr)))
		return -1;

	memset(&req, 0, sizeof(struct kv_request));
	req.version = STRUCTURES_VERSION;
	req.request_id = req_id;
	req.type = KV_DELETE;
	req.client_ip = state.local_ip_net_order;
	req.del.key = key;

	sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (sock < 0)
		return -1;

	ret = connect(sock, (struct sockaddr*)&addr, sizeof(struct sockaddr_in));
	if (ret) {
		err_cache = errno;
		goto out_close;
	}

	ret = send(sock, &req, DEL_SIZE, 0);
	if (ret < 0 || ret != DEL_SIZE) {
		err_cache = errno;
		goto out_close;
	}

	close(sock);
	return get_response(req_id, &resp);
out_close:
	close(sock);
	errno = err_cache;
	return -1;
}
