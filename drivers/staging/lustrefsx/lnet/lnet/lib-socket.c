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
 * Copyright (c) 2008, 2010, Oracle and/or its affiliates. All rights reserved.
 * Use is subject to license terms.
 *
 * Copyright (c) 2015, 2016, Intel Corporation.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 */
#define DEBUG_SUBSYSTEM S_LNET

#include <linux/if.h>
#include <linux/in.h>
#include <linux/net.h>
#include <linux/file.h>
#include <linux/pagemap.h>
/* For sys_open & sys_close */
#include <linux/syscalls.h>
#include <net/sock.h>

#include <libcfs/libcfs.h>
#include <lnet/lib-lnet.h>

static int
kernel_sock_unlocked_ioctl(struct file *filp, int cmd, unsigned long arg)
{
	mm_segment_t oldfs = get_fs();
	int err;

	set_fs(KERNEL_DS);
	err = filp->f_op->unlocked_ioctl(filp, cmd, arg);
	set_fs(oldfs);

	return err;
}

static int
lnet_sock_ioctl(int cmd, unsigned long arg)
{
	struct file    *sock_filp;
	struct socket  *sock;
	int		fd = -1;
	int		rc;

#ifdef HAVE_SOCK_CREATE_KERN_USE_NET
	rc = sock_create_kern(&init_net, PF_INET, SOCK_STREAM, 0, &sock);
#else
	rc = sock_create_kern(PF_INET, SOCK_STREAM, 0, &sock);
#endif
	if (rc != 0) {
		CERROR("Can't create socket: %d\n", rc);
		return rc;
	}

#if !defined(HAVE_SOCK_ALLOC_FILE) && !defined(HAVE_SOCK_ALLOC_FILE_3ARGS)
	fd = sock_map_fd(sock, 0);
	if (fd < 0) {
		rc = fd;
		sock_release(sock);
		goto out;
	}
	sock_filp = fget(fd);
#else
# ifdef HAVE_SOCK_ALLOC_FILE_3ARGS
	sock_filp = sock_alloc_file(sock, 0, NULL);
# else
	sock_filp = sock_alloc_file(sock, 0);
# endif
#endif
	if (IS_ERR(sock_filp)) {
		rc = PTR_ERR(sock_filp);
		sock_release(sock);
		goto out;
	}

	rc = kernel_sock_unlocked_ioctl(sock_filp, cmd, arg);

	fput(sock_filp);
out:
	if (fd >= 0)
#ifdef HAVE_KSYS_CLOSE
		ksys_close(fd);
#else
		sys_close(fd);
#endif
	return rc;
}

int
lnet_ipif_query(char *name, int *up, __u32 *ip, __u32 *mask)
{
	struct ifreq	ifr;
	int		nob;
	int		rc;
	__u32		val;

	nob = strnlen(name, IFNAMSIZ);
	if (nob == IFNAMSIZ) {
		CERROR("Interface name %s too long\n", name);
		return -EINVAL;
	}

	CLASSERT(sizeof(ifr.ifr_name) >= IFNAMSIZ);

	if (strlen(name) > sizeof(ifr.ifr_name)-1)
		return -E2BIG;
	strncpy(ifr.ifr_name, name, sizeof(ifr.ifr_name));

	rc = lnet_sock_ioctl(SIOCGIFFLAGS, (unsigned long)&ifr);
	if (rc != 0) {
		CERROR("Can't get flags for interface %s\n", name);
		return rc;
	}

	if ((ifr.ifr_flags & IFF_UP) == 0) {
		CDEBUG(D_NET, "Interface %s down\n", name);
		*up = 0;
		*ip = *mask = 0;
		return 0;
	}
	*up = 1;

	if (strlen(name) > sizeof(ifr.ifr_name)-1)
		return -E2BIG;
	strncpy(ifr.ifr_name, name, sizeof(ifr.ifr_name));

	ifr.ifr_addr.sa_family = AF_INET;
	rc = lnet_sock_ioctl(SIOCGIFADDR, (unsigned long)&ifr);

	if (rc != 0) {
		CERROR("Can't get IP address for interface %s\n", name);
		return rc;
	}

	val = ((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr.s_addr;
	*ip = ntohl(val);

	if (strlen(name) > sizeof(ifr.ifr_name)-1)
		return -E2BIG;
	strncpy(ifr.ifr_name, name, sizeof(ifr.ifr_name));

	ifr.ifr_addr.sa_family = AF_INET;
	rc = lnet_sock_ioctl(SIOCGIFNETMASK, (unsigned long)&ifr);
	if (rc != 0) {
		CERROR("Can't get netmask for interface %s\n", name);
		return rc;
	}

	val = ((struct sockaddr_in *)&ifr.ifr_netmask)->sin_addr.s_addr;
	*mask = ntohl(val);

	return 0;
}
EXPORT_SYMBOL(lnet_ipif_query);

void
lnet_ipif_free_enumeration(char **names, int n)
{
	int	i;

	LASSERT(n > 0);

	for (i = 0; i < n && names[i] != NULL; i++)
		LIBCFS_FREE(names[i], IFNAMSIZ);

	LIBCFS_FREE(names, n * sizeof(*names));
}
EXPORT_SYMBOL(lnet_ipif_free_enumeration);

int
lnet_ipif_enumerate(char ***namesp)
{
	/* Allocate and fill in 'names', returning # interfaces/error */
	char	      **names;
	int		toobig;
	int		nalloc;
	int		nfound;
	struct ifreq   *ifr;
	struct ifconf	ifc;
	int		rc;
	int		nob;
	int		i;

	nalloc = 16;	/* first guess at max interfaces */
	toobig = 0;
	for (;;) {
		if (nalloc * sizeof(*ifr) > PAGE_SIZE) {
			toobig = 1;
			nalloc = PAGE_SIZE / sizeof(*ifr);
			CWARN("Too many interfaces: only enumerating "
			      "first %d\n", nalloc);
		}

		LIBCFS_ALLOC(ifr, nalloc * sizeof(*ifr));
		if (ifr == NULL) {
			CERROR("ENOMEM enumerating up to %d interfaces\n",
			       nalloc);
			rc = -ENOMEM;
			goto out0;
		}

		ifc.ifc_buf = (char *)ifr;
		ifc.ifc_len = nalloc * sizeof(*ifr);

		rc = lnet_sock_ioctl(SIOCGIFCONF, (unsigned long)&ifc);
		if (rc < 0) {
			CERROR("Error %d enumerating interfaces\n", rc);
			goto out1;
		}

		LASSERT(rc == 0);

		nfound = ifc.ifc_len/sizeof(*ifr);
		LASSERT(nfound <= nalloc);

		if (nfound < nalloc || toobig)
			break;

		LIBCFS_FREE(ifr, nalloc * sizeof(*ifr));
		nalloc *= 2;
	}

	if (nfound == 0)
		goto out1;

	LIBCFS_ALLOC(names, nfound * sizeof(*names));
	if (names == NULL) {
		rc = -ENOMEM;
		goto out1;
	}

	for (i = 0; i < nfound; i++) {
		nob = strnlen(ifr[i].ifr_name, IFNAMSIZ);
		if (nob == IFNAMSIZ) {
			/* no space for terminating NULL */
			CERROR("interface name %.*s too long (%d max)\n",
			       nob, ifr[i].ifr_name, IFNAMSIZ);
			rc = -ENAMETOOLONG;
			goto out2;
		}

		LIBCFS_ALLOC(names[i], IFNAMSIZ);
		if (names[i] == NULL) {
			rc = -ENOMEM;
			goto out2;
		}

		memcpy(names[i], ifr[i].ifr_name, nob);
		names[i][nob] = 0;
	}

	*namesp = names;
	rc = nfound;

 out2:
	if (rc < 0)
		lnet_ipif_free_enumeration(names, nfound);
 out1:
	LIBCFS_FREE(ifr, nalloc * sizeof(*ifr));
 out0:
	return rc;
}
EXPORT_SYMBOL(lnet_ipif_enumerate);

int
lnet_sock_write(struct socket *sock, void *buffer, int nob, int timeout)
{
	int		rc;
	long		jiffies_left = timeout * msecs_to_jiffies(MSEC_PER_SEC);
	unsigned long	then;
	struct timeval	tv;

	LASSERT(nob > 0);
	/* Caller may pass a zero timeout if she thinks the socket buffer is
	 * empty enough to take the whole message immediately */

	for (;;) {
		struct kvec  iov = {
			.iov_base = buffer,
			.iov_len  = nob
		};
		struct msghdr msg = {
			.msg_flags	= (timeout == 0) ? MSG_DONTWAIT : 0
		};

		if (timeout != 0) {
			/* Set send timeout to remaining time */
			tv = (struct timeval) {
				.tv_sec = jiffies_left /
					  msecs_to_jiffies(MSEC_PER_SEC),
				.tv_usec = ((jiffies_left %
					     msecs_to_jiffies(MSEC_PER_SEC)) *
					     USEC_PER_SEC) /
					     msecs_to_jiffies(MSEC_PER_SEC)
			};

			rc = kernel_setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO,
					       (char *)&tv, sizeof(tv));
			if (rc != 0) {
				CERROR("Can't set socket send timeout "
				       "%ld.%06d: %d\n",
				       (long)tv.tv_sec, (int)tv.tv_usec, rc);
				return rc;
			}
		}

		then = jiffies;
		rc = kernel_sendmsg(sock, &msg, &iov, 1, nob);
		jiffies_left -= jiffies - then;

		if (rc == nob)
			return 0;

		if (rc < 0)
			return rc;

		if (rc == 0) {
			CERROR("Unexpected zero rc\n");
			return -ECONNABORTED;
		}

		if (jiffies_left <= 0)
			return -EAGAIN;

		buffer = ((char *)buffer) + rc;
		nob -= rc;
	}
	return 0;
}
EXPORT_SYMBOL(lnet_sock_write);

int
lnet_sock_read(struct socket *sock, void *buffer, int nob, int timeout)
{
	int		rc;
	long		jiffies_left = timeout * msecs_to_jiffies(MSEC_PER_SEC);
	unsigned long	then;
	struct timeval	tv;

	LASSERT(nob > 0);
	LASSERT(jiffies_left > 0);

	for (;;) {
		struct kvec  iov = {
			.iov_base = buffer,
			.iov_len  = nob
		};
		struct msghdr msg = {
			.msg_flags	= 0
		};

		/* Set receive timeout to remaining time */
		tv = (struct timeval) {
			.tv_sec = jiffies_left / msecs_to_jiffies(MSEC_PER_SEC),
			.tv_usec = ((jiffies_left %
					msecs_to_jiffies(MSEC_PER_SEC)) *
					USEC_PER_SEC) /
					msecs_to_jiffies(MSEC_PER_SEC)
		};
		rc = kernel_setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
				       (char *)&tv, sizeof(tv));
		if (rc != 0) {
			CERROR("Can't set socket recv timeout %ld.%06d: %d\n",
			       (long)tv.tv_sec, (int)tv.tv_usec, rc);
			return rc;
		}

		then = jiffies;
		rc = kernel_recvmsg(sock, &msg, &iov, 1, nob, 0);
		jiffies_left -= jiffies - then;

		if (rc < 0)
			return rc;

		if (rc == 0)
			return -ECONNRESET;

		buffer = ((char *)buffer) + rc;
		nob -= rc;

		if (nob == 0)
			return 0;

		if (jiffies_left <= 0)
			return -ETIMEDOUT;
	}
}
EXPORT_SYMBOL(lnet_sock_read);

static int
lnet_sock_create(struct socket **sockp, int *fatal,
		 __u32 local_ip, int local_port)
{
	struct sockaddr_in  locaddr;
	struct socket	   *sock;
	int		    rc;
	int		    option;

	/* All errors are fatal except bind failure if the port is in use */
	*fatal = 1;

#ifdef HAVE_SOCK_CREATE_KERN_USE_NET
	rc = sock_create_kern(&init_net, PF_INET, SOCK_STREAM, 0, &sock);
#else
	rc = sock_create_kern(PF_INET, SOCK_STREAM, 0, &sock);
#endif
	*sockp = sock;
	if (rc != 0) {
		CERROR("Can't create socket: %d\n", rc);
		return rc;
	}

	option = 1;
	rc = kernel_setsockopt(sock, SOL_SOCKET, SO_REUSEADDR,
			       (char *)&option, sizeof(option));
	if (rc != 0) {
		CERROR("Can't set SO_REUSEADDR for socket: %d\n", rc);
		goto failed;
	}

	if (local_ip != 0 || local_port != 0) {
		memset(&locaddr, 0, sizeof(locaddr));
		locaddr.sin_family = AF_INET;
		locaddr.sin_port = htons(local_port);
		locaddr.sin_addr.s_addr = (local_ip == 0) ?
					  INADDR_ANY : htonl(local_ip);

		rc = kernel_bind(sock, (struct sockaddr *)&locaddr,
				 sizeof(locaddr));
		if (rc == -EADDRINUSE) {
			CDEBUG(D_NET, "Port %d already in use\n", local_port);
			*fatal = 0;
			goto failed;
		}
		if (rc != 0) {
			CERROR("Error trying to bind to port %d: %d\n",
			       local_port, rc);
			goto failed;
		}
	}
	return 0;

failed:
	sock_release(sock);
	return rc;
}

int
lnet_sock_setbuf(struct socket *sock, int txbufsize, int rxbufsize)
{
	int		    option;
	int		    rc;

	if (txbufsize != 0) {
		option = txbufsize;
		rc = kernel_setsockopt(sock, SOL_SOCKET, SO_SNDBUF,
				       (char *)&option, sizeof(option));
		if (rc != 0) {
			CERROR("Can't set send buffer %d: %d\n",
				option, rc);
			return rc;
		}
	}

	if (rxbufsize != 0) {
		option = rxbufsize;
		rc = kernel_setsockopt(sock, SOL_SOCKET, SO_RCVBUF,
				       (char *)&option, sizeof(option));
		if (rc != 0) {
			CERROR("Can't set receive buffer %d: %d\n",
				option, rc);
			return rc;
		}
	}
	return 0;
}
EXPORT_SYMBOL(lnet_sock_setbuf);

int
lnet_sock_getaddr(struct socket *sock, bool remote, __u32 *ip, int *port)
{
	struct sockaddr_in sin;
	int		   rc;

#ifdef HAVE_KERNSOCK_RETURNSLEN
	if (remote)
		rc = kernel_getpeername(sock, (struct sockaddr *)&sin);
	else
		rc = kernel_getsockname(sock, (struct sockaddr *)&sin);
	if (rc < 0) {
#else
	int		   len = sizeof(sin);

	if (remote)
		rc = kernel_getpeername(sock, (struct sockaddr *)&sin, &len);
	else
		rc = kernel_getsockname(sock, (struct sockaddr *)&sin, &len);
	if (rc != 0) {
#endif
		CERROR("Error %d getting sock %s IP/port\n",
			rc, remote ? "peer" : "local");
		return rc;
	}

	if (ip != NULL)
		*ip = ntohl(sin.sin_addr.s_addr);

	if (port != NULL)
		*port = ntohs(sin.sin_port);

	return 0;
}
EXPORT_SYMBOL(lnet_sock_getaddr);

int
lnet_sock_getbuf(struct socket *sock, int *txbufsize, int *rxbufsize)
{
	if (txbufsize != NULL)
		*txbufsize = sock->sk->sk_sndbuf;

	if (rxbufsize != NULL)
		*rxbufsize = sock->sk->sk_rcvbuf;

	return 0;
}
EXPORT_SYMBOL(lnet_sock_getbuf);

int
lnet_sock_listen(struct socket **sockp,
		   __u32 local_ip, int local_port, int backlog)
{
	int	 fatal;
	int	 rc;

	rc = lnet_sock_create(sockp, &fatal, local_ip, local_port);
	if (rc != 0) {
		if (!fatal)
			CERROR("Can't create socket: port %d already in use\n",
			       local_port);
		return rc;
	}

	rc = kernel_listen(*sockp, backlog);
	if (rc == 0)
		return 0;

	CERROR("Can't set listen backlog %d: %d\n", backlog, rc);
	sock_release(*sockp);
	return rc;
}

#ifndef HAVE_SK_SLEEP
static inline wait_queue_head_t *sk_sleep(struct sock *sk)
{
	return sk->sk_sleep;
}
#endif

int
lnet_sock_accept(struct socket **newsockp, struct socket *sock)
{
	wait_queue_entry_t wait;
	struct socket *newsock;
	int	       rc;

	/* XXX this should add a ref to sock->ops->owner, if
	 * TCP could be a module */
	rc = sock_create_lite(PF_PACKET, sock->type, IPPROTO_TCP, &newsock);
	if (rc) {
		CERROR("Can't allocate socket\n");
		return rc;
	}

	newsock->ops = sock->ops;

#ifdef HAVE_KERN_SOCK_ACCEPT_FLAG_ARG
	rc = sock->ops->accept(sock, newsock, O_NONBLOCK, false);
#else
	rc = sock->ops->accept(sock, newsock, O_NONBLOCK);
#endif
	if (rc == -EAGAIN) {
		/* Nothing ready, so wait for activity */
		init_waitqueue_entry(&wait, current);
		add_wait_queue(sk_sleep(sock->sk), &wait);
		set_current_state(TASK_INTERRUPTIBLE);
		schedule();
		remove_wait_queue(sk_sleep(sock->sk), &wait);
#ifdef HAVE_KERN_SOCK_ACCEPT_FLAG_ARG
		rc = sock->ops->accept(sock, newsock, O_NONBLOCK, false);
#else
		rc = sock->ops->accept(sock, newsock, O_NONBLOCK);
#endif
	}

	if (rc != 0)
		goto failed;

	*newsockp = newsock;
	return 0;

failed:
	sock_release(newsock);
	return rc;
}

int
lnet_sock_connect(struct socket **sockp, int *fatal,
		  __u32 local_ip, int local_port,
		  __u32 peer_ip, int peer_port)
{
	struct sockaddr_in  srvaddr;
	int		    rc;

	rc = lnet_sock_create(sockp, fatal, local_ip, local_port);
	if (rc != 0)
		return rc;

	memset(&srvaddr, 0, sizeof(srvaddr));
	srvaddr.sin_family = AF_INET;
	srvaddr.sin_port = htons(peer_port);
	srvaddr.sin_addr.s_addr = htonl(peer_ip);

	rc = kernel_connect(*sockp, (struct sockaddr *)&srvaddr,
			    sizeof(srvaddr), 0);
	if (rc == 0)
		return 0;

	/* EADDRNOTAVAIL probably means we're already connected to the same
	 * peer/port on the same local port on a differently typed
	 * connection.	Let our caller retry with a different local
	 * port... */
	*fatal = !(rc == -EADDRNOTAVAIL);

	CDEBUG_LIMIT(*fatal ? D_NETERROR : D_NET,
	       "Error %d connecting %pI4h/%d -> %pI4h/%d\n", rc,
	       &local_ip, local_port, &peer_ip, peer_port);

	sock_release(*sockp);
	return rc;
}
