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

#ifndef CHAIN_PORT
#define CHAIN_PORT 1345
#endif

#define MAX_MSG 0xFFFF

struct kern_store {
	struct socket *sock;
	struct task_struct *thread;
	int running;
};

static struct kern_store *server_state;

struct incoming {
	struct msghdr hdr;
	struct sockaddr_in addr;
	struct kvec vec;
};

static void respond(struct incoming *msg)
{
	struct socket *socket;
	struct msghdr hdr;
	struct kvec vec;
	mm_segment_t old_fs;
	int ret;

	if (sock_create_kern(&init_net, AF_INET, SOCK_DGRAM, IPPROTO_UDP, &socket))
		return;

	if (socket->ops->connect(socket, (struct sockaddr*)&(msg->addr), sizeof(struct sockaddr_in), 0)) {
		printk(KERN_WARNING "Failed to connect to receiving host\n");
		goto out_release;
	}

	memset(&hdr, 0, sizeof(struct msghdr));
	hdr.msg_name = &(msg->addr);
	hdr.msg_namelen = sizeof(struct sockaddr_in);
	hdr.msg_iter.type = ITER_KVEC;
	hdr.msg_iter.count = 1;
	hdr.msg_iter.nr_segs = 1;
	vec.iov_base = kzalloc(9, GFP_NOWAIT);
	if (!vec.iov_base) {
		printk(KERN_WARNING "Failed to allocate memory\n");
		goto out_release;
	}
	strcpy(vec.iov_base,"response");
	vec.iov_len = 9;
	hdr.msg_iter.kvec = &vec;
	old_fs = get_fs();
	set_fs(KERNEL_DS);
	ret = sock_sendmsg(socket, &hdr);
	set_fs(old_fs);
	if (ret < 0) {
		printk(KERN_WARNING "Failed to send\n");
		goto out_free;
	}

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
	mm_segment_t old_fs;

	current->flags |= PF_NOFREEZE;

	for (;;) {
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

		msg->hdr.msg_name = &msg->addr;
		msg->hdr.msg_namelen = sizeof(struct sockaddr_in);
		msg->hdr.msg_iter.type = ITER_KVEC;
		msg->hdr.msg_iter.count = 1;
		msg->hdr.msg_iter.kvec = &msg->vec;
		msg->hdr.msg_iter.nr_segs = 1;

		old_fs = get_fs();
		set_fs(KERNEL_DS);
		size = sock_recvmsg(server_state->sock, &msg->hdr, 0);
		set_fs(old_fs);

		if (!server_state->running) {
			goto out_buf;
		}

		if (size < 0) {
			printk(KERN_WARNING "Failed to recieve message, skipping\n");
			kfree(msg->vec.iov_base);
			kfree(msg);
			continue;
		}

		respond(msg);
	}

out_buf:
	kfree(msg->vec.iov_base);
out_msg:
	kfree(msg);
out:
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
	if (server_state) {
		server_state->running = 0;
		if (server_state->thread) {
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
}

module_init(init_kern_store);
module_exit(remove_kern_store);

MODULE_AUTHOR("Eric B. Munson");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("In kernel distributed key/value store");

