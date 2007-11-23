/*
 *	This module implements the (SPP-derived) Sequenced Packet eXchange
 *	(SPX) protocol for Linux 2.1.X as specified in
 *		NetWare SPX Services Specification, Semantics and API
 *		 Revision:       1.00
 *		 Revision Date:  February 9, 1993
 *
 *	Developers:
 *      Jay Schulist    <Jay.Schulist@spacs.k12.wi.us>
 *	Jim Freeman	<jfree@caldera.com>
 *
 *	Changes:
 *	Alan Cox	:	Fixed an skb_unshare check for NULL
 *				that crashed it under load. Renamed and
 *				made static the ipx ops. Removed the hack
 *				ipx methods interface. Dropped AF_SPX - its
 *				the wrong abstraction.
 *
 *	This program is free software; you can redistribute it and/or
 *      modify it under the terms of the GNU General Public License
 *      as published by the Free Software Foundation; either version
 *      2 of the License, or (at your option) any later version.
 *
 *	None of the authors or maintainers or their employers admit
 *	liability nor provide warranty for any of this software.
 *	This material is provided "as is" and at no charge.
 */

#include <linux/config.h>
#if defined(CONFIG_SPX) || defined(CONFIG_SPX_MODULE)
#include <linux/module.h>
#include <net/ipx.h>
#include <net/spx.h>
#include <net/sock.h>
#include <asm/byteorder.h>
#include <asm/uaccess.h>
#include <linux/uio.h>
#include <linux/unistd.h>
#include <linux/firewall.h>

static struct proto_ops *ipx_operations;
static struct proto_ops spx_operations;
static __u16  connids;

/* Functions needed for SPX connection start up */
static int spx_transmit(struct sock *sk,struct sk_buff *skb,int type,int len);
static void spx_retransmit(unsigned long data);
static void spx_watchdog(unsigned long data);
void spx_rcv(struct sock *sk, int bytes);

/* Create the SPX specific data */
static int spx_sock_init(struct sock *sk)
{
        struct spx_opt *pdata = &sk->tp_pinfo.af_spx;

        pdata->state            = SPX_CLOSED;
        pdata->sequence         = 0;
	pdata->acknowledge	= 0;
        pdata->source_connid    = htons(connids);
	pdata->rmt_seq		= 0;
        connids++;

        pdata->owner            = (void *)sk;
        pdata->sndbuf           = sk->sndbuf;

        pdata->watchdog.function = spx_watchdog;
        pdata->watchdog.data    = (unsigned long)sk;
        pdata->wd_interval      = VERIFY_TIMEOUT;
	pdata->retransmit.function = spx_retransmit;
	pdata->retransmit.data	= (unsigned long)sk;
	pdata->retransmits	= 0;
        pdata->retries          = 0;
        pdata->max_retries      = RETRY_COUNT;

	skb_queue_head_init(&pdata->rcv_queue);
	skb_queue_head_init(&pdata->transmit_queue);
	skb_queue_head_init(&pdata->retransmit_queue);

        return (0);
}

static int spx_create(struct socket *sock, int protocol)
{
	struct sock *sk;

	sk = sk_alloc(PF_IPX, GFP_KERNEL, 1);
	if(sk == NULL)
                return (-ENOMEM);

	switch(sock->type)
        {
                case SOCK_SEQPACKET:
			sock->ops = &spx_operations;
			break;
		default:
			sk_free(sk);
                        return (-ESOCKTNOSUPPORT);
        }

	sock_init_data(sock, sk);
	spx_sock_init(sk);
	sk->data_ready  = spx_rcv;
	sk->destruct 	= NULL;
        sk->mtu 	= IPX_MTU;
        sk->no_check 	= 1;

        MOD_INC_USE_COUNT;

	return (0);
}

static int spx_shutdown(struct socket *sk,int how)
{
        return (-EOPNOTSUPP);
}

void spx_close_socket(struct sock *sk)
{
	struct spx_opt *pdata = &sk->tp_pinfo.af_spx;

	pdata->state	= SPX_CLOSED;
	sk->state 	= TCP_CLOSE;
	del_timer(&pdata->retransmit);
	del_timer(&pdata->watchdog);
}

void spx_destroy_socket(struct sock *sk)
{
	struct spx_opt *pdata = &sk->tp_pinfo.af_spx;
	struct sk_buff *skb;

        ipx_remove_socket(sk);
        while((skb = skb_dequeue(&sk->receive_queue)) != NULL)
                kfree_skb(skb);
	while((skb = skb_dequeue(&pdata->transmit_queue)) != NULL)
                kfree_skb(skb);
	while((skb = skb_dequeue(&pdata->retransmit_queue)) != NULL)
		kfree_skb(skb);
	while((skb = skb_dequeue(&pdata->rcv_queue)) != NULL)
                kfree_skb(skb);

        sk_free(sk);
	MOD_DEC_USE_COUNT;
}

/* Release an SPX socket */
static int spx_release(struct socket *sock, struct socket *peer)
{
 	struct sock *sk = sock->sk;
	struct spx_opt *pdata = &sk->tp_pinfo.af_spx;

	if(sk == NULL)
		return (0);
	if(!sk->dead)
                sk->state_change(sk);
        sk->dead = 1;

	if(pdata->state != SPX_CLOSED)
	{
		spx_transmit(sk, NULL, DISCON, 0);
		spx_close_socket(sk);
	}

	sock->sk	= NULL;
	sk->socket 	= NULL;
	spx_destroy_socket(sk);

        return (0);
}

/* Move a socket into listening state. */
static int spx_listen(struct socket *sock, int backlog)
{
        struct sock *sk = sock->sk;

        if(sock->state != SS_UNCONNECTED)
                return (-EINVAL);
	if(sock->type != SOCK_SEQPACKET)
		return (-EOPNOTSUPP);
        if(sk->zapped != 0)
                return (-EAGAIN);

        if((unsigned) backlog == 0)     /* BSDism */
                backlog = 1;
        if((unsigned) backlog > SOMAXCONN)
                backlog = SOMAXCONN;
        sk->max_ack_backlog = backlog;
        if(sk->state != TCP_LISTEN)
        {
                sk->ack_backlog = 0;
                sk->state = TCP_LISTEN;
        }
        sk->socket->flags |= SO_ACCEPTCON;

        return (0);
}

/* Accept a pending SPX connection */
static int spx_accept(struct socket *sock, struct socket *newsock, int flags)
{
        struct sock *sk;
        struct sock *newsk;
        struct sk_buff *skb;
	int err;

	if(newsock->sk != NULL)
		spx_destroy_socket(newsock->sk);
        newsock->sk = NULL;

	if(sock->sk == NULL)
		return (-EINVAL);
	sk = sock->sk;

        if((sock->state != SS_UNCONNECTED) || !(sock->flags & SO_ACCEPTCON))
                return (-EINVAL);
        if(sock->type != SOCK_SEQPACKET)
		return (-EOPNOTSUPP);
	if(sk->state != TCP_LISTEN)
                return (-EINVAL);

	cli();
	do {
		skb = skb_dequeue(&sk->receive_queue);
		if(skb == NULL)
		{
                	if(flags & O_NONBLOCK)
			{
                                sti();
                                return (-EWOULDBLOCK);
                        }
                	interruptible_sleep_on(sk->sleep);
                	if(signal_pending(current))
                	{
                        	sti();
                        	return (-ERESTARTSYS);
                	}
		}
	} while (skb == NULL);

	newsk 		= skb->sk;
        newsk->pair 	= NULL;
	sti();

	err = spx_transmit(newsk, skb, CONACK, 0);   /* Connection ACK */
	if(err)
		return (err);

	/* Now attach up the new socket */
	sock->sk 	= NULL;
        sk->ack_backlog--;
        newsock->sk 	= newsk;
	newsk->state 	= TCP_ESTABLISHED;
	newsk->protinfo.af_ipx.dest_addr = newsk->tp_pinfo.af_spx.dest_addr;

	return (0);
}

/* Build a connection to an SPX socket */
static int spx_connect(struct socket *sock, struct sockaddr *uaddr,
                int addr_len, int flags)
{
	struct sock *sk = sock->sk;
        struct spx_opt *pdata = &sk->tp_pinfo.af_spx;
        struct sockaddr_ipx src;
	struct sk_buff *skb;
	int size, err;

	size = sizeof(src);
	err  = ipx_operations->getname(sock, (struct sockaddr *)&src, &size, 0);
	if(err)
		return (err);

        pdata->source_addr.net	= src.sipx_network;
        memcpy(pdata->source_addr.node, src.sipx_node, IPX_NODE_LEN);
        pdata->source_addr.sock = (unsigned short)src.sipx_port;

	err = ipx_operations->connect(sock, uaddr, addr_len, flags);
        if(err)
                return (err);

        pdata->dest_addr = sk->protinfo.af_ipx.dest_addr;
	pdata->state	 = SPX_CONNECTING;
	sock->state	 = SS_CONNECTING;
        sk->state	 = TCP_SYN_SENT;

        /* Send Connection request */
	err = spx_transmit(sk, NULL, CONREQ, 0);
        if(err)
                return (err);

	cli();
        do {
                skb = skb_dequeue(&sk->receive_queue);
                if(skb == NULL)
                {
                        if(flags & O_NONBLOCK)
                        {
                                sti();
                                return (-EWOULDBLOCK);
                        }
                        interruptible_sleep_on(sk->sleep);
                        if(signal_pending(current))
                        {
                                sti();
                                return (-ERESTARTSYS);
                        }
                }
        } while (skb == NULL);

        if(pdata->state == SPX_CLOSED)
        {
		sti();
                del_timer(&pdata->watchdog);
                return (-ETIMEDOUT);
        }

	sock->state	= SS_CONNECTED;
	sk->state 	= TCP_ESTABLISHED;
	kfree_skb(skb);
	sti();

        return (0);
}

/*
 * Calculate the timeout for a packet. Thankfully SPX has a large
 * fudge factor (3/4 secs) and does not pay much attention to RTT.
 * As we simply have a default retry time of 1*HZ and a max retry
 * time of 5*HZ. Between those values we increase the timeout based
 * on the number of retransmit tries.
 */
static inline unsigned long spx_calc_rtt(int tries)
{
        if(tries < 1)
                return (RETRY_TIME);
        if(tries > 5)
                return (MAX_RETRY_DELAY);
        return (tries * HZ);
}

static int spx_route_skb(struct spx_opt *pdata, struct sk_buff *skb, int type)
{
	struct sk_buff *skb2;
	int err = 0;

	skb = skb_unshare(skb, GFP_ATOMIC);
	if(skb==NULL)
		return -ENOBUFS;

	switch(type)
	{
		case (DATA):
			if(!skb_queue_empty(&pdata->retransmit_queue))
			{
				skb_queue_tail(&pdata->transmit_queue, skb);
				return 0;
			}

		case (TQUEUE):
			pdata->retransmit.expires = jiffies + spx_calc_rtt(0);
			add_timer(&pdata->retransmit);

			skb2 = skb_clone(skb, GFP_BUFFER);
	                if(skb2 == NULL)
        	                return -ENOBUFS;
        	        skb_queue_tail(&pdata->retransmit_queue, skb2);

		case (ACK):
		case (CONREQ):
		case (CONACK):
		case (WDREQ):
		case (WDACK):
		case (DISCON):
		case (DISACK):
		case (RETRAN):
		default:
			/* Send data */
        		err = ipxrtr_route_skb(skb);
        		if(err)
                		kfree_skb(skb);
	}

	return (err);
}

/* SPX packet transmit engine */
static int spx_transmit(struct sock *sk, struct sk_buff *skb, int type, int len)
{
        struct spx_opt *pdata = &sk->tp_pinfo.af_spx;
        struct ipxspxhdr *ipxh;
	int flags, err;

	if(skb == NULL)
	{
		int offset  = ipx_if_offset(pdata->dest_addr.net);
        	int size    = offset + sizeof(struct ipxspxhdr);

		save_flags(flags);
		cli();
        	skb = sock_alloc_send_skb(sk, size, 0, 0, &err);
        	if(skb == NULL)
                	return (-ENOMEM);
        	skb_reserve(skb, offset);
        	skb->nh.raw = skb_put(skb, sizeof(struct ipxspxhdr));
		restore_flags(flags);
	}

	/* IPX header */
	ipxh = (struct ipxspxhdr *)skb->nh.raw;
	ipxh->ipx.ipx_checksum  = 0xFFFF;
	ipxh->ipx.ipx_pktsize   = htons(SPX_SYS_PKT_LEN);
        ipxh->ipx.ipx_tctrl     = 0;
	ipxh->ipx.ipx_type 	= IPX_TYPE_SPX;
        ipxh->ipx.ipx_dest      = pdata->dest_addr;
        ipxh->ipx.ipx_source    = pdata->source_addr;

	/* SPX header */
        ipxh->spx.dtype         = 0;
	ipxh->spx.sequence      = htons(pdata->sequence);
	ipxh->spx.ackseq        = htons(pdata->rmt_seq);
	ipxh->spx.sconn         = pdata->source_connid;
        ipxh->spx.dconn         = pdata->dest_connid;
        ipxh->spx.allocseq      = htons(pdata->alloc);

	/* Reset/Set WD timer */
        del_timer(&pdata->watchdog);
        pdata->watchdog.expires = jiffies + VERIFY_TIMEOUT;
        add_timer(&pdata->watchdog);

	switch(type)
	{
		case (DATA):	/* Data */
			ipxh->ipx.ipx_pktsize 	= htons(SPX_SYS_PKT_LEN + len);
			ipxh->spx.cctl 		= (CCTL_ACK | CCTL_EOM);
                	pdata->sequence++;
			break;

		case (ACK):	/* Connection/WD/Data ACK */
			pdata->rmt_seq++;
		case (WDACK):
		case (CONACK):
			ipxh->spx.cctl 		= CCTL_SYS;
			ipxh->spx.ackseq 	= htons(pdata->rmt_seq);
			break;

		case (CONREQ):	/* Connection Request */
			del_timer(&pdata->watchdog);
		case (WDREQ):	/* WD Request */
			pdata->source_connid    = htons(connids++);
                	pdata->dest_connid      = 0xFFFF;
                	pdata->alloc 		= 3 + pdata->rmt_seq;
			ipxh->spx.cctl          = (CCTL_ACK | CCTL_SYS);
			ipxh->spx.sconn         = pdata->source_connid;
		        ipxh->spx.dconn         = pdata->dest_connid;
		        ipxh->spx.allocseq      = htons(pdata->alloc);
			break;

		case (DISCON):	/* Informed Disconnect */
			ipxh->spx.cctl 		= CCTL_ACK;
			ipxh->spx.dtype 	= SPX_DTYPE_ECONN;
			break;

		case (DISACK):	/* Informed Disconnect ACK */
			ipxh->spx.cctl  	= 0;
			ipxh->spx.dtype 	= SPX_DTYPE_ECACK;
			ipxh->spx.sequence 	= 0;
			ipxh->spx.ackseq 	= htons(pdata->rmt_seq++);
			break;

		default:
			return (-EOPNOTSUPP);
	}

	/* Send data */
	spx_route_skb(pdata, skb, type);

        return (0);
}

/* Check the state of the connection and send a WD request if needed. */
static void spx_watchdog(unsigned long data)
{
	struct sock *sk = (struct sock*)data;
        struct spx_opt *pdata = &sk->tp_pinfo.af_spx;

        del_timer(&pdata->watchdog);
	if(pdata->retries > pdata->max_retries)
        {
		spx_close_socket(sk);	/* Unilateral Abort */
                return;
        }

        /* Send WD request */
	spx_transmit(sk, NULL, WDREQ, 0);
	pdata->retries++;

        return;
}

static void spx_retransmit(unsigned long data)
{
	struct sock *sk = (struct sock*)data;
        struct spx_opt *pdata = &sk->tp_pinfo.af_spx;
	struct sk_buff *skb;
	int err;

	del_timer(&pdata->retransmit);
	if(pdata->retransmits > RETRY_COUNT)
	{
		spx_close_socket(sk);   /* Unilateral Abort */
                return;
        }

	/* need to leave skb on the queue! */
	skb = skb_peek(&pdata->retransmit_queue);
	if(skb_cloned(skb))
                skb = skb_copy(skb, GFP_ATOMIC);
        else
                skb = skb_clone(skb, GFP_ATOMIC);

	pdata->retransmit.expires = jiffies + spx_calc_rtt(pdata->retransmits);
	add_timer(&pdata->retransmit);

	err = spx_route_skb(pdata, skb, RETRAN);
	pdata->retransmits++;

	return;
}

/* SPX packet receive engine */
void spx_rcv(struct sock *sk, int bytes)
{
	struct sk_buff *skb;
	struct sk_buff *skb2;
	struct ipxspxhdr *ipxh;
	struct ipxspxhdr *ipxh2;
	struct spx_opt *pdata = &sk->tp_pinfo.af_spx;

	skb = skb_dequeue(&sk->receive_queue);
	if(skb == NULL)
		return;
	ipxh = (struct ipxspxhdr *)skb->nh.raw;

	/* Can't receive on a closed connection */
        if((pdata->state == SPX_CLOSED) && (ipxh->spx.sequence != 0))
		return;
	if(ntohs(ipxh->ipx.ipx_pktsize) < SPX_SYS_PKT_LEN)
		return;
        if(ipxh->ipx.ipx_type != IPX_TYPE_SPX)
		return;

	/* insanity - rcv'd ACK of unsent data ?? */
        if(ntohs(ipxh->spx.ackseq) > pdata->sequence)
		return;

	/* Reset WD timer on any received packet */
	del_timer(&pdata->watchdog);
	pdata->retries = 0;
	pdata->watchdog.expires = jiffies + ABORT_TIMEOUT;
        add_timer(&pdata->watchdog);

	switch(ipxh->spx.cctl)
	{
		case (CCTL_SYS | CCTL_ACK):
			if((ipxh->spx.sequence == 0)	/* ConReq */
				&& (ipxh->spx.ackseq == 0)
				&& (ipxh->spx.dconn == 0xFFFF))
			{
				pdata->state		= SPX_CONNECTED;
				pdata->dest_addr        = ipxh->ipx.ipx_source;
				pdata->source_addr      = ipxh->ipx.ipx_dest;
				pdata->dest_connid      = ipxh->spx.sconn;
				pdata->alloc = 3 + ntohs(ipxh->spx.sequence);

				skb_queue_tail(&sk->receive_queue, skb);
				wake_up_interruptible(sk->sleep);
			}
			else	/* WD Request */
				spx_transmit(sk, skb, WDACK, 0);
			break;

		case CCTL_SYS:	/* ACK */
			if((ipxh->spx.dtype == 0)       /* ConReq ACK */
                                && (ipxh->spx.sconn != 0xFFFF)
                                && (ipxh->spx.dconn != 0xFFFF)
                                && (ipxh->spx.sequence == 0)
                                && (ipxh->spx.ackseq == 0)
                                && (pdata->state != SPX_CONNECTED))
                        {
                                pdata->state = SPX_CONNECTED;

                                skb_queue_tail(&sk->receive_queue, skb);
                                wake_up_interruptible(sk->sleep);
                                break;
                        }

			/* Check Data/ACK seq */
                        skb2 = skb_dequeue(&pdata->retransmit_queue);
                        if(skb2)
                        {
				ipxh2 = (struct ipxspxhdr *)skb2->nh.raw;
                                if((ntohs(ipxh2->spx.sequence)
                                        == (ntohs(ipxh->spx.ackseq) - 1))
					|| (ntohs(ipxh2->spx.sequence) == 65535
					&& ntohs(ipxh->spx.ackseq) == 0))
                                {
					del_timer(&pdata->retransmit);
					pdata->retransmits = 0;
                                        kfree_skb(skb2);
					if(skb_queue_empty(&pdata->retransmit_queue))
					{
						skb2 = skb_dequeue(&pdata->transmit_queue);
						if(skb2 != NULL)
							spx_route_skb(pdata, skb2, TQUEUE);
					}
                                }
                                else    /* Out of Seq - ERROR! */
				skb_queue_head(&pdata->retransmit_queue, skb2);
                        }

			kfree_skb(skb);
			break;

		case (CCTL_ACK):	/* Informed Disconnect */
			if(ipxh->spx.dtype == SPX_DTYPE_ECONN)
			{
				spx_transmit(sk, skb, DISACK, 0);
				spx_close_socket(sk);
			}
			break;

		default:
			if(ntohs(ipxh->spx.sequence) == pdata->rmt_seq)
			{
				pdata->rmt_seq = ntohs(ipxh->spx.sequence);
				skb_queue_tail(&pdata->rcv_queue, skb);
				wake_up_interruptible(sk->sleep);
				spx_transmit(sk, NULL, ACK, 0);
				break;
			}

			/* Catch All */
			kfree_skb(skb);
			break;
	}

        return;
}

/* Get message/packet data from user-land */
static int spx_sendmsg(struct socket *sock, struct msghdr *msg, int len,
			struct scm_cookie *scm)
{
	struct sock *sk = sock->sk;
	int flags = msg->msg_flags;
	struct sk_buff *skb;
	int err, offset, size;

	if(len > 534)
                return (-EMSGSIZE);
        if(sk->zapped)
                return (-ENOTCONN); /* Socket not bound */
	if(flags&~MSG_DONTWAIT)
                return (-EINVAL);

	offset	= ipx_if_offset(sk->tp_pinfo.af_spx.dest_addr.net);
        size 	= offset + sizeof(struct ipxspxhdr) + len;
        skb  	= sock_alloc_send_skb(sk, size, 0, flags&MSG_DONTWAIT, &err);
        if(skb == NULL)
                return (err);

	skb->sk = sk;
        skb_reserve(skb, offset);
	skb->nh.raw = skb_put(skb, sizeof(struct ipxspxhdr));

	err = memcpy_fromiovec(skb_put(skb, len), msg->msg_iov, len);
	if(err)
        {
                kfree_skb(skb);
                return (-EFAULT);
        }

	err = spx_transmit(sk, skb, DATA, len);
	if(err)
		return (-EAGAIN);

        return (len);
}

/* Send message/packet data to user-land */
static int spx_recvmsg(struct socket *sock, struct msghdr *msg, int size,
			int flags, struct scm_cookie *scm)
{
	struct sk_buff *skb;
	struct ipxspxhdr *ispxh;
	struct sock *sk = sock->sk;
	struct spx_opt *pdata = &sk->tp_pinfo.af_spx;
	struct sockaddr_ipx *sipx = (struct sockaddr_ipx *)msg->msg_name;
	int copied, err;

        if(sk->zapped)
                return (-ENOTCONN); /* Socket not bound */

	lock_sock(sk);
restart:
        while(skb_queue_empty(&pdata->rcv_queue))      /* No data */
        {
                /* Socket errors? */
                err = sock_error(sk);
                if(err)
			return (err);

                /* Socket shut down? */
                if(sk->shutdown & RCV_SHUTDOWN)
			return (-ESHUTDOWN);

                /* handle signals */
                if(signal_pending(current))
			return (-ERESTARTSYS);

                /* User doesn't want to wait */
                if(flags&MSG_DONTWAIT)
			return (-EAGAIN);

		release_sock(sk);
        	save_flags(flags);
        	cli();
        	if(skb_peek(&pdata->rcv_queue) == NULL)
                	interruptible_sleep_on(sk->sleep);
        	restore_flags(flags);
        	lock_sock(sk);
        }

        skb = skb_dequeue(&pdata->rcv_queue);
        if(skb == NULL) 
		goto restart;

	ispxh 	= (struct ipxspxhdr *)skb->nh.raw;
	copied 	= ntohs(ispxh->ipx.ipx_pktsize) - SPX_SYS_PKT_LEN;
        if(copied > size)
	{
                copied = size;
                msg->msg_flags |= MSG_TRUNC;
        }

	err = memcpy_toiovec(msg->msg_iov, skb->nh.raw+SPX_SYS_PKT_LEN, copied);
        if(err)
                return (-EFAULT);

	msg->msg_namelen = sizeof(*sipx);
	if(sipx)
	{
		sipx->sipx_family	= AF_IPX;
                sipx->sipx_port		= ispxh->ipx.ipx_source.sock;
                memcpy(sipx->sipx_node,ispxh->ipx.ipx_source.node,IPX_NODE_LEN);
                sipx->sipx_network	= ispxh->ipx.ipx_source.net;
                sipx->sipx_type 	= ispxh->ipx.ipx_type;
        }
	kfree_skb(skb);
        release_sock(sk);

	return (copied);
}

/*
 * Functions which just wrap their IPX cousins
 */

static int spx_bind(struct socket *sock, struct sockaddr *uaddr, int addr_len)
{
        int err;
        err = ipx_operations->bind(sock, uaddr, addr_len);
        return (err);
}

static int spx_getname (struct socket *sock, struct sockaddr *uaddr,
                         int *usockaddr_len, int peer)
{
	int err;
	err = ipx_operations->getname(sock, uaddr, usockaddr_len, peer);
	return (err);
}

static int spx_ioctl (struct socket *sock, unsigned int cmd,
                         unsigned long arg)
{
	int err;
	err = ipx_operations->ioctl(sock, cmd, arg);
	return (err);
}

static int spx_setsockopt(struct socket *sock, int level, int optname,
                         char *optval, int optlen)
{
	int err;
	err = ipx_operations->setsockopt(sock, level, optname, optval, optlen);
	return (err);
}

static int spx_getsockopt(struct socket *sock, int level, int optname,
                         char *optval, int *optlen)
{
	int err;
	err = ipx_operations->getsockopt(sock, level, optname, optval, optlen);
	return (err);
}

static struct proto_ops spx_operations = {
        PF_IPX,
        sock_no_dup,
        spx_release,
        spx_bind,
        spx_connect,
        sock_no_socketpair,
        spx_accept,
	spx_getname,
        datagram_poll,  /* this does seqpacket too */
	spx_ioctl,
        spx_listen,
        spx_shutdown,
	spx_setsockopt,
	spx_getsockopt,
        sock_no_fcntl,
        spx_sendmsg,
        spx_recvmsg
};

static struct net_proto_family spx_family_ops=
{
	PF_IPX,
	spx_create
};


void spx_proto_init(void)
{
	int error;

	connids = (__u16)jiffies;	/* initalize random */

	error = ipx_register_spx(&ipx_operations, &spx_family_ops);
        if (error)
                printk(KERN_ERR "SPX: unable to register with IPX.\n");

	/* route socket(PF_IPX, SOCK_SEQPACKET) calls through spx_create() */

	printk(KERN_INFO "Sequenced Packet eXchange (SPX) 0.01 for Linux NET3.037\n");
	return;
}

void spx_proto_finito(void)
{
	ipx_unregister_spx();
	return;
}

#ifdef MODULE

int init_module(void)
{
        spx_proto_init();
        return 0;
}

void cleanup_module(void)
{
        spx_proto_finito();
        return;
}

#endif /* MODULE */
#endif /* CONFIG_SPX || CONFIG_SPX_MODULE */