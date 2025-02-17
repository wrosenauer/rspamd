/*-
 * Copyright 2016 Vsevolod Stakhov
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "config.h"
#include "libutil/util.h"
#include "libutil/logger.h"
#include "ssl_util.h"

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/rand.h>
#include <openssl/conf.h>
#include <openssl/x509v3.h>

enum rspamd_ssl_state {
	ssl_conn_reset = 0,
	ssl_conn_init,
	ssl_conn_connected,
	ssl_next_read,
	ssl_next_write
};

enum rspamd_ssl_shutdown {
	ssl_shut_default = 0,
	ssl_shut_unclean,
};

struct rspamd_ssl_connection {
	gint fd;
	enum rspamd_ssl_state state;
	enum rspamd_ssl_shutdown shut;
	gboolean verify_peer;
	SSL *ssl;
	gchar *hostname;
	struct rspamd_io_ev *ev;
	struct ev_loop *event_loop;
	rspamd_ssl_handler_t handler;
	rspamd_ssl_error_handler_t err_handler;
	gpointer handler_data;
};

static GQuark
rspamd_ssl_quark (void)
{
	return g_quark_from_static_string ("rspamd-ssl");
}

#if (OPENSSL_VERSION_NUMBER >= 0x10100000L) && !defined(LIBRESSL_VERSION_NUMBER)
#ifndef X509_get_notBefore
#define X509_get_notBefore(x) X509_get0_notBefore(x)
#endif
#ifndef X509_get_notAfter
#define X509_get_notAfter(x) X509_get0_notAfter(x)
#endif
#ifndef ASN1_STRING_data
#define ASN1_STRING_data(x) ASN1_STRING_get0_data(x)
#endif
#endif

/* $OpenBSD: tls_verify.c,v 1.14 2015/09/29 10:17:04 deraadt Exp $ */
/*
 * Copyright (c) 2014 Jeremie Courreges-Anglas <jca@openbsd.org>
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

static gboolean
rspamd_tls_match_name (const char *cert_name, const char *name)
{
	const char *cert_domain, *domain, *next_dot;

	if (g_ascii_strcasecmp (cert_name, name) == 0) {
		return TRUE;
	}

	/* Wildcard match? */
	if (cert_name[0] == '*') {
		/*
		 * Valid wildcards:
		 * - "*.domain.tld"
		 * - "*.sub.domain.tld"
		 * - etc.
		 * Reject "*.tld".
		 * No attempt to prevent the use of eg. "*.co.uk".
		 */
		cert_domain = &cert_name[1];
		/* Disallow "*"  */
		if (cert_domain[0] == '\0') {
			return FALSE;
		}

		/* Disallow "*foo" */
		if (cert_domain[0] != '.') {
			return FALSE;
		}
		/* Disallow "*.." */
		if (cert_domain[1] == '.') {
			return FALSE;
		}
		next_dot = strchr (&cert_domain[1], '.');
		/* Disallow "*.bar" */
		if (next_dot == NULL) {
			return FALSE;
		}
		/* Disallow "*.bar.." */
		if (next_dot[1] == '.') {
			return FALSE;
		}

		domain = strchr (name, '.');

		/* No wildcard match against a name with no host part. */
		if (name[0] == '.') {
			return FALSE;
		}
		/* No wildcard match against a name with no domain part. */
		if (domain == NULL || strlen (domain) == 1) {
			return FALSE;
		}

		if (g_ascii_strcasecmp (cert_domain, domain) == 0) {
			return TRUE;
		}
	}

	return FALSE;
}

/* See RFC 5280 section 4.2.1.6 for SubjectAltName details. */
static gboolean
rspamd_tls_check_subject_altname (X509 *cert, const char *name)
{
	STACK_OF(GENERAL_NAME) *altname_stack = NULL;
	int addrlen, type;
	int count, i;
	union {
		struct in_addr ip4;
		struct in6_addr ip6;
	} addrbuf;
	gboolean ret = FALSE;

	altname_stack = X509_get_ext_d2i (cert, NID_subject_alt_name, NULL, NULL);

	if (altname_stack == NULL) {
		return FALSE;
	}

	if (inet_pton (AF_INET, name, &addrbuf) == 1) {
		type = GEN_IPADD;
		addrlen = 4;
	}
	else if (inet_pton (AF_INET6, name, &addrbuf) == 1) {
		type = GEN_IPADD;
		addrlen = 16;
	}
	else {
		type = GEN_DNS;
		addrlen = 0;
	}

	count = sk_GENERAL_NAME_num (altname_stack);

	for (i = 0; i < count; i++) {
		GENERAL_NAME *altname;

		altname = sk_GENERAL_NAME_value (altname_stack, i);

		if (altname->type != type) {
			continue;
		}

		if (type == GEN_DNS) {
			const char *data;
			int format, len;

			format = ASN1_STRING_type (altname->d.dNSName);

			if (format == V_ASN1_IA5STRING) {
				data = (const char *)ASN1_STRING_data (altname->d.dNSName);
				len = ASN1_STRING_length (altname->d.dNSName);

				if (len < 0 || len != (gint)strlen (data)) {
					ret = FALSE;
					break;
				}

				/*
				 * Per RFC 5280 section 4.2.1.6:
				 * " " is a legal domain name, but that
				 * dNSName must be rejected.
				 */
				if (strcmp (data, " ") == 0) {
					ret = FALSE;
					break;
				}

				if (rspamd_tls_match_name (data, name)) {
					ret = TRUE;
					break;
				}
			}
		}
		else if (type == GEN_IPADD) {
			const char *data;
			int datalen;

			datalen = ASN1_STRING_length (altname->d.iPAddress);
			data = (const char *)ASN1_STRING_data (altname->d.iPAddress);

			if (datalen < 0) {
				ret = FALSE;
				break;
			}

			/*
			 * Per RFC 5280 section 4.2.1.6:
			 * IPv4 must use 4 octets and IPv6 must use 16 octets.
			 */
			if (datalen == addrlen && memcmp (data, &addrbuf, addrlen) == 0) {
				ret = TRUE;
				break;
			}
		}
	}

	sk_GENERAL_NAME_pop_free (altname_stack, GENERAL_NAME_free);
	return ret;
}

static gboolean
rspamd_tls_check_common_name (X509 *cert, const char *name)
{
	X509_NAME *subject_name;
	char *common_name = NULL;
	union {
		struct in_addr ip4;
		struct in6_addr ip6;
	} addrbuf;
	int common_name_len;
	gboolean ret = FALSE;

	subject_name = X509_get_subject_name (cert);
	if (subject_name == NULL) {
		goto out;
	}

	common_name_len = X509_NAME_get_text_by_NID (subject_name, NID_commonName, NULL, 0);

	if (common_name_len < 0) {
		goto out;
	}

	common_name = g_malloc0 (common_name_len + 1);
	X509_NAME_get_text_by_NID (subject_name, NID_commonName, common_name,
			common_name_len + 1);

	/* NUL bytes in CN? */
	if (common_name_len != (gint)strlen (common_name)) {
		goto out;
	}

	if (inet_pton (AF_INET, name, &addrbuf) == 1
			|| inet_pton (AF_INET6, name, &addrbuf) == 1) {
		/*
		 * We don't want to attempt wildcard matching against IP
		 * addresses, so perform a simple comparison here.
		 */
		if (strcmp (common_name, name) == 0) {
			ret = TRUE;
		}
		else {
			ret = FALSE;
		}

		goto out;
	}

	if (rspamd_tls_match_name (common_name, name)) {
		ret = TRUE;
	}

out:
	g_free (common_name);

	return ret;
}

static gboolean
rspamd_tls_check_name (X509 *cert, const char *name)
{
	gboolean ret;

	ret = rspamd_tls_check_subject_altname (cert, name);
	if (ret) {
		return ret;
	}

	return rspamd_tls_check_common_name (cert, name);
}

static gboolean
rspamd_ssl_peer_verify (struct rspamd_ssl_connection *c)
{
	X509 *server_cert;
	glong ver_err;
	GError *err = NULL;

	ver_err = SSL_get_verify_result (c->ssl);

	if (ver_err != X509_V_OK) {
		g_set_error (&err, rspamd_ssl_quark (), ver_err, "certificate validation "
				"failed: %s", X509_verify_cert_error_string (ver_err));
		c->err_handler (c->handler_data, err);
		g_error_free (err);

		return FALSE;
	}

	/* Get server's certificate */
	server_cert =  SSL_get_peer_certificate (c->ssl);
	if (server_cert == NULL) {
		g_set_error (&err, rspamd_ssl_quark (), ver_err, "peer certificate is absent");
		c->err_handler (c->handler_data, err);
		g_error_free (err);

		return FALSE;
	}

	if (c->hostname) {
		if (!rspamd_tls_check_name (server_cert, c->hostname)) {
			X509_free (server_cert);
			g_set_error (&err, rspamd_ssl_quark (), ver_err, "peer certificate fails "
					"hostname verification for %s", c->hostname);
			c->err_handler (c->handler_data, err);
			g_error_free (err);

			return FALSE;
		}
	}

	X509_free (server_cert);

	return TRUE;
}

static void
rspamd_tls_set_error (gint retcode, const gchar *stage, GError **err)
{
	GString *reason;
	gchar buf[120];
	gint err_code = 0, last_err = 0;

	reason = g_string_sized_new (sizeof (buf));

	if (retcode == SSL_ERROR_SYSCALL) {
		rspamd_printf_gstring (reason, "syscall fail: %s", strerror (errno));
		err_code = errno;
	}
	else {
		while ((err_code = ERR_get_error()) != 0) {
			last_err = err_code;
			ERR_error_string (err_code, buf);
			rspamd_printf_gstring (reason, "ssl error: %s,", buf);
		}

		err_code = last_err;

		if (reason->str[reason->len - 1] == ',') {
			reason->str[reason->len - 1] = '\0';
			reason->len --;
		}
	}

	g_set_error (err, rspamd_ssl_quark (), err_code,
			"ssl %s error: %s", stage, reason->str);
	g_string_free (reason, TRUE);
}

static void
rspamd_ssl_event_handler (gint fd, short what, gpointer ud)
{
	struct rspamd_ssl_connection *c = ud;
	gint ret;
	GError *err = NULL;

	if (what == EV_TIMER) {
		c->shut = ssl_shut_unclean;
	}

	switch (c->state) {
	case ssl_conn_init:
		/* Continue connection */
		ret = SSL_connect (c->ssl);

		if (ret == 1) {
			rspamd_ev_watcher_stop (c->event_loop, c->ev);
			/* Verify certificate */
			if ((!c->verify_peer) || rspamd_ssl_peer_verify (c)) {
				c->state = ssl_conn_connected;
				c->handler (fd, EV_WRITE, c->handler_data);
			}
			else {
				return;
			}
		}
		else {
			ret = SSL_get_error (c->ssl, ret);

			if (ret == SSL_ERROR_WANT_READ) {
				what = EV_READ;
			}
			else if (ret == SSL_ERROR_WANT_WRITE) {
				what = EV_WRITE;
			}
			else {
				rspamd_ev_watcher_stop (c->event_loop, c->ev);
				rspamd_tls_set_error (ret, "connect", &err);
				c->err_handler (c->handler_data, err);
				g_error_free (err);
				return;
			}

			rspamd_ev_watcher_reschedule (c->event_loop, c->ev, what);

		}
		break;
	case ssl_next_read:
		rspamd_ev_watcher_reschedule (c->event_loop, c->ev, EV_READ);
		c->state = ssl_conn_connected;
		c->handler (fd, EV_READ, c->handler_data);
		break;
	case ssl_next_write:
	case ssl_conn_connected:
		rspamd_ev_watcher_reschedule (c->event_loop, c->ev, what);
		c->state = ssl_conn_connected;
		c->handler (fd, what, c->handler_data);
		break;
	default:
		rspamd_ev_watcher_stop (c->event_loop, c->ev);
		g_set_error (&err, rspamd_ssl_quark (), EINVAL,
				"ssl bad state error: %d", c->state);
		c->err_handler (c->handler_data, err);
		g_error_free (err);
		break;
	}
}

struct rspamd_ssl_connection *
rspamd_ssl_connection_new (gpointer ssl_ctx, struct ev_loop *ev_base,
		gboolean verify_peer)
{
	struct rspamd_ssl_connection *c;

	g_assert (ssl_ctx != NULL);
	c = g_malloc0 (sizeof (*c));
	c->ssl = SSL_new (ssl_ctx);
	c->event_loop = ev_base;
	c->verify_peer = verify_peer;

	return c;
}


gboolean
rspamd_ssl_connect_fd (struct rspamd_ssl_connection *conn, gint fd,
		const gchar *hostname, struct rspamd_io_ev *ev, ev_tstamp timeout,
		rspamd_ssl_handler_t handler, rspamd_ssl_error_handler_t err_handler,
		gpointer handler_data)
{
	gint ret;
	short what;

	g_assert (conn != NULL);

	if (conn->state != ssl_conn_reset) {
		return FALSE;
	}

	conn->fd = fd;
	conn->ev = ev;
	conn->handler = handler;
	conn->err_handler = err_handler;
	conn->handler_data = handler_data;

	if (SSL_set_fd (conn->ssl, fd) != 1) {
		return FALSE;
	}

	if (hostname) {
		conn->hostname = g_strdup (hostname);
#ifdef HAVE_SSL_TLSEXT_HOSTNAME
		SSL_set_tlsext_host_name (conn->ssl, conn->hostname);
#endif
	}

	conn->state = ssl_conn_init;

	ret = SSL_connect (conn->ssl);

	if (ret == 1) {
		conn->state = ssl_conn_connected;

		rspamd_ev_watcher_stop (conn->event_loop, ev);
		rspamd_ev_watcher_init (ev, fd, EV_WRITE, rspamd_ssl_event_handler, conn);
		rspamd_ev_watcher_start (conn->event_loop, ev, timeout);
	}
	else {
		ret = SSL_get_error (conn->ssl, ret);

		if (ret == SSL_ERROR_WANT_READ) {
			what = EV_READ;
		}
		else if (ret == SSL_ERROR_WANT_WRITE) {
			what = EV_WRITE;
		}
		else {
			conn->shut = ssl_shut_unclean;

			return FALSE;
		}

		rspamd_ev_watcher_stop (conn->event_loop, ev);
		rspamd_ev_watcher_init (ev, fd, EV_WRITE|EV_READ,
				rspamd_ssl_event_handler, conn);
		rspamd_ev_watcher_start (conn->event_loop, ev, timeout);
	}

	return TRUE;
}

gssize
rspamd_ssl_read (struct rspamd_ssl_connection *conn, gpointer buf,
		gsize buflen)
{
	gint ret;
	short what;
	GError *err = NULL;

	g_assert (conn != NULL);

	if (conn->state != ssl_conn_connected && conn->state != ssl_next_read) {
		errno = EINVAL;
		g_set_error (&err, rspamd_ssl_quark (), ECONNRESET,
				"ssl state error: cannot read data");
		conn->shut = ssl_shut_unclean;
		conn->err_handler (conn->handler_data, err);
		g_error_free (err);

		return -1;
	}

	ret = SSL_read (conn->ssl, buf, buflen);

	if (ret > 0) {
		conn->state = ssl_conn_connected;
		return ret;
	}
	else if (ret == 0) {
		ret = SSL_get_error (conn->ssl, ret);

		if (ret == SSL_ERROR_ZERO_RETURN || ret == SSL_ERROR_SYSCALL) {
			conn->state = ssl_conn_reset;
			return 0;
		}
		else {
			conn->shut = ssl_shut_unclean;
			rspamd_tls_set_error (ret, "read", &err);
			conn->err_handler (conn->handler_data, err);
			g_error_free (err);
			errno = EINVAL;

			return -1;
		}
	}
	else {
		ret = SSL_get_error (conn->ssl, ret);
		conn->state = ssl_next_read;
		what = 0;

		if (ret == SSL_ERROR_WANT_READ) {
			what |= EV_READ;
		}
		else if (ret == SSL_ERROR_WANT_WRITE) {
			what |= EV_WRITE;
		}
		else {
			conn->shut = ssl_shut_unclean;
			rspamd_tls_set_error (ret, "read", &err);
			conn->err_handler (conn->handler_data, err);
			g_error_free (err);
			errno = EINVAL;

			return -1;
		}

		rspamd_ev_watcher_reschedule (conn->event_loop, conn->ev, what);
		errno = EAGAIN;
	}

	return -1;
}

gssize
rspamd_ssl_write (struct rspamd_ssl_connection *conn, gconstpointer buf,
		gsize buflen)
{
	gint ret;
	short what;
	GError *err = NULL;

	g_assert (conn != NULL);

	if (conn->state != ssl_conn_connected && conn->state != ssl_next_write) {
		errno = EINVAL;
		return -1;
	}

	ret = SSL_write (conn->ssl, buf, buflen);

	if (ret > 0) {
		conn->state = ssl_conn_connected;
		return ret;
	}
	else if (ret == 0) {
		ret = SSL_get_error (conn->ssl, ret);

		if (ret == SSL_ERROR_ZERO_RETURN) {
			rspamd_tls_set_error (ret, "write", &err);
			conn->err_handler (conn->handler_data, err);
			g_error_free (err);
			errno = ECONNRESET;
			conn->state = ssl_conn_reset;

			return -1;
		}
		else {
			conn->shut = ssl_shut_unclean;
			rspamd_tls_set_error (ret, "write", &err);
			conn->err_handler (conn->handler_data, err);
			g_error_free (err);
			errno = EINVAL;

			return -1;
		}
	}
	else {
		ret = SSL_get_error (conn->ssl, ret);
		conn->state = ssl_next_read;

		if (ret == SSL_ERROR_WANT_READ) {
			what = EV_READ;
		}
		else if (ret == SSL_ERROR_WANT_WRITE) {
			what = EV_WRITE;
		}
		else {
			conn->shut = ssl_shut_unclean;
			rspamd_tls_set_error (ret, "write", &err);
			conn->err_handler (conn->handler_data, err);
			g_error_free (err);
			errno = EINVAL;

			return -1;
		}

		rspamd_ev_watcher_reschedule (conn->event_loop, conn->ev, what);
		errno = EAGAIN;
	}

	return -1;
}

gssize
rspamd_ssl_writev (struct rspamd_ssl_connection *conn, struct iovec *iov,
		gsize iovlen)
{
	static guchar ssl_buf[16000];
	guchar *p;
	struct iovec *cur;
	guint i, remain;

	remain = sizeof (ssl_buf);
	p = ssl_buf;

	for (i = 0; i < iovlen; i ++) {
		cur = &iov[i];

		if (cur->iov_len > 0) {
			if (remain >= cur->iov_len) {
				memcpy (p, cur->iov_base, cur->iov_len);
				p += cur->iov_len;
				remain -= cur->iov_len;
			}
			else {
				memcpy (p, cur->iov_base, remain);
				p += remain;
				remain = 0;
				break;
			}
		}
	}

	return rspamd_ssl_write (conn, ssl_buf, p - ssl_buf);
}

/**
 * Removes connection data
 * @param conn
 */
void
rspamd_ssl_connection_free (struct rspamd_ssl_connection *conn)
{
	if (conn) {
		/*
		 * SSL_RECEIVED_SHUTDOWN tells SSL_shutdown to act as if we had already
		 * received a close notify from the other end.  SSL_shutdown will then
		 * send the final close notify in reply.  The other end will receive the
		 * close notify and send theirs.  By this time, we will have already
		 * closed the socket and the other end's real close notify will never be
		 * received.  In effect, both sides will think that they have completed a
		 * clean shutdown and keep their sessions valid.  This strategy will fail
		 * if the socket is not ready for writing, in which case this hack will
		 * lead to an unclean shutdown and lost session on the other end.
		 */
		if (conn->shut == ssl_shut_unclean) {
			SSL_set_shutdown (conn->ssl, SSL_RECEIVED_SHUTDOWN|SSL_SENT_SHUTDOWN);
			SSL_set_quiet_shutdown (conn->ssl, 1);
		}
		else {
			SSL_set_shutdown (conn->ssl, SSL_RECEIVED_SHUTDOWN);
		}

		/* Stupid hack to enforce SSL to do shutdown sequence */
		for (guint i = 0; i < 4; i++) {
			if (SSL_shutdown (conn->ssl)) {
				break;
			}
		}

		SSL_free (conn->ssl);

		if (conn->hostname) {
			g_free (conn->hostname);
		}

		g_free (conn);
	}
}
