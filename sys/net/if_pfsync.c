/*	$OpenBSD: if_pfsync.c,v 1.317 2023/06/05 08:45:20 sashan Exp $	*/

/*
 * Copyright (c) 2002 Michael Shalayeff
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR OR HIS RELATIVES BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF MIND, USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
 * IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * Copyright (c) 2009 David Gwynne <dlg@openbsd.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/time.h>
#include <sys/malloc.h>
#include <sys/mbuf.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/timeout.h>
#include <sys/kernel.h>
#include <sys/sysctl.h>
#include <sys/pool.h>
#include <sys/syslog.h>

#include <net/if.h>
#include <net/if_types.h>
#include <net/bpf.h>
#include <net/netisr.h>

#include <netinet/in.h>
#include <netinet/if_ether.h>
#include <netinet/ip.h>
#include <netinet/in_var.h>
#include <netinet/ip_var.h>
#include <netinet/ip_ipsp.h>
#include <netinet/ip_icmp.h>
#include <netinet/icmp6.h>
#include <netinet/tcp.h>
#include <netinet/tcp_seq.h>
#include <netinet/tcp_fsm.h>
#include <netinet/udp.h>

#ifdef INET6
#include <netinet6/in6_var.h>
#include <netinet/ip6.h>
#include <netinet6/ip6_var.h>
#include <netinet6/nd6.h>
#endif /* INET6 */

#include "carp.h"
#if NCARP > 0
#include <netinet/ip_carp.h>
#endif

#define PF_DEBUGNAME	"pfsync: "
#include <net/pfvar.h>
#include <net/pfvar_priv.h>
#include <net/if_pfsync.h>

#include "bpfilter.h"
#include "pfsync.h"

#define PFSYNC_DEFER_NSEC 20000000ULL

#define PFSYNC_MINPKT ( \
	sizeof(struct ip) + \
	sizeof(struct pfsync_header))

int	pfsync_upd_tcp(struct pf_state *, struct pfsync_state_peer *,
	    struct pfsync_state_peer *);

int	pfsync_in_clr(caddr_t, int, int, int);
int	pfsync_in_iack(caddr_t, int, int, int);
int	pfsync_in_upd_c(caddr_t, int, int, int);
int	pfsync_in_ureq(caddr_t, int, int, int);
int	pfsync_in_del(caddr_t, int, int, int);
int	pfsync_in_del_c(caddr_t, int, int, int);
int	pfsync_in_bus(caddr_t, int, int, int);
int	pfsync_in_tdb(caddr_t, int, int, int);
int	pfsync_in_ins(caddr_t, int, int, int);
int	pfsync_in_upd(caddr_t, int, int, int);
int	pfsync_in_eof(caddr_t, int, int, int);

int	pfsync_in_error(caddr_t, int, int, int);

void	pfsync_update_state_locked(struct pf_state *);

const struct {
	int	(*in)(caddr_t, int, int, int);
	size_t	len;
} pfsync_acts[] = {
	/* PFSYNC_ACT_CLR */
	{ pfsync_in_clr,	sizeof(struct pfsync_clr) },
	 /* PFSYNC_ACT_OINS */
	{ pfsync_in_error,	0 },
	/* PFSYNC_ACT_INS_ACK */
	{ pfsync_in_iack,	sizeof(struct pfsync_ins_ack) },
	/* PFSYNC_ACT_OUPD */
	{ pfsync_in_error,	0 },
	/* PFSYNC_ACT_UPD_C */
	{ pfsync_in_upd_c,	sizeof(struct pfsync_upd_c) },
	/* PFSYNC_ACT_UPD_REQ */
	{ pfsync_in_ureq,	sizeof(struct pfsync_upd_req) },
	/* PFSYNC_ACT_DEL */
	{ pfsync_in_del,	sizeof(struct pfsync_state) },
	/* PFSYNC_ACT_DEL_C */
	{ pfsync_in_del_c,	sizeof(struct pfsync_del_c) },
	/* PFSYNC_ACT_INS_F */
	{ pfsync_in_error,	0 },
	/* PFSYNC_ACT_DEL_F */
	{ pfsync_in_error,	0 },
	/* PFSYNC_ACT_BUS */
	{ pfsync_in_bus,	sizeof(struct pfsync_bus) },
	/* PFSYNC_ACT_OTDB */
	{ pfsync_in_error,	0 },
	/* PFSYNC_ACT_EOF */
	{ pfsync_in_error,	0 },
	/* PFSYNC_ACT_INS */
	{ pfsync_in_ins,	sizeof(struct pfsync_state) },
	/* PFSYNC_ACT_UPD */
	{ pfsync_in_upd,	sizeof(struct pfsync_state) },
	/* PFSYNC_ACT_TDB */
	{ pfsync_in_tdb,	sizeof(struct pfsync_tdb) },
};

struct pfsync_q {
	void		(*write)(struct pf_state *, void *);
	size_t		len;
	u_int8_t	action;
};

/* we have one of these for every PFSYNC_S_ */
void	pfsync_out_state(struct pf_state *, void *);
void	pfsync_out_iack(struct pf_state *, void *);
void	pfsync_out_upd_c(struct pf_state *, void *);
void	pfsync_out_del(struct pf_state *, void *);

struct pfsync_q pfsync_qs[] = {
	{ pfsync_out_iack,  sizeof(struct pfsync_ins_ack), PFSYNC_ACT_INS_ACK },
	{ pfsync_out_upd_c, sizeof(struct pfsync_upd_c),   PFSYNC_ACT_UPD_C },
	{ pfsync_out_del,   sizeof(struct pfsync_del_c),   PFSYNC_ACT_DEL_C },
	{ pfsync_out_state, sizeof(struct pfsync_state),   PFSYNC_ACT_INS },
	{ pfsync_out_state, sizeof(struct pfsync_state),   PFSYNC_ACT_UPD }
};

void	pfsync_q_ins(struct pf_state *, int);
void	pfsync_q_del(struct pf_state *);

struct pfsync_upd_req_item {
	TAILQ_ENTRY(pfsync_upd_req_item)	ur_entry;
	TAILQ_ENTRY(pfsync_upd_req_item)	ur_snap;
	struct pfsync_upd_req			ur_msg;
};
TAILQ_HEAD(pfsync_upd_reqs, pfsync_upd_req_item);

struct pfsync_deferral {
	TAILQ_ENTRY(pfsync_deferral)		 pd_entry;
	struct pf_state				*pd_st;
	struct mbuf				*pd_m;
	uint64_t				 pd_deadline;
};
TAILQ_HEAD(pfsync_deferrals, pfsync_deferral);

#define PFSYNC_PLSIZE	MAX(sizeof(struct pfsync_upd_req_item), \
			    sizeof(struct pfsync_deferral))

void	pfsync_out_tdb(struct tdb *, void *);

struct pfsync_softc {
	struct ifnet		 sc_if;
	unsigned int		 sc_sync_ifidx;

	struct pool		 sc_pool;

	struct ip_moptions	 sc_imo;

	struct in_addr		 sc_sync_peer;
	u_int8_t		 sc_maxupdates;

	struct ip		 sc_template;

	struct pf_state_queue	 sc_qs[PFSYNC_S_COUNT];
	struct mutex		 sc_st_mtx;
	size_t			 sc_len;

	struct pfsync_upd_reqs	 sc_upd_req_list;
	struct mutex		 sc_upd_req_mtx;

	int			 sc_initial_bulk;
	int			 sc_link_demoted;

	int			 sc_defer;
	struct pfsync_deferrals	 sc_deferrals;
	u_int			 sc_deferred;
	struct mutex		 sc_deferrals_mtx;
	struct timeout		 sc_deferrals_tmo;

	void			*sc_plus;
	size_t			 sc_pluslen;

	u_int32_t		 sc_ureq_sent;
	int			 sc_bulk_tries;
	struct timeout		 sc_bulkfail_tmo;

	u_int32_t		 sc_ureq_received;
	struct pf_state		*sc_bulk_next;
	struct pf_state		*sc_bulk_last;
	struct timeout		 sc_bulk_tmo;

	TAILQ_HEAD(, tdb)	 sc_tdb_q;
	struct mutex		 sc_tdb_mtx;

	struct task		 sc_ltask;
	struct task		 sc_dtask;

	struct timeout		 sc_tmo;
};

struct pfsync_snapshot {
	struct pfsync_softc	*sn_sc;
	struct pf_state_queue	 sn_qs[PFSYNC_S_COUNT];
	struct pfsync_upd_reqs	 sn_upd_req_list;
	TAILQ_HEAD(, tdb)	 sn_tdb_q;
	size_t			 sn_len;
	void			*sn_plus;
	size_t			 sn_pluslen;
};

struct pfsync_softc	*pfsyncif = NULL;
struct cpumem		*pfsynccounters;

void	pfsyncattach(int);
int	pfsync_clone_create(struct if_clone *, int);
int	pfsync_clone_destroy(struct ifnet *);
void	pfsync_update_net_tdb(struct pfsync_tdb *);
int	pfsyncoutput(struct ifnet *, struct mbuf *, struct sockaddr *,
	    struct rtentry *);
int	pfsyncioctl(struct ifnet *, u_long, caddr_t);
void	pfsyncstart(struct ifqueue *);
void	pfsync_syncdev_state(void *);
void	pfsync_ifdetach(void *);

void	pfsync_deferred(struct pf_state *, int);
void	pfsync_undefer(struct pfsync_deferral *, int);
void	pfsync_deferrals_tmo(void *);

void	pfsync_cancel_full_update(struct pfsync_softc *);
void	pfsync_request_full_update(struct pfsync_softc *);
void	pfsync_request_update(u_int32_t, u_int64_t);
void	pfsync_update_state_req(struct pf_state *);

void	pfsync_drop(struct pfsync_softc *);
void	pfsync_sendout(void);
void	pfsync_send_plus(void *, size_t);
void	pfsync_timeout(void *);
void	pfsync_tdb_timeout(void *);

void	pfsync_bulk_start(void);
void	pfsync_bulk_status(u_int8_t);
void	pfsync_bulk_update(void *);
void	pfsync_bulk_fail(void *);

void	pfsync_grab_snapshot(struct pfsync_snapshot *, struct pfsync_softc *);
void	pfsync_drop_snapshot(struct pfsync_snapshot *);

void	pfsync_send_dispatch(void *);
void	pfsync_send_pkt(struct mbuf *);

static struct mbuf_queue	pfsync_mq;
static struct task	pfsync_task =
    TASK_INITIALIZER(pfsync_send_dispatch, &pfsync_mq);

#define PFSYNC_MAX_BULKTRIES	12
int	pfsync_sync_ok;

struct if_clone	pfsync_cloner =
    IF_CLONE_INITIALIZER("pfsync", pfsync_clone_create, pfsync_clone_destroy);

void
pfsyncattach(int npfsync)
{
	if_clone_attach(&pfsync_cloner);
	pfsynccounters = counters_alloc(pfsyncs_ncounters);
	mq_init(&pfsync_mq, 4096, IPL_MPFLOOR);
}

int
pfsync_clone_create(struct if_clone *ifc, int unit)
{
	struct pfsync_softc *sc;
	struct ifnet *ifp;
	int q;

	if (unit != 0)
		return (EINVAL);

	pfsync_sync_ok = 1;

	sc = malloc(sizeof(*pfsyncif), M_DEVBUF, M_WAITOK|M_ZERO);
	for (q = 0; q < PFSYNC_S_COUNT; q++)
		TAILQ_INIT(&sc->sc_qs[q]);
	mtx_init(&sc->sc_st_mtx, IPL_MPFLOOR);

	pool_init(&sc->sc_pool, PFSYNC_PLSIZE, 0, IPL_MPFLOOR, 0, "pfsync",
	    NULL);
	TAILQ_INIT(&sc->sc_upd_req_list);
	mtx_init(&sc->sc_upd_req_mtx, IPL_MPFLOOR);
	TAILQ_INIT(&sc->sc_deferrals);
	mtx_init(&sc->sc_deferrals_mtx, IPL_MPFLOOR);
	timeout_set_proc(&sc->sc_deferrals_tmo, pfsync_deferrals_tmo, sc);
	task_set(&sc->sc_ltask, pfsync_syncdev_state, sc);
	task_set(&sc->sc_dtask, pfsync_ifdetach, sc);
	sc->sc_deferred = 0;

	TAILQ_INIT(&sc->sc_tdb_q);
	mtx_init(&sc->sc_tdb_mtx, IPL_MPFLOOR);

	sc->sc_len = PFSYNC_MINPKT;
	sc->sc_maxupdates = 128;

	sc->sc_imo.imo_membership = mallocarray(IP_MIN_MEMBERSHIPS,
	    sizeof(struct in_multi *), M_IPMOPTS, M_WAITOK|M_ZERO);
	sc->sc_imo.imo_max_memberships = IP_MIN_MEMBERSHIPS;

	ifp = &sc->sc_if;
	snprintf(ifp->if_xname, sizeof ifp->if_xname, "pfsync%d", unit);
	ifp->if_softc = sc;
	ifp->if_ioctl = pfsyncioctl;
	ifp->if_output = pfsyncoutput;
	ifp->if_qstart = pfsyncstart;
	ifp->if_type = IFT_PFSYNC;
	ifp->if_hdrlen = sizeof(struct pfsync_header);
	ifp->if_mtu = ETHERMTU;
	ifp->if_xflags = IFXF_CLONED | IFXF_MPSAFE;
	timeout_set_proc(&sc->sc_tmo, pfsync_timeout, NULL);
	timeout_set_proc(&sc->sc_bulk_tmo, pfsync_bulk_update, NULL);
	timeout_set_proc(&sc->sc_bulkfail_tmo, pfsync_bulk_fail, NULL);

	if_attach(ifp);
	if_alloc_sadl(ifp);

#if NCARP > 0
	if_addgroup(ifp, "carp");
#endif

#if NBPFILTER > 0
	bpfattach(&sc->sc_if.if_bpf, ifp, DLT_PFSYNC, PFSYNC_HDRLEN);
#endif

	pfsyncif = sc;

	return (0);
}

int
pfsync_clone_destroy(struct ifnet *ifp)
{
	struct pfsync_softc *sc = ifp->if_softc;
	struct ifnet *ifp0;
	struct pfsync_deferral *pd;
	struct pfsync_deferrals	 deferrals;

	NET_LOCK();

#if NCARP > 0
	if (!pfsync_sync_ok)
		carp_group_demote_adj(&sc->sc_if, -1, "pfsync destroy");
	if (sc->sc_link_demoted)
		carp_group_demote_adj(&sc->sc_if, -1, "pfsync destroy");
#endif
	if ((ifp0 = if_get(sc->sc_sync_ifidx)) != NULL) {
		if_linkstatehook_del(ifp0, &sc->sc_ltask);
		if_detachhook_del(ifp0, &sc->sc_dtask);
	}
	if_put(ifp0);

	/* XXXSMP breaks atomicity */
	NET_UNLOCK();
	if_detach(ifp);
	NET_LOCK();

	pfsync_drop(sc);

	if (sc->sc_deferred > 0) {
		TAILQ_INIT(&deferrals);
		mtx_enter(&sc->sc_deferrals_mtx);
		TAILQ_CONCAT(&deferrals, &sc->sc_deferrals, pd_entry);
		sc->sc_deferred = 0;
		mtx_leave(&sc->sc_deferrals_mtx);

		while ((pd = TAILQ_FIRST(&deferrals)) != NULL) {
			TAILQ_REMOVE(&deferrals, pd, pd_entry);
			pfsync_undefer(pd, 0);
		}
	}

	pfsyncif = NULL;
	timeout_del(&sc->sc_bulkfail_tmo);
	timeout_del(&sc->sc_bulk_tmo);
	timeout_del(&sc->sc_tmo);

	NET_UNLOCK();

	pool_destroy(&sc->sc_pool);
	free(sc->sc_imo.imo_membership, M_IPMOPTS,
	    sc->sc_imo.imo_max_memberships * sizeof(struct in_multi *));
	free(sc, M_DEVBUF, sizeof(*sc));

	return (0);
}

/*
 * Start output on the pfsync interface.
 */
void
pfsyncstart(struct ifqueue *ifq)
{
	ifq_purge(ifq);
}

void
pfsync_syncdev_state(void *arg)
{
	struct pfsync_softc *sc = arg;
	struct ifnet *ifp;

	if ((sc->sc_if.if_flags & IFF_UP) == 0)
		return;
	if ((ifp = if_get(sc->sc_sync_ifidx)) == NULL)
		return;

	if (ifp->if_link_state == LINK_STATE_DOWN) {
		sc->sc_if.if_flags &= ~IFF_RUNNING;
		if (!sc->sc_link_demoted) {
#if NCARP > 0
			carp_group_demote_adj(&sc->sc_if, 1,
			    "pfsync link state down");
#endif
			sc->sc_link_demoted = 1;
		}

		/* drop everything */
		timeout_del(&sc->sc_tmo);
		pfsync_drop(sc);

		pfsync_cancel_full_update(sc);
	} else if (sc->sc_link_demoted) {
		sc->sc_if.if_flags |= IFF_RUNNING;

		pfsync_request_full_update(sc);
	}

	if_put(ifp);
}

void
pfsync_ifdetach(void *arg)
{
	struct pfsync_softc *sc = arg;
	struct ifnet *ifp;

	if ((ifp = if_get(sc->sc_sync_ifidx)) != NULL) {
		if_linkstatehook_del(ifp, &sc->sc_ltask);
		if_detachhook_del(ifp, &sc->sc_dtask);
	}
	if_put(ifp);

	sc->sc_sync_ifidx = 0;
}

int
pfsync_input(struct mbuf **mp, int *offp, int proto, int af)
{
	struct mbuf *n, *m = *mp;
	struct pfsync_softc *sc = pfsyncif;
	struct ip *ip = mtod(m, struct ip *);
	struct pfsync_header *ph;
	struct pfsync_subheader subh;
	int offset, noff, len, count, mlen, flags = 0;
	int e;

	NET_ASSERT_LOCKED();

	pfsyncstat_inc(pfsyncs_ipackets);

	/* verify that we have a sync interface configured */
	if (sc == NULL || !ISSET(sc->sc_if.if_flags, IFF_RUNNING) ||
	    sc->sc_sync_ifidx == 0 || !pf_status.running)
		goto done;

	/* verify that the packet came in on the right interface */
	if (sc->sc_sync_ifidx != m->m_pkthdr.ph_ifidx) {
		pfsyncstat_inc(pfsyncs_badif);
		goto done;
	}

	sc->sc_if.if_ipackets++;
	sc->sc_if.if_ibytes += m->m_pkthdr.len;

	/* verify that the IP TTL is 255. */
	if (ip->ip_ttl != PFSYNC_DFLTTL) {
		pfsyncstat_inc(pfsyncs_badttl);
		goto done;
	}

	offset = ip->ip_hl << 2;
	n = m_pulldown(m, offset, sizeof(*ph), &noff);
	if (n == NULL) {
		pfsyncstat_inc(pfsyncs_hdrops);
		return IPPROTO_DONE;
	}
	ph = (struct pfsync_header *)(n->m_data + noff);

	/* verify the version */
	if (ph->version != PFSYNC_VERSION) {
		pfsyncstat_inc(pfsyncs_badver);
		goto done;
	}
	len = ntohs(ph->len) + offset;
	if (m->m_pkthdr.len < len) {
		pfsyncstat_inc(pfsyncs_badlen);
		goto done;
	}

	if (!bcmp(&ph->pfcksum, &pf_status.pf_chksum, PF_MD5_DIGEST_LENGTH))
		flags = PFSYNC_SI_CKSUM;

	offset += sizeof(*ph);
	while (offset <= len - sizeof(subh)) {
		m_copydata(m, offset, sizeof(subh), &subh);
		offset += sizeof(subh);

		mlen = subh.len << 2;
		count = ntohs(subh.count);

		if (subh.action >= PFSYNC_ACT_MAX ||
		    subh.action >= nitems(pfsync_acts) ||
		    mlen < pfsync_acts[subh.action].len) {
			/*
			 * subheaders are always followed by at least one
			 * message, so if the peer is new
			 * enough to tell us how big its messages are then we
			 * know enough to skip them.
			 */
			if (count > 0 && mlen > 0) {
				offset += count * mlen;
				continue;
			}
			pfsyncstat_inc(pfsyncs_badact);
			goto done;
		}

		n = m_pulldown(m, offset, mlen * count, &noff);
		if (n == NULL) {
			pfsyncstat_inc(pfsyncs_badlen);
			return IPPROTO_DONE;
		}

		e = pfsync_acts[subh.action].in(n->m_data + noff, mlen, count,
		    flags);
		if (e != 0)
			goto done;

		offset += mlen * count;
	}

done:
	m_freem(m);
	return IPPROTO_DONE;
}

int
pfsync_in_clr(caddr_t buf, int len, int count, int flags)
{
	struct pfsync_clr *clr;
	struct pf_state *st, *nexts;
	struct pfi_kif *kif;
	u_int32_t creatorid;
	int i;

	PF_LOCK();
	for (i = 0; i < count; i++) {
		clr = (struct pfsync_clr *)buf + len * i;
		kif = NULL;
		creatorid = clr->creatorid;
		if (strlen(clr->ifname) &&
		    (kif = pfi_kif_find(clr->ifname)) == NULL)
			continue;

		PF_STATE_ENTER_WRITE();
		RBT_FOREACH_SAFE(st, pf_state_tree_id, &tree_id, nexts) {
			if (st->creatorid == creatorid &&
			    ((kif && st->kif == kif) || !kif)) {
				SET(st->state_flags, PFSTATE_NOSYNC);
				pf_remove_state(st);
			}
		}
		PF_STATE_EXIT_WRITE();
	}
	PF_UNLOCK();

	return (0);
}

int
pfsync_in_ins(caddr_t buf, int len, int count, int flags)
{
	struct pfsync_state *sp;
	sa_family_t af1, af2;
	int i;

	PF_LOCK();
	for (i = 0; i < count; i++) {
		sp = (struct pfsync_state *)(buf + len * i);
		af1 = sp->key[0].af;
		af2 = sp->key[1].af;

		/* check for invalid values */
		if (sp->timeout >= PFTM_MAX ||
		    sp->src.state > PF_TCPS_PROXY_DST ||
		    sp->dst.state > PF_TCPS_PROXY_DST ||
		    sp->direction > PF_OUT ||
		    (((af1 || af2) &&
		     ((af1 != AF_INET && af1 != AF_INET6) ||
		      (af2 != AF_INET && af2 != AF_INET6))) ||
		    (sp->af != AF_INET && sp->af != AF_INET6))) {
			DPFPRINTF(LOG_NOTICE,
			    "pfsync_input: PFSYNC5_ACT_INS: invalid value");
			pfsyncstat_inc(pfsyncs_badval);
			continue;
		}

		if (pf_state_import(sp, flags) == ENOMEM) {
			/* drop out, but process the rest of the actions */
			break;
		}
	}
	PF_UNLOCK();

	return (0);
}

int
pfsync_in_iack(caddr_t buf, int len, int count, int flags)
{
	struct pfsync_ins_ack *ia;
	struct pf_state_cmp id_key;
	struct pf_state *st;
	int i;

	for (i = 0; i < count; i++) {
		ia = (struct pfsync_ins_ack *)(buf + len * i);

		id_key.id = ia->id;
		id_key.creatorid = ia->creatorid;

		PF_STATE_ENTER_READ();
		st = pf_find_state_byid(&id_key);
		pf_state_ref(st);
		PF_STATE_EXIT_READ();
		if (st == NULL)
			continue;

		if (ISSET(st->state_flags, PFSTATE_ACK))
			pfsync_deferred(st, 0);

		pf_state_unref(st);
	}

	return (0);
}

int
pfsync_upd_tcp(struct pf_state *st, struct pfsync_state_peer *src,
    struct pfsync_state_peer *dst)
{
	int sync = 0;

	/*
	 * The state should never go backwards except
	 * for syn-proxy states.  Neither should the
	 * sequence window slide backwards.
	 */
	if ((st->src.state > src->state &&
	    (st->src.state < PF_TCPS_PROXY_SRC ||
	    src->state >= PF_TCPS_PROXY_SRC)) ||

	    (st->src.state == src->state &&
	    SEQ_GT(st->src.seqlo, ntohl(src->seqlo))))
		sync++;
	else
		pf_state_peer_ntoh(src, &st->src);

	if ((st->dst.state > dst->state) ||

	    (st->dst.state >= TCPS_SYN_SENT &&
	    SEQ_GT(st->dst.seqlo, ntohl(dst->seqlo))))
		sync++;
	else
		pf_state_peer_ntoh(dst, &st->dst);

	return (sync);
}

int
pfsync_in_upd(caddr_t buf, int len, int count, int flags)
{
	struct pfsync_state *sp;
	struct pf_state_cmp id_key;
	struct pf_state *st;
	int sync, error;
	int i;

	for (i = 0; i < count; i++) {
		sp = (struct pfsync_state *)(buf + len * i);

		/* check for invalid values */
		if (sp->timeout >= PFTM_MAX ||
		    sp->src.state > PF_TCPS_PROXY_DST ||
		    sp->dst.state > PF_TCPS_PROXY_DST) {
			DPFPRINTF(LOG_NOTICE,
			    "pfsync_input: PFSYNC_ACT_UPD: invalid value");
			pfsyncstat_inc(pfsyncs_badval);
			continue;
		}

		id_key.id = sp->id;
		id_key.creatorid = sp->creatorid;

		PF_STATE_ENTER_READ();
		st = pf_find_state_byid(&id_key);
		pf_state_ref(st);
		PF_STATE_EXIT_READ();
		if (st == NULL) {
			/* insert the update */
			PF_LOCK();
			error = pf_state_import(sp, flags);
			if (error)
				pfsyncstat_inc(pfsyncs_badstate);
			PF_UNLOCK();
			continue;
		}

		if (ISSET(st->state_flags, PFSTATE_ACK))
			pfsync_deferred(st, 1);

		if (st->key[PF_SK_WIRE]->proto == IPPROTO_TCP)
			sync = pfsync_upd_tcp(st, &sp->src, &sp->dst);
		else {
			sync = 0;

			/*
			 * Non-TCP protocol state machine always go
			 * forwards
			 */
			if (st->src.state > sp->src.state)
				sync++;
			else
				pf_state_peer_ntoh(&sp->src, &st->src);

			if (st->dst.state > sp->dst.state)
				sync++;
			else
				pf_state_peer_ntoh(&sp->dst, &st->dst);
		}

		if (sync < 2) {
			pf_state_alloc_scrub_memory(&sp->dst, &st->dst);
			pf_state_peer_ntoh(&sp->dst, &st->dst);
			st->expire = getuptime();
			st->timeout = sp->timeout;
		}
		st->pfsync_time = getuptime();

		if (sync) {
			pfsyncstat_inc(pfsyncs_stale);

			pfsync_update_state(st);
			schednetisr(NETISR_PFSYNC);
		}

		pf_state_unref(st);
	}

	return (0);
}

int
pfsync_in_upd_c(caddr_t buf, int len, int count, int flags)
{
	struct pfsync_upd_c *up;
	struct pf_state_cmp id_key;
	struct pf_state *st;

	int sync;

	int i;

	for (i = 0; i < count; i++) {
		up = (struct pfsync_upd_c *)(buf + len * i);

		/* check for invalid values */
		if (up->timeout >= PFTM_MAX ||
		    up->src.state > PF_TCPS_PROXY_DST ||
		    up->dst.state > PF_TCPS_PROXY_DST) {
			DPFPRINTF(LOG_NOTICE,
			    "pfsync_input: PFSYNC_ACT_UPD_C: invalid value");
			pfsyncstat_inc(pfsyncs_badval);
			continue;
		}

		id_key.id = up->id;
		id_key.creatorid = up->creatorid;

		PF_STATE_ENTER_READ();
		st = pf_find_state_byid(&id_key);
		pf_state_ref(st);
		PF_STATE_EXIT_READ();
		if (st == NULL) {
			/* We don't have this state. Ask for it. */
			pfsync_request_update(id_key.creatorid, id_key.id);
			continue;
		}

		if (ISSET(st->state_flags, PFSTATE_ACK))
			pfsync_deferred(st, 1);

		if (st->key[PF_SK_WIRE]->proto == IPPROTO_TCP)
			sync = pfsync_upd_tcp(st, &up->src, &up->dst);
		else {
			sync = 0;
			/*
			 * Non-TCP protocol state machine always go
			 * forwards
			 */
			if (st->src.state > up->src.state)
				sync++;
			else
				pf_state_peer_ntoh(&up->src, &st->src);

			if (st->dst.state > up->dst.state)
				sync++;
			else
				pf_state_peer_ntoh(&up->dst, &st->dst);
		}
		if (sync < 2) {
			pf_state_alloc_scrub_memory(&up->dst, &st->dst);
			pf_state_peer_ntoh(&up->dst, &st->dst);
			st->expire = getuptime();
			st->timeout = up->timeout;
		}
		st->pfsync_time = getuptime();

		if (sync) {
			pfsyncstat_inc(pfsyncs_stale);

			pfsync_update_state(st);
			schednetisr(NETISR_PFSYNC);
		}

		pf_state_unref(st);
	}

	return (0);
}

int
pfsync_in_ureq(caddr_t buf, int len, int count, int flags)
{
	struct pfsync_upd_req *ur;
	int i;

	struct pf_state_cmp id_key;
	struct pf_state *st;

	for (i = 0; i < count; i++) {
		ur = (struct pfsync_upd_req *)(buf + len * i);

		id_key.id = ur->id;
		id_key.creatorid = ur->creatorid;

		if (id_key.id == 0 && id_key.creatorid == 0)
			pfsync_bulk_start();
		else {
			PF_STATE_ENTER_READ();
			st = pf_find_state_byid(&id_key);
			pf_state_ref(st);
			PF_STATE_EXIT_READ();
			if (st == NULL) {
				pfsyncstat_inc(pfsyncs_badstate);
				continue;
			}
			if (ISSET(st->state_flags, PFSTATE_NOSYNC)) {
				pf_state_unref(st);
				continue;
			}

			pfsync_update_state_req(st);
			pf_state_unref(st);
		}
	}

	return (0);
}

int
pfsync_in_del(caddr_t buf, int len, int count, int flags)
{
	struct pfsync_state *sp;
	struct pf_state_cmp id_key;
	struct pf_state *st;
	int i;

	PF_STATE_ENTER_WRITE();
	for (i = 0; i < count; i++) {
		sp = (struct pfsync_state *)(buf + len * i);

		id_key.id = sp->id;
		id_key.creatorid = sp->creatorid;

		st = pf_find_state_byid(&id_key);
		if (st == NULL) {
			pfsyncstat_inc(pfsyncs_badstate);
			continue;
		}
		SET(st->state_flags, PFSTATE_NOSYNC);
		pf_remove_state(st);
	}
	PF_STATE_EXIT_WRITE();

	return (0);
}

int
pfsync_in_del_c(caddr_t buf, int len, int count, int flags)
{
	struct pfsync_del_c *sp;
	struct pf_state_cmp id_key;
	struct pf_state *st;
	int i;

	PF_LOCK();
	PF_STATE_ENTER_WRITE();
	for (i = 0; i < count; i++) {
		sp = (struct pfsync_del_c *)(buf + len * i);

		id_key.id = sp->id;
		id_key.creatorid = sp->creatorid;

		st = pf_find_state_byid(&id_key);
		if (st == NULL) {
			pfsyncstat_inc(pfsyncs_badstate);
			continue;
		}

		SET(st->state_flags, PFSTATE_NOSYNC);
		pf_remove_state(st);
	}
	PF_STATE_EXIT_WRITE();
	PF_UNLOCK();

	return (0);
}

int
pfsync_in_bus(caddr_t buf, int len, int count, int flags)
{
	struct pfsync_softc *sc = pfsyncif;
	struct pfsync_bus *bus;

	/* If we're not waiting for a bulk update, who cares. */
	if (sc->sc_ureq_sent == 0)
		return (0);

	bus = (struct pfsync_bus *)buf;

	switch (bus->status) {
	case PFSYNC_BUS_START:
		PF_LOCK();
		timeout_add(&sc->sc_bulkfail_tmo, 4 * hz +
		    pf_pool_limits[PF_LIMIT_STATES].limit /
		    ((sc->sc_if.if_mtu - PFSYNC_MINPKT) /
		    sizeof(struct pfsync_state)));
		PF_UNLOCK();
		DPFPRINTF(LOG_INFO, "received bulk update start");
		break;

	case PFSYNC_BUS_END:
		if (getuptime() - ntohl(bus->endtime) >=
		    sc->sc_ureq_sent) {
			/* that's it, we're happy */
			sc->sc_ureq_sent = 0;
			sc->sc_bulk_tries = 0;
			timeout_del(&sc->sc_bulkfail_tmo);
#if NCARP > 0
			if (!pfsync_sync_ok)
				carp_group_demote_adj(&sc->sc_if, -1,
				    sc->sc_link_demoted ?
				    "pfsync link state up" :
				    "pfsync bulk done");
			if (sc->sc_initial_bulk) {
				carp_group_demote_adj(&sc->sc_if, -32,
				    "pfsync init");
				sc->sc_initial_bulk = 0;
			}
#endif
			pfsync_sync_ok = 1;
			sc->sc_link_demoted = 0;
			DPFPRINTF(LOG_INFO, "received valid bulk update end");
		} else {
			DPFPRINTF(LOG_WARNING, "received invalid "
			    "bulk update end: bad timestamp");
		}
		break;
	}

	return (0);
}

int
pfsync_in_tdb(caddr_t buf, int len, int count, int flags)
{
#if defined(IPSEC)
	struct pfsync_tdb *tp;
	int i;

	for (i = 0; i < count; i++) {
		tp = (struct pfsync_tdb *)(buf + len * i);
		pfsync_update_net_tdb(tp);
	}
#endif

	return (0);
}

#if defined(IPSEC)
/* Update an in-kernel tdb. Silently fail if no tdb is found. */
void
pfsync_update_net_tdb(struct pfsync_tdb *pt)
{
	struct tdb		*tdb;

	NET_ASSERT_LOCKED();

	/* check for invalid values */
	if (ntohl(pt->spi) <= SPI_RESERVED_MAX ||
	    (pt->dst.sa.sa_family != AF_INET &&
	     pt->dst.sa.sa_family != AF_INET6))
		goto bad;

	tdb = gettdb(ntohs(pt->rdomain), pt->spi,
	    (union sockaddr_union *)&pt->dst, pt->sproto);
	if (tdb) {
		pt->rpl = betoh64(pt->rpl);
		pt->cur_bytes = betoh64(pt->cur_bytes);

		/* Neither replay nor byte counter should ever decrease. */
		if (pt->rpl < tdb->tdb_rpl ||
		    pt->cur_bytes < tdb->tdb_cur_bytes) {
			tdb_unref(tdb);
			goto bad;
		}

		tdb->tdb_rpl = pt->rpl;
		tdb->tdb_cur_bytes = pt->cur_bytes;
		tdb_unref(tdb);
	}
	return;

 bad:
	DPFPRINTF(LOG_WARNING, "pfsync_insert: PFSYNC_ACT_TDB_UPD: "
	    "invalid value");
	pfsyncstat_inc(pfsyncs_badstate);
	return;
}
#endif


int
pfsync_in_eof(caddr_t buf, int len, int count, int flags)
{
	if (len > 0 || count > 0)
		pfsyncstat_inc(pfsyncs_badact);

	/* we're done. let the caller return */
	return (1);
}

int
pfsync_in_error(caddr_t buf, int len, int count, int flags)
{
	pfsyncstat_inc(pfsyncs_badact);
	return (-1);
}

int
pfsyncoutput(struct ifnet *ifp, struct mbuf *m, struct sockaddr *dst,
	struct rtentry *rt)
{
	m_freem(m);	/* drop packet */
	return (EAFNOSUPPORT);
}

int
pfsyncioctl(struct ifnet *ifp, u_long cmd, caddr_t data)
{
	struct proc *p = curproc;
	struct pfsync_softc *sc = ifp->if_softc;
	struct ifreq *ifr = (struct ifreq *)data;
	struct ip_moptions *imo = &sc->sc_imo;
	struct pfsyncreq pfsyncr;
	struct ifnet *ifp0, *sifp;
	struct ip *ip;
	int error;

	switch (cmd) {
	case SIOCSIFFLAGS:
		if ((ifp->if_flags & IFF_RUNNING) == 0 &&
		    (ifp->if_flags & IFF_UP)) {
			ifp->if_flags |= IFF_RUNNING;

#if NCARP > 0
			sc->sc_initial_bulk = 1;
			carp_group_demote_adj(&sc->sc_if, 32, "pfsync init");
#endif

			pfsync_request_full_update(sc);
		}
		if ((ifp->if_flags & IFF_RUNNING) &&
		    (ifp->if_flags & IFF_UP) == 0) {
			ifp->if_flags &= ~IFF_RUNNING;

			/* drop everything */
			timeout_del(&sc->sc_tmo);
			pfsync_drop(sc);

			pfsync_cancel_full_update(sc);
		}
		break;
	case SIOCSIFMTU:
		if ((ifp0 = if_get(sc->sc_sync_ifidx)) == NULL)
			return (EINVAL);
		error = 0;
		if (ifr->ifr_mtu <= PFSYNC_MINPKT ||
		    ifr->ifr_mtu > ifp0->if_mtu) {
			error = EINVAL;
		}
		if_put(ifp0);
		if (error)
			return error;
		if (ifr->ifr_mtu < ifp->if_mtu)
			pfsync_sendout();
		ifp->if_mtu = ifr->ifr_mtu;
		break;
	case SIOCGETPFSYNC:
		bzero(&pfsyncr, sizeof(pfsyncr));
		if ((ifp0 = if_get(sc->sc_sync_ifidx)) != NULL) {
			strlcpy(pfsyncr.pfsyncr_syncdev,
			    ifp0->if_xname, IFNAMSIZ);
		}
		if_put(ifp0);
		pfsyncr.pfsyncr_syncpeer = sc->sc_sync_peer;
		pfsyncr.pfsyncr_maxupdates = sc->sc_maxupdates;
		pfsyncr.pfsyncr_defer = sc->sc_defer;
		return (copyout(&pfsyncr, ifr->ifr_data, sizeof(pfsyncr)));

	case SIOCSETPFSYNC:
		if ((error = suser(p)) != 0)
			return (error);
		if ((error = copyin(ifr->ifr_data, &pfsyncr, sizeof(pfsyncr))))
			return (error);

		if (pfsyncr.pfsyncr_syncpeer.s_addr == 0)
			sc->sc_sync_peer.s_addr = INADDR_PFSYNC_GROUP;
		else
			sc->sc_sync_peer.s_addr =
			    pfsyncr.pfsyncr_syncpeer.s_addr;

		if (pfsyncr.pfsyncr_maxupdates > 255)
			return (EINVAL);
		sc->sc_maxupdates = pfsyncr.pfsyncr_maxupdates;

		sc->sc_defer = pfsyncr.pfsyncr_defer;

		if (pfsyncr.pfsyncr_syncdev[0] == 0) {
			if ((ifp0 = if_get(sc->sc_sync_ifidx)) != NULL) {
				if_linkstatehook_del(ifp0, &sc->sc_ltask);
				if_detachhook_del(ifp0, &sc->sc_dtask);
			}
			if_put(ifp0);
			sc->sc_sync_ifidx = 0;
			if (imo->imo_num_memberships > 0) {
				in_delmulti(imo->imo_membership[
				    --imo->imo_num_memberships]);
				imo->imo_ifidx = 0;
			}
			break;
		}

		if ((sifp = if_unit(pfsyncr.pfsyncr_syncdev)) == NULL)
			return (EINVAL);

		ifp0 = if_get(sc->sc_sync_ifidx);

		if (sifp->if_mtu < sc->sc_if.if_mtu || (ifp0 != NULL &&
		    sifp->if_mtu < ifp0->if_mtu) ||
		    sifp->if_mtu < MCLBYTES - sizeof(struct ip))
			pfsync_sendout();

		if (ifp0) {
			if_linkstatehook_del(ifp0, &sc->sc_ltask);
			if_detachhook_del(ifp0, &sc->sc_dtask);
		}
		if_put(ifp0);
		sc->sc_sync_ifidx = sifp->if_index;

		if (imo->imo_num_memberships > 0) {
			in_delmulti(imo->imo_membership[--imo->imo_num_memberships]);
			imo->imo_ifidx = 0;
		}

		if (sc->sc_sync_peer.s_addr == INADDR_PFSYNC_GROUP) {
			struct in_addr addr;

			if (!(sifp->if_flags & IFF_MULTICAST)) {
				sc->sc_sync_ifidx = 0;
				if_put(sifp);
				return (EADDRNOTAVAIL);
			}

			addr.s_addr = INADDR_PFSYNC_GROUP;

			if ((imo->imo_membership[0] =
			    in_addmulti(&addr, sifp)) == NULL) {
				sc->sc_sync_ifidx = 0;
				if_put(sifp);
				return (ENOBUFS);
			}
			imo->imo_num_memberships++;
			imo->imo_ifidx = sc->sc_sync_ifidx;
			imo->imo_ttl = PFSYNC_DFLTTL;
			imo->imo_loop = 0;
		}

		ip = &sc->sc_template;
		bzero(ip, sizeof(*ip));
		ip->ip_v = IPVERSION;
		ip->ip_hl = sizeof(sc->sc_template) >> 2;
		ip->ip_tos = IPTOS_LOWDELAY;
		/* len and id are set later */
		ip->ip_off = htons(IP_DF);
		ip->ip_ttl = PFSYNC_DFLTTL;
		ip->ip_p = IPPROTO_PFSYNC;
		ip->ip_src.s_addr = INADDR_ANY;
		ip->ip_dst.s_addr = sc->sc_sync_peer.s_addr;

		if_linkstatehook_add(sifp, &sc->sc_ltask);
		if_detachhook_add(sifp, &sc->sc_dtask);
		if_put(sifp);

		pfsync_request_full_update(sc);

		break;

	default:
		return (ENOTTY);
	}

	return (0);
}

void
pfsync_out_state(struct pf_state *st, void *buf)
{
	struct pfsync_state *sp = buf;

	pf_state_export(sp, st);
}

void
pfsync_out_iack(struct pf_state *st, void *buf)
{
	struct pfsync_ins_ack *iack = buf;

	iack->id = st->id;
	iack->creatorid = st->creatorid;
}

void
pfsync_out_upd_c(struct pf_state *st, void *buf)
{
	struct pfsync_upd_c *up = buf;

	bzero(up, sizeof(*up));
	up->id = st->id;
	pf_state_peer_hton(&st->src, &up->src);
	pf_state_peer_hton(&st->dst, &up->dst);
	up->creatorid = st->creatorid;
	up->timeout = st->timeout;
}

void
pfsync_out_del(struct pf_state *st, void *buf)
{
	struct pfsync_del_c *dp = buf;

	dp->id = st->id;
	dp->creatorid = st->creatorid;

	SET(st->state_flags, PFSTATE_NOSYNC);
}

void
pfsync_grab_snapshot(struct pfsync_snapshot *sn, struct pfsync_softc *sc)
{
	int q;
	struct pf_state *st;
	struct pfsync_upd_req_item *ur;
#if defined(IPSEC)
	struct tdb *tdb;
#endif

	sn->sn_sc = sc;

	mtx_enter(&sc->sc_st_mtx);
	mtx_enter(&sc->sc_upd_req_mtx);
	mtx_enter(&sc->sc_tdb_mtx);

	for (q = 0; q < PFSYNC_S_COUNT; q++) {
		TAILQ_INIT(&sn->sn_qs[q]);

		while ((st = TAILQ_FIRST(&sc->sc_qs[q])) != NULL) {
			TAILQ_REMOVE(&sc->sc_qs[q], st, sync_list);
			mtx_enter(&st->mtx);
			if (st->snapped == 0) {
				TAILQ_INSERT_TAIL(&sn->sn_qs[q], st, sync_snap);
				st->snapped = 1;
				mtx_leave(&st->mtx);
			} else {
				/*
				 * item is on snapshot list already, so we can
				 * skip it now.
				 */
				mtx_leave(&st->mtx);
				pf_state_unref(st);
			}
		}
	}

	TAILQ_INIT(&sn->sn_upd_req_list);
	while ((ur = TAILQ_FIRST(&sc->sc_upd_req_list)) != NULL) {
		TAILQ_REMOVE(&sc->sc_upd_req_list, ur, ur_entry);
		TAILQ_INSERT_TAIL(&sn->sn_upd_req_list, ur, ur_snap);
	}

	TAILQ_INIT(&sn->sn_tdb_q);
#if defined(IPSEC)
	while ((tdb = TAILQ_FIRST(&sc->sc_tdb_q)) != NULL) {
		TAILQ_REMOVE(&sc->sc_tdb_q, tdb, tdb_sync_entry);
		TAILQ_INSERT_TAIL(&sn->sn_tdb_q, tdb, tdb_sync_snap);

		mtx_enter(&tdb->tdb_mtx);
		KASSERT(!ISSET(tdb->tdb_flags, TDBF_PFSYNC_SNAPPED));
		SET(tdb->tdb_flags, TDBF_PFSYNC_SNAPPED);
		mtx_leave(&tdb->tdb_mtx);
	}
#endif

	sn->sn_len = sc->sc_len;
	sc->sc_len = PFSYNC_MINPKT;

	sn->sn_plus = sc->sc_plus;
	sc->sc_plus = NULL;
	sn->sn_pluslen = sc->sc_pluslen;
	sc->sc_pluslen = 0;

	mtx_leave(&sc->sc_tdb_mtx);
	mtx_leave(&sc->sc_upd_req_mtx);
	mtx_leave(&sc->sc_st_mtx);
}

void
pfsync_drop_snapshot(struct pfsync_snapshot *sn)
{
	struct pf_state *st;
	struct pfsync_upd_req_item *ur;
#if defined(IPSEC)
	struct tdb *t;
#endif
	int q;

	for (q = 0; q < PFSYNC_S_COUNT; q++) {
		if (TAILQ_EMPTY(&sn->sn_qs[q]))
			continue;

		while ((st = TAILQ_FIRST(&sn->sn_qs[q])) != NULL) {
			mtx_enter(&st->mtx);
			KASSERT(st->sync_state == q);
			KASSERT(st->snapped == 1);
			TAILQ_REMOVE(&sn->sn_qs[q], st, sync_snap);
			st->sync_state = PFSYNC_S_NONE;
			st->snapped = 0;
			mtx_leave(&st->mtx);
			pf_state_unref(st);
		}
	}

	while ((ur = TAILQ_FIRST(&sn->sn_upd_req_list)) != NULL) {
		TAILQ_REMOVE(&sn->sn_upd_req_list, ur, ur_snap);
		pool_put(&sn->sn_sc->sc_pool, ur);
	}

#if defined(IPSEC)
	while ((t = TAILQ_FIRST(&sn->sn_tdb_q)) != NULL) {
		TAILQ_REMOVE(&sn->sn_tdb_q, t, tdb_sync_snap);
		mtx_enter(&t->tdb_mtx);
		KASSERT(ISSET(t->tdb_flags, TDBF_PFSYNC_SNAPPED));
		CLR(t->tdb_flags, TDBF_PFSYNC_SNAPPED);
		CLR(t->tdb_flags, TDBF_PFSYNC);
		mtx_leave(&t->tdb_mtx);
	}
#endif
}

int
pfsync_is_snapshot_empty(struct pfsync_snapshot *sn)
{
	int	q;

	for (q = 0; q < PFSYNC_S_COUNT; q++)
		if (!TAILQ_EMPTY(&sn->sn_qs[q]))
			return (0);

	if (!TAILQ_EMPTY(&sn->sn_upd_req_list))
		return (0);

	if (!TAILQ_EMPTY(&sn->sn_tdb_q))
		return (0);

	return (sn->sn_plus == NULL);
}

void
pfsync_drop(struct pfsync_softc *sc)
{
	struct pfsync_snapshot	sn;

	pfsync_grab_snapshot(&sn, sc);
	pfsync_drop_snapshot(&sn);
}

void
pfsync_send_dispatch(void *xmq)
{
	struct mbuf_queue *mq = xmq;
	struct pfsync_softc *sc;
	struct mbuf *m;
	struct mbuf_list ml;
	int error;

	mq_delist(mq, &ml);
	if (ml_empty(&ml))
		return;

	NET_LOCK();
	sc = pfsyncif;
	if (sc == NULL) {
		ml_purge(&ml);
		goto done;
	}

	while ((m = ml_dequeue(&ml)) != NULL) {
		if ((error = ip_output(m, NULL, NULL, IP_RAWOUTPUT,
		    &sc->sc_imo, NULL, 0)) == 0)
			pfsyncstat_inc(pfsyncs_opackets);
		else {
			DPFPRINTF(LOG_DEBUG,
			    "ip_output() @ %s failed (%d)\n", __func__, error);
			pfsyncstat_inc(pfsyncs_oerrors);
		}
	}
done:
	NET_UNLOCK();
}

void
pfsync_send_pkt(struct mbuf *m)
{
	if (mq_enqueue(&pfsync_mq, m) != 0) {
		pfsyncstat_inc(pfsyncs_oerrors);
		DPFPRINTF(LOG_DEBUG, "mq_enqueue() @ %s failed, queue full\n",
		    __func__);
	} else
		task_add(net_tq(0), &pfsync_task);
}

void
pfsync_sendout(void)
{
	struct pfsync_snapshot sn;
	struct pfsync_softc *sc = pfsyncif;
#if NBPFILTER > 0
	struct ifnet *ifp = &sc->sc_if;
#endif
	struct mbuf *m;
	struct ip *ip;
	struct pfsync_header *ph;
	struct pfsync_subheader *subh;
	struct pf_state *st;
	struct pfsync_upd_req_item *ur;
	int offset;
	int q, count = 0;

	if (sc == NULL || sc->sc_len == PFSYNC_MINPKT)
		return;

	if (!ISSET(sc->sc_if.if_flags, IFF_RUNNING) ||
#if NBPFILTER > 0
	    (ifp->if_bpf == NULL && sc->sc_sync_ifidx == 0)) {
#else
	    sc->sc_sync_ifidx == 0) {
#endif
		pfsync_drop(sc);
		return;
	}

	pfsync_grab_snapshot(&sn, sc);

	/*
	 * Check below is sufficient to prevent us from sending empty packets,
	 * but it does not stop us from sending short packets.
	 */
	if (pfsync_is_snapshot_empty(&sn))
		return;

	MGETHDR(m, M_DONTWAIT, MT_DATA);
	if (m == NULL) {
		sc->sc_if.if_oerrors++;
		pfsyncstat_inc(pfsyncs_onomem);
		pfsync_drop_snapshot(&sn);
		return;
	}

	if (max_linkhdr + sn.sn_len > MHLEN) {
		MCLGETL(m, M_DONTWAIT, max_linkhdr + sn.sn_len);
		if (!ISSET(m->m_flags, M_EXT)) {
			m_free(m);
			sc->sc_if.if_oerrors++;
			pfsyncstat_inc(pfsyncs_onomem);
			pfsync_drop_snapshot(&sn);
			return;
		}
	}
	m->m_data += max_linkhdr;
	m->m_len = m->m_pkthdr.len = sn.sn_len;

	/* build the ip header */
	ip = mtod(m, struct ip *);
	bcopy(&sc->sc_template, ip, sizeof(*ip));
	offset = sizeof(*ip);

	ip->ip_len = htons(m->m_pkthdr.len);
	ip->ip_id = htons(ip_randomid());

	/* build the pfsync header */
	ph = (struct pfsync_header *)(m->m_data + offset);
	bzero(ph, sizeof(*ph));
	offset += sizeof(*ph);

	ph->version = PFSYNC_VERSION;
	ph->len = htons(sn.sn_len - sizeof(*ip));
	bcopy(pf_status.pf_chksum, ph->pfcksum, PF_MD5_DIGEST_LENGTH);

	if (!TAILQ_EMPTY(&sn.sn_upd_req_list)) {
		subh = (struct pfsync_subheader *)(m->m_data + offset);
		offset += sizeof(*subh);

		count = 0;
		while ((ur = TAILQ_FIRST(&sn.sn_upd_req_list)) != NULL) {
			TAILQ_REMOVE(&sn.sn_upd_req_list, ur, ur_snap);

			bcopy(&ur->ur_msg, m->m_data + offset,
			    sizeof(ur->ur_msg));
			offset += sizeof(ur->ur_msg);

			pool_put(&sc->sc_pool, ur);

			count++;
		}

		bzero(subh, sizeof(*subh));
		subh->len = sizeof(ur->ur_msg) >> 2;
		subh->action = PFSYNC_ACT_UPD_REQ;
		subh->count = htons(count);
	}

	/* has someone built a custom region for us to add? */
	if (sn.sn_plus != NULL) {
		bcopy(sn.sn_plus, m->m_data + offset, sn.sn_pluslen);
		offset += sn.sn_pluslen;
		sn.sn_plus = NULL;	/* XXX memory leak ? */
	}

#if defined(IPSEC)
	if (!TAILQ_EMPTY(&sn.sn_tdb_q)) {
		struct tdb *t;

		subh = (struct pfsync_subheader *)(m->m_data + offset);
		offset += sizeof(*subh);

		count = 0;
		while ((t = TAILQ_FIRST(&sn.sn_tdb_q)) != NULL) {
			TAILQ_REMOVE(&sn.sn_tdb_q, t, tdb_sync_snap);
			pfsync_out_tdb(t, m->m_data + offset);
			offset += sizeof(struct pfsync_tdb);
			mtx_enter(&t->tdb_mtx);
			KASSERT(ISSET(t->tdb_flags, TDBF_PFSYNC_SNAPPED));
			CLR(t->tdb_flags, TDBF_PFSYNC_SNAPPED);
			CLR(t->tdb_flags, TDBF_PFSYNC);
			mtx_leave(&t->tdb_mtx);
			tdb_unref(t);
			count++;
		}

		bzero(subh, sizeof(*subh));
		subh->action = PFSYNC_ACT_TDB;
		subh->len = sizeof(struct pfsync_tdb) >> 2;
		subh->count = htons(count);
	}
#endif

	/* walk the queues */
	for (q = 0; q < PFSYNC_S_COUNT; q++) {
		if (TAILQ_EMPTY(&sn.sn_qs[q]))
			continue;

		subh = (struct pfsync_subheader *)(m->m_data + offset);
		offset += sizeof(*subh);

		count = 0;
		while ((st = TAILQ_FIRST(&sn.sn_qs[q])) != NULL) {
			mtx_enter(&st->mtx);
			TAILQ_REMOVE(&sn.sn_qs[q], st, sync_snap);
			KASSERT(st->sync_state == q);
			KASSERT(st->snapped == 1);
			st->sync_state = PFSYNC_S_NONE;
			st->snapped = 0;
			pfsync_qs[q].write(st, m->m_data + offset);
			offset += pfsync_qs[q].len;
			mtx_leave(&st->mtx);

			pf_state_unref(st);
			count++;
		}

		bzero(subh, sizeof(*subh));
		subh->action = pfsync_qs[q].action;
		subh->len = pfsync_qs[q].len >> 2;
		subh->count = htons(count);
	}

	/* we're done, let's put it on the wire */
#if NBPFILTER > 0
	if (ifp->if_bpf) {
		m->m_data += sizeof(*ip);
		m->m_len = m->m_pkthdr.len = sn.sn_len - sizeof(*ip);
		bpf_mtap(ifp->if_bpf, m, BPF_DIRECTION_OUT);
		m->m_data -= sizeof(*ip);
		m->m_len = m->m_pkthdr.len = sn.sn_len;
	}

	if (sc->sc_sync_ifidx == 0) {
		sc->sc_len = PFSYNC_MINPKT;
		m_freem(m);
		return;
	}
#endif

	sc->sc_if.if_opackets++;
	sc->sc_if.if_obytes += m->m_pkthdr.len;

	m->m_pkthdr.ph_rtableid = sc->sc_if.if_rdomain;

	pfsync_send_pkt(m);
}

void
pfsync_insert_state(struct pf_state *st)
{
	struct pfsync_softc *sc = pfsyncif;

	NET_ASSERT_LOCKED();

	if (ISSET(st->rule.ptr->rule_flag, PFRULE_NOSYNC) ||
	    st->key[PF_SK_WIRE]->proto == IPPROTO_PFSYNC) {
		SET(st->state_flags, PFSTATE_NOSYNC);
		return;
	}

	if (sc == NULL || !ISSET(sc->sc_if.if_flags, IFF_RUNNING) ||
	    ISSET(st->state_flags, PFSTATE_NOSYNC))
		return;

	if (sc->sc_len == PFSYNC_MINPKT)
		timeout_add_sec(&sc->sc_tmo, 1);

	pfsync_q_ins(st, PFSYNC_S_INS);

	st->sync_updates = 0;
}

int
pfsync_defer(struct pf_state *st, struct mbuf *m, struct pfsync_deferral **ppd)
{
	struct pfsync_softc *sc = pfsyncif;
	struct pfsync_deferral *pd;
	unsigned int sched;

	NET_ASSERT_LOCKED();

	if (!sc->sc_defer ||
	    ISSET(st->state_flags, PFSTATE_NOSYNC) ||
	    m->m_flags & (M_BCAST|M_MCAST))
		return (0);

	pd = pool_get(&sc->sc_pool, M_NOWAIT);
	if (pd == NULL)
		return (0);

	/*
	 * deferral queue grows faster, than timeout can consume,
	 * we have to ask packet (caller) to help timer and dispatch
	 * one deferral for us.
	 *
	 * We wish to call pfsync_undefer() here. Unfortunately we can't,
	 * because pfsync_undefer() will be calling to ip_output(),
	 * which in turn will call to pf_test(), which would then attempt
	 * to grab PF_LOCK() we currently hold.
	 */
	if (sc->sc_deferred >= 128) {
		mtx_enter(&sc->sc_deferrals_mtx);
		*ppd = TAILQ_FIRST(&sc->sc_deferrals);
		if (*ppd != NULL) {
			TAILQ_REMOVE(&sc->sc_deferrals, *ppd, pd_entry);
			sc->sc_deferred--;
		}
		mtx_leave(&sc->sc_deferrals_mtx);
	} else
		*ppd = NULL;

	m->m_pkthdr.pf.flags |= PF_TAG_GENERATED;
	SET(st->state_flags, PFSTATE_ACK);

	pd->pd_st = pf_state_ref(st);
	pd->pd_m = m;

	pd->pd_deadline = getnsecuptime() + PFSYNC_DEFER_NSEC;

	mtx_enter(&sc->sc_deferrals_mtx);
	sched = TAILQ_EMPTY(&sc->sc_deferrals);

	TAILQ_INSERT_TAIL(&sc->sc_deferrals, pd, pd_entry);
	sc->sc_deferred++;
	mtx_leave(&sc->sc_deferrals_mtx);

	if (sched)
		timeout_add_nsec(&sc->sc_deferrals_tmo, PFSYNC_DEFER_NSEC);

	schednetisr(NETISR_PFSYNC);

	return (1);
}

void
pfsync_undefer_notify(struct pfsync_deferral *pd)
{
	struct pf_pdesc pdesc;
	struct pf_state *st = pd->pd_st;

	/*
	 * pf_remove_state removes the state keys and sets st->timeout
	 * to PFTM_UNLINKED. this is done under NET_LOCK which should
	 * be held here, so we can use PFTM_UNLINKED as a test for
	 * whether the state keys are set for the address family
	 * lookup.
	 */

	if (st->timeout == PFTM_UNLINKED)
		return;

	if (st->rt == PF_ROUTETO) {
		if (pf_setup_pdesc(&pdesc, st->key[PF_SK_WIRE]->af,
		    st->direction, st->kif, pd->pd_m, NULL) != PF_PASS)
			return;
		switch (st->key[PF_SK_WIRE]->af) {
		case AF_INET:
			pf_route(&pdesc, st);
			break;
#ifdef INET6
		case AF_INET6:
			pf_route6(&pdesc, st);
			break;
#endif /* INET6 */
		default:
			unhandled_af(st->key[PF_SK_WIRE]->af);
		}
		pd->pd_m = pdesc.m;
	} else {
		switch (st->key[PF_SK_WIRE]->af) {
		case AF_INET:
			ip_output(pd->pd_m, NULL, NULL, 0, NULL, NULL, 0);
			break;
#ifdef INET6
		case AF_INET6:
			ip6_output(pd->pd_m, NULL, NULL, 0, NULL, NULL);
			break;
#endif /* INET6 */
		default:
			unhandled_af(st->key[PF_SK_WIRE]->af);
		}

		pd->pd_m = NULL;
	}
}

void
pfsync_free_deferral(struct pfsync_deferral *pd)
{
	struct pfsync_softc *sc = pfsyncif;

	pf_state_unref(pd->pd_st);
	m_freem(pd->pd_m);
	pool_put(&sc->sc_pool, pd);
}

void
pfsync_undefer(struct pfsync_deferral *pd, int drop)
{
	struct pfsync_softc *sc = pfsyncif;

	NET_ASSERT_LOCKED();

	if (sc == NULL)
		return;

	CLR(pd->pd_st->state_flags, PFSTATE_ACK);
	if (!drop)
		pfsync_undefer_notify(pd);

	pfsync_free_deferral(pd);
}

void
pfsync_deferrals_tmo(void *arg)
{
	struct pfsync_softc *sc = arg;
	struct pfsync_deferral *pd;
	uint64_t now, nsec = 0;
	struct pfsync_deferrals pds = TAILQ_HEAD_INITIALIZER(pds);

	now = getnsecuptime();

	mtx_enter(&sc->sc_deferrals_mtx);
	for (;;) {
		pd = TAILQ_FIRST(&sc->sc_deferrals);
		if (pd == NULL)
			break;

		if (now < pd->pd_deadline) {
			nsec = pd->pd_deadline - now;
			break;
		}

		TAILQ_REMOVE(&sc->sc_deferrals, pd, pd_entry);
		sc->sc_deferred--;
		TAILQ_INSERT_TAIL(&pds, pd, pd_entry);
	}
	mtx_leave(&sc->sc_deferrals_mtx);

	if (nsec > 0) {
		/* we were looking at a pd, but it wasn't old enough */
		timeout_add_nsec(&sc->sc_deferrals_tmo, nsec);
	}

	if (TAILQ_EMPTY(&pds))
		return;

	NET_LOCK();
	while ((pd = TAILQ_FIRST(&pds)) != NULL) {
		TAILQ_REMOVE(&pds, pd, pd_entry);

		pfsync_undefer(pd, 0);
	}
	NET_UNLOCK();
}

void
pfsync_deferred(struct pf_state *st, int drop)
{
	struct pfsync_softc *sc = pfsyncif;
	struct pfsync_deferral *pd;

	NET_ASSERT_LOCKED();

	mtx_enter(&sc->sc_deferrals_mtx);
	TAILQ_FOREACH(pd, &sc->sc_deferrals, pd_entry) {
		 if (pd->pd_st == st) {
			TAILQ_REMOVE(&sc->sc_deferrals, pd, pd_entry);
			sc->sc_deferred--;
			break;
		}
	}
	mtx_leave(&sc->sc_deferrals_mtx);

	if (pd != NULL)
		pfsync_undefer(pd, drop);
}

void
pfsync_update_state(struct pf_state *st)
{
	struct pfsync_softc *sc = pfsyncif;
	int sync = 0;

	NET_ASSERT_LOCKED();

	if (sc == NULL || !ISSET(sc->sc_if.if_flags, IFF_RUNNING))
		return;

	if (ISSET(st->state_flags, PFSTATE_ACK))
		pfsync_deferred(st, 0);
	if (ISSET(st->state_flags, PFSTATE_NOSYNC)) {
		if (st->sync_state != PFSYNC_S_NONE)
			pfsync_q_del(st);
		return;
	}

	if (sc->sc_len == PFSYNC_MINPKT)
		timeout_add_sec(&sc->sc_tmo, 1);

	switch (st->sync_state) {
	case PFSYNC_S_UPD_C:
	case PFSYNC_S_UPD:
	case PFSYNC_S_INS:
		/* we're already handling it */

		if (st->key[PF_SK_WIRE]->proto == IPPROTO_TCP) {
			st->sync_updates++;
			if (st->sync_updates >= sc->sc_maxupdates)
				sync = 1;
		}
		break;

	case PFSYNC_S_IACK:
		pfsync_q_del(st);
	case PFSYNC_S_NONE:
		pfsync_q_ins(st, PFSYNC_S_UPD_C);
		st->sync_updates = 0;
		break;

	case PFSYNC_S_DEL:
	case PFSYNC_S_COUNT:
	case PFSYNC_S_DEFER:
		break;

	default:
		panic("pfsync_update_state: unexpected sync state %d",
		    st->sync_state);
	}

	if (sync || (getuptime() - st->pfsync_time) < 2)
		schednetisr(NETISR_PFSYNC);
}

void
pfsync_cancel_full_update(struct pfsync_softc *sc)
{
	if (timeout_pending(&sc->sc_bulkfail_tmo) ||
	    timeout_pending(&sc->sc_bulk_tmo)) {
#if NCARP > 0
		if (!pfsync_sync_ok)
			carp_group_demote_adj(&sc->sc_if, -1,
			    "pfsync bulk cancelled");
		if (sc->sc_initial_bulk) {
			carp_group_demote_adj(&sc->sc_if, -32,
			    "pfsync init");
			sc->sc_initial_bulk = 0;
		}
#endif
		pfsync_sync_ok = 1;
		DPFPRINTF(LOG_INFO, "cancelling bulk update");
	}
	timeout_del(&sc->sc_bulkfail_tmo);
	timeout_del(&sc->sc_bulk_tmo);
	sc->sc_bulk_next = NULL;
	sc->sc_bulk_last = NULL;
	sc->sc_ureq_sent = 0;
	sc->sc_bulk_tries = 0;
}

void
pfsync_request_full_update(struct pfsync_softc *sc)
{
	if (sc->sc_sync_ifidx != 0 && ISSET(sc->sc_if.if_flags, IFF_RUNNING)) {
		/* Request a full state table update. */
		sc->sc_ureq_sent = getuptime();
#if NCARP > 0
		if (!sc->sc_link_demoted && pfsync_sync_ok)
			carp_group_demote_adj(&sc->sc_if, 1,
			    "pfsync bulk start");
#endif
		pfsync_sync_ok = 0;
		DPFPRINTF(LOG_INFO, "requesting bulk update");
		PF_LOCK();
		timeout_add(&sc->sc_bulkfail_tmo, 4 * hz +
		    pf_pool_limits[PF_LIMIT_STATES].limit /
		    ((sc->sc_if.if_mtu - PFSYNC_MINPKT) /
		    sizeof(struct pfsync_state)));
		PF_UNLOCK();
		pfsync_request_update(0, 0);
	}
}

void
pfsync_request_update(u_int32_t creatorid, u_int64_t id)
{
	struct pfsync_softc *sc = pfsyncif;
	struct pfsync_upd_req_item *item;
	size_t nlen, sclen;
	int retry;

	/*
	 * this code does nothing to prevent multiple update requests for the
	 * same state being generated.
	 */

	item = pool_get(&sc->sc_pool, PR_NOWAIT);
	if (item == NULL) {
		/* XXX stats */
		return;
	}

	item->ur_msg.id = id;
	item->ur_msg.creatorid = creatorid;

	for (;;) {
		mtx_enter(&sc->sc_upd_req_mtx);

		nlen = sizeof(struct pfsync_upd_req);
		if (TAILQ_EMPTY(&sc->sc_upd_req_list))
			nlen += sizeof(struct pfsync_subheader);

		sclen = atomic_add_long_nv(&sc->sc_len, nlen);
		retry = (sclen > sc->sc_if.if_mtu);
		if (retry)
			atomic_sub_long(&sc->sc_len, nlen);
		else
			TAILQ_INSERT_TAIL(&sc->sc_upd_req_list, item, ur_entry);

		mtx_leave(&sc->sc_upd_req_mtx);

		if (!retry)
			break;

		pfsync_sendout();
	}

	schednetisr(NETISR_PFSYNC);
}

void
pfsync_update_state_req(struct pf_state *st)
{
	struct pfsync_softc *sc = pfsyncif;

	if (sc == NULL)
		panic("pfsync_update_state_req: nonexistent instance");

	if (ISSET(st->state_flags, PFSTATE_NOSYNC)) {
		if (st->sync_state != PFSYNC_S_NONE)
			pfsync_q_del(st);
		return;
	}

	switch (st->sync_state) {
	case PFSYNC_S_UPD_C:
	case PFSYNC_S_IACK:
		pfsync_q_del(st);
	case PFSYNC_S_NONE:
		pfsync_q_ins(st, PFSYNC_S_UPD);
		schednetisr(NETISR_PFSYNC);
		return;

	case PFSYNC_S_INS:
	case PFSYNC_S_UPD:
	case PFSYNC_S_DEL:
		/* we're already handling it */
		return;

	default:
		panic("pfsync_update_state_req: unexpected sync state %d",
		    st->sync_state);
	}
}

void
pfsync_delete_state(struct pf_state *st)
{
	struct pfsync_softc *sc = pfsyncif;

	NET_ASSERT_LOCKED();

	if (sc == NULL || !ISSET(sc->sc_if.if_flags, IFF_RUNNING))
		return;

	if (ISSET(st->state_flags, PFSTATE_ACK))
		pfsync_deferred(st, 1);
	if (ISSET(st->state_flags, PFSTATE_NOSYNC)) {
		if (st->sync_state != PFSYNC_S_NONE)
			pfsync_q_del(st);
		return;
	}

	if (sc->sc_len == PFSYNC_MINPKT)
		timeout_add_sec(&sc->sc_tmo, 1);

	switch (st->sync_state) {
	case PFSYNC_S_INS:
		/* we never got to tell the world so just forget about it */
		pfsync_q_del(st);
		return;

	case PFSYNC_S_UPD_C:
	case PFSYNC_S_UPD:
	case PFSYNC_S_IACK:
		pfsync_q_del(st);
		/*
		 * FALLTHROUGH to putting it on the del list
		 * Note on reference count bookkeeping:
		 *	pfsync_q_del() drops reference for queue
		 *	ownership. But the st entry survives, because
		 *	our caller still holds a reference.
		 */

	case PFSYNC_S_NONE:
		/*
		 * We either fall through here, or there is no reference to
		 * st owned by pfsync queues at this point.
		 *
		 * Calling pfsync_q_ins() puts st to del queue. The pfsync_q_ins()
		 * grabs a reference for delete queue.
		 */
		pfsync_q_ins(st, PFSYNC_S_DEL);
		return;

	default:
		panic("pfsync_delete_state: unexpected sync state %d",
		    st->sync_state);
	}
}

void
pfsync_clear_states(u_int32_t creatorid, const char *ifname)
{
	struct pfsync_softc *sc = pfsyncif;
	struct {
		struct pfsync_subheader subh;
		struct pfsync_clr clr;
	} __packed r;

	NET_ASSERT_LOCKED();

	if (sc == NULL || !ISSET(sc->sc_if.if_flags, IFF_RUNNING))
		return;

	bzero(&r, sizeof(r));

	r.subh.action = PFSYNC_ACT_CLR;
	r.subh.len = sizeof(struct pfsync_clr) >> 2;
	r.subh.count = htons(1);

	strlcpy(r.clr.ifname, ifname, sizeof(r.clr.ifname));
	r.clr.creatorid = creatorid;

	pfsync_send_plus(&r, sizeof(r));
}

void
pfsync_iack(struct pf_state *st)
{
	pfsync_q_ins(st, PFSYNC_S_IACK);
	schednetisr(NETISR_PFSYNC);
}

void
pfsync_q_ins(struct pf_state *st, int q)
{
	struct pfsync_softc *sc = pfsyncif;
	size_t nlen, sclen;

	if (sc->sc_len < PFSYNC_MINPKT)
		panic("pfsync pkt len is too low %zd", sc->sc_len);
	do {
		mtx_enter(&sc->sc_st_mtx);
		mtx_enter(&st->mtx);

		/*
		 * There are either two threads trying to update the
		 * the same state, or the state is just being processed
		 * (is on snapshot queue).
		 */
		if (st->sync_state != PFSYNC_S_NONE) {
			mtx_leave(&st->mtx);
			mtx_leave(&sc->sc_st_mtx);
			break;
		}

		nlen = pfsync_qs[q].len;

		if (TAILQ_EMPTY(&sc->sc_qs[q]))
			nlen += sizeof(struct pfsync_subheader);

		sclen = atomic_add_long_nv(&sc->sc_len, nlen);
		if (sclen > sc->sc_if.if_mtu) {
			atomic_sub_long(&sc->sc_len, nlen);
			mtx_leave(&st->mtx);
			mtx_leave(&sc->sc_st_mtx);
			pfsync_sendout();
			continue;
		}

		pf_state_ref(st);

		TAILQ_INSERT_TAIL(&sc->sc_qs[q], st, sync_list);
		st->sync_state = q;
		mtx_leave(&st->mtx);
		mtx_leave(&sc->sc_st_mtx);
	} while (0);
}

void
pfsync_q_del(struct pf_state *st)
{
	struct pfsync_softc *sc = pfsyncif;
	int q;

	mtx_enter(&sc->sc_st_mtx);
	mtx_enter(&st->mtx);
	q = st->sync_state;
	/*
	 * re-check under mutex
	 * if state is snapped already, then just bail out, because we came
	 * too late, the state is being just processed/dispatched to peer.
	 */
	if ((q == PFSYNC_S_NONE) || (st->snapped)) {
		mtx_leave(&st->mtx);
		mtx_leave(&sc->sc_st_mtx);
		return;
	}
	atomic_sub_long(&sc->sc_len, pfsync_qs[q].len);
	TAILQ_REMOVE(&sc->sc_qs[q], st, sync_list);
	if (TAILQ_EMPTY(&sc->sc_qs[q]))
		atomic_sub_long(&sc->sc_len, sizeof (struct pfsync_subheader));
	st->sync_state = PFSYNC_S_NONE;
	mtx_leave(&st->mtx);
	mtx_leave(&sc->sc_st_mtx);

	pf_state_unref(st);
}

#if defined(IPSEC)
void
pfsync_update_tdb(struct tdb *t, int output)
{
	struct pfsync_softc *sc = pfsyncif;
	size_t nlen, sclen;

	if (sc == NULL)
		return;

	if (!ISSET(t->tdb_flags, TDBF_PFSYNC)) {
		do {
			mtx_enter(&sc->sc_tdb_mtx);
			nlen = sizeof(struct pfsync_tdb);

			mtx_enter(&t->tdb_mtx);
			if (ISSET(t->tdb_flags, TDBF_PFSYNC)) {
				/* we've lost race, no action for us then */
				mtx_leave(&t->tdb_mtx);
				mtx_leave(&sc->sc_tdb_mtx);
				break;
			}

			if (TAILQ_EMPTY(&sc->sc_tdb_q))
				nlen += sizeof(struct pfsync_subheader);

			sclen = atomic_add_long_nv(&sc->sc_len, nlen);
			if (sclen > sc->sc_if.if_mtu) {
				atomic_sub_long(&sc->sc_len, nlen);
				mtx_leave(&t->tdb_mtx);
				mtx_leave(&sc->sc_tdb_mtx);
				pfsync_sendout();
				continue;
			}

			TAILQ_INSERT_TAIL(&sc->sc_tdb_q, t, tdb_sync_entry);
			tdb_ref(t);
			SET(t->tdb_flags, TDBF_PFSYNC);
			mtx_leave(&t->tdb_mtx);

			mtx_leave(&sc->sc_tdb_mtx);
			t->tdb_updates = 0;
		} while (0);
	} else {
		if (++t->tdb_updates >= sc->sc_maxupdates)
			schednetisr(NETISR_PFSYNC);
	}

	mtx_enter(&t->tdb_mtx);
	if (output)
		SET(t->tdb_flags, TDBF_PFSYNC_RPL);
	else
		CLR(t->tdb_flags, TDBF_PFSYNC_RPL);
	mtx_leave(&t->tdb_mtx);
}
#endif

#if defined(IPSEC)
void
pfsync_delete_tdb(struct tdb *t)
{
	struct pfsync_softc *sc = pfsyncif;
	size_t nlen;

	if (sc == NULL || !ISSET(t->tdb_flags, TDBF_PFSYNC))
		return;

	mtx_enter(&sc->sc_tdb_mtx);

	/*
	 * if tdb entry is just being processed (found in snapshot),
	 * then it can not be deleted. we just came too late
	 */
	if (ISSET(t->tdb_flags, TDBF_PFSYNC_SNAPPED)) {
		mtx_leave(&sc->sc_tdb_mtx);
		return;
	}

	TAILQ_REMOVE(&sc->sc_tdb_q, t, tdb_sync_entry);

	mtx_enter(&t->tdb_mtx);
	CLR(t->tdb_flags, TDBF_PFSYNC);
	mtx_leave(&t->tdb_mtx);

	nlen = sizeof(struct pfsync_tdb);
	if (TAILQ_EMPTY(&sc->sc_tdb_q))
		nlen += sizeof(struct pfsync_subheader);
	atomic_sub_long(&sc->sc_len, nlen);

	mtx_leave(&sc->sc_tdb_mtx);

	tdb_unref(t);
}
#endif

void
pfsync_out_tdb(struct tdb *t, void *buf)
{
	struct pfsync_tdb *ut = buf;

	bzero(ut, sizeof(*ut));
	ut->spi = t->tdb_spi;
	bcopy(&t->tdb_dst, &ut->dst, sizeof(ut->dst));
	/*
	 * When a failover happens, the master's rpl is probably above
	 * what we see here (we may be up to a second late), so
	 * increase it a bit for outbound tdbs to manage most such
	 * situations.
	 *
	 * For now, just add an offset that is likely to be larger
	 * than the number of packets we can see in one second. The RFC
	 * just says the next packet must have a higher seq value.
	 *
	 * XXX What is a good algorithm for this? We could use
	 * a rate-determined increase, but to know it, we would have
	 * to extend struct tdb.
	 * XXX pt->rpl can wrap over MAXINT, but if so the real tdb
	 * will soon be replaced anyway. For now, just don't handle
	 * this edge case.
	 */
#define RPL_INCR 16384
	ut->rpl = htobe64(t->tdb_rpl + (ISSET(t->tdb_flags, TDBF_PFSYNC_RPL) ?
	    RPL_INCR : 0));
	ut->cur_bytes = htobe64(t->tdb_cur_bytes);
	ut->sproto = t->tdb_sproto;
	ut->rdomain = htons(t->tdb_rdomain);
}

void
pfsync_bulk_start(void)
{
	struct pfsync_softc *sc = pfsyncif;

	NET_ASSERT_LOCKED();

	/*
	 * pf gc via pfsync_state_in_use reads sc_bulk_next and
	 * sc_bulk_last while exclusively holding the pf_state_list
	 * rwlock. make sure it can't race with us setting these
	 * pointers. they basically act as hazards, and borrow the
	 * lists state reference count.
	 */
	rw_enter_read(&pf_state_list.pfs_rwl);

	/* get a consistent view of the list pointers */
	mtx_enter(&pf_state_list.pfs_mtx);
	if (sc->sc_bulk_next == NULL)
		sc->sc_bulk_next = TAILQ_FIRST(&pf_state_list.pfs_list);

	sc->sc_bulk_last = TAILQ_LAST(&pf_state_list.pfs_list, pf_state_queue);
	mtx_leave(&pf_state_list.pfs_mtx);

	rw_exit_read(&pf_state_list.pfs_rwl);

	DPFPRINTF(LOG_INFO, "received bulk update request");

	if (sc->sc_bulk_last == NULL)
		pfsync_bulk_status(PFSYNC_BUS_END);
	else {
		sc->sc_ureq_received = getuptime();

		pfsync_bulk_status(PFSYNC_BUS_START);
		timeout_add(&sc->sc_bulk_tmo, 0);
	}
}

void
pfsync_bulk_update(void *arg)
{
	struct pfsync_softc *sc;
	struct pf_state *st;
	int i = 0;

	NET_LOCK();
	sc = pfsyncif;
	if (sc == NULL)
		goto out;

	rw_enter_read(&pf_state_list.pfs_rwl);
	st = sc->sc_bulk_next;
	sc->sc_bulk_next = NULL;

	if (st == NULL) {
		rw_exit_read(&pf_state_list.pfs_rwl);
		goto out;
	}

	for (;;) {
		if (st->sync_state == PFSYNC_S_NONE &&
		    st->timeout < PFTM_MAX &&
		    st->pfsync_time <= sc->sc_ureq_received) {
			pfsync_update_state_req(st);
			i++;
		}

		st = TAILQ_NEXT(st, entry_list);
		if ((st == NULL) || (st == sc->sc_bulk_last)) {
			/* we're done */
			sc->sc_bulk_last = NULL;
			pfsync_bulk_status(PFSYNC_BUS_END);
			break;
		}

		if (i > 1 && (sc->sc_if.if_mtu - sc->sc_len) <
		    sizeof(struct pfsync_state)) {
			/* we've filled a packet */
			sc->sc_bulk_next = st;
			timeout_add(&sc->sc_bulk_tmo, 1);
			break;
		}
	}

	rw_exit_read(&pf_state_list.pfs_rwl);
 out:
	NET_UNLOCK();
}

void
pfsync_bulk_status(u_int8_t status)
{
	struct {
		struct pfsync_subheader subh;
		struct pfsync_bus bus;
	} __packed r;

	struct pfsync_softc *sc = pfsyncif;

	bzero(&r, sizeof(r));

	r.subh.action = PFSYNC_ACT_BUS;
	r.subh.len = sizeof(struct pfsync_bus) >> 2;
	r.subh.count = htons(1);

	r.bus.creatorid = pf_status.hostid;
	r.bus.endtime = htonl(getuptime() - sc->sc_ureq_received);
	r.bus.status = status;

	pfsync_send_plus(&r, sizeof(r));
}

void
pfsync_bulk_fail(void *arg)
{
	struct pfsync_softc *sc;

	NET_LOCK();
	sc = pfsyncif;
	if (sc == NULL)
		goto out;
	if (sc->sc_bulk_tries++ < PFSYNC_MAX_BULKTRIES) {
		/* Try again */
		timeout_add_sec(&sc->sc_bulkfail_tmo, 5);
		pfsync_request_update(0, 0);
	} else {
		/* Pretend like the transfer was ok */
		sc->sc_ureq_sent = 0;
		sc->sc_bulk_tries = 0;
#if NCARP > 0
		if (!pfsync_sync_ok)
			carp_group_demote_adj(&sc->sc_if, -1,
			    sc->sc_link_demoted ?
			    "pfsync link state up" :
			    "pfsync bulk fail");
		if (sc->sc_initial_bulk) {
			carp_group_demote_adj(&sc->sc_if, -32,
			    "pfsync init");
			sc->sc_initial_bulk = 0;
		}
#endif
		pfsync_sync_ok = 1;
		sc->sc_link_demoted = 0;
		DPFPRINTF(LOG_ERR, "failed to receive bulk update");
	}
 out:
	NET_UNLOCK();
}

void
pfsync_send_plus(void *plus, size_t pluslen)
{
	struct pfsync_softc *sc = pfsyncif;

	if (sc->sc_len + pluslen > sc->sc_if.if_mtu)
		pfsync_sendout();

	sc->sc_plus = plus;
	sc->sc_pluslen = pluslen;
	atomic_add_long(&sc->sc_len, pluslen);

	pfsync_sendout();
}

int
pfsync_is_up(void)
{
	struct pfsync_softc *sc = pfsyncif;

	if (sc == NULL || !ISSET(sc->sc_if.if_flags, IFF_RUNNING))
		return (0);

	return (1);
}

int
pfsync_state_in_use(struct pf_state *st)
{
	struct pfsync_softc *sc = pfsyncif;

	if (sc == NULL)
		return (0);

	rw_assert_wrlock(&pf_state_list.pfs_rwl);

	if (st->sync_state != PFSYNC_S_NONE ||
	    st == sc->sc_bulk_next ||
	    st == sc->sc_bulk_last)
		return (1);

	return (0);
}

void
pfsync_timeout(void *arg)
{
	NET_LOCK();
	pfsync_sendout();
	NET_UNLOCK();
}

/* this is a softnet/netisr handler */
void
pfsyncintr(void)
{
	pfsync_sendout();
}

int
pfsync_sysctl_pfsyncstat(void *oldp, size_t *oldlenp, void *newp)
{
	struct pfsyncstats pfsyncstat;

	CTASSERT(sizeof(pfsyncstat) == (pfsyncs_ncounters * sizeof(uint64_t)));
	memset(&pfsyncstat, 0, sizeof pfsyncstat);
	counters_read(pfsynccounters, (uint64_t *)&pfsyncstat,
	    pfsyncs_ncounters);
	return (sysctl_rdstruct(oldp, oldlenp, newp,
	    &pfsyncstat, sizeof(pfsyncstat)));
}

int
pfsync_sysctl(int *name, u_int namelen, void *oldp, size_t *oldlenp, void *newp,
    size_t newlen)
{
	/* All sysctl names at this level are terminal. */
	if (namelen != 1)
		return (ENOTDIR);

	switch (name[0]) {
	case PFSYNCCTL_STATS:
		return (pfsync_sysctl_pfsyncstat(oldp, oldlenp, newp));
	default:
		return (ENOPROTOOPT);
	}
}
