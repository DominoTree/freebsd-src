/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2012 Oleksandr Tymoshenko <gonzo@freebsd.org>
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
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 */

#ifndef	_BCM2835_SDHCI_H_
#define	_BCM2835_SDHCI_H_

#define	BCM2835_DEFAULT_SDHCI_FREQ	50
#define	BCM2838_DEFAULT_SDHCI_FREQ	100

#define	BCM_SDHCI_BUFFER_SIZE		512
/*
 * NUM_DMA_SEGS is the number of DMA segments we want to accommodate on average.
 * We add in a number of segments based on how much we may need to spill into
 * another segment due to crossing page boundaries.  e.g. up to PAGE_SIZE, an
 * extra page is needed as we can cross a page boundary exactly once.
 */
#define	NUM_DMA_SEGS			1
#define	NUM_DMA_SPILL_SEGS		\
	((((NUM_DMA_SEGS * BCM_SDHCI_BUFFER_SIZE) - 1) / PAGE_SIZE) + 1)
#define	ALLOCATED_DMA_SEGS		(NUM_DMA_SEGS +	NUM_DMA_SPILL_SEGS)
#define	BCM_DMA_MAXSIZE			(NUM_DMA_SEGS * BCM_SDHCI_BUFFER_SIZE)

#define	BCM_SDHCI_SLOT_LEFT(slot)	\
	((slot)->curcmd->data->len - (slot)->offset)

#define	BCM_SDHCI_SEGSZ_LEFT(slot)	\
	min(BCM_DMA_MAXSIZE,		\
	    rounddown(BCM_SDHCI_SLOT_LEFT(slot), BCM_SDHCI_BUFFER_SIZE))

#define	DATA_PENDING_MASK	(SDHCI_INT_DATA_AVAIL | SDHCI_INT_SPACE_AVAIL)
#define	DATA_XFER_MASK		(DATA_PENDING_MASK | SDHCI_INT_DATA_END)

struct bcm_mmc_conf {
	int	clock_id;
	int	clock_src;
	int	default_freq;
	int	quirks;
	int	emmc_dreq;
};

extern struct bcm_mmc_conf bcm2835_sdhci_conf;
extern struct bcm_mmc_conf bcm2838_emmc2_conf;

extern int bcm2835_sdhci_pio_mode;

struct bcm_sdhci_softc {
	device_t		sc_dev;
	struct resource *	sc_mem_res;
	struct resource *	sc_irq_res;
	bus_space_tag_t		sc_bst;
	bus_space_handle_t	sc_bsh;
	void *			sc_intrhand;
	struct mmc_request *	sc_req;
	struct sdhci_slot	sc_slot;
	struct mmc_helper	sc_mmc_helper;
	int			sc_dma_ch;
	bus_dma_tag_t		sc_dma_tag;
	bus_dmamap_t		sc_dma_map;
	vm_paddr_t		sc_sdhci_buffer_phys;
	bus_addr_t		dmamap_seg_addrs[ALLOCATED_DMA_SEGS];
	bus_size_t		dmamap_seg_sizes[ALLOCATED_DMA_SEGS];
	int			dmamap_seg_count;
	int			dmamap_seg_index;
	int			dmamap_status;
	uint32_t		blksz_and_count;
	uint32_t		cmd_and_mode;
	bool			need_update_blk;
	bool			sc_platform_dma;
#ifdef NOTYET
	device_t		clkman;
#endif
	struct bcm_mmc_conf *	conf;
};

DECLARE_CLASS(bcm_sdhci_driver);

int	bcm_sdhci_attach_slot(device_t dev, u_int default_freq);
void	bcm_sdhci_attach_done(device_t dev);
void	bcm_sdhci_release_resources(device_t dev);
device_detach_t bcm_sdhci_detach;

uint32_t bcm_sdhci_read_4(device_t dev, struct sdhci_slot *slot,
	    bus_size_t off);
void	bcm_sdhci_write_4(device_t dev, struct sdhci_slot *slot,
	    bus_size_t off, uint32_t val);

#endif	/* _BCM2835_SDHCI_H_ */
