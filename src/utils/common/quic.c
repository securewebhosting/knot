/*  Copyright (C) 2022 CZ.NIC, z.s.p.o. <knot-dns@labs.nic.cz>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <stddef.h>
#include "libknot/errcode.h"
#include "utils/common/quic.h"

int quic_params_copy(quic_params_t *dst, const quic_params_t *src)
{
	if (dst == NULL || src == NULL) {
		return KNOT_EINVAL;
	}

	dst->enable = src->enable;

	return KNOT_EOK;
}

void quic_params_clean(quic_params_t *params)
{
	if (params == NULL) {
		return;
	}

	params->enable = false;
}

#ifdef LIBNGTCP2

#include <assert.h>
#include <poll.h>
#include <gnutls/crypto.h>

#include <ngtcp2/ngtcp2_crypto.h>
#include <ngtcp2/ngtcp2_crypto_gnutls.h>

#include "libdnssec/error.h"
#include "libdnssec/random.h"

#include "libknot/xdp/tcp_iobuf.h"
#include "utils/common/params.h"

#define quic_ceil_duration_to_ms(x) (((x) + NGTCP2_MILLISECONDS - 1) / NGTCP2_MILLISECONDS)
#define quic_get_encryption_level(level) ngtcp2_crypto_gnutls_from_gnutls_record_encryption_level(level)
#define quic_send(ctx, sockfd, family) quic_send_data(ctx, sockfd, family, NULL, 0)
#define quic_timeout(ts, wait) (((ts) + NGTCP2_SECONDS * (wait)) <= quic_timestamp())

const gnutls_datum_t doq_alpn[] = {
	{
		.data = (unsigned char *)"doq",
		.size = 3
	},{
		.data = (unsigned char *)"doq-i12",
		.size = 7
	},{
		.data = (unsigned char *)"doq-i11",
		.size = 7
	},{
		.data = (unsigned char *)"doq-i03",
		.size = 7
	}
};

static void set_application_error(quic_ctx_t *ctx, uint64_t error, uint8_t *reason, size_t reasonlen)
{
	ctx->last_err = (ngtcp2_connection_close_error){
		.type = NGTCP2_CONNECTION_CLOSE_ERROR_CODE_TYPE_APPLICATION,
		.error_code = error,
		.reason = reason,
		.reasonlen = reasonlen
	};
}


static void set_transport_error(quic_ctx_t *ctx, uint64_t error, uint8_t *reason, size_t reasonlen)
{
	ctx->last_err = (ngtcp2_connection_close_error){
		.type = NGTCP2_CONNECTION_CLOSE_ERROR_CODE_TYPE_TRANSPORT,
		.error_code = error,
		.reason = reason,
		.reasonlen = reasonlen
	};
}

static int recv_stream_data_cb(ngtcp2_conn *conn, uint32_t flags,
        int64_t stream_id, uint64_t offset, const uint8_t *data,
        size_t datalen, void *user_data, void *stream_user_data)
{
	(void)conn;
	(void)flags;
	(void)offset;
	(void)stream_user_data;

	quic_ctx_t *ctx = (quic_ctx_t *)user_data;

	if (stream_id != ctx->stream.id) {
		return 0;
	}

	struct iovec in = {
		.iov_base = (uint8_t *)data,
		.iov_len = datalen
	};

	int ret = tcp_inbuf_update(&ctx->stream.in_storage, in,
	                &ctx->stream.out_storage, &ctx->stream.out_storage_len,
	                &ctx->stream.out_storage_total);
	if (ret != KNOT_EOK) {
		return NGTCP2_ERR_CALLBACK_FAILURE;
	}

	ctx->timestamp = quic_timestamp();
	ctx->stream.out_storage_it = 0;
	return 0;
}

static int stream_open_cb(ngtcp2_conn *conn, int64_t stream_id,
        void *user_data)
{
	(void)conn;

	quic_ctx_t *ctx = (quic_ctx_t *)user_data;
	// Error - is NOT client initialized bidirectional stream
	if (stream_id % 4 != 0) {
		unsigned char message[] = "Server can't open streams.";
		set_application_error(ctx, DOQ_PROTOCOL_ERROR, message,
		                      sizeof(message) - 1);
		return NGTCP2_ERR_CALLBACK_FAILURE;
	}
	return KNOT_EOK;
}


static int acked_stream_data_offset_cb(ngtcp2_conn *conn, int64_t stream_id,
        uint64_t offset, uint64_t datalen, void *user_data,
        void *stream_user_data)
{
	(void)conn;
	(void)offset;
	(void)stream_user_data;

	quic_ctx_t *ctx = (quic_ctx_t *)user_data;
	if (ctx->stream.id == stream_id) {
		ctx->stream.sent -= datalen;
	}
	return KNOT_EOK;
}

static int stream_close_cb(ngtcp2_conn *conn, uint32_t flags, int64_t stream_id,
        uint64_t app_error_code, void *user_data, void *stream_user_data)
{
	(void)conn;
	(void)flags;
	(void)app_error_code;
	(void)stream_user_data;

	quic_ctx_t *ctx = (quic_ctx_t *)user_data;
	if (ctx && stream_id == ctx->stream.id) {
		ctx->stream.id = -1;
	}
	return KNOT_EOK;
}

static int quic_open_bidi_stream(quic_ctx_t *ctx)
{
	if (ctx->stream.id != -1) {
		return KNOT_EOK;
	}

	int ret = ngtcp2_conn_open_bidi_stream(ctx->conn, &ctx->stream.id, NULL);
	switch (ret) {
		case 0:
			return KNOT_EOK;
		case NGTCP2_ERR_STREAM_ID_BLOCKED:
			return KNOT_EBUSY;
		case NGTCP2_ERR_NOMEM:
			return KNOT_ENOMEM;
		default:
			assert(0);
			return KNOT_ERROR;

	}
}

static int extend_max_bidi_streams_cb(ngtcp2_conn *conn, uint64_t max_streams,
        void *user_data)
{
	(void)conn;

	quic_ctx_t *ctx = (quic_ctx_t *)user_data;
	if(max_streams > 0) {
		int ret = quic_open_bidi_stream(ctx);
		if (ret != KNOT_EOK) {
			return NGTCP2_ERR_CALLBACK_FAILURE;
		}
	}
	return 0;
}

static void rand_cb(uint8_t *dest, size_t destlen,
        const ngtcp2_rand_ctx *rand_ctx)
{
	(void)rand_ctx;

	dnssec_random_buffer(dest, destlen);
}

static int get_new_connection_id_cb(ngtcp2_conn *conn, ngtcp2_cid *cid,
        uint8_t *token, size_t cidlen, void *user_data)
{
	(void)conn;

	quic_ctx_t *ctx = (quic_ctx_t *)user_data;

	if (dnssec_random_buffer(cid->data, cidlen) != DNSSEC_EOK) {
		return NGTCP2_ERR_CALLBACK_FAILURE;
	}
	cid->datalen = cidlen;

	if (ngtcp2_crypto_generate_stateless_reset_token(token, ctx->secret,
	                sizeof(ctx->secret), cid) != 0) {
		return NGTCP2_ERR_CALLBACK_FAILURE;
	}

	return 0;
}

static int handshake_confirmed_cb(ngtcp2_conn *conn, void *user_data)
{
	(void)conn;

	quic_ctx_t *ctx = (quic_ctx_t *)user_data;
	ctx->state = CONNECTED;
	return 0;
}

static const ngtcp2_callbacks quic_client_callbacks = {
	ngtcp2_crypto_client_initial_cb,
	NULL, /* recv_client_initial */
	ngtcp2_crypto_recv_crypto_data_cb,
	NULL, /* handshake_completed */
	NULL, /* recv_version_negotiation */
	ngtcp2_crypto_encrypt_cb,
	ngtcp2_crypto_decrypt_cb,
	ngtcp2_crypto_hp_mask_cb,
	recv_stream_data_cb,
	acked_stream_data_offset_cb,
	stream_open_cb,
	stream_close_cb,
	NULL, /* recv_stateless_reset */
	ngtcp2_crypto_recv_retry_cb,
	extend_max_bidi_streams_cb,
	NULL, /* extend_max_local_streams_uni */
	rand_cb,
	get_new_connection_id_cb,
	NULL, /* remove_connection_id */
	ngtcp2_crypto_update_key_cb,
	NULL, /* path_validation */
	NULL, /* select_preferred_address */
	NULL, /* stream_reset */
	NULL, /* extend_max_remote_streams_bidi */
	NULL, /* extend_max_remote_streams_uni */
	NULL, /* extend_max_stream_data */
	NULL, /* dcid_status */
	handshake_confirmed_cb,
	NULL, /* recv_new_token */
	ngtcp2_crypto_delete_crypto_aead_ctx_cb,
	ngtcp2_crypto_delete_crypto_cipher_ctx_cb,
	NULL, /* recv_datagram */
	NULL, /* ack_datagram */
	NULL, /* lost_datagram */
	ngtcp2_crypto_get_path_challenge_data_cb,
	NULL, /* stream_stop_sending */
};

static int hook_func(gnutls_session_t session, unsigned int htype,
                     unsigned when, unsigned int incoming,
                     const gnutls_datum_t *msg)
{
	(void)session;
	(void)htype;
	(void)when;
	(void)incoming;
        (void)msg;

	return GNUTLS_E_SUCCESS;
}

static int secret_func(gnutls_session_t session,
        gnutls_record_encryption_level_t gtls_level, const void *rx_secret,
        const void *tx_secret, size_t secretlen)
{
	quic_ctx_t *ctx = (quic_ctx_t *)gnutls_session_get_ptr(session);
	ngtcp2_crypto_level level = quic_get_encryption_level(gtls_level);

	if (rx_secret) {
		int ret = ngtcp2_crypto_derive_and_install_rx_key(ctx->conn,
		                NULL, NULL, NULL, level, rx_secret, secretlen);
		if (ret != 0) {
			return -1;
		}

		if (level == NGTCP2_CRYPTO_LEVEL_APPLICATION) {
			quic_open_bidi_stream(ctx);
		}
	}

	if (tx_secret &&
	    ngtcp2_crypto_derive_and_install_tx_key(ctx->conn, NULL, NULL,
	                NULL, level, tx_secret, secretlen) != 0) {
		return -1;
	}

	return GNUTLS_E_SUCCESS;
}

static int read_func(gnutls_session_t session,
        gnutls_record_encryption_level_t gtls_level,
        gnutls_handshake_description_t htype, const void *data, size_t datalen)
{
	if (htype == GNUTLS_HANDSHAKE_CHANGE_CIPHER_SPEC) {
		return GNUTLS_E_SUCCESS;
	}

	quic_ctx_t *ctx = (quic_ctx_t *)gnutls_session_get_ptr(session);
	if (ngtcp2_conn_submit_crypto_data(ctx->conn,
	        quic_get_encryption_level(gtls_level), (const uint8_t *)data,
	        datalen) != 0) {
		return -1;
	}

	return GNUTLS_E_SUCCESS;
}

static int alert_read_func(gnutls_session_t session,
        gnutls_record_encryption_level_t gtls_level,
        gnutls_alert_level_t alert_level, gnutls_alert_description_t alert)
{
	(void)gtls_level;
	(void)alert_level;

	quic_ctx_t *ctx = (quic_ctx_t *)gnutls_session_get_ptr(session);
	set_transport_error(ctx, NGTCP2_CRYPTO_ERROR | alert, NULL, 0);

	return GNUTLS_E_SUCCESS;
}

static int set_remote_transport_params(ngtcp2_conn *conn, const uint8_t *data,
        size_t datalen)
{
	int ret = KNOT_EOK;
	ngtcp2_transport_params params;
	ret = ngtcp2_decode_transport_params(&params,
	        NGTCP2_TRANSPORT_PARAMS_TYPE_ENCRYPTED_EXTENSIONS, data,
	        datalen);
	if (ret != 0)	{
		return ret;
	}

	return ngtcp2_conn_set_remote_transport_params(conn, &params);
}

static int tp_recv_func(gnutls_session_t session, const uint8_t *data,
        size_t datalen)
{
	quic_ctx_t *ctx = (quic_ctx_t *)gnutls_session_get_ptr(session);
	if (set_remote_transport_params(ctx->conn, data, datalen) != KNOT_EOK) {
		return -1;
	}

	return GNUTLS_E_SUCCESS;
}

static int append_local_transport_params(ngtcp2_conn *conn,
        gnutls_buffer_t extdata)
{
	ngtcp2_transport_params params;
	uint8_t buf[64];

	ngtcp2_conn_get_local_transport_params(conn, &params);
	ngtcp2_ssize nwrite = ngtcp2_encode_transport_params(buf, sizeof(buf),
	        NGTCP2_TRANSPORT_PARAMS_TYPE_CLIENT_HELLO, &params);
	if (nwrite < 0) {
		return -1;
	}

	return gnutls_buffer_append_data(extdata, buf, (size_t)nwrite);
}

static int tp_send_func(gnutls_session_t session, gnutls_buffer_t extdata)
{
	quic_ctx_t *ctx = (quic_ctx_t *)gnutls_session_get_ptr(session);
	return append_local_transport_params(ctx->conn, extdata);
}


static int quic_setup_tls(tls_ctx_t *tls_ctx)
{
	gnutls_handshake_set_hook_function(tls_ctx->session,
	        GNUTLS_HANDSHAKE_ANY, GNUTLS_HOOK_POST, hook_func);
	gnutls_handshake_set_secret_function(tls_ctx->session, secret_func);
	gnutls_handshake_set_read_function(tls_ctx->session, read_func);
	gnutls_alert_set_read_function(tls_ctx->session, alert_read_func);
	return gnutls_session_ext_register(tls_ctx->session,
	        "QUIC Transport Parameters",
	        NGTCP2_TLSEXT_QUIC_TRANSPORT_PARAMETERS_V1, GNUTLS_EXT_TLS,
	        tp_recv_func, tp_send_func, NULL, NULL, NULL,
	        GNUTLS_EXT_FLAG_TLS | GNUTLS_EXT_FLAG_CLIENT_HELLO |
	        GNUTLS_EXT_FLAG_EE);
}

static int quic_send_data(quic_ctx_t *ctx, int sockfd, int family, ngtcp2_vec *datav, size_t datavlen)
{
	uint8_t enc_buf[MAX_PACKET_SIZE];
	struct iovec msg_iov = {
		.iov_base = enc_buf,
		.iov_len = 0
	};
	struct msghdr msg = {
		.msg_iov = &msg_iov,
		.msg_iovlen = 1
	};
	uint64_t ts = quic_timestamp();

	while(1) {
		int64_t stream = -1;
		uint32_t flags = NGTCP2_WRITE_STREAM_FLAG_NONE;
		if (datavlen != 0) {
			flags = NGTCP2_WRITE_STREAM_FLAG_FIN;
			stream = ctx->stream.id;
		}
		ngtcp2_ssize nwrite = ngtcp2_conn_writev_stream(ctx->conn,
		                (ngtcp2_path *)ngtcp2_conn_get_path(ctx->conn),
		                &ctx->pi, enc_buf, sizeof(enc_buf), NULL,
		                flags, stream, datav, datavlen, ts);
		if (nwrite < 0) {
			switch(nwrite) {
			case NGTCP2_ERR_STREAM_DATA_BLOCKED:
				return KNOT_EOK; //TODO maybe error instead OK
			case NGTCP2_ERR_NOMEM:
				return KNOT_ENOMEM;
			case NGTCP2_ERR_WRITE_MORE:
				assert(0);
				continue;
			case NGTCP2_ERR_STREAM_SHUT_WR:
				ctx->stream.id = -1;
				// [[ fallthrough ]]
			default:
				set_transport_error(ctx,
				        ngtcp2_err_infer_quic_transport_error_code(nwrite),
					NULL, 0);
				return KNOT_NET_ESEND;	
			}
		} else if (nwrite == 0) {
			ngtcp2_conn_update_pkt_tx_time(ctx->conn, ts);
			return KNOT_EOK;
		}
		datav = NULL;
		datavlen = 0;

		msg_iov.iov_len = (size_t)nwrite;

		int ret = quic_set_enc(sockfd, family, ctx->pi.ecn);
		if (ret != KNOT_EOK) {
			return ret;
		}

		if (sendmsg(sockfd, &msg, 0) == -1) {
			return KNOT_NET_ESEND;
		}
	}
	return KNOT_EOK;
}

static int quic_recv(quic_ctx_t *ctx, int sockfd)
{
	uint8_t enc_buf[MAX_PACKET_SIZE];
	uint8_t msg_ctrl[CMSG_SPACE(sizeof(uint8_t))];
	struct sockaddr_in6 from = { 0 };
	struct iovec msg_iov = {
		.iov_base = enc_buf,
		.iov_len = sizeof(enc_buf)
	};
	struct msghdr msg = {
		.msg_name = &from,
		.msg_namelen = sizeof(from),
		.msg_iov = &msg_iov,
		.msg_iovlen = 1,
		.msg_control = msg_ctrl,
		.msg_controllen = sizeof(msg_ctrl),
		.msg_flags = 0
	};

	ssize_t nwrite = recvmsg(sockfd, &msg, 0);
	if (nwrite <= 0) {
		return knot_map_errno();
	}
	ctx->pi.ecn = quic_get_ecn(&msg, from.sin6_family);
	if (ctx->pi.ecn == 0 && errno != 0) {
		return knot_map_errno();
	}

	int ret = ngtcp2_conn_read_pkt(ctx->conn,
	                               ngtcp2_conn_get_path(ctx->conn),
	                               &ctx->pi, enc_buf, nwrite,
	                               quic_timestamp());
	if (ret != 0) {
		if (nwrite == NGTCP2_ERR_DROP_CONN) {
			ctx->state = CLOSED;
		} else if (ngtcp2_err_is_fatal(nwrite)) {
			set_transport_error(ctx, nwrite, NULL, 0);
			// TODO should immediately close
		}
		return KNOT_NET_ERECV;
	}
	return KNOT_EOK;
}

static int quic_respcpy(quic_ctx_t *ctx, uint8_t *buf, const size_t buf_len)
{
	assert(ctx && buf && buf_len > 0);
	if (ctx->stream.out_storage && ctx->stream.out_storage_it < ctx->stream.out_storage_len) {
		size_t len = ctx->stream.out_storage[ctx->stream.out_storage_it].iov_len;
		if (buf_len < len) {
			return KNOT_ENOMEM;
		}
		memcpy(buf, ctx->stream.out_storage[ctx->stream.out_storage_it].iov_base, len);
		ctx->stream.out_storage_it++;
		if (ctx->stream.out_storage_it == ctx->stream.out_storage_len) {
			free(ctx->stream.out_storage);
			ctx->stream.out_storage = NULL;
			ctx->stream.out_storage_len = 0;
		}
		return len;
	}
	return 0;
}

uint64_t quic_timestamp(void)
{
	struct timespec ts;
	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
		return 0;
	}

	return (uint64_t)ts.tv_sec * NGTCP2_SECONDS + (uint64_t)ts.tv_nsec;
}

int quic_generate_secret(uint8_t *buf, size_t buflen)
{
	assert(buf != NULL && buflen > 0 && buflen <= 32);
	uint8_t rand[16], hash[32];
	int ret = dnssec_random_buffer(rand, sizeof(rand));
	if (ret != DNSSEC_EOK) {
		return ret;
	}
	ret = gnutls_hash_fast(GNUTLS_DIG_SHA256, rand, sizeof(rand), hash);
	if (ret != 0) {
		return ret;
	}
	memcpy(buf, hash, buflen);
	return KNOT_EOK;
}

int quic_set_enc(int sockfd, int family, uint32_t ecn)
{
	switch (family) {
	case AF_INET:
		if (setsockopt(sockfd, IPPROTO_IP, IP_TOS, &ecn,
		               (socklen_t)sizeof(ecn)) == -1) {
			return knot_map_errno();
		}
		break;
	case AF_INET6:
		if (setsockopt(sockfd, IPPROTO_IPV6, IPV6_TCLASS, &ecn,
		               (socklen_t)sizeof(ecn)) == -1) {
			return knot_map_errno();
		}
		break;
	default:
		return KNOT_ENOTSUP;
	}
	return KNOT_EOK;
}

uint32_t quic_get_ecn(struct msghdr *msg, const int family)
{
	errno = 0;
	switch (family) {
	case AF_INET:
		for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(msg); cmsg;
		     cmsg = CMSG_NXTHDR(msg, cmsg)) {
			if (cmsg->cmsg_level == IPPROTO_IP &&
			    cmsg->cmsg_type == IP_TOS && cmsg->cmsg_len) {
				return *(uint8_t *)CMSG_DATA(cmsg);
			}
		}
		errno = ENOENT;
		break;
	case AF_INET6:
		for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(msg); cmsg;
		     cmsg = CMSG_NXTHDR(msg, cmsg)) {
			if (cmsg->cmsg_level == IPPROTO_IPV6 &&
			    cmsg->cmsg_type == IPV6_TCLASS && cmsg->cmsg_len) {
				return *(uint8_t *)CMSG_DATA(cmsg);
			}
		}
		errno = ENOENT;	
		break;
	default:
		errno = ENOTSUP;
	}

	return 0;
}

int quic_ctx_init(quic_ctx_t *ctx, tls_ctx_t *tls_ctx,
        const quic_params_t *params)
{
	if (ctx == NULL || tls_ctx == NULL || params == NULL) {
		return KNOT_EINVAL;
	}

	ctx->params = *params;
	ctx->tls = tls_ctx;
	ctx->state = OPENING;
	ctx->stream.id = -1;
	ctx->timestamp = quic_timestamp();
	set_application_error(ctx, DOQ_NO_ERROR, NULL, 0);
	if (quic_generate_secret(ctx->secret, sizeof(ctx->secret)) != KNOT_EOK) {
		return KNOT_ENOMEM;
	}

	return KNOT_EOK;
}

int quic_ctx_connect(quic_ctx_t *ctx, int sockfd, struct addrinfo *dst_addr)
{
	if (connect(sockfd, (const struct sockaddr *)(dst_addr->ai_addr),
	            sizeof(struct sockaddr_storage)) != 0) {
		return knot_map_errno();
	}

	ngtcp2_cid dcid, scid;
	scid.datalen = 17;
	int ret = dnssec_random_buffer(scid.data, scid.datalen);
	if (ret != DNSSEC_EOK) {
		return ret;
	}
	dcid.datalen = 18;
	ret = dnssec_random_buffer(dcid.data, dcid.datalen);
	if (ret != DNSSEC_EOK) {
		return ret;
	}

	ngtcp2_settings settings;
	ngtcp2_settings_default(&settings);
	settings.initial_ts = quic_timestamp();

	ngtcp2_transport_params params;
	ngtcp2_transport_params_default(&params);
	params.initial_max_streams_uni = 0;
	params.initial_max_streams_bidi = 0;
	params.initial_max_stream_data_bidi_local = MAX_PACKET_SIZE;
	params.initial_max_data = MAX_PACKET_SIZE;

	struct sockaddr_in6 src_addr;
	socklen_t src_addr_len = sizeof(src_addr);
	ret = getsockname(sockfd, (struct sockaddr *)&src_addr, &src_addr_len);
	if (ret < 0) {
		return knot_map_errno();
	}
	ngtcp2_path path = {
		.local = {
			.addrlen = src_addr_len,
			.addr = (struct sockaddr *)&src_addr
		},
		.remote = {
			.addrlen = sizeof(*(dst_addr->ai_addr)),
			.addr = (struct sockaddr *)(dst_addr->ai_addr)
		},
		.user_data = NULL
	};

	if (ngtcp2_conn_client_new(&ctx->conn, &dcid, &scid, &path,
	                           NGTCP2_PROTO_VER_V1, &quic_client_callbacks,
	                           &settings, &params, NULL, ctx) != 0) {
		return KNOT_NET_ECONNECT;
	}

	ret = quic_setup_tls(ctx->tls);
	if (ret != KNOT_EOK) {
		// TODO shouldn't be deinit at tls_ctx_close/disconnect??
		gnutls_deinit(ctx->tls->session);
		return KNOT_NET_ECONNECT;
	}
	gnutls_session_set_ptr(ctx->tls->session, ctx);
	ngtcp2_conn_set_tls_native_handle(ctx->conn, ctx->tls->session);

	// Initialize poll descriptor structure.
	struct pollfd pfd = {
		.fd = sockfd,
		.events = POLLIN,
		.revents = 0,
	};
	ctx->tls->sockfd = sockfd;

	while(ctx->state != CONNECTED) {
		if (quic_timeout(ctx->timestamp, ctx->tls->wait)) {
			return KNOT_NET_ETIMEOUT;
		}
		ret = quic_send(ctx, sockfd, dst_addr->ai_family);
		if (ret != KNOT_EOK) {
			return ret;
		}

		ngtcp2_conn_get_remote_transport_params(ctx->conn, &params);
		int timeout = quic_ceil_duration_to_ms(params.max_ack_delay);
		ret = poll(&pfd, 1, timeout);
		if (ret < 0) {
			return knot_map_errno();
		} else if (ret == 0) {
			continue;
		}

		ret = quic_recv(ctx, sockfd);
		if (ret != KNOT_EOK) {
			return ret;
		}
	}

	return KNOT_EOK;
}

int quic_send_dns_query(quic_ctx_t *ctx, int sockfd, struct addrinfo *srv,
        const uint8_t *buf, const size_t buf_len)
{
	if (ctx == NULL || buf == NULL) {
		return KNOT_NET_ESEND;
	}

	ngtcp2_transport_params params;
	uint16_t query_length = htons(buf_len);
	ngtcp2_vec datav[] = {
		{
			.base = (uint8_t *)&query_length,
			.len = sizeof(uint16_t)
		},{
			.base = (uint8_t *)buf,
			.len = buf_len
		}
	};
	size_t datavlen = sizeof(datav)/sizeof(*datav);
	ngtcp2_vec *pdatav = datav;

	struct pollfd pfd = {
		.fd = sockfd,
		.events = POLLIN,
		.revents = 0,
	};

	ctx->stream.sent += buf_len + sizeof(uint16_t);
	while (ctx->stream.sent) {
		if (quic_timeout(ctx->timestamp, ctx->tls->wait)) {
			return KNOT_NET_ETIMEOUT;
		}
		int ret = quic_send_data(ctx, sockfd, srv->ai_family, pdatav, datavlen);
		if (ret != KNOT_EOK) {
			return ret;
		}
		pdatav = NULL;
		datavlen = 0;

		ngtcp2_conn_get_remote_transport_params(ctx->conn, &params);
		int timeout = quic_ceil_duration_to_ms(params.max_ack_delay);
		ret = poll(&pfd, 1, timeout);
		if (ret < 0) {
			return knot_map_errno();
		} else if (ret == 0) {
			continue;
		}
		ret = quic_recv(ctx, sockfd);
		if (ret != KNOT_EOK) {
			return ret;
		}
	}

	return KNOT_EOK;
}

int quic_recv_dns_response(quic_ctx_t *ctx, uint8_t *buf, const size_t buf_len,
        struct addrinfo *srv)
{
	if (ctx == NULL || ctx->tls == NULL || buf == NULL) {
		return KNOT_EINVAL;
	}

	int ret = quic_respcpy(ctx, buf, buf_len);
	if (ret != 0) {
		return ret;
	}

	int sockfd = ctx->tls->sockfd;

	struct pollfd pfd = {
		.fd = sockfd,
		.events = POLLIN,
		.revents = 0,
	};

	ngtcp2_transport_params params;
	while (!quic_timeout(ctx->timestamp, ctx->tls->wait)) {
		ngtcp2_conn_get_remote_transport_params(ctx->conn, &params);
		int timeout = quic_ceil_duration_to_ms(params.max_ack_delay);
		ret = poll(&pfd, 1, timeout);
		if (ret < 0) {
			return knot_map_errno();
		} else if (ret == 0) {
			goto send;
		}

		ret = quic_recv(ctx, sockfd);
		if (ret != KNOT_EOK) {
			return ret;
		}
		ret = quic_respcpy(ctx, buf, buf_len);
		if (ret != 0) {
			return ret;
		}

		send: ret = quic_send(ctx, sockfd, srv->ai_family);
		if (ret != KNOT_EOK) {
			return ret;
		}
	}

	return KNOT_NET_ETIMEOUT;
}

#endif