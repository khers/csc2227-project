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

#ifndef CHAIN_PORT
#define CHAIN_PORT 1345
#endif

#ifndef CLIENT_PORT
#define CLIENT_PORT 1346
#endif

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

#define MAX_MSG 0xFFFF
#define BASE_SIZE (sizeof(u64) + sizeof(enum request_type) + sizeof(u32))
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
	u64 request_id;
	enum request_type type;
	u32 client_ip;
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
	KV_NOTFOUND,
	KV_ERROR,
};

#define MIN_RESPONSE (sizeof(u64) + sizeof(enum reponse_type) + sizeof(u32))

struct kv_response {
	u64 request_id;
	enum reponse_type type;
	union {
		u32 error_code;
		struct value value;
	};
};

#endif

