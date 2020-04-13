// SPDX-License-Identifier: GPL-2.0

#include <linux/module.h>
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
#include <asm/string.h>

#include <kernkv/structures.h>

struct kern_store {
	struct socket *sock;
	struct task_struct *thread;
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
};

static size_t handle_get(const struct kv_request *req, struct kv_response **buf)
{
	struct kv_value *iter, *value = NULL;
	u64 key = req->get.key;
	struct kv_response *resp;
	size_t ret = 0;
	int idx = srcu_read_lock(&kv_srcu);

	printk(KERN_INFO "Handling GET\n");
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
		printk(KERN_INFO "Value not found\n");
		resp->request_id = req->request_id;
		resp->type = KV_NOTFOUND;
		ret = MIN_RESPONSE;
	} else {
		resp = kzalloc(MIN_RESPONSE + sizeof(u64) + value->len, 0);
		if (!resp) {
			printk(KERN_WARNING "Cannot allocate memory to return requested value\n");
			goto out_unlock;
		}
		printk(KERN_INFO "Value found\n");
		resp->request_id = req->request_id;
		resp->type = KV_SUCCESS;
		resp->value.len = value->len;
		printk(KERN_INFO "Returning value with length %llu and value of the first byte is %d\n",
				value->len, ((char *)value->value)[0]);
		memcpy(resp->value.buf, value->value, value->len);
		ret = MIN_RESPONSE + sizeof(u64) + value->len;
	}

	*buf = resp;
out_unlock:
	srcu_read_unlock(&kv_srcu, idx);
	return ret;
}

static size_t handle_put(const struct kv_request *req, struct kv_response **buf)
{
	struct kv_value *iter, *value = NULL;
	u64 key = req->get.key;
	struct kv_response *resp;
	void *old_val;
	int idx;
	void *new_val = kzalloc(req->put.value.len, 0);

	if (!new_val) {
		printk(KERN_WARNING "Failed to allocate new value memory\n");
		return 0;
	}
	memcpy(new_val, &(req->put.value.buf), req->put.value.len);

	printk(KERN_INFO "Handling PUT request\n");

	resp = kzalloc(MIN_RESPONSE, 0);
	if (!resp) {
		printk(KERN_WARNING "Failed to allocate put response memory\n");
		kfree(new_val);
		return 0;
	}

	resp->request_id = req->request_id;
	resp->type = KV_ERROR;
	*buf = resp;

	idx = srcu_read_lock(&kv_srcu);
	hash_for_each_possible_rcu(kv_store, iter, node, key) {
		if (iter->key == key) {
			value = iter;
			break;
		}
	}

	if (value) {
		old_val = value->value;
		srcu_read_unlock(&kv_srcu, idx);
		printk(KERN_INFO "Replacing existing value in hash table\n");
		synchronize_srcu(&kv_srcu);
		value->len = req->put.value.len;
		value->value = new_val;
		kfree(old_val);
	} else {
		srcu_read_unlock(&kv_srcu, idx);
		value = kzalloc(sizeof(struct value), 0);
		if (!value) {
			printk(KERN_WARNING "Failed to allocate new hash node memory\n");
			kfree(new_val);
			return MIN_RESPONSE;
		}
		printk(KERN_INFO "Inserting new value into hash table\n");
		value->len = req->put.value.len;
		value->value = new_val;
		value->key = key;
		printk(KERN_INFO "Inserting value with length %llu and value of the first byte is %d\n",
				value->len, ((char *)value->value)[0]);
		synchronize_srcu(&kv_srcu);
		hash_add_rcu(kv_store, &(value->node), key);
	}

	printk(KERN_INFO "Done\n");
	resp->type = KV_SUCCESS;
	return MIN_RESPONSE;
}

static size_t handle_delete(const struct kv_request *req, struct kv_response **buf)
{
	struct kv_value *iter, *value = NULL;
	u64 key = req->get.key;
	struct kv_response *resp;
	size_t ret = 0;
	int idx = srcu_read_lock(&kv_srcu);

	printk(KERN_INFO "Handling DELETE request\n");

	hash_for_each_possible_rcu(kv_store, iter, node, key) {
		if (iter->key == key) {
			value = iter;
			break;
		}
	}

	resp = kzalloc(MIN_RESPONSE, 0);
	if (!resp)
		return ret;
	ret = MIN_RESPONSE;
	resp->request_id = req->request_id;

	srcu_read_unlock(&kv_srcu, idx);

	if (value) {
		hash_del_rcu(&(value->node));
		resp->type = KV_SUCCESS;
		synchronize_srcu(&kv_srcu);
		kfree(value->value);
		kfree(value);
		printk(KERN_INFO "Value deleted\n");
	}
	else {
		printk(KERN_INFO "Value not found\n");
		resp->type = KV_NOTFOUND;
	}

	*buf = resp;
	printk(KERN_INFO "Done\n");
	return ret;
}

static void respond(struct incoming *msg)
{
	struct socket *socket;
	struct msghdr hdr;
	struct kvec vec;
	struct kv_request *req;
	size_t left, written = 0;
	char *buf = NULL;
	int ret;
	mm_segment_t old_fs;

	if (!msg->recvd || !msg->vec.iov_len) {
		printk(KERN_WARNING "Received 0 length request recvd %lu iov_len %lu\n",
				msg->recvd, msg->vec.iov_len);
		return;
	}

	req = (struct kv_request*)msg->vec.iov_base;

	switch (req->type) {
	case KV_GET:
		left = handle_get(req, (struct kv_response **)&buf);
		break;
	case KV_PUT:
		left = handle_put(req, (struct kv_response **)&buf);
		break;
	case KV_DELETE:
		left = handle_delete(req, (struct kv_response **)&buf);
		break;
	default:
		printk(KERN_WARNING "Invalid request type %d, valid are {%d, %d, %d}\n", req->type,
				KV_GET, KV_PUT, KV_DELETE);
		return;
	}

	if (sock_create_kern(&init_net, AF_INET, SOCK_DGRAM, IPPROTO_UDP, &socket)) {
		printk(KERN_WARNING "Failed to create response socket\n");
		goto out_free;
	}

	memset(&hdr, 0, sizeof(struct msghdr));
	msg->addr.sin_port = htons(CLIENT_PORT);
	msg->addr.sin_addr.s_addr = req->client_ip;
	hdr.msg_name = &msg->addr;
	printk(KERN_INFO "Responding to %u\n", msg->addr.sin_addr.s_addr);
	hdr.msg_namelen = sizeof(struct sockaddr_in);

	if (socket->ops->connect(socket, (struct sockaddr*)&(msg->addr), sizeof(struct sockaddr_in), 0)) {
		printk(KERN_WARNING "Failed to connect to receiving host\n");
		goto out_release;
	}

	do {
		vec.iov_len = left;
		vec.iov_base = buf + written;
		printk(KERN_INFO "Sending response\n");
		old_fs = get_fs();
		set_fs(KERNEL_DS);
		ret = kernel_sendmsg(socket, &hdr, &vec, 1, left);
		set_fs(old_fs);

		if (ret < 0){
			printk(KERN_WARNING "Failed to send, got error code %d\n", ret);
			goto out_release;
		}

		written += ret;
		left -= ret;
	} while (left);

	printk(KERN_INFO "Done, sent %lu bytes queued %lu\n", written, vec.iov_len);

out_free:
	kfree(vec.iov_base);
out_release:
	sock_release(socket);
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
			printk(KERN_ERR "Failed to allocate memory for incoming message, ternimating kvvd\n");
			ret = -ENOMEM;
			goto out;
		}

		msg->vec.iov_len = MAX_MSG;
		msg->vec.iov_base = kzalloc(MAX_MSG, 0);
		if (!msg->vec.iov_base) {
			printk(KERN_ERR "Failed to allocate memory for incoming buffer, ternimating kvvd\n");
			ret = -ENOMEM;
			goto out_msg;
		}

		printk(KERN_INFO "Waiting on incoming requests\n");
		size = kernel_recvmsg(server_state->sock, &(msg->hdr), &(msg->vec), 1, MAX_MSG, 0);

		printk(KERN_INFO "Wait interrupted\n");

		if (kthread_should_stop() || !server_state->running) {
			goto out_buf;
		}

		if (size < 0) {
			printk(KERN_WARNING "Failed to recieve message, skipping\n");
			kfree(msg->vec.iov_base);
			kfree(msg);
			continue;
		}

		printk("Got Message, handling\n");

		msg->recvd = size;

		respond(msg);
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
	int ret = 0;
	server_state = kzalloc(sizeof(struct kern_store), 0);
	if (!server_state) {
		printk(KERN_WARNING "Cannot allocate memory for server state\n");
		ret = -ENOMEM;
		goto out_err;
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

	server_state->running = 1;
	server_state->thread = kthread_run(run_server, NULL, "kkvd");
	if (IS_ERR(server_state->thread)) {
		ret = -ENOMEM;
		printk(KERN_WARNING "Failed to start kkvd thread.\n");
		goto out_release;
	}

	printk(KERN_INFO "KV service started\n");

	return ret;

out_release:
	sock_release(server_state->sock);
out_free_state:
	kfree(server_state);
out_err:
	return ret;
}

static void remove_kern_store(void)
{
	struct kv_value *iter;
	int bkt;
	struct hlist_node *tmp;

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

MODULE_AUTHOR("Eric B. Munson");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("In kernel distributed key/value store");

