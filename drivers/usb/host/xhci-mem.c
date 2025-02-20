// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * USB HOST XHCI Controller stack
 *
 * Based on xHCI host controller driver in linux-kernel
 * by Sarah Sharp.
 *
 * Copyright (C) 2008 Intel Corp.
 * Author: Sarah Sharp
 *
 * Copyright (C) 2013 Samsung Electronics Co.Ltd
 * Authors: Vivek Gautam <gautam.vivek@samsung.com>
 *	    Vikas Sajjan <vikas.sajjan@samsung.com>
 */

#include <clock.h>
#include <common.h>
#include <dma.h>
#include <init.h>
#include <io.h>
#include <linux/err.h>
#include <linux/sizes.h>
#include <linux/usb/usb.h>
#include <linux/usb/xhci.h>
#include <asm/unaligned.h>

#include "xhci.h"

/*
 * The memory handling for the xhci controller is done different
 * in barebox than in the original U-Boot driver.
 * All device memory is allocated with dma_alloc_coherent(), hence
 * xhci_flush_cache()/xhci_inval_cache() can be no-ops. They are
 * left here for reference if we ever want to change this behaviour.
 * The only exception are the user buffers passed into the driver. These
 * are synced with dma_sync_single_for_*() explicitly.
 */

/**
 * flushes the address passed till the length
 *
 * @param addr	pointer to memory region to be flushed
 * @param len	the length of the cache line to be flushed
 * @return none
 */
void xhci_flush_cache(uintptr_t addr, u32 len)
{
	BUG_ON((void *)addr == NULL || len == 0);
}

/**
 * invalidates the address passed till the length
 *
 * @param addr	pointer to memory region to be invalidates
 * @param len	the length of the cache line to be invalidated
 * @return none
 */
void xhci_inval_cache(uintptr_t addr, u32 len)
{
	BUG_ON((void *)addr == NULL || len == 0);
}

/**
 * Free memory allocated with xhci_malloc
 *
 * @param ptr	pointer to memory to be freed
 */
static void xhci_free(struct xhci_ctrl *ctrl, void *ptr)
{
	/*
	 * These should be freed with dma_free_coherent(), but this
	 * call needs the size which we don't have here. Let this
	 * be a no-op for now. This is called in the shutdown path only
	 * anyway, so loosing memory here won't sum up.
	 */
	dev_dbg(ctrl->dev, "%s: 0x%p\n", __func__, ptr);
}

/**
 * alloc coherent memory for xhci
 *
 * @param size	size of memory to be allocated
 * @return allocates the memory and returns the aligned pointer
 */
static void *xhci_malloc(struct xhci_ctrl *ctrl, unsigned int size, dma_addr_t *dma_addr)
{
	void *ptr;

	ptr = dma_alloc_coherent(DMA_DEVICE_BROKEN, size, dma_addr);
	if (!ptr)
		return NULL;

	dev_dbg(ctrl->dev, "%s: 0x%p (size %d)\n", __func__, ptr, size);

	return ptr;
}

/**
 * frees the "segment" pointer passed
 *
 * @param ptr	pointer to "segement" to be freed
 * @return none
 */
static void xhci_segment_free(struct xhci_ctrl *ctrl, struct xhci_segment *seg)
{
	xhci_free(ctrl, seg->trbs);
	seg->trbs = NULL;

	free(seg);
}

/**
 * frees the "ring" pointer passed
 *
 * @param ptr	pointer to "ring" to be freed
 * @return none
 */
static void xhci_ring_free(struct xhci_ctrl *ctrl, struct xhci_ring *ring)
{
	struct xhci_segment *seg;
	struct xhci_segment *first_seg;

	first_seg = ring->first_seg;
	seg = first_seg->next;
	while (seg != first_seg) {
		struct xhci_segment *next = seg->next;
		xhci_segment_free(ctrl, seg);
		seg = next;
	}
	xhci_segment_free(ctrl, first_seg);

	free(ring);
}

/**
 * Free the scratchpad buffer array and scratchpad buffers
 *
 * @ctrl	host controller data structure
 * @return	none
 */
static void xhci_scratchpad_free(struct xhci_ctrl *ctrl)
{
	if (!ctrl->scratchpad)
		return;

	ctrl->dcbaa->dev_context_ptrs[0] = 0;

	xhci_free(ctrl, ctrl->scratchpad->scratchpad);
	xhci_free(ctrl, ctrl->scratchpad->sp_array);
	free(ctrl->scratchpad);
	ctrl->scratchpad = NULL;
}

/**
 * frees the "xhci_container_ctx" pointer passed
 *
 * @param ptr	pointer to "xhci_container_ctx" to be freed
 * @return none
 */
static void xhci_free_container_ctx(struct xhci_ctrl *ctrl,
				    struct xhci_container_ctx *ctx)
{
	xhci_free(ctrl, ctx->bytes);
	free(ctx);
}

/**
 * frees the virtual devices for "xhci_ctrl" pointer passed
 *
 * @param ptr	pointer to "xhci_ctrl" whose virtual devices are to be freed
 * @return none
 */
static void xhci_free_virt_devices(struct xhci_ctrl *ctrl)
{
	int i;
	int slot_id;
	struct xhci_virt_device *virt_dev;

	/*
	 * refactored here to loop through all virt_dev
	 * Slot ID 0 is reserved
	 */
	for (slot_id = 0; slot_id < MAX_HC_SLOTS; slot_id++) {
		virt_dev = ctrl->devs[slot_id];
		if (!virt_dev)
			continue;

		ctrl->dcbaa->dev_context_ptrs[slot_id] = 0;

		for (i = 0; i < 31; ++i)
			if (virt_dev->eps[i].ring)
				xhci_ring_free(ctrl, virt_dev->eps[i].ring);

		if (virt_dev->in_ctx)
			xhci_free_container_ctx(ctrl, virt_dev->in_ctx);
		if (virt_dev->out_ctx)
			xhci_free_container_ctx(ctrl, virt_dev->out_ctx);

		free(virt_dev);
		/* make sure we are pointing to NULL */
		ctrl->devs[slot_id] = NULL;
	}
}

/**
 * frees all the memory allocated
 *
 * @param ptr	pointer to "xhci_ctrl" to be cleaned up
 * @return none
 */
void xhci_cleanup(struct xhci_ctrl *ctrl)
{
	xhci_ring_free(ctrl, ctrl->event_ring);
	xhci_ring_free(ctrl, ctrl->cmd_ring);
	xhci_scratchpad_free(ctrl);
	xhci_free_virt_devices(ctrl);
	xhci_free(ctrl, ctrl->erst.entries);
	xhci_free(ctrl, ctrl->dcbaa);
	free(ctrl->bounce_buffer);
}

/**
 * Make the prev segment point to the next segment.
 * Change the last TRB in the prev segment to be a Link TRB which points to the
 * address of the next segment.  The caller needs to set any Link TRB
 * related flags, such as End TRB, Toggle Cycle, and no snoop.
 *
 * @param prev	pointer to the previous segment
 * @param next	pointer to the next segment
 * @param link_trbs	flag to indicate whether to link the trbs or NOT
 * @return none
 */
static void xhci_link_segments(struct xhci_segment *prev,
				struct xhci_segment *next, bool link_trbs)
{
	u32 val;

	if (!prev || !next)
		return;
	prev->next = next;
	if (link_trbs) {
		prev->trbs[TRBS_PER_SEGMENT-1].link.segment_ptr =
			cpu_to_le64(next->dma);

		/*
		 * Set the last TRB in the segment to
		 * have a TRB type ID of Link TRB
		 */
		val = le32_to_cpu(prev->trbs[TRBS_PER_SEGMENT-1].link.control);
		val &= ~TRB_TYPE_BITMASK;
		val |= TRB_TYPE(TRB_LINK);
		prev->trbs[TRBS_PER_SEGMENT-1].link.control = cpu_to_le32(val);
	}
}

/**
 * Initialises the Ring's enqueue,dequeue,enq_seg pointers
 *
 * @param ring	pointer to the RING to be intialised
 * @return none
 */
static void xhci_initialize_ring_info(struct xhci_ring *ring)
{
	/*
	 * The ring is empty, so the enqueue pointer == dequeue pointer
	 */
	ring->enqueue = ring->first_seg->trbs;
	ring->enq_seg = ring->first_seg;
	ring->dequeue = ring->enqueue;
	ring->deq_seg = ring->first_seg;

	/*
	 * The ring is initialized to 0. The producer must write 1 to the
	 * cycle bit to handover ownership of the TRB, so PCS = 1.
	 * The consumer must compare CCS to the cycle bit to
	 * check ownership, so CCS = 1.
	 */
	ring->cycle_state = 1;
}

/**
 * Allocates a generic ring segment from the ring pool, sets the dma address,
 * initializes the segment to zero, and sets the private next pointer to NULL.
 * Section 4.11.1.1:
 * "All components of all Command and Transfer TRBs shall be initialized to '0'"
 *
 * @param	none
 * @return pointer to the newly allocated SEGMENT
 */
static struct xhci_segment *xhci_segment_alloc(struct xhci_ctrl *ctrl)
{
	struct xhci_segment *seg;

	seg = xzalloc(sizeof(*seg));

	seg->trbs = xhci_malloc(ctrl, SEGMENT_SIZE, &seg->dma);

	return seg;
}

/**
 * Create a new ring with zero or more segments.
 * TODO: current code only uses one-time-allocated single-segment rings
 * of 1KB anyway, so we might as well get rid of all the segment and
 * linking code (and maybe increase the size a bit, e.g. 4KB).
 *
 *
 * Link each segment together into a ring.
 * Set the end flag and the cycle toggle bit on the last segment.
 * See section 4.9.2 and figures 15 and 16 of XHCI spec rev1.0.
 *
 * @param num_segs	number of segments in the ring
 * @param link_trbs	flag to indicate whether to link the trbs or NOT
 * @return pointer to the newly created RING
 */
struct xhci_ring *xhci_ring_alloc(struct xhci_ctrl *ctrl,
				  unsigned int num_segs, bool link_trbs)
{
	struct xhci_ring *ring;
	struct xhci_segment *prev;

	ring = xmalloc(sizeof(*ring));

	if (num_segs == 0)
		return ring;

	ring->first_seg = xhci_segment_alloc(ctrl);
	BUG_ON(!ring->first_seg);

	num_segs--;

	prev = ring->first_seg;
	while (num_segs > 0) {
		struct xhci_segment *next;

		next = xhci_segment_alloc(ctrl);
		BUG_ON(!next);

		xhci_link_segments(prev, next, link_trbs);

		prev = next;
		num_segs--;
	}
	xhci_link_segments(prev, ring->first_seg, link_trbs);
	if (link_trbs) {
		/* See section 4.9.2.1 and 6.4.4.1 */
		prev->trbs[TRBS_PER_SEGMENT-1].link.control |=
					cpu_to_le32(LINK_TOGGLE);
	}
	xhci_initialize_ring_info(ring);

	return ring;
}

/**
 * Set up the scratchpad buffer array and scratchpad buffers
 *
 * @ctrl	host controller data structure
 * @return	-ENOMEM if buffer allocation fails, 0 on success
 */
static int xhci_scratchpad_alloc(struct xhci_ctrl *ctrl)
{
	struct xhci_hccr *hccr = ctrl->hccr;
	struct xhci_hcor *hcor = ctrl->hcor;
	struct xhci_scratchpad *scratchpad;
	dma_addr_t val_64;
	int num_sp;
	uint32_t page_size;
	void *buf;
	int i;

	num_sp = HCS_MAX_SCRATCHPAD(xhci_readl(&hccr->cr_hcsparams2));
	if (!num_sp)
		return 0;

	scratchpad = malloc(sizeof(*scratchpad));
	if (!scratchpad)
		goto fail_sp;
	ctrl->scratchpad = scratchpad;

	scratchpad->sp_array = xhci_malloc(ctrl, num_sp * sizeof(u64), &val_64);
	if (!scratchpad->sp_array)
		goto fail_sp2;

	ctrl->dcbaa->dev_context_ptrs[0] = cpu_to_le64(val_64);

	xhci_flush_cache((uintptr_t)&ctrl->dcbaa->dev_context_ptrs[0],
		sizeof(ctrl->dcbaa->dev_context_ptrs[0]));

	page_size = xhci_readl(&hcor->or_pagesize) & 0xffff;
	for (i = 0; i < 16; i++) {
		if ((0x1 & page_size) != 0)
			break;
		page_size = page_size >> 1;
	}
	BUG_ON(i == 16);

	page_size = 1 << (i + 12);
	buf = xhci_malloc(ctrl, num_sp * page_size, &val_64);
	if (!buf)
		goto fail_sp3;

	xhci_flush_cache((uintptr_t)buf, num_sp * page_size);

	scratchpad->scratchpad = buf;
	for (i = 0; i < num_sp; i++) {
		scratchpad->sp_array[i] = cpu_to_le64(val_64);
		val_64 += page_size;
	}

	xhci_flush_cache((uintptr_t)scratchpad->sp_array,
			 sizeof(u64) * num_sp);

	return 0;

fail_sp3:
	xhci_free(ctrl, scratchpad->sp_array);

fail_sp2:
	xhci_free(ctrl, scratchpad);
	ctrl->scratchpad = NULL;

fail_sp:
	return -ENOMEM;
}

/**
 * Allocates the Container context
 *
 * @param ctrl	Host controller data structure
 * @param type type of XHCI Container Context
 * @return NULL if failed else pointer to the context on success
 */
static struct xhci_container_ctx
		*xhci_alloc_container_ctx(struct xhci_ctrl *ctrl, int type)
{
	struct xhci_container_ctx *ctx;

	ctx = xmalloc(sizeof(struct xhci_container_ctx));

	BUG_ON((type != XHCI_CTX_TYPE_DEVICE) && (type != XHCI_CTX_TYPE_INPUT));
	ctx->type = type;
	ctx->size = (MAX_EP_CTX_NUM + 1) *
			CTX_SIZE(xhci_readl(&ctrl->hccr->cr_hccparams));
	if (type == XHCI_CTX_TYPE_INPUT)
		ctx->size += CTX_SIZE(xhci_readl(&ctrl->hccr->cr_hccparams));

	ctx->bytes = xhci_malloc(ctrl, ctx->size, &ctx->dma);

	return ctx;
}

/**
 * Allocating virtual device
 *
 * @param udev	pointer to USB deivce structure
 * @return 0 on success else -1 on failure
 */
int xhci_alloc_virt_device(struct xhci_ctrl *ctrl, unsigned int slot_id)
{
	u64 byte_64 = 0;
	struct xhci_virt_device *virt_dev;

	/* Slot ID 0 is reserved */
	if (ctrl->devs[slot_id]) {
		dev_err(ctrl->dev, "Virt dev for slot[%d] already allocated\n", slot_id);
		return -EEXIST;
	}

	ctrl->devs[slot_id] = malloc(sizeof(struct xhci_virt_device));

	if (!ctrl->devs[slot_id]) {
		dev_err(ctrl->dev, "Failed to allocate virtual device\n");
		return -ENOMEM;
	}

	memset(ctrl->devs[slot_id], 0, sizeof(struct xhci_virt_device));
	virt_dev = ctrl->devs[slot_id];

	/* Allocate the (output) device context that will be used in the HC. */
	virt_dev->out_ctx = xhci_alloc_container_ctx(ctrl,
					XHCI_CTX_TYPE_DEVICE);
	if (!virt_dev->out_ctx) {
		dev_err(ctrl->dev, "Failed to allocate out context for virt dev\n");
		return -ENOMEM;
	}

	/* Allocate the (input) device context for address device command */
	virt_dev->in_ctx = xhci_alloc_container_ctx(ctrl,
					XHCI_CTX_TYPE_INPUT);
	if (!virt_dev->in_ctx) {
		dev_err(ctrl->dev, "Failed to allocate in context for virt dev\n");
		return -ENOMEM;
	}

	/* Allocate endpoint 0 ring */
	virt_dev->eps[0].ring = xhci_ring_alloc(ctrl, 1, true);

	byte_64 = virt_dev->out_ctx->dma;

	/* Point to output device context in dcbaa. */
	ctrl->dcbaa->dev_context_ptrs[slot_id] = cpu_to_le64(byte_64);

	xhci_flush_cache((uintptr_t)&ctrl->dcbaa->dev_context_ptrs[slot_id],
			 sizeof(__le64));
	return 0;
}

/**
 * Allocates the necessary data structures
 * for XHCI host controller
 *
 * @param ctrl	Host controller data structure
 * @param hccr	pointer to HOST Controller Control Registers
 * @param hcor	pointer to HOST Controller Operational Registers
 * @return 0 if successful else -1 on failure
 */
int xhci_mem_init(struct xhci_ctrl *ctrl, struct xhci_hccr *hccr,
					struct xhci_hcor *hcor)
{
	dma_addr_t dma;
	uint64_t val_64;
	uint64_t trb_64;
	uint32_t val;
	uint64_t deq;
	int i;
	struct xhci_segment *seg;

	/* DCBAA initialization */
	ctrl->dcbaa = xhci_malloc(ctrl, sizeof(struct xhci_device_context_array),
				  &dma);
	ctrl->dcbaa->dma = dma;

	/* Set the pointer in DCBAA register */
	xhci_writeq(&hcor->or_dcbaap, dma);

	/* Command ring control pointer register initialization */
	ctrl->cmd_ring = xhci_ring_alloc(ctrl, 1, true);

	/* Set the address in the Command Ring Control register */
	trb_64 = ctrl->cmd_ring->first_seg->dma;
	val_64 = xhci_readq(&hcor->or_crcr);
	val_64 = (val_64 & (u64) CMD_RING_RSVD_BITS) |
		(trb_64 & (u64) ~CMD_RING_RSVD_BITS) |
		ctrl->cmd_ring->cycle_state;
	xhci_writeq(&hcor->or_crcr, val_64);

	/* write the address of db register */
	val = xhci_readl(&hccr->cr_dboff);
	val &= DBOFF_MASK;
	ctrl->dba = (struct xhci_doorbell_array *)((char *)hccr + val);

	/* write the address of runtime register */
	val = xhci_readl(&hccr->cr_rtsoff);
	val &= RTSOFF_MASK;
	ctrl->run_regs = (struct xhci_run_regs *)((char *)hccr + val);

	/* writting the address of ir_set structure */
	ctrl->ir_set = &ctrl->run_regs->ir_set[0];

	/* Event ring does not maintain link TRB */
	ctrl->event_ring = xhci_ring_alloc(ctrl, ERST_NUM_SEGS, false);
	ctrl->erst.entries = xhci_malloc(ctrl, sizeof(struct xhci_erst_entry) *
					 ERST_NUM_SEGS, &ctrl->erst.erst_dma_addr);

	ctrl->erst.num_entries = ERST_NUM_SEGS;

	for (val = 0, seg = ctrl->event_ring->first_seg;
			val < ERST_NUM_SEGS;
			val++) {
		struct xhci_erst_entry *entry = &ctrl->erst.entries[val];

		trb_64 = seg->dma;
		entry->seg_addr = cpu_to_le64(trb_64);
		entry->seg_size = cpu_to_le32(TRBS_PER_SEGMENT);
		entry->rsvd = 0;
		seg = seg->next;
	}
	xhci_flush_cache((uintptr_t)ctrl->erst.entries,
			 ERST_NUM_SEGS * sizeof(struct xhci_erst_entry));

	deq = xhci_trb_virt_to_dma(ctrl->event_ring->deq_seg,
				   ctrl->event_ring->dequeue);

	/* Update HC event ring dequeue pointer */
	xhci_writeq(&ctrl->ir_set->erst_dequeue,
				(u64)deq & (u64)~ERST_PTR_MASK);

	/* set ERST count with the number of entries in the segment table */
	val = xhci_readl(&ctrl->ir_set->erst_size);
	val &= ERST_SIZE_MASK;
	val |= ERST_NUM_SEGS;
	xhci_writel(&ctrl->ir_set->erst_size, val);

	/* this is the event ring segment table pointer */
	val_64 = xhci_readq(&ctrl->ir_set->erst_base);
	val_64 &= ERST_PTR_MASK;
	val_64 |= ctrl->erst.erst_dma_addr & ~ERST_PTR_MASK;

	xhci_writeq(&ctrl->ir_set->erst_base, val_64);

	/* set up the scratchpad buffer array and scratchpad buffers */
	xhci_scratchpad_alloc(ctrl);

	ctrl->bounce_buffer = xmemalign(SZ_64K, SZ_64K);

	/* initializing the virtual devices to NULL */
	for (i = 0; i < MAX_HC_SLOTS; ++i)
		ctrl->devs[i] = NULL;

	/*
	 * Just Zero'ing this register completely,
	 * or some spurious Device Notification Events
	 * might screw things here.
	 */
	xhci_writel(&hcor->or_dnctrl, 0x0);

	return 0;
}

/**
 * Give the input control context for the passed container context
 *
 * @param ctx	pointer to the context
 * @return pointer to the Input control context data
 */
struct xhci_input_control_ctx
		*xhci_get_input_control_ctx(struct xhci_container_ctx *ctx)
{
	BUG_ON(ctx->type != XHCI_CTX_TYPE_INPUT);
	return (struct xhci_input_control_ctx *)ctx->bytes;
}

/**
 * Give the slot context for the passed container context
 *
 * @param ctrl	Host controller data structure
 * @param ctx	pointer to the context
 * @return pointer to the slot control context data
 */
struct xhci_slot_ctx *xhci_get_slot_ctx(struct xhci_ctrl *ctrl,
				struct xhci_container_ctx *ctx)
{
	if (ctx->type == XHCI_CTX_TYPE_DEVICE)
		return (struct xhci_slot_ctx *)ctx->bytes;

	return (struct xhci_slot_ctx *)
		(ctx->bytes + CTX_SIZE(xhci_readl(&ctrl->hccr->cr_hccparams)));
}

/**
 * Gets the EP context from based on the ep_index
 *
 * @param ctrl	Host controller data structure
 * @param ctx	context container
 * @param ep_index	index of the endpoint
 * @return pointer to the End point context
 */
struct xhci_ep_ctx *xhci_get_ep_ctx(struct xhci_ctrl *ctrl,
				    struct xhci_container_ctx *ctx,
				    unsigned int ep_index)
{
	/* increment ep index by offset of start of ep ctx array */
	ep_index++;
	if (ctx->type == XHCI_CTX_TYPE_INPUT)
		ep_index++;

	return (struct xhci_ep_ctx *)
		(ctx->bytes +
		(ep_index * CTX_SIZE(xhci_readl(&ctrl->hccr->cr_hccparams))));
}

/**
 * Copy output xhci_ep_ctx to the input xhci_ep_ctx copy.
 * Useful when you want to change one particular aspect of the endpoint
 * and then issue a configure endpoint command.
 *
 * @param ctrl	Host controller data structure
 * @param in_ctx contains the input context
 * @param out_ctx contains the input context
 * @param ep_index index of the end point
 * @return none
 */
void xhci_endpoint_copy(struct xhci_ctrl *ctrl,
			struct xhci_container_ctx *in_ctx,
			struct xhci_container_ctx *out_ctx,
			unsigned int ep_index)
{
	struct xhci_ep_ctx *out_ep_ctx;
	struct xhci_ep_ctx *in_ep_ctx;

	out_ep_ctx = xhci_get_ep_ctx(ctrl, out_ctx, ep_index);
	in_ep_ctx = xhci_get_ep_ctx(ctrl, in_ctx, ep_index);

	in_ep_ctx->ep_info = out_ep_ctx->ep_info;
	in_ep_ctx->ep_info2 = out_ep_ctx->ep_info2;
	in_ep_ctx->deq = out_ep_ctx->deq;
	in_ep_ctx->tx_info = out_ep_ctx->tx_info;
}

/**
 * Copy output xhci_slot_ctx to the input xhci_slot_ctx.
 * Useful when you want to change one particular aspect of the endpoint
 * and then issue a configure endpoint command.
 * Only the context entries field matters, but
 * we'll copy the whole thing anyway.
 *
 * @param ctrl	Host controller data structure
 * @param in_ctx contains the inpout context
 * @param out_ctx contains the inpout context
 * @return none
 */
void xhci_slot_copy(struct xhci_ctrl *ctrl, struct xhci_container_ctx *in_ctx,
					struct xhci_container_ctx *out_ctx)
{
	struct xhci_slot_ctx *in_slot_ctx;
	struct xhci_slot_ctx *out_slot_ctx;

	in_slot_ctx = xhci_get_slot_ctx(ctrl, in_ctx);
	out_slot_ctx = xhci_get_slot_ctx(ctrl, out_ctx);

	in_slot_ctx->dev_info = out_slot_ctx->dev_info;
	in_slot_ctx->dev_info2 = out_slot_ctx->dev_info2;
	in_slot_ctx->tt_info = out_slot_ctx->tt_info;
	in_slot_ctx->dev_state = out_slot_ctx->dev_state;
}

/**
 * Setup an xHCI virtual device for a Set Address command
 *
 * @param udev pointer to the Device Data Structure
 * @return returns negative value on failure else 0 on success
 */
void xhci_setup_addressable_virt_dev(struct xhci_ctrl *ctrl,
				     struct usb_device *udev, int hop_portnr)
{
	struct xhci_virt_device *virt_dev;
	struct xhci_ep_ctx *ep0_ctx;
	struct xhci_slot_ctx *slot_ctx;
	u32 port_num = 0;
	u64 trb_64 = 0;
	int slot_id = udev->slot_id;
	int speed = udev->speed;
	int route = 0;
	struct usb_device *dev = udev;
	struct usb_hub_device *hub;

	virt_dev = ctrl->devs[slot_id];

	BUG_ON(!virt_dev);

	/* Extract the EP0 and Slot Ctrl */
	ep0_ctx = xhci_get_ep_ctx(ctrl, virt_dev->in_ctx, 0);
	slot_ctx = xhci_get_slot_ctx(ctrl, virt_dev->in_ctx);

	/* Only the control endpoint is valid - one endpoint context */
	slot_ctx->dev_info |= cpu_to_le32(LAST_CTX(1));

	/* Calculate the route string for this device */
	port_num = dev->portnr;
	while (!usb_hub_is_root_hub(dev)) {
		/*
		 * Each hub in the topology is expected to have no more than
		 * 15 ports in order for the route string of a device to be
		 * unique. SuperSpeed hubs are restricted to only having 15
		 * ports, but FS/LS/HS hubs are not. The xHCI specification
		 * says that if the port number the device is greater than 15,
		 * that portion of the route string shall be set to 15.
		 */
		if (port_num > 15)
			port_num = 15;
		route |= port_num << (dev->level * 4);
		dev = dev->parent;
		port_num = dev->portnr;
	}

	dev_dbg(&udev->dev, "route string 0x%x\n", route);

	slot_ctx->dev_info |= cpu_to_le32(route);

	switch (speed) {
	case USB_SPEED_SUPER:
		slot_ctx->dev_info |= cpu_to_le32(SLOT_SPEED_SS);
		break;
	case USB_SPEED_HIGH:
		slot_ctx->dev_info |= cpu_to_le32(SLOT_SPEED_HS);
		break;
	case USB_SPEED_FULL:
		slot_ctx->dev_info |= cpu_to_le32(SLOT_SPEED_FS);
		break;
	case USB_SPEED_LOW:
		slot_ctx->dev_info |= cpu_to_le32(SLOT_SPEED_LS);
		break;
	default:
		/* Speed was set earlier, this shouldn't happen. */
		BUG();
	}

	/* Set up TT fields to support FS/LS devices */
	if (speed == USB_SPEED_LOW || speed == USB_SPEED_FULL) {
		dev = udev;
		do {
			port_num = dev->portnr;
			if (usb_hub_is_root_hub(dev))
				break;
			dev = dev->parent;
		} while (dev->speed != USB_SPEED_HIGH);

		if (!usb_hub_is_root_hub(dev)) {
			hub = dev->hub;
			if (hub->tt.multi)
				slot_ctx->dev_info |= cpu_to_le32(DEV_MTT);
			slot_ctx->tt_info |= cpu_to_le32(TT_PORT(port_num));
			slot_ctx->tt_info |= cpu_to_le32(TT_SLOT(dev->slot_id));
		}
	}

	port_num = hop_portnr;
	dev_dbg(&udev->dev, "port_num = %d\n", port_num);

	slot_ctx->dev_info2 |=
			cpu_to_le32(((port_num & ROOT_HUB_PORT_MASK) <<
				ROOT_HUB_PORT_SHIFT));

	/* Step 4 - ring already allocated */
	/* Step 5 */
	ep0_ctx->ep_info2 = cpu_to_le32(EP_TYPE(CTRL_EP));
	dev_dbg(&udev->dev, "SPEED = %d\n", speed);

	switch (speed) {
	case USB_SPEED_SUPER:
		ep0_ctx->ep_info2 |= cpu_to_le32(MAX_PACKET(512));
		dev_dbg(&udev->dev, "Setting Packet size = 512bytes\n");
		break;
	case USB_SPEED_HIGH:
	/* USB core guesses at a 64-byte max packet first for FS devices */
	case USB_SPEED_FULL:
		ep0_ctx->ep_info2 |= cpu_to_le32(MAX_PACKET(64));
		dev_dbg(&udev->dev, "Setting Packet size = 64bytes\n");
		break;
	case USB_SPEED_LOW:
		ep0_ctx->ep_info2 |= cpu_to_le32(MAX_PACKET(8));
		dev_dbg(&udev->dev, "Setting Packet size = 8bytes\n");
		break;
	default:
		/* New speed? */
		BUG();
	}

	/* EP 0 can handle "burst" sizes of 1, so Max Burst Size field is 0 */
	ep0_ctx->ep_info2 |= cpu_to_le32(MAX_BURST(0) | ERROR_COUNT(3));

	trb_64 = virt_dev->eps[0].ring->first_seg->dma;
	ep0_ctx->deq = cpu_to_le64(trb_64 | virt_dev->eps[0].ring->cycle_state);

	/*
	 * xHCI spec 6.2.3:
	 * software shall set 'Average TRB Length' to 8 for control endpoints.
	 */
	ep0_ctx->tx_info = cpu_to_le32(EP_AVG_TRB_LENGTH(8));

	/* Steps 7 and 8 were done in xhci_alloc_virt_device() */

	xhci_flush_cache((uintptr_t)ep0_ctx, sizeof(struct xhci_ep_ctx));
	xhci_flush_cache((uintptr_t)slot_ctx, sizeof(struct xhci_slot_ctx));
}
