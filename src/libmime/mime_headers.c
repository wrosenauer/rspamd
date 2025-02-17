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

#include "mime_headers.h"
#include "smtp_parsers.h"
#include "mime_encoding.h"
#include "contrib/uthash/utlist.h"
#include "libserver/mempool_vars_internal.h"
#include "libserver/url.h"
#include <unicode/utf8.h>

static void
rspamd_mime_header_check_special (struct rspamd_task *task,
		struct rspamd_mime_header *rh)
{
	guint64 h;
	struct received_header *recv;
	const gchar *p, *end;
	gchar *id;

	h = rspamd_icase_hash (rh->name, strlen (rh->name), 0xdeadbabe);

	switch (h) {
	case 0x88705DC4D9D61ABULL:	/* received */
		recv = rspamd_mempool_alloc0 (task->task_pool,
				sizeof (struct received_header));
		recv->hdr = rh;

		if (rspamd_smtp_received_parse (task, rh->decoded,
				strlen (rh->decoded), recv) != -1) {
			g_ptr_array_add (task->received, recv);
		}

		rh->type = RSPAMD_HEADER_RECEIVED;
		break;
	case 0x76F31A09F4352521ULL:	/* to */
		task->rcpt_mime = rspamd_email_address_from_mime (task->task_pool,
				rh->decoded, strlen (rh->decoded), task->rcpt_mime);
		rh->type = RSPAMD_HEADER_TO|RSPAMD_HEADER_RCPT|RSPAMD_HEADER_UNIQUE;
		break;
	case 0x7EB117C1480B76ULL:	/* cc */
		task->rcpt_mime = rspamd_email_address_from_mime (task->task_pool,
				rh->decoded, strlen (rh->decoded), task->rcpt_mime);
		rh->type = RSPAMD_HEADER_CC|RSPAMD_HEADER_RCPT|RSPAMD_HEADER_UNIQUE;
		break;
	case 0xE4923E11C4989C8DULL:	/* bcc */
		task->rcpt_mime = rspamd_email_address_from_mime (task->task_pool,
				rh->decoded, strlen (rh->decoded), task->rcpt_mime);
		rh->type = RSPAMD_HEADER_BCC|RSPAMD_HEADER_RCPT|RSPAMD_HEADER_UNIQUE;
		break;
	case 0x41E1985EDC1CBDE4ULL:	/* from */
		task->from_mime = rspamd_email_address_from_mime (task->task_pool,
				rh->decoded, strlen (rh->decoded), task->from_mime);
		rh->type = RSPAMD_HEADER_FROM|RSPAMD_HEADER_SENDER|RSPAMD_HEADER_UNIQUE;
		break;
	case 0x43A558FC7C240226ULL:	/* message-id */ {

		rh->type = RSPAMD_HEADER_MESSAGE_ID|RSPAMD_HEADER_UNIQUE;
		p = rh->decoded;
		end = p + strlen (p);

		if (*p == '<') {
			p++;
		}

		if (end > p) {
			gchar *d;

			if (*(end - 1) == '>') {
				end --;
			}

			id = rspamd_mempool_alloc (task->task_pool, end - p + 1);
			d = id;

			while (p < end) {
				if (g_ascii_isgraph (*p)) {
					*d++ = *p++;
				}
				else {
					*d++ = '?';
					p++;
				}
			}

			*d = '\0';

			task->message_id = id;
		}

		break;
	}
	case 0xB91D3910358E8212ULL:	/* subject */
		if (task->subject == NULL) {
			task->subject = rh->decoded;
		}
		rh->type = RSPAMD_HEADER_SUBJECT|RSPAMD_HEADER_UNIQUE;
		break;
	case 0xEE4AA2EAAC61D6F4ULL:	/* return-path */
		if (task->from_envelope == NULL) {
			task->from_envelope = rspamd_email_address_from_smtp (rh->decoded,
					strlen (rh->decoded));
		}
		rh->type = RSPAMD_HEADER_RETURN_PATH|RSPAMD_HEADER_UNIQUE;
		break;
	case 0xB9EEFAD2E93C2161ULL:	/* delivered-to */
		if (task->deliver_to == NULL) {
			task->deliver_to = rh->decoded;
		}
		rh->type = RSPAMD_HEADER_DELIVERED_TO;
		break;
	case 0x2EC3BFF3C393FC10ULL: /* date */
	case 0xAC0DDB1A1D214CAULL: /* sender */
	case 0x54094572367AB695ULL: /* in-reply-to */
	case 0x81CD9E9131AB6A9AULL: /* content-type */
	case 0xC39BD9A75AA25B60ULL: /* content-transfer-encoding */
	case 0xB3F6704CB3AD6589ULL: /* references */
		rh->type = RSPAMD_HEADER_UNIQUE;
		break;
	}
}

static void
rspamd_mime_header_add (struct rspamd_task *task,
		GHashTable *target, GQueue *order,
		struct rspamd_mime_header *rh,
		gboolean check_special)
{
	GPtrArray *ar;

	if ((ar = g_hash_table_lookup (target, rh->name)) != NULL) {
		g_ptr_array_add (ar, rh);
		msg_debug_task ("append raw header %s: %s", rh->name, rh->value);
	}
	else {
		ar = g_ptr_array_sized_new (2);
		g_ptr_array_add (ar, rh);
		g_hash_table_insert (target, rh->name, ar);
		msg_debug_task ("add new raw header %s: %s", rh->name, rh->value);
	}

	g_queue_push_tail (order, rh);

	if (check_special) {
		rspamd_mime_header_check_special (task, rh);
	}
}


/* Convert raw headers to a list of struct raw_header * */
void
rspamd_mime_headers_process (struct rspamd_task *task, GHashTable *target,
		GQueue *order,
		const gchar *in, gsize len,
		gboolean check_newlines)
{
	struct rspamd_mime_header *nh = NULL;
	const gchar *p, *c, *end;
	gchar *tmp, *tp;
	gint state = 0, l, next_state = 100, err_state = 100, t_state;
	gboolean valid_folding = FALSE;
	guint nlines_count[RSPAMD_TASK_NEWLINES_MAX];
	guint norder = 0;

	p = in;
	end = p + len;
	c = p;
	memset (nlines_count, 0, sizeof (nlines_count));
	msg_debug_task ("start processing headers");

	while (p < end) {
		/* FSM for processing headers */
		switch (state) {
		case 0:
			/* Begin processing headers */
			if (!g_ascii_isalpha (*p)) {
				/* We have some garbage at the beginning of headers, skip this line */
				state = 100;
				next_state = 0;
			}
			else {
				state = 1;
				c = p;
			}
			break;
		case 1:
			/* We got something like header's name */
			if (*p == ':') {
				nh = rspamd_mempool_alloc0 (task->task_pool,
						sizeof (struct rspamd_mime_header));
				l = p - c;
				tmp = rspamd_mempool_alloc (task->task_pool, l + 1);
				rspamd_null_safe_copy (c, l, tmp, l + 1);
				nh->name = tmp;
				nh->empty_separator = TRUE;
				nh->raw_value = c;
				nh->raw_len = p - c; /* Including trailing ':' */
				p++;
				state = 2;
				c = p;
			}
			else if (g_ascii_isspace (*p)) {
				/* Not header but some garbage */
				task->flags |= RSPAMD_TASK_FLAG_BROKEN_HEADERS;
				state = 100;
				next_state = 0;
			}
			else {
				p++;
			}
			break;
		case 2:
			/* We got header's name, so skip any \t or spaces */
			if (*p == '\t') {
				nh->tab_separated = TRUE;
				nh->empty_separator = FALSE;
				p++;
			}
			else if (*p == ' ') {
				nh->empty_separator = FALSE;
				p++;
			}
			else if (*p == '\n' || *p == '\r') {

				if (check_newlines) {
					if (*p == '\n') {
						nlines_count[RSPAMD_TASK_NEWLINES_LF] ++;
					}
					else if (*(p + 1) == '\n') {
						nlines_count[RSPAMD_TASK_NEWLINES_CRLF] ++;
					}
					else {
						nlines_count[RSPAMD_TASK_NEWLINES_CR] ++;
					}
				}

				/* Process folding */
				state = 99;
				l = p - c;
				if (l > 0) {
					tmp = rspamd_mempool_alloc (task->task_pool, l + 1);
					rspamd_null_safe_copy (c, l, tmp, l + 1);
					nh->separator = tmp;
				}
				next_state = 3;
				err_state = 5;
				c = p;
			}
			else {
				/* Process value */
				l = p - c;
				if (l >= 0) {
					tmp = rspamd_mempool_alloc (task->task_pool, l + 1);
					rspamd_null_safe_copy (c, l, tmp, l + 1);
					nh->separator = tmp;
				}
				c = p;
				state = 3;
			}
			break;
		case 3:
			if (*p == '\r' || *p == '\n') {
				/* Hold folding */
				if (check_newlines) {
					if (*p == '\n') {
						nlines_count[RSPAMD_TASK_NEWLINES_LF] ++;
					}
					else if (*(p + 1) == '\n') {
						nlines_count[RSPAMD_TASK_NEWLINES_CRLF] ++;
					}
					else {
						nlines_count[RSPAMD_TASK_NEWLINES_CR] ++;
					}
				}
				state = 99;
				next_state = 3;
				err_state = 4;
			}
			else if (p + 1 == end) {
				state = 4;
			}
			else {
				p++;
			}
			break;
		case 4:
			/* Copy header's value */

			/*
			 * XXX:
			 * The original decision to use here null terminated
			 * strings was extremely poor!
			 */
			l = p - c;
			tmp = rspamd_mempool_alloc (task->task_pool, l + 1);
			tp = tmp;
			t_state = 0;
			while (l--) {
				if (t_state == 0) {
					/* Before folding */
					if (*c == '\n' || *c == '\r') {
						t_state = 1;
						c++;
						*tp++ = ' ';
					}
					else {
						if (*c != '\0') {
							*tp++ = *c++;
						}
						else {
							c++;
						}
					}
				}
				else if (t_state == 1) {
					/* Inside folding */
					if (g_ascii_isspace (*c)) {
						c++;
					}
					else {
						t_state = 0;
						if (*c != '\0') {
							*tp++ = *c++;
						}
						else {
							c++;
						}
					}
				}
			}
			/* Strip last space that can be added by \r\n parsing */
			if (*(tp - 1) == ' ') {
				tp--;
			}

			*tp = '\0';
			/* Strip the initial spaces that could also be added by folding */
			while (*tmp != '\0' && g_ascii_isspace (*tmp)) {
				tmp ++;
			}

			if (p + 1 == end) {
				nh->raw_len = end - nh->raw_value;
			}
			else {
				nh->raw_len = p - nh->raw_value;
			}

			nh->value = tmp;

			gboolean broken_utf = FALSE;

			nh->decoded = rspamd_mime_header_decode (task->task_pool,
					nh->value, strlen (tmp), &broken_utf);

			if (broken_utf) {
				task->flags |= RSPAMD_TASK_FLAG_BAD_UNICODE;
			}

			if (nh->decoded == NULL) {
				nh->decoded = "";
			}

			/* We also validate utf8 and replace all non-valid utf8 chars */
			rspamd_mime_charset_utf_enforce (nh->decoded, strlen (nh->decoded));
			nh->order = norder ++;
			rspamd_mime_header_add (task, target, order, nh, check_newlines);
			nh = NULL;
			state = 0;
			break;
		case 5:
			/* Header has only name, no value */
			nh->value = "";
			nh->decoded = "";
			nh->raw_len = p - nh->raw_value;
			nh->order = norder ++;
			rspamd_mime_header_add (task, target, order, nh, check_newlines);
			nh = NULL;
			state = 0;
			break;
		case 99:
			/* Folding state */
			if (p + 1 == end) {
				state = err_state;
			}
			else {
				if (*p == '\r' || *p == '\n') {
					p++;
					valid_folding = FALSE;
				}
				else if (*p == '\t' || *p == ' ') {
					/* Valid folding */
					p++;
					valid_folding = TRUE;
				}
				else {
					if (valid_folding) {
						debug_task ("go to state: %d->%d", state, next_state);
						state = next_state;
					}
					else {
						/* Fall back */
						debug_task ("go to state: %d->%d", state, err_state);
						state = err_state;
					}
				}
			}
			break;
		case 100:
			/* Fail state, skip line */

			if (*p == '\r') {
				if (*(p + 1) == '\n') {
					nlines_count[RSPAMD_TASK_NEWLINES_CRLF] ++;
					p++;
				}
				p++;
				state = next_state;
			}
			else if (*p == '\n') {
				nlines_count[RSPAMD_TASK_NEWLINES_LF] ++;

				if (*(p + 1) == '\r') {
					p++;
				}
				p++;
				state = next_state;
			}
			else if (p + 1 == end) {
				state = next_state;
				p++;
			}
			else {
				p++;
			}
			break;
		}
	}

	if (check_newlines) {
		guint max_cnt = 0;
		gint sel = 0;
		GList *cur;
		rspamd_cryptobox_hash_state_t hs;
		guchar hout[rspamd_cryptobox_HASHBYTES], *hexout;

		for (gint i = 0; i < RSPAMD_TASK_NEWLINES_MAX; i ++) {
			if (nlines_count[i] > max_cnt) {
				max_cnt = nlines_count[i];
				sel = i;
			}
		}

		task->nlines_type = sel;

		cur = order->head;
		rspamd_cryptobox_hash_init (&hs, NULL, 0);

		while (cur) {
			nh = cur->data;

			if (nh->name && nh->type != RSPAMD_HEADER_RECEIVED) {
				rspamd_cryptobox_hash_update (&hs, nh->name, strlen (nh->name));
			}

			cur = g_list_next (cur);
		}

		rspamd_cryptobox_hash_final (&hs, hout);
		hexout = rspamd_mempool_alloc (task->task_pool, sizeof (hout) * 2 + 1);
		hexout[sizeof (hout) * 2] = '\0';
		rspamd_encode_hex_buf (hout, sizeof (hout), hexout,
				sizeof (hout) * 2 + 1);
		rspamd_mempool_set_variable (task->task_pool,
				RSPAMD_MEMPOOL_HEADERS_HASH,
				hexout, NULL);
	}
}

static void
rspamd_mime_header_maybe_save_token (rspamd_mempool_t *pool, GString *out,
		GByteArray *token, GByteArray *decoded_token,
		rspamd_ftok_t *old_charset, rspamd_ftok_t *new_charset)
{
	if (new_charset->len == 0) {
		g_assert_not_reached ();
	}

	if (old_charset->len > 0) {
		if (rspamd_ftok_casecmp (new_charset, old_charset) == 0) {
			rspamd_ftok_t srch;

			/*
			 * Special case for iso-2022-jp:
			 * https://github.com/vstakhov/rspamd/issues/1669
			 */
			RSPAMD_FTOK_ASSIGN (&srch, "iso-2022-jp");

			if (rspamd_ftok_casecmp (new_charset, &srch) != 0) {
				/* We can concatenate buffers, just return */
				return;
			}
		}
	}

	/* We need to flush and decode old token to out string */
	if (rspamd_mime_to_utf8_byte_array (token, decoded_token,
			rspamd_mime_detect_charset (new_charset, pool))) {
		g_string_append_len (out, decoded_token->data, decoded_token->len);
	}

	/* We also reset buffer */
	g_byte_array_set_size (token, 0);
	/* Propagate charset */
	memcpy (old_charset, new_charset, sizeof (*old_charset));
}

static void
rspamd_mime_header_sanity_check (GString *str)
{
	gsize i;
	gchar t;

	for (i = 0; i < str->len; i ++) {
		t = str->str[i];
		if (!((t & 0x80) || g_ascii_isgraph (t))) {
			if (g_ascii_isspace (t)) {
				/* Replace spaces characters with plain space */
				str->str[i] = ' ';
			}
			else {
				str->str[i] = '?';
			}
		}
	}
}

gchar *
rspamd_mime_header_decode (rspamd_mempool_t *pool, const gchar *in,
		gsize inlen, gboolean *invalid_utf)
{
	GString *out;
	const guchar *c, *p, *end;
	const gchar *tok_start = NULL;
	gsize tok_len = 0, pos;
	GByteArray *token = NULL, *decoded;
	rspamd_ftok_t cur_charset = {0, NULL}, old_charset = {0, NULL};
	gint encoding;
	gssize r;
	guint qmarks = 0;
	gchar *ret;
	enum {
		parse_normal = 0,
		got_eqsign,
		got_encoded_start,
		got_more_qmark,
		skip_spaces,
	} state = parse_normal;

	g_assert (in != NULL);

	c = in;
	p = in;
	end = in + inlen;
	out = g_string_sized_new (inlen);
	token = g_byte_array_sized_new (80);
	decoded = g_byte_array_sized_new (122);

	while (p < end) {
		switch (state) {
		case parse_normal:
			if (*p == '=') {
				g_string_append_len (out, c, p - c);
				c = p;
				state = got_eqsign;
			}
			else if (*p >= 128) {
				gint off = 0;
				UChar32 uc;
				/* Unencoded character */
				g_string_append_len (out, c, p - c);
				/* Check if that's valid UTF8 */
				U8_NEXT (p, off, end - p, uc);

				if (uc <= 0) {
					c = p + 1;
					/* 0xFFFD in UTF8 */
					g_string_append_len (out, "   ", 3);
					off = 0;
					U8_APPEND_UNSAFE (out->str + out->len - 3,
							off, 0xfffd);

					if (invalid_utf) {
						*invalid_utf = TRUE;
					}
				}
				else {
					c = p;
					p = p + off;
					continue; /* To avoid p ++ after this block */
				}
			}
			p ++;
			break;
		case got_eqsign:
			if (*p == '?') {
				state = got_encoded_start;
				qmarks = 0;
			}
			else {
				g_string_append_len (out, c, 2);
				c = p + 1;
				state = parse_normal;
			}
			p ++;
			break;
		case got_encoded_start:
			if (*p == '?') {
				state = got_more_qmark;
				qmarks ++;
			}
			p ++;
			break;
		case got_more_qmark:
			if (*p == '=') {
				if (qmarks < 3) {
					state = got_encoded_start;
				}
				else {
					/* Finished encoded boundary */
					if (*c == '"') {
						/* Quoted string, non-RFC conformant but used by retards */
						c ++;
					}
					if (rspamd_rfc2047_parser (c, p - c + 1, &encoding,
							&cur_charset.begin, &cur_charset.len,
							&tok_start, &tok_len)) {
						/* We have a token, so we can decode it from `encoding` */
						if (token->len > 0) {
							if (old_charset.len == 0) {
								memcpy (&old_charset, &cur_charset,
										sizeof (old_charset));
							}

							rspamd_mime_header_maybe_save_token (pool, out,
									token, decoded,
									&old_charset, &cur_charset);
						}

						qmarks = 0;
						pos = token->len;
						g_byte_array_set_size (token, pos + tok_len);

						if (encoding == RSPAMD_RFC2047_QP) {
							r = rspamd_decode_qp2047_buf (tok_start, tok_len,
									token->data + pos, tok_len);

							if (r != -1) {
								token->len = pos + r;
							} else {
								/* Cannot decode qp */
								token->len -= tok_len;
							}
						} else {
							if (rspamd_cryptobox_base64_decode (tok_start, tok_len,
									token->data + pos, &tok_len)) {
								token->len = pos + tok_len;
							} else {
								/* Cannot decode */
								token->len -= tok_len;
							}
						}

						c = p + 1;
						state = skip_spaces;
					} else {
						/* Not encoded-word */
						old_charset.len = 0;

						if (token->len > 0) {
							rspamd_mime_header_maybe_save_token (pool, out,
									token, decoded,
									&old_charset, &cur_charset);
						}

						g_string_append_len (out, c, p - c);
						c = p;
						state = parse_normal;
					}
				} /* qmarks >= 3 */
			} /* p == '=' */
			else {
				state = got_encoded_start;
			}
			p ++;
			break;
		case skip_spaces:
			if (g_ascii_isspace (*p)) {
				p ++;
			}
			else if (*p == '=' && p < end - 1 && p[1] == '?') {
				/* Next boundary, can glue */
				c = p;
				p += 2;
				state = got_encoded_start;
			}
			else {
				/* Need to save spaces and decoded token */
				if (token->len > 0) {
					old_charset.len = 0;
					rspamd_mime_header_maybe_save_token (pool, out,
							token, decoded,
							&old_charset, &cur_charset);
				}

				g_string_append_len (out, c, p - c);
				c = p;
				state = parse_normal;
			}
			break;
		}
	}

	/* Leftover */
	switch (state) {
	case skip_spaces:
		if (token->len > 0 && cur_charset.len > 0) {
			old_charset.len = 0;
			rspamd_mime_header_maybe_save_token (pool, out,
					token, decoded,
					&old_charset, &cur_charset);
		}
		break;
	default:
		/* Just copy leftover */
		if (p > c) {
			g_string_append_len (out, c, p - c);
		}
		break;
	}

	g_byte_array_free (token, TRUE);
	g_byte_array_free (decoded, TRUE);
	rspamd_mime_header_sanity_check (out);
	ret = g_string_free (out, FALSE);
	rspamd_mempool_add_destructor (pool, g_free, ret);

	return ret;
}

gchar *
rspamd_mime_header_encode (const gchar *in, gsize len)
{
	const gchar *p = in, *end = in + len;
	gchar *out, encode_buf[80 * sizeof (guint32)];
	GString *res;
	gboolean need_encoding = FALSE;

	/* Check if we need to encode */
	while (p < end) {
		if ((((guchar)*p) & 0x80) != 0) {
			need_encoding = TRUE;
			break;
		}
		p ++;
	}

	if (!need_encoding) {
		out = g_malloc (len + 1);
		rspamd_strlcpy (out, in, len + 1);
	}
	else {
		/* Need encode */
		gsize ulen, pos;
		gint r;
		const gchar *prev;
		/* Choose step: =?UTF-8?Q?<qp>?= should be less than 76 chars */
		guint step = (76 - 12) / 3 + 1;

		ulen = g_utf8_strlen (in, len);
		res = g_string_sized_new (len * 2 + 1);
		pos = 0;
		prev = in;
		/* Adjust chunk size for unicode average length */
		step *= 1.0 * ulen / (gdouble)len;

		while (pos < ulen) {
			p = g_utf8_offset_to_pointer (in, pos);

			if (p > prev) {
				/* Encode and print */
				r = rspamd_encode_qp2047_buf (prev, p - prev,
						encode_buf, sizeof (encode_buf));

				if (r != -1) {
					if (res->len > 0) {
						rspamd_printf_gstring (res, " =?UTF-8?Q?%*s?=", r,
								encode_buf);
					}
					else {
						rspamd_printf_gstring (res, "=?UTF-8?Q?%*s?=", r,
								encode_buf);
					}
				}
			}

			pos += MIN (step, ulen - pos);
			prev = p;
		}

		/* Leftover */
		if (prev < end) {
			r = rspamd_encode_qp2047_buf (prev, end - prev,
					encode_buf, sizeof (encode_buf));

			if (r != -1) {
				if (res->len > 0) {
					rspamd_printf_gstring (res, " =?UTF-8?Q?%*s?=", r,
							encode_buf);
				}
				else {
					rspamd_printf_gstring (res, "=?UTF-8?Q?%*s?=", r,
							encode_buf);
				}
			}
		}

		out = g_string_free (res, FALSE);
	}

	return out;
}

gchar *
rspamd_mime_message_id_generate (const gchar *fqdn)
{
	GString *out;
	guint64 rnd, clk;

	out = g_string_sized_new (strlen (fqdn) + 22);
	rnd = ottery_rand_uint64 ();
	clk = rspamd_get_calendar_ticks () * 1e6;

	rspamd_printf_gstring (out, "%*bs.%*bs@%s",
			(gint)sizeof (guint64) - 3, (guchar *)&clk,
			(gint)sizeof (guint64), (gchar *)&rnd,
			fqdn);

	return g_string_free (out, FALSE);
}

enum rspamd_received_part_type {
	RSPAMD_RECEIVED_PART_FROM,
	RSPAMD_RECEIVED_PART_BY,
	RSPAMD_RECEIVED_PART_FOR,
	RSPAMD_RECEIVED_PART_WITH,
	RSPAMD_RECEIVED_PART_UNKNOWN,
};

struct rspamd_received_comment {
	gchar *data;
	gsize dlen;
	struct rspamd_received_comment *prev;
};

struct rspamd_received_part {
	enum rspamd_received_part_type type;
	gchar *data;
	gsize dlen;
	struct rspamd_received_comment *tail_comment;
	struct rspamd_received_comment *head_comment;
	struct rspamd_received_part *prev, *next;
};

static void
rspamd_smtp_received_part_set_or_append (struct rspamd_task *task,
										 const gchar *begin,
										 gsize len,
										 gchar **dest,
										 gsize *destlen)
{
	if (len == 0) {
		return;
	}

	if (*dest) {
		/* Append */
		gsize total_len = *destlen + len;
		gchar *new_dest;

		new_dest = rspamd_mempool_alloc (task->task_pool, total_len);
		memcpy (new_dest, *dest, *destlen);
		memcpy (new_dest + *destlen, begin, len);
		rspamd_str_lc (new_dest + *destlen, len);
		*dest = new_dest;
		*destlen = total_len;
	}
	else {
		/* Set */
		*dest = rspamd_mempool_alloc (task->task_pool, len);
		memcpy (*dest, begin, len);
		rspamd_str_lc (*dest, len);
		*dest = (gchar *)rspamd_string_len_strip (*dest, &len, " \t");
		*destlen = len;
	}
}

static struct rspamd_received_part *
rspamd_smtp_received_process_part (struct rspamd_task *task,
								   const char *data,
								   size_t len,
								   enum rspamd_received_part_type type,
								   goffset *last)
{
	struct rspamd_received_part *npart;
	const guchar *p, *c, *end;
	guint obraces = 0, ebraces = 0;
	gboolean seen_tcpinfo = FALSE;
	enum _parse_state {
		skip_spaces,
		in_comment,
		read_data,
		read_tcpinfo,
		all_done
	} state, next_state;

	npart = rspamd_mempool_alloc0 (task->task_pool, sizeof (*npart));
	npart->type = type;

	/* In this function, we just process comments and data separately */
	p = data;
	end = data + len;
	c = data;
	state = skip_spaces;
	next_state = read_data;

	while (p < end) {
		switch (state) {
		case skip_spaces:
			if (!g_ascii_isspace (*p)) {
				c = p;
				state = next_state;
			}
			else {
				p ++;
			}
			break;
		case in_comment:
			if (*p == '(') {
				obraces ++;
			}
			else if (*p == ')') {
				ebraces ++;

				if (ebraces >= obraces) {
					if (type != RSPAMD_RECEIVED_PART_UNKNOWN) {
						if (p > c) {
							struct rspamd_received_comment *comment;


							comment = rspamd_mempool_alloc0 (task->task_pool,
									sizeof (*comment));
							rspamd_smtp_received_part_set_or_append (task,
									c, p - c,
									&comment->data, &comment->dlen);

							if (!npart->head_comment) {
								comment->prev = NULL;
								npart->head_comment = comment;
								npart->tail_comment = comment;
							}
							else {
								comment->prev = npart->tail_comment;
								npart->tail_comment = comment;
							}
						}
					}

					p ++;
					c = p;
					state = skip_spaces;
					next_state = read_data;

					continue;
				}
			}

			p ++;
			break;
		case read_data:
			if (*p == '(') {
				if (p > c) {
					if (type != RSPAMD_RECEIVED_PART_UNKNOWN) {
						rspamd_smtp_received_part_set_or_append (task,
								c, p - c,
								&npart->data, &npart->dlen);
					}
				}

				state = in_comment;
				obraces = 1;
				ebraces = 0;
				p ++;
				c = p;
			}
			else if (g_ascii_isspace (*p)) {
				if (p > c) {
					if (type != RSPAMD_RECEIVED_PART_UNKNOWN) {
						rspamd_smtp_received_part_set_or_append (task,
								c, p - c,
								&npart->data, &npart->dlen);
					}
				}

				state = skip_spaces;
				next_state = read_data;
				c = p;
			}
			else if (*p == ';') {
				/* It is actually delimiter of date part if not in the comments */
				if (p > c) {
					if (type != RSPAMD_RECEIVED_PART_UNKNOWN) {
						rspamd_smtp_received_part_set_or_append (task,
								c, p - c,
								&npart->data, &npart->dlen);
					}
				}

				state = all_done;
				continue;
			}
			else if (npart->dlen > 0) {
				/* We have already received data and find something with no ( */
				if (!seen_tcpinfo && type == RSPAMD_RECEIVED_PART_FROM) {
					/* Check if we have something special here, such as TCPinfo */
					if (*c == '[') {
						state = read_tcpinfo;
						p ++;
					}
					else {
						state = all_done;
						continue;
					}
				}
				else {
					state = all_done;
					continue;
				}
			}
			else {
				p ++;
			}
			break;
		case read_tcpinfo:
			if (*p == ']') {
				rspamd_smtp_received_part_set_or_append (task,
						c, p - c + 1,
						&npart->data, &npart->dlen);
				seen_tcpinfo = TRUE;
				state = skip_spaces;
				next_state = read_data;
				c = p;
			}
			p ++;
			break;
		case all_done:
			*last = p - (const guchar *)data;
			return npart;
			break;
		}
	}

	/* Leftover */
	switch (state) {
	case read_data:
		if (p > c) {
			if (type != RSPAMD_RECEIVED_PART_UNKNOWN) {
				rspamd_smtp_received_part_set_or_append (task,
						c, p - c,
						&npart->data, &npart->dlen);
			}

			*last = p - (const guchar *)data;

			return npart;
		}
		break;
	case skip_spaces:
		*last = p - (const guchar *)data;

		return npart;
	default:
		break;
	}

	return NULL;
}

static struct rspamd_received_part *
rspamd_smtp_received_spill (struct rspamd_task *task,
							const char *data,
							size_t len,
							goffset *date_pos)
{
	const guchar *p, *end;
	struct rspamd_received_part *cur_part, *head = NULL;
	goffset pos = 0;

	p = data;
	end = data + len;

	while (p < end && g_ascii_isspace (*p)) {
		p ++;
	}

	len = end - p;

	/* Ignore all received but those started from from part */
	if (len <= 4 || (lc_map[p[0]] != 'f' &&
					 lc_map[p[1]] != 'r' &&
					 lc_map[p[2]] != 'o' &&
					 lc_map[p[3]] != 'm')) {
		return NULL;
	}

	p += sizeof ("from") - 1;

	/* We can now store from part */
	cur_part = rspamd_smtp_received_process_part (task, p, end - p,
			RSPAMD_RECEIVED_PART_FROM, &pos);

	if (!cur_part) {
		return NULL;
	}

	g_assert (pos != 0);
	p += pos;
	len = end > p ? end - p : 0;
	DL_APPEND (head, cur_part);


	if (len > 2 && (lc_map[p[0]] == 'b' &&
					lc_map[p[1]] == 'y')) {
		p += sizeof ("by") - 1;

		cur_part = rspamd_smtp_received_process_part (task, p, end - p,
				RSPAMD_RECEIVED_PART_BY, &pos);

		if (!cur_part) {
			return NULL;
		}

		g_assert (pos != 0);
		p += pos;
		len = end > p ? end - p : 0;
		DL_APPEND (head, cur_part);
	}

	while (p < end) {
		if (*p == ';') {
			/* We are at the date separator, stop here */
			*date_pos = p - (const guchar *)data + 1;
			break;
		}
		else {
			if (len > sizeof ("with") && (lc_map[p[0]] == 'w' &&
										  lc_map[p[1]] == 'i' &&
										  lc_map[p[2]] == 't' &&
										  lc_map[p[3]] == 'h')) {
				p += sizeof ("with") - 1;

				cur_part = rspamd_smtp_received_process_part (task, p, end - p,
						RSPAMD_RECEIVED_PART_WITH, &pos);
			}
			else if (len > sizeof ("for") && (lc_map[p[0]] == 'f' &&
											  lc_map[p[1]] == 'o' &&
											  lc_map[p[2]] == 'r')) {
				p += sizeof ("for") - 1;
				cur_part = rspamd_smtp_received_process_part (task, p, end - p,
						RSPAMD_RECEIVED_PART_FOR, &pos);
			}
			else {
				while (p < end) {
					if (!(g_ascii_isspace (*p) || *p == '(' || *p == ';')) {
						p ++;
					}
					else {
						break;
					}
				}

				if (p == end) {
					return NULL;
				}
				else if (*p == ';') {
					*date_pos = p - (const guchar *)data + 1;
					break;
				}
				else {
					cur_part = rspamd_smtp_received_process_part (task, p, end - p,
							RSPAMD_RECEIVED_PART_UNKNOWN, &pos);
				}
			}

			if (!cur_part) {
				return NULL;
			}
			else {
				g_assert (pos != 0);
				p += pos;
				len = end > p ? end - p : 0;
				DL_APPEND (head, cur_part);
			}
		}
	}

	return head;
}

static gboolean
rspamd_smtp_received_process_rdns (struct rspamd_task *task,
								   const gchar *begin,
								   gsize len,
								   const gchar **pdest)
{
	const gchar *p, *end;
	gsize hlen = 0;
	gboolean seen_dot = FALSE;

	p = begin;
	end = begin + len;

	while (p < end) {
		if (!g_ascii_isspace (*p) && rspamd_url_is_domain (*p)) {
			if (*p == '.') {
				seen_dot = TRUE;
			}

			hlen ++;
		}
		else {
			break;
		}

		p ++;
	}

	if (hlen > 0) {
		if (p == end) {
			/* All data looks like a hostname */
			gchar *dest;

			dest = rspamd_mempool_alloc (task->task_pool,
					hlen + 1);
			rspamd_strlcpy (dest, begin, hlen + 1);
			*pdest = dest;

			return TRUE;
		}
		else if (seen_dot && (g_ascii_isspace (*p) || *p == '[' || *p == '(')) {
			gchar *dest;

			dest = rspamd_mempool_alloc (task->task_pool,
					hlen + 1);
			rspamd_strlcpy (dest, begin, hlen + 1);
			*pdest = dest;

			return TRUE;
		}
	}

	return FALSE;
}

static gboolean
rspamd_smtp_received_process_host_tcpinfo (struct rspamd_task *task,
										   struct received_header *rh,
										   const gchar *data,
										   gsize len)
{
	rspamd_inet_addr_t *addr = NULL;
	gboolean ret = FALSE;

	if (data[0] == '[') {
		/* Likely Exim version */

		const gchar *brace_pos = memchr (data, ']', len);

		if (brace_pos) {
			addr = rspamd_parse_inet_address_pool (data + 1,
					brace_pos - data - 1,
					task->task_pool);

			if (addr) {
				rh->addr = addr;
				rh->real_ip = rspamd_mempool_strdup (task->task_pool,
						rspamd_inet_address_to_string (addr));
				rh->from_ip = rh->real_ip;
			}
		}
	}
	else {
		if (g_ascii_isxdigit (data[0])) {
			/* Try to parse IP address */
			addr = rspamd_parse_inet_address_pool (data,
					len, task->task_pool);
			if (addr) {
				rh->addr = addr;
				rh->real_ip = rspamd_mempool_strdup (task->task_pool,
						rspamd_inet_address_to_string (addr));
				rh->from_ip = rh->real_ip;
			}
		}

		if (!addr) {
			/* Try canonical Postfix version: rdns [ip] */
			const gchar *obrace_pos = memchr (data, '[', len),
					*ebrace_pos, *dend;

			if (obrace_pos) {
				dend = data + len;
				ebrace_pos = memchr (obrace_pos, ']', dend - obrace_pos);

				if (ebrace_pos) {
					addr = rspamd_parse_inet_address_pool (obrace_pos + 1,
							ebrace_pos - obrace_pos - 1,
							task->task_pool);

					if (addr) {
						rh->addr = addr;
						rh->real_ip = rspamd_mempool_strdup (task->task_pool,
								rspamd_inet_address_to_string (addr));
						rh->from_ip = rh->real_ip;

						/* Process with rDNS */
						if (rspamd_smtp_received_process_rdns (task,
								data,
								obrace_pos - data,
								&rh->real_hostname)) {
							ret = TRUE;
						}
					}
				}
			}
			else {
				/* Hostname or some crap, sigh... */
				if (rspamd_smtp_received_process_rdns (task,
						data,
						len,
						&rh->real_hostname)) {
					ret = TRUE;
				}
			}
		}
	}

	return ret;
}

static void
rspamd_smtp_received_process_from (struct rspamd_task *task,
								   struct rspamd_received_part *rpart,
								   struct received_header *rh)
{
	if (rpart->dlen > 0) {
		/* We have seen multiple cases:
		 * - [ip] (hostname/unknown [real_ip])
		 * - helo (hostname/unknown [real_ip])
		 * - [ip]
		 * - hostname
		 * - hostname ([ip]:port helo=xxx)
		 * Maybe more...
		 */
		gboolean seen_ip_in_data = FALSE, seen_rdns_in_comment = FALSE;

		if (rpart->head_comment && rpart->head_comment->dlen > 0) {
			/* We can have info within comment as part of RFC */
			seen_rdns_in_comment = rspamd_smtp_received_process_host_tcpinfo (
					task, rh,
					rpart->head_comment->data, rpart->head_comment->dlen);
		}

		if (!rh->real_ip) {
			if (rpart->data[0] == '[') {
				/* No comment, just something that looks like SMTP IP */
				const gchar *brace_pos = memchr (rpart->data, ']', rpart->dlen);
				rspamd_inet_addr_t *addr;

				if (brace_pos) {
					addr = rspamd_parse_inet_address_pool (rpart->data + 1,
							brace_pos - rpart->data - 1,
							task->task_pool);

					if (addr) {
						seen_ip_in_data = TRUE;
						rh->addr = addr;
						rh->real_ip = rspamd_mempool_strdup (task->task_pool,
								rspamd_inet_address_to_string (addr));
						rh->from_ip = rh->real_ip;
					}
				}
			} else if (g_ascii_isxdigit (rpart->data[0])) {
				/* Try to parse IP address */
				rspamd_inet_addr_t *addr;
				addr = rspamd_parse_inet_address_pool (rpart->data,
						rpart->dlen, task->task_pool);
				if (addr) {
					seen_ip_in_data = TRUE;
					rh->addr = addr;
					rh->real_ip = rspamd_mempool_strdup (task->task_pool,
							rspamd_inet_address_to_string (addr));
					rh->from_ip = rh->real_ip;
				}
			}
		}

		if (!seen_ip_in_data) {
			if (rh->real_ip) {
				/* Get anounced hostname (usually helo) */
				rspamd_smtp_received_process_rdns (task,
						rpart->data,
						rpart->dlen,
						&rh->from_hostname);
			}
			else {
				rspamd_smtp_received_process_host_tcpinfo (task,
						rh, rpart->data, rpart->dlen);
			}
		}
	}
	else {
		/* rpart->dlen = 0 */

		if (rpart->head_comment && rpart->head_comment->dlen > 0) {
			rspamd_smtp_received_process_host_tcpinfo (task,
					rh,
					rpart->head_comment->data,
					rpart->head_comment->dlen);
		}
	}
}

int
rspamd_smtp_received_parse (struct rspamd_task *task,
							const char *data,
							size_t len,
							struct received_header *rh)
{
	goffset date_pos = -1;
	struct rspamd_received_part *head, *cur;
	rspamd_ftok_t t1, t2;

	head = rspamd_smtp_received_spill (task, data, len, &date_pos);

	if (head == NULL) {
		return -1;
	}

	rh->type = RSPAMD_RECEIVED_UNKNOWN;

	DL_FOREACH (head, cur) {
		switch (cur->type) {
		case RSPAMD_RECEIVED_PART_FROM:
			rspamd_smtp_received_process_from (task, cur, rh);
			break;
		case RSPAMD_RECEIVED_PART_BY:
			rspamd_smtp_received_process_rdns (task,
					cur->data,
					cur->dlen,
					&rh->by_hostname);
			break;
		case RSPAMD_RECEIVED_PART_WITH:
			t1.begin = cur->data;
			t1.len = cur->dlen;

			if (t1.len > 0) {
				RSPAMD_FTOK_ASSIGN (&t2, "smtp");

				if (rspamd_ftok_cmp (&t1, &t2) == 0) {
					rh->type = RSPAMD_RECEIVED_SMTP;
				}

				RSPAMD_FTOK_ASSIGN (&t2, "esmtp");

				if (rspamd_ftok_starts_with (&t1, &t2)) {
					/*
					 * esmtp, esmtps, esmtpsa
					 */
					if (t1.len == t2.len + 1) {
						if (t1.begin[t2.len] == 'a') {
							rh->type = RSPAMD_RECEIVED_ESMTPA;
							rh->flags |= RSPAMD_RECEIVED_FLAG_AUTHENTICATED;
						}
						else if (t1.begin[t2.len] == 's') {
							rh->type = RSPAMD_RECEIVED_ESMTPS;
							rh->flags |= RSPAMD_RECEIVED_FLAG_SSL;
						}
						continue;
					}
					else if (t1.len == t2.len + 2) {
						if (t1.begin[t2.len] == 's' &&
								t1.begin[t2.len + 1] == 'a') {
							rh->type = RSPAMD_RECEIVED_ESMTPSA;
							rh->flags |= RSPAMD_RECEIVED_FLAG_AUTHENTICATED;
							rh->flags |= RSPAMD_RECEIVED_FLAG_SSL;
						}
						continue;
					}
					else if (t1.len == t2.len) {
						rh->type = RSPAMD_RECEIVED_ESMTP;
						continue;
					}
				}

				RSPAMD_FTOK_ASSIGN (&t2, "lmtp");

				if (rspamd_ftok_cmp (&t1, &t2) == 0) {
					rh->type = RSPAMD_RECEIVED_LMTP;
					continue;
				}

				RSPAMD_FTOK_ASSIGN (&t2, "imap");

				if (rspamd_ftok_cmp (&t1, &t2) == 0) {
					rh->type = RSPAMD_RECEIVED_IMAP;
					continue;
				}

				RSPAMD_FTOK_ASSIGN (&t2, "local");

				if (rspamd_ftok_cmp (&t1, &t2) == 0) {
					rh->type = RSPAMD_RECEIVED_LOCAL;
					continue;
				}

				RSPAMD_FTOK_ASSIGN (&t2, "http");

				if (rspamd_ftok_starts_with (&t1, &t2)) {
					if (t1.len == t2.len + 1) {
						if (t1.begin[t2.len] == 's') {
							rh->type = RSPAMD_RECEIVED_HTTP;
							rh->flags |= RSPAMD_RECEIVED_FLAG_SSL;
						}
					}
					else if (t1.len == t2.len) {
						rh->type = RSPAMD_RECEIVED_HTTP;
					}

					continue;
				}
			}

			break;
		default:
			/* Do nothing */
			break;
		}
	}

	if (rh->real_ip && !rh->from_ip) {
		rh->from_ip = rh->real_ip;
	}

	if (rh->real_hostname && !rh->from_hostname) {
		rh->from_hostname = rh->real_hostname;
	}

	if (date_pos > 0 && date_pos < len) {
		rh->timestamp = rspamd_parse_smtp_date (data + date_pos,
				len - date_pos);
	}

	return 0;
}