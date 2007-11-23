/*
 *	AF_INET6 socket family
 *	Linux INET6 implementation 
 *
 *	Authors:
 *	Pedro Roque		<roque@di.fc.ul.pt>	
 *
 *	Adapted from linux/net/ipv4/af_inet.c
 *
 *	$Id: af_inet6.c,v 1.18 1997/05/07 09:40:12 davem Exp $
 *
 *	This program is free software; you can redistribute it and/or
 *      modify it under the terms of the GNU General Public License
 *      as published by the Free Software Foundation; either version
 *      2 of the License, or (at your option) any later version.
 */


#include <linux/module.h>
#include <linux/config.h>
#include <linux/errno.h>
#include <linux/types.h>
#include <linux/socket.h>
#include <linux/in.h>
#include <linux/kernel.h>
#include <linux/major.h>
#include <linux/sched.h>
#include <linux/timer.h>
#include <linux/string.h>
#include <linux/sockios.h>
#include <linux/net.h>
#include <linux/fcntl.h>
#include <linux/mm.h>
#include <linux/interrupt.h>
#include <linux/proc_fs.h>
#include <linux/stat.h>
#include <linux/init.h>

#include <linux/inet.h>
#include <linux/netdevice.h>
#include <linux/icmpv6.h>

#include <net/ip.h>
#include <net/ipv6.h>
#include <net/udp.h>
#include <net/tcp.h>
#include <net/sit.h>
#include <net/protocol.h>
#include <net/inet_common.h>
#include <net/transp_v6.h>
#include <net/ip6_route.h>
#include <net/addrconf.h>

#include <asm/uaccess.h>
#include <asm/system.h>

extern struct proto_ops inet6_stream_ops;
extern struct proto_ops inet6_dgram_ops;

/* IPv6 procfs goodies... */

#ifdef CONFIG_PROC_FS
extern int raw6_get_info(char *, char **, off_t, int, int);
extern int tcp6_get_info(char *, char **, off_t, int, int);
extern int udp6_get_info(char *, char **, off_t, int, int);
extern int afinet6_get_info(char *, char **, off_t, int, int);
#endif

static int inet6_create(struct socket *sock, int protocol)
{
	struct sock *sk;
	struct proto *prot;

	sk = sk_alloc(GFP_KERNEL);
	if (sk == NULL) 
		goto do_oom;

	/* Note for tcp that also wiped the dummy_th block for us. */
	if(sock->type == SOCK_STREAM || sock->type == SOCK_SEQPACKET) {
		if (protocol && protocol != IPPROTO_TCP) 
			goto free_and_noproto;
		protocol = IPPROTO_TCP;
		sk->no_check = TCP_NO_CHECK;
		prot = &tcpv6_prot;
		sock->ops = &inet6_stream_ops;
	} else if(sock->type == SOCK_DGRAM) {
		if (protocol && protocol != IPPROTO_UDP) 
			goto free_and_noproto;
		protocol = IPPROTO_UDP;
		sk->no_check = UDP_NO_CHECK;
		prot=&udpv6_prot;
		sock->ops = &inet6_dgram_ops;
	} else if(sock->type == SOCK_RAW) {
		if (!suser()) 
			goto free_and_badperm;
		if (!protocol) 
			goto free_and_noproto;
		prot = &rawv6_prot;
		sock->ops = &inet6_dgram_ops;
		sk->reuse = 1;
		sk->num = protocol;
	} else {
		goto free_and_badtype;
	}
	
	sock_init_data(sock, sk);

	sk->zapped		= 0;
	sk->family		= AF_INET6;
	sk->protocol		= protocol;

	sk->prot		= prot;
	sk->backlog_rcv		= prot->backlog_rcv;

	sk->timer.data		= (unsigned long)sk;
	sk->timer.function	= &net_timer;

	sk->net_pinfo.af_inet6.hop_limit  = ipv6_config.hop_limit;
	sk->net_pinfo.af_inet6.mcast_hops = IPV6_DEFAULT_MCASTHOPS;
	sk->net_pinfo.af_inet6.mc_loop	  = 1;

	/* Init the ipv4 part of the socket since we can have sockets
	 * using v6 API for ipv4.
	 */
	sk->ip_ttl	= 64;

	sk->ip_mc_loop	= 1;
	sk->ip_mc_ttl	= 1;
	sk->ip_mc_index	= 0;
	sk->ip_mc_list	= NULL;

	if (sk->type==SOCK_RAW && protocol==IPPROTO_RAW)
		sk->ip_hdrincl=1;

	if (sk->num) {
		/* It assumes that any protocol which allows
		 * the user to assign a number at socket
		 * creation time automatically shares.
		 */
		sk->dummy_th.source = ntohs(sk->num);
		if(sk->prot->hash)
			sk->prot->hash(sk);
		add_to_prot_sklist(sk);
	}

	if (sk->prot->init) {
		int err = sk->prot->init(sk);
		if (err != 0) {
			destroy_sock(sk);
			return(err);
		}
	}
	MOD_INC_USE_COUNT;
	return(0);

free_and_badtype:
	sk_free(sk);
	return -ESOCKTNOSUPPORT;
free_and_badperm:
	sk_free(sk);
	return -EPERM;
free_and_noproto:
	sk_free(sk);
	return -EPROTONOSUPPORT;
do_oom:
	return -ENOBUFS;
}

static int inet6_dup(struct socket *newsock, struct socket *oldsock)
{
	return(inet6_create(newsock, oldsock->sk->protocol));
}

/* bind for INET6 API */
static int inet6_bind(struct socket *sock, struct sockaddr *uaddr, int addr_len)
{
	struct sockaddr_in6 *addr=(struct sockaddr_in6 *)uaddr;
	struct sock *sk = sock->sk;
	__u32 v4addr = 0;
	unsigned short snum = 0;
	int addr_type = 0;

	/* If the socket has its own bind function then use it. */
	if(sk->prot->bind)
		return sk->prot->bind(sk, uaddr, addr_len);
		
	/* Check these errors (active socket, bad address length, double bind). */
	if ((sk->state != TCP_CLOSE)			||
	    (addr_len < sizeof(struct sockaddr_in6))	||
	    (sk->num != 0))
		return -EINVAL;
		
	snum = ntohs(addr->sin6_port);
	if (snum == 0) 
		snum = sk->prot->good_socknum();
	if (snum < PROT_SOCK && !suser()) 
		return(-EACCES);
	
	addr_type = ipv6_addr_type(&addr->sin6_addr);
	if ((addr_type & IPV6_ADDR_MULTICAST) && sock->type == SOCK_STREAM)
		return(-EINVAL);

	/* Check if the address belongs to the host. */
	if (addr_type == IPV6_ADDR_MAPPED) {
		v4addr = addr->sin6_addr.s6_addr32[3];
		if (__ip_chk_addr(v4addr) != IS_MYADDR)
			return(-EADDRNOTAVAIL);
	} else {
		if (addr_type != IPV6_ADDR_ANY) {
			/* ipv4 addr of the socket is invalid.  Only the
			 * unpecified and mapped address have a v4 equivalent.
			 */
			v4addr = LOOPBACK4_IPV6;
			if (!(addr_type & IPV6_ADDR_MULTICAST))	{
				if (ipv6_chk_addr(&addr->sin6_addr) == NULL)
					return(-EADDRNOTAVAIL);
			}
		}
	}

	sk->rcv_saddr = v4addr;
	sk->saddr = v4addr;
		
	memcpy(&sk->net_pinfo.af_inet6.rcv_saddr, &addr->sin6_addr, 
	       sizeof(struct in6_addr));
		
	if (!(addr_type & IPV6_ADDR_MULTICAST))
		memcpy(&sk->net_pinfo.af_inet6.saddr, &addr->sin6_addr, 
		       sizeof(struct in6_addr));

	/* Make sure we are allowed to bind here. */
	if(sk->prot->verify_bind(sk, snum))
		return -EADDRINUSE;

	sk->num = snum;
	sk->dummy_th.source = ntohs(sk->num);
	sk->dummy_th.dest = 0;
	sk->daddr = 0;
	sk->prot->rehash(sk);
	add_to_prot_sklist(sk);

	return(0);
}

static int inet6_release(struct socket *sock, struct socket *peer)
{
	MOD_DEC_USE_COUNT;
	return inet_release(sock, peer);
}

static int inet6_socketpair(struct socket *sock1, struct socket *sock2)
{
	return(-EOPNOTSUPP);
}

/*
 *	This does both peername and sockname.
 */
 
static int inet6_getname(struct socket *sock, struct sockaddr *uaddr,
		 int *uaddr_len, int peer)
{
	struct sockaddr_in6 *sin=(struct sockaddr_in6 *)uaddr;
	struct sock *sk;
  
	sin->sin6_family = AF_INET6;
	sk = sock->sk;
	if (peer) {
		if (!tcp_connected(sk->state))
			return(-ENOTCONN);
		sin->sin6_port = sk->dummy_th.dest;
		memcpy(&sin->sin6_addr, &sk->net_pinfo.af_inet6.daddr,
		       sizeof(struct in6_addr));
	} else {
		if (ipv6_addr_type(&sk->net_pinfo.af_inet6.rcv_saddr) == IPV6_ADDR_ANY)
			memcpy(&sin->sin6_addr, 
			       &sk->net_pinfo.af_inet6.saddr,
			       sizeof(struct in6_addr));
		else
			memcpy(&sin->sin6_addr, 
			       &sk->net_pinfo.af_inet6.rcv_saddr,
			       sizeof(struct in6_addr));

		sin->sin6_port = sk->dummy_th.source;
	}
	*uaddr_len = sizeof(*sin);	
	return(0);
}

static int inet6_ioctl(struct socket *sock, unsigned int cmd, unsigned long arg)
{
	struct sock *sk = sock->sk;
	int err;
	int pid;

	switch(cmd) 
	{
	case FIOSETOWN:
	case SIOCSPGRP:
		err = get_user(pid, (int *) arg);
		if(err)
			return err;

		/* see sock_no_fcntl */
		if (current->pid != pid && current->pgrp != -pid && !suser())
			return -EPERM;
		sk->proc = pid;
		return(0);
	case FIOGETOWN:
	case SIOCGPGRP:
		err = put_user(sk->proc,(int *)arg);
		if(err)
			return err;
		return(0);
	case SIOCGSTAMP:
		if(sk->stamp.tv_sec==0)
			return -ENOENT;
		err = copy_to_user((void *)arg, &sk->stamp,
				   sizeof(struct timeval));
		if (err)
			return -EFAULT;
		return 0;

	case SIOCADDRT:
	case SIOCDELRT:
	  
		return(ipv6_route_ioctl(cmd,(void *)arg));

	case SIOCGIFCONF:
	case SIOCGIFFLAGS:
	case SIOCSIFFLAGS:
	case SIOCADDMULTI:
	case SIOCDELMULTI:
/*

  this ioctls deal with addresses
  must process the addr info before
  calling dev_ioctl to perform dev specific functions

	case SIOCGIFADDR:
	case SIOCSIFADDR:


	case SIOCGIFDSTADDR:

	case SIOCGIFBRDADDR:
	case SIOCSIFBRDADDR:
	case SIOCGIFNETMASK:
	case SIOCSIFNETMASK:
	*/

	case SIOCGIFMETRIC:
	case SIOCSIFMETRIC:
	case SIOCGIFMEM:
	case SIOCSIFMEM:
	case SIOCGIFMTU:
	case SIOCSIFMTU:
	case SIOCSIFLINK:
	case SIOCGIFHWADDR:
	case SIOCSIFHWADDR:
	case SIOCSIFMAP:
	case SIOCGIFMAP:
	case SIOCSIFSLAVE:
	case SIOCGIFSLAVE:
	case SIOGIFINDEX:

		return(dev_ioctl(cmd,(void *) arg));		
		
	case SIOCSIFADDR:
		return addrconf_add_ifaddr((void *) arg);
	case SIOCSIFDSTADDR:
		return addrconf_set_dstaddr((void *) arg);
	default:
		if ((cmd >= SIOCDEVPRIVATE) &&
		    (cmd <= (SIOCDEVPRIVATE + 15)))
			return(dev_ioctl(cmd,(void *) arg));
		
		if (sk->prot->ioctl==NULL) 
			return(-EINVAL);
		return(sk->prot->ioctl(sk, cmd, arg));
	}
	/*NOTREACHED*/
	return(0);
}

struct proto_ops inet6_stream_ops = {
	AF_INET6,

	inet6_dup,
	inet6_release,
	inet6_bind,
	inet_stream_connect,		/* ok		*/
	inet6_socketpair,		/* a do nothing	*/
	inet_accept,			/* ok		*/
	inet6_getname, 
	inet_poll,			/* ok		*/
	inet6_ioctl,			/* must change  */
	inet_listen,			/* ok		*/
	inet_shutdown,			/* ok		*/
	inet_setsockopt,		/* ok		*/
	inet_getsockopt,		/* ok		*/
	sock_no_fcntl,			/* ok		*/
	inet_sendmsg,			/* ok		*/
	inet_recvmsg			/* ok		*/
};

struct proto_ops inet6_dgram_ops = {
	AF_INET6,

	inet6_dup,
	inet6_release,
	inet6_bind,
	inet_dgram_connect,		/* ok		*/
	inet6_socketpair,		/* a do nothing	*/
	inet_accept,			/* ok		*/
	inet6_getname, 
	datagram_poll,			/* ok		*/
	inet6_ioctl,			/* must change  */
	sock_no_listen,			/* ok		*/
	inet_shutdown,			/* ok		*/
	inet_setsockopt,		/* ok		*/
	inet_getsockopt,		/* ok		*/
	sock_no_fcntl,			/* ok		*/
	inet_sendmsg,			/* ok		*/
	inet_recvmsg			/* ok		*/
};

struct net_proto_family inet6_family_ops = {
	AF_INET6,
	inet6_create
};

#ifdef CONFIG_PROC_FS
static struct proc_dir_entry proc_net_raw6 = {
	PROC_NET_RAW6, 4, "raw6",
	S_IFREG | S_IRUGO, 1, 0, 0,
	0, &proc_net_inode_operations,
	raw6_get_info
};
static struct proc_dir_entry proc_net_tcp6 = {
	PROC_NET_TCP6, 4, "tcp6",
	S_IFREG | S_IRUGO, 1, 0, 0,
	0, &proc_net_inode_operations,
	tcp6_get_info
};
static struct proc_dir_entry proc_net_udp6 = {
	PROC_NET_RAW6, 4, "udp6",
	S_IFREG | S_IRUGO, 1, 0, 0,
	0, &proc_net_inode_operations,
	udp6_get_info
};
static struct proc_dir_entry proc_net_sockstat6 = {
	PROC_NET_SOCKSTAT6, 9, "sockstat6",
	S_IFREG | S_IRUGO, 1, 0, 0,
	0, &proc_net_inode_operations,
	afinet6_get_info
};
#endif	/* CONFIG_PROC_FS */

#ifdef MODULE
int ipv6_unload(void)
{
	return 0;
}
#endif

#ifdef MODULE
int init_module(void)
#else
__initfunc(void inet6_proto_init(struct net_proto *pro))
#endif
{
	struct sk_buff *dummy_skb;

#ifdef MODULE
	if (!mod_member_present(&__this_module, can_unload))
	  return -EINVAL;

	__this_module.can_unload = &ipv6_unload;
#endif

	printk(KERN_INFO "IPv6 v0.2 for NET3.037\n");

	if (sizeof(struct ipv6_options) > sizeof(dummy_skb->cb))
	{
		printk(KERN_CRIT "inet6_proto_init: size fault\n");
#ifdef MODULE
		return -EINVAL;
#else
		return;
#endif
	}

  	(void) sock_register(&inet6_family_ops);
	
	/*
	 *	ipngwg API draft makes clear that the correct semantics
	 *	for TCP and UDP is to consider one TCP and UDP instance
	 *	in a host availiable by both INET and INET6 APIs and
	 *	able to communicate via both network protocols.
	 */

	ipv6_init();

	icmpv6_init(&inet6_family_ops);

        addrconf_init();
 
        sit_init();

	/* init v6 transport protocols */

	udpv6_init();
	/* add /proc entries here */

	tcpv6_init();

	/* Create /proc/foo6 entries. */
#ifdef CONFIG_PROC_FS
	proc_net_register(&proc_net_raw6);
	proc_net_register(&proc_net_tcp6);
	proc_net_register(&proc_net_udp6);
	proc_net_register(&proc_net_sockstat6);
#endif

#ifdef MODULE
	return 0;
#endif
}

#ifdef MODULE
void cleanup_module(void)
{
	sit_cleanup();
	ipv6_cleanup();
	sock_unregister(AF_INET6);
#ifdef CONFIG_PROC_FS
	proc_net_unregister(proc_net_raw6.low_ino);
	proc_net_unregister(proc_net_tcp6.low_ino);
	proc_net_unregister(proc_net_udp6.low_ino);
	proc_net_unregister(proc_net_sockstat6.low_ino);
#endif
}
#endif	/* MODULE */