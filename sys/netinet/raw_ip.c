/*
 * Copyright (c) 1982, 1986, 1988, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 *	@(#)raw_ip.c	8.7 (Berkeley) 5/15/95
 * $FreeBSD: src/sys/netinet/raw_ip.c,v 1.64.2.16 2003/08/24 08:24:38 hsu Exp $
 */

#include "opt_inet6.h"
#include "opt_carp.h"

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/jail.h>
#include <sys/malloc.h>
#include <sys/mbuf.h>
#include <sys/proc.h>
#include <sys/caps.h>
#include <sys/protosw.h>
#include <sys/socket.h>
#include <sys/socketvar.h>
#include <sys/sysctl.h>

#include <sys/socketvar2.h>
#include <sys/msgport2.h>

#include <machine/stdarg.h>

#include <net/if.h>
#ifdef CARP
#include <net/if_types.h>
#endif
#include <net/route.h>

#define _IP_VHL
#include <netinet/in.h>
#include <netinet/in_systm.h>
#include <netinet/ip.h>
#include <netinet/in_pcb.h>
#include <netinet/in_var.h>
#include <netinet/ip_var.h>

#include <net/ip_mroute/ip_mroute.h>
#include <net/ipfw/ip_fw.h>
#include <net/ipfw3/ip_fw.h>
#include <net/dummynet/ip_dummynet.h>
#include <net/dummynet3/ip_dummynet3.h>

struct	inpcbinfo ripcbinfo;
struct	inpcbportinfo ripcbportinfo;

/*
 * hooks for multicast routing. They all default to NULL,
 * so leave them not initialized and rely on BSS being set to 0.
 */

/* The socket used to communicate with the multicast routing daemon.  */
struct socket  *ip_mrouter;

/* The various mrouter and rsvp functions */
int (*ip_mrouter_set)(struct socket *, struct sockopt *);
int (*ip_mrouter_get)(struct socket *, struct sockopt *);
int (*ip_mrouter_done)(void);
int (*ip_mforward)(struct ip *, struct ifnet *, struct mbuf *,
		struct ip_moptions *);
int (*mrt_ioctl)(u_long, caddr_t);
int (*legal_vif_num)(int);
u_long (*ip_mcast_src)(int);

int (*rsvp_input_p)(struct mbuf **, int *, int);
int (*ip_rsvp_vif)(struct socket *, struct sockopt *);
void (*ip_rsvp_force_done)(struct socket *);

/*
 * Nominal space allocated to a raw ip socket.
 */
#define	RIPSNDQ		8192
#define	RIPRCVQ		8192

/*
 * Raw interface to IP protocol.
 */

/*
 * Initialize raw connection block queue.
 */
void
rip_init(void)
{
	in_pcbinfo_init(&ripcbinfo, 0, FALSE);
	in_pcbportinfo_init(&ripcbportinfo, 1, 0);
	/*
	 * XXX We don't use the hash list for raw IP, but it's easier
	 * to allocate a one entry hash list than it is to check all
	 * over the place for hashbase == NULL.
	 */
	ripcbinfo.hashbase = hashinit(1, M_PCB, &ripcbinfo.hashmask);
	in_pcbportinfo_set(&ripcbinfo, &ripcbportinfo, 1);
	ripcbinfo.wildcardhashbase = hashinit(1, M_PCB,
					      &ripcbinfo.wildcardhashmask);
	ripcbinfo.ipi_size = sizeof(struct inpcb);
}

/*
 * Setup generic address and protocol structures
 * for raw_input routine, then pass them along with
 * mbuf chain.
 */
int
rip_input(struct mbuf **mp, int *offp, int proto)
{
	struct sockaddr_in ripsrc = { sizeof ripsrc, AF_INET };
	struct mbuf *m = *mp;
	struct ip *ip = mtod(m, struct ip *);
	struct inpcb *inp;
	struct inpcb *last = NULL;
	struct mbuf *opts = NULL;

	ASSERT_NETISR0;

	*mp = NULL;

	ripsrc.sin_addr = ip->ip_src;
	LIST_FOREACH(inp, &ripcbinfo.pcblisthead, inp_list) {
		if (inp->inp_flags & INP_PLACEMARKER)
			continue;
#ifdef INET6
		if (!INP_ISIPV4(inp))
			continue;
#endif
		if (inp->inp_ip_p && inp->inp_ip_p != proto)
			continue;
		if (inp->inp_laddr.s_addr != INADDR_ANY &&
		    inp->inp_laddr.s_addr != ip->ip_dst.s_addr)
			continue;
		if (inp->inp_faddr.s_addr != INADDR_ANY &&
		    inp->inp_faddr.s_addr != ip->ip_src.s_addr)
			continue;
		if (last) {
			struct mbuf *n = m_copypacket(m, M_NOWAIT);

			if (n) {
				lwkt_gettoken(&last->inp_socket->so_rcv.ssb_token);
				if (last->inp_flags & INP_CONTROLOPTS ||
				    last->inp_socket->so_options & SO_TIMESTAMP)
				    ip_savecontrol(last, &opts, ip, n);
				if (ssb_appendaddr(&last->inp_socket->so_rcv,
					    (struct sockaddr *)&ripsrc, n,
					    opts) == 0) {
					m_freem(n);
					if (opts)
					    m_freem(opts);
					soroverflow(last->inp_socket);
				} else {
					sorwakeup(last->inp_socket);
				}
				lwkt_reltoken(&last->inp_socket->so_rcv.ssb_token);
				opts = NULL;
			}
		}
		last = inp;
	}
	/* Check the minimum TTL for socket. */
	if (last && ip->ip_ttl < last->inp_ip_minttl) {
		m_freem(opts);
		ipstat.ips_delivered--;
	} else if (last) {
		if (last->inp_flags & INP_CONTROLOPTS ||
		    last->inp_socket->so_options & SO_TIMESTAMP)
			ip_savecontrol(last, &opts, ip, m);
		lwkt_gettoken(&last->inp_socket->so_rcv.ssb_token);
		if (ssb_appendaddr(&last->inp_socket->so_rcv,
		    (struct sockaddr *)&ripsrc, m, opts) == 0) {
			m_freem(m);
			if (opts)
			    m_freem(opts);
			soroverflow(last->inp_socket);
		} else {
			sorwakeup(last->inp_socket);
		}
		lwkt_reltoken(&last->inp_socket->so_rcv.ssb_token);
	} else {
		m_freem(m);
		ipstat.ips_noproto++;
		ipstat.ips_delivered--;
	}
	return(IPPROTO_DONE);
}

/*
 * Generate IP header and pass packet to ip_output.
 * Tack on options user may have setup with control call.
 */
int
rip_output(struct mbuf *m, struct socket *so, ...)
{
	struct ip *ip;
	struct inpcb *inp = so->so_pcb;
	__va_list ap;
	int flags = (so->so_options & SO_DONTROUTE) | IP_ALLOWBROADCAST;
	u_long dst;

	ASSERT_NETISR0;

	__va_start(ap, so);
	dst = __va_arg(ap, u_long);
	__va_end(ap);

	/*
	 * If the user handed us a complete IP packet, use it.
	 * Otherwise, allocate an mbuf for a header and fill it in.
	 */
	if ((inp->inp_flags & INP_HDRINCL) == 0) {
		if (m->m_pkthdr.len + sizeof(struct ip) > IP_MAXPACKET) {
			m_freem(m);
			return(EMSGSIZE);
		}
		M_PREPEND(m, sizeof(struct ip), M_WAITOK);
		if (m == NULL)
			return(ENOBUFS);
		ip = mtod(m, struct ip *);
		ip->ip_tos = inp->inp_ip_tos;
		ip->ip_off = 0;
		ip->ip_p = inp->inp_ip_p;
		ip->ip_len = htons(m->m_pkthdr.len); /* incls header now */
		ip->ip_src = inp->inp_laddr;
		ip->ip_dst.s_addr = dst;
		ip->ip_ttl = inp->inp_ip_ttl;
	} else {
		int hlen;
		int ip_len;

		if (m->m_pkthdr.len > IP_MAXPACKET) {
			m_freem(m);
			return(EMSGSIZE);
		}
		if (m->m_len < sizeof(struct ip)) {
			m = m_pullup(m, sizeof(struct ip));
			if (m == NULL)
				return ENOBUFS;
		}
		ip = mtod(m, struct ip *);
		hlen = IP_VHL_HL(ip->ip_vhl) << 2;
		ip_len = ntohs(ip->ip_len);	/* includes header */

		/*
		 * Don't allow both user specified and setsockopt options.
		 * Don't allow packet length sizes that will crash.
		 */
		if (hlen < sizeof(struct ip) ||
		    (hlen != sizeof(struct ip) && inp->inp_options) ||
		    ip_len > m->m_pkthdr.len ||
		    ip_len < hlen)
		{
			m_freem(m);
			return EINVAL;
		}

		/*
		 * System supplies ip_id if passed as 0.
		 */
		if (ip->ip_id == 0)
			ip->ip_id = ip_newid();

		/* Prevent ip_output from overwriting header fields */
		flags |= IP_RAWOUTPUT;
		ipstat.ips_rawout++;
	}

	return ip_output(m, inp->inp_options, &inp->inp_route, flags,
			 inp->inp_moptions, inp);
}

/*
 * Raw IP socket option processing.
 */
void
rip_ctloutput(netmsg_t msg)
{
	struct socket *so = msg->base.nm_so;
	struct sockopt *sopt = msg->ctloutput.nm_sopt;
	struct	inpcb *inp = so->so_pcb;
	int	error, optval;

	ASSERT_NETISR0;

	error = 0;

	/* Get socket's owner cpuid hint */
	if (sopt->sopt_level == SOL_SOCKET &&
	    sopt->sopt_dir == SOPT_GET &&
	    sopt->sopt_name == SO_CPUHINT) {
		optval = mycpuid;
		soopt_from_kbuf(sopt, &optval, sizeof(optval));
		goto done;
	}

	if (sopt->sopt_level != IPPROTO_IP) {
		error = EINVAL;
		goto done;
	}

	switch (sopt->sopt_dir) {
	case SOPT_GET:
		switch (sopt->sopt_name) {
		case IP_HDRINCL:
			optval = inp->inp_flags & INP_HDRINCL;
			soopt_from_kbuf(sopt, &optval, sizeof optval);
			break;

		case IP_FW_X:
			error = ip_fw3_sockopt(sopt);
			break;

		case IP_FW_ADD: /* ADD actually returns the body... */
		case IP_FW_GET:
		case IP_FW_TBL_GET:
		case IP_FW_TBL_EXPIRE: /* returns # of expired addresses */
			error = ip_fw_sockopt(sopt);
			break;

		case IP_DUMMYNET_GET:
			error = ip_dn_sockopt(sopt);
			break ;

		case MRT_INIT:
		case MRT_DONE:
		case MRT_ADD_VIF:
		case MRT_DEL_VIF:
		case MRT_ADD_MFC:
		case MRT_DEL_MFC:
		case MRT_VERSION:
		case MRT_ASSERT:
		case MRT_API_SUPPORT:
		case MRT_API_CONFIG:
		case MRT_ADD_BW_UPCALL:
		case MRT_DEL_BW_UPCALL:
			error = ip_mrouter_get ? ip_mrouter_get(so, sopt) :
				EOPNOTSUPP;
			break;

		default:
			ip_ctloutput(msg);
			/* msg invalid now */
			return;
		}
		break;

	case SOPT_SET:
		switch (sopt->sopt_name) {
		case IP_HDRINCL:
			error = soopt_to_kbuf(sopt, &optval, sizeof optval,
					      sizeof optval);
			if (error)
				break;
			if (optval)
				inp->inp_flags |= INP_HDRINCL;
			else
				inp->inp_flags &= ~INP_HDRINCL;
			break;

		case IP_FW_X:
			error = ip_fw3_sockopt(sopt);
			break;

		case IP_FW_ADD:
		case IP_FW_DEL:
		case IP_FW_FLUSH:
		case IP_FW_ZERO:
		case IP_FW_RESETLOG:
		case IP_FW_TBL_CREATE:
		case IP_FW_TBL_DESTROY:
		case IP_FW_TBL_ADD:
		case IP_FW_TBL_DEL:
		case IP_FW_TBL_FLUSH:
		case IP_FW_TBL_ZERO:
		case IP_FW_TBL_EXPIRE:
			error = ip_fw_sockopt(sopt);
			break;

		case IP_DUMMYNET_CONFIGURE:
		case IP_DUMMYNET_DEL:
		case IP_DUMMYNET_FLUSH:
			error = ip_dn_sockopt(sopt);
			break;

		case IP_RSVP_ON:
			error = ip_rsvp_init(so);
			break;

		case IP_RSVP_OFF:
			error = ip_rsvp_done();
			break;

		case IP_RSVP_VIF_ON:
		case IP_RSVP_VIF_OFF:
			error = ip_rsvp_vif ?
				ip_rsvp_vif(so, sopt) : EINVAL;
			break;

		case MRT_INIT:
		case MRT_DONE:
		case MRT_ADD_VIF:
		case MRT_DEL_VIF:
		case MRT_ADD_MFC:
		case MRT_DEL_MFC:
		case MRT_VERSION:
		case MRT_ASSERT:
		case MRT_API_SUPPORT:
		case MRT_API_CONFIG:
		case MRT_ADD_BW_UPCALL:
		case MRT_DEL_BW_UPCALL:
			error = ip_mrouter_set ? ip_mrouter_set(so, sopt) :
					EOPNOTSUPP;
			break;

		default:
			ip_ctloutput(msg);
			/* msg invalid now */
			return;
		}
		break;
	}
done:
	lwkt_replymsg(&msg->lmsg, error);
}

u_long	rip_sendspace = RIPSNDQ;
u_long	rip_recvspace = RIPRCVQ;

SYSCTL_INT(_net_inet_raw, OID_AUTO, maxdgram, CTLFLAG_RW,
    &rip_sendspace, 0, "Maximum outgoing raw IP datagram size");
SYSCTL_INT(_net_inet_raw, OID_AUTO, recvspace, CTLFLAG_RW,
    &rip_recvspace, 0, "Maximum incoming raw IP datagram size");

static void
rip_attach(netmsg_t msg)
{
	struct socket *so = msg->base.nm_so;
	int proto = msg->attach.nm_proto;
	struct pru_attach_info *ai = msg->attach.nm_ai;
	struct inpcb *inp;
	int error;

	ASSERT_NETISR0;

	inp = so->so_pcb;
	if (inp)
		panic("rip_attach");
	error = caps_priv_check(ai->p_ucred, SYSCAP_NONET_RAW |
					     __SYSCAP_NULLCRED);
	if (error)
		goto done;

	error = soreserve(so, rip_sendspace, rip_recvspace, ai->sb_rlimit);
	if (error)
		goto done;

	error = in_pcballoc(so, &ripcbinfo);
	if (error == 0) {
		inp = (struct inpcb *)so->so_pcb;
		inp->inp_ip_p = proto;
		inp->inp_ip_ttl = ip_defttl;
	}
done:
	lwkt_replymsg(&msg->lmsg, error);
}

static void
rip_detach(netmsg_t msg)
{
	struct socket *so = msg->base.nm_so;
	struct inpcb *inp;

	ASSERT_NETISR0;

	inp = so->so_pcb;
	if (inp == NULL)
		panic("rip_detach");
	if (so == ip_mrouter && ip_mrouter_done)
		ip_mrouter_done();
	if (ip_rsvp_force_done)
		ip_rsvp_force_done(so);
	if (so == ip_rsvpd)
		ip_rsvp_done();
	in_pcbdetach(inp);
	lwkt_replymsg(&msg->lmsg, 0);
}

static void
rip_abort(netmsg_t msg)
{
	/*
	 * Raw socket does not support listen(2),
	 * so this should never be called.
	 */
	panic("rip_abort is called");
}

static void
rip_disconnect(netmsg_t msg)
{
	struct socket *so = msg->base.nm_so;
	int error;

	ASSERT_NETISR0;

	if (so->so_state & SS_ISCONNECTED) {
		soisdisconnected(so);
		error = 0;
	} else {
		error = ENOTCONN;
	}
	lwkt_replymsg(&msg->lmsg, error);
}

static void
rip_bind(netmsg_t msg)
{
	struct socket *so = msg->base.nm_so;
	struct sockaddr *nam = msg->bind.nm_nam;
	struct inpcb *inp = so->so_pcb;
	struct sockaddr_in *addr = (struct sockaddr_in *)nam;
	int error;

	ASSERT_NETISR0;

	if (nam->sa_len == sizeof(*addr)) {
		if (ifnet_array_isempty() ||
		    ((addr->sin_family != AF_INET) &&
		     (addr->sin_family != AF_IMPLINK)) ||
		    (addr->sin_addr.s_addr != INADDR_ANY &&
		     ifa_ifwithaddr((struct sockaddr *)addr) == 0)) {
			error = EADDRNOTAVAIL;
		} else {
			inp->inp_laddr = addr->sin_addr;
			error = 0;
		}
	} else {
		error = EINVAL;
	}
	lwkt_replymsg(&msg->lmsg, error);
}

static void
rip_connect(netmsg_t msg)
{
	struct socket *so = msg->base.nm_so;
	struct sockaddr *nam = msg->connect.nm_nam;
	struct inpcb *inp = so->so_pcb;
	struct sockaddr_in *addr = (struct sockaddr_in *)nam;
	int error;

	ASSERT_NETISR0;

	if (nam->sa_len != sizeof(*addr)) {
		error = EINVAL;
	} else if (ifnet_array_isempty()) {
		error = EADDRNOTAVAIL;
	} else {
		if ((addr->sin_family != AF_INET) &&
		    (addr->sin_family != AF_IMPLINK)) {
			error = EAFNOSUPPORT;
		} else {
			inp->inp_faddr = addr->sin_addr;
			soisconnected(so);
			error = 0;
		}
	}
	lwkt_replymsg(&msg->lmsg, error);
}

static void
rip_shutdown(netmsg_t msg)
{
	ASSERT_NETISR0;

	socantsendmore(msg->base.nm_so);
	lwkt_replymsg(&msg->lmsg, 0);
}

static void
rip_send(netmsg_t msg)
{
	struct socket *so = msg->base.nm_so;
	struct mbuf *m = msg->send.nm_m;
	/*struct mbuf *control = msg->send.nm_control;*/
	struct sockaddr *nam = msg->send.nm_addr;
	/*int flags = msg->send.nm_flags;*/
	struct inpcb *inp = so->so_pcb;
	u_long dst;
	int error;

	ASSERT_NETISR0;

	if (so->so_state & SS_ISCONNECTED) {
		if (nam) {
			m_freem(m);
			error = EISCONN;
		} else {
			dst = inp->inp_faddr.s_addr;
			error = rip_output(m, so, dst);
		}
	} else {
		if (nam == NULL) {
			m_freem(m);
			error = ENOTCONN;
		} else {
			dst = ((struct sockaddr_in *)nam)->sin_addr.s_addr;
			error = rip_output(m, so, dst);
		}
	}
	lwkt_replymsg(&msg->lmsg, error);
}

SYSCTL_PROC(_net_inet_raw, OID_AUTO/*XXX*/, pcblist, CTLFLAG_RD, &ripcbinfo, 1,
	    in_pcblist_range, "S,xinpcb", "List of active raw IP sockets");

struct pr_usrreqs rip_usrreqs = {
	.pru_abort = rip_abort,
	.pru_accept = pr_generic_notsupp,
	.pru_attach = rip_attach,
	.pru_bind = rip_bind,
	.pru_connect = rip_connect,
	.pru_connect2 = pr_generic_notsupp,
	.pru_control = in_control_dispatch,
	.pru_detach = rip_detach,
	.pru_disconnect = rip_disconnect,
	.pru_listen = pr_generic_notsupp,
	.pru_peeraddr = in_setpeeraddr_dispatch,
	.pru_rcvd = pr_generic_notsupp,
	.pru_rcvoob = pr_generic_notsupp,
	.pru_send = rip_send,
	.pru_sense = pru_sense_null,
	.pru_shutdown = rip_shutdown,
	.pru_sockaddr = in_setsockaddr_dispatch,
	.pru_sosend = sosend,
	.pru_soreceive = soreceive
};
