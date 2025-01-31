/*
 * GPL HEADER START
 *
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 only,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License version 2 for more details (a copy is included
 * in the LICENSE file that accompanied this code).
 *
 * You should have received a copy of the GNU General Public License
 * version 2 along with this program; If not, see
 * http://www.gnu.org/licenses/gpl-2.0.html
 *
 * GPL HEADER END
 */
/*
 * Copyright (c) 2007, 2010, Oracle and/or its affiliates. All rights reserved.
 * Use is subject to license terms.
 *
 * Copyright (c) 2011, 2016, Intel Corporation.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 *
 * lustre/fid/fid_request.c
 *
 * Lustre Sequence Manager
 *
 * Author: Yury Umanets <umka@clusterfs.com>
 */

#define DEBUG_SUBSYSTEM S_FID

#include <linux/module.h>
#include <libcfs/libcfs.h>
#include <obd.h>
#include <obd_class.h>
#include <obd_support.h>
#include <lustre_fid.h>
/* mdc RPC locks */
#include <lustre_mdc.h>
#include "fid_internal.h"

static int seq_client_rpc(struct lu_client_seq *seq,
                          struct lu_seq_range *output, __u32 opc,
                          const char *opcname)
{
	struct obd_export     *exp = seq->lcs_exp;
	struct ptlrpc_request *req;
	struct lu_seq_range   *out, *in;
	__u32                 *op;
	unsigned int           debug_mask;
	int                    rc;
	ENTRY;

	LASSERT(exp != NULL && !IS_ERR(exp));
	req = ptlrpc_request_alloc_pack(class_exp2cliimp(exp), &RQF_SEQ_QUERY,
					LUSTRE_MDS_VERSION, SEQ_QUERY);
	if (req == NULL)
		RETURN(-ENOMEM);

	/* Init operation code */
	op = req_capsule_client_get(&req->rq_pill, &RMF_SEQ_OPC);
	*op = opc;

	/* Zero out input range, this is not recovery yet. */
	in = req_capsule_client_get(&req->rq_pill, &RMF_SEQ_RANGE);
	lu_seq_range_init(in);

	ptlrpc_request_set_replen(req);

	in->lsr_index = seq->lcs_space.lsr_index;
	if (seq->lcs_type == LUSTRE_SEQ_METADATA)
		fld_range_set_mdt(in);
	else
		fld_range_set_ost(in);

	if (opc == SEQ_ALLOC_SUPER) {
		req->rq_request_portal = SEQ_CONTROLLER_PORTAL;
		req->rq_reply_portal = MDC_REPLY_PORTAL;
		/* During allocating super sequence for data object,
		 * the current thread might hold the export of MDT0(MDT0
		 * precreating objects on this OST), and it will send the
		 * request to MDT0 here, so we can not keep resending the
		 * request here, otherwise if MDT0 is failed(umounted),
		 * it can not release the export of MDT0 */
		if (seq->lcs_type == LUSTRE_SEQ_DATA)
			req->rq_no_delay = req->rq_no_resend = 1;
		debug_mask = D_CONSOLE;
	} else {
		if (seq->lcs_type == LUSTRE_SEQ_METADATA) {
			req->rq_reply_portal = MDC_REPLY_PORTAL;
			req->rq_request_portal = SEQ_METADATA_PORTAL;
		} else {
			req->rq_reply_portal = OSC_REPLY_PORTAL;
			req->rq_request_portal = SEQ_DATA_PORTAL;
		}

		debug_mask = D_INFO;
	}

	/* Allow seq client RPC during recovery time. */
	req->rq_allow_replay = 1;

	ptlrpc_at_set_req_timeout(req);

	rc = ptlrpc_queue_wait(req);

	if (rc)
		GOTO(out_req, rc);

	out = req_capsule_server_get(&req->rq_pill, &RMF_SEQ_RANGE);
	*output = *out;

	if (!lu_seq_range_is_sane(output)) {
		CERROR("%s: Invalid range received from server: "
		       DRANGE"\n", seq->lcs_name, PRANGE(output));
		GOTO(out_req, rc = -EINVAL);
	}

	if (lu_seq_range_is_exhausted(output)) {
		CERROR("%s: Range received from server is exhausted: "
		       DRANGE"]\n", seq->lcs_name, PRANGE(output));
		GOTO(out_req, rc = -EINVAL);
	}

	CDEBUG_LIMIT(debug_mask, "%s: Allocated %s-sequence "DRANGE"]\n",
		     seq->lcs_name, opcname, PRANGE(output));

	EXIT;
out_req:
	ptlrpc_req_finished(req);
	return rc;
}

/* Request sequence-controller node to allocate new super-sequence. */
int seq_client_alloc_super(struct lu_client_seq *seq,
                           const struct lu_env *env)
{
        int rc;
        ENTRY;

	mutex_lock(&seq->lcs_mutex);

        if (seq->lcs_srv) {
#ifdef HAVE_SEQ_SERVER
                LASSERT(env != NULL);
                rc = seq_server_alloc_super(seq->lcs_srv, &seq->lcs_space,
                                            env);
#else
		rc = 0;
#endif
	} else {
		/* Check whether the connection to seq controller has been
		 * setup (lcs_exp != NULL) */
		if (seq->lcs_exp == NULL) {
			mutex_unlock(&seq->lcs_mutex);
			RETURN(-EINPROGRESS);
		}

		rc = seq_client_rpc(seq, &seq->lcs_space,
                                    SEQ_ALLOC_SUPER, "super");
        }
	mutex_unlock(&seq->lcs_mutex);
        RETURN(rc);
}

/* Request sequence-controller node to allocate new meta-sequence. */
static int seq_client_alloc_meta(const struct lu_env *env,
                                 struct lu_client_seq *seq)
{
        int rc;
        ENTRY;

        if (seq->lcs_srv) {
#ifdef HAVE_SEQ_SERVER
                LASSERT(env != NULL);
                rc = seq_server_alloc_meta(seq->lcs_srv, &seq->lcs_space, env);
#else
		rc = 0;
#endif
	} else {
		do {
			/* If meta server return -EINPROGRESS or EAGAIN,
			 * it means meta server might not be ready to
			 * allocate super sequence from sequence controller
			 * (MDT0)yet */
			rc = seq_client_rpc(seq, &seq->lcs_space,
					    SEQ_ALLOC_META, "meta");
			if (rc == -EINPROGRESS || rc == -EAGAIN) {
				wait_queue_head_t waitq;
				struct l_wait_info  lwi;

				/* MDT0 is not ready, let's wait for 2
				 * seconds and retry. */
				init_waitqueue_head(&waitq);
				lwi = LWI_TIMEOUT(cfs_time_seconds(2), NULL,
						  NULL);
				l_wait_event(waitq, 0, &lwi);
			}
		} while (rc == -EINPROGRESS || rc == -EAGAIN);
        }

        RETURN(rc);
}

/* Allocate new sequence for client. */
static int seq_client_alloc_seq(const struct lu_env *env,
				struct lu_client_seq *seq, u64 *seqnr)
{
	int rc;
	ENTRY;

	LASSERT(lu_seq_range_is_sane(&seq->lcs_space));

	if (lu_seq_range_is_exhausted(&seq->lcs_space)) {
                rc = seq_client_alloc_meta(env, seq);
                if (rc) {
			if (rc != -EINPROGRESS)
				CERROR("%s: Can't allocate new meta-sequence,"
				       "rc = %d\n", seq->lcs_name, rc);
                        RETURN(rc);
                } else {
                        CDEBUG(D_INFO, "%s: New range - "DRANGE"\n",
                               seq->lcs_name, PRANGE(&seq->lcs_space));
                }
        } else {
                rc = 0;
        }

	LASSERT(!lu_seq_range_is_exhausted(&seq->lcs_space));
	*seqnr = seq->lcs_space.lsr_start;
	seq->lcs_space.lsr_start += 1;

	CDEBUG(D_INFO, "%s: Allocated sequence [%#llx]\n", seq->lcs_name,
               *seqnr);

        RETURN(rc);
}

static int seq_fid_alloc_prep(struct lu_client_seq *seq,
			      wait_queue_entry_t *link)
{
	if (seq->lcs_update) {
		add_wait_queue(&seq->lcs_waitq, link);
		set_current_state(TASK_UNINTERRUPTIBLE);
		mutex_unlock(&seq->lcs_mutex);

		schedule();

		mutex_lock(&seq->lcs_mutex);
		remove_wait_queue(&seq->lcs_waitq, link);
		set_current_state(TASK_RUNNING);
		return -EAGAIN;
	}

	++seq->lcs_update;
	mutex_unlock(&seq->lcs_mutex);

	return 0;
}

static void seq_fid_alloc_fini(struct lu_client_seq *seq, __u64 seqnr,
			       bool whole)
{
	LASSERT(seq->lcs_update == 1);

	mutex_lock(&seq->lcs_mutex);
	if (seqnr != 0) {
		CDEBUG(D_INFO, "%s: New sequence [0x%16.16llx]\n",
		       seq->lcs_name, seqnr);

		seq->lcs_fid.f_seq = seqnr;
		if (whole) {
			/* Since the caller require the whole seq,
			 * so marked this seq to be used */
			if (seq->lcs_type == LUSTRE_SEQ_METADATA)
				seq->lcs_fid.f_oid =
					LUSTRE_METADATA_SEQ_MAX_WIDTH;
			else
				seq->lcs_fid.f_oid = LUSTRE_DATA_SEQ_MAX_WIDTH;
		} else {
			seq->lcs_fid.f_oid = LUSTRE_FID_INIT_OID;
		}
		seq->lcs_fid.f_ver = 0;
	}

	--seq->lcs_update;
	wake_up_all(&seq->lcs_waitq);
}

/**
 * Allocate the whole non-used seq to the caller.
 *
 * \param[in] env	pointer to the thread context
 * \param[in,out] seq	pointer to the client sequence manager
 * \param[out] seqnr	to hold the new allocated sequence
 *
 * \retval		0 for new sequence allocated.
 * \retval		Negative error number on failure.
 */
int seq_client_get_seq(const struct lu_env *env,
		       struct lu_client_seq *seq, u64 *seqnr)
{
	wait_queue_entry_t link;
	int rc;

	LASSERT(seqnr != NULL);

	mutex_lock(&seq->lcs_mutex);
	init_waitqueue_entry(&link, current);

	/* To guarantee that we can get a whole non-used sequence. */
	while (seq_fid_alloc_prep(seq, &link) != 0);

	rc = seq_client_alloc_seq(env, seq, seqnr);
	seq_fid_alloc_fini(seq, rc ? 0 : *seqnr, true);
	if (rc)
		CERROR("%s: Can't allocate new sequence: rc = %d\n",
		       seq->lcs_name, rc);
	mutex_unlock(&seq->lcs_mutex);

	return rc;
}
EXPORT_SYMBOL(seq_client_get_seq);

/**
 * Allocate new fid on passed client @seq and save it to @fid.
 *
 * \param[in] env	pointer to the thread context
 * \param[in,out] seq	pointer to the client sequence manager
 * \param[out] fid	to hold the new allocated fid
 *
 * \retval		1 for notify the caller that sequence switch
 *			is performed to allow it to setup FLD for it.
 * \retval		0 for new FID allocated in current sequence.
 * \retval		Negative error number on failure.
 */
int seq_client_alloc_fid(const struct lu_env *env,
			 struct lu_client_seq *seq, struct lu_fid *fid)
{
	wait_queue_entry_t link;
	int rc;
	ENTRY;

	LASSERT(seq != NULL);
	LASSERT(fid != NULL);

	init_waitqueue_entry(&link, current);
	mutex_lock(&seq->lcs_mutex);

	if (OBD_FAIL_CHECK(OBD_FAIL_SEQ_EXHAUST))
		seq->lcs_fid.f_oid = seq->lcs_width;

	while (1) {
		u64 seqnr;

		if (unlikely(!fid_is_zero(&seq->lcs_fid) &&
			     fid_oid(&seq->lcs_fid) < seq->lcs_width)) {
			/* Just bump last allocated fid and return to caller. */
			seq->lcs_fid.f_oid++;
			rc = 0;
			break;
		}

		/* Release seq::lcs_mutex via seq_fid_alloc_prep() to avoid
		 * deadlock during seq_client_alloc_seq(). */
		rc = seq_fid_alloc_prep(seq, &link);
		if (rc)
			continue;

		rc = seq_client_alloc_seq(env, seq, &seqnr);
		/* Re-take seq::lcs_mutex via seq_fid_alloc_fini(). */
		seq_fid_alloc_fini(seq, rc ? 0 : seqnr, false);
		if (rc) {
			if (rc != -EINPROGRESS)
				CERROR("%s: Can't allocate new sequence: "
				       "rc = %d\n", seq->lcs_name, rc);
			mutex_unlock(&seq->lcs_mutex);

			RETURN(rc);
		}

		rc = 1;
		break;
	}

	*fid = seq->lcs_fid;
	mutex_unlock(&seq->lcs_mutex);

	CDEBUG(D_INFO, "%s: Allocated FID "DFID"\n", seq->lcs_name,  PFID(fid));

	RETURN(rc);
}
EXPORT_SYMBOL(seq_client_alloc_fid);

/*
 * Finish the current sequence due to disconnect.
 * See mdc_import_event()
 */
void seq_client_flush(struct lu_client_seq *seq)
{
	wait_queue_entry_t link;

	LASSERT(seq != NULL);
	init_waitqueue_entry(&link, current);
	mutex_lock(&seq->lcs_mutex);

	while (seq->lcs_update) {
		add_wait_queue(&seq->lcs_waitq, &link);
		set_current_state(TASK_UNINTERRUPTIBLE);
		mutex_unlock(&seq->lcs_mutex);

		schedule();

		mutex_lock(&seq->lcs_mutex);
		remove_wait_queue(&seq->lcs_waitq, &link);
		set_current_state(TASK_RUNNING);
	}

        fid_zero(&seq->lcs_fid);
        /**
         * this id shld not be used for seq range allocation.
         * set to -1 for dgb check.
         */

        seq->lcs_space.lsr_index = -1;

	lu_seq_range_init(&seq->lcs_space);
	mutex_unlock(&seq->lcs_mutex);
}
EXPORT_SYMBOL(seq_client_flush);

static void seq_client_proc_fini(struct lu_client_seq *seq)
{
#ifdef CONFIG_PROC_FS
	ENTRY;
	if (seq->lcs_proc_dir) {
		if (!IS_ERR(seq->lcs_proc_dir))
			lprocfs_remove(&seq->lcs_proc_dir);
		seq->lcs_proc_dir = NULL;
	}
	EXIT;
#endif /* CONFIG_PROC_FS */
}

static int seq_client_proc_init(struct lu_client_seq *seq)
{
#ifdef CONFIG_PROC_FS
        int rc;
        ENTRY;

	seq->lcs_proc_dir = lprocfs_register(seq->lcs_name, seq_type_proc_dir,
					     NULL, NULL);
        if (IS_ERR(seq->lcs_proc_dir)) {
                CERROR("%s: LProcFS failed in seq-init\n",
                       seq->lcs_name);
                rc = PTR_ERR(seq->lcs_proc_dir);
                RETURN(rc);
        }

	rc = lprocfs_add_vars(seq->lcs_proc_dir, seq_client_proc_list, seq);
        if (rc) {
                CERROR("%s: Can't init sequence manager "
                       "proc, rc %d\n", seq->lcs_name, rc);
                GOTO(out_cleanup, rc);
        }

        RETURN(0);

out_cleanup:
        seq_client_proc_fini(seq);
        return rc;

#else /* !CONFIG_PROC_FS */
	return 0;
#endif /* CONFIG_PROC_FS */
}

int seq_client_init(struct lu_client_seq *seq,
                    struct obd_export *exp,
                    enum lu_cli_type type,
                    const char *prefix,
                    struct lu_server_seq *srv)
{
	int rc;
	ENTRY;

	LASSERT(seq != NULL);
	LASSERT(prefix != NULL);

	seq->lcs_srv = srv;
	seq->lcs_type = type;

	mutex_init(&seq->lcs_mutex);
	if (type == LUSTRE_SEQ_METADATA)
		seq->lcs_width = LUSTRE_METADATA_SEQ_MAX_WIDTH;
	else
		seq->lcs_width = LUSTRE_DATA_SEQ_MAX_WIDTH;

	init_waitqueue_head(&seq->lcs_waitq);
	/* Make sure that things are clear before work is started. */
	seq_client_flush(seq);

	if (exp != NULL)
		seq->lcs_exp = class_export_get(exp);

	snprintf(seq->lcs_name, sizeof(seq->lcs_name),
		 "cli-%s", prefix);

	rc = seq_client_proc_init(seq);
	if (rc)
		seq_client_fini(seq);
	RETURN(rc);
}
EXPORT_SYMBOL(seq_client_init);

void seq_client_fini(struct lu_client_seq *seq)
{
        ENTRY;

        seq_client_proc_fini(seq);

        if (seq->lcs_exp != NULL) {
                class_export_put(seq->lcs_exp);
                seq->lcs_exp = NULL;
        }

        seq->lcs_srv = NULL;
        EXIT;
}
EXPORT_SYMBOL(seq_client_fini);

int client_fid_init(struct obd_device *obd,
		    struct obd_export *exp, enum lu_cli_type type)
{
	struct client_obd *cli = &obd->u.cli;
	char *prefix;
	int rc;
	ENTRY;

	down_write(&cli->cl_seq_rwsem);
	OBD_ALLOC_PTR(cli->cl_seq);
	if (!cli->cl_seq)
		GOTO(out, rc = -ENOMEM);

	OBD_ALLOC(prefix, MAX_OBD_NAME + 5);
	if (!prefix)
		GOTO(out, rc = -ENOMEM);

	snprintf(prefix, MAX_OBD_NAME + 5, "cli-%s", obd->obd_name);

	/* Init client side sequence-manager */
	rc = seq_client_init(cli->cl_seq, exp, type, prefix, NULL);
	OBD_FREE(prefix, MAX_OBD_NAME + 5);

	GOTO(out, rc);

out:
	if (rc && cli->cl_seq) {
		OBD_FREE_PTR(cli->cl_seq);
		cli->cl_seq = NULL;
	}
	up_write(&cli->cl_seq_rwsem);

	return rc;
}
EXPORT_SYMBOL(client_fid_init);

int client_fid_fini(struct obd_device *obd)
{
	struct client_obd *cli = &obd->u.cli;
	ENTRY;

	down_write(&cli->cl_seq_rwsem);
	if (cli->cl_seq) {
		seq_client_fini(cli->cl_seq);
		OBD_FREE_PTR(cli->cl_seq);
		cli->cl_seq = NULL;
	}
	up_write(&cli->cl_seq_rwsem);

	RETURN(0);
}
EXPORT_SYMBOL(client_fid_fini);

struct proc_dir_entry *seq_type_proc_dir;

static int __init fid_init(void)
{
	seq_type_proc_dir = lprocfs_register(LUSTRE_SEQ_NAME,
					     proc_lustre_root,
					     NULL, NULL);
	if (IS_ERR(seq_type_proc_dir))
		return PTR_ERR(seq_type_proc_dir);

# ifdef HAVE_SERVER_SUPPORT
	fid_server_mod_init();
# endif

	return 0;
}

static void __exit fid_exit(void)
{
# ifdef HAVE_SERVER_SUPPORT
	fid_server_mod_exit();
# endif

	if (seq_type_proc_dir != NULL && !IS_ERR(seq_type_proc_dir)) {
		lprocfs_remove(&seq_type_proc_dir);
		seq_type_proc_dir = NULL;
	}
}

MODULE_AUTHOR("OpenSFS, Inc. <http://www.lustre.org/>");
MODULE_DESCRIPTION("Lustre File IDentifier");
MODULE_VERSION(LUSTRE_VERSION_STRING);
MODULE_LICENSE("GPL");

module_init(fid_init);
module_exit(fid_exit);
