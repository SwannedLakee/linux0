/*
 * Copyright 2023 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include "kfd_events.h"
#include "kfd_debug.h"
#include "soc15_int.h"
#include "kfd_device_queue_manager.h"

/*
 * GFX10 SQ Interrupts
 *
 * There are 3 encoding types of interrupts sourced from SQ sent as a 44-bit
 * packet to the Interrupt Handler:
 * Auto - Generated by the SQG (various cmd overflows, timestamps etc)
 * Wave - Generated by S_SENDMSG through a shader program
 * Error - HW generated errors (Illegal instructions, Memviols, EDC etc)
 *
 * The 44-bit packet is mapped as {context_id1[7:0],context_id0[31:0]} plus
 * 4-bits for VMID (SOC15_VMID_FROM_IH_ENTRY) as such:
 *
 * - context_id1[7:6]
 * Encoding type (0 = Auto, 1 = Wave, 2 = Error)
 *
 * - context_id0[24]
 * PRIV bit indicates that Wave S_SEND or error occurred within trap
 *
 * - context_id0[22:0]
 * 23-bit data with the following layout per encoding type:
 * Auto - only context_id0[8:0] is used, which reports various interrupts
 * generated by SQG.  The rest is 0.
 * Wave - user data sent from m0 via S_SENDMSG
 * Error - Error type (context_id0[22:19]), Error Details (rest of bits)
 *
 * The other context_id bits show coordinates (SE/SH/CU/SIMD/WGP) for wave
 * S_SENDMSG and Errors.  These are 0 for Auto.
 */

enum SQ_INTERRUPT_WORD_ENCODING {
	SQ_INTERRUPT_WORD_ENCODING_AUTO = 0x0,
	SQ_INTERRUPT_WORD_ENCODING_INST,
	SQ_INTERRUPT_WORD_ENCODING_ERROR,
};

enum SQ_INTERRUPT_ERROR_TYPE {
	SQ_INTERRUPT_ERROR_TYPE_EDC_FUE = 0x0,
	SQ_INTERRUPT_ERROR_TYPE_ILLEGAL_INST,
	SQ_INTERRUPT_ERROR_TYPE_MEMVIOL,
	SQ_INTERRUPT_ERROR_TYPE_EDC_FED,
};

/* SQ_INTERRUPT_WORD_AUTO_CTXID */
#define SQ_INTERRUPT_WORD_AUTO_CTXID0__THREAD_TRACE__SHIFT 0
#define SQ_INTERRUPT_WORD_AUTO_CTXID0__WLT__SHIFT 1
#define SQ_INTERRUPT_WORD_AUTO_CTXID0__THREAD_TRACE_BUF0_FULL__SHIFT 2
#define SQ_INTERRUPT_WORD_AUTO_CTXID0__THREAD_TRACE_BUF1_FULL__SHIFT 3
#define SQ_INTERRUPT_WORD_AUTO_CTXID0__THREAD_TRACE_UTC_ERROR__SHIFT 7
#define SQ_INTERRUPT_WORD_AUTO_CTXID1__SE_ID__SHIFT 4
#define SQ_INTERRUPT_WORD_AUTO_CTXID1__ENCODING__SHIFT 6

#define SQ_INTERRUPT_WORD_AUTO_CTXID0__THREAD_TRACE_MASK 0x00000001
#define SQ_INTERRUPT_WORD_AUTO_CTXID0__WLT_MASK 0x00000002
#define SQ_INTERRUPT_WORD_AUTO_CTXID0__THREAD_TRACE_BUF0_FULL_MASK 0x00000004
#define SQ_INTERRUPT_WORD_AUTO_CTXID0__THREAD_TRACE_BUF1_FULL_MASK 0x00000008
#define SQ_INTERRUPT_WORD_AUTO_CTXID0__THREAD_TRACE_UTC_ERROR_MASK 0x00000080
#define SQ_INTERRUPT_WORD_AUTO_CTXID1__SE_ID_MASK 0x030
#define SQ_INTERRUPT_WORD_AUTO_CTXID1__ENCODING_MASK 0x0c0

/* SQ_INTERRUPT_WORD_WAVE_CTXID */
#define SQ_INTERRUPT_WORD_WAVE_CTXID0__DATA__SHIFT 0
#define SQ_INTERRUPT_WORD_WAVE_CTXID0__SA_ID__SHIFT 23
#define SQ_INTERRUPT_WORD_WAVE_CTXID0__PRIV__SHIFT 24
#define SQ_INTERRUPT_WORD_WAVE_CTXID0__WAVE_ID__SHIFT 25
#define SQ_INTERRUPT_WORD_WAVE_CTXID0__SIMD_ID__SHIFT 30
#define SQ_INTERRUPT_WORD_WAVE_CTXID1__WGP_ID__SHIFT 0
#define SQ_INTERRUPT_WORD_WAVE_CTXID1__SE_ID__SHIFT 4
#define SQ_INTERRUPT_WORD_WAVE_CTXID1__ENCODING__SHIFT 6

#define SQ_INTERRUPT_WORD_WAVE_CTXID0__DATA_MASK 0x000007fffff
#define SQ_INTERRUPT_WORD_WAVE_CTXID0__SA_ID_MASK 0x0000800000
#define SQ_INTERRUPT_WORD_WAVE_CTXID0__PRIV_MASK 0x00001000000
#define SQ_INTERRUPT_WORD_WAVE_CTXID0__WAVE_ID_MASK 0x0003e000000
#define SQ_INTERRUPT_WORD_WAVE_CTXID0__SIMD_ID_MASK 0x000c0000000
#define SQ_INTERRUPT_WORD_WAVE_CTXID1__WGP_ID_MASK 0x00f
#define SQ_INTERRUPT_WORD_WAVE_CTXID1__SE_ID_MASK 0x030
#define SQ_INTERRUPT_WORD_WAVE_CTXID1__ENCODING_MASK 0x0c0

#define KFD_CTXID0__ERR_TYPE_MASK 0x780000
#define KFD_CTXID0__ERR_TYPE__SHIFT 19

/* GFX10 SQ interrupt ENC type bit (context_id1[7:6]) for wave s_sendmsg */
#define KFD_CONTEXT_ID1_ENC_TYPE_WAVE_MASK	0x40
/* GFX10 SQ interrupt PRIV bit (context_id0[24]) for s_sendmsg inside trap */
#define KFD_CONTEXT_ID0_PRIV_MASK		0x1000000
/*
 * The debugger will send user data(m0) with PRIV=1 to indicate it requires
 * notification from the KFD with the following queue id (DOORBELL_ID) and
 * trap code (TRAP_CODE).
 */
#define KFD_CONTEXT_ID0_DEBUG_DOORBELL_MASK	0x0003ff
#define KFD_CONTEXT_ID0_DEBUG_TRAP_CODE_SHIFT	10
#define KFD_CONTEXT_ID0_DEBUG_TRAP_CODE_MASK	0x07fc00
#define KFD_DEBUG_DOORBELL_ID(ctxid0)	((ctxid0) &	\
				KFD_CONTEXT_ID0_DEBUG_DOORBELL_MASK)
#define KFD_DEBUG_TRAP_CODE(ctxid0)	(((ctxid0) &	\
				KFD_CONTEXT_ID0_DEBUG_TRAP_CODE_MASK)	\
				>> KFD_CONTEXT_ID0_DEBUG_TRAP_CODE_SHIFT)
#define KFD_DEBUG_CP_BAD_OP_ECODE_MASK		0x3fffc00
#define KFD_DEBUG_CP_BAD_OP_ECODE_SHIFT		10
#define KFD_DEBUG_CP_BAD_OP_ECODE(ctxid0) (((ctxid0) &			\
				KFD_DEBUG_CP_BAD_OP_ECODE_MASK)		\
				>> KFD_DEBUG_CP_BAD_OP_ECODE_SHIFT)

static bool event_interrupt_isr_v10(struct kfd_node *dev,
					const uint32_t *ih_ring_entry,
					uint32_t *patched_ihre,
					bool *patched_flag)
{
	uint16_t source_id, client_id, pasid, vmid;
	const uint32_t *data = ih_ring_entry;

	source_id = SOC15_SOURCE_ID_FROM_IH_ENTRY(ih_ring_entry);
	client_id = SOC15_CLIENT_ID_FROM_IH_ENTRY(ih_ring_entry);

	/* Only handle interrupts from KFD VMIDs */
	vmid = SOC15_VMID_FROM_IH_ENTRY(ih_ring_entry);
	if (!KFD_IRQ_IS_FENCE(client_id, source_id) &&
	   (vmid < dev->vm_info.first_vmid_kfd ||
	    vmid > dev->vm_info.last_vmid_kfd))
		return false;

	pasid = SOC15_PASID_FROM_IH_ENTRY(ih_ring_entry);

	/* Only handle clients we care about */
	if (client_id != SOC15_IH_CLIENTID_GRBM_CP &&
	    client_id != SOC15_IH_CLIENTID_SDMA0 &&
	    client_id != SOC15_IH_CLIENTID_SDMA1 &&
	    client_id != SOC15_IH_CLIENTID_SDMA2 &&
	    client_id != SOC15_IH_CLIENTID_SDMA3 &&
	    client_id != SOC15_IH_CLIENTID_SDMA4 &&
	    client_id != SOC15_IH_CLIENTID_SDMA5 &&
	    client_id != SOC15_IH_CLIENTID_SDMA6 &&
	    client_id != SOC15_IH_CLIENTID_SDMA7 &&
	    client_id != SOC15_IH_CLIENTID_VMC &&
	    client_id != SOC15_IH_CLIENTID_VMC1 &&
	    client_id != SOC15_IH_CLIENTID_UTCL2 &&
	    client_id != SOC15_IH_CLIENTID_SE0SH &&
	    client_id != SOC15_IH_CLIENTID_SE1SH &&
	    client_id != SOC15_IH_CLIENTID_SE2SH &&
	    client_id != SOC15_IH_CLIENTID_SE3SH)
		return false;

	dev_dbg(dev->adev->dev,
		"client id 0x%x, source id %d, vmid %d, pasid 0x%x. raw data:\n",
		client_id, source_id, vmid, pasid);
	dev_dbg(dev->adev->dev, "%8X, %8X, %8X, %8X, %8X, %8X, %8X, %8X.\n",
		data[0], data[1], data[2], data[3], data[4], data[5], data[6],
		data[7]);

	if (pasid == 0)
		return 0;

	/* Interrupt types we care about: various signals and faults.
	 * They will be forwarded to a work queue (see below).
	 */
	return source_id == SOC15_INTSRC_CP_END_OF_PIPE ||
		source_id == SOC15_INTSRC_SDMA_TRAP ||
		source_id == SOC15_INTSRC_SQ_INTERRUPT_MSG ||
		source_id == SOC15_INTSRC_CP_BAD_OPCODE ||
		client_id == SOC15_IH_CLIENTID_VMC ||
		client_id == SOC15_IH_CLIENTID_VMC1 ||
		client_id == SOC15_IH_CLIENTID_UTCL2 ||
		KFD_IRQ_IS_FENCE(client_id, source_id);
}

static void event_interrupt_wq_v10(struct kfd_node *dev,
					const uint32_t *ih_ring_entry)
{
	uint16_t source_id, client_id, pasid, vmid;
	uint32_t context_id0, context_id1;
	uint32_t encoding, sq_intr_err_type;

	source_id = SOC15_SOURCE_ID_FROM_IH_ENTRY(ih_ring_entry);
	client_id = SOC15_CLIENT_ID_FROM_IH_ENTRY(ih_ring_entry);
	pasid = SOC15_PASID_FROM_IH_ENTRY(ih_ring_entry);
	vmid = SOC15_VMID_FROM_IH_ENTRY(ih_ring_entry);
	context_id0 = SOC15_CONTEXT_ID0_FROM_IH_ENTRY(ih_ring_entry);
	context_id1 = SOC15_CONTEXT_ID1_FROM_IH_ENTRY(ih_ring_entry);

	if (client_id == SOC15_IH_CLIENTID_GRBM_CP ||
	    client_id == SOC15_IH_CLIENTID_SE0SH ||
	    client_id == SOC15_IH_CLIENTID_SE1SH ||
	    client_id == SOC15_IH_CLIENTID_SE2SH ||
	    client_id == SOC15_IH_CLIENTID_SE3SH) {
		if (source_id == SOC15_INTSRC_CP_END_OF_PIPE)
			kfd_signal_event_interrupt(pasid, context_id0, 32);
		else if (source_id == SOC15_INTSRC_SQ_INTERRUPT_MSG) {
			encoding = REG_GET_FIELD(context_id1,
						SQ_INTERRUPT_WORD_WAVE_CTXID1, ENCODING);
			switch (encoding) {
			case SQ_INTERRUPT_WORD_ENCODING_AUTO:
				dev_dbg_ratelimited(
					dev->adev->dev,
					"sq_intr: auto, se %d, ttrace %d, wlt %d, ttrac_buf0_full %d, ttrac_buf1_full %d, ttrace_utc_err %d\n",
					REG_GET_FIELD(
						context_id1,
						SQ_INTERRUPT_WORD_AUTO_CTXID1,
						SE_ID),
					REG_GET_FIELD(
						context_id0,
						SQ_INTERRUPT_WORD_AUTO_CTXID0,
						THREAD_TRACE),
					REG_GET_FIELD(
						context_id0,
						SQ_INTERRUPT_WORD_AUTO_CTXID0,
						WLT),
					REG_GET_FIELD(
						context_id0,
						SQ_INTERRUPT_WORD_AUTO_CTXID0,
						THREAD_TRACE_BUF0_FULL),
					REG_GET_FIELD(
						context_id0,
						SQ_INTERRUPT_WORD_AUTO_CTXID0,
						THREAD_TRACE_BUF1_FULL),
					REG_GET_FIELD(
						context_id0,
						SQ_INTERRUPT_WORD_AUTO_CTXID0,
						THREAD_TRACE_UTC_ERROR));
				break;
			case SQ_INTERRUPT_WORD_ENCODING_INST:
				dev_dbg_ratelimited(
					dev->adev->dev,
					"sq_intr: inst, se %d, data 0x%x, sa %d, priv %d, wave_id %d, simd_id %d, wgp_id %d\n",
					REG_GET_FIELD(
						context_id1,
						SQ_INTERRUPT_WORD_WAVE_CTXID1,
						SE_ID),
					REG_GET_FIELD(
						context_id0,
						SQ_INTERRUPT_WORD_WAVE_CTXID0,
						DATA),
					REG_GET_FIELD(
						context_id0,
						SQ_INTERRUPT_WORD_WAVE_CTXID0,
						SA_ID),
					REG_GET_FIELD(
						context_id0,
						SQ_INTERRUPT_WORD_WAVE_CTXID0,
						PRIV),
					REG_GET_FIELD(
						context_id0,
						SQ_INTERRUPT_WORD_WAVE_CTXID0,
						WAVE_ID),
					REG_GET_FIELD(
						context_id0,
						SQ_INTERRUPT_WORD_WAVE_CTXID0,
						SIMD_ID),
					REG_GET_FIELD(
						context_id1,
						SQ_INTERRUPT_WORD_WAVE_CTXID1,
						WGP_ID));
				if (context_id0 & SQ_INTERRUPT_WORD_WAVE_CTXID0__PRIV_MASK) {
					if (kfd_set_dbg_ev_from_interrupt(dev, pasid,
							KFD_DEBUG_DOORBELL_ID(context_id0),
							KFD_DEBUG_TRAP_CODE(context_id0),
							NULL, 0))
						return;
				}
				break;
			case SQ_INTERRUPT_WORD_ENCODING_ERROR:
				sq_intr_err_type = REG_GET_FIELD(context_id0, KFD_CTXID0,
								ERR_TYPE);
				dev_warn_ratelimited(
					dev->adev->dev,
					"sq_intr: error, se %d, data 0x%x, sa %d, priv %d, wave_id %d, simd_id %d, wgp_id %d, err_type %d\n",
					REG_GET_FIELD(
						context_id1,
						SQ_INTERRUPT_WORD_WAVE_CTXID1,
						SE_ID),
					REG_GET_FIELD(
						context_id0,
						SQ_INTERRUPT_WORD_WAVE_CTXID0,
						DATA),
					REG_GET_FIELD(
						context_id0,
						SQ_INTERRUPT_WORD_WAVE_CTXID0,
						SA_ID),
					REG_GET_FIELD(
						context_id0,
						SQ_INTERRUPT_WORD_WAVE_CTXID0,
						PRIV),
					REG_GET_FIELD(
						context_id0,
						SQ_INTERRUPT_WORD_WAVE_CTXID0,
						WAVE_ID),
					REG_GET_FIELD(
						context_id0,
						SQ_INTERRUPT_WORD_WAVE_CTXID0,
						SIMD_ID),
					REG_GET_FIELD(
						context_id1,
						SQ_INTERRUPT_WORD_WAVE_CTXID1,
						WGP_ID),
					sq_intr_err_type);
				break;
			default:
				break;
			}
			kfd_signal_event_interrupt(pasid, context_id0 & 0x7fffff, 23);
		} else if (source_id == SOC15_INTSRC_CP_BAD_OPCODE &&
			   KFD_DBG_EC_TYPE_IS_PACKET(KFD_DEBUG_CP_BAD_OP_ECODE(context_id0))) {
			kfd_set_dbg_ev_from_interrupt(dev, pasid,
				KFD_DEBUG_DOORBELL_ID(context_id0),
				KFD_EC_MASK(KFD_DEBUG_CP_BAD_OP_ECODE(context_id0)),
				NULL,
				0);
		}
	} else if (client_id == SOC15_IH_CLIENTID_SDMA0 ||
		   client_id == SOC15_IH_CLIENTID_SDMA1 ||
		   client_id == SOC15_IH_CLIENTID_SDMA2 ||
		   client_id == SOC15_IH_CLIENTID_SDMA3 ||
		   (client_id == SOC15_IH_CLIENTID_SDMA3_Sienna_Cichlid &&
		    KFD_GC_VERSION(dev) == IP_VERSION(10, 3, 0)) ||
		   client_id == SOC15_IH_CLIENTID_SDMA4 ||
		   client_id == SOC15_IH_CLIENTID_SDMA5 ||
		   client_id == SOC15_IH_CLIENTID_SDMA6 ||
		   client_id == SOC15_IH_CLIENTID_SDMA7) {
		if (source_id == SOC15_INTSRC_SDMA_TRAP) {
			kfd_signal_event_interrupt(pasid, context_id0 & 0xfffffff, 28);
		}
	} else if (client_id == SOC15_IH_CLIENTID_VMC ||
		   client_id == SOC15_IH_CLIENTID_VMC1 ||
		   client_id == SOC15_IH_CLIENTID_UTCL2) {
		struct kfd_vm_fault_info info = {0};
		uint16_t ring_id = SOC15_RING_ID_FROM_IH_ENTRY(ih_ring_entry);
		struct kfd_hsa_memory_exception_data exception_data;

		info.vmid = vmid;
		info.mc_id = client_id;
		info.page_addr = ih_ring_entry[4] |
			(uint64_t)(ih_ring_entry[5] & 0xf) << 32;
		info.prot_valid = ring_id & 0x08;
		info.prot_read  = ring_id & 0x10;
		info.prot_write = ring_id & 0x20;

		memset(&exception_data, 0, sizeof(exception_data));
		exception_data.gpu_id = dev->id;
		exception_data.va = (info.page_addr) << PAGE_SHIFT;
		exception_data.failure.NotPresent = info.prot_valid ? 1 : 0;
		exception_data.failure.NoExecute = info.prot_exec ? 1 : 0;
		exception_data.failure.ReadOnly = info.prot_write ? 1 : 0;
		exception_data.failure.imprecise = 0;

		kfd_set_dbg_ev_from_interrupt(dev,
						pasid,
						-1,
						KFD_EC_MASK(EC_DEVICE_MEMORY_VIOLATION),
						&exception_data,
						sizeof(exception_data));
	} else if (KFD_IRQ_IS_FENCE(client_id, source_id)) {
		kfd_process_close_interrupt_drain(pasid);
	}
}

const struct kfd_event_interrupt_class event_interrupt_class_v10 = {
	.interrupt_isr = event_interrupt_isr_v10,
	.interrupt_wq = event_interrupt_wq_v10,
};
