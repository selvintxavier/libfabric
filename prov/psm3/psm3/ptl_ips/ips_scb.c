/*

  This file is provided under a dual BSD/GPLv2 license.  When using or
  redistributing this file, you may do so under either license.

  GPL LICENSE SUMMARY

  Copyright(c) 2015 Intel Corporation.

  This program is free software; you can redistribute it and/or modify
  it under the terms of version 2 of the GNU General Public License as
  published by the Free Software Foundation.

  This program is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  General Public License for more details.

  Contact Information:
  Intel Corporation, www.intel.com

  BSD LICENSE

  Copyright(c) 2015 Intel Corporation.

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions
  are met:

    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in
      the documentation and/or other materials provided with the
      distribution.
    * Neither the name of Intel Corporation nor the names of its
      contributors may be used to endorse or promote products derived
      from this software without specific prior written permission.

  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
  A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
  OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
  SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
  LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
  DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
  THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

*/

/* Copyright (c) 2003-2014 Intel Corporation. All rights reserved. */

#include "psm_user.h"
#include "psm2_hal.h"
#include "ips_proto.h"
#include "ips_scb.h"
#include "ips_proto_internal.h"

psm2_error_t
psm3_ips_scbctrl_init(const struct ips_proto *proto,
		 uint32_t numscb, uint32_t numbufs,
		 uint32_t imm_size, uint32_t bufsize,
		 ips_scbctrl_avail_callback_fn_t scb_avail_callback,
		 void *scb_avail_context, struct ips_scbctrl *scbc)
{
	int i;
	struct ips_scb *scb;
	size_t scb_size;
	size_t alloc_sz;
	uintptr_t base, imm_base;
	psm2_error_t err = PSM2_OK;
	psm2_ep_t ep = proto->ep;

	psmi_assert_always(numscb > 0);
	scbc->sbuf_num = scbc->sbuf_num_cur = numbufs;
	SLIST_INIT(&scbc->sbuf_free);
	scbc->sbuf_buf_size = bufsize;
	scbc->sbuf_buf_base = NULL;
	scbc->sbuf_buf_alloc = NULL;
	scbc->sbuf_buf_last = NULL;
	scbc->proto = (struct ips_proto *)proto;

	/* send buffers are not mandatory but when allocating them, make sure they
	 * are on a page boundary */
	if (numbufs > 0) {
		struct ips_scbbuf *sbuf;

		bufsize = PSMI_ALIGNUP(bufsize, 64);

		alloc_sz = numbufs * bufsize + PSMI_PAGESIZE;
		scbc->sbuf_buf_alloc =
		    psmi_calloc(ep, NETWORK_BUFFERS, 1, alloc_sz);
		if (scbc->sbuf_buf_alloc == NULL) {
			err = PSM2_NO_MEMORY;
			goto fail;
		}
		base = (uintptr_t) scbc->sbuf_buf_alloc;
		base = PSMI_ALIGNUP(base, PSMI_PAGESIZE);
		scbc->sbuf_buf_base = (void *)base;
		scbc->sbuf_buf_last = (void *)(base + bufsize * (numbufs - 1));
		_HFI_VDBG
		    ("sendbufs=%d, (size=%d),base=[%p..%p)\n",
		     numbufs,  bufsize,
		     (void *)scbc->sbuf_buf_base, (void *)scbc->sbuf_buf_last);

		for (i = 0; i < numbufs; i++) {
			sbuf = (struct ips_scbbuf *)(base + bufsize * i);
			SLIST_NEXT(sbuf, next) = NULL;
			SLIST_INSERT_HEAD(&scbc->sbuf_free, sbuf, next);
		}
	}

	imm_base = 0;
	scbc->scb_imm_size = imm_size;
	if (scbc->scb_imm_size) {
		scbc->scb_imm_size = PSMI_ALIGNUP(imm_size, 64);
		alloc_sz = numscb * scbc->scb_imm_size + 64;
		scbc->scb_imm_buf = psmi_memalign(ep, NETWORK_BUFFERS, 64,
						  alloc_sz);

		if (scbc->scb_imm_buf == NULL) {
			err = PSM2_NO_MEMORY;
			goto fail;
		}

		memset(scbc->scb_imm_buf, 0, alloc_sz);
		imm_base = PSMI_ALIGNUP(scbc->scb_imm_buf, 64);
	} else
		scbc->scb_imm_buf = NULL;

	scbc->scb_num = scbc->scb_num_cur = numscb;
	SLIST_INIT(&scbc->scb_free);

	scb_size = PSMI_ALIGNUP(sizeof(*scb), 64);
	alloc_sz = numscb * scb_size;

	scbc->scb_base = psmi_memalign(ep, NETWORK_BUFFERS, 64, alloc_sz);
	if (scbc->scb_base == NULL) {
		err = PSM2_NO_MEMORY;
		goto fail;
	}

	memset(scbc->scb_base, 0, alloc_sz);
	base = (uintptr_t) scbc->scb_base;

	for (i = 0; i < numscb; i++) {
		scb = (struct ips_scb *)(base + i * scb_size);

		scb->scbc = scbc;
		if (scbc->scb_imm_buf)
			scb->imm_payload =
			    (void *)(imm_base + (i * scbc->scb_imm_size));
		else
			scb->imm_payload = NULL;

		SLIST_INSERT_HEAD(&scbc->scb_free, scb, next);
	}
	scbc->scb_avail_callback = scb_avail_callback;
	scbc->scb_avail_context = scb_avail_context;


fail:
	return err;
}

psm2_error_t psm3_ips_scbctrl_fini(struct ips_scbctrl *scbc)
{
	if (scbc->scb_base != NULL) {
		psmi_free(scbc->scb_base);
	}
	if (scbc->sbuf_buf_alloc) {
		psmi_free(scbc->sbuf_buf_alloc);
	}
	if (scbc->scb_imm_buf) {
		psmi_free(scbc->scb_imm_buf);
	}
	return PSM2_OK;
}

int psm3_ips_scbctrl_bufalloc(ips_scb_t *scb)
{
	struct ips_scbctrl *scbc = scb->scbc;

	psmi_assert(scbc->sbuf_num > 0);
	psmi_assert(!((ips_scb_buffer(scb) >= scbc->sbuf_buf_base) &&
			     (ips_scb_buffer(scb) <= scbc->sbuf_buf_last)));
	psmi_assert(scb->payload_size <= scbc->sbuf_buf_size);

	if (scb->payload_size <= scbc->scb_imm_size) {
		/* Attach immediate buffer */
		ips_scb_buffer(scb) = scb->imm_payload;
		return 1;
	}

	if (SLIST_EMPTY(&scbc->sbuf_free))
		return 0;
	else {
		psmi_assert(scbc->sbuf_num_cur);
		ips_scb_buffer(scb) = SLIST_FIRST(&scbc->sbuf_free);
		scbc->sbuf_num_cur--;

		/* If under memory pressure request ACK for packet to reclaim
		 * credits.
		 */
		if (scbc->sbuf_num_cur < (scbc->sbuf_num >> 1))
			scb->scb_flags |= IPS_SEND_FLAG_ACKREQ;

		SLIST_REMOVE_HEAD(&scbc->sbuf_free, next);
		return 1;
	}
}

static ips_scb_t *
psm3_ips_scbctrl_alloc_dynamic(struct ips_scbctrl *scbc, int len,
			       uint32_t flags)
{
	struct ips_scb *scb;
	size_t scb_size;
	size_t alloc_sz;
	struct ips_scb_stats *stats = &scbc->proto->scb_stats;

	scb_size = PSMI_ALIGNUP(sizeof(*scb), 64);
	scb = psmi_memalign(PSMI_EP_NONE, NETWORK_BUFFERS, 64, scb_size);
	if (!scb)
		goto fail_exit;

	/* Zero out all fields */
	memset(scb, 0, scb_size);

	scb->scbc = scbc;

	/* Allocate the buffer if requested */
	if (flags & IPS_SCB_FLAG_ADD_BUFFER) {
		scb->payload_size = len;

		alloc_sz = PSMI_ALIGNUP(len, 64);
		scb->dyn_buf = psmi_calloc(PSMI_EP_NONE, NETWORK_BUFFERS,
					   1, alloc_sz);
		if (scb->dyn_buf == NULL)
			goto fail_scb;
		ips_scb_buffer(scb) = scb->dyn_buf;
	}

	scb->nfrag = 1;
	scb->is_dynamic = true;

	/* We are under memory pressure */
	scb->scb_flags |= IPS_SEND_FLAG_ACKREQ;

	/* Update the stats */
	stats->dyn_alloc++;
	stats->max_dyn_inuse = max(stats->max_dyn_inuse,
				   stats->dyn_alloc - stats->dyn_free);

	return scb;
fail_scb:
	psmi_free(scb);
fail_exit:

	return NULL;
}

static void psm3_ips_scbctrl_free_dynamic(ips_scb_t *scb)
{
	/* Update stats */
	scb->scbc->proto->scb_stats.dyn_free++;

	if (scb->dyn_buf)
		psmi_free(scb->dyn_buf);
	psmi_free(scb);
}

ips_scb_t *MOCKABLE(psm3_ips_scbctrl_alloc)(struct ips_scbctrl *scbc, int scbnum, int len,
				uint32_t flags)
{
	ips_scb_t *scb, *scb_head = NULL;
	struct ips_scb_stats *stats = &scbc->proto->scb_stats;

	psmi_assert(flags & IPS_SCB_FLAG_ADD_BUFFER ? (scbc->sbuf_num > 0) : 1);
	psmi_assert(scbc->sbuf_buf_size >= len);

	while (scbnum--) {
		if (SLIST_EMPTY(&scbc->scb_free)) {
			/*
			 * If we are out of scb and we can't fail, allocate all
			 * the resources dynamically.
			 */
			if (flags & IPS_SCB_FLAG_PRIORITY) {
				scb = psm3_ips_scbctrl_alloc_dynamic(scbc, len,
					flags);
				if (scb)
					goto scb_link;
			}
			break;
		}
		scb = SLIST_FIRST(&scbc->scb_free);
		/* Need to set this here as bufalloc may request
		 * an ACK under memory pressure
		 */
		scb->scb_flags = 0;
		if (flags & IPS_SCB_FLAG_ADD_BUFFER) {
			scb->payload_size = len;
			if (!psm3_ips_scbctrl_bufalloc(scb))
				break;
		} else {
			ips_scb_buffer(scb) = NULL;
			scb->payload_size = 0;
		}

		scb->tidsendc = NULL;
		scb->callback = NULL;
		scb->nfrag = 1;
		scb->frag_size = 0;
		scb->chunk_size = 0;
#ifdef PSM_HAVE_GPU
		scb->mq_req = NULL;
#endif
#ifdef PSM_HAVE_REG_MR
		scb->mr = NULL;
#endif
#ifdef PSM_HAVE_SDMA
		scb->sdma_outstanding = 0;
#endif

		scbc->scb_num_cur--;
		if (scbc->scb_num_cur < (scbc->scb_num >> 1))
			scb->scb_flags |= IPS_SEND_FLAG_ACKREQ;

		SLIST_REMOVE_HEAD(&scbc->scb_free, next);
scb_link:
		SLIST_NEXT(scb, next) = scb_head;
		scb_head = scb;

		/* Update stats */
		stats->alloc++;
		stats->max_inuse = max(stats->max_inuse,
				stats->alloc - stats->free);
	}
	return scb_head;
}
MOCK_DEF_EPILOGUE(psm3_ips_scbctrl_alloc);

void psm3_ips_scbctrl_free(ips_scb_t *scb)
{
	struct ips_scbctrl *scbc = scb->scbc;

	/* Update stats */
	scbc->proto->scb_stats.free++;

	/* If it is allocated dynamically, simply free the resources */
	if (scb->is_dynamic) {
		psm3_ips_scbctrl_free_dynamic(scb);
		return;
	}
	if (scbc->sbuf_num && (ips_scb_buffer(scb) >= scbc->sbuf_buf_base) &&
	    (ips_scb_buffer(scb) <= scbc->sbuf_buf_last)) {
		scbc->sbuf_num_cur++;
		SLIST_INSERT_HEAD(&scbc->sbuf_free, scb->sbuf, next);
	}

	ips_scb_buffer(scb) = NULL;
	scb->tidsendc = NULL;
#ifdef PSM_HAVE_REG_MR
	scb->mr = NULL;
#endif
#ifdef PSM_HAVE_SDMA
	psmi_assert(scb->sdma_outstanding == 0);
	scb->sdma_outstanding = 0;
#endif
	scb->payload_size = 0;
	scbc->scb_num_cur++;
	if (SLIST_EMPTY(&scbc->scb_free)) {
		SLIST_INSERT_HEAD(&scbc->scb_free, scb, next);
		if (scbc->scb_avail_callback != NULL)
			scbc->scb_avail_callback(scbc, scbc->scb_avail_context);
	} else
		SLIST_INSERT_HEAD(&scbc->scb_free, scb, next);

	return;
}

ips_scb_t *MOCKABLE(psm3_ips_scbctrl_alloc_tiny)(struct ips_scbctrl *scbc,
						 uint32_t flags)
{
	ips_scb_t *scb;
	struct ips_scb_stats *stats = &scbc->proto->scb_stats;

	if (SLIST_EMPTY(&scbc->scb_free)) {
		if (flags & IPS_SCB_FLAG_PRIORITY) {
			scb = psm3_ips_scbctrl_alloc_dynamic(scbc, 0, 0);
			goto update_stats;
		}
		return NULL;
	}
	scb = SLIST_FIRST(&scbc->scb_free);

	SLIST_REMOVE_HEAD(&scbc->scb_free, next);
	SLIST_NEXT(scb, next) = NULL;

	ips_scb_buffer(scb) = NULL;
	scb->payload_size = 0;
	scb->scb_flags = 0;
	scb->tidsendc = NULL;
	scb->callback = NULL;
	scb->nfrag = 1;
	scb->frag_size = 0;
	scb->chunk_size = 0;
#ifdef PSM_HAVE_GPU
	scb->mq_req = NULL;
#endif
#ifdef PSM_HAVE_REG_MR
	scb->mr = NULL;
#endif
#ifdef PSM_HAVE_SDMA
	scb->sdma_outstanding = 0;
#endif

	scbc->scb_num_cur--;
	if (scbc->scb_num_cur < (scbc->scb_num >> 1))
		scb->scb_flags |= IPS_SEND_FLAG_ACKREQ;
update_stats:
	stats->alloc++;
	stats->max_inuse = max(stats->max_inuse,
			       stats->alloc - stats->free);

	return scb;
}
MOCK_DEF_EPILOGUE(psm3_ips_scbctrl_alloc_tiny);
