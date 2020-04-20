/* Copyright (c) 2020 Eric B Munson */

/*
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 2, or 3 of the License,
 *  or (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef KV_STRUCTURES_H
#define KV_STRUCTURES_H

#ifdef __KERNEL__

#include <linux/types.h>

#else

#include <stdint.h>
typedef uint64_t u64;
typedef uint32_t u32;
typedef uint16_t u16;
typedef uint8_t u8;

#endif

#define STRUCTURES_VERSION 2

#ifndef CHAIN_PORT
#define CHAIN_PORT 1345
#endif

#ifndef CLIENT_PORT
#define CLIENT_PORT 1946
#endif

#ifndef RESPONSE_TIMEOUT_MS
#define RESPONSE_TIMEOUT_MS 2000
#endif

#define CHAIN_LENGTH 2

#ifndef MAX_CHAIN
#define MAX_CHAIN 10
#endif

#define MAX_RING_ID (1 << 7)

#define RING_NODES (MAX_RING_ID + 1)

#define RING_HASH(item) (item % RING_NODES)

enum request_type {
	KV_GET = 1,
	KV_PUT,
	KV_DELETE,
};

struct get_request {
	u64 key;
};

struct delete_request {
	u64 key;
};

struct header {
	u64 request_id;
	enum request_type type;
	u32 client_ip;
	u32 version;
	u16 hop;
	u16 length;
};

#define MAX_MSG 0xFFFF
#define BASE_SIZE (sizeof(struct header))
#define MAX_VALUE (MAX_MSG - (BASE_SIZE + 2 * sizeof(u64)))

struct value {
	u64 len;
	u8 buf[MAX_VALUE];
};

struct put_request {
	u64 key;
	struct value value;
};

struct kv_request {
	struct header hdr;
	union {
		struct get_request get;
		struct delete_request del;
		struct put_request put;
	};
};


#define PUT_SIZE(len) (BASE_SIZE + 2 * sizeof(u64) + len)
#define GET_SIZE (BASE_SIZE + sizeof(struct get_request))
#define DEL_SIZE (BASE_SIZE + sizeof(struct delete_request))

enum reponse_type {
	KV_SUCCESS = 1,
	KV_VALUE,
	KV_NOTFOUND,
	KV_ERROR,
};

struct resp_hdr {
	u64 request_id;
	u32 version;
	enum reponse_type type;
};

#define MIN_RESPONSE (sizeof(struct resp_hdr) + sizeof(u32))
#define VALUE_RESPONSE(len) (sizeof(struct resp_hdr) + sizeof(u64) + len)

struct kv_response {
	struct resp_hdr hdr;
	union {
		u32 error_code;
		struct value value;
	};
};

#endif

