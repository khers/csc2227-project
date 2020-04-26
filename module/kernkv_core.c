// SPDX-License-Identifier: GPL-2.0

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/skbuff.h>
#include <linux/udp.h>
#include <linux/socket.h>
#include <linux/uio.h>
#include <linux/ip.h>
#include <linux/inet.h>
#include <linux/hashtable.h>
#include <linux/kthread.h>
#include <linux/workqueue.h>
#include <asm/string.h>

#include <kernkv/structures.h>

#include "hashring.h"

MODULE_AUTHOR("Eric B. Munson");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("In kernel distributed key/value store");

static int num_entries = RING_NODES;
static char *node_ips[RING_NODES];
module_param_array(node_ips, charp, &num_entries, 0644);
static char *my_ip;
module_param(my_ip, charp, 0644);

struct kern_store {
	struct socket *sock;
	struct task_struct *thread;
	struct workqueue_struct *wq;
	u32 listen_ip;
	int running;
};

static struct kern_store *server_state;

struct kv_value {
	struct hlist_node node;
	u64 key;
	u64 len;
	void *value;
};

#define KV_BITS 16

static DEFINE_HASHTABLE(kv_store, KV_BITS);
static DEFINE_SRCU(kv_srcu);

struct incoming {
	struct msghdr hdr;
	struct kvec vec;
	struct sockaddr_in addr;
	size_t recvd;
	struct work_struct work;
};

static int send_to(u32 ip, u16 port, char *buf, size_t len)
{
	struct socket *sck;
	struct msghdr hdr;
	struct sockaddr_in addr;
	struct kvec vec;
	size_t left = len, written = 0;
	ssize_t ret;
	mm_segment_t old_fs;

	if (sock_create_kern(&init_net, AF_INET, SOCK_DGRAM, IPPROTO_UDP, &sck)) {
		printk(KERN_WARNING "Failed to create response socket\n");
		return -1;
	}

	memset(&hdr, 0, sizeof(struct msghdr));
	addr.sin_family = AF_INET;
	addr.sin_port = port;
	addr.sin_addr.s_addr = ip;
	hdr.msg_name = &addr;
	hdr.msg_namelen = sizeof(struct sockaddr_in);

	ret = sck->ops->connect(sck, (struct sockaddr*)&addr, sizeof(struct sockaddr_in), 0);
	if (ret < 0) {
		printk(KERN_WARNING "Failed to connect to receiving host, got error code %ld\n", ret);
		goto out_release;
	}

	do {
		vec.iov_len = left;
		vec.iov_base = buf + written;
		old_fs = get_fs();
		set_fs(KERNEL_DS);
		ret = kernel_sendmsg(sck, &hdr, &vec, 1, left);
		set_fs(old_fs);

		if (ret < 0){
			printk(KERN_WARNING "Failed to send, got error code %ld\n", ret);
			goto out_release;
		}

		written += ret;
		left -= ret;
	} while (left);

	sock_release(sck);

	return 0;

out_release:
	sock_release(sck);
	return -1;
}

static int handle_get(const struct kv_request *req)
{
	struct kv_value *iter, *value = NULL;
	u64 key = req->get.key;
	struct kv_response *resp;
	int ret = -1;
	size_t send_size = 0;
	int idx = srcu_read_lock(&kv_srcu);

	hash_for_each_possible_rcu(kv_store, iter, node, key) {
		if (iter->key == key) {
			value = iter;
			break;
		}
	}

	if (!value) {
		resp = kzalloc(MIN_RESPONSE, 0);
		if (!resp)
			goto out_unlock;
		resp->hdr.version = STRUCTURES_VERSION;
		resp->hdr.request_id = req->hdr.request_id;
		resp->hdr.type = KV_NOTFOUND;
		send_size = MIN_RESPONSE;
	} else {
		resp = kzalloc(VALUE_RESPONSE(value->len), 0);
		if (!resp) {
			printk(KERN_WARNING "Cannot allocate memory to return requested value\n");
			goto out_unlock;
		}
		resp->hdr.version = STRUCTURES_VERSION;
		resp->hdr.request_id = req->hdr.request_id;
		resp->hdr.type = KV_VALUE;
		resp->value.len = value->len;
		memcpy(resp->value.buf, value->value, value->len);
		send_size = MIN_RESPONSE + sizeof(u64) + value->len;
	}
	srcu_read_unlock(&kv_srcu, idx);

	ret = send_to(req->hdr.client_ip, req->hdr.client_port, (char *)resp, send_size);
	kfree(resp);
	return ret;

out_unlock:
	srcu_read_unlock(&kv_srcu, idx);
	return ret;
}

static int forward_chain(struct kv_request *req)
{
	size_t send_size = 0;
	u32 dest_ip;

	if (req->hdr.type == KV_PUT) {
		send_size = PUT_SIZE(req->put.value.len);
	} else { /* Must be delete */
		send_size = DEL_SIZE;
	}

	req->hdr.hop++;

	dest_ip = get_next_hop(server_state->listen_ip);
	if (dest_ip == 0) {
		printk(KERN_WARNING "Failed to lookup next hop, aborting forward\n");
		return -1;
	}

	if (send_to(dest_ip, htons(CHAIN_PORT), (char *)req, send_size)) {
		printk(KERN_WARNING "Failed to send forwarded request\n");
		return -1;
	}

	return 0;
}

static int handle_put(struct kv_request *req)
{
	struct kv_value *iter, *value = NULL;
	u64 key = req->put.key;
	struct kv_response *resp;
	void *old_val;
	int idx;
	void *new_val = kzalloc(req->put.value.len, 0);
	int ret = -1;

	if (!new_val) {
		printk(KERN_WARNING "Failed to allocate new value memory\n");
		return ret;
	}
	memcpy(new_val, &(req->put.value.buf), req->put.value.len);

	resp = kzalloc(MIN_RESPONSE, 0);
	if (!resp) {
		printk(KERN_WARNING "Failed to allocate put response memory\n");
		kfree(new_val);
		return ret;
	}

	resp->hdr.version = STRUCTURES_VERSION;
	resp->hdr.request_id = req->hdr.request_id;
	resp->hdr.type = KV_ERROR;

	idx = srcu_read_lock(&kv_srcu);
	hash_for_each_possible_rcu(kv_store, iter, node, key) {
		if (iter->key == key) {
			value = iter;
			break;
		}
	}

	if (value) {
		old_val = value->value;
		value->len = req->put.value.len;
		value->value = new_val;
		srcu_read_unlock(&kv_srcu, idx);
		synchronize_srcu(&kv_srcu);
		kfree(old_val);
	} else {
		srcu_read_unlock(&kv_srcu, idx);
		value = kzalloc(sizeof(struct value), 0);
		if (!value) {
			printk(KERN_WARNING "Failed to allocate new hash node memory\n");
			kfree(new_val);
			goto out_respond;
		}
		value->len = req->put.value.len;
		value->value = new_val;
		value->key = key;
		synchronize_srcu(&kv_srcu);
		hash_add_rcu(kv_store, &(value->node), key);
	}

	if (req->hdr.hop != req->hdr.length) {
		if (forward_chain(req)) {
			goto out_respond;
		}
		ret = 0;
		goto out;
	}

	resp->hdr.type = KV_SUCCESS;
	print_hex_dump(KERN_INFO, "", DUMP_PREFIX_NONE, 16, 4, resp, MIN_RESPONSE, 0);

out_respond:
	ret = send_to(req->hdr.client_ip, req->hdr.client_port, (char *)resp, MIN_RESPONSE);
out:
	kfree(resp);
	return ret;
}

static int handle_delete(struct kv_request *req)
{
	struct kv_value *iter, *value = NULL;
	u64 key = req->get.key;
	struct kv_response *resp;
	int ret = -1;
	int idx = srcu_read_lock(&kv_srcu);

	hash_for_each_possible_rcu(kv_store, iter, node, key) {
		if (iter->key == key) {
			value = iter;
			break;
		}
	}

	resp = kzalloc(MIN_RESPONSE, 0);
	if (!resp)
		return ret;
	resp->hdr.version = STRUCTURES_VERSION;
	resp->hdr.request_id = req->hdr.request_id;
	resp->hdr.type = KV_ERROR;

	srcu_read_unlock(&kv_srcu, idx);

	if (value) {
		hash_del_rcu(&(value->node));
		synchronize_srcu(&kv_srcu);
		kfree(value->value);
		kfree(value);

		if (req->hdr.hop != req->hdr.length) {
			if (forward_chain(req)) {
				send_to(req->hdr.client_ip, req->hdr.client_port,
						(char *)resp, MIN_RESPONSE);
				goto out;
			}
			ret = 0;
			goto out;
		}
	}
	else {
		resp->hdr.type = KV_NOTFOUND;
	}

	resp->hdr.type = KV_SUCCESS;
	ret = send_to(req->hdr.client_ip, req->hdr.client_port, (char *)resp, MIN_RESPONSE);

out:
	kfree(resp);
	return ret;
}

static void respond(struct work_struct *work)
{
	struct kv_request *req;
	int ret = -1;
	struct incoming *msg;

	msg = container_of(work, struct incoming, work);

	if (!msg || !msg->recvd || !msg->vec.iov_len) {
		printk(KERN_WARNING "Received 0 length request recvd %lu iov_len %lu\n",
				msg->recvd, msg->vec.iov_len);
		return;
	}

	req = (struct kv_request*)msg->vec.iov_base;

	if (req->hdr.version != STRUCTURES_VERSION) {
		printk(KERN_ERR "Request version mismatch.  Got %d, expected %d\n",
				req->hdr.version, STRUCTURES_VERSION);
		goto out_free;
	}

	switch (req->hdr.type) {
	case KV_GET:
		ret = handle_get(req);
		break;
	case KV_PUT:
		ret = handle_put(req);
		break;
	case KV_DELETE:
		ret = handle_delete(req);
		break;
	default:
		printk(KERN_WARNING "Invalid request type %d, valid are {%d, %d, %d}\n", req->hdr.type,
				KV_GET, KV_PUT, KV_DELETE);
		goto out_free;
	}

	if (ret)
		printk(KERN_WARNING "Failed to handle request\n");

out_free:
	kfree(msg->vec.iov_base);
	kfree(msg);
}

static int run_server(void *ignored)
{
	struct incoming *msg;
	int size, ret = 0;

	allow_signal(SIGINT);
	allow_signal(SIGKILL);
	allow_signal(SIGQUIT);
	allow_signal(SIGHUP);

	allow_kernel_signal(SIGINT);
	allow_kernel_signal(SIGKILL);
	allow_kernel_signal(SIGQUIT);
	allow_kernel_signal(SIGHUP);

	current->flags |= PF_NOFREEZE;

	while (1) {
		msg = kzalloc(sizeof(struct incoming), 0);
		if (!msg) {
			printk(KERN_ERR "Failed to allocate memory for incoming message, terminating kvvd\n");
			ret = -ENOMEM;
			goto out;
		}

		msg->vec.iov_len = MAX_MSG;
		msg->vec.iov_base = kzalloc(MAX_MSG, 0);
		if (!msg->vec.iov_base) {
			printk(KERN_ERR "Failed to allocate memory for incoming buffer, terminating kvvd\n");
			ret = -ENOMEM;
			goto out_msg;
		}

		INIT_WORK(&msg->work, respond);

retry:
		size = kernel_recvmsg(server_state->sock, &(msg->hdr), &(msg->vec), 1, MAX_MSG, 0);

		if (kthread_should_stop() || !server_state->running) {
			goto out_buf;
		}

		if (size < 0) {
			printk(KERN_WARNING "Failed to recieve message, skipping\n");
			goto retry;
		}

		msg->recvd = size;

		queue_work(server_state->wq, &msg->work);
	}

out_buf:
	kfree(msg->vec.iov_base);
out_msg:
	kfree(msg);
out:
	flush_signals(current);
	return ret;
}

static int init_kern_store(void)
{
	struct sockaddr_in bind_addr;
	int ret = -1;
	int i;

	if (init_hashring(CHAIN_LENGTH))
		return -1;

	for (i = 0; i < num_entries; i++) {
		if (add_node(node_ips[i])) {
			printk("Invalid value given for node ip address: %s\n", node_ips[i]);
			goto out_err;
		}
	}

	server_state = kzalloc(sizeof(struct kern_store), 0);
	if (!server_state) {
		printk(KERN_WARNING "Cannot allocate memory for server state\n");
		ret = -ENOMEM;
		goto out_err;
	}

	if (!my_ip) {
		printk("Missing listen ip address value in my_ip\n");
		goto out_free_state;
	}

	server_state->listen_ip = in_aton(my_ip);
	if (!server_state->listen_ip) {
		printk("Invalid value given for my ip address: %s\n", my_ip);
		goto out_free_state;
	}


	ret = sock_create_kern(&init_net, AF_INET, SOCK_DGRAM, IPPROTO_UDP, &server_state->sock);
	if (ret < 0) {
		printk(KERN_WARNING "Failed to create incoming socket\n");
		goto out_free_state;
	}

	bind_addr.sin_family = AF_INET;
	bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
	bind_addr.sin_port = htons(CHAIN_PORT);
	ret = server_state->sock->ops->bind(server_state->sock, (struct sockaddr*)&bind_addr, sizeof(struct sockaddr_in));
	if (ret) {
		printk(KERN_WARNING "Failed to bind receiving socket\n");
		goto out_release;
	}

	server_state->wq = alloc_workqueue("kkvd-responder", WQ_UNBOUND, 0);
	if (!server_state->wq) {
		ret = -ENOMEM;
		printk(KERN_WARNING "Failed to create responder work queue.\n");
		goto out_release;
	}

	server_state->running = 1;
	server_state->thread = kthread_run(run_server, NULL, "kkvd");
	if (IS_ERR(server_state->thread)) {
		ret = -ENOMEM;
		printk(KERN_WARNING "Failed to start kkvd thread.\n");
		goto work_queue;
	}

	printk(KERN_INFO "KV service started\n");

	return ret;

work_queue:
	destroy_workqueue(server_state->wq);
out_release:
	sock_release(server_state->sock);
out_free_state:
	kfree(server_state);
out_err:
	cleanup_hashring();
	return ret;
}

static void remove_kern_store(void)
{
	struct kv_value *iter;
	int bkt;
	struct hlist_node *tmp;

	cleanup_hashring();

	if (server_state) {
		server_state->running = 0;
		if (server_state->thread) {
			send_sig_info(SIGKILL, SEND_SIG_PRIV, server_state->thread);
			kthread_stop(server_state->thread);
			msleep(10);
			server_state->thread = NULL;
		}

		if (server_state->sock) {
			sock_release(server_state->sock);
			server_state->sock = NULL;
		}

		destroy_workqueue(server_state->wq);

		kfree(server_state);
	}

	hash_for_each_safe(kv_store, bkt, tmp, iter, node) {
		kfree(iter->value);
		kfree(iter);
	}


	printk(KERN_INFO "KV service stopped\n");
}

module_init(init_kern_store);
module_exit(remove_kern_store);

