// SPDX-License-Identifier: GPL-2.0-only
/*
 * common LSM auditing functions
 *
 * Based on code written for SELinux by :
 *			Stephen Smalley
 * 			James Morris <jmorris@redhat.com>
 * Author : Etienne Basset, <etienne.basset@ensta.org>
 */

#include <linux/types.h>
#include <linux/stddef.h>
#include <linux/kernel.h>
#include <linux/gfp.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <net/sock.h>
#include <linux/un.h>
#include <net/af_unix.h>
#include <linux/audit.h>
#include <linux/ipv6.h>
#include <linux/ip.h>
#include <net/ip.h>
#include <net/ipv6.h>
#include <linux/tcp.h>
#include <linux/udp.h>
#include <linux/sctp.h>
#include <linux/lsm_audit.h>
#include <linux/security.h>

/**
 * ipv4_skb_to_auditdata : fill auditdata from skb
 * @skb : the skb
 * @ad : the audit data to fill
 * @proto : the layer 4 protocol
 *
 * return  0 on success
 */
int ipv4_skb_to_auditdata(struct sk_buff *skb,
		struct common_audit_data *ad, u8 *proto)
{
	int ret = 0;
	struct iphdr *ih;

	ih = ip_hdr(skb);
	ad->u.net->v4info.saddr = ih->saddr;
	ad->u.net->v4info.daddr = ih->daddr;

	if (proto)
		*proto = ih->protocol;
	/* non initial fragment */
	if (ntohs(ih->frag_off) & IP_OFFSET)
		return 0;

	switch (ih->protocol) {
	case IPPROTO_TCP: {
		struct tcphdr *th = tcp_hdr(skb);

		ad->u.net->sport = th->source;
		ad->u.net->dport = th->dest;
		break;
	}
	case IPPROTO_UDP: {
		struct udphdr *uh = udp_hdr(skb);

		ad->u.net->sport = uh->source;
		ad->u.net->dport = uh->dest;
		break;
	}
	case IPPROTO_SCTP: {
		struct sctphdr *sh = sctp_hdr(skb);

		ad->u.net->sport = sh->source;
		ad->u.net->dport = sh->dest;
		break;
	}
	default:
		ret = -EINVAL;
	}
	return ret;
}
#if IS_ENABLED(CONFIG_IPV6)
/**
 * ipv6_skb_to_auditdata : fill auditdata from skb
 * @skb : the skb
 * @ad : the audit data to fill
 * @proto : the layer 4 protocol
 *
 * return  0 on success
 */
int ipv6_skb_to_auditdata(struct sk_buff *skb,
		struct common_audit_data *ad, u8 *proto)
{
	int offset, ret = 0;
	struct ipv6hdr *ip6;
	u8 nexthdr;
	__be16 frag_off;

	ip6 = ipv6_hdr(skb);
	ad->u.net->v6info.saddr = ip6->saddr;
	ad->u.net->v6info.daddr = ip6->daddr;
	/* IPv6 can have several extension header before the Transport header
	 * skip them */
	offset = skb_network_offset(skb);
	offset += sizeof(*ip6);
	nexthdr = ip6->nexthdr;
	offset = ipv6_skip_exthdr(skb, offset, &nexthdr, &frag_off);
	if (offset < 0)
		return 0;
	if (proto)
		*proto = nexthdr;
	switch (nexthdr) {
	case IPPROTO_TCP: {
		struct tcphdr _tcph, *th;

		th = skb_header_pointer(skb, offset, sizeof(_tcph), &_tcph);
		if (th == NULL)
			break;

		ad->u.net->sport = th->source;
		ad->u.net->dport = th->dest;
		break;
	}
	case IPPROTO_UDP: {
		struct udphdr _udph, *uh;

		uh = skb_header_pointer(skb, offset, sizeof(_udph), &_udph);
		if (uh == NULL)
			break;

		ad->u.net->sport = uh->source;
		ad->u.net->dport = uh->dest;
		break;
	}
	case IPPROTO_SCTP: {
		struct sctphdr _sctph, *sh;

		sh = skb_header_pointer(skb, offset, sizeof(_sctph), &_sctph);
		if (sh == NULL)
			break;
		ad->u.net->sport = sh->source;
		ad->u.net->dport = sh->dest;
		break;
	}
	default:
		ret = -EINVAL;
	}
	return ret;
}
#endif


static inline void print_ipv6_addr(struct audit_buffer *ab,
				   const struct in6_addr *addr, __be16 port,
				   const char *name1, const char *name2)
{
	if (!ipv6_addr_any(addr))
		audit_log_format(ab, " %s=%pI6c", name1, addr);
	if (port)
		audit_log_format(ab, " %s=%d", name2, ntohs(port));
}

static inline void print_ipv4_addr(struct audit_buffer *ab, __be32 addr,
				   __be16 port, const char *name1, const char *name2)
{
	if (addr)
		audit_log_format(ab, " %s=%pI4", name1, &addr);
	if (port)
		audit_log_format(ab, " %s=%d", name2, ntohs(port));
}

/**
 * audit_log_lsm_data - helper to log common LSM audit data
 * @ab : the audit buffer
 * @a : common audit data
 */
void audit_log_lsm_data(struct audit_buffer *ab,
			const struct common_audit_data *a)
{
	/*
	 * To keep stack sizes in check force programmers to notice if they
	 * start making this union too large!  See struct lsm_network_audit
	 * as an example of how to deal with large data.
	 */
	BUILD_BUG_ON(sizeof(a->u) > sizeof(void *)*2);

	switch (a->type) {
	case LSM_AUDIT_DATA_NONE:
		return;
	case LSM_AUDIT_DATA_IPC:
		audit_log_format(ab, " ipc_key=%d ", a->u.ipc_id);
		break;
	case LSM_AUDIT_DATA_CAP:
		audit_log_format(ab, " capability=%d ", a->u.cap);
		break;
	case LSM_AUDIT_DATA_PATH: {
		struct inode *inode;

		audit_log_d_path(ab, " path=", &a->u.path);

		inode = d_backing_inode(a->u.path.dentry);
		if (inode) {
			audit_log_format(ab, " dev=");
			audit_log_untrustedstring(ab, inode->i_sb->s_id);
			audit_log_format(ab, " ino=%lu", inode->i_ino);
		}
		break;
	}
	case LSM_AUDIT_DATA_FILE: {
		struct inode *inode;

		audit_log_d_path(ab, " path=", &a->u.file->f_path);

		inode = file_inode(a->u.file);
		if (inode) {
			audit_log_format(ab, " dev=");
			audit_log_untrustedstring(ab, inode->i_sb->s_id);
			audit_log_format(ab, " ino=%lu", inode->i_ino);
		}
		break;
	}
	case LSM_AUDIT_DATA_IOCTL_OP: {
		struct inode *inode;

		audit_log_d_path(ab, " path=", &a->u.op->path);

		inode = a->u.op->path.dentry->d_inode;
		if (inode) {
			audit_log_format(ab, " dev=");
			audit_log_untrustedstring(ab, inode->i_sb->s_id);
			audit_log_format(ab, " ino=%lu", inode->i_ino);
		}

		audit_log_format(ab, " ioctlcmd=0x%hx", a->u.op->cmd);
		break;
	}
	case LSM_AUDIT_DATA_DENTRY: {
		struct inode *inode;

		audit_log_format(ab, " name=");
		spin_lock(&a->u.dentry->d_lock);
		audit_log_untrustedstring(ab, a->u.dentry->d_name.name);
		spin_unlock(&a->u.dentry->d_lock);

		inode = d_backing_inode(a->u.dentry);
		if (inode) {
			audit_log_format(ab, " dev=");
			audit_log_untrustedstring(ab, inode->i_sb->s_id);
			audit_log_format(ab, " ino=%lu", inode->i_ino);
		}
		break;
	}
	case LSM_AUDIT_DATA_INODE: {
		struct dentry *dentry;
		struct inode *inode;

		rcu_read_lock();
		inode = a->u.inode;
		dentry = d_find_alias_rcu(inode);
		if (dentry) {
			audit_log_format(ab, " name=");
			spin_lock(&dentry->d_lock);
			audit_log_untrustedstring(ab, dentry->d_name.name);
			spin_unlock(&dentry->d_lock);
		}
		audit_log_format(ab, " dev=");
		audit_log_untrustedstring(ab, inode->i_sb->s_id);
		audit_log_format(ab, " ino=%lu", inode->i_ino);
		rcu_read_unlock();
		break;
	}
	case LSM_AUDIT_DATA_TASK: {
		struct task_struct *tsk = a->u.tsk;
		if (tsk) {
			pid_t pid = task_tgid_nr(tsk);
			if (pid) {
				char tskcomm[sizeof(tsk->comm)];
				audit_log_format(ab, " opid=%d ocomm=", pid);
				audit_log_untrustedstring(ab,
				    get_task_comm(tskcomm, tsk));
			}
		}
		break;
	}
	case LSM_AUDIT_DATA_NET:
		if (a->u.net->sk) {
			const struct sock *sk = a->u.net->sk;
			const struct unix_sock *u;
			struct unix_address *addr;
			int len = 0;
			char *p = NULL;

			switch (sk->sk_family) {
			case AF_INET: {
				const struct inet_sock *inet = inet_sk(sk);

				print_ipv4_addr(ab, inet->inet_rcv_saddr,
						inet->inet_sport,
						"laddr", "lport");
				print_ipv4_addr(ab, inet->inet_daddr,
						inet->inet_dport,
						"faddr", "fport");
				break;
			}
#if IS_ENABLED(CONFIG_IPV6)
			case AF_INET6: {
				const struct inet_sock *inet = inet_sk(sk);

				print_ipv6_addr(ab, &sk->sk_v6_rcv_saddr,
						inet->inet_sport,
						"laddr", "lport");
				print_ipv6_addr(ab, &sk->sk_v6_daddr,
						inet->inet_dport,
						"faddr", "fport");
				break;
			}
#endif
			case AF_UNIX:
				u = unix_sk(sk);
				addr = smp_load_acquire(&u->addr);
				if (!addr)
					break;
				if (u->path.dentry) {
					audit_log_d_path(ab, " path=", &u->path);
					break;
				}
				len = addr->len-sizeof(short);
				p = &addr->name->sun_path[0];
				audit_log_format(ab, " path=");
				if (*p)
					audit_log_untrustedstring(ab, p);
				else
					audit_log_n_hex(ab, p, len);
				break;
			}
		}

		switch (a->u.net->family) {
		case AF_INET:
			print_ipv4_addr(ab, a->u.net->v4info.saddr,
					a->u.net->sport,
					"saddr", "src");
			print_ipv4_addr(ab, a->u.net->v4info.daddr,
					a->u.net->dport,
					"daddr", "dest");
			break;
		case AF_INET6:
			print_ipv6_addr(ab, &a->u.net->v6info.saddr,
					a->u.net->sport,
					"saddr", "src");
			print_ipv6_addr(ab, &a->u.net->v6info.daddr,
					a->u.net->dport,
					"daddr", "dest");
			break;
		}
		if (a->u.net->netif > 0) {
			struct net_device *dev;

			/* NOTE: we always use init's namespace */
			dev = dev_get_by_index(&init_net, a->u.net->netif);
			if (dev) {
				audit_log_format(ab, " netif=%s", dev->name);
				dev_put(dev);
			}
		}
		break;
#ifdef CONFIG_KEYS
	case LSM_AUDIT_DATA_KEY:
		audit_log_format(ab, " key_serial=%u", a->u.key_struct.key);
		if (a->u.key_struct.key_desc) {
			audit_log_format(ab, " key_desc=");
			audit_log_untrustedstring(ab, a->u.key_struct.key_desc);
		}
		break;
#endif
	case LSM_AUDIT_DATA_KMOD:
		audit_log_format(ab, " kmod=");
		audit_log_untrustedstring(ab, a->u.kmod_name);
		break;
	case LSM_AUDIT_DATA_IBPKEY: {
		struct in6_addr sbn_pfx;

		memset(&sbn_pfx.s6_addr, 0,
		       sizeof(sbn_pfx.s6_addr));
		memcpy(&sbn_pfx.s6_addr, &a->u.ibpkey->subnet_prefix,
		       sizeof(a->u.ibpkey->subnet_prefix));
		audit_log_format(ab, " pkey=0x%x subnet_prefix=%pI6c",
				 a->u.ibpkey->pkey, &sbn_pfx);
		break;
	}
	case LSM_AUDIT_DATA_IBENDPORT:
		audit_log_format(ab, " device=%s port_num=%u",
				 a->u.ibendport->dev_name,
				 a->u.ibendport->port);
		break;
	case LSM_AUDIT_DATA_LOCKDOWN:
		audit_log_format(ab, " lockdown_reason=\"%s\"",
				 lockdown_reasons[a->u.reason]);
		break;
	case LSM_AUDIT_DATA_ANONINODE:
		audit_log_format(ab, " anonclass=%s", a->u.anonclass);
		break;
	case LSM_AUDIT_DATA_NLMSGTYPE:
		audit_log_format(ab, " nl-msgtype=%hu", a->u.nlmsg_type);
		break;
	} /* switch (a->type) */
}

/**
 * dump_common_audit_data - helper to dump common audit data
 * @ab : the audit buffer
 * @a : common audit data
 */
static void dump_common_audit_data(struct audit_buffer *ab,
				   const struct common_audit_data *a)
{
	char comm[sizeof(current->comm)];

	audit_log_format(ab, " pid=%d comm=", task_tgid_nr(current));
	audit_log_untrustedstring(ab, get_task_comm(comm, current));
	audit_log_lsm_data(ab, a);
}

/**
 * common_lsm_audit - generic LSM auditing function
 * @a:  auxiliary audit data
 * @pre_audit: lsm-specific pre-audit callback
 * @post_audit: lsm-specific post-audit callback
 *
 * setup the audit buffer for common security information
 * uses callback to print LSM specific information
 */
void common_lsm_audit(struct common_audit_data *a,
	void (*pre_audit)(struct audit_buffer *, void *),
	void (*post_audit)(struct audit_buffer *, void *))
{
	struct audit_buffer *ab;

	if (a == NULL)
		return;
	/* we use GFP_ATOMIC so we won't sleep */
	ab = audit_log_start(audit_context(), GFP_ATOMIC | __GFP_NOWARN,
			     AUDIT_AVC);

	if (ab == NULL)
		return;

	if (pre_audit)
		pre_audit(ab, a);

	dump_common_audit_data(ab, a);

	if (post_audit)
		post_audit(ab, a);

	audit_log_end(ab);
}
