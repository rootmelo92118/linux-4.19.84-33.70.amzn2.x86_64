/*
 * Copyright (c) 2007, 2010, Oracle and/or its affiliates. All rights reserved.
 *
 * Copyright (c) 2011, 2015, Intel Corporation.
 *
 *   This file is part of Lustre, https://wiki.hpdd.intel.com/
 *
 *   Portals is free software; you can redistribute it and/or
 *   modify it under the terms of version 2 of the GNU General Public
 *   License as published by the Free Software Foundation.
 *
 *   Portals is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with Portals; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */

#define DEBUG_SUBSYSTEM S_LNET
#include <libcfs/libcfs.h>
#include <lnet/lib-lnet.h>

/* This is really lnet_proc.c. You might need to update sanity test 215
 * if any file format is changed. */

static struct ctl_table_header *lnet_table_header = NULL;

#define LNET_LOFFT_BITS		(sizeof(loff_t) * 8)
/*
 * NB: max allowed LNET_CPT_BITS is 8 on 64-bit system and 2 on 32-bit system
 */
#define LNET_PROC_CPT_BITS	(LNET_CPT_BITS + 1)
/* change version, 16 bits or 8 bits */
#define LNET_PROC_VER_BITS	MAX(((MIN(LNET_LOFFT_BITS, 64)) / 4), 8)

#define LNET_PROC_HASH_BITS	LNET_PEER_HASH_BITS
/*
 * bits for peer hash offset
 * NB: we don't use the highest bit of *ppos because it's signed
 */
#define LNET_PROC_HOFF_BITS	(LNET_LOFFT_BITS -	 \
				 LNET_PROC_CPT_BITS -	 \
				 LNET_PROC_VER_BITS -	 \
				 LNET_PROC_HASH_BITS - 1)
/* bits for hash index + position */
#define LNET_PROC_HPOS_BITS	(LNET_PROC_HASH_BITS + LNET_PROC_HOFF_BITS)
/* bits for peer hash table + hash version */
#define LNET_PROC_VPOS_BITS	(LNET_PROC_HPOS_BITS + LNET_PROC_VER_BITS)

#define LNET_PROC_CPT_MASK	((1ULL << LNET_PROC_CPT_BITS) - 1)
#define LNET_PROC_VER_MASK	((1ULL << LNET_PROC_VER_BITS) - 1)
#define LNET_PROC_HASH_MASK	((1ULL << LNET_PROC_HASH_BITS) - 1)
#define LNET_PROC_HOFF_MASK	((1ULL << LNET_PROC_HOFF_BITS) - 1)

#define LNET_PROC_CPT_GET(pos)				\
	(int)(((pos) >> LNET_PROC_VPOS_BITS) & LNET_PROC_CPT_MASK)

#define LNET_PROC_VER_GET(pos)				\
	(int)(((pos) >> LNET_PROC_HPOS_BITS) & LNET_PROC_VER_MASK)

#define LNET_PROC_HASH_GET(pos)				\
	(int)(((pos) >> LNET_PROC_HOFF_BITS) & LNET_PROC_HASH_MASK)

#define LNET_PROC_HOFF_GET(pos)				\
	(int)((pos) & LNET_PROC_HOFF_MASK)

#define LNET_PROC_POS_MAKE(cpt, ver, hash, off)		\
	(((((loff_t)(cpt)) & LNET_PROC_CPT_MASK) << LNET_PROC_VPOS_BITS) |   \
	((((loff_t)(ver)) & LNET_PROC_VER_MASK) << LNET_PROC_HPOS_BITS) |   \
	((((loff_t)(hash)) & LNET_PROC_HASH_MASK) << LNET_PROC_HOFF_BITS) | \
	((off) & LNET_PROC_HOFF_MASK))

#define LNET_PROC_VERSION(v)	((unsigned int)((v) & LNET_PROC_VER_MASK))

static int __proc_lnet_stats(void *data, int write,
			     loff_t pos, void __user *buffer, int nob)
{
	int		 rc;
	struct lnet_counters *ctrs;
	int		 len;
	char		*tmpstr;
	const int	 tmpsiz = 256; /* 7 %u and 4 __u64 */

	if (write) {
		lnet_counters_reset();
		return 0;
	}

	/* read */

	LIBCFS_ALLOC(ctrs, sizeof(*ctrs));
	if (ctrs == NULL)
		return -ENOMEM;

	LIBCFS_ALLOC(tmpstr, tmpsiz);
	if (tmpstr == NULL) {
		LIBCFS_FREE(ctrs, sizeof(*ctrs));
		return -ENOMEM;
	}

	lnet_counters_get(ctrs);

	len = snprintf(tmpstr, tmpsiz,
		       "%u %u %u %u %u %u %u %llu %llu "
		       "%llu %llu",
		       ctrs->msgs_alloc, ctrs->msgs_max,
		       ctrs->errors,
		       ctrs->send_count, ctrs->recv_count,
		       ctrs->route_count, ctrs->drop_count,
		       ctrs->send_length, ctrs->recv_length,
		       ctrs->route_length, ctrs->drop_length);

	if (pos >= min_t(int, len, strlen(tmpstr)))
		rc = 0;
	else
		rc = cfs_trace_copyout_string(buffer, nob,
					      tmpstr + pos, "\n");

	LIBCFS_FREE(tmpstr, tmpsiz);
	LIBCFS_FREE(ctrs, sizeof(*ctrs));
	return rc;
}

static int
proc_lnet_stats(struct ctl_table *table, int write, void __user *buffer,
		size_t *lenp, loff_t *ppos)
{
	return lprocfs_call_handler(table->data, write, ppos, buffer, lenp,
				    __proc_lnet_stats);
}

static int
proc_lnet_routes(struct ctl_table *table, int write, void __user *buffer,
		 size_t *lenp, loff_t *ppos)
{
	const int	tmpsiz = 256;
	char		*tmpstr;
	char		*s;
	int		rc = 0;
	int		len;
	int		ver;
	int		off;

	CLASSERT(sizeof(loff_t) >= 4);

	off = LNET_PROC_HOFF_GET(*ppos);
	ver = LNET_PROC_VER_GET(*ppos);

	LASSERT(!write);

	if (*lenp == 0)
		return 0;

	LIBCFS_ALLOC(tmpstr, tmpsiz);
	if (tmpstr == NULL)
		return -ENOMEM;

	s = tmpstr; /* points to current position in tmpstr[] */

	if (*ppos == 0) {
		s += snprintf(s, tmpstr + tmpsiz - s, "Routing %s\n",
			      the_lnet.ln_routing ? "enabled" : "disabled");
		LASSERT(tmpstr + tmpsiz - s > 0);

		s += snprintf(s, tmpstr + tmpsiz - s, "%-8s %4s %8s %7s %s\n",
			      "net", "hops", "priority", "state", "router");
		LASSERT(tmpstr + tmpsiz - s > 0);

		lnet_net_lock(0);
		ver = (unsigned int)the_lnet.ln_remote_nets_version;
		lnet_net_unlock(0);
		*ppos = LNET_PROC_POS_MAKE(0, ver, 0, off);
	} else {
		struct list_head	*n;
		struct list_head	*r;
		struct lnet_route		*route = NULL;
		struct lnet_remotenet	*rnet  = NULL;
		int			skip  = off - 1;
		struct list_head	*rn_list;
		int			i;

		lnet_net_lock(0);

		if (ver != LNET_PROC_VERSION(the_lnet.ln_remote_nets_version)) {
			lnet_net_unlock(0);
			LIBCFS_FREE(tmpstr, tmpsiz);
			return -ESTALE;
		}

		for (i = 0; i < LNET_REMOTE_NETS_HASH_SIZE && route == NULL;
		     i++) {
			rn_list = &the_lnet.ln_remote_nets_hash[i];

			n = rn_list->next;

			while (n != rn_list && route == NULL) {
				rnet = list_entry(n, struct lnet_remotenet,
						  lrn_list);

				r = rnet->lrn_routes.next;

				while (r != &rnet->lrn_routes) {
					struct lnet_route *re =
						list_entry(r, struct lnet_route,
							   lr_list);
					if (skip == 0) {
						route = re;
						break;
					}

					skip--;
					r = r->next;
				}

				n = n->next;
			}
		}

		if (route != NULL) {
			__u32	     net	= rnet->lrn_net;
			__u32 hops		= route->lr_hops;
			unsigned int priority	= route->lr_priority;
			lnet_nid_t   nid	= route->lr_gateway->lpni_nid;
			int          alive	= lnet_is_route_alive(route);

			s += snprintf(s, tmpstr + tmpsiz - s,
				      "%-8s %4d %8u %7s %s\n",
				      libcfs_net2str(net), hops,
				      priority,
				      alive ? "up" : "down",
				      libcfs_nid2str(nid));
			LASSERT(tmpstr + tmpsiz - s > 0);
		}

		lnet_net_unlock(0);
	}

	len = s - tmpstr;     /* how many bytes was written */

	if (len > *lenp) {    /* linux-supplied buffer is too small */
		rc = -EINVAL;
	} else if (len > 0) { /* wrote something */
		if (copy_to_user(buffer, tmpstr, len))
			rc = -EFAULT;
		else {
			off += 1;
			*ppos = LNET_PROC_POS_MAKE(0, ver, 0, off);
		}
	}

	LIBCFS_FREE(tmpstr, tmpsiz);

	if (rc == 0)
		*lenp = len;

	return rc;
}

static int
proc_lnet_routers(struct ctl_table *table, int write, void __user *buffer,
		  size_t *lenp, loff_t *ppos)
{
	int	   rc = 0;
	char	  *tmpstr;
	char	  *s;
	const int  tmpsiz = 256;
	int	   len;
	int	   ver;
	int	   off;

	off = LNET_PROC_HOFF_GET(*ppos);
	ver = LNET_PROC_VER_GET(*ppos);

	LASSERT(!write);

	if (*lenp == 0)
		return 0;

	LIBCFS_ALLOC(tmpstr, tmpsiz);
	if (tmpstr == NULL)
		return -ENOMEM;

	s = tmpstr; /* points to current position in tmpstr[] */

	if (*ppos == 0) {
		s += snprintf(s, tmpstr + tmpsiz - s,
			      "%-4s %7s %9s %6s %12s %9s %8s %7s %s\n",
			      "ref", "rtr_ref", "alive_cnt", "state",
			      "last_ping", "ping_sent", "deadline",
			      "down_ni", "router");
		LASSERT(tmpstr + tmpsiz - s > 0);

		lnet_net_lock(0);
		ver = (unsigned int)the_lnet.ln_routers_version;
		lnet_net_unlock(0);
		*ppos = LNET_PROC_POS_MAKE(0, ver, 0, off);
	} else {
		struct list_head *r;
		struct lnet_peer_ni *peer = NULL;
		int		  skip = off - 1;

		lnet_net_lock(0);

		if (ver != LNET_PROC_VERSION(the_lnet.ln_routers_version)) {
			lnet_net_unlock(0);

			LIBCFS_FREE(tmpstr, tmpsiz);
			return -ESTALE;
		}

		r = the_lnet.ln_routers.next;

		while (r != &the_lnet.ln_routers) {
			struct lnet_peer_ni *lp =
			  list_entry(r, struct lnet_peer_ni,
				     lpni_rtr_list);

			if (skip == 0) {
				peer = lp;
				break;
			}

			skip--;
			r = r->next;
		}

		if (peer != NULL) {
			lnet_nid_t nid = peer->lpni_nid;
			cfs_time_t now = cfs_time_current();
			cfs_time_t deadline = peer->lpni_ping_deadline;
			int nrefs     = atomic_read(&peer->lpni_refcount);
			int nrtrrefs  = peer->lpni_rtr_refcount;
			int alive_cnt = peer->lpni_alive_count;
			int alive     = peer->lpni_alive;
			int pingsent  = !peer->lpni_ping_notsent;
			int last_ping = cfs_duration_sec(cfs_time_sub(now,
						     peer->lpni_ping_timestamp));
			int down_ni   = 0;
			struct lnet_route *rtr;

			if ((peer->lpni_ping_feats &
			     LNET_PING_FEAT_NI_STATUS) != 0) {
				list_for_each_entry(rtr, &peer->lpni_routes,
						    lr_gwlist) {
					/* downis on any route should be the
					 * number of downis on the gateway */
					if (rtr->lr_downis != 0) {
						down_ni = rtr->lr_downis;
						break;
					}
				}
			}

			if (deadline == 0)
				s += snprintf(s, tmpstr + tmpsiz - s,
					      "%-4d %7d %9d %6s %12d %9d %8s %7d %s\n",
					      nrefs, nrtrrefs, alive_cnt,
					      alive ? "up" : "down", last_ping,
					      pingsent, "NA", down_ni,
					      libcfs_nid2str(nid));
			else
				s += snprintf(s, tmpstr + tmpsiz - s,
					      "%-4d %7d %9d %6s %12d %9d %8lu %7d %s\n",
					      nrefs, nrtrrefs, alive_cnt,
					      alive ? "up" : "down", last_ping,
					      pingsent,
					      cfs_duration_sec(cfs_time_sub(deadline, now)),
					      down_ni, libcfs_nid2str(nid));
			LASSERT(tmpstr + tmpsiz - s > 0);
		}

		lnet_net_unlock(0);
	}

	len = s - tmpstr;     /* how many bytes was written */

	if (len > *lenp) {    /* linux-supplied buffer is too small */
		rc = -EINVAL;
	} else if (len > 0) { /* wrote something */
		if (copy_to_user(buffer, tmpstr, len))
			rc = -EFAULT;
		else {
			off += 1;
			*ppos = LNET_PROC_POS_MAKE(0, ver, 0, off);
		}
	}

	LIBCFS_FREE(tmpstr, tmpsiz);

	if (rc == 0)
		*lenp = len;

	return rc;
}

/* TODO: there should be no direct access to ptable. We should add a set
 * of APIs that give access to the ptable and its members */
static int
proc_lnet_peers(struct ctl_table *table, int write, void __user *buffer,
		size_t *lenp, loff_t *ppos)
{
	const int		tmpsiz	= 256;
	struct lnet_peer_table	*ptable;
	char			*tmpstr = NULL;
	char			*s;
	int			cpt  = LNET_PROC_CPT_GET(*ppos);
	int			ver  = LNET_PROC_VER_GET(*ppos);
	int			hash = LNET_PROC_HASH_GET(*ppos);
	int			hoff = LNET_PROC_HOFF_GET(*ppos);
	int			rc = 0;
	int			len;

	if (write) {
		int i;
		struct lnet_peer_ni *peer;

		cfs_percpt_for_each(ptable, i, the_lnet.ln_peer_tables) {
			lnet_net_lock(i);
			for (hash = 0; hash < LNET_PEER_HASH_SIZE; hash++) {
				list_for_each_entry(peer,
						    &ptable->pt_hash[hash],
						    lpni_hashlist) {
					peer->lpni_mintxcredits =
						peer->lpni_txcredits;
					peer->lpni_minrtrcredits =
						peer->lpni_rtrcredits;
				}
			}
			lnet_net_unlock(i);
		}
		*ppos += *lenp;
		return 0;
	}

	if (*lenp == 0)
		return 0;

	CLASSERT(LNET_PROC_HASH_BITS >= LNET_PEER_HASH_BITS);

	if (cpt >= LNET_CPT_NUMBER) {
		*lenp = 0;
		return 0;
	}

	LIBCFS_ALLOC(tmpstr, tmpsiz);
	if (tmpstr == NULL)
		return -ENOMEM;

	s = tmpstr; /* points to current position in tmpstr[] */

	if (*ppos == 0) {
		s += snprintf(s, tmpstr + tmpsiz - s,
			      "%-24s %4s %5s %5s %5s %5s %5s %5s %5s %s\n",
			      "nid", "refs", "state", "last", "max",
			      "rtr", "min", "tx", "min", "queue");
		LASSERT(tmpstr + tmpsiz - s > 0);

		hoff++;
	} else {
		struct lnet_peer_ni	*peer;
		struct list_head	*p;
		int			skip;

 again:
		p = NULL;
		peer = NULL;
		skip = hoff - 1;

		lnet_net_lock(cpt);
		ptable = the_lnet.ln_peer_tables[cpt];
		if (hoff == 1)
			ver = LNET_PROC_VERSION(ptable->pt_version);

		if (ver != LNET_PROC_VERSION(ptable->pt_version)) {
			lnet_net_unlock(cpt);
			LIBCFS_FREE(tmpstr, tmpsiz);
			return -ESTALE;
		}

		while (hash < LNET_PEER_HASH_SIZE) {
			if (p == NULL)
				p = ptable->pt_hash[hash].next;

			while (p != &ptable->pt_hash[hash]) {
				struct lnet_peer_ni *lp =
				  list_entry(p, struct lnet_peer_ni,
					     lpni_hashlist);
				if (skip == 0) {
					peer = lp;

					/* minor optimization: start from idx+1
					 * on next iteration if we've just
					 * drained lpni_hashlist */
					if (lp->lpni_hashlist.next ==
					    &ptable->pt_hash[hash]) {
						hoff = 1;
						hash++;
					} else {
						hoff++;
					}

					break;
				}

				skip--;
				p = lp->lpni_hashlist.next;
			}

			if (peer != NULL)
				break;

			p = NULL;
			hoff = 1;
			hash++;
                }

		if (peer != NULL) {
			lnet_nid_t nid = peer->lpni_nid;
			int nrefs = atomic_read(&peer->lpni_refcount);
			int lastalive = -1;
			char *aliveness = "NA";
			int maxcr = (peer->lpni_net) ?
			  peer->lpni_net->net_tunables.lct_peer_tx_credits : 0;
			int txcr = peer->lpni_txcredits;
			int mintxcr = peer->lpni_mintxcredits;
			int rtrcr = peer->lpni_rtrcredits;
			int minrtrcr = peer->lpni_minrtrcredits;
			int txqnob = peer->lpni_txqnob;

			if (lnet_isrouter(peer) ||
			    lnet_peer_aliveness_enabled(peer))
				aliveness = peer->lpni_alive ? "up" : "down";

			if (lnet_peer_aliveness_enabled(peer)) {
				cfs_time_t now = cfs_time_current();
				cfs_duration_t delta;

				delta = cfs_time_sub(now, peer->lpni_last_alive);
				lastalive = cfs_duration_sec(delta);

				/* No need to mess up peers contents with
				 * arbitrarily long integers - it suffices to
				 * know that lastalive is more than 10000s old
				 */
				if (lastalive >= 10000)
					lastalive = 9999;
			}

			lnet_net_unlock(cpt);

			s += snprintf(s, tmpstr + tmpsiz - s,
				      "%-24s %4d %5s %5d %5d %5d %5d %5d %5d %d\n",
				      libcfs_nid2str(nid), nrefs, aliveness,
				      lastalive, maxcr, rtrcr, minrtrcr, txcr,
				      mintxcr, txqnob);
			LASSERT(tmpstr + tmpsiz - s > 0);

		} else { /* peer is NULL */
			lnet_net_unlock(cpt);
		}

		if (hash == LNET_PEER_HASH_SIZE) {
			cpt++;
			hash = 0;
			hoff = 1;
			if (peer == NULL && cpt < LNET_CPT_NUMBER)
				goto again;
		}
	}

	len = s - tmpstr;     /* how many bytes was written */

	if (len > *lenp) {    /* linux-supplied buffer is too small */
		rc = -EINVAL;
	} else if (len > 0) { /* wrote something */
		if (copy_to_user(buffer, tmpstr, len))
			rc = -EFAULT;
		else
			*ppos = LNET_PROC_POS_MAKE(cpt, ver, hash, hoff);
	}

	LIBCFS_FREE(tmpstr, tmpsiz);

	if (rc == 0)
		*lenp = len;

	return rc;
}

static int __proc_lnet_buffers(void *data, int write,
			       loff_t pos, void __user *buffer, int nob)
{
	char		*s;
	char		*tmpstr;
	int		tmpsiz;
	int		idx;
	int		len;
	int		rc;
	int		i;

	LASSERT(!write);

	/* (4 %d) * 4 * LNET_CPT_NUMBER */
	tmpsiz = 64 * (LNET_NRBPOOLS + 1) * LNET_CPT_NUMBER;
	LIBCFS_ALLOC(tmpstr, tmpsiz);
	if (tmpstr == NULL)
		return -ENOMEM;

	s = tmpstr; /* points to current position in tmpstr[] */

	s += snprintf(s, tmpstr + tmpsiz - s,
		      "%5s %5s %7s %7s\n",
		      "pages", "count", "credits", "min");
	LASSERT(tmpstr + tmpsiz - s > 0);

	if (the_lnet.ln_rtrpools == NULL)
		goto out; /* I'm not a router */

	for (idx = 0; idx < LNET_NRBPOOLS; idx++) {
		struct lnet_rtrbufpool *rbp;

		lnet_net_lock(LNET_LOCK_EX);
		cfs_percpt_for_each(rbp, i, the_lnet.ln_rtrpools) {
			s += snprintf(s, tmpstr + tmpsiz - s,
				      "%5d %5d %7d %7d\n",
				      rbp[idx].rbp_npages,
				      rbp[idx].rbp_nbuffers,
				      rbp[idx].rbp_credits,
				      rbp[idx].rbp_mincredits);
			LASSERT(tmpstr + tmpsiz - s > 0);
		}
		lnet_net_unlock(LNET_LOCK_EX);
	}

 out:
	len = s - tmpstr;

	if (pos >= min_t(int, len, strlen(tmpstr)))
		rc = 0;
	else
		rc = cfs_trace_copyout_string(buffer, nob,
					      tmpstr + pos, NULL);

	LIBCFS_FREE(tmpstr, tmpsiz);
	return rc;
}

static int
proc_lnet_buffers(struct ctl_table *table, int write, void __user *buffer,
		  size_t *lenp, loff_t *ppos)
{
	return lprocfs_call_handler(table->data, write, ppos, buffer, lenp,
				    __proc_lnet_buffers);
}

static int
proc_lnet_nis(struct ctl_table *table, int write, void __user *buffer,
	      size_t *lenp, loff_t *ppos)
{
	int	tmpsiz = 128 * LNET_CPT_NUMBER;
	int	rc = 0;
	char	*tmpstr;
	char	*s;
	int	len;

	if (*lenp == 0)
		return 0;

	if (write) {
		/* Just reset the min stat. */
		struct lnet_ni	*ni;
		struct lnet_net	*net;

		lnet_net_lock(0);

		list_for_each_entry(net, &the_lnet.ln_nets, net_list) {
			list_for_each_entry(ni, &net->net_ni_list, ni_netlist) {
				struct lnet_tx_queue *tq;
				int i;
				int j;

				cfs_percpt_for_each(tq, i, ni->ni_tx_queues) {
					for (j = 0; ni->ni_cpts != NULL &&
					     j < ni->ni_ncpts; j++) {
						if (i == ni->ni_cpts[j])
							break;
					}

					if (j == ni->ni_ncpts)
						continue;

					if (i != 0)
						lnet_net_lock(i);
					tq->tq_credits_min = tq->tq_credits;
					if (i != 0)
						lnet_net_unlock(i);
				}
			}
		}
		lnet_net_unlock(0);
		*ppos += *lenp;
		return 0;
	}

	LIBCFS_ALLOC(tmpstr, tmpsiz);
	if (tmpstr == NULL)
		return -ENOMEM;

	s = tmpstr; /* points to current position in tmpstr[] */

	if (*ppos == 0) {
		s += snprintf(s, tmpstr + tmpsiz - s,
			      "%-24s %6s %5s %4s %4s %4s %5s %5s %5s\n",
			      "nid", "status", "alive", "refs", "peer",
			      "rtr", "max", "tx", "min");
		LASSERT (tmpstr + tmpsiz - s > 0);
	} else {
		struct lnet_ni *ni   = NULL;
		int skip = *ppos - 1;

		lnet_net_lock(0);

		ni = lnet_get_ni_idx_locked(skip);

		if (ni != NULL) {
			struct lnet_tx_queue	*tq;
			char	*stat;
			long now = cfs_time_current_sec();
			int	last_alive = -1;
			int	i;
			int	j;

			if (the_lnet.ln_routing)
				last_alive = now - ni->ni_last_alive;

			/* @lo forever alive */
			if (ni->ni_net->net_lnd->lnd_type == LOLND)
				last_alive = 0;

			lnet_ni_lock(ni);
			LASSERT(ni->ni_status != NULL);
			stat = (ni->ni_status->ns_status ==
				LNET_NI_STATUS_UP) ? "up" : "down";
			lnet_ni_unlock(ni);

			/* we actually output credits information for
			 * TX queue of each partition */
			cfs_percpt_for_each(tq, i, ni->ni_tx_queues) {
				for (j = 0; ni->ni_cpts != NULL &&
				     j < ni->ni_ncpts; j++) {
					if (i == ni->ni_cpts[j])
						break;
				}

				if (j == ni->ni_ncpts)
					continue;

				if (i != 0)
					lnet_net_lock(i);

				s += snprintf(s, tmpstr + tmpsiz - s,
				      "%-24s %6s %5d %4d %4d %4d %5d %5d %5d\n",
				      libcfs_nid2str(ni->ni_nid), stat,
				      last_alive, *ni->ni_refs[i],
				      ni->ni_net->net_tunables.lct_peer_tx_credits,
				      ni->ni_net->net_tunables.lct_peer_rtr_credits,
				      tq->tq_credits_max,
				      tq->tq_credits, tq->tq_credits_min);
				if (i != 0)
					lnet_net_unlock(i);
			}
			LASSERT(tmpstr + tmpsiz - s > 0);
		}

		lnet_net_unlock(0);
	}

	len = s - tmpstr;     /* how many bytes was written */

	if (len > *lenp) {    /* linux-supplied buffer is too small */
		rc = -EINVAL;
	} else if (len > 0) { /* wrote something */
		if (copy_to_user(buffer, tmpstr, len))
			rc = -EFAULT;
		else
			*ppos += 1;
	}

	LIBCFS_FREE(tmpstr, tmpsiz);

	if (rc == 0)
		*lenp = len;

	return rc;
}

struct lnet_portal_rotors {
	int		pr_value;
	const char	*pr_name;
	const char	*pr_desc;
};

static struct lnet_portal_rotors	portal_rotors[] = {
	{
		.pr_value = LNET_PTL_ROTOR_OFF,
		.pr_name  = "OFF",
		.pr_desc  = "Turn off message rotor for wildcard portals"
	},
	{
		.pr_value = LNET_PTL_ROTOR_ON,
		.pr_name  = "ON",
		.pr_desc  = "round-robin dispatch all PUT messages for "
			    "wildcard portals"
	},
	{
		.pr_value = LNET_PTL_ROTOR_RR_RT,
		.pr_name  = "RR_RT",
		.pr_desc  = "round-robin dispatch routed PUT message for "
			    "wildcard portals"
	},
	{
		.pr_value = LNET_PTL_ROTOR_HASH_RT,
		.pr_name  = "HASH_RT",
		.pr_desc  = "dispatch routed PUT message by hashing source "
			    "NID for wildcard portals"
	},
	{
		.pr_value = -1,
		.pr_name  = NULL,
		.pr_desc  = NULL
	},
};

static int __proc_lnet_portal_rotor(void *data, int write,
				    loff_t pos, void __user *buffer, int nob)
{
	const int	buf_len	= 128;
	char		*buf;
	char		*tmp;
	int		rc;
	int		i;

	LIBCFS_ALLOC(buf, buf_len);
	if (buf == NULL)
		return -ENOMEM;

	if (!write) {
		lnet_res_lock(0);

		for (i = 0; portal_rotors[i].pr_value >= 0; i++) {
			if (portal_rotors[i].pr_value == portal_rotor)
				break;
		}

		LASSERT(portal_rotors[i].pr_value == portal_rotor);
		lnet_res_unlock(0);

		rc = snprintf(buf, buf_len,
			      "{\n\tportals: all\n"
			      "\trotor: %s\n\tdescription: %s\n}",
			      portal_rotors[i].pr_name,
			      portal_rotors[i].pr_desc);

		if (pos >= min_t(int, rc, buf_len)) {
			rc = 0;
		} else {
			rc = cfs_trace_copyout_string(buffer, nob,
					buf + pos, "\n");
		}
		goto out;
	}

	rc = cfs_trace_copyin_string(buf, buf_len, buffer, nob);
	if (rc < 0)
		goto out;

	tmp = cfs_trimwhite(buf);

	rc = -EINVAL;
	lnet_res_lock(0);
	for (i = 0; portal_rotors[i].pr_name != NULL; i++) {
		if (strncasecmp(portal_rotors[i].pr_name, tmp,
				strlen(portal_rotors[i].pr_name)) == 0) {
			portal_rotor = portal_rotors[i].pr_value;
			rc = 0;
			break;
		}
	}
	lnet_res_unlock(0);
out:
	LIBCFS_FREE(buf, buf_len);
	return rc;
}

static int
proc_lnet_portal_rotor(struct ctl_table *table, int write, void __user *buffer,
		       size_t *lenp, loff_t *ppos)
{
	return lprocfs_call_handler(table->data, write, ppos, buffer, lenp,
				    __proc_lnet_portal_rotor);
}


static struct ctl_table lnet_table[] = {
	/*
	 * NB No .strategy entries have been provided since sysctl(8) prefers
	 * to go via /proc for portability.
	 */
	{
		INIT_CTL_NAME
		.procname	= "stats",
		.mode		= 0644,
		.proc_handler	= &proc_lnet_stats,
	},
	{
		INIT_CTL_NAME
		.procname	= "routes",
		.mode		= 0444,
		.proc_handler	= &proc_lnet_routes,
	},
	{
		INIT_CTL_NAME
		.procname	= "routers",
		.mode		= 0444,
		.proc_handler	= &proc_lnet_routers,
	},
	{
		INIT_CTL_NAME
		.procname	= "peers",
		.mode		= 0644,
		.proc_handler	= &proc_lnet_peers,
	},
	{
		INIT_CTL_NAME
		.procname	= "buffers",
		.mode		= 0444,
		.proc_handler	= &proc_lnet_buffers,
	},
	{
		INIT_CTL_NAME
		.procname	= "nis",
		.mode		= 0644,
		.proc_handler	= &proc_lnet_nis,
	},
	{
		INIT_CTL_NAME
		.procname	= "portal_rotor",
		.mode		= 0644,
		.proc_handler	= &proc_lnet_portal_rotor,
	},
	{ .procname = NULL }
};

static struct ctl_table top_table[] = {
	{
		INIT_CTL_NAME
		.procname	= "lnet",
		.mode		= 0555,
		.data		= NULL,
		.maxlen		= 0,
		.child		= lnet_table,
	},
	{ .procname = NULL }
};

void
lnet_proc_init(void)
{
#ifdef CONFIG_SYSCTL
	if (lnet_table_header == NULL)
		lnet_table_header = register_sysctl_table(top_table);
#endif
}

void
lnet_proc_fini(void)
{
#ifdef CONFIG_SYSCTL
	if (lnet_table_header != NULL)
		unregister_sysctl_table(lnet_table_header);

	lnet_table_header = NULL;
#endif
}
