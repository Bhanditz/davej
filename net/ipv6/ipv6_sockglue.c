/*
 *	IPv6 BSD socket options interface
 *	Linux INET6 implementation 
 *
 *	Authors:
 *	Pedro Roque		<roque@di.fc.ul.pt>	
 *
 *	Based on linux/net/ipv4/ip_sockglue.c
 *
 *	$Id: ipv6_sockglue.c,v 1.12 1996/10/29 22:45:53 roque Exp $
 *
 *	This program is free software; you can redistribute it and/or
 *      modify it under the terms of the GNU General Public License
 *      as published by the Free Software Foundation; either version
 *      2 of the License, or (at your option) any later version.
 */

#include <linux/errno.h>
#include <linux/types.h>
#include <linux/socket.h>
#include <linux/sockios.h>
#include <linux/sched.h>
#include <linux/net.h>
#include <linux/in6.h>
#include <linux/netdevice.h>
#include <linux/if_arp.h>

#include <linux/sysctl.h>

#include <net/sock.h>
#include <net/snmp.h>

#include <net/ipv6.h>
#include <net/ndisc.h>
#include <net/protocol.h>
#include <net/transp_v6.h>
#include <net/ipv6_route.h>
#include <net/addrconf.h>
#include <net/inet_common.h>
#include <net/sit.h>
#include <net/tcp.h>
#include <net/udp.h>

#include <asm/uaccess.h>

struct ipv6_mib ipv6_statistics={0, };
struct packet_type ipv6_packet_type =
{
	0, 
	NULL,					/* All devices */
	ipv6_rcv,
	NULL,
	NULL
};

/*
 *	addrconf module should be notifyed of a device going up
 */
static struct notifier_block ipv6_dev_notf = {
	addrconf_notify,
	NULL,
	0
};

int ipv6_setsockopt(struct sock *sk, int level, int optname, char *optval, 
		    int optlen)
{
	struct ipv6_pinfo *np = &sk->net_pinfo.af_inet6;
	int val, err;
	int retv = -EOPNOTSUPP;

	if(level!=SOL_IPV6)
		goto out;

	if (optval == NULL)
	{
		val=0;
	}
	else
	{
		err = get_user(val, (int *) optval);
		if(err)
			return err;
	}
	

	switch (optname) {

	case IPV6_ADDRFORM:
		if (val == PF_INET)
		{
			if (sk->protocol != IPPROTO_UDP &&
			    sk->protocol != IPPROTO_TCP)
			{				
				goto out;
			}
			
			if (sk->state != TCP_ESTABLISHED)
			{
				retv = ENOTCONN;
				goto out;
			}
			
			if (!(ipv6_addr_type(&np->daddr) & IPV6_ADDR_MAPPED))
			{
				retv = -EADDRNOTAVAIL;
				goto out;
			}

			if (sk->protocol == IPPROTO_TCP)
			{
				struct tcp_opt *tp = &(sk->tp_pinfo.af_tcp);
				
				sk->prot = &tcp_prot;
				tp->af_specific = &ipv4_specific;
				sk->socket->ops = &inet_stream_ops;
			}
			else
			{
				sk->prot = &udp_prot;
				sk->socket->ops = &inet_dgram_ops;
			}
			retv = 0;
		}
		else
		{
			retv = -EINVAL;
		}
		break;

	case IPV6_RXINFO:
		np->rxinfo = val;
		retv = 0;
		break;

	case IPV6_UNICAST_HOPS:
		if (val > 255)
		{
			retv = -EINVAL;
		}
		else
		{
			np->hop_limit = val;
			retv = 0;
		}
		break;

	case IPV6_MULTICAST_HOPS:
		if (val > 255)
		{
			retv = -EINVAL;
		}
		else
		{
			np->mcast_hops = val;
			retv = 0;
		}
		break;

	case IPV6_MULTICAST_LOOP:
		np->mc_loop = val;
		break;

	case IPV6_MULTICAST_IF:
	{
		struct in6_addr addr;

		err = copy_from_user(&addr, optval, sizeof(struct in6_addr));
		if(err)
			return -EFAULT;
				
		if (ipv6_addr_any(&addr))
		{
			np->mc_if = NULL;
		}
		else
		{
			struct inet6_ifaddr *ifp;

			ifp = ipv6_chk_addr(&addr);

			if (ifp == NULL)
			{
				retv = -EADDRNOTAVAIL;
				break;
			}

			np->mc_if = ifp->idev->dev;
		}
		retv = 0;
		break;
	}
	case IPV6_ADD_MEMBERSHIP:
	case IPV6_DROP_MEMBERSHIP:
	{
		struct ipv6_mreq mreq;
		struct device *dev = NULL;
		int err;

		err = copy_from_user(&mreq, optval, sizeof(struct ipv6_mreq));
		if(err)
			return -EFAULT;
		
		if (mreq.ipv6mr_ifindex == 0)
		{
			struct in6_addr mcast;
			struct dest_entry *dc;

			ipv6_addr_set(&mcast, __constant_htonl(0xff000000),
				      0, 0, 0);
			dc = ipv6_dst_route(&mcast, NULL, 0);

			if (dc)
			{
				dev = dc->rt.rt_dev;
				ipv6_dst_unlock(dc);
			}
		}
		else
		{
			struct inet6_dev *idev;
			
			if ((idev = ipv6_dev_by_index(mreq.ipv6mr_ifindex)))
			{
				dev = idev->dev;
			}
		}

		if (dev == NULL)
		{
			return -ENODEV;
		}
		
		if (optname == IPV6_ADD_MEMBERSHIP)
		{
			retv = ipv6_sock_mc_join(sk, dev, &mreq.ipv6mr_multiaddr);
		}
		else
		{
			retv = ipv6_sock_mc_drop(sk, dev, &mreq.ipv6mr_multiaddr);
		}
	}
	}

  out:
	return retv;
}

int ipv6_getsockopt(struct sock *sk, int level, int optname, char *optval, 
		    int *optlen)
{
	return 0;
}

#ifdef MODULE

/*
 *	sysctl registration functions defined in sysctl_net_ipv6.c
 */

extern void ipv6_sysctl_register(void);
extern void ipv6_sysctl_unregister(void);
#endif

void ipv6_init(void)
{
	ipv6_packet_type.type = ntohs(ETH_P_IPV6);

	dev_add_pack(&ipv6_packet_type);

#ifdef MODULE
	ipv6_sysctl_register();
#endif

	register_netdevice_notifier(&ipv6_dev_notf);
	
	ipv6_route_init();
}

#ifdef MODULE
void ipv6_cleanup(void)
{
	unregister_netdevice_notifier(&ipv6_dev_notf);
	dev_remove_pack(&ipv6_packet_type);
	ipv6_sysctl_unregister();	
	ipv6_route_cleanup();
	ndisc_cleanup();
	addrconf_cleanup();	
}
#endif

/*
 * Local variables:
 *  compile-command: "gcc -D__KERNEL__ -I/usr/src/linux/include -Wall -Wstrict-prototypes -O6 -m486 -c ipv6_sockglue.c"
 * End:
 */
