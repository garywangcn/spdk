/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * NVMe over PCIe transport
 */

#include "nvme_internal.h"

#define NVME_ADMIN_ENTRIES	(128)
#define NVME_ADMIN_TRACKERS	(16)

/*
 * NVME_IO_ENTRIES defines the size of an I/O qpair's submission and completion
 *  queues, while NVME_IO_TRACKERS defines the maximum number of I/O that we
 *  will allow outstanding on an I/O qpair at any time.  The only advantage in
 *  having IO_ENTRIES > IO_TRACKERS is for debugging purposes - when dumping
 *  the contents of the submission and completion queues, it will show a longer
 *  history of data.
 */
#define NVME_IO_ENTRIES		(256)
#define NVME_IO_TRACKERS	(128)

/*
 * NVME_MAX_SGL_DESCRIPTORS defines the maximum number of descriptors in one SGL
 *  segment.
 */
#define NVME_MAX_SGL_DESCRIPTORS	(253)

#define NVME_MAX_PRP_LIST_ENTRIES	(506)

/*
 * For commands requiring more than 2 PRP entries, one PRP will be
 *  embedded in the command (prp1), and the rest of the PRP entries
 *  will be in a list pointed to by the command (prp2).  This means
 *  that real max number of PRP entries we support is 506+1, which
 *  results in a max xfer size of 506*PAGE_SIZE.
 */
#define NVME_MAX_XFER_SIZE	NVME_MAX_PRP_LIST_ENTRIES * PAGE_SIZE


/* PCIe transport extensions for spdk_nvme_ctrlr */
struct nvme_pcie_ctrlr {
	struct spdk_nvme_ctrlr ctrlr;

	/** NVMe MMIO register space */
	volatile struct spdk_nvme_registers *regs;

	/* BAR mapping address which contains controller memory buffer */
	void *cmb_bar_virt_addr;

	/* BAR physical address which contains controller memory buffer */
	uint64_t cmb_bar_phys_addr;

	/* Controller memory buffer size in Bytes */
	uint64_t cmb_size;

	/* Current offset of controller memory buffer */
	uint64_t cmb_current_offset;

	/** stride in uint32_t units between doorbell registers (1 = 4 bytes, 2 = 8 bytes, ...) */
	uint32_t doorbell_stride_u32;
};

struct nvme_tracker {
	LIST_ENTRY(nvme_tracker)	list;

	struct nvme_request		*req;
	uint16_t			cid;

	uint16_t			rsvd1: 15;
	uint16_t			active: 1;

	uint32_t			rsvd2;

	uint64_t			prp_sgl_bus_addr;

	union {
		uint64_t			prp[NVME_MAX_PRP_LIST_ENTRIES];
		struct spdk_nvme_sgl_descriptor	sgl[NVME_MAX_SGL_DESCRIPTORS];
	} u;

	uint64_t			rsvd3;
};
/*
 * struct nvme_tracker must be exactly 4K so that the prp[] array does not cross a page boundary
 * and so that there is no padding required to meet alignment requirements.
 */
SPDK_STATIC_ASSERT(sizeof(struct nvme_tracker) == 4096, "nvme_tracker is not 4K");
SPDK_STATIC_ASSERT((offsetof(struct nvme_tracker, u.sgl) & 7) == 0, "SGL must be Qword aligned");

/* PCIe transport extensions for spdk_nvme_qpair */
struct nvme_pcie_qpair {
	/* Submission queue tail doorbell */
	volatile uint32_t *sq_tdbl;

	/* Completion queue head doorbell */
	volatile uint32_t *cq_hdbl;

	/* Submission queue */
	struct spdk_nvme_cmd *cmd;

	/* Completion queue */
	struct spdk_nvme_cpl *cpl;

	LIST_HEAD(, nvme_tracker) free_tr;
	LIST_HEAD(, nvme_tracker) outstanding_tr;

	/* Array of trackers indexed by command ID. */
	struct nvme_tracker *tr;

	uint16_t sq_tail;
	uint16_t cq_head;

	uint8_t phase;

	bool is_enabled;

	/*
	 * Base qpair structure.
	 * This is located after the hot data in this structure so that the important parts of
	 * nvme_pcie_qpair are in the same cache line.
	 */
	struct spdk_nvme_qpair qpair;

	/*
	 * Fields below this point should not be touched on the normal I/O path.
	 */

	bool sq_in_cmb;

	uint64_t cmd_bus_addr;
	uint64_t cpl_bus_addr;
};

static inline struct nvme_pcie_ctrlr *
nvme_pcie_ctrlr(struct spdk_nvme_ctrlr *ctrlr)
{
	assert(ctrlr->transport == SPDK_NVME_TRANSPORT_PCIE);
	return (struct nvme_pcie_ctrlr *)((uintptr_t)ctrlr - offsetof(struct nvme_pcie_ctrlr, ctrlr));
}

static inline struct nvme_pcie_qpair *
nvme_pcie_qpair(struct spdk_nvme_qpair *qpair)
{
	assert(qpair->transport == SPDK_NVME_TRANSPORT_PCIE);
	return (struct nvme_pcie_qpair *)((uintptr_t)qpair - offsetof(struct nvme_pcie_qpair, qpair));
}

int
nvme_pcie_ctrlr_get_pci_id(struct spdk_nvme_ctrlr *ctrlr, struct spdk_pci_id *pci_id)
{
	assert(ctrlr != NULL);
	assert(pci_id != NULL);

	*pci_id = ctrlr->probe_info.pci_id;

	return 0;
}

static volatile void *
nvme_pcie_reg_addr(struct spdk_nvme_ctrlr *ctrlr, uint32_t offset)
{
	struct nvme_pcie_ctrlr *pctrlr = nvme_pcie_ctrlr(ctrlr);

	return (volatile void *)((uintptr_t)pctrlr->regs + offset);
}

int
nvme_pcie_ctrlr_set_reg_4(struct spdk_nvme_ctrlr *ctrlr, uint32_t offset, uint32_t value)
{
	assert(offset <= sizeof(struct spdk_nvme_registers) - 4);
	spdk_mmio_write_4(nvme_pcie_reg_addr(ctrlr, offset), value);
	return 0;
}

int
nvme_pcie_ctrlr_set_reg_8(struct spdk_nvme_ctrlr *ctrlr, uint32_t offset, uint64_t value)
{
	assert(offset <= sizeof(struct spdk_nvme_registers) - 8);
	spdk_mmio_write_8(nvme_pcie_reg_addr(ctrlr, offset), value);
	return 0;
}

int
nvme_pcie_ctrlr_get_reg_4(struct spdk_nvme_ctrlr *ctrlr, uint32_t offset, uint32_t *value)
{
	assert(offset <= sizeof(struct spdk_nvme_registers) - 4);
	assert(value != NULL);
	*value = spdk_mmio_read_4(nvme_pcie_reg_addr(ctrlr, offset));
	return 0;
}

int
nvme_pcie_ctrlr_get_reg_8(struct spdk_nvme_ctrlr *ctrlr, uint32_t offset, uint64_t *value)
{
	assert(offset <= sizeof(struct spdk_nvme_registers) - 8);
	assert(value != NULL);
	*value = spdk_mmio_read_8(nvme_pcie_reg_addr(ctrlr, offset));
	return 0;
}

static int
nvme_pcie_ctrlr_set_asq(struct nvme_pcie_ctrlr *pctrlr, uint64_t value)
{
	return nvme_pcie_ctrlr_set_reg_8(&pctrlr->ctrlr, offsetof(struct spdk_nvme_registers, asq),
					 value);
}

static int
nvme_pcie_ctrlr_set_acq(struct nvme_pcie_ctrlr *pctrlr, uint64_t value)
{
	return nvme_pcie_ctrlr_set_reg_8(&pctrlr->ctrlr, offsetof(struct spdk_nvme_registers, acq),
					 value);
}

static int
nvme_pcie_ctrlr_set_aqa(struct nvme_pcie_ctrlr *pctrlr, const union spdk_nvme_aqa_register *aqa)
{
	return nvme_pcie_ctrlr_set_reg_4(&pctrlr->ctrlr, offsetof(struct spdk_nvme_registers, aqa.raw),
					 aqa->raw);
}

static int
nvme_pcie_ctrlr_get_cmbloc(struct nvme_pcie_ctrlr *pctrlr, union spdk_nvme_cmbloc_register *cmbloc)
{
	return nvme_pcie_ctrlr_get_reg_4(&pctrlr->ctrlr, offsetof(struct spdk_nvme_registers, cmbloc.raw),
					 &cmbloc->raw);
}

static int
nvme_pcie_ctrlr_get_cmbsz(struct nvme_pcie_ctrlr *pctrlr, union spdk_nvme_cmbsz_register *cmbsz)
{
	return nvme_pcie_ctrlr_get_reg_4(&pctrlr->ctrlr, offsetof(struct spdk_nvme_registers, cmbsz.raw),
					 &cmbsz->raw);
}

uint32_t
nvme_pcie_ctrlr_get_max_xfer_size(struct spdk_nvme_ctrlr *ctrlr)
{
	return NVME_MAX_XFER_SIZE;
}

static void
nvme_pcie_ctrlr_map_cmb(struct nvme_pcie_ctrlr *pctrlr)
{
	int rc;
	void *addr;
	uint32_t bir;
	union spdk_nvme_cmbsz_register cmbsz;
	union spdk_nvme_cmbloc_register cmbloc;
	uint64_t size, unit_size, offset, bar_size, bar_phys_addr;

	if (nvme_pcie_ctrlr_get_cmbsz(pctrlr, &cmbsz) ||
	    nvme_pcie_ctrlr_get_cmbloc(pctrlr, &cmbloc)) {
		SPDK_ERRLOG("get registers failed\n");
		goto exit;
	}

	if (!cmbsz.bits.sz)
		goto exit;

	bir = cmbloc.bits.bir;
	/* Values 0 2 3 4 5 are valid for BAR */
	if (bir > 5 || bir == 1)
		goto exit;

	/* unit size for 4KB/64KB/1MB/16MB/256MB/4GB/64GB */
	unit_size = (uint64_t)1 << (12 + 4 * cmbsz.bits.szu);
	/* controller memory buffer size in Bytes */
	size = unit_size * cmbsz.bits.sz;
	/* controller memory buffer offset from BAR in Bytes */
	offset = unit_size * cmbloc.bits.ofst;

	rc = spdk_pci_device_map_bar(pctrlr->ctrlr.devhandle, bir, &addr,
				     &bar_phys_addr, &bar_size);
	if ((rc != 0) || addr == NULL) {
		goto exit;
	}

	if (offset > bar_size) {
		goto exit;
	}

	if (size > bar_size - offset) {
		goto exit;
	}

	pctrlr->cmb_bar_virt_addr = addr;
	pctrlr->cmb_bar_phys_addr = bar_phys_addr;
	pctrlr->cmb_size = size;
	pctrlr->cmb_current_offset = offset;

	if (!cmbsz.bits.sqs) {
		pctrlr->ctrlr.opts.use_cmb_sqs = false;
	}

	return;
exit:
	pctrlr->cmb_bar_virt_addr = NULL;
	pctrlr->ctrlr.opts.use_cmb_sqs = false;
	return;
}

static int
nvme_pcie_ctrlr_unmap_cmb(struct nvme_pcie_ctrlr *pctrlr)
{
	int rc = 0;
	union spdk_nvme_cmbloc_register cmbloc;
	void *addr = pctrlr->cmb_bar_virt_addr;

	if (addr) {
		if (nvme_pcie_ctrlr_get_cmbloc(pctrlr, &cmbloc)) {
			SPDK_ERRLOG("get_cmbloc() failed\n");
			return -EIO;
		}
		rc = spdk_pci_device_unmap_bar(pctrlr->ctrlr.devhandle, cmbloc.bits.bir, addr);
	}
	return rc;
}

static int
nvme_pcie_ctrlr_alloc_cmb(struct spdk_nvme_ctrlr *ctrlr, uint64_t length, uint64_t aligned,
			  uint64_t *offset)
{
	struct nvme_pcie_ctrlr *pctrlr = nvme_pcie_ctrlr(ctrlr);
	uint64_t round_offset;

	round_offset = pctrlr->cmb_current_offset;
	round_offset = (round_offset + (aligned - 1)) & ~(aligned - 1);

	if (round_offset + length > pctrlr->cmb_size)
		return -1;

	*offset = round_offset;
	pctrlr->cmb_current_offset = round_offset + length;

	return 0;
}

static int
nvme_pcie_ctrlr_allocate_bars(struct nvme_pcie_ctrlr *pctrlr)
{
	int rc;
	void *addr;
	uint64_t phys_addr, size;

	rc = spdk_pci_device_map_bar(pctrlr->ctrlr.devhandle, 0, &addr,
				     &phys_addr, &size);
	pctrlr->regs = (volatile struct spdk_nvme_registers *)addr;
	if ((pctrlr->regs == NULL) || (rc != 0)) {
		SPDK_ERRLOG("nvme_pcicfg_map_bar failed with rc %d or bar %p\n",
			    rc, pctrlr->regs);
		return -1;
	}

	nvme_pcie_ctrlr_map_cmb(pctrlr);

	return 0;
}

static int
nvme_pcie_ctrlr_free_bars(struct nvme_pcie_ctrlr *pctrlr)
{
	int rc = 0;
	void *addr = (void *)pctrlr->regs;

	rc = nvme_pcie_ctrlr_unmap_cmb(pctrlr);
	if (rc != 0) {
		SPDK_ERRLOG("nvme_ctrlr_unmap_cmb failed with error code %d\n", rc);
		return -1;
	}

	if (addr) {
		rc = spdk_pci_device_unmap_bar(pctrlr->ctrlr.devhandle, 0, addr);
	}
	return rc;
}

static int
nvme_pcie_ctrlr_construct_admin_qpair(struct spdk_nvme_ctrlr *ctrlr)
{
	struct nvme_pcie_qpair *pqpair;

	pqpair = spdk_zmalloc(sizeof(*pqpair), 64, NULL);
	if (pqpair == NULL) {
		return -ENOMEM;
	}

	ctrlr->adminq = &pqpair->qpair;

	return nvme_qpair_construct(ctrlr->adminq,
				    0, /* qpair ID */
				    NVME_ADMIN_ENTRIES,
				    ctrlr,
				    SPDK_NVME_QPRIO_URGENT);
}

struct spdk_nvme_ctrlr *nvme_pcie_ctrlr_construct(enum spdk_nvme_transport transport,
		void *devhandle)
{
	struct spdk_pci_device *pci_dev = devhandle;
	struct nvme_pcie_ctrlr *pctrlr;
	union spdk_nvme_cap_register cap;
	uint32_t cmd_reg;
	int rc;

	pctrlr = spdk_zmalloc(sizeof(struct nvme_pcie_ctrlr), 64, NULL);
	if (pctrlr == NULL) {
		SPDK_ERRLOG("could not allocate ctrlr\n");
		return NULL;
	}

	pctrlr->ctrlr.transport = SPDK_NVME_TRANSPORT_PCIE;
	pctrlr->ctrlr.devhandle = devhandle;

	rc = nvme_pcie_ctrlr_allocate_bars(pctrlr);
	if (rc != 0) {
		spdk_free(pctrlr);
		return NULL;
	}

	/* Enable PCI busmaster and disable INTx */
	spdk_pci_device_cfg_read32(pci_dev, &cmd_reg, 4);
	cmd_reg |= 0x404;
	spdk_pci_device_cfg_write32(pci_dev, cmd_reg, 4);

	if (nvme_ctrlr_get_cap(&pctrlr->ctrlr, &cap)) {
		SPDK_ERRLOG("get_cap() failed\n");
		spdk_free(pctrlr);
		return NULL;
	}

	pctrlr->ctrlr.cap = cap;

	/* Doorbell stride is 2 ^ (dstrd + 2),
	 * but we want multiples of 4, so drop the + 2 */
	pctrlr->doorbell_stride_u32 = 1 << cap.bits.dstrd;

	rc = nvme_ctrlr_construct(&pctrlr->ctrlr);
	if (rc != 0) {
		nvme_ctrlr_destruct(&pctrlr->ctrlr);
		return NULL;
	}

	rc = nvme_pcie_ctrlr_construct_admin_qpair(&pctrlr->ctrlr);
	if (rc != 0) {
		nvme_ctrlr_destruct(&pctrlr->ctrlr);
		return NULL;
	}

	/* Construct the primary process properties */
	rc = nvme_ctrlr_add_process(&pctrlr->ctrlr, pci_dev);
	if (rc != 0) {
		nvme_ctrlr_destruct(&pctrlr->ctrlr);
		return NULL;
	}

	return &pctrlr->ctrlr;
}

int
nvme_pcie_ctrlr_enable(struct spdk_nvme_ctrlr *ctrlr)
{
	struct nvme_pcie_ctrlr *pctrlr = nvme_pcie_ctrlr(ctrlr);
	struct nvme_pcie_qpair *padminq = nvme_pcie_qpair(ctrlr->adminq);
	union spdk_nvme_aqa_register aqa;

	if (nvme_pcie_ctrlr_set_asq(pctrlr, padminq->cmd_bus_addr)) {
		SPDK_ERRLOG("set_asq() failed\n");
		return -EIO;
	}

	if (nvme_pcie_ctrlr_set_acq(pctrlr, padminq->cpl_bus_addr)) {
		SPDK_ERRLOG("set_acq() failed\n");
		return -EIO;
	}

	aqa.raw = 0;
	/* acqs and asqs are 0-based. */
	aqa.bits.acqs = ctrlr->adminq->num_entries - 1;
	aqa.bits.asqs = ctrlr->adminq->num_entries - 1;

	if (nvme_pcie_ctrlr_set_aqa(pctrlr, &aqa)) {
		SPDK_ERRLOG("set_aqa() failed\n");
		return -EIO;
	}

	return 0;
}

int
nvme_pcie_ctrlr_destruct(struct spdk_nvme_ctrlr *ctrlr)
{
	struct nvme_pcie_ctrlr *pctrlr = nvme_pcie_ctrlr(ctrlr);
	struct nvme_pcie_qpair *pqpair;

	if (ctrlr->adminq) {
		pqpair = nvme_pcie_qpair(ctrlr->adminq);

		nvme_qpair_destroy(ctrlr->adminq);
		spdk_free(pqpair);
	}

	nvme_pcie_ctrlr_free_bars(pctrlr);
	spdk_free(pctrlr);

	return 0;
}

static void
nvme_qpair_construct_tracker(struct nvme_tracker *tr, uint16_t cid, uint64_t phys_addr)
{
	tr->prp_sgl_bus_addr = phys_addr + offsetof(struct nvme_tracker, u.prp);
	tr->cid = cid;
	tr->active = false;
}

int
nvme_pcie_qpair_reset(struct spdk_nvme_qpair *qpair)
{
	struct nvme_pcie_qpair *pqpair = nvme_pcie_qpair(qpair);

	pqpair->sq_tail = pqpair->cq_head = 0;

	/*
	 * First time through the completion queue, HW will set phase
	 *  bit on completions to 1.  So set this to 1 here, indicating
	 *  we're looking for a 1 to know which entries have completed.
	 *  we'll toggle the bit each time when the completion queue
	 *  rolls over.
	 */
	pqpair->phase = 1;

	memset(pqpair->cmd, 0,
	       qpair->num_entries * sizeof(struct spdk_nvme_cmd));
	memset(pqpair->cpl, 0,
	       qpair->num_entries * sizeof(struct spdk_nvme_cpl));

	return 0;
}

int
nvme_pcie_qpair_construct(struct spdk_nvme_qpair *qpair)
{
	struct spdk_nvme_ctrlr	*ctrlr = qpair->ctrlr;
	struct nvme_pcie_ctrlr	*pctrlr = nvme_pcie_ctrlr(ctrlr);
	struct nvme_pcie_qpair	*pqpair = nvme_pcie_qpair(qpair);
	struct nvme_tracker	*tr;
	uint16_t		i;
	volatile uint32_t	*doorbell_base;
	uint64_t		phys_addr = 0;
	uint64_t		offset;
	uint16_t		num_trackers;

	if (qpair->id == 0) {
		num_trackers = NVME_ADMIN_TRACKERS;
	} else {
		/*
		 * No need to have more trackers than entries in the submit queue.
		 *  Note also that for a queue size of N, we can only have (N-1)
		 *  commands outstanding, hence the "-1" here.
		 */
		num_trackers = nvme_min(NVME_IO_TRACKERS, qpair->num_entries - 1);
	}

	assert(num_trackers != 0);

	pqpair->sq_in_cmb = false;

	/* cmd and cpl rings must be aligned on 4KB boundaries. */
	if (ctrlr->opts.use_cmb_sqs) {
		if (nvme_pcie_ctrlr_alloc_cmb(ctrlr, qpair->num_entries * sizeof(struct spdk_nvme_cmd),
					      0x1000, &offset) == 0) {
			pqpair->cmd = pctrlr->cmb_bar_virt_addr + offset;
			pqpair->cmd_bus_addr = pctrlr->cmb_bar_phys_addr + offset;
			pqpair->sq_in_cmb = true;
		}
	}
	if (pqpair->sq_in_cmb == false) {
		pqpair->cmd = spdk_zmalloc(qpair->num_entries * sizeof(struct spdk_nvme_cmd),
					   0x1000,
					   &pqpair->cmd_bus_addr);
		if (pqpair->cmd == NULL) {
			SPDK_ERRLOG("alloc qpair_cmd failed\n");
			return -ENOMEM;
		}
	}

	pqpair->cpl = spdk_zmalloc(qpair->num_entries * sizeof(struct spdk_nvme_cpl),
				   0x1000,
				   &pqpair->cpl_bus_addr);
	if (pqpair->cpl == NULL) {
		SPDK_ERRLOG("alloc qpair_cpl failed\n");
		return -ENOMEM;
	}

	doorbell_base = &pctrlr->regs->doorbell[0].sq_tdbl;
	pqpair->sq_tdbl = doorbell_base + (2 * qpair->id + 0) * pctrlr->doorbell_stride_u32;
	pqpair->cq_hdbl = doorbell_base + (2 * qpair->id + 1) * pctrlr->doorbell_stride_u32;

	/*
	 * Reserve space for all of the trackers in a single allocation.
	 *   struct nvme_tracker must be padded so that its size is already a power of 2.
	 *   This ensures the PRP list embedded in the nvme_tracker object will not span a
	 *   4KB boundary, while allowing access to trackers in tr[] via normal array indexing.
	 */
	pqpair->tr = spdk_zmalloc(num_trackers * sizeof(*tr), sizeof(*tr), &phys_addr);
	if (pqpair->tr == NULL) {
		SPDK_ERRLOG("nvme_tr failed\n");
		return -ENOMEM;
	}

	LIST_INIT(&pqpair->free_tr);
	LIST_INIT(&pqpair->outstanding_tr);

	for (i = 0; i < num_trackers; i++) {
		tr = &pqpair->tr[i];
		nvme_qpair_construct_tracker(tr, i, phys_addr);
		LIST_INSERT_HEAD(&pqpair->free_tr, tr, list);
		phys_addr += sizeof(struct nvme_tracker);
	}

	nvme_pcie_qpair_reset(qpair);

	return 0;
}

static inline void
nvme_pcie_copy_command(struct spdk_nvme_cmd *dst, const struct spdk_nvme_cmd *src)
{
	/* dst and src are known to be non-overlapping and 64-byte aligned. */
#if defined(__AVX__)
	__m256i *d256 = (__m256i *)dst;
	const __m256i *s256 = (const __m256i *)src;

	_mm256_store_si256(&d256[0], _mm256_load_si256(&s256[0]));
	_mm256_store_si256(&d256[1], _mm256_load_si256(&s256[1]));
#elif defined(__SSE2__)
	__m128i *d128 = (__m128i *)dst;
	const __m128i *s128 = (const __m128i *)src;

	_mm_store_si128(&d128[0], _mm_load_si128(&s128[0]));
	_mm_store_si128(&d128[1], _mm_load_si128(&s128[1]));
	_mm_store_si128(&d128[2], _mm_load_si128(&s128[2]));
	_mm_store_si128(&d128[3], _mm_load_si128(&s128[3]));
#else
	*dst = *src;
#endif
}

static void
nvme_pcie_qpair_insert_pending_admin_request(struct spdk_nvme_qpair *qpair,
		struct nvme_request *req, struct spdk_nvme_cpl *cpl)
{
	struct spdk_nvme_ctrlr			*ctrlr = qpair->ctrlr;
	struct nvme_request			*active_req = req;
	struct spdk_nvme_controller_process	*active_proc, *tmp;
	bool					pending_on_proc = false;

	/*
	 * The admin request is from another process. Move to the per
	 *  process list for that process to handle it later.
	 */
	assert(nvme_qpair_is_admin_queue(qpair));
	assert(active_req->pid != getpid());

	/* Acquire the recursive lock first if not held already. */
	pthread_mutex_lock(&ctrlr->ctrlr_lock);

	TAILQ_FOREACH_SAFE(active_proc, &ctrlr->active_procs, tailq, tmp) {
		if (active_proc->pid == active_req->pid) {
			/* Saved the original completion information */
			memcpy(&active_req->cpl, cpl, sizeof(*cpl));
			STAILQ_INSERT_TAIL(&active_proc->active_reqs, active_req, stailq);
			pending_on_proc = true;

			break;
		}
	}

	pthread_mutex_unlock(&ctrlr->ctrlr_lock);

	if (pending_on_proc == false) {
		SPDK_ERRLOG("The owning process is not found. Drop the request.\n");

		nvme_free_request(active_req);
	}
}

static void
nvme_pcie_qpair_complete_pending_admin_request(struct spdk_nvme_qpair *qpair)
{
	struct spdk_nvme_ctrlr		*ctrlr = qpair->ctrlr;
	struct nvme_request		*req, *tmp_req;
	bool				proc_found = false;
	pid_t				pid = getpid();
	struct spdk_nvme_controller_process	*proc;

	/*
	 * Check whether there is any pending admin request from
	 * other active processes.
	 */
	assert(nvme_qpair_is_admin_queue(qpair));

	/* Acquire the recursive lock if not held already */
	pthread_mutex_lock(&ctrlr->ctrlr_lock);

	TAILQ_FOREACH(proc, &ctrlr->active_procs, tailq) {
		if (proc->pid == pid) {
			proc_found = true;

			break;
		}
	}

	pthread_mutex_unlock(&ctrlr->ctrlr_lock);

	if (proc_found == false) {
		SPDK_ERRLOG("the active process is not found for this controller.");
		assert(proc_found);
	}

	STAILQ_FOREACH_SAFE(req, &proc->active_reqs, stailq, tmp_req) {
		STAILQ_REMOVE(&proc->active_reqs, req, nvme_request, stailq);

		assert(req->pid == pid);

		if (req->cb_fn) {
			req->cb_fn(req->cb_arg, &req->cpl);
		}

		nvme_free_request(req);
	}
}

static void
nvme_pcie_qpair_submit_tracker(struct spdk_nvme_qpair *qpair, struct nvme_tracker *tr)
{
	struct nvme_request	*req;
	struct nvme_pcie_qpair	*pqpair = nvme_pcie_qpair(qpair);

	req = tr->req;
	pqpair->tr[tr->cid].active = true;

	/* Copy the command from the tracker to the submission queue. */
	nvme_pcie_copy_command(&pqpair->cmd[pqpair->sq_tail], &req->cmd);

	if (++pqpair->sq_tail == qpair->num_entries) {
		pqpair->sq_tail = 0;
	}

	spdk_wmb();
	spdk_mmio_write_4(pqpair->sq_tdbl, pqpair->sq_tail);
}

static void
nvme_pcie_qpair_complete_tracker(struct spdk_nvme_qpair *qpair, struct nvme_tracker *tr,
				 struct spdk_nvme_cpl *cpl, bool print_on_error)
{
	struct nvme_pcie_qpair		*pqpair = nvme_pcie_qpair(qpair);
	struct nvme_request		*req;
	bool				retry, error, was_active;
	bool				req_from_current_proc = true;

	req = tr->req;

	assert(req != NULL);

	error = spdk_nvme_cpl_is_error(cpl);
	retry = error && nvme_completion_is_retry(cpl) &&
		req->retries < spdk_nvme_retry_count;

	if (error && print_on_error) {
		nvme_qpair_print_command(qpair, &req->cmd);
		nvme_qpair_print_completion(qpair, cpl);
	}

	was_active = pqpair->tr[cpl->cid].active;
	pqpair->tr[cpl->cid].active = false;

	assert(cpl->cid == req->cmd.cid);

	if (retry) {
		req->retries++;
		nvme_pcie_qpair_submit_tracker(qpair, tr);
	} else {
		if (was_active && req->cb_fn) {
			/* Only check admin requests from different processes. */
			if (nvme_qpair_is_admin_queue(qpair) && req->pid != getpid()) {
				req_from_current_proc = false;
				nvme_pcie_qpair_insert_pending_admin_request(qpair, req, cpl);
			} else {
				req->cb_fn(req->cb_arg, cpl);
			}
		}

		if (req_from_current_proc == true) {
			nvme_free_request(req);
		}

		tr->req = NULL;

		LIST_REMOVE(tr, list);
		LIST_INSERT_HEAD(&pqpair->free_tr, tr, list);

		/*
		 * If the controller is in the middle of resetting, don't
		 *  try to submit queued requests here - let the reset logic
		 *  handle that instead.
		 */
		if (!STAILQ_EMPTY(&qpair->queued_req) &&
		    !qpair->ctrlr->is_resetting) {
			req = STAILQ_FIRST(&qpair->queued_req);
			STAILQ_REMOVE_HEAD(&qpair->queued_req, stailq);
			nvme_qpair_submit_request(qpair, req);
		}
	}
}

static void
nvme_pcie_qpair_manual_complete_tracker(struct spdk_nvme_qpair *qpair,
					struct nvme_tracker *tr, uint32_t sct, uint32_t sc, uint32_t dnr,
					bool print_on_error)
{
	struct spdk_nvme_cpl	cpl;

	memset(&cpl, 0, sizeof(cpl));
	cpl.sqid = qpair->id;
	cpl.cid = tr->cid;
	cpl.status.sct = sct;
	cpl.status.sc = sc;
	cpl.status.dnr = dnr;
	nvme_pcie_qpair_complete_tracker(qpair, tr, &cpl, print_on_error);
}

static void
nvme_pcie_qpair_abort_trackers(struct spdk_nvme_qpair *qpair, uint32_t dnr)
{
	struct nvme_pcie_qpair *pqpair = nvme_pcie_qpair(qpair);
	struct nvme_tracker *tr, *temp;

	LIST_FOREACH_SAFE(tr, &pqpair->outstanding_tr, list, temp) {
		SPDK_ERRLOG("aborting outstanding command\n");
		nvme_pcie_qpair_manual_complete_tracker(qpair, tr, SPDK_NVME_SCT_GENERIC,
							SPDK_NVME_SC_ABORTED_BY_REQUEST, dnr, true);
	}
}

static void
nvme_pcie_admin_qpair_abort_aers(struct spdk_nvme_qpair *qpair)
{
	struct nvme_pcie_qpair	*pqpair = nvme_pcie_qpair(qpair);
	struct nvme_tracker	*tr;

	tr = LIST_FIRST(&pqpair->outstanding_tr);
	while (tr != NULL) {
		assert(tr->req != NULL);
		if (tr->req->cmd.opc == SPDK_NVME_OPC_ASYNC_EVENT_REQUEST) {
			nvme_pcie_qpair_manual_complete_tracker(qpair, tr,
								SPDK_NVME_SCT_GENERIC, SPDK_NVME_SC_ABORTED_SQ_DELETION, 0,
								false);
			tr = LIST_FIRST(&pqpair->outstanding_tr);
		} else {
			tr = LIST_NEXT(tr, list);
		}
	}
}

static void
nvme_pcie_admin_qpair_destroy(struct spdk_nvme_qpair *qpair)
{
	nvme_pcie_admin_qpair_abort_aers(qpair);
}

int
nvme_pcie_qpair_destroy(struct spdk_nvme_qpair *qpair)
{
	struct nvme_pcie_qpair *pqpair = nvme_pcie_qpair(qpair);

	if (nvme_qpair_is_admin_queue(qpair)) {
		nvme_pcie_admin_qpair_destroy(qpair);
	}
	if (pqpair->cmd && !pqpair->sq_in_cmb) {
		spdk_free(pqpair->cmd);
		pqpair->cmd = NULL;
	}
	if (pqpair->cpl) {
		spdk_free(pqpair->cpl);
		pqpair->cpl = NULL;
	}
	if (pqpair->tr) {
		spdk_free(pqpair->tr);
		pqpair->tr = NULL;
	}

	return 0;
}

static void
nvme_pcie_admin_qpair_enable(struct spdk_nvme_qpair *qpair)
{
	/*
	 * Manually abort each outstanding admin command.  Do not retry
	 *  admin commands found here, since they will be left over from
	 *  a controller reset and its likely the context in which the
	 *  command was issued no longer applies.
	 */
	nvme_pcie_qpair_abort_trackers(qpair, 1 /* do not retry */);
}

static void
nvme_pcie_io_qpair_enable(struct spdk_nvme_qpair *qpair)
{
	/* Manually abort each outstanding I/O. */
	nvme_pcie_qpair_abort_trackers(qpair, 0);
}

int
nvme_pcie_qpair_enable(struct spdk_nvme_qpair *qpair)
{
	struct nvme_pcie_qpair *pqpair = nvme_pcie_qpair(qpair);

	pqpair->is_enabled = true;
	if (nvme_qpair_is_io_queue(qpair)) {
		nvme_pcie_io_qpair_enable(qpair);
	} else {
		nvme_pcie_admin_qpair_enable(qpair);
	}

	return 0;
}

static void
nvme_pcie_admin_qpair_disable(struct spdk_nvme_qpair *qpair)
{
	nvme_pcie_admin_qpair_abort_aers(qpair);
}

static void
nvme_pcie_io_qpair_disable(struct spdk_nvme_qpair *qpair)
{
}

int
nvme_pcie_qpair_disable(struct spdk_nvme_qpair *qpair)
{
	struct nvme_pcie_qpair *pqpair = nvme_pcie_qpair(qpair);

	pqpair->is_enabled = false;
	if (nvme_qpair_is_io_queue(qpair)) {
		nvme_pcie_io_qpair_disable(qpair);
	} else {
		nvme_pcie_admin_qpair_disable(qpair);
	}

	return 0;
}


int
nvme_pcie_qpair_fail(struct spdk_nvme_qpair *qpair)
{
	nvme_pcie_qpair_abort_trackers(qpair, 1 /* do not retry */);

	return 0;
}

static int
nvme_pcie_ctrlr_cmd_create_io_cq(struct spdk_nvme_ctrlr *ctrlr,
				 struct spdk_nvme_qpair *io_que, spdk_nvme_cmd_cb cb_fn,
				 void *cb_arg)
{
	struct nvme_pcie_qpair *pqpair = nvme_pcie_qpair(io_que);
	struct nvme_request *req;
	struct spdk_nvme_cmd *cmd;

	req = nvme_allocate_request_null(cb_fn, cb_arg);
	if (req == NULL) {
		return -ENOMEM;
	}

	cmd = &req->cmd;
	cmd->opc = SPDK_NVME_OPC_CREATE_IO_CQ;

	/*
	 * TODO: create a create io completion queue command data
	 *  structure.
	 */
	cmd->cdw10 = ((io_que->num_entries - 1) << 16) | io_que->id;
	/*
	 * 0x2 = interrupts enabled
	 * 0x1 = physically contiguous
	 */
	cmd->cdw11 = 0x1;
	cmd->dptr.prp.prp1 = pqpair->cpl_bus_addr;

	return nvme_ctrlr_submit_admin_request(ctrlr, req);
}

static int
nvme_pcie_ctrlr_cmd_create_io_sq(struct spdk_nvme_ctrlr *ctrlr,
				 struct spdk_nvme_qpair *io_que, spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	struct nvme_pcie_qpair *pqpair = nvme_pcie_qpair(io_que);
	struct nvme_request *req;
	struct spdk_nvme_cmd *cmd;

	req = nvme_allocate_request_null(cb_fn, cb_arg);
	if (req == NULL) {
		return -ENOMEM;
	}

	cmd = &req->cmd;
	cmd->opc = SPDK_NVME_OPC_CREATE_IO_SQ;

	/*
	 * TODO: create a create io submission queue command data
	 *  structure.
	 */
	cmd->cdw10 = ((io_que->num_entries - 1) << 16) | io_que->id;
	/* 0x1 = physically contiguous */
	cmd->cdw11 = (io_que->id << 16) | (io_que->qprio << 1) | 0x1;
	cmd->dptr.prp.prp1 = pqpair->cmd_bus_addr;

	return nvme_ctrlr_submit_admin_request(ctrlr, req);
}

static int
nvme_pcie_ctrlr_cmd_delete_io_cq(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair,
				 spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	struct nvme_request *req;
	struct spdk_nvme_cmd *cmd;

	req = nvme_allocate_request_null(cb_fn, cb_arg);
	if (req == NULL) {
		return -ENOMEM;
	}

	cmd = &req->cmd;
	cmd->opc = SPDK_NVME_OPC_DELETE_IO_CQ;
	cmd->cdw10 = qpair->id;

	return nvme_ctrlr_submit_admin_request(ctrlr, req);
}

static int
nvme_pcie_ctrlr_cmd_delete_io_sq(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair,
				 spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	struct nvme_request *req;
	struct spdk_nvme_cmd *cmd;

	req = nvme_allocate_request_null(cb_fn, cb_arg);
	if (req == NULL) {
		return -ENOMEM;
	}

	cmd = &req->cmd;
	cmd->opc = SPDK_NVME_OPC_DELETE_IO_SQ;
	cmd->cdw10 = qpair->id;

	return nvme_ctrlr_submit_admin_request(ctrlr, req);
}

static int
_nvme_pcie_ctrlr_create_io_qpair(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair,
				 uint16_t qid)
{
	struct nvme_completion_poll_status	status;
	int					rc;

	status.done = false;
	rc = nvme_pcie_ctrlr_cmd_create_io_cq(ctrlr, qpair, nvme_completion_poll_cb, &status);
	if (rc != 0) {
		return rc;
	}

	while (status.done == false) {
		spdk_nvme_qpair_process_completions(ctrlr->adminq, 0);
	}
	if (spdk_nvme_cpl_is_error(&status.cpl)) {
		SPDK_ERRLOG("nvme_create_io_cq failed!\n");
		return -1;
	}

	status.done = false;
	rc = nvme_pcie_ctrlr_cmd_create_io_sq(qpair->ctrlr, qpair, nvme_completion_poll_cb, &status);
	if (rc != 0) {
		return rc;
	}

	while (status.done == false) {
		spdk_nvme_qpair_process_completions(ctrlr->adminq, 0);
	}
	if (spdk_nvme_cpl_is_error(&status.cpl)) {
		SPDK_ERRLOG("nvme_create_io_sq failed!\n");
		/* Attempt to delete the completion queue */
		status.done = false;
		rc = nvme_pcie_ctrlr_cmd_delete_io_cq(qpair->ctrlr, qpair, nvme_completion_poll_cb, &status);
		if (rc != 0) {
			return -1;
		}
		while (status.done == false) {
			spdk_nvme_qpair_process_completions(ctrlr->adminq, 0);
		}
		return -1;
	}

	nvme_pcie_qpair_reset(qpair);

	return 0;
}

struct spdk_nvme_qpair *
nvme_pcie_ctrlr_create_io_qpair(struct spdk_nvme_ctrlr *ctrlr, uint16_t qid,
				enum spdk_nvme_qprio qprio)
{
	struct nvme_pcie_qpair *pqpair;
	struct spdk_nvme_qpair *qpair;
	uint32_t num_entries;
	int rc;

	assert(ctrlr != NULL);

	pqpair = calloc(1, sizeof(*pqpair));
	if (pqpair == NULL) {
		return NULL;
	}

	qpair = &pqpair->qpair;

	/*
	 * NVMe spec sets a hard limit of 64K max entries, but
	 *  devices may specify a smaller limit, so we need to check
	 *  the MQES field in the capabilities register.
	 */
	num_entries = nvme_min(NVME_IO_ENTRIES, ctrlr->cap.bits.mqes + 1);

	rc = nvme_qpair_construct(qpair, qid, num_entries, ctrlr, qprio);
	if (rc != 0) {
		free(pqpair);
		return NULL;
	}

	rc = _nvme_pcie_ctrlr_create_io_qpair(ctrlr, qpair, qid);

	if (rc != 0) {
		SPDK_ERRLOG("I/O queue creation failed\n");
		free(pqpair);
		return NULL;
	}

	return qpair;
}

int
nvme_pcie_ctrlr_reinit_io_qpair(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair)
{
	return _nvme_pcie_ctrlr_create_io_qpair(ctrlr, qpair, qpair->id);
}

int
nvme_pcie_ctrlr_delete_io_qpair(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair)
{
	struct nvme_pcie_qpair *pqpair = nvme_pcie_qpair(qpair);
	struct nvme_completion_poll_status status;
	int rc;

	assert(ctrlr != NULL);

	/* Delete the I/O submission queue and then the completion queue */

	status.done = false;
	rc = nvme_pcie_ctrlr_cmd_delete_io_sq(ctrlr, qpair, nvme_completion_poll_cb, &status);
	if (rc != 0) {
		return rc;
	}
	while (status.done == false) {
		spdk_nvme_qpair_process_completions(ctrlr->adminq, 0);
	}
	if (spdk_nvme_cpl_is_error(&status.cpl)) {
		return -1;
	}

	status.done = false;
	rc = nvme_pcie_ctrlr_cmd_delete_io_cq(ctrlr, qpair, nvme_completion_poll_cb, &status);
	if (rc != 0) {
		return rc;
	}
	while (status.done == false) {
		spdk_nvme_qpair_process_completions(ctrlr->adminq, 0);
	}
	if (spdk_nvme_cpl_is_error(&status.cpl)) {
		return -1;
	}

	free(pqpair);

	return 0;
}

static void
nvme_pcie_fail_request_bad_vtophys(struct spdk_nvme_qpair *qpair, struct nvme_tracker *tr)
{
	/*
	 * Bad vtophys translation, so abort this request and return
	 *  immediately.
	 */
	nvme_pcie_qpair_manual_complete_tracker(qpair, tr, SPDK_NVME_SCT_GENERIC,
						SPDK_NVME_SC_INVALID_FIELD,
						1 /* do not retry */, true);
}

/**
 * Build PRP list describing physically contiguous payload buffer.
 */
static int
nvme_pcie_qpair_build_contig_request(struct spdk_nvme_qpair *qpair, struct nvme_request *req,
				     struct nvme_tracker *tr)
{
	uint64_t phys_addr;
	void *seg_addr;
	uint32_t nseg, cur_nseg, modulo, unaligned;
	void *md_payload;
	void *payload = req->payload.u.contig + req->payload_offset;

	phys_addr = spdk_vtophys(payload);
	if (phys_addr == SPDK_VTOPHYS_ERROR) {
		nvme_pcie_fail_request_bad_vtophys(qpair, tr);
		return -1;
	}
	nseg = req->payload_size >> nvme_u32log2(PAGE_SIZE);
	modulo = req->payload_size & (PAGE_SIZE - 1);
	unaligned = phys_addr & (PAGE_SIZE - 1);
	if (modulo || unaligned) {
		nseg += 1 + ((modulo + unaligned - 1) >> nvme_u32log2(PAGE_SIZE));
	}

	if (req->payload.md) {
		md_payload = req->payload.md + req->md_offset;
		tr->req->cmd.mptr = spdk_vtophys(md_payload);
		if (tr->req->cmd.mptr == SPDK_VTOPHYS_ERROR) {
			nvme_pcie_fail_request_bad_vtophys(qpair, tr);
			return -1;
		}
	}

	tr->req->cmd.psdt = SPDK_NVME_PSDT_PRP;
	tr->req->cmd.dptr.prp.prp1 = phys_addr;
	if (nseg == 2) {
		seg_addr = payload + PAGE_SIZE - unaligned;
		tr->req->cmd.dptr.prp.prp2 = spdk_vtophys(seg_addr);
	} else if (nseg > 2) {
		cur_nseg = 1;
		tr->req->cmd.dptr.prp.prp2 = (uint64_t)tr->prp_sgl_bus_addr;
		while (cur_nseg < nseg) {
			seg_addr = payload + cur_nseg * PAGE_SIZE - unaligned;
			phys_addr = spdk_vtophys(seg_addr);
			if (phys_addr == SPDK_VTOPHYS_ERROR) {
				nvme_pcie_fail_request_bad_vtophys(qpair, tr);
				return -1;
			}
			tr->u.prp[cur_nseg - 1] = phys_addr;
			cur_nseg++;
		}
	}

	return 0;
}

/**
 * Build SGL list describing scattered payload buffer.
 */
static int
nvme_pcie_qpair_build_hw_sgl_request(struct spdk_nvme_qpair *qpair, struct nvme_request *req,
				     struct nvme_tracker *tr)
{
	int rc;
	void *virt_addr;
	uint64_t phys_addr;
	uint32_t remaining_transfer_len, length;
	struct spdk_nvme_sgl_descriptor *sgl;
	uint32_t nseg = 0;

	/*
	 * Build scattered payloads.
	 */
	assert(req->payload_size != 0);
	assert(req->payload.type == NVME_PAYLOAD_TYPE_SGL);
	assert(req->payload.u.sgl.reset_sgl_fn != NULL);
	assert(req->payload.u.sgl.next_sge_fn != NULL);
	req->payload.u.sgl.reset_sgl_fn(req->payload.u.sgl.cb_arg, req->payload_offset);

	sgl = tr->u.sgl;
	req->cmd.psdt = SPDK_NVME_PSDT_SGL_MPTR_SGL;
	req->cmd.dptr.sgl1.unkeyed.subtype = 0;

	remaining_transfer_len = req->payload_size;

	while (remaining_transfer_len > 0) {
		if (nseg >= NVME_MAX_SGL_DESCRIPTORS) {
			nvme_pcie_fail_request_bad_vtophys(qpair, tr);
			return -1;
		}

		rc = req->payload.u.sgl.next_sge_fn(req->payload.u.sgl.cb_arg, &virt_addr, &length);
		if (rc) {
			nvme_pcie_fail_request_bad_vtophys(qpair, tr);
			return -1;
		}

		phys_addr = spdk_vtophys(virt_addr);
		if (phys_addr == SPDK_VTOPHYS_ERROR) {
			nvme_pcie_fail_request_bad_vtophys(qpair, tr);
			return -1;
		}

		length = nvme_min(remaining_transfer_len, length);
		remaining_transfer_len -= length;

		sgl->unkeyed.type = SPDK_NVME_SGL_TYPE_DATA_BLOCK;
		sgl->unkeyed.length = length;
		sgl->address = phys_addr;
		sgl->unkeyed.subtype = 0;

		sgl++;
		nseg++;
	}

	if (nseg == 1) {
		/*
		 * The whole transfer can be described by a single SGL descriptor.
		 *  Use the special case described by the spec where SGL1's type is Data Block.
		 *  This means the SGL in the tracker is not used at all, so copy the first (and only)
		 *  SGL element into SGL1.
		 */
		req->cmd.dptr.sgl1.unkeyed.type = SPDK_NVME_SGL_TYPE_DATA_BLOCK;
		req->cmd.dptr.sgl1.address = tr->u.sgl[0].address;
		req->cmd.dptr.sgl1.unkeyed.length = tr->u.sgl[0].unkeyed.length;
	} else {
		/* For now we can only support 1 SGL segment in NVMe controller */
		req->cmd.dptr.sgl1.unkeyed.type = SPDK_NVME_SGL_TYPE_LAST_SEGMENT;
		req->cmd.dptr.sgl1.address = tr->prp_sgl_bus_addr;
		req->cmd.dptr.sgl1.unkeyed.length = nseg * sizeof(struct spdk_nvme_sgl_descriptor);
	}

	return 0;
}

/**
 * Build PRP list describing scattered payload buffer.
 */
static int
nvme_pcie_qpair_build_prps_sgl_request(struct spdk_nvme_qpair *qpair, struct nvme_request *req,
				       struct nvme_tracker *tr)
{
	int rc;
	void *virt_addr;
	uint64_t phys_addr;
	uint32_t data_transferred, remaining_transfer_len, length;
	uint32_t nseg, cur_nseg, total_nseg, last_nseg, modulo, unaligned;
	uint32_t sge_count = 0;
	uint64_t prp2 = 0;

	/*
	 * Build scattered payloads.
	 */
	assert(req->payload.type == NVME_PAYLOAD_TYPE_SGL);
	assert(req->payload.u.sgl.reset_sgl_fn != NULL);
	req->payload.u.sgl.reset_sgl_fn(req->payload.u.sgl.cb_arg, req->payload_offset);

	remaining_transfer_len = req->payload_size;
	total_nseg = 0;
	last_nseg = 0;

	while (remaining_transfer_len > 0) {
		assert(req->payload.u.sgl.next_sge_fn != NULL);
		rc = req->payload.u.sgl.next_sge_fn(req->payload.u.sgl.cb_arg, &virt_addr, &length);
		if (rc) {
			nvme_pcie_fail_request_bad_vtophys(qpair, tr);
			return -1;
		}

		phys_addr = spdk_vtophys(virt_addr);
		if (phys_addr == SPDK_VTOPHYS_ERROR) {
			nvme_pcie_fail_request_bad_vtophys(qpair, tr);
			return -1;
		}

		/* Confirm that this sge is prp compatible. */
		if (phys_addr & 0x3 ||
		    (length < remaining_transfer_len && ((phys_addr + length) & (PAGE_SIZE - 1)))) {
			nvme_pcie_fail_request_bad_vtophys(qpair, tr);
			return -1;
		}

		data_transferred = nvme_min(remaining_transfer_len, length);

		nseg = data_transferred >> nvme_u32log2(PAGE_SIZE);
		modulo = data_transferred & (PAGE_SIZE - 1);
		unaligned = phys_addr & (PAGE_SIZE - 1);
		if (modulo || unaligned) {
			nseg += 1 + ((modulo + unaligned - 1) >> nvme_u32log2(PAGE_SIZE));
		}

		if (total_nseg == 0) {
			req->cmd.psdt = SPDK_NVME_PSDT_PRP;
			req->cmd.dptr.prp.prp1 = phys_addr;
			phys_addr -= unaligned;
		}

		total_nseg += nseg;
		sge_count++;
		remaining_transfer_len -= data_transferred;

		if (total_nseg == 2) {
			if (sge_count == 1)
				tr->req->cmd.dptr.prp.prp2 = phys_addr + PAGE_SIZE;
			else if (sge_count == 2)
				tr->req->cmd.dptr.prp.prp2 = phys_addr;
			/* save prp2 value */
			prp2 = tr->req->cmd.dptr.prp.prp2;
		} else if (total_nseg > 2) {
			if (sge_count == 1)
				cur_nseg = 1;
			else
				cur_nseg = 0;

			tr->req->cmd.dptr.prp.prp2 = (uint64_t)tr->prp_sgl_bus_addr;
			while (cur_nseg < nseg) {
				if (prp2) {
					tr->u.prp[0] = prp2;
					tr->u.prp[last_nseg + 1] = phys_addr + cur_nseg * PAGE_SIZE;
				} else
					tr->u.prp[last_nseg] = phys_addr + cur_nseg * PAGE_SIZE;

				last_nseg++;
				cur_nseg++;
			}
		}
	}

	return 0;
}

static inline bool
nvme_pcie_qpair_check_enabled(struct spdk_nvme_qpair *qpair)
{
	struct nvme_pcie_qpair *pqpair = nvme_pcie_qpair(qpair);

	if (!pqpair->is_enabled &&
	    !qpair->ctrlr->is_resetting) {
		nvme_qpair_enable(qpair);
	}
	return pqpair->is_enabled;
}

int
nvme_pcie_qpair_submit_request(struct spdk_nvme_qpair *qpair, struct nvme_request *req)
{
	struct nvme_tracker *tr;
	int rc;
	struct spdk_nvme_ctrlr *ctrlr = qpair->ctrlr;
	struct nvme_pcie_qpair *pqpair = nvme_pcie_qpair(qpair);

	nvme_pcie_qpair_check_enabled(qpair);

	tr = LIST_FIRST(&pqpair->free_tr);

	if (tr == NULL || !pqpair->is_enabled) {
		/*
		 * No tracker is available, or the qpair is disabled due to
		 *  an in-progress controller-level reset.
		 *
		 * Put the request on the qpair's request queue to be
		 *  processed when a tracker frees up via a command
		 *  completion or when the controller reset is
		 *  completed.
		 */
		STAILQ_INSERT_TAIL(&qpair->queued_req, req, stailq);
		return 0;
	}

	LIST_REMOVE(tr, list); /* remove tr from free_tr */
	LIST_INSERT_HEAD(&pqpair->outstanding_tr, tr, list);
	tr->req = req;
	req->cmd.cid = tr->cid;

	if (req->payload_size == 0) {
		/* Null payload - leave PRP fields zeroed */
		rc = 0;
	} else if (req->payload.type == NVME_PAYLOAD_TYPE_CONTIG) {
		rc = nvme_pcie_qpair_build_contig_request(qpair, req, tr);
	} else if (req->payload.type == NVME_PAYLOAD_TYPE_SGL) {
		if (ctrlr->flags & SPDK_NVME_CTRLR_SGL_SUPPORTED) {
			rc = nvme_pcie_qpair_build_hw_sgl_request(qpair, req, tr);
		} else {
			rc = nvme_pcie_qpair_build_prps_sgl_request(qpair, req, tr);
		}
	} else {
		assert(0);
		nvme_pcie_fail_request_bad_vtophys(qpair, tr);
		rc = -EINVAL;
	}

	if (rc < 0) {
		return rc;
	}

	nvme_pcie_qpair_submit_tracker(qpair, tr);
	return 0;
}

int32_t
nvme_pcie_qpair_process_completions(struct spdk_nvme_qpair *qpair, uint32_t max_completions)
{
	struct nvme_pcie_qpair	*pqpair = nvme_pcie_qpair(qpair);
	struct nvme_tracker	*tr;
	struct spdk_nvme_cpl	*cpl;
	uint32_t num_completions = 0;

	if (!nvme_pcie_qpair_check_enabled(qpair)) {
		/*
		 * qpair is not enabled, likely because a controller reset is
		 *  is in progress.  Ignore the interrupt - any I/O that was
		 *  associated with this interrupt will get retried when the
		 *  reset is complete.
		 */
		return 0;
	}

	if (max_completions == 0 || (max_completions > (qpair->num_entries - 1U))) {

		/*
		 * max_completions == 0 means unlimited, but complete at most one
		 * queue depth batch of I/O at a time so that the completion
		 * queue doorbells don't wrap around.
		 */
		max_completions = qpair->num_entries - 1;
	}

	while (1) {
		cpl = &pqpair->cpl[pqpair->cq_head];

		if (cpl->status.p != pqpair->phase)
			break;

		tr = &pqpair->tr[cpl->cid];

		if (tr->active) {
			nvme_pcie_qpair_complete_tracker(qpair, tr, cpl, true);
		} else {
			SPDK_ERRLOG("cpl does not map to outstanding cmd\n");
			nvme_qpair_print_completion(qpair, cpl);
			assert(0);
		}

		if (++pqpair->cq_head == qpair->num_entries) {
			pqpair->cq_head = 0;
			pqpair->phase = !pqpair->phase;
		}

		if (++num_completions == max_completions) {
			break;
		}
	}

	if (num_completions > 0) {
		spdk_mmio_write_4(pqpair->cq_hdbl, pqpair->cq_head);
	}

	/* Before returning, complete any pending admin request. */
	if (nvme_qpair_is_admin_queue(qpair)) {
		nvme_pcie_qpair_complete_pending_admin_request(qpair);
	}

	return num_completions;
}
