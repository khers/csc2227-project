// SPDX-License-Identifier: GPL-2.0-only

#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/netfilter.h>
#include <linux/netfilter_ipv4.h>
#include <linux/skbuff.h>
#include <linux/udp.h>
#include <linux/socket.h>
#include <linux/uio.h>
#include <linux/ip.h>
#include <linux/inet.h>
#include <asm/string.h>

#ifndef CHAIN_PORT
#define CHAIN_PORT 1345
#endif

static void respond(u32 addr)
{
	struct sockaddr_in send;
	struct socket *socket;
	struct msghdr msg;
	struct kvec vec;
	int ret;

	if (sock_create_kern(&init_net, AF_INET, SOCK_DGRAM, IPPROTO_UDP, &socket))
		return;

	memset(&send, 0, sizeof(struct sockaddr_in));
	send.sin_family = AF_INET;
	send.sin_port = CHAIN_PORT;
	send.sin_addr.s_addr = addr;

	if (socket->ops->connect(socket, (struct sockaddr*)&send, sizeof(struct sockaddr_in), 0)) {
		printk(KERN_WARNING "Failed to connect to receiving host\n");
		goto out_release;
	}

	memset(&msg, 0, sizeof(struct msghdr));
	msg.msg_name = &send;
	msg.msg_namelen = sizeof(struct sockaddr_in);
	msg.msg_iter.type = ITER_KVEC;
	msg.msg_iter.count = 1;
	msg.msg_iter.nr_segs = 1;
	vec.iov_base = kzalloc(9, GFP_NOWAIT);
	if (!vec.iov_base) {
		printk(KERN_WARNING "Failed to allocate memory\n");
		goto out_release;
	}
	strcpy(vec.iov_base,"response");
	vec.iov_len = 9;
	msg.msg_iter.kvec = &vec;
	ret = sock_sendmsg(socket, &msg);
	if (ret < 0) {
		printk(KERN_WARNING "Failed to send\n");
		goto out_free;
	}

out_free:
	kfree(vec.iov_base);
out_release:
	sock_release(socket);
}

static unsigned int packet_interceptor(void *priv, struct sk_buff *skb,
				       const struct nf_hook_state *state)
{
	struct iphdr *ip;
	struct udphdr *udp;

	if (!skb)
		return NF_DROP;

	ip = (struct iphdr *)skb_network_header(skb);
	if (!ip)
		return NF_DROP;

	if (ip->protocol == IPPROTO_UDP) {
		udp = (struct udphdr *)skb_transport_header(skb);
		if (!udp)
			return NF_DROP;

		if (ntohs(udp->dest) == CHAIN_PORT) {
			/* Handle incoming request */
			respond(ntohl(ip->saddr));
			return NF_DROP;
		}
	}

	return NF_ACCEPT;
}

static const struct nf_hook_ops hook_ops = {
	.hook		= packet_interceptor,
	.pf		= NFPROTO_INET,
	.hooknum	= NF_INET_LOCAL_IN,
	.priority	= NF_IP_PRI_FIRST
};

static int init_kern_store(void)
{
	int ret = 0;
	ret = nf_register_net_hook(&init_net, &hook_ops);

	return ret;
}

static void remove_kern_store(void)
{
	nf_unregister_net_hook(&init_net, &hook_ops);
}

module_init(init_kern_store);
module_exit(remove_kern_store);

MODULE_AUTHOR("Eric B. Munson");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("In kernel distributed key/value store");

