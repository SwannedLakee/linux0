/* Copyright (c) 2018, Mellanox Technologies All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <net/tls.h>
#include <crypto/aead.h>
#include <crypto/scatterwalk.h>
#include <net/ip6_checksum.h>
#include <linux/skbuff_ref.h>

#include "tls.h"

static int tls_enc_record(struct aead_request *aead_req,
			  struct crypto_aead *aead, char *aad,
			  char *iv, __be64 rcd_sn,
			  struct scatter_walk *in,
			  struct scatter_walk *out, int *in_len,
			  struct tls_prot_info *prot)
{
	unsigned char buf[TLS_HEADER_SIZE + TLS_MAX_IV_SIZE];
	const struct tls_cipher_desc *cipher_desc;
	struct scatterlist sg_in[3];
	struct scatterlist sg_out[3];
	unsigned int buf_size;
	u16 len;
	int rc;

	cipher_desc = get_cipher_desc(prot->cipher_type);
	DEBUG_NET_WARN_ON_ONCE(!cipher_desc || !cipher_desc->offloadable);

	buf_size = TLS_HEADER_SIZE + cipher_desc->iv;
	len = min_t(int, *in_len, buf_size);

	memcpy_from_scatterwalk(buf, in, len);
	memcpy_to_scatterwalk(out, buf, len);

	*in_len -= len;
	if (!*in_len)
		return 0;

	len = buf[4] | (buf[3] << 8);
	len -= cipher_desc->iv;

	tls_make_aad(aad, len - cipher_desc->tag, (char *)&rcd_sn, buf[0], prot);

	memcpy(iv + cipher_desc->salt, buf + TLS_HEADER_SIZE, cipher_desc->iv);

	sg_init_table(sg_in, ARRAY_SIZE(sg_in));
	sg_init_table(sg_out, ARRAY_SIZE(sg_out));
	sg_set_buf(sg_in, aad, TLS_AAD_SPACE_SIZE);
	sg_set_buf(sg_out, aad, TLS_AAD_SPACE_SIZE);
	scatterwalk_get_sglist(in, sg_in + 1);
	scatterwalk_get_sglist(out, sg_out + 1);

	*in_len -= len;
	if (*in_len < 0) {
		*in_len += cipher_desc->tag;
		/* the input buffer doesn't contain the entire record.
		 * trim len accordingly. The resulting authentication tag
		 * will contain garbage, but we don't care, so we won't
		 * include any of it in the output skb
		 * Note that we assume the output buffer length
		 * is larger then input buffer length + tag size
		 */
		if (*in_len < 0)
			len += *in_len;

		*in_len = 0;
	}

	if (*in_len) {
		scatterwalk_skip(in, len);
		scatterwalk_skip(out, len);
	}

	len -= cipher_desc->tag;
	aead_request_set_crypt(aead_req, sg_in, sg_out, len, iv);

	rc = crypto_aead_encrypt(aead_req);

	return rc;
}

static void tls_init_aead_request(struct aead_request *aead_req,
				  struct crypto_aead *aead)
{
	aead_request_set_tfm(aead_req, aead);
	aead_request_set_ad(aead_req, TLS_AAD_SPACE_SIZE);
}

static struct aead_request *tls_alloc_aead_request(struct crypto_aead *aead,
						   gfp_t flags)
{
	unsigned int req_size = sizeof(struct aead_request) +
		crypto_aead_reqsize(aead);
	struct aead_request *aead_req;

	aead_req = kzalloc(req_size, flags);
	if (aead_req)
		tls_init_aead_request(aead_req, aead);
	return aead_req;
}

static int tls_enc_records(struct aead_request *aead_req,
			   struct crypto_aead *aead, struct scatterlist *sg_in,
			   struct scatterlist *sg_out, char *aad, char *iv,
			   u64 rcd_sn, int len, struct tls_prot_info *prot)
{
	struct scatter_walk out, in;
	int rc;

	scatterwalk_start(&in, sg_in);
	scatterwalk_start(&out, sg_out);

	do {
		rc = tls_enc_record(aead_req, aead, aad, iv,
				    cpu_to_be64(rcd_sn), &in, &out, &len, prot);
		rcd_sn++;

	} while (rc == 0 && len);

	return rc;
}

/* Can't use icsk->icsk_af_ops->send_check here because the ip addresses
 * might have been changed by NAT.
 */
static void update_chksum(struct sk_buff *skb, int headln)
{
	struct tcphdr *th = tcp_hdr(skb);
	int datalen = skb->len - headln;
	const struct ipv6hdr *ipv6h;
	const struct iphdr *iph;

	/* We only changed the payload so if we are using partial we don't
	 * need to update anything.
	 */
	if (likely(skb->ip_summed == CHECKSUM_PARTIAL))
		return;

	skb->ip_summed = CHECKSUM_PARTIAL;
	skb->csum_start = skb_transport_header(skb) - skb->head;
	skb->csum_offset = offsetof(struct tcphdr, check);

	if (skb->sk->sk_family == AF_INET6) {
		ipv6h = ipv6_hdr(skb);
		th->check = ~csum_ipv6_magic(&ipv6h->saddr, &ipv6h->daddr,
					     datalen, IPPROTO_TCP, 0);
	} else {
		iph = ip_hdr(skb);
		th->check = ~csum_tcpudp_magic(iph->saddr, iph->daddr, datalen,
					       IPPROTO_TCP, 0);
	}
}

static void complete_skb(struct sk_buff *nskb, struct sk_buff *skb, int headln)
{
	struct sock *sk = skb->sk;
	int delta;

	skb_copy_header(nskb, skb);

	skb_put(nskb, skb->len);
	memcpy(nskb->data, skb->data, headln);

	nskb->destructor = skb->destructor;
	nskb->sk = sk;
	skb->destructor = NULL;
	skb->sk = NULL;

	update_chksum(nskb, headln);

	/* sock_efree means skb must gone through skb_orphan_partial() */
	if (nskb->destructor == sock_efree)
		return;

	delta = nskb->truesize - skb->truesize;
	if (likely(delta < 0))
		WARN_ON_ONCE(refcount_sub_and_test(-delta, &sk->sk_wmem_alloc));
	else if (delta)
		refcount_add(delta, &sk->sk_wmem_alloc);
}

/* This function may be called after the user socket is already
 * closed so make sure we don't use anything freed during
 * tls_sk_proto_close here
 */

static int fill_sg_in(struct scatterlist *sg_in,
		      struct sk_buff *skb,
		      struct tls_offload_context_tx *ctx,
		      u64 *rcd_sn,
		      s32 *sync_size,
		      int *resync_sgs)
{
	int tcp_payload_offset = skb_tcp_all_headers(skb);
	int payload_len = skb->len - tcp_payload_offset;
	u32 tcp_seq = ntohl(tcp_hdr(skb)->seq);
	struct tls_record_info *record;
	unsigned long flags;
	int remaining;
	int i;

	spin_lock_irqsave(&ctx->lock, flags);
	record = tls_get_record(ctx, tcp_seq, rcd_sn);
	if (!record) {
		spin_unlock_irqrestore(&ctx->lock, flags);
		return -EINVAL;
	}

	*sync_size = tcp_seq - tls_record_start_seq(record);
	if (*sync_size < 0) {
		int is_start_marker = tls_record_is_start_marker(record);

		spin_unlock_irqrestore(&ctx->lock, flags);
		/* This should only occur if the relevant record was
		 * already acked. In that case it should be ok
		 * to drop the packet and avoid retransmission.
		 *
		 * There is a corner case where the packet contains
		 * both an acked and a non-acked record.
		 * We currently don't handle that case and rely
		 * on TCP to retransmit a packet that doesn't contain
		 * already acked payload.
		 */
		if (!is_start_marker)
			*sync_size = 0;
		return -EINVAL;
	}

	remaining = *sync_size;
	for (i = 0; remaining > 0; i++) {
		skb_frag_t *frag = &record->frags[i];

		__skb_frag_ref(frag);
		sg_set_page(sg_in + i, skb_frag_page(frag),
			    skb_frag_size(frag), skb_frag_off(frag));

		remaining -= skb_frag_size(frag);

		if (remaining < 0)
			sg_in[i].length += remaining;
	}
	*resync_sgs = i;

	spin_unlock_irqrestore(&ctx->lock, flags);
	if (skb_to_sgvec(skb, &sg_in[i], tcp_payload_offset, payload_len) < 0)
		return -EINVAL;

	return 0;
}

static void fill_sg_out(struct scatterlist sg_out[3], void *buf,
			struct tls_context *tls_ctx,
			struct sk_buff *nskb,
			int tcp_payload_offset,
			int payload_len,
			int sync_size,
			void *dummy_buf)
{
	const struct tls_cipher_desc *cipher_desc =
		get_cipher_desc(tls_ctx->crypto_send.info.cipher_type);

	sg_set_buf(&sg_out[0], dummy_buf, sync_size);
	sg_set_buf(&sg_out[1], nskb->data + tcp_payload_offset, payload_len);
	/* Add room for authentication tag produced by crypto */
	dummy_buf += sync_size;
	sg_set_buf(&sg_out[2], dummy_buf, cipher_desc->tag);
}

static struct sk_buff *tls_enc_skb(struct tls_context *tls_ctx,
				   struct scatterlist sg_out[3],
				   struct scatterlist *sg_in,
				   struct sk_buff *skb,
				   s32 sync_size, u64 rcd_sn)
{
	struct tls_offload_context_tx *ctx = tls_offload_ctx_tx(tls_ctx);
	int tcp_payload_offset = skb_tcp_all_headers(skb);
	int payload_len = skb->len - tcp_payload_offset;
	const struct tls_cipher_desc *cipher_desc;
	void *buf, *iv, *aad, *dummy_buf, *salt;
	struct aead_request *aead_req;
	struct sk_buff *nskb = NULL;
	int buf_len;

	aead_req = tls_alloc_aead_request(ctx->aead_send, GFP_ATOMIC);
	if (!aead_req)
		return NULL;

	cipher_desc = get_cipher_desc(tls_ctx->crypto_send.info.cipher_type);
	DEBUG_NET_WARN_ON_ONCE(!cipher_desc || !cipher_desc->offloadable);

	buf_len = cipher_desc->salt + cipher_desc->iv + TLS_AAD_SPACE_SIZE +
		  sync_size + cipher_desc->tag;
	buf = kmalloc(buf_len, GFP_ATOMIC);
	if (!buf)
		goto free_req;

	iv = buf;
	salt = crypto_info_salt(&tls_ctx->crypto_send.info, cipher_desc);
	memcpy(iv, salt, cipher_desc->salt);
	aad = buf + cipher_desc->salt + cipher_desc->iv;
	dummy_buf = aad + TLS_AAD_SPACE_SIZE;

	nskb = alloc_skb(skb_headroom(skb) + skb->len, GFP_ATOMIC);
	if (!nskb)
		goto free_buf;

	skb_reserve(nskb, skb_headroom(skb));

	fill_sg_out(sg_out, buf, tls_ctx, nskb, tcp_payload_offset,
		    payload_len, sync_size, dummy_buf);

	if (tls_enc_records(aead_req, ctx->aead_send, sg_in, sg_out, aad, iv,
			    rcd_sn, sync_size + payload_len,
			    &tls_ctx->prot_info) < 0)
		goto free_nskb;

	complete_skb(nskb, skb, tcp_payload_offset);

	/* validate_xmit_skb_list assumes that if the skb wasn't segmented
	 * nskb->prev will point to the skb itself
	 */
	nskb->prev = nskb;

free_buf:
	kfree(buf);
free_req:
	kfree(aead_req);
	return nskb;
free_nskb:
	kfree_skb(nskb);
	nskb = NULL;
	goto free_buf;
}

static struct sk_buff *tls_sw_fallback(struct sock *sk, struct sk_buff *skb)
{
	int tcp_payload_offset = skb_tcp_all_headers(skb);
	struct tls_context *tls_ctx = tls_get_ctx(sk);
	struct tls_offload_context_tx *ctx = tls_offload_ctx_tx(tls_ctx);
	int payload_len = skb->len - tcp_payload_offset;
	struct scatterlist *sg_in, sg_out[3];
	struct sk_buff *nskb = NULL;
	int sg_in_max_elements;
	int resync_sgs = 0;
	s32 sync_size = 0;
	u64 rcd_sn;

	/* worst case is:
	 * MAX_SKB_FRAGS in tls_record_info
	 * MAX_SKB_FRAGS + 1 in SKB head and frags.
	 */
	sg_in_max_elements = 2 * MAX_SKB_FRAGS + 1;

	if (!payload_len)
		return skb;

	sg_in = kmalloc_array(sg_in_max_elements, sizeof(*sg_in), GFP_ATOMIC);
	if (!sg_in)
		goto free_orig;

	sg_init_table(sg_in, sg_in_max_elements);
	sg_init_table(sg_out, ARRAY_SIZE(sg_out));

	if (fill_sg_in(sg_in, skb, ctx, &rcd_sn, &sync_size, &resync_sgs)) {
		/* bypass packets before kernel TLS socket option was set */
		if (sync_size < 0 && payload_len <= -sync_size)
			nskb = skb_get(skb);
		goto put_sg;
	}

	nskb = tls_enc_skb(tls_ctx, sg_out, sg_in, skb, sync_size, rcd_sn);

put_sg:
	while (resync_sgs)
		put_page(sg_page(&sg_in[--resync_sgs]));
	kfree(sg_in);
free_orig:
	if (nskb)
		consume_skb(skb);
	else
		kfree_skb(skb);
	return nskb;
}

struct sk_buff *tls_validate_xmit_skb(struct sock *sk,
				      struct net_device *dev,
				      struct sk_buff *skb)
{
	if (dev == rcu_dereference_bh(tls_get_ctx(sk)->netdev) ||
	    netif_is_bond_master(dev))
		return skb;

	return tls_sw_fallback(sk, skb);
}
EXPORT_SYMBOL_GPL(tls_validate_xmit_skb);

struct sk_buff *tls_validate_xmit_skb_sw(struct sock *sk,
					 struct net_device *dev,
					 struct sk_buff *skb)
{
	return tls_sw_fallback(sk, skb);
}

struct sk_buff *tls_encrypt_skb(struct sk_buff *skb)
{
	return tls_sw_fallback(skb->sk, skb);
}
EXPORT_SYMBOL_GPL(tls_encrypt_skb);

int tls_sw_fallback_init(struct sock *sk,
			 struct tls_offload_context_tx *offload_ctx,
			 struct tls_crypto_info *crypto_info)
{
	const struct tls_cipher_desc *cipher_desc;
	int rc;

	cipher_desc = get_cipher_desc(crypto_info->cipher_type);
	if (!cipher_desc || !cipher_desc->offloadable)
		return -EINVAL;

	offload_ctx->aead_send =
	    crypto_alloc_aead(cipher_desc->cipher_name, 0, CRYPTO_ALG_ASYNC);
	if (IS_ERR(offload_ctx->aead_send)) {
		rc = PTR_ERR(offload_ctx->aead_send);
		pr_err_ratelimited("crypto_alloc_aead failed rc=%d\n", rc);
		offload_ctx->aead_send = NULL;
		goto err_out;
	}

	rc = crypto_aead_setkey(offload_ctx->aead_send,
				crypto_info_key(crypto_info, cipher_desc),
				cipher_desc->key);
	if (rc)
		goto free_aead;

	rc = crypto_aead_setauthsize(offload_ctx->aead_send, cipher_desc->tag);
	if (rc)
		goto free_aead;

	return 0;
free_aead:
	crypto_free_aead(offload_ctx->aead_send);
err_out:
	return rc;
}
