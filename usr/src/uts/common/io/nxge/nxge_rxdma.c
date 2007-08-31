/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */
/*
 * Copyright 2007 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */

#pragma ident	"%Z%%M%	%I%	%E% SMI"

#include <sys/nxge/nxge_impl.h>
#include <sys/nxge/nxge_rxdma.h>

#define	NXGE_ACTUAL_RDCGRP(nxgep, rdcgrp)	\
	(rdcgrp + nxgep->pt_config.hw_config.start_rdc_grpid)
#define	NXGE_ACTUAL_RDC(nxgep, rdc)	\
	(rdc + nxgep->pt_config.hw_config.start_rdc)

/*
 * Globals: tunable parameters (/etc/system or adb)
 *
 */
extern uint32_t nxge_rbr_size;
extern uint32_t nxge_rcr_size;
extern uint32_t	nxge_rbr_spare_size;

extern uint32_t nxge_mblks_pending;

/*
 * Tunable to reduce the amount of time spent in the
 * ISR doing Rx Processing.
 */
extern uint32_t nxge_max_rx_pkts;
boolean_t nxge_jumbo_enable;

/*
 * Tunables to manage the receive buffer blocks.
 *
 * nxge_rx_threshold_hi: copy all buffers.
 * nxge_rx_bcopy_size_type: receive buffer block size type.
 * nxge_rx_threshold_lo: copy only up to tunable block size type.
 */
extern nxge_rxbuf_threshold_t nxge_rx_threshold_hi;
extern nxge_rxbuf_type_t nxge_rx_buf_size_type;
extern nxge_rxbuf_threshold_t nxge_rx_threshold_lo;

static nxge_status_t nxge_map_rxdma(p_nxge_t);
static void nxge_unmap_rxdma(p_nxge_t);

static nxge_status_t nxge_rxdma_hw_start_common(p_nxge_t);
static void nxge_rxdma_hw_stop_common(p_nxge_t);

static nxge_status_t nxge_rxdma_hw_start(p_nxge_t);
static void nxge_rxdma_hw_stop(p_nxge_t);

static nxge_status_t nxge_map_rxdma_channel(p_nxge_t, uint16_t,
    p_nxge_dma_common_t *,  p_rx_rbr_ring_t *,
    uint32_t,
    p_nxge_dma_common_t *, p_rx_rcr_ring_t *,
    p_rx_mbox_t *);
static void nxge_unmap_rxdma_channel(p_nxge_t, uint16_t,
    p_rx_rbr_ring_t, p_rx_rcr_ring_t, p_rx_mbox_t);

static nxge_status_t nxge_map_rxdma_channel_cfg_ring(p_nxge_t,
    uint16_t,
    p_nxge_dma_common_t *, p_rx_rbr_ring_t *,
    p_rx_rcr_ring_t *, p_rx_mbox_t *);
static void nxge_unmap_rxdma_channel_cfg_ring(p_nxge_t,
    p_rx_rcr_ring_t, p_rx_mbox_t);

static nxge_status_t nxge_map_rxdma_channel_buf_ring(p_nxge_t,
    uint16_t,
    p_nxge_dma_common_t *,
    p_rx_rbr_ring_t *, uint32_t);
static void nxge_unmap_rxdma_channel_buf_ring(p_nxge_t,
    p_rx_rbr_ring_t);

static nxge_status_t nxge_rxdma_start_channel(p_nxge_t, uint16_t,
    p_rx_rbr_ring_t, p_rx_rcr_ring_t, p_rx_mbox_t);
static nxge_status_t nxge_rxdma_stop_channel(p_nxge_t, uint16_t);

mblk_t *
nxge_rx_pkts(p_nxge_t, uint_t, p_nxge_ldv_t,
    p_rx_rcr_ring_t *, rx_dma_ctl_stat_t);

static void nxge_receive_packet(p_nxge_t,
	p_rx_rcr_ring_t,
	p_rcr_entry_t,
	boolean_t *,
	mblk_t **, mblk_t **);

nxge_status_t nxge_disable_rxdma_channel(p_nxge_t, uint16_t);

static p_rx_msg_t nxge_allocb(size_t, uint32_t, p_nxge_dma_common_t);
static void nxge_freeb(p_rx_msg_t);
static void nxge_rx_pkts_vring(p_nxge_t, uint_t,
    p_nxge_ldv_t, rx_dma_ctl_stat_t);
static nxge_status_t nxge_rx_err_evnts(p_nxge_t, uint_t,
				p_nxge_ldv_t, rx_dma_ctl_stat_t);

static nxge_status_t nxge_rxdma_handle_port_errors(p_nxge_t,
				uint32_t, uint32_t);

static nxge_status_t nxge_rxbuf_index_info_init(p_nxge_t,
    p_rx_rbr_ring_t);


static nxge_status_t
nxge_rxdma_fatal_err_recover(p_nxge_t, uint16_t);

nxge_status_t
nxge_rx_port_fatal_err_recover(p_nxge_t);

static uint16_t
nxge_get_pktbuf_size(p_nxge_t nxgep, int bufsz_type, rbr_cfig_b_t rbr_cfgb);

nxge_status_t
nxge_init_rxdma_channels(p_nxge_t nxgep)
{
	nxge_status_t	status = NXGE_OK;

	NXGE_DEBUG_MSG((nxgep, MEM2_CTL, "==> nxge_init_rxdma_channels"));

	status = nxge_map_rxdma(nxgep);
	if (status != NXGE_OK) {
		NXGE_ERROR_MSG((nxgep, NXGE_ERR_CTL,
			"<== nxge_init_rxdma: status 0x%x", status));
		return (status);
	}

	status = nxge_rxdma_hw_start_common(nxgep);
	if (status != NXGE_OK) {
		nxge_unmap_rxdma(nxgep);
	}

	status = nxge_rxdma_hw_start(nxgep);
	if (status != NXGE_OK) {
		nxge_unmap_rxdma(nxgep);
	}

	NXGE_DEBUG_MSG((nxgep, MEM2_CTL,
		"<== nxge_init_rxdma_channels: status 0x%x", status));

	return (status);
}

void
nxge_uninit_rxdma_channels(p_nxge_t nxgep)
{
	NXGE_DEBUG_MSG((nxgep, MEM2_CTL, "==> nxge_uninit_rxdma_channels"));

	nxge_rxdma_hw_stop(nxgep);
	nxge_rxdma_hw_stop_common(nxgep);
	nxge_unmap_rxdma(nxgep);

	NXGE_DEBUG_MSG((nxgep, MEM2_CTL,
		"<== nxge_uinit_rxdma_channels"));
}

nxge_status_t
nxge_reset_rxdma_channel(p_nxge_t nxgep, uint16_t channel)
{
	npi_handle_t		handle;
	npi_status_t		rs = NPI_SUCCESS;
	nxge_status_t		status = NXGE_OK;

	NXGE_DEBUG_MSG((nxgep, DMA_CTL, "<== nxge_reset_rxdma_channel"));

	handle = NXGE_DEV_NPI_HANDLE(nxgep);
	rs = npi_rxdma_cfg_rdc_reset(handle, channel);

	if (rs != NPI_SUCCESS) {
		status = NXGE_ERROR | rs;
	}

	return (status);
}

void
nxge_rxdma_regs_dump_channels(p_nxge_t nxgep)
{
	int			i, ndmas;
	uint16_t		channel;
	p_rx_rbr_rings_t 	rx_rbr_rings;
	p_rx_rbr_ring_t		*rbr_rings;
	npi_handle_t		handle;

	NXGE_DEBUG_MSG((nxgep, RX_CTL, "==> nxge_rxdma_regs_dump_channels"));

	handle = NXGE_DEV_NPI_HANDLE(nxgep);
	(void) npi_rxdma_dump_fzc_regs(handle);

	rx_rbr_rings = nxgep->rx_rbr_rings;
	if (rx_rbr_rings == NULL) {
		NXGE_DEBUG_MSG((nxgep, RX_CTL,
			"<== nxge_rxdma_regs_dump_channels: "
			"NULL ring pointer"));
		return;
	}
	if (rx_rbr_rings->rbr_rings == NULL) {
		NXGE_DEBUG_MSG((nxgep, RX_CTL,
			"<== nxge_rxdma_regs_dump_channels: "
			" NULL rbr rings pointer"));
		return;
	}

	ndmas = rx_rbr_rings->ndmas;
	if (!ndmas) {
		NXGE_DEBUG_MSG((nxgep, RX_CTL,
			"<== nxge_rxdma_regs_dump_channels: no channel"));
		return;
	}

	NXGE_DEBUG_MSG((nxgep, MEM2_CTL,
		"==> nxge_rxdma_regs_dump_channels (ndmas %d)", ndmas));

	rbr_rings = rx_rbr_rings->rbr_rings;
	for (i = 0; i < ndmas; i++) {
		if (rbr_rings == NULL || rbr_rings[i] == NULL) {
			continue;
		}
		channel = rbr_rings[i]->rdc;
		(void) nxge_dump_rxdma_channel(nxgep, channel);
	}

	NXGE_DEBUG_MSG((nxgep, RX_CTL, "<== nxge_rxdma_regs_dump"));

}

nxge_status_t
nxge_dump_rxdma_channel(p_nxge_t nxgep, uint8_t channel)
{
	npi_handle_t		handle;
	npi_status_t		rs = NPI_SUCCESS;
	nxge_status_t		status = NXGE_OK;

	NXGE_DEBUG_MSG((nxgep, DMA_CTL, "==> nxge_dump_rxdma_channel"));

	handle = NXGE_DEV_NPI_HANDLE(nxgep);
	rs = npi_rxdma_dump_rdc_regs(handle, channel);

	if (rs != NPI_SUCCESS) {
		status = NXGE_ERROR | rs;
	}
	NXGE_DEBUG_MSG((nxgep, DMA_CTL, "<== nxge_dump_rxdma_channel"));
	return (status);
}

nxge_status_t
nxge_init_rxdma_channel_event_mask(p_nxge_t nxgep, uint16_t channel,
    p_rx_dma_ent_msk_t mask_p)
{
	npi_handle_t		handle;
	npi_status_t		rs = NPI_SUCCESS;
	nxge_status_t		status = NXGE_OK;

	NXGE_DEBUG_MSG((nxgep, DMA_CTL,
		"<== nxge_init_rxdma_channel_event_mask"));

	handle = NXGE_DEV_NPI_HANDLE(nxgep);
	rs = npi_rxdma_event_mask(handle, OP_SET, channel, mask_p);
	if (rs != NPI_SUCCESS) {
		status = NXGE_ERROR | rs;
	}

	return (status);
}

nxge_status_t
nxge_init_rxdma_channel_cntl_stat(p_nxge_t nxgep, uint16_t channel,
    p_rx_dma_ctl_stat_t cs_p)
{
	npi_handle_t		handle;
	npi_status_t		rs = NPI_SUCCESS;
	nxge_status_t		status = NXGE_OK;

	NXGE_DEBUG_MSG((nxgep, DMA_CTL,
		"<== nxge_init_rxdma_channel_cntl_stat"));

	handle = NXGE_DEV_NPI_HANDLE(nxgep);
	rs = npi_rxdma_control_status(handle, OP_SET, channel, cs_p);

	if (rs != NPI_SUCCESS) {
		status = NXGE_ERROR | rs;
	}

	return (status);
}

nxge_status_t
nxge_rxdma_cfg_rdcgrp_default_rdc(p_nxge_t nxgep, uint8_t rdcgrp,
				    uint8_t rdc)
{
	npi_handle_t		handle;
	npi_status_t		rs = NPI_SUCCESS;
	p_nxge_dma_pt_cfg_t	p_dma_cfgp;
	p_nxge_rdc_grp_t	rdc_grp_p;
	uint8_t actual_rdcgrp, actual_rdc;

	NXGE_DEBUG_MSG((nxgep, RX2_CTL,
			    " ==> nxge_rxdma_cfg_rdcgrp_default_rdc"));
	p_dma_cfgp = (p_nxge_dma_pt_cfg_t)&nxgep->pt_config;

	handle = NXGE_DEV_NPI_HANDLE(nxgep);

	rdc_grp_p = &p_dma_cfgp->rdc_grps[rdcgrp];
	rdc_grp_p->rdc[0] = rdc;

	actual_rdcgrp = NXGE_ACTUAL_RDCGRP(nxgep, rdcgrp);
	actual_rdc = NXGE_ACTUAL_RDC(nxgep, rdc);

	rs = npi_rxdma_cfg_rdc_table_default_rdc(handle, actual_rdcgrp,
							    actual_rdc);

	if (rs != NPI_SUCCESS) {
		return (NXGE_ERROR | rs);
	}
	NXGE_DEBUG_MSG((nxgep, RX2_CTL,
			    " <== nxge_rxdma_cfg_rdcgrp_default_rdc"));
	return (NXGE_OK);
}

nxge_status_t
nxge_rxdma_cfg_port_default_rdc(p_nxge_t nxgep, uint8_t port, uint8_t rdc)
{
	npi_handle_t		handle;

	uint8_t actual_rdc;
	npi_status_t		rs = NPI_SUCCESS;

	NXGE_DEBUG_MSG((nxgep, RX2_CTL,
			    " ==> nxge_rxdma_cfg_port_default_rdc"));

	handle = NXGE_DEV_NPI_HANDLE(nxgep);
	actual_rdc = NXGE_ACTUAL_RDC(nxgep, rdc);
	rs = npi_rxdma_cfg_default_port_rdc(handle, port, actual_rdc);


	if (rs != NPI_SUCCESS) {
		return (NXGE_ERROR | rs);
	}
	NXGE_DEBUG_MSG((nxgep, RX2_CTL,
			    " <== nxge_rxdma_cfg_port_default_rdc"));

	return (NXGE_OK);
}

nxge_status_t
nxge_rxdma_cfg_rcr_threshold(p_nxge_t nxgep, uint8_t channel,
				    uint16_t pkts)
{
	npi_status_t	rs = NPI_SUCCESS;
	npi_handle_t	handle;
	NXGE_DEBUG_MSG((nxgep, RX2_CTL,
			    " ==> nxge_rxdma_cfg_rcr_threshold"));
	handle = NXGE_DEV_NPI_HANDLE(nxgep);

	rs = npi_rxdma_cfg_rdc_rcr_threshold(handle, channel, pkts);

	if (rs != NPI_SUCCESS) {
		return (NXGE_ERROR | rs);
	}
	NXGE_DEBUG_MSG((nxgep, RX2_CTL, " <== nxge_rxdma_cfg_rcr_threshold"));
	return (NXGE_OK);
}

nxge_status_t
nxge_rxdma_cfg_rcr_timeout(p_nxge_t nxgep, uint8_t channel,
			    uint16_t tout, uint8_t enable)
{
	npi_status_t	rs = NPI_SUCCESS;
	npi_handle_t	handle;
	NXGE_DEBUG_MSG((nxgep, RX2_CTL, " ==> nxge_rxdma_cfg_rcr_timeout"));
	handle = NXGE_DEV_NPI_HANDLE(nxgep);
	if (enable == 0) {
		rs = npi_rxdma_cfg_rdc_rcr_timeout_disable(handle, channel);
	} else {
		rs = npi_rxdma_cfg_rdc_rcr_timeout(handle, channel,
							    tout);
	}

	if (rs != NPI_SUCCESS) {
		return (NXGE_ERROR | rs);
	}
	NXGE_DEBUG_MSG((nxgep, RX2_CTL, " <== nxge_rxdma_cfg_rcr_timeout"));
	return (NXGE_OK);
}

nxge_status_t
nxge_enable_rxdma_channel(p_nxge_t nxgep, uint16_t channel,
    p_rx_rbr_ring_t rbr_p, p_rx_rcr_ring_t rcr_p, p_rx_mbox_t mbox_p)
{
	npi_handle_t		handle;
	rdc_desc_cfg_t 		rdc_desc;
	p_rcrcfig_b_t		cfgb_p;
	npi_status_t		rs = NPI_SUCCESS;

	NXGE_DEBUG_MSG((nxgep, DMA_CTL, "==> nxge_enable_rxdma_channel"));
	handle = NXGE_DEV_NPI_HANDLE(nxgep);
	/*
	 * Use configuration data composed at init time.
	 * Write to hardware the receive ring configurations.
	 */
	rdc_desc.mbox_enable = 1;
	rdc_desc.mbox_addr = mbox_p->mbox_addr;
	NXGE_DEBUG_MSG((nxgep, RX_CTL,
		"==> nxge_enable_rxdma_channel: mboxp $%p($%p)",
		mbox_p->mbox_addr, rdc_desc.mbox_addr));

	rdc_desc.rbr_len = rbr_p->rbb_max;
	rdc_desc.rbr_addr = rbr_p->rbr_addr;

	switch (nxgep->rx_bksize_code) {
	case RBR_BKSIZE_4K:
		rdc_desc.page_size = SIZE_4KB;
		break;
	case RBR_BKSIZE_8K:
		rdc_desc.page_size = SIZE_8KB;
		break;
	case RBR_BKSIZE_16K:
		rdc_desc.page_size = SIZE_16KB;
		break;
	case RBR_BKSIZE_32K:
		rdc_desc.page_size = SIZE_32KB;
		break;
	}

	rdc_desc.size0 = rbr_p->npi_pkt_buf_size0;
	rdc_desc.valid0 = 1;

	rdc_desc.size1 = rbr_p->npi_pkt_buf_size1;
	rdc_desc.valid1 = 1;

	rdc_desc.size2 = rbr_p->npi_pkt_buf_size2;
	rdc_desc.valid2 = 1;

	rdc_desc.full_hdr = rcr_p->full_hdr_flag;
	rdc_desc.offset = rcr_p->sw_priv_hdr_len;

	rdc_desc.rcr_len = rcr_p->comp_size;
	rdc_desc.rcr_addr = rcr_p->rcr_addr;

	cfgb_p = &(rcr_p->rcr_cfgb);
	rdc_desc.rcr_threshold = cfgb_p->bits.ldw.pthres;
	rdc_desc.rcr_timeout = cfgb_p->bits.ldw.timeout;
	rdc_desc.rcr_timeout_enable = cfgb_p->bits.ldw.entout;

	NXGE_DEBUG_MSG((nxgep, DMA_CTL, "==> nxge_enable_rxdma_channel: "
		"rbr_len qlen %d pagesize code %d rcr_len %d",
		rdc_desc.rbr_len, rdc_desc.page_size, rdc_desc.rcr_len));
	NXGE_DEBUG_MSG((nxgep, DMA_CTL, "==> nxge_enable_rxdma_channel: "
		"size 0 %d size 1 %d size 2 %d",
		rbr_p->npi_pkt_buf_size0, rbr_p->npi_pkt_buf_size1,
		rbr_p->npi_pkt_buf_size2));

	rs = npi_rxdma_cfg_rdc_ring(handle, rbr_p->rdc, &rdc_desc);
	if (rs != NPI_SUCCESS) {
		return (NXGE_ERROR | rs);
	}

	/*
	 * Enable the timeout and threshold.
	 */
	rs = npi_rxdma_cfg_rdc_rcr_threshold(handle, channel,
			rdc_desc.rcr_threshold);
	if (rs != NPI_SUCCESS) {
		return (NXGE_ERROR | rs);
	}

	rs = npi_rxdma_cfg_rdc_rcr_timeout(handle, channel,
			rdc_desc.rcr_timeout);
	if (rs != NPI_SUCCESS) {
		return (NXGE_ERROR | rs);
	}

	/* Enable the DMA */
	rs = npi_rxdma_cfg_rdc_enable(handle, channel);
	if (rs != NPI_SUCCESS) {
		return (NXGE_ERROR | rs);
	}

	/* Kick the DMA engine. */
	npi_rxdma_rdc_rbr_kick(handle, channel, rbr_p->rbb_max);
	/* Clear the rbr empty bit */
	(void) npi_rxdma_channel_rbr_empty_clear(handle, channel);

	NXGE_DEBUG_MSG((nxgep, DMA_CTL, "<== nxge_enable_rxdma_channel"));

	return (NXGE_OK);
}

nxge_status_t
nxge_disable_rxdma_channel(p_nxge_t nxgep, uint16_t channel)
{
	npi_handle_t		handle;
	npi_status_t		rs = NPI_SUCCESS;

	NXGE_DEBUG_MSG((nxgep, DMA_CTL, "==> nxge_disable_rxdma_channel"));
	handle = NXGE_DEV_NPI_HANDLE(nxgep);

	/* disable the DMA */
	rs = npi_rxdma_cfg_rdc_disable(handle, channel);
	if (rs != NPI_SUCCESS) {
		NXGE_DEBUG_MSG((nxgep, RX_CTL,
			"<== nxge_disable_rxdma_channel:failed (0x%x)",
			rs));
		return (NXGE_ERROR | rs);
	}

	NXGE_DEBUG_MSG((nxgep, DMA_CTL, "<== nxge_disable_rxdma_channel"));
	return (NXGE_OK);
}

nxge_status_t
nxge_rxdma_channel_rcrflush(p_nxge_t nxgep, uint8_t channel)
{
	npi_handle_t		handle;
	nxge_status_t		status = NXGE_OK;

	NXGE_DEBUG_MSG((nxgep, DMA_CTL,
		"<== nxge_init_rxdma_channel_rcrflush"));

	handle = NXGE_DEV_NPI_HANDLE(nxgep);
	npi_rxdma_rdc_rcr_flush(handle, channel);

	NXGE_DEBUG_MSG((nxgep, DMA_CTL,
		"<== nxge_init_rxdma_channel_rcrflsh"));
	return (status);

}

#define	MID_INDEX(l, r) ((r + l + 1) >> 1)

#define	TO_LEFT -1
#define	TO_RIGHT 1
#define	BOTH_RIGHT (TO_RIGHT + TO_RIGHT)
#define	BOTH_LEFT (TO_LEFT + TO_LEFT)
#define	IN_MIDDLE (TO_RIGHT + TO_LEFT)
#define	NO_HINT 0xffffffff

/*ARGSUSED*/
nxge_status_t
nxge_rxbuf_pp_to_vp(p_nxge_t nxgep, p_rx_rbr_ring_t rbr_p,
	uint8_t pktbufsz_type, uint64_t *pkt_buf_addr_pp,
	uint64_t **pkt_buf_addr_p, uint32_t *bufoffset, uint32_t *msg_index)
{
	int			bufsize;
	uint64_t		pktbuf_pp;
	uint64_t 		dvma_addr;
	rxring_info_t 		*ring_info;
	int 			base_side, end_side;
	int 			r_index, l_index, anchor_index;
	int 			found, search_done;
	uint32_t offset, chunk_size, block_size, page_size_mask;
	uint32_t chunk_index, block_index, total_index;
	int 			max_iterations, iteration;
	rxbuf_index_info_t 	*bufinfo;

	NXGE_DEBUG_MSG((nxgep, RX2_CTL, "==> nxge_rxbuf_pp_to_vp"));

	NXGE_DEBUG_MSG((nxgep, RX2_CTL,
		"==> nxge_rxbuf_pp_to_vp: buf_pp $%p btype %d",
		pkt_buf_addr_pp,
		pktbufsz_type));

	pktbuf_pp = (uint64_t)pkt_buf_addr_pp;

	switch (pktbufsz_type) {
	case 0:
		bufsize = rbr_p->pkt_buf_size0;
		break;
	case 1:
		bufsize = rbr_p->pkt_buf_size1;
		break;
	case 2:
		bufsize = rbr_p->pkt_buf_size2;
		break;
	case RCR_SINGLE_BLOCK:
		bufsize = 0;
		anchor_index = 0;
		break;
	default:
		return (NXGE_ERROR);
	}

	if (rbr_p->num_blocks == 1) {
		anchor_index = 0;
		ring_info = rbr_p->ring_info;
		bufinfo = (rxbuf_index_info_t *)ring_info->buffer;
		NXGE_DEBUG_MSG((nxgep, RX2_CTL,
			"==> nxge_rxbuf_pp_to_vp: (found, 1 block) "
			"buf_pp $%p btype %d anchor_index %d "
			"bufinfo $%p",
			pkt_buf_addr_pp,
			pktbufsz_type,
			anchor_index,
			bufinfo));

		goto found_index;
	}

	NXGE_DEBUG_MSG((nxgep, RX2_CTL,
		"==> nxge_rxbuf_pp_to_vp: "
		"buf_pp $%p btype %d  anchor_index %d",
		pkt_buf_addr_pp,
		pktbufsz_type,
		anchor_index));

	ring_info = rbr_p->ring_info;
	found = B_FALSE;
	bufinfo = (rxbuf_index_info_t *)ring_info->buffer;
	iteration = 0;
	max_iterations = ring_info->max_iterations;
		/*
		 * First check if this block has been seen
		 * recently. This is indicated by a hint which
		 * is initialized when the first buffer of the block
		 * is seen. The hint is reset when the last buffer of
		 * the block has been processed.
		 * As three block sizes are supported, three hints
		 * are kept. The idea behind the hints is that once
		 * the hardware  uses a block for a buffer  of that
		 * size, it will use it exclusively for that size
		 * and will use it until it is exhausted. It is assumed
		 * that there would a single block being used for the same
		 * buffer sizes at any given time.
		 */
	if (ring_info->hint[pktbufsz_type] != NO_HINT) {
		anchor_index = ring_info->hint[pktbufsz_type];
		dvma_addr =  bufinfo[anchor_index].dvma_addr;
		chunk_size = bufinfo[anchor_index].buf_size;
		if ((pktbuf_pp >= dvma_addr) &&
			(pktbuf_pp < (dvma_addr + chunk_size))) {
			found = B_TRUE;
				/*
				 * check if this is the last buffer in the block
				 * If so, then reset the hint for the size;
				 */

			if ((pktbuf_pp + bufsize) >= (dvma_addr + chunk_size))
				ring_info->hint[pktbufsz_type] = NO_HINT;
		}
	}

	if (found == B_FALSE) {
		NXGE_DEBUG_MSG((nxgep, RX2_CTL,
			"==> nxge_rxbuf_pp_to_vp: (!found)"
			"buf_pp $%p btype %d anchor_index %d",
			pkt_buf_addr_pp,
			pktbufsz_type,
			anchor_index));

			/*
			 * This is the first buffer of the block of this
			 * size. Need to search the whole information
			 * array.
			 * the search algorithm uses a binary tree search
			 * algorithm. It assumes that the information is
			 * already sorted with increasing order
			 * info[0] < info[1] < info[2]  .... < info[n-1]
			 * where n is the size of the information array
			 */
		r_index = rbr_p->num_blocks - 1;
		l_index = 0;
		search_done = B_FALSE;
		anchor_index = MID_INDEX(r_index, l_index);
		while (search_done == B_FALSE) {
			if ((r_index == l_index) ||
				(iteration >= max_iterations))
				search_done = B_TRUE;
			end_side = TO_RIGHT; /* to the right */
			base_side = TO_LEFT; /* to the left */
			/* read the DVMA address information and sort it */
			dvma_addr =  bufinfo[anchor_index].dvma_addr;
			chunk_size = bufinfo[anchor_index].buf_size;
			NXGE_DEBUG_MSG((nxgep, RX2_CTL,
				"==> nxge_rxbuf_pp_to_vp: (searching)"
				"buf_pp $%p btype %d "
				"anchor_index %d chunk_size %d dvmaaddr $%p",
				pkt_buf_addr_pp,
				pktbufsz_type,
				anchor_index,
				chunk_size,
				dvma_addr));

			if (pktbuf_pp >= dvma_addr)
				base_side = TO_RIGHT; /* to the right */
			if (pktbuf_pp < (dvma_addr + chunk_size))
				end_side = TO_LEFT; /* to the left */

			switch (base_side + end_side) {
				case IN_MIDDLE:
					/* found */
					found = B_TRUE;
					search_done = B_TRUE;
					if ((pktbuf_pp + bufsize) <
						(dvma_addr + chunk_size))
						ring_info->hint[pktbufsz_type] =
						bufinfo[anchor_index].buf_index;
					break;
				case BOTH_RIGHT:
						/* not found: go to the right */
					l_index = anchor_index + 1;
					anchor_index =
						MID_INDEX(r_index, l_index);
					break;

				case  BOTH_LEFT:
						/* not found: go to the left */
					r_index = anchor_index - 1;
					anchor_index = MID_INDEX(r_index,
						l_index);
					break;
				default: /* should not come here */
					return (NXGE_ERROR);
			}
			iteration++;
		}

		NXGE_DEBUG_MSG((nxgep, RX2_CTL,
			"==> nxge_rxbuf_pp_to_vp: (search done)"
			"buf_pp $%p btype %d anchor_index %d",
			pkt_buf_addr_pp,
			pktbufsz_type,
			anchor_index));
	}

	if (found == B_FALSE) {
		NXGE_DEBUG_MSG((nxgep, RX2_CTL,
			"==> nxge_rxbuf_pp_to_vp: (search failed)"
			"buf_pp $%p btype %d anchor_index %d",
			pkt_buf_addr_pp,
			pktbufsz_type,
			anchor_index));
		return (NXGE_ERROR);
	}

found_index:
	NXGE_DEBUG_MSG((nxgep, RX2_CTL,
		"==> nxge_rxbuf_pp_to_vp: (FOUND1)"
		"buf_pp $%p btype %d bufsize %d anchor_index %d",
		pkt_buf_addr_pp,
		pktbufsz_type,
		bufsize,
		anchor_index));

	/* index of the first block in this chunk */
	chunk_index = bufinfo[anchor_index].start_index;
	dvma_addr =  bufinfo[anchor_index].dvma_addr;
	page_size_mask = ring_info->block_size_mask;

	NXGE_DEBUG_MSG((nxgep, RX2_CTL,
		"==> nxge_rxbuf_pp_to_vp: (FOUND3), get chunk)"
		"buf_pp $%p btype %d bufsize %d "
		"anchor_index %d chunk_index %d dvma $%p",
		pkt_buf_addr_pp,
		pktbufsz_type,
		bufsize,
		anchor_index,
		chunk_index,
		dvma_addr));

	offset = pktbuf_pp - dvma_addr; /* offset within the chunk */
	block_size = rbr_p->block_size; /* System  block(page) size */

	NXGE_DEBUG_MSG((nxgep, RX2_CTL,
		"==> nxge_rxbuf_pp_to_vp: (FOUND4), get chunk)"
		"buf_pp $%p btype %d bufsize %d "
		"anchor_index %d chunk_index %d dvma $%p "
		"offset %d block_size %d",
		pkt_buf_addr_pp,
		pktbufsz_type,
		bufsize,
		anchor_index,
		chunk_index,
		dvma_addr,
		offset,
		block_size));

	NXGE_DEBUG_MSG((nxgep, RX2_CTL, "==> getting total index"));

	block_index = (offset / block_size); /* index within chunk */
	total_index = chunk_index + block_index;


	NXGE_DEBUG_MSG((nxgep, RX2_CTL,
		"==> nxge_rxbuf_pp_to_vp: "
		"total_index %d dvma_addr $%p "
		"offset %d block_size %d "
		"block_index %d ",
		total_index, dvma_addr,
		offset, block_size,
		block_index));

	*pkt_buf_addr_p = (uint64_t *)((uint64_t)bufinfo[anchor_index].kaddr
				+ offset);

	NXGE_DEBUG_MSG((nxgep, RX2_CTL,
		"==> nxge_rxbuf_pp_to_vp: "
		"total_index %d dvma_addr $%p "
		"offset %d block_size %d "
		"block_index %d "
		"*pkt_buf_addr_p $%p",
		total_index, dvma_addr,
		offset, block_size,
		block_index,
		*pkt_buf_addr_p));


	*msg_index = total_index;
	*bufoffset =  (offset & page_size_mask);

	NXGE_DEBUG_MSG((nxgep, RX2_CTL,
		"==> nxge_rxbuf_pp_to_vp: get msg index: "
		"msg_index %d bufoffset_index %d",
		*msg_index,
		*bufoffset));

	NXGE_DEBUG_MSG((nxgep, RX2_CTL, "<== nxge_rxbuf_pp_to_vp"));

	return (NXGE_OK);
}

/*
 * used by quick sort (qsort) function
 * to perform comparison
 */
static int
nxge_sort_compare(const void *p1, const void *p2)
{

	rxbuf_index_info_t *a, *b;

	a = (rxbuf_index_info_t *)p1;
	b = (rxbuf_index_info_t *)p2;

	if (a->dvma_addr > b->dvma_addr)
		return (1);
	if (a->dvma_addr < b->dvma_addr)
		return (-1);
	return (0);
}



/*
 * grabbed this sort implementation from common/syscall/avl.c
 *
 */
/*
 * Generic shellsort, from K&R (1st ed, p 58.), somewhat modified.
 * v = Ptr to array/vector of objs
 * n = # objs in the array
 * s = size of each obj (must be multiples of a word size)
 * f = ptr to function to compare two objs
 *	returns (-1 = less than, 0 = equal, 1 = greater than
 */
void
nxge_ksort(caddr_t v, int n, int s, int (*f)())
{
	int g, i, j, ii;
	unsigned int *p1, *p2;
	unsigned int tmp;

	/* No work to do */
	if (v == NULL || n <= 1)
		return;
	/* Sanity check on arguments */
	ASSERT(((uintptr_t)v & 0x3) == 0 && (s & 0x3) == 0);
	ASSERT(s > 0);

	for (g = n / 2; g > 0; g /= 2) {
		for (i = g; i < n; i++) {
			for (j = i - g; j >= 0 &&
				(*f)(v + j * s, v + (j + g) * s) == 1;
					j -= g) {
				p1 = (unsigned *)(v + j * s);
				p2 = (unsigned *)(v + (j + g) * s);
				for (ii = 0; ii < s / 4; ii++) {
					tmp = *p1;
					*p1++ = *p2;
					*p2++ = tmp;
				}
			}
		}
	}
}

/*
 * Initialize data structures required for rxdma
 * buffer dvma->vmem address lookup
 */
/*ARGSUSED*/
static nxge_status_t
nxge_rxbuf_index_info_init(p_nxge_t nxgep, p_rx_rbr_ring_t rbrp)
{

	int index;
	rxring_info_t *ring_info;
	int max_iteration = 0, max_index = 0;

	NXGE_DEBUG_MSG((nxgep, DMA_CTL, "==> nxge_rxbuf_index_info_init"));

	ring_info = rbrp->ring_info;
	ring_info->hint[0] = NO_HINT;
	ring_info->hint[1] = NO_HINT;
	ring_info->hint[2] = NO_HINT;
	max_index = rbrp->num_blocks;

		/* read the DVMA address information and sort it */
		/* do init of the information array */


	NXGE_DEBUG_MSG((nxgep, DMA2_CTL,
		" nxge_rxbuf_index_info_init Sort ptrs"));

		/* sort the array */
	nxge_ksort((void *)ring_info->buffer, max_index,
		sizeof (rxbuf_index_info_t), nxge_sort_compare);



	for (index = 0; index < max_index; index++) {
		NXGE_DEBUG_MSG((nxgep, DMA2_CTL,
			" nxge_rxbuf_index_info_init: sorted chunk %d "
			" ioaddr $%p kaddr $%p size %x",
			index, ring_info->buffer[index].dvma_addr,
			ring_info->buffer[index].kaddr,
			ring_info->buffer[index].buf_size));
	}

	max_iteration = 0;
	while (max_index >= (1ULL << max_iteration))
		max_iteration++;
	ring_info->max_iterations = max_iteration + 1;
	NXGE_DEBUG_MSG((nxgep, DMA2_CTL,
		" nxge_rxbuf_index_info_init Find max iter %d",
					ring_info->max_iterations));

	NXGE_DEBUG_MSG((nxgep, DMA_CTL, "<== nxge_rxbuf_index_info_init"));
	return (NXGE_OK);
}

/* ARGSUSED */
void
nxge_dump_rcr_entry(p_nxge_t nxgep, p_rcr_entry_t entry_p)
{
#ifdef	NXGE_DEBUG

	uint32_t bptr;
	uint64_t pp;

	bptr = entry_p->bits.hdw.pkt_buf_addr;

	NXGE_DEBUG_MSG((nxgep, RX_CTL,
		"\trcr entry $%p "
		"\trcr entry 0x%0llx "
		"\trcr entry 0x%08x "
		"\trcr entry 0x%08x "
		"\tvalue 0x%0llx\n"
		"\tmulti = %d\n"
		"\tpkt_type = 0x%x\n"
		"\tzero_copy = %d\n"
		"\tnoport = %d\n"
		"\tpromis = %d\n"
		"\terror = 0x%04x\n"
		"\tdcf_err = 0x%01x\n"
		"\tl2_len = %d\n"
		"\tpktbufsize = %d\n"
		"\tpkt_buf_addr = $%p\n"
		"\tpkt_buf_addr (<< 6) = $%p\n",
		entry_p,
		*(int64_t *)entry_p,
		*(int32_t *)entry_p,
		*(int32_t *)((char *)entry_p + 32),
		entry_p->value,
		entry_p->bits.hdw.multi,
		entry_p->bits.hdw.pkt_type,
		entry_p->bits.hdw.zero_copy,
		entry_p->bits.hdw.noport,
		entry_p->bits.hdw.promis,
		entry_p->bits.hdw.error,
		entry_p->bits.hdw.dcf_err,
		entry_p->bits.hdw.l2_len,
		entry_p->bits.hdw.pktbufsz,
		bptr,
		entry_p->bits.ldw.pkt_buf_addr));

	pp = (entry_p->value & RCR_PKT_BUF_ADDR_MASK) <<
		RCR_PKT_BUF_ADDR_SHIFT;

	NXGE_DEBUG_MSG((nxgep, RX_CTL, "rcr pp 0x%llx l2 len %d",
		pp, (*(int64_t *)entry_p >> 40) & 0x3fff));
#endif
}

void
nxge_rxdma_regs_dump(p_nxge_t nxgep, int rdc)
{
	npi_handle_t		handle;
	rbr_stat_t 		rbr_stat;
	addr44_t 		hd_addr;
	addr44_t 		tail_addr;
	uint16_t 		qlen;

	NXGE_DEBUG_MSG((nxgep, RX_CTL,
		"==> nxge_rxdma_regs_dump: rdc channel %d", rdc));

	handle = NXGE_DEV_NPI_HANDLE(nxgep);

	/* RBR head */
	hd_addr.addr = 0;
	(void) npi_rxdma_rdc_rbr_head_get(handle, rdc, &hd_addr);
	printf("nxge_rxdma_regs_dump: got hdptr $%p \n",
		(void *)hd_addr.addr);

	/* RBR stats */
	(void) npi_rxdma_rdc_rbr_stat_get(handle, rdc, &rbr_stat);
	printf("nxge_rxdma_regs_dump: rbr len %d \n", rbr_stat.bits.ldw.qlen);

	/* RCR tail */
	tail_addr.addr = 0;
	(void) npi_rxdma_rdc_rcr_tail_get(handle, rdc, &tail_addr);
	printf("nxge_rxdma_regs_dump: got tail ptr $%p \n",
		(void *)tail_addr.addr);

	/* RCR qlen */
	(void) npi_rxdma_rdc_rcr_qlen_get(handle, rdc, &qlen);
	printf("nxge_rxdma_regs_dump: rcr len %x \n", qlen);

	NXGE_DEBUG_MSG((nxgep, RX_CTL,
		"<== nxge_rxdma_regs_dump: rdc rdc %d", rdc));
}

void
nxge_rxdma_stop(p_nxge_t nxgep)
{
	NXGE_DEBUG_MSG((nxgep, RX_CTL, "==> nxge_rxdma_stop"));

	(void) nxge_link_monitor(nxgep, LINK_MONITOR_STOP);
	(void) nxge_rx_mac_disable(nxgep);
	(void) nxge_rxdma_hw_mode(nxgep, NXGE_DMA_STOP);
	NXGE_DEBUG_MSG((nxgep, RX_CTL, "<== nxge_rxdma_stop"));
}

void
nxge_rxdma_stop_reinit(p_nxge_t nxgep)
{
	NXGE_DEBUG_MSG((nxgep, RX_CTL, "==> nxge_rxdma_stop_reinit"));

	(void) nxge_rxdma_stop(nxgep);
	(void) nxge_uninit_rxdma_channels(nxgep);
	(void) nxge_init_rxdma_channels(nxgep);

#ifndef	AXIS_DEBUG_LB
	(void) nxge_xcvr_init(nxgep);
	(void) nxge_link_monitor(nxgep, LINK_MONITOR_START);
#endif
	(void) nxge_rx_mac_enable(nxgep);

	NXGE_DEBUG_MSG((nxgep, RX_CTL, "<== nxge_rxdma_stop_reinit"));
}

nxge_status_t
nxge_rxdma_hw_mode(p_nxge_t nxgep, boolean_t enable)
{
	int			i, ndmas;
	uint16_t		channel;
	p_rx_rbr_rings_t 	rx_rbr_rings;
	p_rx_rbr_ring_t		*rbr_rings;
	npi_handle_t		handle;
	npi_status_t		rs = NPI_SUCCESS;
	nxge_status_t		status = NXGE_OK;

	NXGE_DEBUG_MSG((nxgep, MEM2_CTL,
		"==> nxge_rxdma_hw_mode: mode %d", enable));

	if (!(nxgep->drv_state & STATE_HW_INITIALIZED)) {
		NXGE_DEBUG_MSG((nxgep, RX_CTL,
			"<== nxge_rxdma_mode: not initialized"));
		return (NXGE_ERROR);
	}

	rx_rbr_rings = nxgep->rx_rbr_rings;
	if (rx_rbr_rings == NULL) {
		NXGE_DEBUG_MSG((nxgep, RX_CTL,
			"<== nxge_rxdma_mode: NULL ring pointer"));
		return (NXGE_ERROR);
	}
	if (rx_rbr_rings->rbr_rings == NULL) {
		NXGE_DEBUG_MSG((nxgep, RX_CTL,
			"<== nxge_rxdma_mode: NULL rbr rings pointer"));
		return (NXGE_ERROR);
	}

	ndmas = rx_rbr_rings->ndmas;
	if (!ndmas) {
		NXGE_DEBUG_MSG((nxgep, RX_CTL,
			"<== nxge_rxdma_mode: no channel"));
		return (NXGE_ERROR);
	}

	NXGE_DEBUG_MSG((nxgep, MEM2_CTL,
		"==> nxge_rxdma_mode (ndmas %d)", ndmas));

	rbr_rings = rx_rbr_rings->rbr_rings;

	handle = NXGE_DEV_NPI_HANDLE(nxgep);
	for (i = 0; i < ndmas; i++) {
		if (rbr_rings == NULL || rbr_rings[i] == NULL) {
			continue;
		}
		channel = rbr_rings[i]->rdc;
		if (enable) {
			NXGE_DEBUG_MSG((nxgep, MEM2_CTL,
				"==> nxge_rxdma_hw_mode: channel %d (enable)",
				channel));
			rs = npi_rxdma_cfg_rdc_enable(handle, channel);
		} else {
			NXGE_DEBUG_MSG((nxgep, MEM2_CTL,
				"==> nxge_rxdma_hw_mode: channel %d (disable)",
				channel));
			rs = npi_rxdma_cfg_rdc_disable(handle, channel);
		}
	}

	status = ((rs == NPI_SUCCESS) ? NXGE_OK : NXGE_ERROR | rs);

	NXGE_DEBUG_MSG((nxgep, MEM2_CTL,
		"<== nxge_rxdma_hw_mode: status 0x%x", status));

	return (status);
}

void
nxge_rxdma_enable_channel(p_nxge_t nxgep, uint16_t channel)
{
	npi_handle_t		handle;

	NXGE_DEBUG_MSG((nxgep, DMA_CTL,
		"==> nxge_rxdma_enable_channel: channel %d", channel));

	handle = NXGE_DEV_NPI_HANDLE(nxgep);
	(void) npi_rxdma_cfg_rdc_enable(handle, channel);

	NXGE_DEBUG_MSG((nxgep, DMA_CTL, "<== nxge_rxdma_enable_channel"));
}

void
nxge_rxdma_disable_channel(p_nxge_t nxgep, uint16_t channel)
{
	npi_handle_t		handle;

	NXGE_DEBUG_MSG((nxgep, DMA_CTL,
		"==> nxge_rxdma_disable_channel: channel %d", channel));

	handle = NXGE_DEV_NPI_HANDLE(nxgep);
	(void) npi_rxdma_cfg_rdc_disable(handle, channel);

	NXGE_DEBUG_MSG((nxgep, DMA_CTL, "<== nxge_rxdma_disable_channel"));
}

void
nxge_hw_start_rx(p_nxge_t nxgep)
{
	NXGE_DEBUG_MSG((nxgep, DDI_CTL, "==> nxge_hw_start_rx"));

	(void) nxge_rxdma_hw_mode(nxgep, NXGE_DMA_START);
	(void) nxge_rx_mac_enable(nxgep);

	NXGE_DEBUG_MSG((nxgep, DDI_CTL, "<== nxge_hw_start_rx"));
}

/*ARGSUSED*/
void
nxge_fixup_rxdma_rings(p_nxge_t nxgep)
{
	int			i, ndmas;
	uint16_t		rdc;
	p_rx_rbr_rings_t 	rx_rbr_rings;
	p_rx_rbr_ring_t		*rbr_rings;
	p_rx_rcr_rings_t 	rx_rcr_rings;

	NXGE_DEBUG_MSG((nxgep, RX_CTL, "==> nxge_fixup_rxdma_rings"));

	rx_rbr_rings = nxgep->rx_rbr_rings;
	if (rx_rbr_rings == NULL) {
		NXGE_DEBUG_MSG((nxgep, RX_CTL,
			"<== nxge_fixup_rxdma_rings: NULL ring pointer"));
		return;
	}
	ndmas = rx_rbr_rings->ndmas;
	if (!ndmas) {
		NXGE_DEBUG_MSG((nxgep, RX_CTL,
			"<== nxge_fixup_rxdma_rings: no channel"));
		return;
	}

	rx_rcr_rings = nxgep->rx_rcr_rings;
	if (rx_rcr_rings == NULL) {
		NXGE_DEBUG_MSG((nxgep, RX_CTL,
			"<== nxge_fixup_rxdma_rings: NULL ring pointer"));
		return;
	}
	NXGE_DEBUG_MSG((nxgep, RX_CTL,
		"==> nxge_fixup_rxdma_rings (ndmas %d)", ndmas));

	nxge_rxdma_hw_stop(nxgep);

	rbr_rings = rx_rbr_rings->rbr_rings;
	for (i = 0; i < ndmas; i++) {
		rdc = rbr_rings[i]->rdc;
		NXGE_DEBUG_MSG((nxgep, RX_CTL,
			"==> nxge_fixup_rxdma_rings: channel %d "
			"ring $%px", rdc, rbr_rings[i]));
		(void) nxge_rxdma_fixup_channel(nxgep, rdc, i);
	}

	NXGE_DEBUG_MSG((nxgep, RX_CTL, "<== nxge_fixup_rxdma_rings"));
}

void
nxge_rxdma_fix_channel(p_nxge_t nxgep, uint16_t channel)
{
	int		i;

	NXGE_DEBUG_MSG((nxgep, RX_CTL, "==> nxge_rxdma_fix_channel"));
	i = nxge_rxdma_get_ring_index(nxgep, channel);
	if (i < 0) {
		NXGE_DEBUG_MSG((nxgep, RX_CTL,
			"<== nxge_rxdma_fix_channel: no entry found"));
		return;
	}

	nxge_rxdma_fixup_channel(nxgep, channel, i);

	NXGE_DEBUG_MSG((nxgep, RX_CTL, "<== nxge_txdma_fix_channel"));
}

void
nxge_rxdma_fixup_channel(p_nxge_t nxgep, uint16_t channel, int entry)
{
	int			ndmas;
	p_rx_rbr_rings_t 	rx_rbr_rings;
	p_rx_rbr_ring_t		*rbr_rings;
	p_rx_rcr_rings_t 	rx_rcr_rings;
	p_rx_rcr_ring_t		*rcr_rings;
	p_rx_mbox_areas_t 	rx_mbox_areas_p;
	p_rx_mbox_t		*rx_mbox_p;
	p_nxge_dma_pool_t	dma_buf_poolp;
	p_nxge_dma_pool_t	dma_cntl_poolp;
	p_rx_rbr_ring_t 	rbrp;
	p_rx_rcr_ring_t 	rcrp;
	p_rx_mbox_t 		mboxp;
	p_nxge_dma_common_t 	dmap;
	nxge_status_t		status = NXGE_OK;

	NXGE_DEBUG_MSG((nxgep, RX_CTL, "==> nxge_rxdma_fixup_channel"));

	(void) nxge_rxdma_stop_channel(nxgep, channel);

	dma_buf_poolp = nxgep->rx_buf_pool_p;
	dma_cntl_poolp = nxgep->rx_cntl_pool_p;

	if (!dma_buf_poolp->buf_allocated || !dma_cntl_poolp->buf_allocated) {
		NXGE_DEBUG_MSG((nxgep, MEM2_CTL,
			"<== nxge_rxdma_fixup_channel: buf not allocated"));
		return;
	}

	ndmas = dma_buf_poolp->ndmas;
	if (!ndmas) {
		NXGE_DEBUG_MSG((nxgep, MEM2_CTL,
			"<== nxge_rxdma_fixup_channel: no dma allocated"));
		return;
	}

	rx_rbr_rings = nxgep->rx_rbr_rings;
	rx_rcr_rings = nxgep->rx_rcr_rings;
	rbr_rings = rx_rbr_rings->rbr_rings;
	rcr_rings = rx_rcr_rings->rcr_rings;
	rx_mbox_areas_p = nxgep->rx_mbox_areas_p;
	rx_mbox_p = rx_mbox_areas_p->rxmbox_areas;

	/* Reinitialize the receive block and completion rings */
	rbrp = (p_rx_rbr_ring_t)rbr_rings[entry],
	rcrp = (p_rx_rcr_ring_t)rcr_rings[entry],
	mboxp = (p_rx_mbox_t)rx_mbox_p[entry];


	rbrp->rbr_wr_index = (rbrp->rbb_max - 1);
	rbrp->rbr_rd_index = 0;
	rcrp->comp_rd_index = 0;
	rcrp->comp_wt_index = 0;

	dmap = (p_nxge_dma_common_t)&rcrp->rcr_desc;
	bzero((caddr_t)dmap->kaddrp, dmap->alength);

	status = nxge_rxdma_start_channel(nxgep, channel,
			rbrp, rcrp, mboxp);
	if (status != NXGE_OK) {
		goto nxge_rxdma_fixup_channel_fail;
	}
	if (status != NXGE_OK) {
		goto nxge_rxdma_fixup_channel_fail;
	}

nxge_rxdma_fixup_channel_fail:
	NXGE_DEBUG_MSG((nxgep, RX_CTL,
		"==> nxge_rxdma_fixup_channel: failed (0x%08x)", status));

	NXGE_DEBUG_MSG((nxgep, RX_CTL, "<== nxge_rxdma_fixup_channel"));
}

int
nxge_rxdma_get_ring_index(p_nxge_t nxgep, uint16_t channel)
{
	int			i, ndmas;
	uint16_t		rdc;
	p_rx_rbr_rings_t 	rx_rbr_rings;
	p_rx_rbr_ring_t		*rbr_rings;

	NXGE_DEBUG_MSG((nxgep, RX_CTL,
		"==> nxge_rxdma_get_ring_index: channel %d", channel));

	rx_rbr_rings = nxgep->rx_rbr_rings;
	if (rx_rbr_rings == NULL) {
		NXGE_DEBUG_MSG((nxgep, RX_CTL,
			"<== nxge_rxdma_get_ring_index: NULL ring pointer"));
		return (-1);
	}
	ndmas = rx_rbr_rings->ndmas;
	if (!ndmas) {
		NXGE_DEBUG_MSG((nxgep, RX_CTL,
			"<== nxge_rxdma_get_ring_index: no channel"));
		return (-1);
	}

	NXGE_DEBUG_MSG((nxgep, RX_CTL,
		"==> nxge_rxdma_get_ring_index (ndmas %d)", ndmas));

	rbr_rings = rx_rbr_rings->rbr_rings;
	for (i = 0; i < ndmas; i++) {
		rdc = rbr_rings[i]->rdc;
		if (channel == rdc) {
			NXGE_DEBUG_MSG((nxgep, RX_CTL,
				"==> nxge_rxdma_get_rbr_ring: "
				"channel %d (index %d) "
				"ring %d", channel, i,
				rbr_rings[i]));
			return (i);
		}
	}

	NXGE_DEBUG_MSG((nxgep, RX_CTL,
		"<== nxge_rxdma_get_rbr_ring_index: not found"));

	return (-1);
}

p_rx_rbr_ring_t
nxge_rxdma_get_rbr_ring(p_nxge_t nxgep, uint16_t channel)
{
	int			i, ndmas;
	uint16_t		rdc;
	p_rx_rbr_rings_t 	rx_rbr_rings;
	p_rx_rbr_ring_t		*rbr_rings;

	NXGE_DEBUG_MSG((nxgep, RX_CTL,
		"==> nxge_rxdma_get_rbr_ring: channel %d", channel));

	rx_rbr_rings = nxgep->rx_rbr_rings;
	if (rx_rbr_rings == NULL) {
		NXGE_DEBUG_MSG((nxgep, RX_CTL,
			"<== nxge_rxdma_get_rbr_ring: NULL ring pointer"));
		return (NULL);
	}
	ndmas = rx_rbr_rings->ndmas;
	if (!ndmas) {
		NXGE_DEBUG_MSG((nxgep, RX_CTL,
			"<== nxge_rxdma_get_rbr_ring: no channel"));
		return (NULL);
	}

	NXGE_DEBUG_MSG((nxgep, RX_CTL,
		"==> nxge_rxdma_get_ring (ndmas %d)", ndmas));

	rbr_rings = rx_rbr_rings->rbr_rings;
	for (i = 0; i < ndmas; i++) {
		rdc = rbr_rings[i]->rdc;
		if (channel == rdc) {
			NXGE_DEBUG_MSG((nxgep, RX_CTL,
				"==> nxge_rxdma_get_rbr_ring: channel %d "
				"ring $%p", channel, rbr_rings[i]));
			return (rbr_rings[i]);
		}
	}

	NXGE_DEBUG_MSG((nxgep, RX_CTL,
		"<== nxge_rxdma_get_rbr_ring: not found"));

	return (NULL);
}

p_rx_rcr_ring_t
nxge_rxdma_get_rcr_ring(p_nxge_t nxgep, uint16_t channel)
{
	int			i, ndmas;
	uint16_t		rdc;
	p_rx_rcr_rings_t 	rx_rcr_rings;
	p_rx_rcr_ring_t		*rcr_rings;

	NXGE_DEBUG_MSG((nxgep, RX_CTL,
		"==> nxge_rxdma_get_rcr_ring: channel %d", channel));

	rx_rcr_rings = nxgep->rx_rcr_rings;
	if (rx_rcr_rings == NULL) {
		NXGE_DEBUG_MSG((nxgep, RX_CTL,
			"<== nxge_rxdma_get_rcr_ring: NULL ring pointer"));
		return (NULL);
	}
	ndmas = rx_rcr_rings->ndmas;
	if (!ndmas) {
		NXGE_DEBUG_MSG((nxgep, RX_CTL,
			"<== nxge_rxdma_get_rcr_ring: no channel"));
		return (NULL);
	}

	NXGE_DEBUG_MSG((nxgep, RX_CTL,
		"==> nxge_rxdma_get_rcr_ring (ndmas %d)", ndmas));

	rcr_rings = rx_rcr_rings->rcr_rings;
	for (i = 0; i < ndmas; i++) {
		rdc = rcr_rings[i]->rdc;
		if (channel == rdc) {
			NXGE_DEBUG_MSG((nxgep, RX_CTL,
				"==> nxge_rxdma_get_rcr_ring: channel %d "
				"ring $%p", channel, rcr_rings[i]));
			return (rcr_rings[i]);
		}
	}

	NXGE_DEBUG_MSG((nxgep, RX_CTL,
		"<== nxge_rxdma_get_rcr_ring: not found"));

	return (NULL);
}

/*
 * Static functions start here.
 */
static p_rx_msg_t
nxge_allocb(size_t size, uint32_t pri, p_nxge_dma_common_t dmabuf_p)
{
	p_rx_msg_t nxge_mp 		= NULL;
	p_nxge_dma_common_t		dmamsg_p;
	uchar_t 			*buffer;

	nxge_mp = KMEM_ZALLOC(sizeof (rx_msg_t), KM_NOSLEEP);
	if (nxge_mp == NULL) {
		NXGE_ERROR_MSG((NULL, NXGE_ERR_CTL,
			"Allocation of a rx msg failed."));
		goto nxge_allocb_exit;
	}

	nxge_mp->use_buf_pool = B_FALSE;
	if (dmabuf_p) {
		nxge_mp->use_buf_pool = B_TRUE;
		dmamsg_p = (p_nxge_dma_common_t)&nxge_mp->buf_dma;
		*dmamsg_p = *dmabuf_p;
		dmamsg_p->nblocks = 1;
		dmamsg_p->block_size = size;
		dmamsg_p->alength = size;
		buffer = (uchar_t *)dmabuf_p->kaddrp;

		dmabuf_p->kaddrp = (void *)
				((char *)dmabuf_p->kaddrp + size);
		dmabuf_p->ioaddr_pp = (void *)
				((char *)dmabuf_p->ioaddr_pp + size);
		dmabuf_p->alength -= size;
		dmabuf_p->offset += size;
		dmabuf_p->dma_cookie.dmac_laddress += size;
		dmabuf_p->dma_cookie.dmac_size -= size;

	} else {
		buffer = KMEM_ALLOC(size, KM_NOSLEEP);
		if (buffer == NULL) {
			NXGE_ERROR_MSG((NULL, NXGE_ERR_CTL,
				"Allocation of a receive page failed."));
			goto nxge_allocb_fail1;
		}
	}

	nxge_mp->rx_mblk_p = desballoc(buffer, size, pri, &nxge_mp->freeb);
	if (nxge_mp->rx_mblk_p == NULL) {
		NXGE_ERROR_MSG((NULL, NXGE_ERR_CTL, "desballoc failed."));
		goto nxge_allocb_fail2;
	}

	nxge_mp->buffer = buffer;
	nxge_mp->block_size = size;
	nxge_mp->freeb.free_func = (void (*)())nxge_freeb;
	nxge_mp->freeb.free_arg = (caddr_t)nxge_mp;
	nxge_mp->ref_cnt = 1;
	nxge_mp->free = B_TRUE;
	nxge_mp->rx_use_bcopy = B_FALSE;

	atomic_inc_32(&nxge_mblks_pending);

	goto nxge_allocb_exit;

nxge_allocb_fail2:
	if (!nxge_mp->use_buf_pool) {
		KMEM_FREE(buffer, size);
	}

nxge_allocb_fail1:
	KMEM_FREE(nxge_mp, sizeof (rx_msg_t));
	nxge_mp = NULL;

nxge_allocb_exit:
	return (nxge_mp);
}

p_mblk_t
nxge_dupb(p_rx_msg_t nxge_mp, uint_t offset, size_t size)
{
	p_mblk_t mp;

	NXGE_DEBUG_MSG((NULL, MEM_CTL, "==> nxge_dupb"));
	NXGE_DEBUG_MSG((NULL, MEM_CTL, "nxge_mp = $%p "
		"offset = 0x%08X "
		"size = 0x%08X",
		nxge_mp, offset, size));

	mp = desballoc(&nxge_mp->buffer[offset], size,
				0, &nxge_mp->freeb);
	if (mp == NULL) {
		NXGE_DEBUG_MSG((NULL, RX_CTL, "desballoc failed"));
		goto nxge_dupb_exit;
	}
	atomic_inc_32(&nxge_mp->ref_cnt);
	atomic_inc_32(&nxge_mblks_pending);


nxge_dupb_exit:
	NXGE_DEBUG_MSG((NULL, MEM_CTL, "<== nxge_dupb mp = $%p",
		nxge_mp));
	return (mp);
}

p_mblk_t
nxge_dupb_bcopy(p_rx_msg_t nxge_mp, uint_t offset, size_t size)
{
	p_mblk_t mp;
	uchar_t *dp;

	mp = allocb(size + NXGE_RXBUF_EXTRA, 0);
	if (mp == NULL) {
		NXGE_DEBUG_MSG((NULL, RX_CTL, "desballoc failed"));
		goto nxge_dupb_bcopy_exit;
	}
	dp = mp->b_rptr = mp->b_rptr + NXGE_RXBUF_EXTRA;
	bcopy((void *)&nxge_mp->buffer[offset], dp, size);
	mp->b_wptr = dp + size;

nxge_dupb_bcopy_exit:
	NXGE_DEBUG_MSG((NULL, MEM_CTL, "<== nxge_dupb mp = $%p",
		nxge_mp));
	return (mp);
}

void nxge_post_page(p_nxge_t nxgep, p_rx_rbr_ring_t rx_rbr_p,
	p_rx_msg_t rx_msg_p);

void
nxge_post_page(p_nxge_t nxgep, p_rx_rbr_ring_t rx_rbr_p, p_rx_msg_t rx_msg_p)
{

	npi_handle_t		handle;

	NXGE_DEBUG_MSG((nxgep, RX_CTL, "==> nxge_post_page"));

	/* Reuse this buffer */
	rx_msg_p->free = B_FALSE;
	rx_msg_p->cur_usage_cnt = 0;
	rx_msg_p->max_usage_cnt = 0;
	rx_msg_p->pkt_buf_size = 0;

	if (rx_rbr_p->rbr_use_bcopy) {
		rx_msg_p->rx_use_bcopy = B_FALSE;
		atomic_dec_32(&rx_rbr_p->rbr_consumed);
	}

	/*
	 * Get the rbr header pointer and its offset index.
	 */
	MUTEX_ENTER(&rx_rbr_p->post_lock);


	rx_rbr_p->rbr_wr_index =  ((rx_rbr_p->rbr_wr_index + 1) &
					    rx_rbr_p->rbr_wrap_mask);
	rx_rbr_p->rbr_desc_vp[rx_rbr_p->rbr_wr_index] = rx_msg_p->shifted_addr;
	MUTEX_EXIT(&rx_rbr_p->post_lock);
	handle = NXGE_DEV_NPI_HANDLE(nxgep);
	npi_rxdma_rdc_rbr_kick(handle, rx_rbr_p->rdc, 1);

	NXGE_DEBUG_MSG((nxgep, RX_CTL,
		"<== nxge_post_page (channel %d post_next_index %d)",
		rx_rbr_p->rdc, rx_rbr_p->rbr_wr_index));

	NXGE_DEBUG_MSG((nxgep, RX_CTL, "<== nxge_post_page"));
}

void
nxge_freeb(p_rx_msg_t rx_msg_p)
{
	size_t size;
	uchar_t *buffer = NULL;
	int ref_cnt;
	boolean_t free_state = B_FALSE;

	NXGE_DEBUG_MSG((NULL, MEM2_CTL, "==> nxge_freeb"));
	NXGE_DEBUG_MSG((NULL, MEM2_CTL,
		"nxge_freeb:rx_msg_p = $%p (block pending %d)",
		rx_msg_p, nxge_mblks_pending));

	atomic_dec_32(&nxge_mblks_pending);
	/*
	 * First we need to get the free state, then
	 * atomic decrement the reference count to prevent
	 * the race condition with the interrupt thread that
	 * is processing a loaned up buffer block.
	 */
	free_state = rx_msg_p->free;
	ref_cnt = atomic_add_32_nv(&rx_msg_p->ref_cnt, -1);
	if (!ref_cnt) {
		buffer = rx_msg_p->buffer;
		size = rx_msg_p->block_size;
		NXGE_DEBUG_MSG((NULL, MEM2_CTL, "nxge_freeb: "
			"will free: rx_msg_p = $%p (block pending %d)",
			rx_msg_p, nxge_mblks_pending));

		if (!rx_msg_p->use_buf_pool) {
			KMEM_FREE(buffer, size);
		}

		KMEM_FREE(rx_msg_p, sizeof (rx_msg_t));
		return;
	}

	/*
	 * Repost buffer.
	 */
	if (free_state && (ref_cnt == 1)) {
		NXGE_DEBUG_MSG((NULL, RX_CTL,
		    "nxge_freeb: post page $%p:", rx_msg_p));
		nxge_post_page(rx_msg_p->nxgep, rx_msg_p->rx_rbr_p,
		    rx_msg_p);
	}

	NXGE_DEBUG_MSG((NULL, MEM2_CTL, "<== nxge_freeb"));
}

uint_t
nxge_rx_intr(void *arg1, void *arg2)
{
	p_nxge_ldv_t		ldvp = (p_nxge_ldv_t)arg1;
	p_nxge_t		nxgep = (p_nxge_t)arg2;
	p_nxge_ldg_t		ldgp;
	uint8_t			channel;
	npi_handle_t		handle;
	rx_dma_ctl_stat_t	cs;

#ifdef	NXGE_DEBUG
	rxdma_cfig1_t		cfg;
#endif
	uint_t 			serviced = DDI_INTR_UNCLAIMED;

	if (ldvp == NULL) {
		NXGE_DEBUG_MSG((NULL, INT_CTL,
			"<== nxge_rx_intr: arg2 $%p arg1 $%p",
			nxgep, ldvp));

		return (DDI_INTR_CLAIMED);
	}

	if (arg2 == NULL || (void *)ldvp->nxgep != arg2) {
		nxgep = ldvp->nxgep;
	}
	NXGE_DEBUG_MSG((nxgep, RX_CTL,
		"==> nxge_rx_intr: arg2 $%p arg1 $%p",
		nxgep, ldvp));

	/*
	 * This interrupt handler is for a specific
	 * receive dma channel.
	 */
	handle = NXGE_DEV_NPI_HANDLE(nxgep);
	/*
	 * Get the control and status for this channel.
	 */
	channel = ldvp->channel;
	ldgp = ldvp->ldgp;
	RXDMA_REG_READ64(handle, RX_DMA_CTL_STAT_REG, channel, &cs.value);

	NXGE_DEBUG_MSG((nxgep, RX_CTL, "==> nxge_rx_intr:channel %d "
		"cs 0x%016llx rcrto 0x%x rcrthres %x",
		channel,
		cs.value,
		cs.bits.hdw.rcrto,
		cs.bits.hdw.rcrthres));

	nxge_rx_pkts_vring(nxgep, ldvp->vdma_index, ldvp, cs);
	serviced = DDI_INTR_CLAIMED;

	/* error events. */
	if (cs.value & RX_DMA_CTL_STAT_ERROR) {
		(void) nxge_rx_err_evnts(nxgep, ldvp->vdma_index, ldvp, cs);
	}

nxge_intr_exit:


	/*
	 * Enable the mailbox update interrupt if we want
	 * to use mailbox. We probably don't need to use
	 * mailbox as it only saves us one pio read.
	 * Also write 1 to rcrthres and rcrto to clear
	 * these two edge triggered bits.
	 */

	cs.value &= RX_DMA_CTL_STAT_WR1C;
	cs.bits.hdw.mex = 1;
	RXDMA_REG_WRITE64(handle, RX_DMA_CTL_STAT_REG, channel,
			cs.value);

	/*
	 * Rearm this logical group if this is a single device
	 * group.
	 */
	if (ldgp->nldvs == 1) {
		ldgimgm_t		mgm;
		mgm.value = 0;
		mgm.bits.ldw.arm = 1;
		mgm.bits.ldw.timer = ldgp->ldg_timer;
		NXGE_REG_WR64(handle,
			    LDGIMGN_REG + LDSV_OFFSET(ldgp->ldg),
			    mgm.value);
	}

	NXGE_DEBUG_MSG((nxgep, RX_CTL, "<== nxge_rx_intr: serviced %d",
		serviced));
	return (serviced);
}

/*
 * Process the packets received in the specified logical device
 * and pass up a chain of message blocks to the upper layer.
 */
static void
nxge_rx_pkts_vring(p_nxge_t nxgep, uint_t vindex, p_nxge_ldv_t ldvp,
				    rx_dma_ctl_stat_t cs)
{
	p_mblk_t		mp;
	p_rx_rcr_ring_t		rcrp;

	NXGE_DEBUG_MSG((nxgep, RX_CTL, "==> nxge_rx_pkts_vring"));
	if ((mp = nxge_rx_pkts(nxgep, vindex, ldvp, &rcrp, cs)) == NULL) {
		NXGE_DEBUG_MSG((nxgep, RX_CTL,
			"<== nxge_rx_pkts_vring: no mp"));
		return;
	}

	NXGE_DEBUG_MSG((nxgep, RX_CTL, "==> nxge_rx_pkts_vring: $%p",
		mp));

#ifdef  NXGE_DEBUG
		NXGE_DEBUG_MSG((nxgep, RX_CTL,
			"==> nxge_rx_pkts_vring:calling mac_rx "
			"LEN %d mp $%p mp->b_cont $%p mp->b_next $%p rcrp $%p "
			"mac_handle $%p",
			mp->b_wptr - mp->b_rptr,
			mp, mp->b_cont, mp->b_next,
			rcrp, rcrp->rcr_mac_handle));

		NXGE_DEBUG_MSG((nxgep, RX_CTL,
			"==> nxge_rx_pkts_vring: dump packets "
			"(mp $%p b_rptr $%p b_wptr $%p):\n %s",
			mp,
			mp->b_rptr,
			mp->b_wptr,
			nxge_dump_packet((char *)mp->b_rptr,
			mp->b_wptr - mp->b_rptr)));
		if (mp->b_cont) {
			NXGE_DEBUG_MSG((nxgep, RX_CTL,
				"==> nxge_rx_pkts_vring: dump b_cont packets "
				"(mp->b_cont $%p b_rptr $%p b_wptr $%p):\n %s",
				mp->b_cont,
				mp->b_cont->b_rptr,
				mp->b_cont->b_wptr,
				nxge_dump_packet((char *)mp->b_cont->b_rptr,
				mp->b_cont->b_wptr - mp->b_cont->b_rptr)));
		}
		if (mp->b_next) {
			NXGE_DEBUG_MSG((nxgep, RX_CTL,
				"==> nxge_rx_pkts_vring: dump next packets "
				"(b_rptr $%p): %s",
				mp->b_next->b_rptr,
				nxge_dump_packet((char *)mp->b_next->b_rptr,
				mp->b_next->b_wptr - mp->b_next->b_rptr)));
		}
#endif

	mac_rx(nxgep->mach, rcrp->rcr_mac_handle, mp);
}


/*
 * This routine is the main packet receive processing function.
 * It gets the packet type, error code, and buffer related
 * information from the receive completion entry.
 * How many completion entries to process is based on the number of packets
 * queued by the hardware, a hardware maintained tail pointer
 * and a configurable receive packet count.
 *
 * A chain of message blocks will be created as result of processing
 * the completion entries. This chain of message blocks will be returned and
 * a hardware control status register will be updated with the number of
 * packets were removed from the hardware queue.
 *
 */
mblk_t *
nxge_rx_pkts(p_nxge_t nxgep, uint_t vindex, p_nxge_ldv_t ldvp,
    p_rx_rcr_ring_t *rcrp, rx_dma_ctl_stat_t cs)
{
	npi_handle_t		handle;
	uint8_t			channel;
	p_rx_rcr_rings_t	rx_rcr_rings;
	p_rx_rcr_ring_t		rcr_p;
	uint32_t		comp_rd_index;
	p_rcr_entry_t		rcr_desc_rd_head_p;
	p_rcr_entry_t		rcr_desc_rd_head_pp;
	p_mblk_t		nmp, mp_cont, head_mp, *tail_mp;
	uint16_t		qlen, nrcr_read, npkt_read;
	uint32_t qlen_hw;
	boolean_t		multi;
	rcrcfig_b_t rcr_cfg_b;
#if defined(_BIG_ENDIAN)
	npi_status_t		rs = NPI_SUCCESS;
#endif

	NXGE_DEBUG_MSG((nxgep, RX_CTL, "==> nxge_rx_pkts:vindex %d "
		"channel %d", vindex, ldvp->channel));

	if (!(nxgep->drv_state & STATE_HW_INITIALIZED)) {
		return (NULL);
	}
	handle = NXGE_DEV_NPI_HANDLE(nxgep);
	rx_rcr_rings = nxgep->rx_rcr_rings;
	rcr_p = rx_rcr_rings->rcr_rings[vindex];
	channel = rcr_p->rdc;
	if (channel != ldvp->channel) {
		NXGE_DEBUG_MSG((nxgep, RX_CTL, "==> nxge_rx_pkts:index %d "
			"channel %d, and rcr channel %d not matched.",
			vindex, ldvp->channel, channel));
		return (NULL);
	}

	NXGE_DEBUG_MSG((nxgep, RX_CTL,
		"==> nxge_rx_pkts: START: rcr channel %d "
		"head_p $%p head_pp $%p  index %d ",
		channel, rcr_p->rcr_desc_rd_head_p,
		rcr_p->rcr_desc_rd_head_pp,
		rcr_p->comp_rd_index));


#if !defined(_BIG_ENDIAN)
	qlen = RXDMA_REG_READ32(handle, RCRSTAT_A_REG, channel) & 0xffff;
#else
	rs = npi_rxdma_rdc_rcr_qlen_get(handle, channel, &qlen);
	if (rs != NPI_SUCCESS) {
		NXGE_DEBUG_MSG((nxgep, RX_CTL, "==> nxge_rx_pkts:index %d "
		"channel %d, get qlen failed 0x%08x",
		vindex, ldvp->channel, rs));
		return (NULL);
	}
#endif
	NXGE_DEBUG_MSG((nxgep, RX_CTL, "==> nxge_rx_pkts:rcr channel %d "
		"qlen %d", channel, qlen));



	if (!qlen) {
		NXGE_DEBUG_MSG((nxgep, RX_CTL,
			"==> nxge_rx_pkts:rcr channel %d "
			"qlen %d (no pkts)", channel, qlen));

		return (NULL);
	}

	comp_rd_index = rcr_p->comp_rd_index;

	rcr_desc_rd_head_p = rcr_p->rcr_desc_rd_head_p;
	rcr_desc_rd_head_pp = rcr_p->rcr_desc_rd_head_pp;
	nrcr_read = npkt_read = 0;

	/*
	 * Number of packets queued
	 * (The jumbo or multi packet will be counted as only one
	 *  packets and it may take up more than one completion entry).
	 */
	qlen_hw = (qlen < nxge_max_rx_pkts) ?
		qlen : nxge_max_rx_pkts;
	head_mp = NULL;
	tail_mp = &head_mp;
	nmp = mp_cont = NULL;
	multi = B_FALSE;

	while (qlen_hw) {

#ifdef NXGE_DEBUG
		nxge_dump_rcr_entry(nxgep, rcr_desc_rd_head_p);
#endif
		/*
		 * Process one completion ring entry.
		 */
		nxge_receive_packet(nxgep,
			rcr_p, rcr_desc_rd_head_p, &multi, &nmp, &mp_cont);

		/*
		 * message chaining modes
		 */
		if (nmp) {
			nmp->b_next = NULL;
			if (!multi && !mp_cont) { /* frame fits a partition */
				*tail_mp = nmp;
				tail_mp = &nmp->b_next;
				nmp = NULL;
			} else if (multi && !mp_cont) { /* first segment */
				*tail_mp = nmp;
				tail_mp = &nmp->b_cont;
			} else if (multi && mp_cont) {	/* mid of multi segs */
				*tail_mp = mp_cont;
				tail_mp = &mp_cont->b_cont;
			} else if (!multi && mp_cont) { /* last segment */
				*tail_mp = mp_cont;
				tail_mp = &nmp->b_next;
				nmp = NULL;
			}
		}
		NXGE_DEBUG_MSG((nxgep, RX_CTL,
			"==> nxge_rx_pkts: loop: rcr channel %d "
			"before updating: multi %d "
			"nrcr_read %d "
			"npk read %d "
			"head_pp $%p  index %d ",
			channel,
			multi,
			nrcr_read, npkt_read, rcr_desc_rd_head_pp,
			comp_rd_index));

		if (!multi) {
			qlen_hw--;
			npkt_read++;
		}

		/*
		 * Update the next read entry.
		 */
		comp_rd_index = NEXT_ENTRY(comp_rd_index,
					rcr_p->comp_wrap_mask);

		rcr_desc_rd_head_p = NEXT_ENTRY_PTR(rcr_desc_rd_head_p,
				rcr_p->rcr_desc_first_p,
				rcr_p->rcr_desc_last_p);

		nrcr_read++;

		NXGE_DEBUG_MSG((nxgep, RX_CTL,
			"<== nxge_rx_pkts: (SAM, process one packet) "
			"nrcr_read %d",
			nrcr_read));
		NXGE_DEBUG_MSG((nxgep, RX_CTL,
			"==> nxge_rx_pkts: loop: rcr channel %d "
			"multi %d "
			"nrcr_read %d "
			"npk read %d "
			"head_pp $%p  index %d ",
			channel,
			multi,
			nrcr_read, npkt_read, rcr_desc_rd_head_pp,
			comp_rd_index));

	}

	rcr_p->rcr_desc_rd_head_pp = rcr_desc_rd_head_pp;
	rcr_p->comp_rd_index = comp_rd_index;
	rcr_p->rcr_desc_rd_head_p = rcr_desc_rd_head_p;

	if ((nxgep->intr_timeout != rcr_p->intr_timeout) ||
		(nxgep->intr_threshold != rcr_p->intr_threshold)) {
		rcr_p->intr_timeout = nxgep->intr_timeout;
		rcr_p->intr_threshold = nxgep->intr_threshold;
		rcr_cfg_b.value = 0x0ULL;
		if (rcr_p->intr_timeout)
			rcr_cfg_b.bits.ldw.entout = 1;
		rcr_cfg_b.bits.ldw.timeout = rcr_p->intr_timeout;
		rcr_cfg_b.bits.ldw.pthres = rcr_p->intr_threshold;
		RXDMA_REG_WRITE64(handle, RCRCFIG_B_REG,
				    channel, rcr_cfg_b.value);
	}

	cs.bits.ldw.pktread = npkt_read;
	cs.bits.ldw.ptrread = nrcr_read;
	RXDMA_REG_WRITE64(handle, RX_DMA_CTL_STAT_REG,
			    channel, cs.value);
	NXGE_DEBUG_MSG((nxgep, RX_CTL,
		"==> nxge_rx_pkts: EXIT: rcr channel %d "
		"head_pp $%p  index %016llx ",
		channel,
		rcr_p->rcr_desc_rd_head_pp,
		rcr_p->comp_rd_index));
	/*
	 * Update RCR buffer pointer read and number of packets
	 * read.
	 */

	*rcrp = rcr_p;
	NXGE_DEBUG_MSG((nxgep, RX_CTL, "<== nxge_rx_pkts"));
	return (head_mp);
}

void
nxge_receive_packet(p_nxge_t nxgep,
    p_rx_rcr_ring_t rcr_p, p_rcr_entry_t rcr_desc_rd_head_p,
    boolean_t *multi_p, mblk_t **mp, mblk_t **mp_cont)
{
	p_mblk_t		nmp = NULL;
	uint64_t		multi;
	uint64_t		dcf_err;
	uint8_t			channel;

	boolean_t		first_entry = B_TRUE;
	boolean_t		is_tcp_udp = B_FALSE;
	boolean_t		buffer_free = B_FALSE;
	boolean_t		error_send_up = B_FALSE;
	uint8_t			error_type;
	uint16_t		l2_len;
	uint16_t		skip_len;
	uint8_t			pktbufsz_type;
	uint16_t		pktbufsz;
	uint64_t		rcr_entry;
	uint64_t		*pkt_buf_addr_pp;
	uint64_t		*pkt_buf_addr_p;
	uint32_t		buf_offset;
	uint32_t		bsize;
	uint32_t		error_disp_cnt;
	uint32_t		msg_index;
	p_rx_rbr_ring_t		rx_rbr_p;
	p_rx_msg_t 		*rx_msg_ring_p;
	p_rx_msg_t		rx_msg_p;
	uint16_t		sw_offset_bytes = 0, hdr_size = 0;
	nxge_status_t		status = NXGE_OK;
	boolean_t		is_valid = B_FALSE;
	p_nxge_rx_ring_stats_t	rdc_stats;
	uint32_t		bytes_read;
	uint64_t		pkt_type;
	uint64_t		frag;
#ifdef	NXGE_DEBUG
	int			dump_len;
#endif
	NXGE_DEBUG_MSG((nxgep, RX2_CTL, "==> nxge_receive_packet"));
	first_entry = (*mp == NULL) ? B_TRUE : B_FALSE;

	rcr_entry = *((uint64_t *)rcr_desc_rd_head_p);

	multi = (rcr_entry & RCR_MULTI_MASK);
	dcf_err = (rcr_entry & RCR_DCF_ERROR_MASK);
	pkt_type = (rcr_entry & RCR_PKT_TYPE_MASK);

	error_type = ((rcr_entry & RCR_ERROR_MASK) >> RCR_ERROR_SHIFT);
	frag = (rcr_entry & RCR_FRAG_MASK);

	l2_len = ((rcr_entry & RCR_L2_LEN_MASK) >> RCR_L2_LEN_SHIFT);

	pktbufsz_type = ((rcr_entry & RCR_PKTBUFSZ_MASK) >>
				RCR_PKTBUFSZ_SHIFT);

	pkt_buf_addr_pp = (uint64_t *)((rcr_entry & RCR_PKT_BUF_ADDR_MASK) <<
			RCR_PKT_BUF_ADDR_SHIFT);

	channel = rcr_p->rdc;

	NXGE_DEBUG_MSG((nxgep, RX2_CTL,
		"==> nxge_receive_packet: entryp $%p entry 0x%0llx "
		"pkt_buf_addr_pp $%p l2_len %d multi 0x%llx "
		"error_type 0x%x pkt_type 0x%x  "
		"pktbufsz_type %d ",
		rcr_desc_rd_head_p,
		rcr_entry, pkt_buf_addr_pp, l2_len,
		multi,
		error_type,
		pkt_type,
		pktbufsz_type));

	NXGE_DEBUG_MSG((nxgep, RX2_CTL,
		"==> nxge_receive_packet: entryp $%p entry 0x%0llx "
		"pkt_buf_addr_pp $%p l2_len %d multi 0x%llx "
		"error_type 0x%x pkt_type 0x%x ", rcr_desc_rd_head_p,
		rcr_entry, pkt_buf_addr_pp, l2_len,
		multi,
		error_type,
		pkt_type));

	NXGE_DEBUG_MSG((nxgep, RX2_CTL,
		"==> (rbr) nxge_receive_packet: entry 0x%0llx "
		"full pkt_buf_addr_pp $%p l2_len %d",
		rcr_entry, pkt_buf_addr_pp, l2_len));

	/* get the stats ptr */
	rdc_stats = rcr_p->rdc_stats;

	if (!l2_len) {

		NXGE_DEBUG_MSG((nxgep, RX_CTL,
			"<== nxge_receive_packet: failed: l2 length is 0."));
		return;
	}

	/* Hardware sends us 4 bytes of CRC as no stripping is done.  */
	l2_len -= ETHERFCSL;

	/* shift 6 bits to get the full io address */
	pkt_buf_addr_pp = (uint64_t *)((uint64_t)pkt_buf_addr_pp <<
				RCR_PKT_BUF_ADDR_SHIFT_FULL);
	NXGE_DEBUG_MSG((nxgep, RX2_CTL,
		"==> (rbr) nxge_receive_packet: entry 0x%0llx "
		"full pkt_buf_addr_pp $%p l2_len %d",
		rcr_entry, pkt_buf_addr_pp, l2_len));

	rx_rbr_p = rcr_p->rx_rbr_p;
	rx_msg_ring_p = rx_rbr_p->rx_msg_ring;

	if (first_entry) {
		hdr_size = (rcr_p->full_hdr_flag ? RXDMA_HDR_SIZE_FULL :
			RXDMA_HDR_SIZE_DEFAULT);

		NXGE_DEBUG_MSG((nxgep, RX_CTL,
			"==> nxge_receive_packet: first entry 0x%016llx "
			"pkt_buf_addr_pp $%p l2_len %d hdr %d",
			rcr_entry, pkt_buf_addr_pp, l2_len,
			hdr_size));
	}

	MUTEX_ENTER(&rcr_p->lock);
	MUTEX_ENTER(&rx_rbr_p->lock);

	bytes_read = rcr_p->rcvd_pkt_bytes;

	NXGE_DEBUG_MSG((nxgep, RX_CTL,
		"==> (rbr 1) nxge_receive_packet: entry 0x%0llx "
		"full pkt_buf_addr_pp $%p l2_len %d",
		rcr_entry, pkt_buf_addr_pp, l2_len));

	/*
	 * Packet buffer address in the completion entry points
	 * to the starting buffer address (offset 0).
	 * Use the starting buffer address to locate the corresponding
	 * kernel address.
	 */
	status = nxge_rxbuf_pp_to_vp(nxgep, rx_rbr_p,
			pktbufsz_type, pkt_buf_addr_pp, &pkt_buf_addr_p,
			&buf_offset,
			&msg_index);

	NXGE_DEBUG_MSG((nxgep, RX_CTL,
		"==> (rbr 2) nxge_receive_packet: entry 0x%0llx "
		"full pkt_buf_addr_pp $%p l2_len %d",
		rcr_entry, pkt_buf_addr_pp, l2_len));

	if (status != NXGE_OK) {
		MUTEX_EXIT(&rx_rbr_p->lock);
		MUTEX_EXIT(&rcr_p->lock);
		NXGE_DEBUG_MSG((nxgep, RX_CTL,
			"<== nxge_receive_packet: found vaddr failed %d",
				status));
		return;
	}

	NXGE_DEBUG_MSG((nxgep, RX2_CTL,
		"==> (rbr 3) nxge_receive_packet: entry 0x%0llx "
		"full pkt_buf_addr_pp $%p l2_len %d",
		rcr_entry, pkt_buf_addr_pp, l2_len));

	NXGE_DEBUG_MSG((nxgep, RX2_CTL,
		"==> (rbr 4 msgindex %d) nxge_receive_packet: entry 0x%0llx "
		"full pkt_buf_addr_pp $%p l2_len %d",
		msg_index, rcr_entry, pkt_buf_addr_pp, l2_len));

	rx_msg_p = rx_msg_ring_p[msg_index];

	NXGE_DEBUG_MSG((nxgep, RX2_CTL,
		"==> (rbr 4 msgindex %d) nxge_receive_packet: entry 0x%0llx "
		"full pkt_buf_addr_pp $%p l2_len %d",
		msg_index, rcr_entry, pkt_buf_addr_pp, l2_len));

	switch (pktbufsz_type) {
	case RCR_PKTBUFSZ_0:
		bsize = rx_rbr_p->pkt_buf_size0_bytes;
		NXGE_DEBUG_MSG((nxgep, RX2_CTL,
			"==> nxge_receive_packet: 0 buf %d", bsize));
		break;
	case RCR_PKTBUFSZ_1:
		bsize = rx_rbr_p->pkt_buf_size1_bytes;
		NXGE_DEBUG_MSG((nxgep, RX2_CTL,
			"==> nxge_receive_packet: 1 buf %d", bsize));
		break;
	case RCR_PKTBUFSZ_2:
		bsize = rx_rbr_p->pkt_buf_size2_bytes;
		NXGE_DEBUG_MSG((nxgep, RX_CTL,
			"==> nxge_receive_packet: 2 buf %d", bsize));
		break;
	case RCR_SINGLE_BLOCK:
		bsize = rx_msg_p->block_size;
		NXGE_DEBUG_MSG((nxgep, RX2_CTL,
			"==> nxge_receive_packet: single %d", bsize));

		break;
	default:
		MUTEX_EXIT(&rx_rbr_p->lock);
		MUTEX_EXIT(&rcr_p->lock);
		return;
	}

	DMA_COMMON_SYNC_OFFSET(rx_msg_p->buf_dma,
		(buf_offset + sw_offset_bytes),
		(hdr_size + l2_len),
		DDI_DMA_SYNC_FORCPU);

	NXGE_DEBUG_MSG((nxgep, RX2_CTL,
		"==> nxge_receive_packet: after first dump:usage count"));

	if (rx_msg_p->cur_usage_cnt == 0) {
		if (rx_rbr_p->rbr_use_bcopy) {
			atomic_inc_32(&rx_rbr_p->rbr_consumed);
			if (rx_rbr_p->rbr_consumed <
					rx_rbr_p->rbr_threshold_hi) {
				if (rx_rbr_p->rbr_threshold_lo == 0 ||
					((rx_rbr_p->rbr_consumed >=
						rx_rbr_p->rbr_threshold_lo) &&
						(rx_rbr_p->rbr_bufsize_type >=
							pktbufsz_type))) {
					rx_msg_p->rx_use_bcopy = B_TRUE;
				}
			} else {
				rx_msg_p->rx_use_bcopy = B_TRUE;
			}
		}
		NXGE_DEBUG_MSG((nxgep, RX2_CTL,
			"==> nxge_receive_packet: buf %d (new block) ",
			bsize));

		rx_msg_p->pkt_buf_size_code = pktbufsz_type;
		rx_msg_p->pkt_buf_size = bsize;
		rx_msg_p->cur_usage_cnt = 1;
		if (pktbufsz_type == RCR_SINGLE_BLOCK) {
			NXGE_DEBUG_MSG((nxgep, RX2_CTL,
				"==> nxge_receive_packet: buf %d "
				"(single block) ",
				bsize));
			/*
			 * Buffer can be reused once the free function
			 * is called.
			 */
			rx_msg_p->max_usage_cnt = 1;
			buffer_free = B_TRUE;
		} else {
			rx_msg_p->max_usage_cnt = rx_msg_p->block_size/bsize;
			if (rx_msg_p->max_usage_cnt == 1) {
				buffer_free = B_TRUE;
			}
		}
	} else {
		rx_msg_p->cur_usage_cnt++;
		if (rx_msg_p->cur_usage_cnt == rx_msg_p->max_usage_cnt) {
			buffer_free = B_TRUE;
		}
	}

	NXGE_DEBUG_MSG((nxgep, RX_CTL,
	    "msgbuf index = %d l2len %d bytes usage %d max_usage %d ",
		msg_index, l2_len,
		rx_msg_p->cur_usage_cnt, rx_msg_p->max_usage_cnt));

	if ((error_type) || (dcf_err)) {
		rdc_stats->ierrors++;
		if (dcf_err) {
			rdc_stats->dcf_err++;
#ifdef	NXGE_DEBUG
			if (!rdc_stats->dcf_err) {
				NXGE_DEBUG_MSG((nxgep, RX_CTL,
				"nxge_receive_packet: channel %d dcf_err rcr"
				" 0x%llx", channel, rcr_entry));
			}
#endif
			NXGE_FM_REPORT_ERROR(nxgep, nxgep->mac.portnum, NULL,
					NXGE_FM_EREPORT_RDMC_DCF_ERR);
		} else {
				/* Update error stats */
			error_disp_cnt = NXGE_ERROR_SHOW_MAX;
			rdc_stats->errlog.compl_err_type = error_type;
			NXGE_FM_REPORT_ERROR(nxgep, nxgep->mac.portnum, NULL,
				    NXGE_FM_EREPORT_RDMC_COMPLETION_ERR);

			switch (error_type) {
				case RCR_L2_ERROR:
					rdc_stats->l2_err++;
					if (rdc_stats->l2_err <
						error_disp_cnt)
						NXGE_ERROR_MSG((nxgep,
						NXGE_ERR_CTL,
						" nxge_receive_packet:"
						" channel %d RCR L2_ERROR",
						channel));
					break;
				case RCR_L4_CSUM_ERROR:
					error_send_up = B_TRUE;
					rdc_stats->l4_cksum_err++;
					if (rdc_stats->l4_cksum_err <
						error_disp_cnt)
						NXGE_ERROR_MSG((nxgep,
						NXGE_ERR_CTL,
							" nxge_receive_packet:"
							" channel %d"
							" RCR L4_CSUM_ERROR",
							channel));
					break;
				case RCR_FFLP_SOFT_ERROR:
					error_send_up = B_TRUE;
					rdc_stats->fflp_soft_err++;
					if (rdc_stats->fflp_soft_err <
						error_disp_cnt)
						NXGE_ERROR_MSG((nxgep,
							NXGE_ERR_CTL,
							" nxge_receive_packet:"
							" channel %d"
							" RCR FFLP_SOFT_ERROR",
							channel));
					break;
				case RCR_ZCP_SOFT_ERROR:
					error_send_up = B_TRUE;
					rdc_stats->fflp_soft_err++;
					if (rdc_stats->zcp_soft_err <
						error_disp_cnt)
						NXGE_ERROR_MSG((nxgep,
							NXGE_ERR_CTL,
							" nxge_receive_packet:"
							" Channel %d"
							" RCR ZCP_SOFT_ERROR",
							channel));
					break;
				default:
					NXGE_ERROR_MSG((nxgep, NXGE_ERR_CTL,
							" nxge_receive_packet:"
							" Channel %d"
							" RCR entry 0x%llx"
							" error 0x%x",
							rcr_entry, channel,
							error_type));
					break;
			}
		}

		/*
		 * Update and repost buffer block if max usage
		 * count is reached.
		 */
		if (error_send_up == B_FALSE) {
			atomic_inc_32(&rx_msg_p->ref_cnt);
			atomic_inc_32(&nxge_mblks_pending);
			if (buffer_free == B_TRUE) {
				rx_msg_p->free = B_TRUE;
			}

			MUTEX_EXIT(&rx_rbr_p->lock);
			MUTEX_EXIT(&rcr_p->lock);
			nxge_freeb(rx_msg_p);
			return;
		}
	}

	NXGE_DEBUG_MSG((nxgep, RX2_CTL,
		"==> nxge_receive_packet: DMA sync second "));

	skip_len = sw_offset_bytes + hdr_size;
	if (!rx_msg_p->rx_use_bcopy) {
		/*
		 * For loaned up buffers, the driver reference count
		 * will be incremented first and then the free state.
		 */
		nmp = nxge_dupb(rx_msg_p, buf_offset, bsize);
	} else {
		nmp = nxge_dupb_bcopy(rx_msg_p, buf_offset + skip_len, l2_len);
		NXGE_DEBUG_MSG((nxgep, RX_CTL,
			"==> nxge_receive_packet: use bcopy "
			"rbr consumed %d "
			"pktbufsz_type %d "
			"offset %d "
			"hdr_size %d l2_len %d "
			"nmp->b_rptr $%p",
			rx_rbr_p->rbr_consumed,
			pktbufsz_type,
			buf_offset, hdr_size, l2_len,
			nmp->b_rptr));
	}
	if (nmp != NULL) {
		pktbufsz = nxge_get_pktbuf_size(nxgep, pktbufsz_type,
			rx_rbr_p->rbr_cfgb);
		if (!rx_msg_p->rx_use_bcopy) {
			if (first_entry) {
				bytes_read = 0;
				nmp->b_rptr = &nmp->b_rptr[skip_len];
				if (l2_len > pktbufsz - skip_len)
					nmp->b_wptr = &nmp->b_rptr[pktbufsz
						- skip_len];
				else
					nmp->b_wptr = &nmp->b_rptr[l2_len];
			} else {
				if (l2_len - bytes_read > pktbufsz)
					nmp->b_wptr = &nmp->b_rptr[pktbufsz];
				else
					nmp->b_wptr =
					    &nmp->b_rptr[l2_len - bytes_read];
			}
			bytes_read += nmp->b_wptr - nmp->b_rptr;
			NXGE_DEBUG_MSG((nxgep, RX_CTL,
				"==> nxge_receive_packet after dupb: "
				"rbr consumed %d "
				"pktbufsz_type %d "
				"nmp $%p rptr $%p wptr $%p "
				"buf_offset %d bzise %d l2_len %d skip_len %d",
				rx_rbr_p->rbr_consumed,
				pktbufsz_type,
				nmp, nmp->b_rptr, nmp->b_wptr,
				buf_offset, bsize, l2_len, skip_len));
		}
	} else {
		cmn_err(CE_WARN, "!nxge_receive_packet: "
			"update stats (error)");
		atomic_inc_32(&rx_msg_p->ref_cnt);
		atomic_inc_32(&nxge_mblks_pending);
		if (buffer_free == B_TRUE) {
			rx_msg_p->free = B_TRUE;
		}
		MUTEX_EXIT(&rx_rbr_p->lock);
		MUTEX_EXIT(&rcr_p->lock);
		nxge_freeb(rx_msg_p);
		return;
	}
	if (buffer_free == B_TRUE) {
		rx_msg_p->free = B_TRUE;
	}

	/*
	 * ERROR, FRAG and PKT_TYPE are only reported
	 * in the first entry.
	 * If a packet is not fragmented and no error bit is set, then
	 * L4 checksum is OK.
	 */
	is_valid = (nmp != NULL);
	rdc_stats->ibytes += l2_len;
	rdc_stats->ipackets++;
	MUTEX_EXIT(&rx_rbr_p->lock);
	MUTEX_EXIT(&rcr_p->lock);

	if (rx_msg_p->free && rx_msg_p->rx_use_bcopy) {
		atomic_inc_32(&rx_msg_p->ref_cnt);
		atomic_inc_32(&nxge_mblks_pending);
		nxge_freeb(rx_msg_p);
	}

	if (is_valid) {
		nmp->b_cont = NULL;
		if (first_entry) {
			*mp = nmp;
			*mp_cont = NULL;
		} else
			*mp_cont = nmp;
	}

	/*
	 * Update stats and hardware checksuming.
	 */
	if (is_valid && !multi) {

		is_tcp_udp = ((pkt_type == RCR_PKT_IS_TCP ||
				pkt_type == RCR_PKT_IS_UDP) ?
					B_TRUE: B_FALSE);

		NXGE_DEBUG_MSG((nxgep, RX_CTL, "==> nxge_receive_packet: "
			"is_valid 0x%x multi 0x%llx pkt %d frag %d error %d",
			is_valid, multi, is_tcp_udp, frag, error_type));

		if (is_tcp_udp && !frag && !error_type) {
			(void) hcksum_assoc(nmp, NULL, NULL, 0, 0, 0, 0,
				HCK_FULLCKSUM_OK | HCK_FULLCKSUM, 0);
			NXGE_DEBUG_MSG((nxgep, RX_CTL,
				"==> nxge_receive_packet: Full tcp/udp cksum "
				"is_valid 0x%x multi 0x%llx pkt %d frag %d "
				"error %d",
				is_valid, multi, is_tcp_udp, frag, error_type));
		}
	}

	NXGE_DEBUG_MSG((nxgep, RX2_CTL,
		"==> nxge_receive_packet: *mp 0x%016llx", *mp));

	*multi_p = (multi == RCR_MULTI_MASK);
	NXGE_DEBUG_MSG((nxgep, RX_CTL, "<== nxge_receive_packet: "
		"multi %d nmp 0x%016llx *mp 0x%016llx *mp_cont 0x%016llx",
		*multi_p, nmp, *mp, *mp_cont));
}

/*ARGSUSED*/
static nxge_status_t
nxge_rx_err_evnts(p_nxge_t nxgep, uint_t index, p_nxge_ldv_t ldvp,
						rx_dma_ctl_stat_t cs)
{
	p_nxge_rx_ring_stats_t	rdc_stats;
	npi_handle_t		handle;
	npi_status_t		rs;
	boolean_t		rxchan_fatal = B_FALSE;
	boolean_t		rxport_fatal = B_FALSE;
	uint8_t			channel;
	uint8_t			portn;
	nxge_status_t		status = NXGE_OK;
	uint32_t		error_disp_cnt = NXGE_ERROR_SHOW_MAX;
	NXGE_DEBUG_MSG((nxgep, INT_CTL, "==> nxge_rx_err_evnts"));

	handle = NXGE_DEV_NPI_HANDLE(nxgep);
	channel = ldvp->channel;
	portn = nxgep->mac.portnum;
	rdc_stats = &nxgep->statsp->rdc_stats[ldvp->vdma_index];

	if (cs.bits.hdw.rbr_tmout) {
		rdc_stats->rx_rbr_tmout++;
		NXGE_FM_REPORT_ERROR(nxgep, portn, channel,
					NXGE_FM_EREPORT_RDMC_RBR_TMOUT);
		rxchan_fatal = B_TRUE;
		NXGE_ERROR_MSG((nxgep, NXGE_ERR_CTL,
			"==> nxge_rx_err_evnts: rx_rbr_timeout"));
	}
	if (cs.bits.hdw.rsp_cnt_err) {
		rdc_stats->rsp_cnt_err++;
		NXGE_FM_REPORT_ERROR(nxgep, portn, channel,
					NXGE_FM_EREPORT_RDMC_RSP_CNT_ERR);
		rxchan_fatal = B_TRUE;
		NXGE_ERROR_MSG((nxgep, NXGE_ERR_CTL,
			"==> nxge_rx_err_evnts(channel %d): "
			"rsp_cnt_err", channel));
	}
	if (cs.bits.hdw.byte_en_bus) {
		rdc_stats->byte_en_bus++;
		NXGE_FM_REPORT_ERROR(nxgep, portn, channel,
					NXGE_FM_EREPORT_RDMC_BYTE_EN_BUS);
		NXGE_ERROR_MSG((nxgep, NXGE_ERR_CTL,
			"==> nxge_rx_err_evnts(channel %d): "
			"fatal error: byte_en_bus", channel));
		rxchan_fatal = B_TRUE;
	}
	if (cs.bits.hdw.rsp_dat_err) {
		rdc_stats->rsp_dat_err++;
		NXGE_FM_REPORT_ERROR(nxgep, portn, channel,
					NXGE_FM_EREPORT_RDMC_RSP_DAT_ERR);
		rxchan_fatal = B_TRUE;
		NXGE_ERROR_MSG((nxgep, NXGE_ERR_CTL,
			"==> nxge_rx_err_evnts(channel %d): "
			"fatal error: rsp_dat_err", channel));
	}
	if (cs.bits.hdw.rcr_ack_err) {
		rdc_stats->rcr_ack_err++;
		NXGE_FM_REPORT_ERROR(nxgep, portn, channel,
					NXGE_FM_EREPORT_RDMC_RCR_ACK_ERR);
		rxchan_fatal = B_TRUE;
		NXGE_ERROR_MSG((nxgep, NXGE_ERR_CTL,
			"==> nxge_rx_err_evnts(channel %d): "
			"fatal error: rcr_ack_err", channel));
	}
	if (cs.bits.hdw.dc_fifo_err) {
		rdc_stats->dc_fifo_err++;
		NXGE_FM_REPORT_ERROR(nxgep, portn, channel,
					NXGE_FM_EREPORT_RDMC_DC_FIFO_ERR);
		/* This is not a fatal error! */
		NXGE_ERROR_MSG((nxgep, NXGE_ERR_CTL,
			"==> nxge_rx_err_evnts(channel %d): "
			"dc_fifo_err", channel));
		rxport_fatal = B_TRUE;
	}
	if ((cs.bits.hdw.rcr_sha_par) || (cs.bits.hdw.rbr_pre_par)) {
		if ((rs = npi_rxdma_ring_perr_stat_get(handle,
				&rdc_stats->errlog.pre_par,
				&rdc_stats->errlog.sha_par))
				!= NPI_SUCCESS) {
			NXGE_ERROR_MSG((nxgep, NXGE_ERR_CTL,
				"==> nxge_rx_err_evnts(channel %d): "
				"rcr_sha_par: get perr", channel));
			return (NXGE_ERROR | rs);
		}
		if (cs.bits.hdw.rcr_sha_par) {
			rdc_stats->rcr_sha_par++;
			NXGE_FM_REPORT_ERROR(nxgep, portn, channel,
					NXGE_FM_EREPORT_RDMC_RCR_SHA_PAR);
			rxchan_fatal = B_TRUE;
			NXGE_ERROR_MSG((nxgep, NXGE_ERR_CTL,
				"==> nxge_rx_err_evnts(channel %d): "
				"fatal error: rcr_sha_par", channel));
		}
		if (cs.bits.hdw.rbr_pre_par) {
			rdc_stats->rbr_pre_par++;
			NXGE_FM_REPORT_ERROR(nxgep, portn, channel,
					NXGE_FM_EREPORT_RDMC_RBR_PRE_PAR);
			rxchan_fatal = B_TRUE;
			NXGE_ERROR_MSG((nxgep, NXGE_ERR_CTL,
				"==> nxge_rx_err_evnts(channel %d): "
				"fatal error: rbr_pre_par", channel));
		}
	}
	if (cs.bits.hdw.port_drop_pkt) {
		rdc_stats->port_drop_pkt++;
		if (rdc_stats->port_drop_pkt < error_disp_cnt)
		NXGE_ERROR_MSG((nxgep, NXGE_ERR_CTL,
			"==> nxge_rx_err_evnts (channel %d): "
			"port_drop_pkt", channel));
	}
	if (cs.bits.hdw.wred_drop) {
		rdc_stats->wred_drop++;
		NXGE_DEBUG_MSG((nxgep, NXGE_ERR_CTL,
			"==> nxge_rx_err_evnts(channel %d): "
		"wred_drop", channel));
	}
	if (cs.bits.hdw.rbr_pre_empty) {
		rdc_stats->rbr_pre_empty++;
		if (rdc_stats->rbr_pre_empty < error_disp_cnt)
		NXGE_ERROR_MSG((nxgep, NXGE_ERR_CTL,
			"==> nxge_rx_err_evnts(channel %d): "
			"rbr_pre_empty", channel));
	}
	if (cs.bits.hdw.rcr_shadow_full) {
		rdc_stats->rcr_shadow_full++;
		NXGE_ERROR_MSG((nxgep, NXGE_ERR_CTL,
			"==> nxge_rx_err_evnts(channel %d): "
			"rcr_shadow_full", channel));
	}
	if (cs.bits.hdw.config_err) {
		rdc_stats->config_err++;
		NXGE_FM_REPORT_ERROR(nxgep, portn, channel,
					NXGE_FM_EREPORT_RDMC_CONFIG_ERR);
		rxchan_fatal = B_TRUE;
		NXGE_ERROR_MSG((nxgep, NXGE_ERR_CTL,
			"==> nxge_rx_err_evnts(channel %d): "
			"config error", channel));
	}
	if (cs.bits.hdw.rcrincon) {
		rdc_stats->rcrincon++;
		NXGE_FM_REPORT_ERROR(nxgep, portn, channel,
					NXGE_FM_EREPORT_RDMC_RCRINCON);
		rxchan_fatal = B_TRUE;
		NXGE_ERROR_MSG((nxgep, NXGE_ERR_CTL,
			"==> nxge_rx_err_evnts(channel %d): "
			"fatal error: rcrincon error", channel));
	}
	if (cs.bits.hdw.rcrfull) {
		rdc_stats->rcrfull++;
		NXGE_FM_REPORT_ERROR(nxgep, portn, channel,
					NXGE_FM_EREPORT_RDMC_RCRFULL);
		rxchan_fatal = B_TRUE;
		if (rdc_stats->rcrfull < error_disp_cnt)
		NXGE_ERROR_MSG((nxgep, NXGE_ERR_CTL,
			"==> nxge_rx_err_evnts(channel %d): "
			"fatal error: rcrfull error", channel));
	}
	if (cs.bits.hdw.rbr_empty) {
		rdc_stats->rbr_empty++;
		if (rdc_stats->rbr_empty < error_disp_cnt)
		NXGE_ERROR_MSG((nxgep, NXGE_ERR_CTL,
			"==> nxge_rx_err_evnts(channel %d): "
			"rbr empty error", channel));
	}
	if (cs.bits.hdw.rbrfull) {
		rdc_stats->rbrfull++;
		NXGE_FM_REPORT_ERROR(nxgep, portn, channel,
					NXGE_FM_EREPORT_RDMC_RBRFULL);
		rxchan_fatal = B_TRUE;
		NXGE_ERROR_MSG((nxgep, NXGE_ERR_CTL,
			"==> nxge_rx_err_evnts(channel %d): "
			"fatal error: rbr_full error", channel));
	}
	if (cs.bits.hdw.rbrlogpage) {
		rdc_stats->rbrlogpage++;
		NXGE_FM_REPORT_ERROR(nxgep, portn, channel,
					NXGE_FM_EREPORT_RDMC_RBRLOGPAGE);
		rxchan_fatal = B_TRUE;
		NXGE_ERROR_MSG((nxgep, NXGE_ERR_CTL,
			"==> nxge_rx_err_evnts(channel %d): "
			"fatal error: rbr logical page error", channel));
	}
	if (cs.bits.hdw.cfiglogpage) {
		rdc_stats->cfiglogpage++;
		NXGE_FM_REPORT_ERROR(nxgep, portn, channel,
					NXGE_FM_EREPORT_RDMC_CFIGLOGPAGE);
		rxchan_fatal = B_TRUE;
		NXGE_ERROR_MSG((nxgep, NXGE_ERR_CTL,
			"==> nxge_rx_err_evnts(channel %d): "
			"fatal error: cfig logical page error", channel));
	}

	if (rxport_fatal)  {
		NXGE_ERROR_MSG((nxgep, NXGE_ERR_CTL,
				" nxge_rx_err_evnts: "
				" fatal error on Port #%d\n",
				portn));
		status = nxge_ipp_fatal_err_recover(nxgep);
		if (status == NXGE_OK) {
			FM_SERVICE_RESTORED(nxgep);
		}
	}

	if (rxchan_fatal) {
		NXGE_ERROR_MSG((nxgep, NXGE_ERR_CTL,
				" nxge_rx_err_evnts: "
				" fatal error on Channel #%d\n",
				channel));
		status = nxge_rxdma_fatal_err_recover(nxgep, channel);
		if (status == NXGE_OK) {
			FM_SERVICE_RESTORED(nxgep);
		}
	}

	NXGE_DEBUG_MSG((nxgep, RX2_CTL, "<== nxge_rx_err_evnts"));

	return (status);
}

static nxge_status_t
nxge_map_rxdma(p_nxge_t nxgep)
{
	int			i, ndmas;
	uint16_t		channel;
	p_rx_rbr_rings_t 	rx_rbr_rings;
	p_rx_rbr_ring_t		*rbr_rings;
	p_rx_rcr_rings_t 	rx_rcr_rings;
	p_rx_rcr_ring_t		*rcr_rings;
	p_rx_mbox_areas_t 	rx_mbox_areas_p;
	p_rx_mbox_t		*rx_mbox_p;
	p_nxge_dma_pool_t	dma_buf_poolp;
	p_nxge_dma_pool_t	dma_cntl_poolp;
	p_nxge_dma_common_t	*dma_buf_p;
	p_nxge_dma_common_t	*dma_cntl_p;
	uint32_t		*num_chunks;
	nxge_status_t		status = NXGE_OK;
#if	defined(sun4v) && defined(NIU_LP_WORKAROUND)
	p_nxge_dma_common_t	t_dma_buf_p;
	p_nxge_dma_common_t	t_dma_cntl_p;
#endif

	NXGE_DEBUG_MSG((nxgep, MEM2_CTL, "==> nxge_map_rxdma"));

	dma_buf_poolp = nxgep->rx_buf_pool_p;
	dma_cntl_poolp = nxgep->rx_cntl_pool_p;

	if (!dma_buf_poolp->buf_allocated || !dma_cntl_poolp->buf_allocated) {
		NXGE_ERROR_MSG((nxgep, NXGE_ERR_CTL,
			"<== nxge_map_rxdma: buf not allocated"));
		return (NXGE_ERROR);
	}

	ndmas = dma_buf_poolp->ndmas;
	if (!ndmas) {
		NXGE_DEBUG_MSG((nxgep, RX_CTL,
			"<== nxge_map_rxdma: no dma allocated"));
		return (NXGE_ERROR);
	}

	num_chunks = dma_buf_poolp->num_chunks;
	dma_buf_p = dma_buf_poolp->dma_buf_pool_p;
	dma_cntl_p = dma_cntl_poolp->dma_buf_pool_p;

	rx_rbr_rings = (p_rx_rbr_rings_t)
		KMEM_ZALLOC(sizeof (rx_rbr_rings_t), KM_SLEEP);
	rbr_rings = (p_rx_rbr_ring_t *)
		KMEM_ZALLOC(sizeof (p_rx_rbr_ring_t) * ndmas, KM_SLEEP);
	rx_rcr_rings = (p_rx_rcr_rings_t)
		KMEM_ZALLOC(sizeof (rx_rcr_rings_t), KM_SLEEP);
	rcr_rings = (p_rx_rcr_ring_t *)
		KMEM_ZALLOC(sizeof (p_rx_rcr_ring_t) * ndmas, KM_SLEEP);
	rx_mbox_areas_p = (p_rx_mbox_areas_t)
		KMEM_ZALLOC(sizeof (rx_mbox_areas_t), KM_SLEEP);
	rx_mbox_p = (p_rx_mbox_t *)
		KMEM_ZALLOC(sizeof (p_rx_mbox_t) * ndmas, KM_SLEEP);

	/*
	 * Timeout should be set based on the system clock divider.
	 * The following timeout value of 1 assumes that the
	 * granularity (1000) is 3 microseconds running at 300MHz.
	 */

	nxgep->intr_threshold = RXDMA_RCR_PTHRES_DEFAULT;
	nxgep->intr_timeout = RXDMA_RCR_TO_DEFAULT;

	/*
	 * Map descriptors from the buffer polls for each dam channel.
	 */
	for (i = 0; i < ndmas; i++) {
		/*
		 * Set up and prepare buffer blocks, descriptors
		 * and mailbox.
		 */
		channel = ((p_nxge_dma_common_t)dma_buf_p[i])->dma_channel;
		status = nxge_map_rxdma_channel(nxgep, channel,
				(p_nxge_dma_common_t *)&dma_buf_p[i],
				(p_rx_rbr_ring_t *)&rbr_rings[i],
				num_chunks[i],
				(p_nxge_dma_common_t *)&dma_cntl_p[i],
				(p_rx_rcr_ring_t *)&rcr_rings[i],
				(p_rx_mbox_t *)&rx_mbox_p[i]);
		if (status != NXGE_OK) {
			goto nxge_map_rxdma_fail1;
		}
		rbr_rings[i]->index = (uint16_t)i;
		rcr_rings[i]->index = (uint16_t)i;
		rcr_rings[i]->rdc_stats = &nxgep->statsp->rdc_stats[i];

#if	defined(sun4v) && defined(NIU_LP_WORKAROUND)
		if (nxgep->niu_type == N2_NIU && NXGE_DMA_BLOCK == 1) {
			rbr_rings[i]->hv_set = B_FALSE;
			t_dma_buf_p = (p_nxge_dma_common_t)dma_buf_p[i];
			t_dma_cntl_p =
				(p_nxge_dma_common_t)dma_cntl_p[i];

			rbr_rings[i]->hv_rx_buf_base_ioaddr_pp =
				(uint64_t)t_dma_buf_p->orig_ioaddr_pp;
			rbr_rings[i]->hv_rx_buf_ioaddr_size =
				(uint64_t)t_dma_buf_p->orig_alength;
			NXGE_DEBUG_MSG((nxgep, MEM2_CTL,
				"==> nxge_map_rxdma_channel: "
				"channel %d "
				"data buf base io $%p ($%p) "
				"size 0x%llx (%d 0x%x)",
				channel,
				rbr_rings[i]->hv_rx_buf_base_ioaddr_pp,
				t_dma_cntl_p->ioaddr_pp,
				rbr_rings[i]->hv_rx_buf_ioaddr_size,
				t_dma_buf_p->orig_alength,
				t_dma_buf_p->orig_alength));

			rbr_rings[i]->hv_rx_cntl_base_ioaddr_pp =
				(uint64_t)t_dma_cntl_p->orig_ioaddr_pp;
			rbr_rings[i]->hv_rx_cntl_ioaddr_size =
				(uint64_t)t_dma_cntl_p->orig_alength;
			NXGE_DEBUG_MSG((nxgep, MEM2_CTL,
				"==> nxge_map_rxdma_channel: "
				"channel %d "
				"cntl base io $%p ($%p) "
				"size 0x%llx (%d 0x%x)",
				channel,
				rbr_rings[i]->hv_rx_cntl_base_ioaddr_pp,
				t_dma_cntl_p->ioaddr_pp,
				rbr_rings[i]->hv_rx_cntl_ioaddr_size,
				t_dma_cntl_p->orig_alength,
				t_dma_cntl_p->orig_alength));
		}

#endif	/* sun4v and NIU_LP_WORKAROUND */
	}

	rx_rbr_rings->ndmas = rx_rcr_rings->ndmas = ndmas;
	rx_rbr_rings->rbr_rings = rbr_rings;
	nxgep->rx_rbr_rings = rx_rbr_rings;
	rx_rcr_rings->rcr_rings = rcr_rings;
	nxgep->rx_rcr_rings = rx_rcr_rings;

	rx_mbox_areas_p->rxmbox_areas = rx_mbox_p;
	nxgep->rx_mbox_areas_p = rx_mbox_areas_p;

	goto nxge_map_rxdma_exit;

nxge_map_rxdma_fail1:
	NXGE_ERROR_MSG((nxgep, NXGE_ERR_CTL,
		"==> nxge_map_rxdma: unmap rbr,rcr "
		"(status 0x%x channel %d i %d)",
		status, channel, i));
	i--;
	for (; i >= 0; i--) {
		channel = ((p_nxge_dma_common_t)dma_buf_p[i])->dma_channel;
		nxge_unmap_rxdma_channel(nxgep, channel,
			rbr_rings[i],
			rcr_rings[i],
			rx_mbox_p[i]);
	}

	KMEM_FREE(rbr_rings, sizeof (p_rx_rbr_ring_t) * ndmas);
	KMEM_FREE(rx_rbr_rings, sizeof (rx_rbr_rings_t));
	KMEM_FREE(rcr_rings, sizeof (p_rx_rcr_ring_t) * ndmas);
	KMEM_FREE(rx_rcr_rings, sizeof (rx_rcr_rings_t));
	KMEM_FREE(rx_mbox_p, sizeof (p_rx_mbox_t) * ndmas);
	KMEM_FREE(rx_mbox_areas_p, sizeof (rx_mbox_areas_t));

nxge_map_rxdma_exit:
	NXGE_DEBUG_MSG((nxgep, MEM2_CTL,
		"<== nxge_map_rxdma: "
		"(status 0x%x channel %d)",
		status, channel));

	return (status);
}

static void
nxge_unmap_rxdma(p_nxge_t nxgep)
{
	int			i, ndmas;
	uint16_t		channel;
	p_rx_rbr_rings_t 	rx_rbr_rings;
	p_rx_rbr_ring_t		*rbr_rings;
	p_rx_rcr_rings_t 	rx_rcr_rings;
	p_rx_rcr_ring_t		*rcr_rings;
	p_rx_mbox_areas_t 	rx_mbox_areas_p;
	p_rx_mbox_t		*rx_mbox_p;
	p_nxge_dma_pool_t	dma_buf_poolp;
	p_nxge_dma_pool_t	dma_cntl_poolp;
	p_nxge_dma_common_t	*dma_buf_p;

	NXGE_DEBUG_MSG((nxgep, MEM2_CTL, "==> nxge_unmap_rxdma"));

	dma_buf_poolp = nxgep->rx_buf_pool_p;
	dma_cntl_poolp = nxgep->rx_cntl_pool_p;

	if (!dma_buf_poolp->buf_allocated || !dma_cntl_poolp->buf_allocated) {
		NXGE_ERROR_MSG((nxgep, NXGE_ERR_CTL,
			"<== nxge_unmap_rxdma: NULL buf pointers"));
		return;
	}

	rx_rbr_rings = nxgep->rx_rbr_rings;
	rx_rcr_rings = nxgep->rx_rcr_rings;
	if (rx_rbr_rings == NULL || rx_rcr_rings == NULL) {
		NXGE_ERROR_MSG((nxgep, NXGE_ERR_CTL,
			"<== nxge_unmap_rxdma: NULL ring pointers"));
		return;
	}
	ndmas = rx_rbr_rings->ndmas;
	if (!ndmas) {
		NXGE_ERROR_MSG((nxgep, NXGE_ERR_CTL,
			"<== nxge_unmap_rxdma: no channel"));
		return;
	}

	NXGE_DEBUG_MSG((nxgep, MEM2_CTL,
		"==> nxge_unmap_rxdma (ndmas %d)", ndmas));
	rbr_rings = rx_rbr_rings->rbr_rings;
	rcr_rings = rx_rcr_rings->rcr_rings;
	rx_mbox_areas_p = nxgep->rx_mbox_areas_p;
	rx_mbox_p = rx_mbox_areas_p->rxmbox_areas;
	dma_buf_p = dma_buf_poolp->dma_buf_pool_p;

	for (i = 0; i < ndmas; i++) {
		channel = ((p_nxge_dma_common_t)dma_buf_p[i])->dma_channel;
		NXGE_DEBUG_MSG((nxgep, MEM2_CTL,
			"==> nxge_unmap_rxdma (ndmas %d) channel %d",
				ndmas, channel));
		(void) nxge_unmap_rxdma_channel(nxgep, channel,
				(p_rx_rbr_ring_t)rbr_rings[i],
				(p_rx_rcr_ring_t)rcr_rings[i],
				(p_rx_mbox_t)rx_mbox_p[i]);
	}

	KMEM_FREE(rx_rbr_rings, sizeof (rx_rbr_rings_t));
	KMEM_FREE(rbr_rings, sizeof (p_rx_rbr_ring_t) * ndmas);
	KMEM_FREE(rx_rcr_rings, sizeof (rx_rcr_rings_t));
	KMEM_FREE(rcr_rings, sizeof (p_rx_rcr_ring_t) * ndmas);
	KMEM_FREE(rx_mbox_areas_p, sizeof (rx_mbox_areas_t));
	KMEM_FREE(rx_mbox_p, sizeof (p_rx_mbox_t) * ndmas);

	NXGE_DEBUG_MSG((nxgep, MEM2_CTL,
		"<== nxge_unmap_rxdma"));
}

nxge_status_t
nxge_map_rxdma_channel(p_nxge_t nxgep, uint16_t channel,
    p_nxge_dma_common_t *dma_buf_p,  p_rx_rbr_ring_t *rbr_p,
    uint32_t num_chunks,
    p_nxge_dma_common_t *dma_cntl_p, p_rx_rcr_ring_t *rcr_p,
    p_rx_mbox_t *rx_mbox_p)
{
	int	status = NXGE_OK;

	/*
	 * Set up and prepare buffer blocks, descriptors
	 * and mailbox.
	 */
	NXGE_DEBUG_MSG((nxgep, MEM2_CTL,
		"==> nxge_map_rxdma_channel (channel %d)", channel));
	/*
	 * Receive buffer blocks
	 */
	status = nxge_map_rxdma_channel_buf_ring(nxgep, channel,
			dma_buf_p, rbr_p, num_chunks);
	if (status != NXGE_OK) {
		NXGE_ERROR_MSG((nxgep, NXGE_ERR_CTL,
			"==> nxge_map_rxdma_channel (channel %d): "
			"map buffer failed 0x%x", channel, status));
		goto nxge_map_rxdma_channel_exit;
	}

	/*
	 * Receive block ring, completion ring and mailbox.
	 */
	status = nxge_map_rxdma_channel_cfg_ring(nxgep, channel,
			dma_cntl_p, rbr_p, rcr_p, rx_mbox_p);
	if (status != NXGE_OK) {
		NXGE_ERROR_MSG((nxgep, NXGE_ERR_CTL,
			"==> nxge_map_rxdma_channel (channel %d): "
			"map config failed 0x%x", channel, status));
		goto nxge_map_rxdma_channel_fail2;
	}

	goto nxge_map_rxdma_channel_exit;

nxge_map_rxdma_channel_fail3:
	/* Free rbr, rcr */
	NXGE_ERROR_MSG((nxgep, NXGE_ERR_CTL,
		"==> nxge_map_rxdma_channel: free rbr/rcr "
		"(status 0x%x channel %d)",
		status, channel));
	nxge_unmap_rxdma_channel_cfg_ring(nxgep,
		*rcr_p, *rx_mbox_p);

nxge_map_rxdma_channel_fail2:
	/* Free buffer blocks */
	NXGE_ERROR_MSG((nxgep, NXGE_ERR_CTL,
		"==> nxge_map_rxdma_channel: free rx buffers"
		"(nxgep 0x%x status 0x%x channel %d)",
		nxgep, status, channel));
	nxge_unmap_rxdma_channel_buf_ring(nxgep, *rbr_p);

	status = NXGE_ERROR;

nxge_map_rxdma_channel_exit:
	NXGE_DEBUG_MSG((nxgep, MEM2_CTL,
		"<== nxge_map_rxdma_channel: "
		"(nxgep 0x%x status 0x%x channel %d)",
		nxgep, status, channel));

	return (status);
}

/*ARGSUSED*/
static void
nxge_unmap_rxdma_channel(p_nxge_t nxgep, uint16_t channel,
    p_rx_rbr_ring_t rbr_p, p_rx_rcr_ring_t rcr_p, p_rx_mbox_t rx_mbox_p)
{
	NXGE_DEBUG_MSG((nxgep, MEM2_CTL,
		"==> nxge_unmap_rxdma_channel (channel %d)", channel));

	/*
	 * unmap receive block ring, completion ring and mailbox.
	 */
	(void) nxge_unmap_rxdma_channel_cfg_ring(nxgep,
			rcr_p, rx_mbox_p);

	/* unmap buffer blocks */
	(void) nxge_unmap_rxdma_channel_buf_ring(nxgep, rbr_p);

	NXGE_DEBUG_MSG((nxgep, MEM2_CTL, "<== nxge_unmap_rxdma_channel"));
}

/*ARGSUSED*/
static nxge_status_t
nxge_map_rxdma_channel_cfg_ring(p_nxge_t nxgep, uint16_t dma_channel,
    p_nxge_dma_common_t *dma_cntl_p, p_rx_rbr_ring_t *rbr_p,
    p_rx_rcr_ring_t *rcr_p, p_rx_mbox_t *rx_mbox_p)
{
	p_rx_rbr_ring_t 	rbrp;
	p_rx_rcr_ring_t 	rcrp;
	p_rx_mbox_t 		mboxp;
	p_nxge_dma_common_t 	cntl_dmap;
	p_nxge_dma_common_t 	dmap;
	p_rx_msg_t 		*rx_msg_ring;
	p_rx_msg_t 		rx_msg_p;
	p_rbr_cfig_a_t		rcfga_p;
	p_rbr_cfig_b_t		rcfgb_p;
	p_rcrcfig_a_t		cfga_p;
	p_rcrcfig_b_t		cfgb_p;
	p_rxdma_cfig1_t		cfig1_p;
	p_rxdma_cfig2_t		cfig2_p;
	p_rbr_kick_t		kick_p;
	uint32_t		dmaaddrp;
	uint32_t		*rbr_vaddrp;
	uint32_t		bkaddr;
	nxge_status_t		status = NXGE_OK;
	int			i;
	uint32_t 		nxge_port_rcr_size;

	NXGE_DEBUG_MSG((nxgep, MEM2_CTL,
		"==> nxge_map_rxdma_channel_cfg_ring"));

	cntl_dmap = *dma_cntl_p;

	/* Map in the receive block ring */
	rbrp = *rbr_p;
	dmap = (p_nxge_dma_common_t)&rbrp->rbr_desc;
	nxge_setup_dma_common(dmap, cntl_dmap, rbrp->rbb_max, 4);
	/*
	 * Zero out buffer block ring descriptors.
	 */
	bzero((caddr_t)dmap->kaddrp, dmap->alength);

	rcfga_p = &(rbrp->rbr_cfga);
	rcfgb_p = &(rbrp->rbr_cfgb);
	kick_p = &(rbrp->rbr_kick);
	rcfga_p->value = 0;
	rcfgb_p->value = 0;
	kick_p->value = 0;
	rbrp->rbr_addr = dmap->dma_cookie.dmac_laddress;
	rcfga_p->value = (rbrp->rbr_addr &
				(RBR_CFIG_A_STDADDR_MASK |
				RBR_CFIG_A_STDADDR_BASE_MASK));
	rcfga_p->value |= ((uint64_t)rbrp->rbb_max << RBR_CFIG_A_LEN_SHIFT);

	rcfgb_p->bits.ldw.bufsz0 = rbrp->pkt_buf_size0;
	rcfgb_p->bits.ldw.vld0 = 1;
	rcfgb_p->bits.ldw.bufsz1 = rbrp->pkt_buf_size1;
	rcfgb_p->bits.ldw.vld1 = 1;
	rcfgb_p->bits.ldw.bufsz2 = rbrp->pkt_buf_size2;
	rcfgb_p->bits.ldw.vld2 = 1;
	rcfgb_p->bits.ldw.bksize = nxgep->rx_bksize_code;

	/*
	 * For each buffer block, enter receive block address to the ring.
	 */
	rbr_vaddrp = (uint32_t *)dmap->kaddrp;
	rbrp->rbr_desc_vp = (uint32_t *)dmap->kaddrp;
	NXGE_DEBUG_MSG((nxgep, MEM2_CTL,
		"==> nxge_map_rxdma_channel_cfg_ring: channel %d "
		"rbr_vaddrp $%p", dma_channel, rbr_vaddrp));

	rx_msg_ring = rbrp->rx_msg_ring;
	for (i = 0; i < rbrp->tnblocks; i++) {
		rx_msg_p = rx_msg_ring[i];
		rx_msg_p->nxgep = nxgep;
		rx_msg_p->rx_rbr_p = rbrp;
		bkaddr = (uint32_t)
			((rx_msg_p->buf_dma.dma_cookie.dmac_laddress
				>> RBR_BKADDR_SHIFT));
		rx_msg_p->free = B_FALSE;
		rx_msg_p->max_usage_cnt = 0xbaddcafe;

		*rbr_vaddrp++ = bkaddr;
	}

	kick_p->bits.ldw.bkadd = rbrp->rbb_max;
	rbrp->rbr_wr_index = (rbrp->rbb_max - 1);

	rbrp->rbr_rd_index = 0;

	rbrp->rbr_consumed = 0;
	rbrp->rbr_use_bcopy = B_TRUE;
	rbrp->rbr_bufsize_type = RCR_PKTBUFSZ_0;
	/*
	 * Do bcopy on packets greater than bcopy size once
	 * the lo threshold is reached.
	 * This lo threshold should be less than the hi threshold.
	 *
	 * Do bcopy on every packet once the hi threshold is reached.
	 */
	if (nxge_rx_threshold_lo >= nxge_rx_threshold_hi) {
		/* default it to use hi */
		nxge_rx_threshold_lo = nxge_rx_threshold_hi;
	}

	if (nxge_rx_buf_size_type > NXGE_RBR_TYPE2) {
		nxge_rx_buf_size_type = NXGE_RBR_TYPE2;
	}
	rbrp->rbr_bufsize_type = nxge_rx_buf_size_type;

	switch (nxge_rx_threshold_hi) {
	default:
	case	NXGE_RX_COPY_NONE:
		/* Do not do bcopy at all */
		rbrp->rbr_use_bcopy = B_FALSE;
		rbrp->rbr_threshold_hi = rbrp->rbb_max;
		break;

	case NXGE_RX_COPY_1:
	case NXGE_RX_COPY_2:
	case NXGE_RX_COPY_3:
	case NXGE_RX_COPY_4:
	case NXGE_RX_COPY_5:
	case NXGE_RX_COPY_6:
	case NXGE_RX_COPY_7:
		rbrp->rbr_threshold_hi =
			rbrp->rbb_max *
			(nxge_rx_threshold_hi)/NXGE_RX_BCOPY_SCALE;
		break;

	case NXGE_RX_COPY_ALL:
		rbrp->rbr_threshold_hi = 0;
		break;
	}

	switch (nxge_rx_threshold_lo) {
	default:
	case	NXGE_RX_COPY_NONE:
		/* Do not do bcopy at all */
		if (rbrp->rbr_use_bcopy) {
			rbrp->rbr_use_bcopy = B_FALSE;
		}
		rbrp->rbr_threshold_lo = rbrp->rbb_max;
		break;

	case NXGE_RX_COPY_1:
	case NXGE_RX_COPY_2:
	case NXGE_RX_COPY_3:
	case NXGE_RX_COPY_4:
	case NXGE_RX_COPY_5:
	case NXGE_RX_COPY_6:
	case NXGE_RX_COPY_7:
		rbrp->rbr_threshold_lo =
			rbrp->rbb_max *
			(nxge_rx_threshold_lo)/NXGE_RX_BCOPY_SCALE;
		break;

	case NXGE_RX_COPY_ALL:
		rbrp->rbr_threshold_lo = 0;
		break;
	}

	NXGE_DEBUG_MSG((nxgep, RX_CTL,
		"nxge_map_rxdma_channel_cfg_ring: channel %d "
		"rbb_max %d "
		"rbrp->rbr_bufsize_type %d "
		"rbb_threshold_hi %d "
		"rbb_threshold_lo %d",
		dma_channel,
		rbrp->rbb_max,
		rbrp->rbr_bufsize_type,
		rbrp->rbr_threshold_hi,
		rbrp->rbr_threshold_lo));

	rbrp->page_valid.value = 0;
	rbrp->page_mask_1.value = rbrp->page_mask_2.value = 0;
	rbrp->page_value_1.value = rbrp->page_value_2.value = 0;
	rbrp->page_reloc_1.value = rbrp->page_reloc_2.value = 0;
	rbrp->page_hdl.value = 0;

	rbrp->page_valid.bits.ldw.page0 = 1;
	rbrp->page_valid.bits.ldw.page1 = 1;

	/* Map in the receive completion ring */
	rcrp = (p_rx_rcr_ring_t)
		KMEM_ZALLOC(sizeof (rx_rcr_ring_t), KM_SLEEP);
	rcrp->rdc = dma_channel;

	nxge_port_rcr_size = nxgep->nxge_port_rcr_size;
	rcrp->comp_size = nxge_port_rcr_size;
	rcrp->comp_wrap_mask = nxge_port_rcr_size - 1;

	rcrp->max_receive_pkts = nxge_max_rx_pkts;

	dmap = (p_nxge_dma_common_t)&rcrp->rcr_desc;
	nxge_setup_dma_common(dmap, cntl_dmap, rcrp->comp_size,
			sizeof (rcr_entry_t));
	rcrp->comp_rd_index = 0;
	rcrp->comp_wt_index = 0;
	rcrp->rcr_desc_rd_head_p = rcrp->rcr_desc_first_p =
		(p_rcr_entry_t)DMA_COMMON_VPTR(rcrp->rcr_desc);
	rcrp->rcr_desc_rd_head_pp = rcrp->rcr_desc_first_pp =
		(p_rcr_entry_t)DMA_COMMON_IOADDR(rcrp->rcr_desc);

	rcrp->rcr_desc_last_p = rcrp->rcr_desc_rd_head_p +
			(nxge_port_rcr_size - 1);
	rcrp->rcr_desc_last_pp = rcrp->rcr_desc_rd_head_pp +
			(nxge_port_rcr_size - 1);

	NXGE_DEBUG_MSG((nxgep, MEM2_CTL,
		"==> nxge_map_rxdma_channel_cfg_ring: "
		"channel %d "
		"rbr_vaddrp $%p "
		"rcr_desc_rd_head_p $%p "
		"rcr_desc_rd_head_pp $%p "
		"rcr_desc_rd_last_p $%p "
		"rcr_desc_rd_last_pp $%p ",
		dma_channel,
		rbr_vaddrp,
		rcrp->rcr_desc_rd_head_p,
		rcrp->rcr_desc_rd_head_pp,
		rcrp->rcr_desc_last_p,
		rcrp->rcr_desc_last_pp));

	/*
	 * Zero out buffer block ring descriptors.
	 */
	bzero((caddr_t)dmap->kaddrp, dmap->alength);
	rcrp->intr_timeout = nxgep->intr_timeout;
	rcrp->intr_threshold = nxgep->intr_threshold;
	rcrp->full_hdr_flag = B_FALSE;
	rcrp->sw_priv_hdr_len = 0;

	cfga_p = &(rcrp->rcr_cfga);
	cfgb_p = &(rcrp->rcr_cfgb);
	cfga_p->value = 0;
	cfgb_p->value = 0;
	rcrp->rcr_addr = dmap->dma_cookie.dmac_laddress;
	cfga_p->value = (rcrp->rcr_addr &
			    (RCRCFIG_A_STADDR_MASK |
			    RCRCFIG_A_STADDR_BASE_MASK));

	rcfga_p->value |= ((uint64_t)rcrp->comp_size <<
				RCRCFIG_A_LEN_SHIF);

	/*
	 * Timeout should be set based on the system clock divider.
	 * The following timeout value of 1 assumes that the
	 * granularity (1000) is 3 microseconds running at 300MHz.
	 */
	cfgb_p->bits.ldw.pthres = rcrp->intr_threshold;
	cfgb_p->bits.ldw.timeout = rcrp->intr_timeout;
	cfgb_p->bits.ldw.entout = 1;

	/* Map in the mailbox */
	mboxp = (p_rx_mbox_t)
			KMEM_ZALLOC(sizeof (rx_mbox_t), KM_SLEEP);
	dmap = (p_nxge_dma_common_t)&mboxp->rx_mbox;
	nxge_setup_dma_common(dmap, cntl_dmap, 1, sizeof (rxdma_mailbox_t));
	cfig1_p = (p_rxdma_cfig1_t)&mboxp->rx_cfg1;
	cfig2_p = (p_rxdma_cfig2_t)&mboxp->rx_cfg2;
	cfig1_p->value = cfig2_p->value = 0;

	mboxp->mbox_addr = dmap->dma_cookie.dmac_laddress;
	NXGE_DEBUG_MSG((nxgep, MEM2_CTL,
		"==> nxge_map_rxdma_channel_cfg_ring: "
		"channel %d cfg1 0x%016llx cfig2 0x%016llx cookie 0x%016llx",
		dma_channel, cfig1_p->value, cfig2_p->value,
		mboxp->mbox_addr));

	dmaaddrp = (uint32_t)(dmap->dma_cookie.dmac_laddress >> 32
			& 0xfff);
	cfig1_p->bits.ldw.mbaddr_h = dmaaddrp;


	dmaaddrp = (uint32_t)(dmap->dma_cookie.dmac_laddress & 0xffffffff);
	dmaaddrp = (uint32_t)(dmap->dma_cookie.dmac_laddress &
				RXDMA_CFIG2_MBADDR_L_MASK);

	cfig2_p->bits.ldw.mbaddr = (dmaaddrp >> RXDMA_CFIG2_MBADDR_L_SHIFT);

	NXGE_DEBUG_MSG((nxgep, MEM2_CTL,
		"==> nxge_map_rxdma_channel_cfg_ring: "
		"channel %d damaddrp $%p "
		"cfg1 0x%016llx cfig2 0x%016llx",
		dma_channel, dmaaddrp,
		cfig1_p->value, cfig2_p->value));

	cfig2_p->bits.ldw.full_hdr = rcrp->full_hdr_flag;
	cfig2_p->bits.ldw.offset = rcrp->sw_priv_hdr_len;

	rbrp->rx_rcr_p = rcrp;
	rcrp->rx_rbr_p = rbrp;
	*rcr_p = rcrp;
	*rx_mbox_p = mboxp;

	NXGE_DEBUG_MSG((nxgep, MEM2_CTL,
		"<== nxge_map_rxdma_channel_cfg_ring status 0x%08x", status));

	return (status);
}

/*ARGSUSED*/
static void
nxge_unmap_rxdma_channel_cfg_ring(p_nxge_t nxgep,
    p_rx_rcr_ring_t rcr_p, p_rx_mbox_t rx_mbox_p)
{
	NXGE_DEBUG_MSG((nxgep, MEM2_CTL,
		"==> nxge_unmap_rxdma_channel_cfg_ring: channel %d",
		rcr_p->rdc));

	KMEM_FREE(rcr_p, sizeof (rx_rcr_ring_t));
	KMEM_FREE(rx_mbox_p, sizeof (rx_mbox_t));

	NXGE_DEBUG_MSG((nxgep, MEM2_CTL,
		"<== nxge_unmap_rxdma_channel_cfg_ring"));
}

static nxge_status_t
nxge_map_rxdma_channel_buf_ring(p_nxge_t nxgep, uint16_t channel,
    p_nxge_dma_common_t *dma_buf_p,
    p_rx_rbr_ring_t *rbr_p, uint32_t num_chunks)
{
	p_rx_rbr_ring_t 	rbrp;
	p_nxge_dma_common_t 	dma_bufp, tmp_bufp;
	p_rx_msg_t 		*rx_msg_ring;
	p_rx_msg_t 		rx_msg_p;
	p_mblk_t 		mblk_p;

	rxring_info_t *ring_info;
	nxge_status_t		status = NXGE_OK;
	int			i, j, index;
	uint32_t		size, bsize, nblocks, nmsgs;

	NXGE_DEBUG_MSG((nxgep, MEM2_CTL,
		"==> nxge_map_rxdma_channel_buf_ring: channel %d",
		channel));

	dma_bufp = tmp_bufp = *dma_buf_p;
	NXGE_DEBUG_MSG((nxgep, MEM2_CTL,
		" nxge_map_rxdma_channel_buf_ring: channel %d to map %d "
		"chunks bufp 0x%016llx",
		channel, num_chunks, dma_bufp));

	nmsgs = 0;
	for (i = 0; i < num_chunks; i++, tmp_bufp++) {
		NXGE_DEBUG_MSG((nxgep, MEM2_CTL,
			"==> nxge_map_rxdma_channel_buf_ring: channel %d "
			"bufp 0x%016llx nblocks %d nmsgs %d",
			channel, tmp_bufp, tmp_bufp->nblocks, nmsgs));
		nmsgs += tmp_bufp->nblocks;
	}
	if (!nmsgs) {
		NXGE_ERROR_MSG((nxgep, NXGE_ERR_CTL,
			"<== nxge_map_rxdma_channel_buf_ring: channel %d "
			"no msg blocks",
			channel));
		status = NXGE_ERROR;
		goto nxge_map_rxdma_channel_buf_ring_exit;
	}

	rbrp = (p_rx_rbr_ring_t)
		KMEM_ZALLOC(sizeof (rx_rbr_ring_t), KM_SLEEP);

	size = nmsgs * sizeof (p_rx_msg_t);
	rx_msg_ring = KMEM_ZALLOC(size, KM_SLEEP);
	ring_info = (rxring_info_t *)KMEM_ZALLOC(sizeof (rxring_info_t),
		KM_SLEEP);

	MUTEX_INIT(&rbrp->lock, NULL, MUTEX_DRIVER,
				(void *)nxgep->interrupt_cookie);
	MUTEX_INIT(&rbrp->post_lock, NULL, MUTEX_DRIVER,
				(void *)nxgep->interrupt_cookie);
	rbrp->rdc = channel;
	rbrp->num_blocks = num_chunks;
	rbrp->tnblocks = nmsgs;
	rbrp->rbb_max = nmsgs;
	rbrp->rbr_max_size = nmsgs;
	rbrp->rbr_wrap_mask = (rbrp->rbb_max - 1);

	/*
	 * Buffer sizes suggested by NIU architect.
	 * 256, 512 and 2K.
	 */

	rbrp->pkt_buf_size0 = RBR_BUFSZ0_256B;
	rbrp->pkt_buf_size0_bytes = RBR_BUFSZ0_256_BYTES;
	rbrp->npi_pkt_buf_size0 = SIZE_256B;

	rbrp->pkt_buf_size1 = RBR_BUFSZ1_1K;
	rbrp->pkt_buf_size1_bytes = RBR_BUFSZ1_1K_BYTES;
	rbrp->npi_pkt_buf_size1 = SIZE_1KB;

	rbrp->block_size = nxgep->rx_default_block_size;

	if (!nxge_jumbo_enable && !nxgep->param_arr[param_accept_jumbo].value) {
		rbrp->pkt_buf_size2 = RBR_BUFSZ2_2K;
		rbrp->pkt_buf_size2_bytes = RBR_BUFSZ2_2K_BYTES;
		rbrp->npi_pkt_buf_size2 = SIZE_2KB;
	} else {
		if (rbrp->block_size >= 0x2000) {
			rbrp->pkt_buf_size2 = RBR_BUFSZ2_8K;
			rbrp->pkt_buf_size2_bytes = RBR_BUFSZ2_8K_BYTES;
			rbrp->npi_pkt_buf_size2 = SIZE_8KB;
		} else {
			rbrp->pkt_buf_size2 = RBR_BUFSZ2_4K;
			rbrp->pkt_buf_size2_bytes = RBR_BUFSZ2_4K_BYTES;
			rbrp->npi_pkt_buf_size2 = SIZE_4KB;
		}
	}

	NXGE_DEBUG_MSG((nxgep, MEM2_CTL,
		"==> nxge_map_rxdma_channel_buf_ring: channel %d "
		"actual rbr max %d rbb_max %d nmsgs %d "
		"rbrp->block_size %d default_block_size %d "
		"(config nxge_rbr_size %d nxge_rbr_spare_size %d)",
		channel, rbrp->rbr_max_size, rbrp->rbb_max, nmsgs,
		rbrp->block_size, nxgep->rx_default_block_size,
		nxge_rbr_size, nxge_rbr_spare_size));

	/* Map in buffers from the buffer pool.  */
	index = 0;
	for (i = 0; i < rbrp->num_blocks; i++, dma_bufp++) {
		bsize = dma_bufp->block_size;
		nblocks = dma_bufp->nblocks;
		ring_info->buffer[i].dvma_addr = (uint64_t)dma_bufp->ioaddr_pp;
		ring_info->buffer[i].buf_index = i;
		ring_info->buffer[i].buf_size = dma_bufp->alength;
		ring_info->buffer[i].start_index = index;
		ring_info->buffer[i].kaddr = (uint64_t)dma_bufp->kaddrp;

		NXGE_DEBUG_MSG((nxgep, MEM2_CTL,
			" nxge_map_rxdma_channel_buf_ring: map channel %d "
			"chunk %d"
			" nblocks %d chunk_size %x block_size 0x%x "
			"dma_bufp $%p", channel, i,
			dma_bufp->nblocks, ring_info->buffer[i].buf_size, bsize,
			dma_bufp));

		for (j = 0; j < nblocks; j++) {
			if ((rx_msg_p = nxge_allocb(bsize, BPRI_LO,
					dma_bufp)) == NULL) {
				NXGE_ERROR_MSG((nxgep, NXGE_ERR_CTL,
					"allocb failed (index %d i %d j %d)",
					index, i, j));
				goto nxge_map_rxdma_channel_buf_ring_fail1;
			}
			rx_msg_ring[index] = rx_msg_p;
			rx_msg_p->block_index = index;
			rx_msg_p->shifted_addr = (uint32_t)
				((rx_msg_p->buf_dma.dma_cookie.dmac_laddress >>
					    RBR_BKADDR_SHIFT));

			NXGE_DEBUG_MSG((nxgep, MEM2_CTL,
				"index %d j %d rx_msg_p $%p mblk %p",
				index, j, rx_msg_p, rx_msg_p->rx_mblk_p));

			mblk_p = rx_msg_p->rx_mblk_p;
			mblk_p->b_wptr = mblk_p->b_rptr + bsize;
			index++;
			rx_msg_p->buf_dma.dma_channel = channel;
		}
	}
	if (i < rbrp->num_blocks) {
		goto nxge_map_rxdma_channel_buf_ring_fail1;
	}

	NXGE_DEBUG_MSG((nxgep, MEM2_CTL,
		"nxge_map_rxdma_channel_buf_ring: done buf init "
			"channel %d msg block entries %d",
			channel, index));
	ring_info->block_size_mask = bsize - 1;
	rbrp->rx_msg_ring = rx_msg_ring;
	rbrp->dma_bufp = dma_buf_p;
	rbrp->ring_info = ring_info;

	status = nxge_rxbuf_index_info_init(nxgep, rbrp);
	NXGE_DEBUG_MSG((nxgep, MEM2_CTL,
		" nxge_map_rxdma_channel_buf_ring: "
		"channel %d done buf info init", channel));

	*rbr_p = rbrp;
	goto nxge_map_rxdma_channel_buf_ring_exit;

nxge_map_rxdma_channel_buf_ring_fail1:
	NXGE_DEBUG_MSG((nxgep, MEM2_CTL,
		" nxge_map_rxdma_channel_buf_ring: failed channel (0x%x)",
		channel, status));

	index--;
	for (; index >= 0; index--) {
		rx_msg_p = rx_msg_ring[index];
		if (rx_msg_p != NULL) {
			freeb(rx_msg_p->rx_mblk_p);
			rx_msg_ring[index] = NULL;
		}
	}
nxge_map_rxdma_channel_buf_ring_fail:
	MUTEX_DESTROY(&rbrp->post_lock);
	MUTEX_DESTROY(&rbrp->lock);
	KMEM_FREE(ring_info, sizeof (rxring_info_t));
	KMEM_FREE(rx_msg_ring, size);
	KMEM_FREE(rbrp, sizeof (rx_rbr_ring_t));

	status = NXGE_ERROR;

nxge_map_rxdma_channel_buf_ring_exit:
	NXGE_DEBUG_MSG((nxgep, MEM2_CTL,
		"<== nxge_map_rxdma_channel_buf_ring status 0x%08x", status));

	return (status);
}

/*ARGSUSED*/
static void
nxge_unmap_rxdma_channel_buf_ring(p_nxge_t nxgep,
    p_rx_rbr_ring_t rbr_p)
{
	p_rx_msg_t 		*rx_msg_ring;
	p_rx_msg_t 		rx_msg_p;
	rxring_info_t 		*ring_info;
	int			i;
	uint32_t		size;
#ifdef	NXGE_DEBUG
	int			num_chunks;
#endif

	NXGE_DEBUG_MSG((nxgep, MEM2_CTL,
		"==> nxge_unmap_rxdma_channel_buf_ring"));
	if (rbr_p == NULL) {
		NXGE_DEBUG_MSG((nxgep, RX_CTL,
			"<== nxge_unmap_rxdma_channel_buf_ring: NULL rbrp"));
		return;
	}
	NXGE_DEBUG_MSG((nxgep, MEM2_CTL,
		"==> nxge_unmap_rxdma_channel_buf_ring: channel %d",
		rbr_p->rdc));

	rx_msg_ring = rbr_p->rx_msg_ring;
	ring_info = rbr_p->ring_info;

	if (rx_msg_ring == NULL || ring_info == NULL) {
			NXGE_DEBUG_MSG((nxgep, MEM2_CTL,
		"<== nxge_unmap_rxdma_channel_buf_ring: "
		"rx_msg_ring $%p ring_info $%p",
		rx_msg_p, ring_info));
		return;
	}

#ifdef	NXGE_DEBUG
	num_chunks = rbr_p->num_blocks;
#endif
	size = rbr_p->tnblocks * sizeof (p_rx_msg_t);
	NXGE_DEBUG_MSG((nxgep, MEM2_CTL,
		" nxge_unmap_rxdma_channel_buf_ring: channel %d chunks %d "
		"tnblocks %d (max %d) size ptrs %d ",
		rbr_p->rdc, num_chunks,
		rbr_p->tnblocks, rbr_p->rbr_max_size, size));

	for (i = 0; i < rbr_p->tnblocks; i++) {
		rx_msg_p = rx_msg_ring[i];
		NXGE_DEBUG_MSG((nxgep, MEM2_CTL,
			" nxge_unmap_rxdma_channel_buf_ring: "
			"rx_msg_p $%p",
			rx_msg_p));
		if (rx_msg_p != NULL) {
			freeb(rx_msg_p->rx_mblk_p);
			rx_msg_ring[i] = NULL;
		}
	}

	MUTEX_DESTROY(&rbr_p->post_lock);
	MUTEX_DESTROY(&rbr_p->lock);
	KMEM_FREE(ring_info, sizeof (rxring_info_t));
	KMEM_FREE(rx_msg_ring, size);
	KMEM_FREE(rbr_p, sizeof (rx_rbr_ring_t));

	NXGE_DEBUG_MSG((nxgep, MEM2_CTL,
		"<== nxge_unmap_rxdma_channel_buf_ring"));
}

static nxge_status_t
nxge_rxdma_hw_start_common(p_nxge_t nxgep)
{
	nxge_status_t		status = NXGE_OK;

	NXGE_DEBUG_MSG((nxgep, MEM2_CTL, "==> nxge_rxdma_hw_start_common"));

	/*
	 * Load the sharable parameters by writing to the
	 * function zero control registers. These FZC registers
	 * should be initialized only once for the entire chip.
	 */
	(void) nxge_init_fzc_rx_common(nxgep);

	/*
	 * Initialize the RXDMA port specific FZC control configurations.
	 * These FZC registers are pertaining to each port.
	 */
	(void) nxge_init_fzc_rxdma_port(nxgep);

	NXGE_DEBUG_MSG((nxgep, MEM2_CTL, "==> nxge_rxdma_hw_start_common"));

	return (status);
}

/*ARGSUSED*/
static void
nxge_rxdma_hw_stop_common(p_nxge_t nxgep)
{
	NXGE_DEBUG_MSG((nxgep, MEM2_CTL, "==> nxge_rxdma_hw_stop_common"));

	NXGE_DEBUG_MSG((nxgep, MEM2_CTL, "==> nxge_rxdma_hw_stop_common"));
}

static nxge_status_t
nxge_rxdma_hw_start(p_nxge_t nxgep)
{
	int			i, ndmas;
	uint16_t		channel;
	p_rx_rbr_rings_t 	rx_rbr_rings;
	p_rx_rbr_ring_t		*rbr_rings;
	p_rx_rcr_rings_t 	rx_rcr_rings;
	p_rx_rcr_ring_t		*rcr_rings;
	p_rx_mbox_areas_t 	rx_mbox_areas_p;
	p_rx_mbox_t		*rx_mbox_p;
	nxge_status_t		status = NXGE_OK;

	NXGE_DEBUG_MSG((nxgep, MEM2_CTL, "==> nxge_rxdma_hw_start"));

	rx_rbr_rings = nxgep->rx_rbr_rings;
	rx_rcr_rings = nxgep->rx_rcr_rings;
	if (rx_rbr_rings == NULL || rx_rcr_rings == NULL) {
		NXGE_DEBUG_MSG((nxgep, RX_CTL,
			"<== nxge_rxdma_hw_start: NULL ring pointers"));
		return (NXGE_ERROR);
	}
	ndmas = rx_rbr_rings->ndmas;
	if (ndmas == 0) {
		NXGE_DEBUG_MSG((nxgep, RX_CTL,
			"<== nxge_rxdma_hw_start: no dma channel allocated"));
		return (NXGE_ERROR);
	}

	NXGE_DEBUG_MSG((nxgep, MEM2_CTL,
		"==> nxge_rxdma_hw_start (ndmas %d)", ndmas));

	rbr_rings = rx_rbr_rings->rbr_rings;
	rcr_rings = rx_rcr_rings->rcr_rings;
	rx_mbox_areas_p = nxgep->rx_mbox_areas_p;
	if (rx_mbox_areas_p) {
		rx_mbox_p = rx_mbox_areas_p->rxmbox_areas;
	}

	for (i = 0; i < ndmas; i++) {
		channel = rbr_rings[i]->rdc;
		NXGE_DEBUG_MSG((nxgep, MEM2_CTL,
			"==> nxge_rxdma_hw_start (ndmas %d) channel %d",
				ndmas, channel));
		status = nxge_rxdma_start_channel(nxgep, channel,
				(p_rx_rbr_ring_t)rbr_rings[i],
				(p_rx_rcr_ring_t)rcr_rings[i],
				(p_rx_mbox_t)rx_mbox_p[i]);
		if (status != NXGE_OK) {
			goto nxge_rxdma_hw_start_fail1;
		}
	}

	NXGE_DEBUG_MSG((nxgep, MEM2_CTL, "==> nxge_rxdma_hw_start: "
		"rx_rbr_rings 0x%016llx rings 0x%016llx",
		rx_rbr_rings, rx_rcr_rings));

	goto nxge_rxdma_hw_start_exit;

nxge_rxdma_hw_start_fail1:
	NXGE_ERROR_MSG((nxgep, NXGE_ERR_CTL,
		"==> nxge_rxdma_hw_start: disable "
		"(status 0x%x channel %d i %d)", status, channel, i));
	for (; i >= 0; i--) {
		channel = rbr_rings[i]->rdc;
		(void) nxge_rxdma_stop_channel(nxgep, channel);
	}

nxge_rxdma_hw_start_exit:
	NXGE_DEBUG_MSG((nxgep, MEM2_CTL,
		"==> nxge_rxdma_hw_start: (status 0x%x)", status));

	return (status);
}

static void
nxge_rxdma_hw_stop(p_nxge_t nxgep)
{
	int			i, ndmas;
	uint16_t		channel;
	p_rx_rbr_rings_t 	rx_rbr_rings;
	p_rx_rbr_ring_t		*rbr_rings;
	p_rx_rcr_rings_t 	rx_rcr_rings;

	NXGE_DEBUG_MSG((nxgep, MEM2_CTL, "==> nxge_rxdma_hw_stop"));

	rx_rbr_rings = nxgep->rx_rbr_rings;
	rx_rcr_rings = nxgep->rx_rcr_rings;
	if (rx_rbr_rings == NULL || rx_rcr_rings == NULL) {
		NXGE_DEBUG_MSG((nxgep, RX_CTL,
			"<== nxge_rxdma_hw_stop: NULL ring pointers"));
		return;
	}
	ndmas = rx_rbr_rings->ndmas;
	if (!ndmas) {
		NXGE_DEBUG_MSG((nxgep, RX_CTL,
			"<== nxge_rxdma_hw_stop: no dma channel allocated"));
		return;
	}

	NXGE_DEBUG_MSG((nxgep, MEM2_CTL,
		"==> nxge_rxdma_hw_stop (ndmas %d)", ndmas));

	rbr_rings = rx_rbr_rings->rbr_rings;

	for (i = 0; i < ndmas; i++) {
		channel = rbr_rings[i]->rdc;
		NXGE_DEBUG_MSG((nxgep, MEM2_CTL,
			"==> nxge_rxdma_hw_stop (ndmas %d) channel %d",
				ndmas, channel));
		(void) nxge_rxdma_stop_channel(nxgep, channel);
	}

	NXGE_DEBUG_MSG((nxgep, MEM2_CTL, "==> nxge_rxdma_hw_stop: "
		"rx_rbr_rings 0x%016llx rings 0x%016llx",
		rx_rbr_rings, rx_rcr_rings));

	NXGE_DEBUG_MSG((nxgep, MEM2_CTL, "<== nxge_rxdma_hw_stop"));
}


static nxge_status_t
nxge_rxdma_start_channel(p_nxge_t nxgep, uint16_t channel,
    p_rx_rbr_ring_t rbr_p, p_rx_rcr_ring_t rcr_p, p_rx_mbox_t mbox_p)

{
	npi_handle_t		handle;
	npi_status_t		rs = NPI_SUCCESS;
	rx_dma_ctl_stat_t	cs;
	rx_dma_ent_msk_t	ent_mask;
	nxge_status_t		status = NXGE_OK;

	NXGE_DEBUG_MSG((nxgep, MEM2_CTL, "==> nxge_rxdma_start_channel"));

	handle = NXGE_DEV_NPI_HANDLE(nxgep);

	NXGE_DEBUG_MSG((nxgep, MEM2_CTL, "nxge_rxdma_start_channel: "
		"npi handle addr $%p acc $%p",
		nxgep->npi_handle.regp, nxgep->npi_handle.regh));

	/* Reset RXDMA channel */
	rs = npi_rxdma_cfg_rdc_reset(handle, channel);
	if (rs != NPI_SUCCESS) {
		NXGE_ERROR_MSG((nxgep, NXGE_ERR_CTL,
			"==> nxge_rxdma_start_channel: "
			"reset rxdma failed (0x%08x channel %d)",
			status, channel));
		return (NXGE_ERROR | rs);
	}

	NXGE_DEBUG_MSG((nxgep, MEM2_CTL,
		"==> nxge_rxdma_start_channel: reset done: channel %d",
		channel));

	/*
	 * Initialize the RXDMA channel specific FZC control
	 * configurations. These FZC registers are pertaining
	 * to each RX channel (logical pages).
	 */
	status = nxge_init_fzc_rxdma_channel(nxgep,
			channel, rbr_p, rcr_p, mbox_p);
	if (status != NXGE_OK) {
		NXGE_ERROR_MSG((nxgep, NXGE_ERR_CTL,
			"==> nxge_rxdma_start_channel: "
			"init fzc rxdma failed (0x%08x channel %d)",
			status, channel));
		return (status);
	}

	NXGE_DEBUG_MSG((nxgep, MEM2_CTL,
		"==> nxge_rxdma_start_channel: fzc done"));

	/*
	 * Zero out the shadow  and prefetch ram.
	 */

	NXGE_DEBUG_MSG((nxgep, MEM2_CTL, "==> nxge_rxdma_start_channel: "
		"ram done"));

	/* Set up the interrupt event masks. */
	ent_mask.value = 0;
	ent_mask.value |= RX_DMA_ENT_MSK_RBREMPTY_MASK;
	rs = npi_rxdma_event_mask(handle, OP_SET, channel,
			&ent_mask);
	if (rs != NPI_SUCCESS) {
		NXGE_ERROR_MSG((nxgep, NXGE_ERR_CTL,
			"==> nxge_rxdma_start_channel: "
			"init rxdma event masks failed (0x%08x channel %d)",
			status, channel));
		return (NXGE_ERROR | rs);
	}

	NXGE_DEBUG_MSG((nxgep, MEM2_CTL, "==> nxge_rxdma_start_channel: "
		"event done: channel %d (mask 0x%016llx)",
		channel, ent_mask.value));

	/* Initialize the receive DMA control and status register */
	cs.value = 0;
	cs.bits.hdw.mex = 1;
	cs.bits.hdw.rcrthres = 1;
	cs.bits.hdw.rcrto = 1;
	cs.bits.hdw.rbr_empty = 1;
	status = nxge_init_rxdma_channel_cntl_stat(nxgep, channel, &cs);
	NXGE_DEBUG_MSG((nxgep, MEM2_CTL, "==> nxge_rxdma_start_channel: "
		"channel %d rx_dma_cntl_stat 0x%0016llx", channel, cs.value));
	if (status != NXGE_OK) {
		NXGE_ERROR_MSG((nxgep, NXGE_ERR_CTL,
			"==> nxge_rxdma_start_channel: "
			"init rxdma control register failed (0x%08x channel %d",
			status, channel));
		return (status);
	}

	NXGE_DEBUG_MSG((nxgep, MEM2_CTL, "==> nxge_rxdma_start_channel: "
		"control done - channel %d cs 0x%016llx", channel, cs.value));

	/*
	 * Load RXDMA descriptors, buffers, mailbox,
	 * initialise the receive DMA channels and
	 * enable each DMA channel.
	 */
	status = nxge_enable_rxdma_channel(nxgep,
			channel, rbr_p, rcr_p, mbox_p);

	if (status != NXGE_OK) {
		NXGE_ERROR_MSG((nxgep, NXGE_ERR_CTL,
			    " nxge_rxdma_start_channel: "
			    " init enable rxdma failed (0x%08x channel %d)",
			    status, channel));
		return (status);
	}

	ent_mask.value = 0;
	ent_mask.value |= (RX_DMA_ENT_MSK_WRED_DROP_MASK |
				RX_DMA_ENT_MSK_PTDROP_PKT_MASK);
	rs = npi_rxdma_event_mask(handle, OP_SET, channel,
			&ent_mask);
	if (rs != NPI_SUCCESS) {
		NXGE_DEBUG_MSG((nxgep, MEM2_CTL,
			"==> nxge_rxdma_start_channel: "
			"init rxdma event masks failed (0x%08x channel %d)",
			status, channel));
		return (NXGE_ERROR | rs);
	}

	NXGE_DEBUG_MSG((nxgep, MEM2_CTL, "==> nxge_rxdma_start_channel: "
		"control done - channel %d cs 0x%016llx", channel, cs.value));

	NXGE_DEBUG_MSG((nxgep, MEM2_CTL,
		"==> nxge_rxdma_start_channel: enable done"));

	NXGE_DEBUG_MSG((nxgep, MEM2_CTL, "<== nxge_rxdma_start_channel"));

	return (NXGE_OK);
}

static nxge_status_t
nxge_rxdma_stop_channel(p_nxge_t nxgep, uint16_t channel)
{
	npi_handle_t		handle;
	npi_status_t		rs = NPI_SUCCESS;
	rx_dma_ctl_stat_t	cs;
	rx_dma_ent_msk_t	ent_mask;
	nxge_status_t		status = NXGE_OK;

	NXGE_DEBUG_MSG((nxgep, RX_CTL, "==> nxge_rxdma_stop_channel"));

	handle = NXGE_DEV_NPI_HANDLE(nxgep);

	NXGE_DEBUG_MSG((nxgep, RX_CTL, "nxge_rxdma_stop_channel: "
		"npi handle addr $%p acc $%p",
		nxgep->npi_handle.regp, nxgep->npi_handle.regh));

	/* Reset RXDMA channel */
	rs = npi_rxdma_cfg_rdc_reset(handle, channel);
	if (rs != NPI_SUCCESS) {
		NXGE_ERROR_MSG((nxgep, NXGE_ERR_CTL,
			    " nxge_rxdma_stop_channel: "
			    " reset rxdma failed (0x%08x channel %d)",
			    rs, channel));
		return (NXGE_ERROR | rs);
	}

	NXGE_DEBUG_MSG((nxgep, RX_CTL,
		"==> nxge_rxdma_stop_channel: reset done"));

	/* Set up the interrupt event masks. */
	ent_mask.value = RX_DMA_ENT_MSK_ALL;
	rs = npi_rxdma_event_mask(handle, OP_SET, channel,
			&ent_mask);
	if (rs != NPI_SUCCESS) {
		NXGE_ERROR_MSG((nxgep, NXGE_ERR_CTL,
			    "==> nxge_rxdma_stop_channel: "
			    "set rxdma event masks failed (0x%08x channel %d)",
			    rs, channel));
		return (NXGE_ERROR | rs);
	}

	NXGE_DEBUG_MSG((nxgep, RX_CTL,
		"==> nxge_rxdma_stop_channel: event done"));

	/* Initialize the receive DMA control and status register */
	cs.value = 0;
	status = nxge_init_rxdma_channel_cntl_stat(nxgep, channel,
			&cs);
	NXGE_DEBUG_MSG((nxgep, RX_CTL, "==> nxge_rxdma_stop_channel: control "
		" to default (all 0s) 0x%08x", cs.value));
	if (status != NXGE_OK) {
		NXGE_ERROR_MSG((nxgep, NXGE_ERR_CTL,
			    " nxge_rxdma_stop_channel: init rxdma"
			    " control register failed (0x%08x channel %d",
			status, channel));
		return (status);
	}

	NXGE_DEBUG_MSG((nxgep, RX_CTL,
		"==> nxge_rxdma_stop_channel: control done"));

	/* disable dma channel */
	status = nxge_disable_rxdma_channel(nxgep, channel);

	if (status != NXGE_OK) {
		NXGE_ERROR_MSG((nxgep, NXGE_ERR_CTL,
			    " nxge_rxdma_stop_channel: "
			    " init enable rxdma failed (0x%08x channel %d)",
			    status, channel));
		return (status);
	}

	NXGE_DEBUG_MSG((nxgep,
		RX_CTL, "==> nxge_rxdma_stop_channel: disable done"));

	NXGE_DEBUG_MSG((nxgep, RX_CTL, "<== nxge_rxdma_stop_channel"));

	return (NXGE_OK);
}

nxge_status_t
nxge_rxdma_handle_sys_errors(p_nxge_t nxgep)
{
	npi_handle_t		handle;
	p_nxge_rdc_sys_stats_t	statsp;
	rx_ctl_dat_fifo_stat_t	stat;
	uint32_t		zcp_err_status;
	uint32_t		ipp_err_status;
	nxge_status_t		status = NXGE_OK;
	npi_status_t		rs = NPI_SUCCESS;
	boolean_t		my_err = B_FALSE;

	handle = nxgep->npi_handle;
	statsp = (p_nxge_rdc_sys_stats_t)&nxgep->statsp->rdc_sys_stats;

	rs = npi_rxdma_rxctl_fifo_error_intr_get(handle, &stat);

	if (rs != NPI_SUCCESS)
		return (NXGE_ERROR | rs);

	if (stat.bits.ldw.id_mismatch) {
		statsp->id_mismatch++;
		NXGE_FM_REPORT_ERROR(nxgep, nxgep->mac.portnum, NULL,
					NXGE_FM_EREPORT_RDMC_ID_MISMATCH);
		/* Global fatal error encountered */
	}

	if ((stat.bits.ldw.zcp_eop_err) || (stat.bits.ldw.ipp_eop_err)) {
		switch (nxgep->mac.portnum) {
		case 0:
			if ((stat.bits.ldw.zcp_eop_err & FIFO_EOP_PORT0) ||
				(stat.bits.ldw.ipp_eop_err & FIFO_EOP_PORT0)) {
				my_err = B_TRUE;
				zcp_err_status = stat.bits.ldw.zcp_eop_err;
				ipp_err_status = stat.bits.ldw.ipp_eop_err;
			}
			break;
		case 1:
			if ((stat.bits.ldw.zcp_eop_err & FIFO_EOP_PORT1) ||
				(stat.bits.ldw.ipp_eop_err & FIFO_EOP_PORT1)) {
				my_err = B_TRUE;
				zcp_err_status = stat.bits.ldw.zcp_eop_err;
				ipp_err_status = stat.bits.ldw.ipp_eop_err;
			}
			break;
		case 2:
			if ((stat.bits.ldw.zcp_eop_err & FIFO_EOP_PORT2) ||
				(stat.bits.ldw.ipp_eop_err & FIFO_EOP_PORT2)) {
				my_err = B_TRUE;
				zcp_err_status = stat.bits.ldw.zcp_eop_err;
				ipp_err_status = stat.bits.ldw.ipp_eop_err;
			}
			break;
		case 3:
			if ((stat.bits.ldw.zcp_eop_err & FIFO_EOP_PORT3) ||
				(stat.bits.ldw.ipp_eop_err & FIFO_EOP_PORT3)) {
				my_err = B_TRUE;
				zcp_err_status = stat.bits.ldw.zcp_eop_err;
				ipp_err_status = stat.bits.ldw.ipp_eop_err;
			}
			break;
		default:
			return (NXGE_ERROR);
		}
	}

	if (my_err) {
		status = nxge_rxdma_handle_port_errors(nxgep, ipp_err_status,
							zcp_err_status);
		if (status != NXGE_OK)
			return (status);
	}

	return (NXGE_OK);
}

static nxge_status_t
nxge_rxdma_handle_port_errors(p_nxge_t nxgep, uint32_t ipp_status,
							uint32_t zcp_status)
{
	boolean_t		rxport_fatal = B_FALSE;
	p_nxge_rdc_sys_stats_t	statsp;
	nxge_status_t		status = NXGE_OK;
	uint8_t			portn;

	portn = nxgep->mac.portnum;
	statsp = (p_nxge_rdc_sys_stats_t)&nxgep->statsp->rdc_sys_stats;

	if (ipp_status & (0x1 << portn)) {
		statsp->ipp_eop_err++;
		NXGE_FM_REPORT_ERROR(nxgep, portn, NULL,
					NXGE_FM_EREPORT_RDMC_IPP_EOP_ERR);
		rxport_fatal = B_TRUE;
	}

	if (zcp_status & (0x1 << portn)) {
		statsp->zcp_eop_err++;
		NXGE_FM_REPORT_ERROR(nxgep, portn, NULL,
					NXGE_FM_EREPORT_RDMC_ZCP_EOP_ERR);
		rxport_fatal = B_TRUE;
	}

	if (rxport_fatal) {
		NXGE_ERROR_MSG((nxgep, NXGE_ERR_CTL,
			    " nxge_rxdma_handle_port_error: "
			    " fatal error on Port #%d\n",
				portn));
		status = nxge_rx_port_fatal_err_recover(nxgep);
		if (status == NXGE_OK) {
			FM_SERVICE_RESTORED(nxgep);
		}
	}

	return (status);
}

static nxge_status_t
nxge_rxdma_fatal_err_recover(p_nxge_t nxgep, uint16_t channel)
{
	npi_handle_t		handle;
	npi_status_t		rs = NPI_SUCCESS;
	nxge_status_t		status = NXGE_OK;
	p_rx_rbr_ring_t		rbrp;
	p_rx_rcr_ring_t		rcrp;
	p_rx_mbox_t		mboxp;
	rx_dma_ent_msk_t	ent_mask;
	p_nxge_dma_common_t	dmap;
	int			ring_idx;
	uint32_t		ref_cnt;
	p_rx_msg_t		rx_msg_p;
	int			i;
	uint32_t		nxge_port_rcr_size;

	NXGE_DEBUG_MSG((nxgep, RX_CTL, "<== nxge_rxdma_fatal_err_recover"));
	NXGE_ERROR_MSG((nxgep, NXGE_ERR_CTL,
			"Recovering from RxDMAChannel#%d error...", channel));

	/*
	 * Stop the dma channel waits for the stop done.
	 * If the stop done bit is not set, then create
	 * an error.
	 */

	handle = NXGE_DEV_NPI_HANDLE(nxgep);
	NXGE_DEBUG_MSG((nxgep, RX_CTL, "Rx DMA stop..."));

	ring_idx = nxge_rxdma_get_ring_index(nxgep, channel);
	rbrp = (p_rx_rbr_ring_t)nxgep->rx_rbr_rings->rbr_rings[ring_idx];
	rcrp = (p_rx_rcr_ring_t)nxgep->rx_rcr_rings->rcr_rings[ring_idx];

	MUTEX_ENTER(&rcrp->lock);
	MUTEX_ENTER(&rbrp->lock);
	MUTEX_ENTER(&rbrp->post_lock);

	NXGE_DEBUG_MSG((nxgep, RX_CTL, "Disable RxDMA channel..."));

	rs = npi_rxdma_cfg_rdc_disable(handle, channel);
	if (rs != NPI_SUCCESS) {
		NXGE_ERROR_MSG((nxgep, NXGE_ERR_CTL,
			"nxge_disable_rxdma_channel:failed"));
		goto fail;
	}

	NXGE_DEBUG_MSG((nxgep, RX_CTL, "Disable RxDMA interrupt..."));

	/* Disable interrupt */
	ent_mask.value = RX_DMA_ENT_MSK_ALL;
	rs = npi_rxdma_event_mask(handle, OP_SET, channel, &ent_mask);
	if (rs != NPI_SUCCESS) {
		NXGE_ERROR_MSG((nxgep, NXGE_ERR_CTL,
				"nxge_rxdma_stop_channel: "
				"set rxdma event masks failed (channel %d)",
				channel));
	}

	NXGE_DEBUG_MSG((nxgep, RX_CTL, "RxDMA channel reset..."));

	/* Reset RXDMA channel */
	rs = npi_rxdma_cfg_rdc_reset(handle, channel);
	if (rs != NPI_SUCCESS) {
		NXGE_ERROR_MSG((nxgep, NXGE_ERR_CTL,
			"nxge_rxdma_fatal_err_recover: "
				" reset rxdma failed (channel %d)", channel));
		goto fail;
	}

	nxge_port_rcr_size = nxgep->nxge_port_rcr_size;

	mboxp =
	(p_rx_mbox_t)nxgep->rx_mbox_areas_p->rxmbox_areas[ring_idx];

	rbrp->rbr_wr_index = (rbrp->rbb_max - 1);
	rbrp->rbr_rd_index = 0;

	rcrp->comp_rd_index = 0;
	rcrp->comp_wt_index = 0;
	rcrp->rcr_desc_rd_head_p = rcrp->rcr_desc_first_p =
		(p_rcr_entry_t)DMA_COMMON_VPTR(rcrp->rcr_desc);
	rcrp->rcr_desc_rd_head_pp = rcrp->rcr_desc_first_pp =
		(p_rcr_entry_t)DMA_COMMON_IOADDR(rcrp->rcr_desc);

	rcrp->rcr_desc_last_p = rcrp->rcr_desc_rd_head_p +
		(nxge_port_rcr_size - 1);
	rcrp->rcr_desc_last_pp = rcrp->rcr_desc_rd_head_pp +
		(nxge_port_rcr_size - 1);

	dmap = (p_nxge_dma_common_t)&rcrp->rcr_desc;
	bzero((caddr_t)dmap->kaddrp, dmap->alength);

	cmn_err(CE_NOTE, "!rbr entries = %d\n", rbrp->rbr_max_size);

	for (i = 0; i < rbrp->rbr_max_size; i++) {
		rx_msg_p = rbrp->rx_msg_ring[i];
		ref_cnt = rx_msg_p->ref_cnt;
		if (ref_cnt != 1) {
			if (rx_msg_p->cur_usage_cnt !=
					rx_msg_p->max_usage_cnt) {
				NXGE_ERROR_MSG((nxgep, NXGE_ERR_CTL,
						"buf[%d]: cur_usage_cnt = %d "
						"max_usage_cnt = %d\n", i,
						rx_msg_p->cur_usage_cnt,
						rx_msg_p->max_usage_cnt));
			} else {
				/* Buffer can be re-posted */
				rx_msg_p->free = B_TRUE;
				rx_msg_p->cur_usage_cnt = 0;
				rx_msg_p->max_usage_cnt = 0xbaddcafe;
				rx_msg_p->pkt_buf_size = 0;
			}
		}
	}

	NXGE_DEBUG_MSG((nxgep, RX_CTL, "RxDMA channel re-start..."));

	status = nxge_rxdma_start_channel(nxgep, channel, rbrp, rcrp, mboxp);
	if (status != NXGE_OK) {
		goto fail;
	}

	MUTEX_EXIT(&rbrp->post_lock);
	MUTEX_EXIT(&rbrp->lock);
	MUTEX_EXIT(&rcrp->lock);

	NXGE_ERROR_MSG((nxgep, NXGE_ERR_CTL,
			"Recovery Successful, RxDMAChannel#%d Restored",
			channel));
	NXGE_DEBUG_MSG((nxgep, RX_CTL, "==> nxge_rxdma_fatal_err_recover"));

	return (NXGE_OK);
fail:
	MUTEX_EXIT(&rbrp->post_lock);
	MUTEX_EXIT(&rbrp->lock);
	MUTEX_EXIT(&rcrp->lock);
	NXGE_ERROR_MSG((nxgep, NXGE_ERR_CTL, "Recovery failed"));

	return (NXGE_ERROR | rs);
}

nxge_status_t
nxge_rx_port_fatal_err_recover(p_nxge_t nxgep)
{
	nxge_status_t		status = NXGE_OK;
	p_nxge_dma_common_t	*dma_buf_p;
	uint16_t		channel;
	int			ndmas;
	int			i;

	NXGE_DEBUG_MSG((nxgep, RX_CTL, "<== nxge_rx_port_fatal_err_recover"));
	NXGE_ERROR_MSG((nxgep, NXGE_ERR_CTL,
				"Recovering from RxPort error..."));
	/* Disable RxMAC */

	NXGE_DEBUG_MSG((nxgep, RX_CTL, "Disable RxMAC...\n"));
	if (nxge_rx_mac_disable(nxgep) != NXGE_OK)
		goto fail;

	NXGE_DELAY(1000);

	NXGE_DEBUG_MSG((nxgep, RX_CTL, "Stop all RxDMA channels..."));

	ndmas = nxgep->rx_buf_pool_p->ndmas;
	dma_buf_p = nxgep->rx_buf_pool_p->dma_buf_pool_p;

	for (i = 0; i < ndmas; i++) {
		channel = ((p_nxge_dma_common_t)dma_buf_p[i])->dma_channel;
		if (nxge_rxdma_fatal_err_recover(nxgep, channel) != NXGE_OK) {
			NXGE_ERROR_MSG((nxgep, NXGE_ERR_CTL,
					"Could not recover channel %d",
					channel));
		}
	}

	NXGE_DEBUG_MSG((nxgep, RX_CTL, "Reset IPP..."));

	/* Reset IPP */
	if (nxge_ipp_reset(nxgep) != NXGE_OK) {
		NXGE_ERROR_MSG((nxgep, NXGE_ERR_CTL,
			"nxge_rx_port_fatal_err_recover: "
			"Failed to reset IPP"));
		goto fail;
	}

	NXGE_DEBUG_MSG((nxgep, RX_CTL, "Reset RxMAC..."));

	/* Reset RxMAC */
	if (nxge_rx_mac_reset(nxgep) != NXGE_OK) {
		NXGE_ERROR_MSG((nxgep, NXGE_ERR_CTL,
			"nxge_rx_port_fatal_err_recover: "
			"Failed to reset RxMAC"));
		goto fail;
	}

	NXGE_DEBUG_MSG((nxgep, RX_CTL, "Re-initialize IPP..."));

	/* Re-Initialize IPP */
	if (nxge_ipp_init(nxgep) != NXGE_OK) {
		NXGE_ERROR_MSG((nxgep, NXGE_ERR_CTL,
			"nxge_rx_port_fatal_err_recover: "
			"Failed to init IPP"));
		goto fail;
	}

	NXGE_DEBUG_MSG((nxgep, RX_CTL, "Re-initialize RxMAC..."));

	/* Re-Initialize RxMAC */
	if ((status = nxge_rx_mac_init(nxgep)) != NXGE_OK) {
		NXGE_ERROR_MSG((nxgep, NXGE_ERR_CTL,
			"nxge_rx_port_fatal_err_recover: "
			"Failed to reset RxMAC"));
		goto fail;
	}

	NXGE_DEBUG_MSG((nxgep, RX_CTL, "Re-enable RxMAC..."));

	/* Re-enable RxMAC */
	if ((status = nxge_rx_mac_enable(nxgep)) != NXGE_OK) {
		NXGE_ERROR_MSG((nxgep, NXGE_ERR_CTL,
			"nxge_rx_port_fatal_err_recover: "
			"Failed to enable RxMAC"));
		goto fail;
	}

	NXGE_ERROR_MSG((nxgep, NXGE_ERR_CTL,
			"Recovery Successful, RxPort Restored"));

	return (NXGE_OK);
fail:
	NXGE_ERROR_MSG((nxgep, NXGE_ERR_CTL, "Recovery failed"));
	return (status);
}

void
nxge_rxdma_inject_err(p_nxge_t nxgep, uint32_t err_id, uint8_t chan)
{
	rx_dma_ctl_stat_t	cs;
	rx_ctl_dat_fifo_stat_t	cdfs;

	switch (err_id) {
	case NXGE_FM_EREPORT_RDMC_RCR_ACK_ERR:
	case NXGE_FM_EREPORT_RDMC_DC_FIFO_ERR:
	case NXGE_FM_EREPORT_RDMC_RCR_SHA_PAR:
	case NXGE_FM_EREPORT_RDMC_RBR_PRE_PAR:
	case NXGE_FM_EREPORT_RDMC_RBR_TMOUT:
	case NXGE_FM_EREPORT_RDMC_RSP_CNT_ERR:
	case NXGE_FM_EREPORT_RDMC_BYTE_EN_BUS:
	case NXGE_FM_EREPORT_RDMC_RSP_DAT_ERR:
	case NXGE_FM_EREPORT_RDMC_RCRINCON:
	case NXGE_FM_EREPORT_RDMC_RCRFULL:
	case NXGE_FM_EREPORT_RDMC_RBRFULL:
	case NXGE_FM_EREPORT_RDMC_RBRLOGPAGE:
	case NXGE_FM_EREPORT_RDMC_CFIGLOGPAGE:
	case NXGE_FM_EREPORT_RDMC_CONFIG_ERR:
		RXDMA_REG_READ64(nxgep->npi_handle, RX_DMA_CTL_STAT_DBG_REG,
			chan, &cs.value);
		if (err_id == NXGE_FM_EREPORT_RDMC_RCR_ACK_ERR)
			cs.bits.hdw.rcr_ack_err = 1;
		else if (err_id == NXGE_FM_EREPORT_RDMC_DC_FIFO_ERR)
			cs.bits.hdw.dc_fifo_err = 1;
		else if (err_id == NXGE_FM_EREPORT_RDMC_RCR_SHA_PAR)
			cs.bits.hdw.rcr_sha_par = 1;
		else if (err_id == NXGE_FM_EREPORT_RDMC_RBR_PRE_PAR)
			cs.bits.hdw.rbr_pre_par = 1;
		else if (err_id == NXGE_FM_EREPORT_RDMC_RBR_TMOUT)
			cs.bits.hdw.rbr_tmout = 1;
		else if (err_id == NXGE_FM_EREPORT_RDMC_RSP_CNT_ERR)
			cs.bits.hdw.rsp_cnt_err = 1;
		else if (err_id == NXGE_FM_EREPORT_RDMC_BYTE_EN_BUS)
			cs.bits.hdw.byte_en_bus = 1;
		else if (err_id == NXGE_FM_EREPORT_RDMC_RSP_DAT_ERR)
			cs.bits.hdw.rsp_dat_err = 1;
		else if (err_id == NXGE_FM_EREPORT_RDMC_CONFIG_ERR)
			cs.bits.hdw.config_err = 1;
		else if (err_id == NXGE_FM_EREPORT_RDMC_RCRINCON)
			cs.bits.hdw.rcrincon = 1;
		else if (err_id == NXGE_FM_EREPORT_RDMC_RCRFULL)
			cs.bits.hdw.rcrfull = 1;
		else if (err_id == NXGE_FM_EREPORT_RDMC_RBRFULL)
			cs.bits.hdw.rbrfull = 1;
		else if (err_id == NXGE_FM_EREPORT_RDMC_RBRLOGPAGE)
			cs.bits.hdw.rbrlogpage = 1;
		else if (err_id == NXGE_FM_EREPORT_RDMC_CFIGLOGPAGE)
			cs.bits.hdw.cfiglogpage = 1;
		cmn_err(CE_NOTE, "!Write 0x%lx to RX_DMA_CTL_STAT_DBG_REG\n",
				cs.value);
		RXDMA_REG_WRITE64(nxgep->npi_handle, RX_DMA_CTL_STAT_DBG_REG,
			chan, cs.value);
		break;
	case NXGE_FM_EREPORT_RDMC_ID_MISMATCH:
	case NXGE_FM_EREPORT_RDMC_ZCP_EOP_ERR:
	case NXGE_FM_EREPORT_RDMC_IPP_EOP_ERR:
		cdfs.value = 0;
		if (err_id ==  NXGE_FM_EREPORT_RDMC_ID_MISMATCH)
			cdfs.bits.ldw.id_mismatch = (1 << nxgep->mac.portnum);
		else if (err_id == NXGE_FM_EREPORT_RDMC_ZCP_EOP_ERR)
			cdfs.bits.ldw.zcp_eop_err = (1 << nxgep->mac.portnum);
		else if (err_id == NXGE_FM_EREPORT_RDMC_IPP_EOP_ERR)
			cdfs.bits.ldw.ipp_eop_err = (1 << nxgep->mac.portnum);
		cmn_err(CE_NOTE,
			"!Write 0x%lx to RX_CTL_DAT_FIFO_STAT_DBG_REG\n",
			cdfs.value);
		RXDMA_REG_WRITE64(nxgep->npi_handle,
			RX_CTL_DAT_FIFO_STAT_DBG_REG, chan, cdfs.value);
		break;
	case NXGE_FM_EREPORT_RDMC_DCF_ERR:
		break;
	case NXGE_FM_EREPORT_RDMC_COMPLETION_ERR:
		break;
	}
}


static uint16_t
nxge_get_pktbuf_size(p_nxge_t nxgep, int bufsz_type, rbr_cfig_b_t rbr_cfgb)
{
	uint16_t sz = RBR_BKSIZE_8K_BYTES;

	switch (bufsz_type) {
	case RCR_PKTBUFSZ_0:
		switch (rbr_cfgb.bits.ldw.bufsz0) {
		case RBR_BUFSZ0_256B:
			sz = RBR_BUFSZ0_256_BYTES;
			break;
		case RBR_BUFSZ0_512B:
			sz = RBR_BUFSZ0_512B_BYTES;
			break;
		case RBR_BUFSZ0_1K:
			sz = RBR_BUFSZ0_1K_BYTES;
			break;
		case RBR_BUFSZ0_2K:
			sz = RBR_BUFSZ0_2K_BYTES;
			break;
		default:
			NXGE_ERROR_MSG((nxgep, NXGE_ERR_CTL,
			"nxge_get_pktbug_size: bad bufsz0"));
			break;
		}
		break;
	case RCR_PKTBUFSZ_1:
		switch (rbr_cfgb.bits.ldw.bufsz1) {
		case RBR_BUFSZ1_1K:
			sz = RBR_BUFSZ1_1K_BYTES;
			break;
		case RBR_BUFSZ1_2K:
			sz = RBR_BUFSZ1_2K_BYTES;
			break;
		case RBR_BUFSZ1_4K:
			sz = RBR_BUFSZ1_4K_BYTES;
			break;
		case RBR_BUFSZ1_8K:
			sz = RBR_BUFSZ1_8K_BYTES;
			break;
		default:
			NXGE_ERROR_MSG((nxgep, NXGE_ERR_CTL,
			"nxge_get_pktbug_size: bad bufsz1"));
			break;
		}
		break;
	case RCR_PKTBUFSZ_2:
		switch (rbr_cfgb.bits.ldw.bufsz2) {
		case RBR_BUFSZ2_2K:
			sz = RBR_BUFSZ2_2K_BYTES;
			break;
		case RBR_BUFSZ2_4K:
			sz = RBR_BUFSZ2_4K_BYTES;
			break;
		case RBR_BUFSZ2_8K:
			sz = RBR_BUFSZ2_8K_BYTES;
			break;
		case RBR_BUFSZ2_16K:
			sz = RBR_BUFSZ2_16K_BYTES;
			break;
		default:
			NXGE_ERROR_MSG((nxgep, NXGE_ERR_CTL,
			"nxge_get_pktbug_size: bad bufsz2"));
			break;
		}
		break;
	case RCR_SINGLE_BLOCK:
		switch (rbr_cfgb.bits.ldw.bksize) {
		case BKSIZE_4K:
			sz = RBR_BKSIZE_4K_BYTES;
			break;
		case BKSIZE_8K:
			sz = RBR_BKSIZE_8K_BYTES;
			break;
		case BKSIZE_16K:
			sz = RBR_BKSIZE_16K_BYTES;
			break;
		case BKSIZE_32K:
			sz = RBR_BKSIZE_32K_BYTES;
			break;
		default:
			NXGE_ERROR_MSG((nxgep, NXGE_ERR_CTL,
			"nxge_get_pktbug_size: bad bksize"));
			break;
		}
		break;
	default:
		NXGE_ERROR_MSG((nxgep, NXGE_ERR_CTL,
		"nxge_get_pktbug_size: bad bufsz_type"));
		break;
	}
	return (sz);
}
