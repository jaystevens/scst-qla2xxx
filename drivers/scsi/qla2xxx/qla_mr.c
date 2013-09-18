/*
 * QLogic Fibre Channel HBA Driver
 * Copyright (c)  2003-2012 QLogic Corporation
 *
 * See LICENSE.qla2xxx for copyright and licensing details.
 */
#include "qla_def.h"
#include <linux/delay.h>
#include <linux/pci.h>
#include <linux/ratelimit.h>
#include <linux/vmalloc.h>
#include <scsi/scsi_tcq.h>
#include <linux/utsname.h>

/**
 * qlafx00_pci_config() - Setup ISA7xxx PCI configuration registers.
 * @ha: HA context
 *
 * Returns 0 on success.
 */
int
qlafx00_pci_config(scsi_qla_host_t *vha)
{
	uint16_t w;
	struct qla_hw_data *ha = vha->hw;

	pci_set_master(ha->pdev);
	pci_try_set_mwi(ha->pdev);

	pci_read_config_word(ha->pdev, PCI_COMMAND, &w);
	w |= (PCI_COMMAND_PARITY | PCI_COMMAND_SERR);
	w &= ~PCI_COMMAND_INTX_DISABLE;
	pci_write_config_word(ha->pdev, PCI_COMMAND, w);

	/* PCIe -- adjust Maximum Read Request Size (2048). */
	if (pci_find_capability(ha->pdev, PCI_CAP_ID_EXP))
		pcie_set_readrq(ha->pdev, 2048);

	ha->chip_revision = ha->pdev->revision;

	return QLA_SUCCESS;
}

/**
 * qlafx00_warm_reset() - Perform warm reset of iSA(CPUs being reset on SOC).
 * @ha: HA context
 *
  */
static inline void
qlafx00_soc_cpu_reset(scsi_qla_host_t *vha)
{
	unsigned long flags = 0;
	struct qla_hw_data *ha = vha->hw;
	int i, core;
	uint32_t cnt;

	/* Set all 4 cores in reset */
	for (i = 0; i < 4; i++) {
		QLAFX00_SET_HBA_SOC_REG(ha,
		    (SOC_SW_RST_CONTROL_REG_CORE0 + 8*i), (0xF01));
	}

	/* Set all 4 core Clock gating control */
	for (i = 0; i < 4; i++) {
		QLAFX00_SET_HBA_SOC_REG(ha,
		    (SOC_SW_RST_CONTROL_REG_CORE0 + 4 + 8*i), (0x01010101));
	}

	/* Reset all units in Fabric */
	QLAFX00_SET_HBA_SOC_REG(ha, SOC_FABRIC_RST_CONTROL_REG, (0x11F0101));

	/* Reset all interrupt control registers */
	for (i = 0; i < 115; i++) {
		QLAFX00_SET_HBA_SOC_REG(ha,
		    (SOC_INTERRUPT_SOURCE_I_CONTROL_REG + 4*i), (0x0));
	}

	/* Reset Timers control registers. per core */
	for (core = 0; core < 4; core++)
		for (i = 0; i < 8; i++)
			QLAFX00_SET_HBA_SOC_REG(ha,
			    (SOC_CORE_TIMER_REG + 0x100*core + 4*i), (0x0));

	/* Reset per core IRQ ack register */
	for (core = 0; core < 4; core++)
		QLAFX00_SET_HBA_SOC_REG(ha,
		    (SOC_IRQ_ACK_REG + 0x100*core), (0x3FF));

	/* Set Fabric control and config to defaults */
	QLAFX00_SET_HBA_SOC_REG(ha, SOC_FABRIC_CONTROL_REG, (0x2));
	QLAFX00_SET_HBA_SOC_REG(ha, SOC_FABRIC_CONFIG_REG, (0x3));

	spin_lock_irqsave(&ha->hardware_lock, flags);

	/* Kick in Fabric units */
	QLAFX00_SET_HBA_SOC_REG(ha, SOC_FABRIC_RST_CONTROL_REG, (0x0));

	/* Kick in Core0 to start boot process */
	QLAFX00_SET_HBA_SOC_REG(ha, SOC_SW_RST_CONTROL_REG_CORE0, (0xF00));

	/* Wait 10secs for soft-reset to complete. */
	for (cnt = 10; cnt; cnt--) {
		msleep(1000);
		barrier();
	}
	spin_unlock_irqrestore(&ha->hardware_lock, flags);
}

/**
 * qlafx00_soft_reset() - Soft Reset ISPFx00.
 * @ha: HA context
 *
 * Returns 0 on success.
 */
void
qlafx00_soft_reset(scsi_qla_host_t *vha)
{
	struct qla_hw_data *ha = vha->hw;

	if (unlikely(pci_channel_offline(ha->pdev) &&
	    ha->flags.pci_channel_io_perm_failure))
		return;

	ha->isp_ops->disable_intrs(ha);
	qlafx00_soc_cpu_reset(vha);
	ha->isp_ops->enable_intrs(ha);
}

/**
 * qlafx00_chip_diag() - Test ISA7xxx for proper operation.
 * @ha: HA context
 *
 * Returns 0 on success.
 */
int
qlafx00_chip_diag(scsi_qla_host_t *vha)
{
	int rval = 0;
	struct qla_hw_data *ha = vha->hw;
	struct req_que *req = ha->req_q_map[0];

	ha->fw_transfer_size = REQUEST_ENTRY_SIZE * req->length;

	rval = qlafx00_mbx_reg_test(vha);
	if (rval) {
		ql_log(ql_log_warn, vha, 0x1165,
		    "Failed mailbox send register test\n");
	} else {
		/* Flag a successful rval */
		rval = QLA_SUCCESS;
	}
	return rval;
}

void
qlafx00_config_rings(struct scsi_qla_host *vha)
{
	struct qla_hw_data *ha = vha->hw;
	struct device_reg_fx00 __iomem *reg = &ha->iobase->ispfx00;
	init_cb_fx_t *icb;
	struct req_que *req = ha->req_q_map[0];
	struct rsp_que *rsp = ha->rsp_q_map[0];

	/* Setup ring parameters in initialization control block. */
	icb = (init_cb_fx_t *)ha->init_cb;
	icb->request_q_outpointer = __constant_cpu_to_le16(0);
	icb->response_q_inpointer = __constant_cpu_to_le16(0);
	icb->request_q_length = cpu_to_le16(req->length);
	icb->response_q_length = cpu_to_le16(rsp->length);
	icb->request_q_address[0] = cpu_to_le32(LSD(req->dma));
	icb->request_q_address[1] = cpu_to_le32(MSD(req->dma));
	icb->response_q_address[0] = cpu_to_le32(LSD(rsp->dma));
	icb->response_q_address[1] = cpu_to_le32(MSD(rsp->dma));

	WRT_REG_DWORD(&reg->req_q_in, 0);
	WRT_REG_DWORD(&reg->req_q_out, 0);

	WRT_REG_DWORD(&reg->rsp_q_in, 0);
	WRT_REG_DWORD(&reg->rsp_q_out, 0);

	/* PCI posting */
	RD_REG_DWORD(&reg->rsp_q_out);
}

char *
qlafx00_pci_info_str(struct scsi_qla_host *vha, char *str)
{
	struct qla_hw_data *ha = vha->hw;
	int pcie_reg;

	pcie_reg = pci_find_capability(ha->pdev, PCI_CAP_ID_EXP);
	if (pcie_reg) {
		strcpy(str, "PCIe iSA");
		return str;
	}
	return str;
}

char *
qlafx00_fw_version_str(struct scsi_qla_host *vha, char *str)
{
	struct qla_hw_data *ha = vha->hw;

	sprintf(str, "%s", ha->mr.fw_version);
	return str;
}

void
qlafx00_enable_intrs(struct qla_hw_data *ha)
{
	unsigned long flags = 0;

	spin_lock_irqsave(&ha->hardware_lock, flags);
	ha->interrupts_on = 1;
	QLAFX00_ENABLE_ICNTRL_REG(ha);
	spin_unlock_irqrestore(&ha->hardware_lock, flags);
}

void
qlafx00_disable_intrs(struct qla_hw_data *ha)
{
	unsigned long flags = 0;

	spin_lock_irqsave(&ha->hardware_lock, flags);
	ha->interrupts_on = 0;
	QLAFX00_DISABLE_ICNTRL_REG(ha);
	spin_unlock_irqrestore(&ha->hardware_lock, flags);
}

int
qlafx00_abort_command(srb_t *sp)
{
	unsigned long   flags = 0;

	uint32_t	handle;
	fc_port_t	*fcport = sp->fcport;
	struct scsi_qla_host *vha = fcport->vha;
	struct qla_hw_data *ha = vha->hw;
	struct req_que *req = vha->req;

	spin_lock_irqsave(&ha->hardware_lock, flags);
	for (handle = 1; handle < DEFAULT_OUTSTANDING_COMMANDS; handle++) {
		if (req->outstanding_cmds[handle] == sp)
			break;
	}
	spin_unlock_irqrestore(&ha->hardware_lock, flags);
	if (handle == DEFAULT_OUTSTANDING_COMMANDS) {
		/* Command not found. */
		return QLA_FUNCTION_FAILED;
	}
	return qlafx00_async_abt_cmd(sp);
}

static void
qlafx00_tmf_iocb_timeout(void *data)
{
	srb_t *sp = (srb_t *)data;
	struct srb_iocb *tmf = &sp->u.iocb_cmd;

	tmf->u.tmf.comp_status = CS_TIMEOUT;
	complete(&tmf->u.tmf.comp);
}

static void
qlafx00_tmf_sp_done(void *data, void *ptr, int res)
{
	srb_t *sp = (srb_t *)ptr;
	struct srb_iocb *tmf = &sp->u.iocb_cmd;

	complete(&tmf->u.tmf.comp);
}

int
qlafx00_async_tm_cmd(fc_port_t *fcport, uint32_t flags,
    uint32_t lun, uint32_t tag)
{
	scsi_qla_host_t *vha = fcport->vha;
	struct srb_iocb *tm_iocb;
	srb_t *sp;
	int rval = QLA_FUNCTION_FAILED;

	sp = qla2x00_get_sp(vha, fcport, GFP_KERNEL);
	if (!sp)
		goto done;

	tm_iocb = &sp->u.iocb_cmd;
	sp->type = SRB_TM_CMD;
	sp->name = "tmf";
	qla2x00_init_timer(sp, qla2x00_get_async_timeout(vha));
	tm_iocb->u.tmf.flags = flags;
	tm_iocb->u.tmf.lun = lun;
	tm_iocb->u.tmf.data = tag;
	sp->done = qlafx00_tmf_sp_done;
	tm_iocb->timeout = qlafx00_tmf_iocb_timeout;
	init_completion(&tm_iocb->u.tmf.comp);

	rval = qla2x00_start_sp(sp);
	if (rval != QLA_SUCCESS)
		goto done_free_sp;

	ql_dbg(ql_dbg_async, vha, 0x507b,
	    "Task management command issued target_id=%x\n",
	    fcport->tgt_id);

	wait_for_completion(&tm_iocb->u.tmf.comp);

	rval = tm_iocb->u.tmf.comp_status == CS_COMPLETE ?
	    QLA_SUCCESS : QLA_FUNCTION_FAILED;

done_free_sp:
	sp->free(vha, sp);
done:
	return rval;
}

int
qlafx00_abort_target(fc_port_t *fcport, unsigned int l, int tag)
{
	return qlafx00_async_tm_cmd(fcport, TCF_TARGET_RESET, l, tag);
}

int
qlafx00_lun_reset(fc_port_t *fcport, unsigned int l, int tag)
{
	return qlafx00_async_tm_cmd(fcport, TCF_LUN_RESET, l, tag);
}

int
qlafx00_loop_reset(scsi_qla_host_t *vha)
{
	int ret;
	struct fc_port *fcport;
	struct qla_hw_data *ha = vha->hw;

	if (ql2xtargetreset) {
		list_for_each_entry(fcport, &vha->vp_fcports, list) {
			if (fcport->port_type != FCT_TARGET)
				continue;

			ret = ha->isp_ops->target_reset(fcport, 0, 0);
			if (ret != QLA_SUCCESS) {
				ql_dbg(ql_dbg_taskm, vha, 0x803d,
				    "Bus Reset failed: Reset=%d "
				    "d_id=%x.\n", ret, fcport->d_id.b24);
			}
		}
	}
	return QLA_SUCCESS;
}

int
qlafx00_iospace_config(struct qla_hw_data *ha)
{
	if (pci_request_selected_regions(ha->pdev, ha->bars,
	    QLA2XXX_DRIVER_NAME)) {
		ql_log_pci(ql_log_fatal, ha->pdev, 0x0125,
		    "Failed to reserve PIO/MMIO regions (%s), aborting.\n",
		    pci_name(ha->pdev));
		goto iospace_error_exit;
	}

	/* Use MMIO operations for all accesses. */
	if (!(pci_resource_flags(ha->pdev, 0) & IORESOURCE_MEM)) {
		ql_log_pci(ql_log_warn, ha->pdev, 0x0126,
		    "Invalid pci I/O region size (%s).\n",
		    pci_name(ha->pdev));
		goto iospace_error_exit;
	}
	if (pci_resource_len(ha->pdev, 0) < BAR0_LEN_FX00) {
		ql_log_pci(ql_log_warn, ha->pdev, 0x0127,
		    "Invalid PCI mem BAR0 region size (%s), aborting\n",
			pci_name(ha->pdev));
		goto iospace_error_exit;
	}

	ha->cregbase = ioremap_nocache(pci_resource_start(ha->pdev, 0), BAR0_LEN_FX00);
	if (!ha->cregbase) {
		ql_log_pci(ql_log_fatal, ha->pdev, 0x0128,
		    "cannot remap MMIO (%s), aborting\n", pci_name(ha->pdev));
		goto iospace_error_exit;
	}

	if (!(pci_resource_flags(ha->pdev, 2) & IORESOURCE_MEM)) {
		ql_log_pci(ql_log_warn, ha->pdev, 0x0129,
		    "region #2 not an MMIO resource (%s), aborting\n",
		    pci_name(ha->pdev));
		goto iospace_error_exit;
	}
	if (pci_resource_len(ha->pdev, 2) < BAR2_LEN_FX00) {
		ql_log_pci(ql_log_warn, ha->pdev, 0x012a,
		    "Invalid PCI mem BAR2 region size (%s), aborting\n",
			pci_name(ha->pdev));
		goto iospace_error_exit;
	}

	ha->iobase = ioremap_nocache(pci_resource_start(ha->pdev, 2), BAR2_LEN_FX00);
	if (!ha->iobase) {
		ql_log_pci(ql_log_fatal, ha->pdev, 0x012b,
		    "cannot remap MMIO (%s), aborting\n", pci_name(ha->pdev));
		goto iospace_error_exit;
	}

	/* Determine queue resources */
	ha->max_req_queues = ha->max_rsp_queues = 1;

	ql_log_pci(ql_log_info, ha->pdev, 0x012c,
	    "Bars 0x%x, iobase0 0x%p, iobase2 0x%p\n",
	    ha->bars, ha->cregbase, ha->iobase);

	return 0;

iospace_error_exit:
	return -ENOMEM;
}

void
qlafx00_save_queue_ptrs(struct scsi_qla_host *vha)
{
	struct qla_hw_data *ha = vha->hw;
	struct req_que *req = ha->req_q_map[0];
	struct rsp_que *rsp = ha->rsp_q_map[0];

	req->length_fx00 = req->length;
	req->ring_fx00 = req->ring;
	req->dma_fx00 = req->dma;

	rsp->length_fx00 = rsp->length;
	rsp->ring_fx00 = rsp->ring;
	rsp->dma_fx00 = rsp->dma;

	ql_dbg(ql_dbg_init, vha, 0x012d,
	    "req: %p, ring_fx00: %p, length_fx00: 0x%x,"
	    "req->dma_fx00: 0x%llx\n", req, req->ring_fx00,
	    req->length_fx00, (u64)req->dma_fx00);

	ql_dbg(ql_dbg_init, vha, 0x012e,
	    "rsp: %p, ring_fx00: %p, length_fx00: 0x%x,"
	    "rsp->dma_fx00: 0x%llx\n", rsp, rsp->ring_fx00,
	    rsp->length_fx00, (u64)rsp->dma_fx00);
}

int
qlafx00_config_queues(struct scsi_qla_host *vha)
{
	struct qla_hw_data *ha = vha->hw;
	struct req_que *req = ha->req_q_map[0];
	struct rsp_que *rsp = ha->rsp_q_map[0];
	dma_addr_t bar2_hdl = pci_resource_start(ha->pdev, 2);

	req->length = ha->req_que_len;
	req->ring = (void *)ha->iobase + ha->req_que_off;
	req->dma = bar2_hdl + ha->req_que_off;
	if ((!req->ring) || (req->length == 0)) {
		ql_log_pci(ql_log_info, ha->pdev, 0x012f,
		    "Unable to allocate memory for req_ring\n");
		return QLA_FUNCTION_FAILED;
	}

	ql_dbg(ql_dbg_init, vha, 0x0130,
	    "req: %p req_ring pointer %p req len 0x%x "
	    "req off 0x%x\n, req->dma: 0x%llx",
	    req, req->ring, req->length,
	    ha->req_que_off, (u64)req->dma);

	rsp->length = ha->rsp_que_len;
	rsp->ring = (void *)ha->iobase + ha->rsp_que_off;
	rsp->dma = bar2_hdl + ha->rsp_que_off;
	if ((!rsp->ring) || (rsp->length == 0)) {
		ql_log_pci(ql_log_info, ha->pdev, 0x0131,
		    "Unable to allocate memory for rsp_ring\n");
		return QLA_FUNCTION_FAILED;
	}

	ql_dbg(ql_dbg_init, vha, 0x0132,
	    "rsp: %p rsp_ring pointer %p rsp len 0x%x "
	    "rsp off 0x%x, rsp->dma: 0x%llx\n",
	    rsp, rsp->ring, rsp->length,
	    ha->rsp_que_off, (u64)rsp->dma);

	return QLA_SUCCESS;
}

int
qlafx00_init_fw_ready(scsi_qla_host_t *vha)
{
	int rval = 0;
	unsigned long wtime;
	uint16_t wait_time;	/* Wait time */
	struct qla_hw_data *ha = vha->hw;
	struct device_reg_fx00 __iomem *reg = &ha->iobase->ispfx00;
	uint32_t aenmbx, aenmbx7 = 0;
	uint32_t pseudo_aen;
	uint32_t state[5];
	bool done = false;

	/* 30 seconds wait - Adjust if required */
	wait_time = 30;

	pseudo_aen = RD_REG_DWORD(&reg->pseudoaen);
	if (pseudo_aen == 1) {
		aenmbx7 = RD_REG_DWORD(&reg->initval7);
		ha->mbx_intr_code = MSW(aenmbx7);
		ha->rqstq_intr_code = LSW(aenmbx7);
		rval = qlafx00_driver_shutdown(vha, 10);
		if (rval != QLA_SUCCESS)
			qlafx00_soft_reset(vha);
	}

	/* wait time before firmware ready */
	wtime = jiffies + (wait_time * HZ);
	do {
		aenmbx = RD_REG_DWORD(&reg->aenmailbox0);
		barrier();
		ql_dbg(ql_dbg_mbx, vha, 0x0133,
		    "aenmbx: 0x%x\n", aenmbx);

		switch (aenmbx) {
		case MBA_FW_NOT_STARTED:
		case MBA_FW_STARTING:
			break;

		case MBA_SYSTEM_ERR:
		case MBA_REQ_TRANSFER_ERR:
		case MBA_RSP_TRANSFER_ERR:
		case MBA_FW_INIT_FAILURE:
			qlafx00_soft_reset(vha);
			break;

		case MBA_FW_RESTART_CMPLT:
			/* Set the mbx and rqstq intr code */
			aenmbx7 = RD_REG_DWORD(&reg->aenmailbox7);
			ha->mbx_intr_code = MSW(aenmbx7);
			ha->rqstq_intr_code = LSW(aenmbx7);
			ha->req_que_off = RD_REG_DWORD(&reg->aenmailbox1);
			ha->rsp_que_off = RD_REG_DWORD(&reg->aenmailbox3);
			ha->req_que_len = RD_REG_DWORD(&reg->aenmailbox5);
			ha->rsp_que_len = RD_REG_DWORD(&reg->aenmailbox6);
			WRT_REG_DWORD(&reg->aenmailbox0, 0);
			RD_REG_DWORD_RELAXED(&reg->aenmailbox0);
			ql_dbg(ql_dbg_init, vha, 0x0134,
			    "f/w returned mbx_intr_code: 0x%x, rqstq_intr_code: 0x%x\n",
			    ha->mbx_intr_code, ha->rqstq_intr_code);
			QLAFX00_CLR_INTR_REG(ha, QLAFX00_HST_INT_STS_BITS);
			rval = QLA_SUCCESS;
			done = true;
			break;

		default:
			/* If fw is apparently not ready. In order to continue, we
			 * might need to issue Mbox cmd, but the problem is that the
			 * DoorBell vector values that come with the 8060 AEN are
			 * most likely gone by now (and thus no bell would be rung
			 * on the fw side when mbox cmd is issued). We have to
			 * therefore grab the 8060 AEN shadow regs (filled in by FW
			 * when the last 8060 AEN was being posted).
			 * Do the following to determine what is needed in order to
			 * get the FW ready:
			 * 1. reload the 8060 AEN values from the shadow regs
			 * 2. clear int status to get rid of possible pending ints
			 * 3. issue Get FW State Mbox cmd to determine fw state
			 * Set the mbx and rqstq intr code from Shadow Regs
			 */
			aenmbx7 = RD_REG_DWORD(&reg->initval7);
			ha->mbx_intr_code = MSW(aenmbx7);
			ha->rqstq_intr_code = LSW(aenmbx7);
			ha->req_que_off = RD_REG_DWORD(&reg->initval1);
			ha->rsp_que_off = RD_REG_DWORD(&reg->initval3);
			ha->req_que_len = RD_REG_DWORD(&reg->initval5);
			ha->rsp_que_len = RD_REG_DWORD(&reg->initval6);
			ql_dbg(ql_dbg_init, vha, 0x0135,
			    "f/w returned mbx_intr_code: 0x%x, rqstq_intr_code: 0x%x\n",
			    ha->mbx_intr_code, ha->rqstq_intr_code);
			QLAFX00_CLR_INTR_REG(ha, QLAFX00_HST_INT_STS_BITS);

			/* Get the FW state */
			rval = qlafx00_get_firmware_state(vha, state);
			if (rval != QLA_SUCCESS) {
				/* Retry if timer has not expired */
				break;
			}

			if (state[0] == FSTATE_FX00_CONFIG_WAIT) {
				/* Firmware is waiting to be initialized by driver */
				rval = QLA_SUCCESS;
				done = true;
				break;
			}

			/* Issue driver shutdown and wait until f/w recovers.
			 * Driver should continue to poll until 8060 AEN is received
			 * indicating firmware recovery.
			 */
			ql_dbg(ql_dbg_init, vha, 0x0136,
			    "Sending Driver shutdown fw_state 0x%x \n",
			    state[0]);

			rval = qlafx00_driver_shutdown(vha, 10);
			if (rval != QLA_SUCCESS) {
				rval = QLA_FUNCTION_FAILED;
				break;
			}
			msleep(500);

			wtime = jiffies + (wait_time * HZ);
			break;
		}

		if (!done) {
			if (time_after_eq(jiffies, wtime)) {
				ql_dbg(ql_dbg_init, vha, 0x0137,
				    "Init f/w failed: aen[7]: 0x%x\n",
				    RD_REG_DWORD(&reg->aenmailbox7));
				rval = QLA_FUNCTION_FAILED;
				done = true;
				break;
			}
			/* Delay for a while */
			msleep(500);
		}
	} while (!done);

	if (rval)
		ql_dbg(ql_dbg_init, vha, 0x0138,
		    "%s **** FAILED ****.\n", __func__);
	else
		ql_dbg(ql_dbg_init, vha, 0x0139,
		    "%s **** SUCCESS ****.\n", __func__);

	return rval;
}

/*
 * qlafx00_fw_ready() - Waits for firmware ready.
 * @ha: HA context
 *
 * Returns 0 on success.
 */
int
qlafx00_fw_ready(scsi_qla_host_t *vha)
{
	int		rval;
	unsigned long	wtime;
	uint16_t	wait_time;	/* Wait time if loop is coming ready */
	uint32_t	state[5];

	rval = QLA_SUCCESS;

	wait_time = 10;

	/* wait time before firmware ready */
	wtime = jiffies + (wait_time * HZ);

	/* Wait for ISP to finish init */
	if (!vha->flags.init_done)
		ql_dbg(ql_dbg_init, vha, 0x013a,
		    "Waiting for init to complete...\n");

	do {
		rval = qlafx00_get_firmware_state(vha, state);

		if (rval == QLA_SUCCESS) {
			if (state[0] == FSTATE_FX00_INITIALIZED) {
				ql_dbg(ql_dbg_init, vha, 0x013b,
				    "fw_state=%x\n", state[0]);
				rval = QLA_SUCCESS;
					break;
			}
		}
		rval = QLA_FUNCTION_FAILED;

		if (time_after_eq(jiffies, wtime))
			break;

		/* Delay for a while */
		msleep(500);

		ql_dbg(ql_dbg_init, vha, 0x013c,
		    "fw_state=%x curr time=%lx.\n", state[0], jiffies);
	} while (1);


	if (rval)
		ql_dbg(ql_dbg_init, vha, 0x013d,
		    "Firmware ready **** FAILED ****.\n");
	else
		ql_dbg(ql_dbg_init, vha, 0x013e,
		    "Firmware ready **** SUCCESS ****.\n");

	return rval;
}

static int
qlafx00_find_all_targets(scsi_qla_host_t *vha,
	struct list_head *new_fcports)
{
	int		rval;
	uint16_t	tgt_id;
	fc_port_t	*fcport, *new_fcport;
	int		found;
	struct qla_hw_data *ha = vha->hw;

	rval = QLA_SUCCESS;

	if (!test_bit(LOOP_RESYNC_ACTIVE, &vha->dpc_flags))
		return QLA_FUNCTION_FAILED;

	if ((atomic_read(&vha->loop_down_timer) ||
	     STATE_TRANSITION(vha))) {
		atomic_set(&vha->loop_down_timer, 0);
		set_bit(LOOP_RESYNC_NEEDED, &vha->dpc_flags);
		return QLA_FUNCTION_FAILED;
	}

	ql_dbg(ql_dbg_disc + ql_dbg_init, vha, 0x2088,
	    "Listing Target bit map...\n");
	ql_dump_buffer(ql_dbg_disc + ql_dbg_init, vha,
	    0x2089, (uint8_t *)ha->gid_list, 32);

	/* Allocate temporary rmtport for any new rmtports discovered. */
	new_fcport = qla2x00_alloc_fcport(vha, GFP_KERNEL);
	if (new_fcport == NULL)
		return QLA_MEMORY_ALLOC_FAILED;

	for_each_bit(tgt_id, (void *)ha->gid_list,  QLAFX00_TGT_NODE_LIST_SIZE) {

		/* Send get target node info */
		new_fcport->tgt_id = tgt_id;
		rval = qlafx00_fx_disc(vha, new_fcport,
		    FXDISC_GET_TGT_NODE_INFO);
		if (rval != QLA_SUCCESS) {
			ql_log(ql_log_warn, vha, 0x208a,
			    "Target info scan failed -- assuming zero-entry "
			    "result...\n");
			continue;
		}

		/* Locate matching device in database. */
		found = 0;
		list_for_each_entry(fcport, &vha->vp_fcports, list) {
			if (memcmp(new_fcport->port_name,
			    fcport->port_name, WWN_SIZE))
				continue;

			found++;

			/*
			 * If tgt_id is same and state FCS_ONLINE, nothing
			 * changed.
			 */
			if (fcport->tgt_id == new_fcport->tgt_id &&
			    atomic_read(&fcport->state) == FCS_ONLINE)
				break;

			/*
			 * Tgt ID changed or device was marked to be updated.
			 */
			ql_dbg(ql_dbg_disc + ql_dbg_init, vha, 0x208b,
			    "TGT-ID Change(%s): Present tgt id: 0x%x state: 0x%x "
			    "wwnn = %llx wwpn = %llx.\n",
			    __func__, fcport->tgt_id,
			    atomic_read(&fcport->state),
			    (unsigned long long)wwn_to_u64(fcport->node_name),
			    (unsigned long long)wwn_to_u64(fcport->port_name));

			ql_log(ql_log_info, vha, 0x208c,
			    "TGT-ID Announce(%s): Discovered tgt id 0x%x wwnn = %llx "
			    "wwpn = %llx.\n", __func__, new_fcport->tgt_id,
			    (unsigned long long)wwn_to_u64(new_fcport->node_name),
			    (unsigned long long)wwn_to_u64(new_fcport->port_name));

			if (atomic_read(&fcport->state) != FCS_ONLINE) {
				fcport->old_tgt_id = fcport->tgt_id;
				fcport->tgt_id = new_fcport->tgt_id;
				ql_log(ql_log_info, vha, 0x208d,
				   "TGT-ID: New fcport Added: %p\n", fcport);
				qla2x00_update_fcport(vha, fcport);
			} else {
				ql_log(ql_log_info, vha, 0x208e,
				    " Existing TGT-ID %x did not get "
				    " offline event from firmware.\n",
				    fcport->old_tgt_id);
				qla2x00_mark_device_lost(vha, fcport, 0, 0);
				set_bit(LOOP_RESYNC_NEEDED, &vha->dpc_flags);
				kfree(new_fcport);
				return rval;
			}
			break;
		}

		if (found)
			continue;

		/* If device was not in our fcports list, then add it. */
		list_add_tail(&new_fcport->list, new_fcports);

		/* Allocate a new replacement fcport. */
		new_fcport = qla2x00_alloc_fcport(vha, GFP_KERNEL);
		if (new_fcport == NULL)
			return QLA_MEMORY_ALLOC_FAILED;
	}

	kfree(new_fcport);
	return rval;
}

/*
 * qlafx00_configure_all_targets
 *      Setup target devices with node ID's.
 *
 * Input:
 *      ha = adapter block pointer.
 *
 * Returns:
 *      0 = success.
 *      BIT_0 = error
 */
int
qlafx00_configure_all_targets(scsi_qla_host_t *vha)
{
	int rval;
	fc_port_t *fcport, *rmptemp;
	LIST_HEAD(new_fcports);

	rval = qlafx00_fx_disc(vha, &vha->hw->mr.fcport, FXDISC_GET_TGT_NODE_LIST);
	if (rval != QLA_SUCCESS) {
		set_bit(LOOP_RESYNC_NEEDED, &vha->dpc_flags);
		return rval;
	}

	rval = qlafx00_find_all_targets(vha, &new_fcports);
	if (rval != QLA_SUCCESS) {
		set_bit(LOOP_RESYNC_NEEDED, &vha->dpc_flags);
		return rval;
	}

	/*
	 * Delete all previous devices marked lost.
	 */
	list_for_each_entry(fcport, &vha->vp_fcports, list) {
		if (test_bit(LOOP_RESYNC_NEEDED, &vha->dpc_flags))
			break;

		if (atomic_read(&fcport->state) == FCS_DEVICE_LOST) {
			if (fcport->port_type != FCT_INITIATOR)
				qla2x00_mark_device_lost(vha, fcport, 0, 0);
		}
	}

	/*
	 * Add the new devices to our devices list.
	 */
	list_for_each_entry_safe(fcport, rmptemp, &new_fcports, list) {
		if (test_bit(LOOP_RESYNC_NEEDED, &vha->dpc_flags))
			break;

		qla2x00_update_fcport(vha, fcport);
		list_move_tail(&fcport->list, &vha->vp_fcports);
		ql_log(ql_log_info, vha, 0x208f,
		    "Attach new target id 0x%x wwnn = %llx "
		    "wwpn = %llx.\n",
		    fcport->tgt_id,
		    (unsigned long long)wwn_to_u64(fcport->node_name),
		    (unsigned long long)wwn_to_u64(fcport->port_name));
	}

	/* Free all new device structures not processed. */
	list_for_each_entry_safe(fcport, rmptemp, &new_fcports, list) {
		list_del(&fcport->list);
		kfree(fcport);
	}

	return rval;
}

/*
 * qlafx00_configure_devices
 *      Updates Fibre Channel Device Database with what is actually on loop.
 *
 * Input:
 *      ha                = adapter block pointer.
 *
 * Returns:
 *      0 = success.
 *      1 = error.
 *      2 = database was full and device was not configured.
 */
int
qlafx00_configure_devices(scsi_qla_host_t *vha)
{
	int  rval;
	unsigned long flags, save_flags;
	rval = QLA_SUCCESS;

	save_flags = flags = vha->dpc_flags;

	ql_dbg(ql_dbg_disc, vha, 0x2090,
	    "Configure devices -- dpc flags =0x%lx\n", flags);

	rval = qlafx00_configure_all_targets(vha);

	if (rval == QLA_SUCCESS) {
		if (test_bit(LOOP_RESYNC_NEEDED, &vha->dpc_flags)) {
			rval = QLA_FUNCTION_FAILED;
		} else {
			atomic_set(&vha->loop_state, LOOP_READY);
			ql_log(ql_log_info, vha, 0x2091,
			    "Device Ready\n");
		}
	}

	if (rval) {
		ql_dbg(ql_dbg_disc, vha, 0x2092,
		    "%s *** FAILED ***.\n", __func__);
	} else {
		ql_dbg(ql_dbg_disc, vha, 0x2093,
		    "%s: exiting normally.\n", __func__);
	}
	return rval;
}

void
qlafx00_abort_isp_cleanup(scsi_qla_host_t *vha, bool critemp)
{
	struct qla_hw_data *ha = vha->hw;
	fc_port_t *fcport;

	vha->flags.online = 0;
	ha->mr.fw_hbt_en = 0;
	if (!critemp) {
		ha->flags.chip_reset_done = 0;
		clear_bit(ISP_ABORT_NEEDED, &vha->dpc_flags);
		vha->qla_stats.total_isp_aborts++;
		ql_log(ql_log_info, vha, 0x013f,
		    "Performing ISP error recovery - ha = %p.\n", ha);
		ha->isp_ops->reset_chip(vha);
	}

	if (atomic_read(&vha->loop_state) != LOOP_DOWN) {
		atomic_set(&vha->loop_state, LOOP_DOWN);
		atomic_set(&vha->loop_down_timer,
		    QLAFX00_LOOP_DOWN_TIME);
	} else {
		if (!atomic_read(&vha->loop_down_timer))
			atomic_set(&vha->loop_down_timer,
			    QLAFX00_LOOP_DOWN_TIME);
	}

	/* Clear all async request states across all VPs. */
	list_for_each_entry(fcport, &vha->vp_fcports, list) {
		fcport->flags = 0;
		if (atomic_read(&fcport->state) == FCS_ONLINE)
			qla2x00_set_fcport_state(fcport, FCS_DEVICE_LOST);
	}

	if (!ha->flags.eeh_busy) {
		if (critemp) {
			qla2x00_abort_all_cmds(vha, DID_NO_CONNECT << 16);
		} else {
			/* Requeue all commands in outstanding command list. */
			qla2x00_abort_all_cmds(vha, DID_RESET << 16);
		}
	}

	qla2x00_free_irqs(vha);
	if (critemp)
		set_bit(FX00_CRITEMP_RECOVERY, &vha->dpc_flags);
	else
		set_bit(FX00_RESET_RECOVERY, &vha->dpc_flags);

	/* Clear the Interrupts */
	QLAFX00_CLR_INTR_REG(ha, QLAFX00_HST_INT_STS_BITS);

	ql_log(ql_log_info, vha, 0x0140,
	    "%s Done done - ha=%p.\n", __func__, ha);
}

/**
 * qlafx00_init_response_q_entries() - Initializes response queue entries.
 * @ha: HA context
 *
 * Beginning of request ring has initialization control block already built
 * by nvram config routine.
 *
 * Returns 0 on success.
 */
void
qlafx00_init_response_q_entries(struct rsp_que *rsp)
{
	uint16_t cnt;
	response_t *pkt;

	rsp->ring_ptr = rsp->ring;
	rsp->ring_index    = 0;
	rsp->status_srb = NULL;
	pkt = rsp->ring_ptr;
	for (cnt = 0; cnt < rsp->length; cnt++) {
		pkt->signature = RESPONSE_PROCESSED;
		WRT_REG_DWORD(&pkt->signature, RESPONSE_PROCESSED);
		pkt++;
	}
}

int
qlafx00_rescan_isp(scsi_qla_host_t *vha)
{
	uint32_t status = QLA_FUNCTION_FAILED;
	struct qla_hw_data *ha = vha->hw;
	struct device_reg_fx00 __iomem *reg = &ha->iobase->ispfx00;
	uint32_t aenmbx7;

	qla2x00_request_irqs(ha, ha->rsp_q_map[0]);

	aenmbx7 = RD_REG_DWORD(&reg->aenmailbox7);
	ha->mbx_intr_code = MSW(aenmbx7);
	ha->rqstq_intr_code = LSW(aenmbx7);
	ha->req_que_off = RD_REG_DWORD(&reg->aenmailbox1);
	ha->rsp_que_off = RD_REG_DWORD(&reg->aenmailbox3);
	ha->req_que_len = RD_REG_DWORD(&reg->aenmailbox5);
	ha->rsp_que_len = RD_REG_DWORD(&reg->aenmailbox6);

	ql_dbg(ql_dbg_disc, vha, 0x2097,
	    "fw returned mbx_intr_code: 0x%x, rqstq_intr_code: 0x%x "
	    " Req que offset 0x%x Rsp que offset 0x%x\n",
	    ha->mbx_intr_code, ha->rqstq_intr_code,
	    ha->req_que_off, ha->rsp_que_len);

	/* Clear the Interrupts */
	QLAFX00_CLR_INTR_REG(ha, QLAFX00_HST_INT_STS_BITS);

	status = qla2x00_init_rings(vha);
	if (!status) {
		vha->flags.online = 1;

		/* if no cable then assume it's good */
		if ((vha->device_flags & DFLG_NO_CABLE))
			status = 0;
		/* Register system information */
		if (qlafx00_fx_disc(vha,
		    &vha->hw->mr.fcport, FXDISC_REG_HOST_INFO))
			ql_dbg(ql_dbg_disc, vha, 0x2098,
			    "failed to register host info\n");
	}
	scsi_unblock_requests(vha->host);
	return status;
}

void
qlafx00_timer_routine(scsi_qla_host_t *vha)
{
	struct qla_hw_data *ha = vha->hw;
	uint32_t fw_heart_beat;
	uint32_t aenmbx0;
	struct device_reg_fx00 __iomem *reg = &ha->iobase->ispfx00;
	uint32_t tempc;

	/* Check firmware health */
	if (ha->mr.fw_hbt_cnt)
		ha->mr.fw_hbt_cnt--;
	else {
		if ((!ha->flags.mr_reset_hdlr_active) &&
		    (!test_bit(UNLOADING, &vha->dpc_flags)) &&
		    (!test_bit(ABORT_ISP_ACTIVE, &vha->dpc_flags)) &&
		    (ha->mr.fw_hbt_en)) {
			fw_heart_beat = RD_REG_DWORD(&reg->fwheartbeat);
			if (fw_heart_beat != ha->mr.old_fw_hbt_cnt) {
				ha->mr.old_fw_hbt_cnt = fw_heart_beat;
				ha->mr.fw_hbt_miss_cnt = 0;
			} else {
				ha->mr.fw_hbt_miss_cnt++;
				if (ha->mr.fw_hbt_miss_cnt ==
				    QLAFX00_HEARTBEAT_MISS_CNT) {
					set_bit(ISP_ABORT_NEEDED,
					    &vha->dpc_flags);
					qla2xxx_wake_dpc(vha);
					ha->mr.fw_hbt_miss_cnt = 0;
				}
			}
		}
		ha->mr.fw_hbt_cnt = QLAFX00_HEARTBEAT_INTERVAL;
	}

	if (test_bit(FX00_RESET_RECOVERY, &vha->dpc_flags)) {
		/* Reset recovery to be performed in timer routine */
		aenmbx0 = RD_REG_DWORD(&reg->aenmailbox0);
		if (ha->mr.fw_reset_timer_exp) {
			set_bit(ISP_ABORT_NEEDED, &vha->dpc_flags);
			qla2xxx_wake_dpc(vha);
			ha->mr.fw_reset_timer_exp = 0;
		} else if (aenmbx0 == MBA_FW_RESTART_CMPLT) {
			/* Wake up DPC to rescan the targets */
			set_bit(FX00_TARGET_SCAN, &vha->dpc_flags);
			clear_bit(FX00_RESET_RECOVERY, &vha->dpc_flags);
			qla2xxx_wake_dpc(vha);
			ha->mr.fw_reset_timer_tick = QLAFX00_RESET_INTERVAL;
		} else if ((aenmbx0 == MBA_FW_STARTING) &&
		    (!ha->mr.fw_hbt_en)) {
			ha->mr.fw_hbt_en = 1;
		} else if (!ha->mr.fw_reset_timer_tick) {
			if (aenmbx0 == ha->mr.old_aenmbx0_state)
				ha->mr.fw_reset_timer_exp = 1;
			ha->mr.fw_reset_timer_tick = QLAFX00_RESET_INTERVAL;
		} else if (aenmbx0 == 0xFFFFFFFF) {
			uint32_t data0, data1;

			data0 = QLAFX00_RD_REG(ha,
			    QLAFX00_BAR1_BASE_ADDR_REG);
			data1 = QLAFX00_RD_REG(ha,
			    QLAFX00_PEX0_WIN0_BASE_ADDR_REG);

			data0 &= 0xffff0000;
			data1 &= 0x0000ffff;

			QLAFX00_WR_REG(ha,
			    QLAFX00_PEX0_WIN0_BASE_ADDR_REG,
			    (data0 | data1));
		} else if ((aenmbx0 & 0xFF00) == MBA_FW_POLL_STATE) {
			ha->mr.fw_reset_timer_tick =
			    QLAFX00_MAX_RESET_INTERVAL;
		} else if (aenmbx0 == MBA_FW_RESET_FCT) {
			ha->mr.fw_reset_timer_tick =
			    QLAFX00_MAX_RESET_INTERVAL;
		}
		ha->mr.old_aenmbx0_state = aenmbx0;
		ha->mr.fw_reset_timer_tick--;
	}
	if (test_bit(FX00_CRITEMP_RECOVERY, &vha->dpc_flags)) {
		/*
		 * Critical temperature recovery to be
		 * performed in timer routine
		 */
		if (ha->mr.fw_critemp_timer_tick == 0) {
			tempc = QLAFX00_GET_TEMPERATURE(ha);
			ql_dbg(ql_dbg_timer, vha, 0x6012,
			    "ISPFx00(%s): Critical temp timer, "
			    "current SOC temperature: %d\n",
			    __func__, tempc);
			if (tempc < ha->mr.critical_temperature) {
				set_bit(ISP_ABORT_NEEDED, &vha->dpc_flags);
				clear_bit(FX00_CRITEMP_RECOVERY,
				    &vha->dpc_flags);
				qla2xxx_wake_dpc(vha);
			}
			ha->mr.fw_critemp_timer_tick =
			    QLAFX00_CRITEMP_INTERVAL;
		} else {
			ha->mr.fw_critemp_timer_tick--;
		}
	}
	if (ha->mr.host_info_resend) {
		/*
		 * Incomplete host info might be sent to firmware
		 * durinng system boot - info should be resend
		 */
		if (ha->mr.hinfo_resend_timer_tick == 0) {
			ha->mr.host_info_resend = false;
			set_bit(FX00_HOST_INFO_RESEND, &vha->dpc_flags);
			ha->mr.hinfo_resend_timer_tick =
			    QLAFX00_HINFO_RESEND_INTERVAL;
			qla2xxx_wake_dpc(vha);
		} else {
			ha->mr.hinfo_resend_timer_tick--;
		}
	}

}

/*
 *  qlfx00a_reset_initialize
 *      Re-initialize after a iSA device reset.
 *
 * Input:
 *      ha  = adapter block pointer.
 *
 * Returns:
 *      0 = success
 */
int
qlafx00_reset_initialize(scsi_qla_host_t *vha)
{
	struct qla_hw_data *ha = vha->hw;

	if (vha->device_flags & DFLG_DEV_FAILED) {
		ql_dbg(ql_dbg_init, vha, 0x0142,
		    "Device in failed state\n");
		return QLA_SUCCESS;
	}

	ha->flags.mr_reset_hdlr_active = 1;

	if (vha->flags.online) {
		scsi_block_requests(vha->host);
		qlafx00_abort_isp_cleanup(vha, false);
	}

	ql_log(ql_log_info, vha, 0x0143,
	    "(%s): succeeded.\n", __func__);
	ha->flags.mr_reset_hdlr_active = 0;
	return QLA_SUCCESS;
}

/*
 *  qlafx00_abort_isp
 *      Resets ISP and aborts all outstanding commands.
 *
 * Input:
 *      ha  = adapter block pointer.
 *
 * Returns:
 *      0 = success
 */
int
qlafx00_abort_isp(scsi_qla_host_t *vha)
{
	struct qla_hw_data *ha = vha->hw;

	if (vha->flags.online) {
		if (unlikely(pci_channel_offline(ha->pdev) &&
		    ha->flags.pci_channel_io_perm_failure)) {
			clear_bit(ISP_ABORT_RETRY, &vha->dpc_flags);
			return QLA_SUCCESS;
		}

		scsi_block_requests(vha->host);
		qlafx00_abort_isp_cleanup(vha, false);
	} else {
		scsi_block_requests(vha->host);
		clear_bit(ISP_ABORT_NEEDED, &vha->dpc_flags);
		vha->qla_stats.total_isp_aborts++;
		ha->isp_ops->reset_chip(vha);
		set_bit(FX00_RESET_RECOVERY, &vha->dpc_flags);
		/* Clear the Interrupts */
		QLAFX00_CLR_INTR_REG(ha, QLAFX00_HST_INT_STS_BITS);
	}

	ql_log(ql_log_info, vha, 0x0145,
	    "(%s): succeeded.\n", __func__);

	return QLA_SUCCESS;
}

static inline fc_port_t	*
qlafx00_get_fcport(struct scsi_qla_host *vha, int tgt_id)
{
	fc_port_t	*fcport;

	/* Check for matching device in remote port list. */
	fcport = NULL;
	list_for_each_entry(fcport, &vha->vp_fcports, list) {
		if (fcport->tgt_id == tgt_id) {
			ql_dbg(ql_dbg_async, vha, 0x5072,
			    "Matching fcport(%p) found with TGT-ID: 0x%x "
			    "and Remote TGT_ID: 0x%x\n",
			    fcport, fcport->tgt_id, tgt_id);
			break;
		}
	}
	return fcport;
}

static void
qlafx00_tgt_detach(struct scsi_qla_host *vha, int tgt_id)
{
	fc_port_t	*fcport;

	ql_log(ql_log_info, vha, 0x5073,
	    "Detach TGT-ID: 0x%x\n", tgt_id);

	fcport = qlafx00_get_fcport(vha, tgt_id);
	if (!fcport)
		return;

	qla2x00_mark_device_lost(vha, fcport, 0, 0);

	return;
}

/*
 * qlafx00_find_target
 *
 * Input:
 *	ha = adapter block pointer.
 *	dev = database device entry pointer.
 *
 * Returns:
 *	0 = success.
 *
 * Context:
 *	Kernel context.
 */
static int
qlafx00_find_target(scsi_qla_host_t *vha,
	struct list_head *new_fcports, uint32_t	tgt_id)
{
	int		rval;
	fc_port_t	*fcport, *new_fcport;
	int		found;

	rval = QLA_SUCCESS;

	if (test_bit(LOOP_RESYNC_NEEDED, &vha->dpc_flags)) {
		return rval;
	}

	/* Allocate temporary rmtport for any new rmtports discovered. */
	new_fcport = qla2x00_alloc_fcport(vha, GFP_KERNEL);
	if (new_fcport == NULL) {
		return QLA_MEMORY_ALLOC_FAILED;
	}

	if (STATE_TRANSITION(vha)) {
		kfree(new_fcport);
		return rval;
	}

	/* Send get target node info */
	new_fcport->tgt_id = tgt_id;
	rval = qlafx00_fx_disc(vha, new_fcport, FXDISC_GET_TGT_NODE_INFO);
	if (rval != QLA_SUCCESS) {
		kfree(new_fcport);
		return rval;
	}
	/* Locate matching device in database. */
	found = 0;
	list_for_each_entry(fcport, &vha->vp_fcports, list) {
		if (memcmp(new_fcport->port_name, fcport->port_name,
		    WWN_SIZE)) {
			continue;
		}
		found++;

		/*
		 * If address the same and state FCS_ONLINE, nothing
		 * changed.
		 */
		if (fcport->tgt_id == new_fcport->tgt_id &&
		    atomic_read(&fcport->state) == FCS_ONLINE) {
			break;
		}

		/*
		 * Tgt ID changed or device was marked to be updated.
		 */
		ql_log(ql_log_info, vha, 0x2094,
		    "TGT-ID Change(%s): Present tgt id: 0x%x state: 0x%x "
		    "wwnn = %llx wwpn = %llx.\n",
		    __func__, fcport->tgt_id, atomic_read(&fcport->state),
		    (unsigned long long)wwn_to_u64(fcport->node_name),
		    (unsigned long long)wwn_to_u64(fcport->port_name));
		ql_log(ql_log_info, vha, 0x2095,
		    "TGT-ID Announce(%s): Discovered tgt id 0x%x wwnn = %llx "
		    "wwpn = %llx.\n", __func__, new_fcport->tgt_id,
		    (unsigned long long)wwn_to_u64(new_fcport->node_name),
		    (unsigned long long)wwn_to_u64(new_fcport->port_name));

		if (atomic_read(&fcport->state) != FCS_ONLINE) {
			fcport->old_tgt_id = fcport->tgt_id;
			fcport->tgt_id = new_fcport->tgt_id;
			qla2x00_update_fcport(vha, fcport);
		} else {
			qla2x00_mark_device_lost(vha, fcport, 0, 0);
			set_bit(LOOP_RESYNC_NEEDED, &vha->dpc_flags);
			kfree(new_fcport);
			return rval;
		}

		break;
	}

	if (found) {
		kfree(new_fcport);
		return rval;
	}
	/* If device was not in our fcports list, then add it. */
	list_add_tail(&new_fcport->list, new_fcports);

	return rval;
}

int
qlafx00_configure_target(struct scsi_qla_host *vha, uint32_t tgtidx)
{
	int	rval;
	fc_port_t	*fcport, *rmptemp;
	LIST_HEAD(new_fcports);

	rval = qlafx00_find_target(vha, &new_fcports, tgtidx);
	if (rval != QLA_SUCCESS) {
		set_bit(LOOP_RESYNC_NEEDED, &vha->dpc_flags);
		return rval;
	} else if (atomic_read(&vha->loop_down_timer) ||
		    test_bit(LOOP_RESYNC_NEEDED, &vha->dpc_flags)) {
		return rval;
	}

	/*
	 * Add the new devices to our devices list.
	 */
	list_for_each_entry_safe(fcport, rmptemp, &new_fcports, list) {
		if (test_bit(LOOP_RESYNC_NEEDED, &vha->dpc_flags))
			break;

		qla2x00_update_fcport(vha, fcport);
		list_move_tail(&fcport->list, &vha->vp_fcports);
		ql_log(ql_log_info, vha, 0x2096,
		    "Attach new target id 0x%x wwnn = %llx "
		    "wwpn = %llx for fcport: %p.\n", fcport->tgt_id,
		    (unsigned long long)wwn_to_u64(fcport->node_name),
		    (unsigned long long)wwn_to_u64(fcport->port_name), fcport);
	}

	/* Free all new device structures not processed. */
	list_for_each_entry_safe(fcport, rmptemp, &new_fcports, list) {
		list_del(&fcport->list);
		kfree(fcport);
	}
	return rval;
}

int
qlafx00_process_aen(struct scsi_qla_host *vha, struct qla_work_evt *evt)
{
	int rval = 0;
	uint32_t aen_code, aen_data;

	aen_code = FCH_EVT_VENDOR_UNIQUE;
	aen_data = evt->u.aenfx.evtcode;

	switch (evt->u.aenfx.evtcode) {
	case QLAFX00_MBA_PORT_UPDATE:		/* Port database update */
		if (evt->u.aenfx.mbx[1] == 0) {
			if (evt->u.aenfx.mbx[2] == 1) {
				if (!vha->flags.fw_tgt_reported)
					vha->flags.fw_tgt_reported = 1;
				atomic_set(&vha->loop_down_timer, 0);
				atomic_set(&vha->loop_state, LOOP_UP);
				set_bit(LOOP_RESYNC_NEEDED, &vha->dpc_flags);
				qla2xxx_wake_dpc(vha);
			} else if (evt->u.aenfx.mbx[2] == 2) {
				qlafx00_tgt_detach(vha, evt->u.aenfx.mbx[3]);
			}
		} else if (evt->u.aenfx.mbx[1] == 0xffff) {
			if (evt->u.aenfx.mbx[2] == 1) {
				if (!vha->flags.fw_tgt_reported)
					vha->flags.fw_tgt_reported = 1;
				set_bit(LOOP_RESYNC_NEEDED, &vha->dpc_flags);
			} else if (evt->u.aenfx.mbx[2] == 2) {
				vha->device_flags |= DFLG_NO_CABLE;
				qla2x00_mark_all_devices_lost(vha, 1);
			}
		}
		break;
	case QLAFX00_MBA_LINK_UP:
		aen_code = FCH_EVT_LINKUP;
		aen_data = 0;
		break;
	case QLAFX00_MBA_LINK_DOWN:
		aen_code = FCH_EVT_LINKDOWN;
		aen_data = 0;
		break;
	case QLAFX00_MBA_TEMP_CRIT:	/* Critical temperature event */
		ql_log(ql_log_info, vha, 0x5083,
		    "Process critical temperature event "
		    "aenmb[0]: %x\n",
		    evt->u.aenfx.evtcode);
		scsi_block_requests(vha->host);
		qlafx00_abort_isp_cleanup(vha, true);
		scsi_unblock_requests(vha->host);
		break;
	}

	fc_host_post_event(vha->host, fc_get_event_number(),
	    aen_code, aen_data);

	return rval;
}

static void
qlafx00_update_host_attr(scsi_qla_host_t *vha, port_info_data_t *pinfo)
{
	u64 port_name = 0, node_name = 0;

	port_name = (unsigned long long)wwn_to_u64(pinfo->port_name);
	node_name = (unsigned long long)wwn_to_u64(pinfo->node_name);

	fc_host_node_name(vha->host) = node_name;
	fc_host_port_name(vha->host) = port_name;
	if (!pinfo->port_type)
		vha->hw->current_topology = ISP_CFG_F;
	if (pinfo->link_status == QLAFX00_LINK_STATUS_UP)
		atomic_set(&vha->loop_state, LOOP_READY);
	else if (pinfo->link_status == QLAFX00_LINK_STATUS_DOWN)
		atomic_set(&vha->loop_state, LOOP_DOWN);
	vha->hw->link_data_rate = (uint16_t)pinfo->link_config;
}

static void
qla2x00_fxdisc_iocb_timeout(void *data)
{
	srb_t *sp = (srb_t *)data;
	struct srb_iocb *lio = &sp->u.iocb_cmd;

	complete(&lio->u.fxiocb.fxiocb_comp);
}

static void
qla2x00_fxdisc_sp_done(void *data, void *ptr, int res)
{
	srb_t *sp = (srb_t *)ptr;
	struct srb_iocb *lio = &sp->u.iocb_cmd;

	complete(&lio->u.fxiocb.fxiocb_comp);
}

int
qlafx00_fx_disc(scsi_qla_host_t *vha, fc_port_t *fcport, uint8_t fx_type)
{
	srb_t *sp;
	struct srb_iocb *fdisc;
	int rval = QLA_FUNCTION_FAILED;
	struct qla_hw_data *ha = vha->hw;
	struct host_system_info *phost_info;
	struct register_host_info *preg_hsi;
	struct new_utsname *p_sysid = NULL;
	struct timeval tv;

	sp = qla2x00_get_sp(vha, fcport, GFP_KERNEL);
	if (!sp)
		goto done;

	fdisc = &sp->u.iocb_cmd;
	switch (fx_type) {
	case FXDISC_GET_CONFIG_INFO:
	    fdisc->u.fxiocb.flags =
		SRB_FXDISC_RESP_DMA_VALID;
	    fdisc->u.fxiocb.rsp_len = sizeof(config_info_data_t);
	    break;
	case FXDISC_GET_PORT_INFO:
	    fdisc->u.fxiocb.flags =
		SRB_FXDISC_RESP_DMA_VALID | SRB_FXDISC_REQ_DWRD_VALID;
	    fdisc->u.fxiocb.rsp_len = QLAFX00_PORT_DATA_INFO;
	    fdisc->u.fxiocb.req_data = fcport->port_id;
	    break;
	case FXDISC_GET_TGT_NODE_INFO:
	    fdisc->u.fxiocb.flags =
		SRB_FXDISC_RESP_DMA_VALID | SRB_FXDISC_REQ_DWRD_VALID;
	    fdisc->u.fxiocb.rsp_len = QLAFX00_TGT_NODE_INFO;
	    fdisc->u.fxiocb.req_data = fcport->tgt_id;
	    break;
	case FXDISC_GET_TGT_NODE_LIST:
	    fdisc->u.fxiocb.flags =
		SRB_FXDISC_RESP_DMA_VALID | SRB_FXDISC_REQ_DWRD_VALID;
	    fdisc->u.fxiocb.rsp_len = QLAFX00_TGT_NODE_LIST_SIZE;
	    break;
	case FXDISC_REG_HOST_INFO:
	    fdisc->u.fxiocb.flags = SRB_FXDISC_REQ_DMA_VALID;
	    fdisc->u.fxiocb.req_len = sizeof(struct register_host_info);
	    p_sysid = utsname();
	    if (!p_sysid) {
		    ql_log(ql_log_warn, vha, 0x303c,
			"Not able to get the system informtion\n");
		    goto done_free_sp;
	    }
	    break;
	default:
	    break;
	}

	if (fdisc->u.fxiocb.flags & SRB_FXDISC_REQ_DMA_VALID) {
		fdisc->u.fxiocb.req_addr = dma_alloc_coherent(&ha->pdev->dev,
		    fdisc->u.fxiocb.req_len,
		    &fdisc->u.fxiocb.req_dma_handle, GFP_KERNEL);
		if (!fdisc->u.fxiocb.req_addr) {
			goto done_free_sp;
		}

		if (fx_type == FXDISC_REG_HOST_INFO) {
			preg_hsi = (struct register_host_info *)
				fdisc->u.fxiocb.req_addr;
			phost_info = &preg_hsi->hsi;
			memset(preg_hsi, 0, sizeof(struct register_host_info));
			phost_info->os_type = OS_TYPE_LINUX;
			strncpy(phost_info->sysname,
			    p_sysid->sysname, SYSNAME_LENGTH);
			strncpy(phost_info->nodename,
			    p_sysid->nodename, NODENAME_LENGTH);
			if (!strcmp(phost_info->nodename, "(none)"))
				ha->mr.host_info_resend = true;
			strncpy(phost_info->release,
			    p_sysid->release, RELEASE_LENGTH);
			strncpy(phost_info->version,
			    p_sysid->version, VERSION_LENGTH);
			strncpy(phost_info->machine,
			    p_sysid->machine, MACHINE_LENGTH);
			strncpy(phost_info->domainname,
			    p_sysid->domainname, DOMNAME_LENGTH);
			strncpy(phost_info->hostdriver,
			    QLA2XXX_VERSION, VERSION_LENGTH);
			do_gettimeofday(&tv);
			preg_hsi->utc = (uint64_t)tv.tv_sec;
			ql_dbg(ql_dbg_init, vha, 0x0149,
			    "ISP%04X: Host registration with firmware\n",
			    ha->pdev->device);
			ql_dbg(ql_dbg_init, vha, 0x014a,
			    "os_type = '%d', sysname = '%s', nodname = '%s'\n",
			    phost_info->os_type,
			    phost_info->sysname,
			    phost_info->nodename);
			ql_dbg(ql_dbg_init, vha, 0x014b,
			    "release = '%s', version = '%s'\n",
			    phost_info->release,
			    phost_info->version);
			ql_dbg(ql_dbg_init, vha, 0x014c,
			    "machine = '%s' "
			    "domainname = '%s', hostdriver = '%s'\n",
			    phost_info->machine,
			    phost_info->domainname,
			    phost_info->hostdriver);
			ql_dump_buffer(ql_dbg_init + ql_dbg_disc, vha, 0x014d,
			    (uint8_t *)phost_info, sizeof(host_system_info_t));
		}
	}

	if (fdisc->u.fxiocb.flags & SRB_FXDISC_RESP_DMA_VALID) {
		fdisc->u.fxiocb.rsp_addr = dma_alloc_coherent(&ha->pdev->dev,
		    fdisc->u.fxiocb.rsp_len,
		    &fdisc->u.fxiocb.rsp_dma_handle, GFP_KERNEL);
		if (!fdisc->u.fxiocb.rsp_addr) {
			goto done_unmap_req;
		}
	}

	sp->type = SRB_FXIOCB_DCMD;
	sp->name = "fxdisc";
	qla2x00_init_timer(sp, FXDISC_TIMEOUT);
	fdisc->timeout = qla2x00_fxdisc_iocb_timeout;
	fdisc->u.fxiocb.req_func_type = fx_type;
	sp->done = qla2x00_fxdisc_sp_done;

	rval = qla2x00_start_sp(sp);
	if (rval != QLA_SUCCESS)
		goto done_unmap_dma;

	wait_for_completion(&fdisc->u.fxiocb.fxiocb_comp);

	if (fx_type == FXDISC_GET_CONFIG_INFO) {
		config_info_data_t *pinfo =
		    (config_info_data_t *) fdisc->u.fxiocb.rsp_addr;
		memcpy(&vha->hw->mr.product_name, pinfo->product_name,
		    sizeof(vha->hw->mr.product_name));
		memcpy(&vha->hw->mr.symbolic_name, pinfo->symbolic_name,
		    sizeof(vha->hw->mr.symbolic_name));
		memcpy(&vha->hw->mr.serial_num, pinfo->serial_num,
		    sizeof(vha->hw->mr.serial_num));
		memcpy(&vha->hw->mr.hw_version, pinfo->hw_version,
		    sizeof(vha->hw->mr.hw_version));
		memcpy(&vha->hw->mr.fw_version, pinfo->fw_version,
		    sizeof(vha->hw->mr.fw_version));
		strim(vha->hw->mr.fw_version);
		memcpy(&vha->hw->mr.uboot_version, pinfo->uboot_version,
		    sizeof(vha->hw->mr.uboot_version));
		memcpy(&vha->hw->mr.fru_serial_num, pinfo->fru_serial_num,
		    sizeof(vha->hw->mr.fru_serial_num));
		vha->hw->mr.critical_temperature =
		    (pinfo->nominal_temp_value) ?
		    pinfo->nominal_temp_value : QLAFX00_CRITEMP_THRSHLD;
		ha->mr.extended_io_enabled = (pinfo->enabled_capabilities &
		    QLAFX00_EXTENDED_IO_EN_MASK) != 0;
	} else if (fx_type == FXDISC_GET_PORT_INFO) {
		port_info_data_t *pinfo =
		    (port_info_data_t *) fdisc->u.fxiocb.rsp_addr;
		memcpy(vha->node_name, pinfo->node_name, WWN_SIZE);
		memcpy(vha->port_name, pinfo->port_name, WWN_SIZE);
		vha->d_id.b.domain = pinfo->port_id[0];
		vha->d_id.b.area = pinfo->port_id[1];
		vha->d_id.b.al_pa = pinfo->port_id[2];
		qlafx00_update_host_attr(vha, pinfo);
		ql_dump_buffer(ql_dbg_init + ql_dbg_buffer, vha, 0x014e,
		    (uint8_t *)pinfo, 16);
	} else if (fx_type == FXDISC_GET_TGT_NODE_INFO) {
		qlafx00_tgt_node_info_t *pinfo =
		    (qlafx00_tgt_node_info_t *) fdisc->u.fxiocb.rsp_addr;
		memcpy(fcport->node_name, pinfo->tgt_node_wwnn, WWN_SIZE);
		memcpy(fcport->port_name, pinfo->tgt_node_wwpn, WWN_SIZE);
		fcport->port_type = FCT_TARGET;
		ql_dump_buffer(ql_dbg_init + ql_dbg_buffer, vha, 0x014f,
		    (uint8_t *)pinfo, 16);
	} else if (fx_type == FXDISC_GET_TGT_NODE_LIST) {
		qlafx00_tgt_node_info_t *pinfo =
		    (qlafx00_tgt_node_info_t *) fdisc->u.fxiocb.rsp_addr;
		ql_dump_buffer(ql_dbg_init + ql_dbg_buffer, vha, 0x0150,
		    (uint8_t *)pinfo, 16);
		memcpy(vha->hw->gid_list, pinfo, QLAFX00_TGT_NODE_LIST_SIZE);
	}
	rval = fdisc->u.fxiocb.result;

done_unmap_dma:
	if (fdisc->u.fxiocb.rsp_addr)
		dma_free_coherent(&ha->pdev->dev, fdisc->u.fxiocb.rsp_len,
		    fdisc->u.fxiocb.rsp_addr, fdisc->u.fxiocb.rsp_dma_handle);

done_unmap_req:
	if (fdisc->u.fxiocb.req_addr)
		dma_free_coherent(&ha->pdev->dev, fdisc->u.fxiocb.req_len,
		    fdisc->u.fxiocb.req_addr, fdisc->u.fxiocb.req_dma_handle);
done_free_sp:
	sp->free(vha, sp);
done:
	return rval;
}

static void
qlafx00_abort_iocb_timeout(void *data)
{
	srb_t *sp = (srb_t *)data;
	struct srb_iocb *abt = &sp->u.iocb_cmd;

	abt->u.abt.comp_status = CS_TIMEOUT;
	complete(&abt->u.abt.comp);
}

static void
qlafx00_abort_sp_done(void *data, void *ptr, int res)
{
	srb_t *sp = (srb_t *)ptr;
	struct srb_iocb *abt = &sp->u.iocb_cmd;

	complete(&abt->u.abt.comp);
}

int
qlafx00_async_abt_cmd(srb_t *cmd_sp)
{
	scsi_qla_host_t *vha = cmd_sp->fcport->vha;
	fc_port_t *fcport = cmd_sp->fcport;
	struct srb_iocb *abt_iocb;
	srb_t *sp;
	int rval = QLA_FUNCTION_FAILED;

	sp = qla2x00_get_sp(vha, fcport, GFP_KERNEL);
	if (!sp)
		goto done;

	abt_iocb = &sp->u.iocb_cmd;
	sp->type = SRB_ABT_CMD;
	sp->name = "abort";
	qla2x00_init_timer(sp, FXDISC_TIMEOUT);
	abt_iocb->u.abt.cmd_hndl = cmd_sp->handle;
	sp->done = qlafx00_abort_sp_done;
	abt_iocb->timeout = qlafx00_abort_iocb_timeout;
	init_completion(&abt_iocb->u.abt.comp);

	rval = qla2x00_start_sp(sp);
	if (rval != QLA_SUCCESS)
		goto done_free_sp;

	ql_dbg(ql_dbg_async, vha, 0x507c,
	    "Abort command issued - hdl=%x, target_id=%x\n",
	    cmd_sp->handle, fcport->tgt_id);

	wait_for_completion(&abt_iocb->u.abt.comp);

	rval = abt_iocb->u.abt.comp_status == CS_COMPLETE ?
	    QLA_SUCCESS : QLA_FUNCTION_FAILED;

done_free_sp:
	sp->free(vha, sp);
done:
	return rval;
}

/*
 * qlafx00_initialize_adapter
 *      Initialize board.
 *
 * Input:
 *      ha = adapter block pointer.
 *
 * Returns:
 *      0 = success
 */
int
qlafx00_initialize_adapter(scsi_qla_host_t *vha)
{
	int	rval;
	struct qla_hw_data *ha = vha->hw;
	uint32_t tempc;

	/* Clear adapter flags. */
	vha->flags.online = 0;
	ha->flags.chip_reset_done = 0;
	vha->flags.reset_active = 0;
	ha->flags.pci_channel_io_perm_failure = 0;
	ha->flags.eeh_busy = 0;
	atomic_set(&vha->loop_down_timer, LOOP_DOWN_TIME);
	atomic_set(&vha->loop_state, LOOP_DOWN);
	vha->device_flags = DFLG_NO_CABLE;
	vha->dpc_flags = 0;
	vha->flags.management_server_logged_in = 0;
	vha->marker_needed = 0;
	ha->isp_abort_cnt = 0;
	ha->beacon_blink_led = 0;

	set_bit(0, ha->req_qid_map);
	set_bit(0, ha->rsp_qid_map);

	ql_dbg(ql_dbg_init, vha, 0x0147,
	    "Configuring PCI space...\n");

	rval = ha->isp_ops->pci_config(vha);
	if (rval) {
		ql_log(ql_log_warn, vha, 0x0148,
		    "Unable to configure PCI space.\n");
		return rval;
	}

	rval = qlafx00_init_fw_ready(vha);
	if (rval != QLA_SUCCESS) {
		return rval;
	}

	qlafx00_save_queue_ptrs(vha);

	rval = qlafx00_config_queues(vha);
	if (rval != QLA_SUCCESS) {
		return rval;
	}

	/*
	 * Allocate the array of outstanding commands
	 * now that we know the firmware resources.
	 */
	rval = qla2x00_alloc_outstanding_cmds(ha, vha->req);
	if (rval != QLA_SUCCESS) {
		return rval;
	}

	rval = qla2x00_init_rings(vha);
	ha->flags.chip_reset_done = 1;

	tempc = QLAFX00_GET_TEMPERATURE(ha);
	ql_dbg(ql_dbg_init, vha, 0x0153,
	    "ISPFx00(%s): Critical temp timer, current SOC temperature: 0x%x\n",
	    __func__, tempc);

	return rval;
}

uint32_t
qlafx00_fw_state_show(struct device *dev, struct device_attribute *attr,
    char *buf)
{
	scsi_qla_host_t *vha = shost_priv(class_to_shost(dev));
	int rval = QLA_FUNCTION_FAILED;
	uint32_t state[1];

	if (qla2x00_reset_active(vha))
		ql_log(ql_log_warn, vha, 0x70ce,
		    "ISP reset active.\n");
	else if (!vha->hw->flags.eeh_busy) {
		rval = qlafx00_get_firmware_state(vha, state);
	}
	if (rval != QLA_SUCCESS)
		memset(state, -1, sizeof(state));

	return state[0];
}

void
qlafx00_get_host_speed(struct Scsi_Host *shost)
{
	struct qla_hw_data *ha = ((struct scsi_qla_host *)
					(shost_priv(shost)))->hw;
	u32 speed = FC_PORTSPEED_UNKNOWN;

	switch (ha->link_data_rate) {
	case QLAFX00_PORT_SPEED_2G:
		speed = FC_PORTSPEED_2GBIT;
		break;
	case QLAFX00_PORT_SPEED_4G:
		speed = FC_PORTSPEED_4GBIT;
		break;
	case QLAFX00_PORT_SPEED_8G:
		speed = FC_PORTSPEED_8GBIT;
		break;
	case QLAFX00_PORT_SPEED_10G:
		speed = FC_PORTSPEED_10GBIT;
		break;
	}
	fc_host_speed(shost) = speed;
}

/** QLAFX00 specific Mailbox implementation functions */

/*
 * qlafx00_mailbox_command
 *	Issue mailbox command and waits for completion.
 *
 * Input:
 *	ha = adapter block pointer.
 *	mcp = driver internal mbx struct pointer.
 *
 * Output:
 *	mb[MAX_MAILBOX_REGISTER_COUNT] = returned mailbox data.
 *
 * Returns:
 *	0 : QLA_SUCCESS = cmd performed success
 *	1 : QLA_FUNCTION_FAILED   (error encountered)
 *	6 : QLA_FUNCTION_TIMEOUT (timeout condition encountered)
 *
 * Context:
 *	Kernel context.
 */
static int
qlafx00_mailbox_command(scsi_qla_host_t *vha, mbx_cmd_32_t *mcp)

{
	int		rval;
	unsigned long    flags = 0;
	device_reg_t __iomem *reg;
	uint8_t		abort_active;
	uint8_t		io_lock_on;
	uint16_t	command = 0;
	uint32_t	*iptr;
	uint32_t __iomem *optr;
	uint32_t	cnt;
	uint32_t	mboxes;
	unsigned long	wait_time;
	struct qla_hw_data *ha = vha->hw;
	scsi_qla_host_t *base_vha = pci_get_drvdata(ha->pdev);

	if (ha->pdev->error_state > pci_channel_io_frozen) {
		ql_log(ql_log_warn, vha, 0x115c,
		    "error_state is greater than pci_channel_io_frozen, "
		    "exiting.\n");
		return QLA_FUNCTION_TIMEOUT;
	}

	if (vha->device_flags & DFLG_DEV_FAILED) {
		ql_log(ql_log_warn, vha, 0x115f,
		    "Device in failed state, exiting.\n");
		return QLA_FUNCTION_TIMEOUT;
	}

	reg = ha->iobase;
	io_lock_on = base_vha->flags.init_done;

	rval = QLA_SUCCESS;
	abort_active = test_bit(ABORT_ISP_ACTIVE, &base_vha->dpc_flags);

	if (ha->flags.pci_channel_io_perm_failure) {
		ql_log(ql_log_warn, vha, 0x1153,
		    "Perm failure on EEH timeout MBX, exiting.\n");
		return QLA_FUNCTION_TIMEOUT;
	}

	if (ha->flags.isp82xx_fw_hung) {
		/* Setting Link-Down error */
		mcp->mb[0] = MBS_LINK_DOWN_ERROR;
		ql_log(ql_log_warn, vha, 0x1154,
		    "FW hung = %d.\n", ha->flags.isp82xx_fw_hung);
		rval = QLA_FUNCTION_FAILED;
		goto premature_exit;
	}

	/*
	 * Wait for active mailbox commands to finish by waiting at most tov
	 * seconds. This is to serialize actual issuing of mailbox cmds during
	 * non ISP abort time.
	 */
	if (!wait_for_completion_timeout(&ha->mbx_cmd_comp, mcp->tov * HZ)) {
		/* Timeout occurred. Return error. */
		ql_log(ql_log_warn, vha, 0x1155,
		    "Cmd access timeout, cmd=0x%x, Exiting.\n",
		    mcp->mb[0]);
		return QLA_FUNCTION_TIMEOUT;
	}

	ha->flags.mbox_busy = 1;
	/* Save mailbox command for debug */
	ha->mcp32 = mcp;

	ql_dbg(ql_dbg_mbx, vha, 0x1156,
	    "Prepare to issue mbox cmd=0x%x.\n", mcp->mb[0]);

	spin_lock_irqsave(&ha->hardware_lock, flags);

	/* Load mailbox registers. */
	optr = (uint32_t __iomem *)&reg->ispfx00.mailbox0;

	iptr = mcp->mb;
	command = mcp->mb[0];
	mboxes = mcp->out_mb;

	for (cnt = 0; cnt < ha->mbx_count; cnt++) {
		if (mboxes & BIT_0)
			WRT_REG_DWORD(optr, *iptr);

		mboxes >>= 1;
		optr++;
		iptr++;
	}

	/* Issue set host interrupt command to send cmd out. */
	ha->flags.mbox_int = 0;
	clear_bit(MBX_INTERRUPT, &ha->mbx_cmd_flags);

	ql_dump_buffer(ql_dbg_mbx + ql_dbg_buffer, vha, 0x1172,
	    (uint8_t *)mcp->mb, 16);
	ql_dump_buffer(ql_dbg_mbx + ql_dbg_buffer, vha, 0x1173,
	    ((uint8_t *)mcp->mb + 0x10), 16);
	ql_dump_buffer(ql_dbg_mbx + ql_dbg_buffer, vha, 0x1174,
	    ((uint8_t *)mcp->mb + 0x20), 8);

	/* Unlock mbx registers and wait for interrupt */
	ql_dbg(ql_dbg_mbx, vha, 0x1157,
	    "Going to unlock irq & waiting for interrupts. "
	    "jiffies=%lx.\n", jiffies);

	/* Wait for mbx cmd completion until timeout */
	if ((!abort_active && io_lock_on) || IS_NOPOLLING_TYPE(ha)) {
		set_bit(MBX_INTR_WAIT, &ha->mbx_cmd_flags);

		QLAFX00_SET_HST_INTR(ha, ha->mbx_intr_code);
		spin_unlock_irqrestore(&ha->hardware_lock, flags);

		wait_for_completion_timeout(&ha->mbx_intr_comp, mcp->tov * HZ);
	} else {
		ql_dbg(ql_dbg_mbx, vha, 0x1158,
		    "Cmd=%x Polling Mode.\n", command);

		QLAFX00_SET_HST_INTR(ha, ha->mbx_intr_code);
		spin_unlock_irqrestore(&ha->hardware_lock, flags);

		wait_time = jiffies + mcp->tov * HZ; /* wait at most tov secs */
		while (!ha->flags.mbox_int) {
			if (time_after(jiffies, wait_time))
				break;

			/* Check for pending interrupts. */
			qla2x00_poll(ha->rsp_q_map[0]);

			if (!ha->flags.mbox_int &&
			    !(IS_QLA2200(ha) &&
			    command == MBC_LOAD_RISC_RAM_EXTENDED))
				msleep(10);
		} /* while */
		ql_dbg(ql_dbg_mbx, vha, 0x1159,
		    "Waited %d sec.\n",
		    (uint)((jiffies - (wait_time - (mcp->tov * HZ)))/HZ));
	}

	/* Check whether we timed out */
	if (ha->flags.mbox_int) {
		uint32_t *iptr2;

		ql_dbg(ql_dbg_mbx, vha, 0x115a,
		    "Cmd=%x completed.\n", command);

		/* Got interrupt. Clear the flag. */
		ha->flags.mbox_int = 0;
		clear_bit(MBX_INTERRUPT, &ha->mbx_cmd_flags);

		if (ha->mailbox_out32[0] != MBS_COMMAND_COMPLETE)
			rval = QLA_FUNCTION_FAILED;

		/* Load return mailbox registers. */
		iptr2 = mcp->mb;
		iptr = (uint32_t *)&ha->mailbox_out32[0];
		mboxes = mcp->in_mb;
		for (cnt = 0; cnt < ha->mbx_count; cnt++) {
			if (mboxes & BIT_0)
				*iptr2 = *iptr;

			mboxes >>= 1;
			iptr2++;
			iptr++;
		}
	} else {

		rval = QLA_FUNCTION_TIMEOUT;
	}

	ha->flags.mbox_busy = 0;

	/* Clean up */
	ha->mcp32 = NULL;

	if ((abort_active || !io_lock_on) && !IS_NOPOLLING_TYPE(ha)) {
		ql_dbg(ql_dbg_mbx, vha, 0x115b,
		    "checking for additional resp interrupt.\n");

		/* polling mode for non isp_abort commands. */
		qla2x00_poll(ha->rsp_q_map[0]);
	}

	if (rval == QLA_FUNCTION_TIMEOUT &&
	    mcp->mb[0] != MBC_GEN_SYSTEM_ERROR) {
		if (!io_lock_on || (mcp->flags & IOCTL_CMD) ||
		    ha->flags.eeh_busy) {
			/* not in dpc. schedule it for dpc to take over. */
			ql_dbg(ql_dbg_mbx, vha, 0x115d,
			    "Timeout, schedule isp_abort_needed.\n");

			if (!test_bit(ISP_ABORT_NEEDED, &vha->dpc_flags) &&
			    !test_bit(ABORT_ISP_ACTIVE, &vha->dpc_flags) &&
			    !test_bit(ISP_ABORT_RETRY, &vha->dpc_flags)) {

				ql_log(ql_log_info, base_vha, 0x115e,
				    "Mailbox cmd timeout occurred, cmd=0x%x, "
				    "mb[0]=0x%x, eeh_busy=0x%x. Scheduling ISP "
				    "abort.\n", command, mcp->mb[0],
				    ha->flags.eeh_busy);
				set_bit(ISP_ABORT_NEEDED, &vha->dpc_flags);
				qla2xxx_wake_dpc(vha);
			}
		} else if (!abort_active) {
			/* call abort directly since we are in the DPC thread */
			ql_dbg(ql_dbg_mbx, vha, 0x1160,
			    "Timeout, calling abort_isp.\n");

			if (!test_bit(ISP_ABORT_NEEDED, &vha->dpc_flags) &&
			    !test_bit(ABORT_ISP_ACTIVE, &vha->dpc_flags) &&
			    !test_bit(ISP_ABORT_RETRY, &vha->dpc_flags)) {

				ql_log(ql_log_info, base_vha, 0x1161,
				    "Mailbox cmd timeout occurred, cmd=0x%x, "
				    "mb[0]=0x%x. Scheduling ISP abort ",
				    command, mcp->mb[0]);

				set_bit(ABORT_ISP_ACTIVE, &vha->dpc_flags);
				clear_bit(ISP_ABORT_NEEDED, &vha->dpc_flags);
				if (ha->isp_ops->abort_isp(vha)) {
					/* Failed. retry later. */
					set_bit(ISP_ABORT_NEEDED,
					    &vha->dpc_flags);
				}
				clear_bit(ABORT_ISP_ACTIVE, &vha->dpc_flags);
				ql_dbg(ql_dbg_mbx, vha, 0x1162,
				    "Finished abort_isp.\n");
			}
		}
	}

premature_exit:
	/* Allow next mbx cmd to come in. */
	complete(&ha->mbx_cmd_comp);

	if (rval) {
		ql_log(ql_log_warn, base_vha, 0x1163,
		    "**** Failed mbx[0]=%x, mb[1]=%x, mb[2]=%x, mb[3]=%x, cmd=%x ****.\n",
		    mcp->mb[0], mcp->mb[1], mcp->mb[2], mcp->mb[3], command);
	} else {
		ql_dbg(ql_dbg_mbx, base_vha, 0x1164, "Done %s.\n", __func__);
	}

	return rval;
}

/*
 * qlafx00_driver_shutdown
 *	Indicate a driver shutdown to firmware.
 *
 * Input:
 *	ha = adapter block pointer.
 *
 * Returns:
 *	local function return status code.
 *
 * Context:
 *	Kernel context.
 */
int
qlafx00_driver_shutdown(scsi_qla_host_t *vha, int tmo)
{
	int rval;
	mbx_cmd_32_t mc;
	mbx_cmd_32_t *mcp = &mc;

	ql_dbg(ql_dbg_mbx + ql_dbg_verbose, vha, 0x1166,
	    "Entered %s.\n", __func__);

	mcp->mb[0] = MBC_MR_DRV_SHUTDOWN;
	mcp->out_mb = MBX_0;
	mcp->in_mb = MBX_0;
	if (tmo)
		mcp->tov = tmo;
	else
		mcp->tov = MBX_TOV_SECONDS;
	mcp->flags = 0;
	rval = qlafx00_mailbox_command(vha, mcp);

	if (rval != QLA_SUCCESS) {
		ql_dbg(ql_dbg_mbx, vha, 0x1167,
		    "Failed=%x.\n", rval);
	} else {
		ql_dbg(ql_dbg_mbx + ql_dbg_verbose, vha, 0x1168,
		    "Done %s.\n", __func__);
	}

	return rval;
}

/*
 * qlafx00_get_firmware_state
 *	Get adapter firmware state.
 *
 * Input:
 *	ha = adapter block pointer.
 *	TARGET_QUEUE_LOCK must be released.
 *	ADAPTER_STATE_LOCK must be released.
 *
 * Returns:
 *	qla7xxx local function return status code.
 *
 * Context:
 *	Kernel context.
 */
int
qlafx00_get_firmware_state(scsi_qla_host_t *vha, uint32_t *states)
{
	int rval;
	mbx_cmd_32_t mc;
	mbx_cmd_32_t *mcp = &mc;

	ql_dbg(ql_dbg_mbx + ql_dbg_verbose, vha, 0x1169,
	    "Entered %s.\n", __func__);

	mcp->mb[0] = MBC_GET_FIRMWARE_STATE;
	mcp->out_mb = MBX_0;
	mcp->in_mb = MBX_1|MBX_0;
	mcp->tov = MBX_TOV_SECONDS;
	mcp->flags = 0;
	rval = qlafx00_mailbox_command(vha, mcp);

	/* Return firmware states. */
	states[0] = mcp->mb[1];

	if (rval != QLA_SUCCESS) {
		ql_dbg(ql_dbg_mbx, vha, 0x116a,
		    "Failed=%x mb[0]=%x.\n", rval, mcp->mb[0]);
	} else {
		ql_dbg(ql_dbg_mbx + ql_dbg_verbose, vha, 0x116b,
		    "Done %s.\n", __func__);
	}
	return rval;
}

/*
 * qlafx00_init_firmware
 *	Initialize adapter firmware.
 *
 * Input:
 *	ha = adapter block pointer.
 *	dptr = Initialization control block pointer.
 *	size = size of initialization control block.
 *	TARGET_QUEUE_LOCK must be released.
 *	ADAPTER_STATE_LOCK must be released.
 *
 * Returns:
 *	qlafx00 local function return status code.
 *
 * Context:
 *	Kernel context.
 */
int
qlafx00_init_firmware(scsi_qla_host_t *vha, uint16_t size)
{
	int rval;
	mbx_cmd_32_t mc;
	mbx_cmd_32_t *mcp = &mc;
	struct qla_hw_data *ha = vha->hw;

	ql_dbg(ql_dbg_mbx + ql_dbg_verbose, vha, 0x116c,
	    "Entered %s.\n", __func__);

	mcp->mb[0] = MBC_INITIALIZE_FIRMWARE;

	mcp->mb[1] = 0;
	mcp->mb[2] = MSD(ha->init_cb_dma);
	mcp->mb[3] = LSD(ha->init_cb_dma);

	mcp->out_mb = MBX_3|MBX_2|MBX_1|MBX_0;
	mcp->in_mb = MBX_0;
	mcp->buf_size = size;
	mcp->flags = MBX_DMA_OUT;
	mcp->tov = MBX_TOV_SECONDS;
	rval = qlafx00_mailbox_command(vha, mcp);

	if (rval != QLA_SUCCESS) {
		ql_dbg(ql_dbg_mbx, vha, 0x116d,
		    "Failed=%x mb[0]=%x.\n", rval, mcp->mb[0]);
	} else {
		ql_dbg(ql_dbg_mbx + ql_dbg_verbose, vha, 0x116e,
		    "Done %s.\n", __func__);
	}
	return rval;
}

/*
 * qlafx00_mbx_reg_test
 */
int
qlafx00_mbx_reg_test(scsi_qla_host_t *vha)
{
	int rval;
	mbx_cmd_32_t mc;
	mbx_cmd_32_t *mcp = &mc;

	ql_dbg(ql_dbg_mbx + ql_dbg_verbose, vha, 0x116f,
	    "Entered %s.\n", __func__);


	mcp->mb[0] = MBC_MAILBOX_REGISTER_TEST;
	mcp->mb[1] = 0xAAAA;
	mcp->mb[2] = 0x5555;
	mcp->mb[3] = 0xAA55;
	mcp->mb[4] = 0x55AA;
	mcp->mb[5] = 0xA5A5;
	mcp->mb[6] = 0x5A5A;
	mcp->mb[7] = 0x2525;
	mcp->mb[8] = 0xBBBB;
	mcp->mb[9] = 0x6666;
	mcp->mb[10] = 0xBB66;
	mcp->mb[11] = 0x66BB;
	mcp->mb[12] = 0xB6B6;
	mcp->mb[13] = 0x6B6B;
	mcp->mb[14] = 0x3636;
	mcp->mb[15] = 0xCCCC;


	mcp->out_mb = MBX_15|MBX_14|MBX_13|MBX_12|MBX_11|MBX_10|MBX_9|MBX_8|
			MBX_7|MBX_6|MBX_5|MBX_4|MBX_3|MBX_2|MBX_1|MBX_0;
	mcp->in_mb = MBX_15|MBX_14|MBX_13|MBX_12|MBX_11|MBX_10|MBX_9|MBX_8|
			MBX_7|MBX_6|MBX_5|MBX_4|MBX_3|MBX_2|MBX_1|MBX_0;
	mcp->buf_size = 0;
	mcp->flags = MBX_DMA_OUT;
	mcp->tov = MBX_TOV_SECONDS;
	rval = qlafx00_mailbox_command(vha, mcp);
	if (rval == QLA_SUCCESS) {
		if (mcp->mb[17] != 0xAAAA || mcp->mb[18] != 0x5555 ||
		    mcp->mb[19] != 0xAA55 || mcp->mb[20] != 0x55AA)
			rval = QLA_FUNCTION_FAILED;
		if (mcp->mb[21] != 0xA5A5 || mcp->mb[22] != 0x5A5A ||
		    mcp->mb[23] != 0x2525 || mcp->mb[24] != 0xBBBB)
			rval = QLA_FUNCTION_FAILED;
		if (mcp->mb[25] != 0x6666 || mcp->mb[26] != 0xBB66 ||
		    mcp->mb[27] != 0x66BB || mcp->mb[28] != 0xB6B6)
			rval = QLA_FUNCTION_FAILED;
		if (mcp->mb[29] != 0x6B6B || mcp->mb[30] != 0x3636 ||
		    mcp->mb[31] != 0xCCCC)
			rval = QLA_FUNCTION_FAILED;
	}

	if (rval != QLA_SUCCESS) {
		ql_dbg(ql_dbg_mbx, vha, 0x1170,
		    "Failed=%x mb[0]=%x.\n", rval, mcp->mb[0]);
	} else {
		ql_dbg(ql_dbg_mbx + ql_dbg_verbose, vha, 0x1171,
		    "Done %s.\n", __func__);
	}
	return rval;
}

/** QLAFX00 specific ISR implementation functions */

static inline void
qlafx00_handle_sense(srb_t *sp, uint8_t *sense_data, uint32_t par_sense_len,
    uint32_t sense_len, struct rsp_que *rsp, int res)
{
	struct scsi_qla_host *vha = sp->fcport->vha;
	struct scsi_cmnd *cp = GET_CMD_SP(sp);
	uint32_t track_sense_len;

	SET_FW_SENSE_LEN(sp, sense_len);

	if (sense_len >= SCSI_SENSE_BUFFERSIZE)
		sense_len = SCSI_SENSE_BUFFERSIZE;

	SET_CMD_SENSE_LEN(sp, sense_len);
	SET_CMD_SENSE_PTR(sp, cp->sense_buffer);
	track_sense_len = sense_len;

	if (sense_len > par_sense_len)
		sense_len = par_sense_len;

	memcpy(cp->sense_buffer, sense_data, sense_len);

	SET_FW_SENSE_LEN(sp, GET_FW_SENSE_LEN(sp) - sense_len);

	SET_CMD_SENSE_PTR(sp, cp->sense_buffer + sense_len);
	track_sense_len -= sense_len;
	SET_CMD_SENSE_LEN(sp, track_sense_len);

	ql_dbg(ql_dbg_io, vha, 0x304d,
	    "sense_len=0x%x par_sense_len=0x%x track_sense_len=0x%x.\n",
	    sense_len, par_sense_len, track_sense_len);
	if (GET_FW_SENSE_LEN(sp) > 0) {
		rsp->status_srb = sp;
		cp->result = res;
	}

	if (sense_len) {
		ql_dbg(ql_dbg_io + ql_dbg_buffer, vha, 0x3039,
		    "Check condition Sense data, nexus%ld:%d:%d cmd=%p.\n",
		    sp->fcport->vha->host_no, cp->device->id, cp->device->lun,
		    cp);
		ql_dump_buffer(ql_dbg_io + ql_dbg_buffer, vha, 0x3049,
		    cp->sense_buffer, sense_len);
	}
}

static void
qlafx00_tm_iocb_entry(scsi_qla_host_t *vha, struct req_que *req,
    struct tsk_mgmt_entry_fx00 *pkt, srb_t *sp,
    uint16_t sstatus, uint16_t cpstatus)
{
	struct srb_iocb *tmf;

	tmf = &sp->u.iocb_cmd;
	if (cpstatus != CS_COMPLETE ||
	    (sstatus & SS_RESPONSE_INFO_LEN_VALID))
		cpstatus = CS_INCOMPLETE;
	tmf->u.tmf.comp_status = cpstatus;
	sp->done(vha, sp, 0);
}

static void
qlafx00_abort_iocb_entry(scsi_qla_host_t *vha, struct req_que *req,
    struct abort_iocb_entry_fx00 *pkt)
{
	const char func[] = "ABT_IOCB";
	srb_t *sp;
	struct srb_iocb *abt;

	sp = qla2x00_get_sp_from_handle(vha, func, req, pkt);
	if (!sp)
		return;

	abt = &sp->u.iocb_cmd;
	abt->u.abt.comp_status = le32_to_cpu(pkt->tgt_id_sts);
	sp->done(vha, sp, 0);
}

static void
qlafx00_ioctl_iosb_entry(scsi_qla_host_t *vha, struct req_que *req,
    struct ioctl_iocb_entry_fx00 *pkt)
{
	const char func[] = "IOSB_IOCB";
	srb_t *sp;
	struct fc_bsg_job *bsg_job;
	struct srb_iocb *iocb_job;
	int res;
	qla_mt_iocb_rsp_fx00_t fstatus;
	uint8_t	*fw_sts_ptr;

	sp = qla2x00_get_sp_from_handle(vha, func, req, pkt);
	if (!sp)
		return;

	if (sp->type == SRB_FXIOCB_DCMD) {
		iocb_job = &sp->u.iocb_cmd;
		iocb_job->u.fxiocb.seq_number = le32_to_cpu(pkt->seq_no);
		iocb_job->u.fxiocb.fw_flags = le32_to_cpu(pkt->fw_iotcl_flags);
		iocb_job->u.fxiocb.result = le32_to_cpu(pkt->status);
		if (iocb_job->u.fxiocb.flags & SRB_FXDISC_RSP_DWRD_VALID)
			iocb_job->u.fxiocb.req_data = le32_to_cpu(pkt->dataword_r);
	} else {
		bsg_job = sp->u.bsg_job;

		memset(&fstatus, 0, sizeof(qla_mt_iocb_rsp_fx00_t));

		fstatus.reserved_1 = pkt->reserved_0;
		fstatus.func_type = pkt->comp_func_num;
		fstatus.ioctl_flags = pkt->fw_iotcl_flags;
		fstatus.ioctl_data = pkt->dataword_r;
		fstatus.adapid = pkt->adapid;
		fstatus.adapid_hi = pkt->adapid_hi;
		fstatus.reserved_2 = pkt->reserved_1;
		fstatus.res_count = pkt->residuallen;
		fstatus.status = pkt->status;
		fstatus.seq_number = pkt->seq_no;
		memcpy(fstatus.reserved_3,
		    pkt->reserved_2, 20 * sizeof(uint8_t));

		fw_sts_ptr = ((uint8_t *)bsg_job->req->sense) +
		    sizeof(struct fc_bsg_reply);

		memcpy(fw_sts_ptr, (uint8_t *)&fstatus,
		    sizeof(qla_mt_iocb_rsp_fx00_t));
		bsg_job->reply_len = sizeof(struct fc_bsg_reply) +
			sizeof(qla_mt_iocb_rsp_fx00_t) + sizeof(uint8_t);

		ql_dump_buffer(ql_dbg_user + ql_dbg_verbose, sp->fcport->vha, 0x5080,
		    (uint8_t *)pkt, sizeof(struct ioctl_iocb_entry_fx00));

		ql_dump_buffer(ql_dbg_user + ql_dbg_verbose, sp->fcport->vha, 0x5081,
		    (uint8_t *)fw_sts_ptr, sizeof(qla_mt_iocb_rsp_fx00_t));

		res = bsg_job->reply->result = DID_OK << 16;
		bsg_job->reply->reply_payload_rcv_len =
		    bsg_job->reply_payload.payload_len;
	}
	sp->done(vha, sp, res);
}

/**
 * qlafx00_status_entry() - Process a Status IOCB entry.
 * @ha: SCSI driver HA context
 * @pkt: Entry pointer
 */
static void
qlafx00_status_entry(scsi_qla_host_t *vha, struct rsp_que *rsp, void *pkt)
{
	srb_t		*sp;
	fc_port_t	*fcport;
	struct scsi_cmnd *cp;
	struct sts_entry_fx00 *sts;
	uint16_t	comp_status;
	uint16_t	scsi_status;
	uint16_t	ox_id;
	uint8_t		lscsi_status;
	int32_t		resid;
	uint32_t	sense_len, par_sense_len, rsp_info_len, resid_len,
	    fw_resid_len;
	uint8_t		*rsp_info = NULL, *sense_data = NULL;
	struct qla_hw_data *ha = vha->hw;
	uint32_t hindex, handle;
	uint16_t que;
	struct req_que *req;
	int logit = 1;
	int res = 0;

	sts = (struct sts_entry_fx00 *) pkt;

	comp_status = le16_to_cpu(sts->comp_status);
	scsi_status = le16_to_cpu(sts->scsi_status) & SS_MASK;
	hindex = sts->handle;
	handle = LSW(hindex);

	que = MSW(hindex);
	req = ha->req_q_map[que];

	/* Validate handle. */
	if (handle < req->num_outstanding_cmds)
		sp = req->outstanding_cmds[handle];
	else
		sp = NULL;

	if (sp == NULL) {
		ql_dbg(ql_dbg_io, vha, 0x3034,
		    "Invalid status handle (0x%x).\n", handle);

		set_bit(ISP_ABORT_NEEDED, &vha->dpc_flags);
		qla2xxx_wake_dpc(vha);
		return;
	}

	if (sp->type == SRB_TM_CMD) {
		req->outstanding_cmds[handle] = NULL;
		qlafx00_tm_iocb_entry(vha, req, pkt, sp,
		    scsi_status, comp_status);
		return;
	}

	/* Fast path completion. */
	if (comp_status == CS_COMPLETE && scsi_status == 0) {
		qla2x00_process_completed_request(vha, req, handle);
		return;
	}

	req->outstanding_cmds[handle] = NULL;
	cp = GET_CMD_SP(sp);
	if (cp == NULL) {
		ql_dbg(ql_dbg_io, vha, 0x3048,
		    "Command already returned (0x%x/%p).\n",
		    handle, sp);

		return;
	}

	lscsi_status = scsi_status & STATUS_MASK;

	fcport = sp->fcport;

	ox_id = 0;
	sense_len = par_sense_len = rsp_info_len = resid_len =
		fw_resid_len = 0;
	if (scsi_status & SS_SENSE_LEN_VALID)
		sense_len = le32_to_cpu(sts->sense_len);
	if (scsi_status & (SS_RESIDUAL_UNDER | SS_RESIDUAL_OVER))
		resid_len = le32_to_cpu(sts->residual_len);
	if (comp_status == CS_DATA_UNDERRUN)
		fw_resid_len = le32_to_cpu(sts->residual_len);
	rsp_info = sense_data = sts->data;
	par_sense_len = sizeof(sts->data);

	/* TODO : The rsp_info_len field is not passed up from FW. */
	/* Check for overrun. */
	if (comp_status == CS_COMPLETE &&
	    scsi_status & SS_RESIDUAL_OVER)
		comp_status = CS_DATA_OVERRUN;

	/*
	 * Based on Host and scsi status generate status code for Linux
	 */
	switch (comp_status) {
	case CS_COMPLETE:
	case CS_QUEUE_FULL:
		if (scsi_status == 0) {
			res = DID_OK << 16;
			break;
		}
		if (scsi_status & (SS_RESIDUAL_UNDER | SS_RESIDUAL_OVER)) {
			resid = resid_len;
			scsi_set_resid(cp, resid);

			if (!lscsi_status &&
			    ((unsigned)(scsi_bufflen(cp) - resid) <
			     cp->underflow)) {
				ql_dbg(ql_dbg_io, fcport->vha, 0x301a,
				    "Mid-layer underflow "
				    "detected (0x%x of 0x%x bytes).\n",
				    resid, scsi_bufflen(cp));

				res = DID_ERROR << 16;
				break;
			}
		}
		res = DID_OK << 16 | lscsi_status;

		if (lscsi_status == SAM_STAT_TASK_SET_FULL) {
			ql_dbg(ql_dbg_io, fcport->vha, 0x301b,
			    "QUEUE FULL detected.\n");
			break;
		}
		logit = 0;
		if (lscsi_status != SS_CHECK_CONDITION)
			break;

		memset(cp->sense_buffer, 0, SCSI_SENSE_BUFFERSIZE);
		if (!(scsi_status & SS_SENSE_LEN_VALID))
			break;

		qlafx00_handle_sense(sp, sense_data, par_sense_len, sense_len,
		    rsp, res);
		break;

	case CS_DATA_UNDERRUN:
		/* Use F/W calculated residual length. */
		if (IS_FWI2_CAPABLE(ha) || IS_QLAFX00(ha))
			resid = fw_resid_len;
		else
			resid = resid_len;
		scsi_set_resid(cp, resid);
		if (scsi_status & SS_RESIDUAL_UNDER) {
			if ((IS_FWI2_CAPABLE(ha) || IS_QLAFX00(ha))
			    && fw_resid_len != resid_len) {
				ql_dbg(ql_dbg_io, fcport->vha, 0x301d,
				    "Dropped frame(s) detected "
				    "(0x%x of 0x%x bytes).\n",
				    resid, scsi_bufflen(cp));

				res = DID_ERROR << 16 | lscsi_status;
				goto check_scsi_status;
			}

			if (!lscsi_status &&
			    ((unsigned)(scsi_bufflen(cp) - resid) <
			    cp->underflow)) {
				ql_dbg(ql_dbg_io, fcport->vha, 0x301e,
				    "Mid-layer underflow "
				    "detected (0x%x of 0x%x bytes, cp->underflow: 0x%x).\n",
				    resid, scsi_bufflen(cp), cp->underflow);

				res = DID_ERROR << 16;
				break;
			}
		} else if (lscsi_status != SAM_STAT_TASK_SET_FULL &&
			    lscsi_status != SAM_STAT_BUSY) {
			/*
			 * scsi status of task set and busy are considered to be
			 * task not completed.
			 */

			ql_dbg(ql_dbg_io, fcport->vha, 0x301f,
			    "Dropped frame(s) detected (0x%x "
			    "of 0x%x bytes).\n", resid,
			    scsi_bufflen(cp));

			res = DID_ERROR << 16 | lscsi_status;
			goto check_scsi_status;
		} else {
			ql_dbg(ql_dbg_io, fcport->vha, 0x3030,
			    "scsi_status: 0x%x, lscsi_status: 0x%x\n",
			    scsi_status, lscsi_status);
		}

		res = DID_OK << 16 | lscsi_status;
		logit = 0;

check_scsi_status:
		/*
		 * Check to see if SCSI Status is non zero. If so report SCSI
		 * Status.
		 */
		if (lscsi_status != 0) {
			if (lscsi_status == SAM_STAT_TASK_SET_FULL) {
				ql_dbg(ql_dbg_io, fcport->vha, 0x3020,
				    "QUEUE FULL detected.\n");
				logit = 1;
				break;
			}
			if (lscsi_status != SS_CHECK_CONDITION)
				break;

			memset(cp->sense_buffer, 0, SCSI_SENSE_BUFFERSIZE);
			if (!(scsi_status & SS_SENSE_LEN_VALID))
				break;

			qlafx00_handle_sense(sp, sense_data, par_sense_len,
			    sense_len, rsp, res);
		}
		break;

	case CS_PORT_LOGGED_OUT:
	case CS_PORT_CONFIG_CHG:
	case CS_PORT_BUSY:
	case CS_INCOMPLETE:
	case CS_PORT_UNAVAILABLE:
	case CS_TIMEOUT:
	case CS_RESET:

		/*
		 * We are going to have the fc class block the rport
		 * while we try to recover so instruct the mid layer
		 * to requeue until the class decides how to handle this.
		 */
		res = DID_TRANSPORT_DISRUPTED << 16;

		ql_dbg(ql_dbg_io, fcport->vha, 0x3021,
		    "Port down status: port-state=0x%x.\n",
		    atomic_read(&fcport->state));

		if (atomic_read(&fcport->state) == FCS_ONLINE)
			qla2x00_mark_device_lost(fcport->vha, fcport, 1, 1);
		break;

	case CS_ABORTED:
		res = DID_RESET << 16;
		break;

	default:
		res = DID_ERROR << 16;
		break;
	}

	if (logit)
		ql_dbg(ql_dbg_io, fcport->vha, 0x3033,
		    "FCP command status: 0x%x-0x%x (0x%x) "
		    "nexus=%ld:%d:%d tgt_id: 0x%x lscsi_status: 0x%x"
		    "cdb=%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x len=0x%x "
		    "rsp_info=0x%x resid=0x%x fw_resid=0x%x "
		    "sense_len=0x%x, par_sense_len=0x%x, rsp_info_len=0x%x\n",
		    comp_status, scsi_status, res, vha->host_no,
		    cp->device->id, cp->device->lun, fcport->tgt_id,
		    lscsi_status, cp->cmnd[0], cp->cmnd[1], cp->cmnd[2],
		    cp->cmnd[3], cp->cmnd[4], cp->cmnd[5], cp->cmnd[6],
		    cp->cmnd[7], cp->cmnd[8], cp->cmnd[9], scsi_bufflen(cp),
		    rsp_info_len, resid_len, fw_resid_len, sense_len,
		    par_sense_len, rsp_info_len);

	if (rsp->status_srb == NULL)
		sp->done(ha, sp, res);
}

/**
 * qlafx00_status_cont_entry() - Process a Status Continuations entry.
 * @ha: SCSI driver HA context
 * @pkt: Entry pointer
 *
 * Extended sense data.
 */
static void
qlafx00_status_cont_entry(struct rsp_que *rsp, sts_cont_entry_t *pkt)
{
	uint8_t	sense_sz = 0;
	struct qla_hw_data *ha = rsp->hw;
	struct scsi_qla_host *vha = pci_get_drvdata(ha->pdev);
	srb_t *sp = rsp->status_srb;
	struct scsi_cmnd *cp;
	uint32_t sense_len;
	uint8_t *sense_ptr;

	if (!sp) {
		ql_dbg(ql_dbg_io, vha, 0x3037,
		    "no SP, sp = %p\n", sp);
		return;
	}

	if (!GET_FW_SENSE_LEN(sp)) {
		ql_dbg(ql_dbg_io, vha, 0x3050,
		    "no fw sense data, sp = %p\n", sp);
		return;
	}
	cp = GET_CMD_SP(sp);
	if (cp == NULL) {
		ql_log(ql_log_warn, vha, 0x303b,
		    "cmd is NULL: already returned to OS (sp=%p).\n", sp);

		rsp->status_srb = NULL;
		return;
	}

	if (!GET_CMD_SENSE_LEN(sp)) {
		ql_dbg(ql_dbg_io, vha, 0x3051,
		    "no sense data, sp = %p\n", sp);
	} else {
		sense_len = GET_CMD_SENSE_LEN(sp);
		sense_ptr = GET_CMD_SENSE_PTR(sp);
		ql_dbg(ql_dbg_io, vha, 0x304f,
		    "sp=%p sense_len=0x%x sense_ptr=%p.\n",
		    sp, sense_len, sense_ptr);

		if (sense_len > sizeof(pkt->data))
			sense_sz = sizeof(pkt->data);
		else
			sense_sz = sense_len;

		/* Move sense data. */
		ql_dump_buffer(ql_dbg_io + ql_dbg_buffer, vha, 0x304e,
		    (uint8_t *)pkt, sizeof(sts_cont_entry_t));
		memcpy(sense_ptr, pkt->data, sense_sz);
		ql_dump_buffer(ql_dbg_io + ql_dbg_buffer, vha, 0x304a,
		    sense_ptr, sense_sz);

		sense_len -= sense_sz;
		sense_ptr += sense_sz;

		SET_CMD_SENSE_PTR(sp, sense_ptr);
		SET_CMD_SENSE_LEN(sp, sense_len);
	}
	sense_len = GET_FW_SENSE_LEN(sp);
	sense_len = (sense_len > sizeof(pkt->data)) ?
	    (sense_len - sizeof(pkt->data)) : 0;
	SET_FW_SENSE_LEN(sp, sense_len);

	/* Place command on done queue. */
	if (sense_len == 0) {
		rsp->status_srb = NULL;
		ql_dbg(ql_dbg_io, vha, 0x302d,
		    "*** Calling sp->done(%s): (cmd:sp:res: %p:%p:0x%x)\n",
		    __func__, GET_CMD_SP(sp), sp, cp->result);
		sp->done(ha, sp, cp->result);
	}
}

/**
 * qlafx00_multistatus_entry() - Process Multi response queue entries.
 * @ha: SCSI driver HA context
 */
void qlafx00_multistatus_entry(struct scsi_qla_host *vha,
	struct rsp_que *rsp, void *pkt)
{
	srb_t		*sp;
	struct multi_sts_entry_fx00 *stsmfx;
	struct qla_hw_data *ha = vha->hw;
	uint32_t handle, hindex, handle_count, i;
	uint16_t que;
	struct req_que *req;
	uint32_t *handle_ptr;

	stsmfx = (struct multi_sts_entry_fx00 *) pkt;

	handle_count = stsmfx->handle_count;

	if (handle_count > MAX_HANDLE_COUNT) {
		ql_dbg(ql_dbg_io, vha, 0x3035,
		    "Invalid handle count (0x%x).\n", handle_count);
		set_bit(ISP_ABORT_NEEDED, &vha->dpc_flags);
		qla2xxx_wake_dpc(vha);
		return;
	}

	handle_ptr = (uint32_t *) &stsmfx->handles[0];

	for (i = 0; i < handle_count; i++) {
		hindex = le32_to_cpu(*handle_ptr);
		handle = LSW(hindex);
		que = MSW(hindex);
		req = ha->req_q_map[que];

		/* Validate handle. */
		if (handle < req->num_outstanding_cmds)
			sp = req->outstanding_cmds[handle];
		else
			sp = NULL;

		if (sp == NULL) {
			ql_dbg(ql_dbg_io, vha, 0x3044,
			    "Invalid status handle (0x%x).\n", handle);
			set_bit(ISP_ABORT_NEEDED, &vha->dpc_flags);
			qla2xxx_wake_dpc(vha);
			return;
		}
		qla2x00_process_completed_request(vha, req, handle);
		handle_ptr++;
	}
}

/**
 * qlafx00_error_entry() - Process an error entry.
 * @ha: SCSI driver HA context
 * @pkt: Entry pointer
 */
static void
qlafx00_error_entry(scsi_qla_host_t *vha, struct rsp_que *rsp,
    struct sts_entry_fx00 *pkt, uint8_t estatus, uint8_t etype)
{
	srb_t *sp;
	struct qla_hw_data *ha = vha->hw;
	const char func[] = "ERROR-IOCB";
	uint16_t que = MSW(pkt->handle);
	struct req_que *req = NULL;
	int res = DID_ERROR << 16;

	ql_dbg(ql_dbg_async, vha, 0x507f,
	    "type of error status in response: 0x%x\n", estatus);

	req = ha->req_q_map[que];

	sp = qla2x00_get_sp_from_handle(vha, func, req, pkt);
	if (sp) {
		sp->done(ha, sp, res);
		return;
	}

	set_bit(ISP_ABORT_NEEDED, &vha->dpc_flags);
	qla2xxx_wake_dpc(vha);
}

/**
 * qlafx00_process_response_queue() - Process response queue entries.
 * @ha: SCSI driver HA context
 */
void qlafx00_process_response_queue(struct scsi_qla_host *vha,
	struct rsp_que *rsp)
{
	struct sts_entry_fx00 *pkt;
	response_t *lptr;

	while (RD_REG_DWORD(&(rsp->ring_ptr->signature)) != RESPONSE_PROCESSED) {
		lptr = rsp->ring_ptr;
		memcpy_fromio(rsp->rsp_pkt, lptr, sizeof(rsp->rsp_pkt));
		pkt = (struct sts_entry_fx00 *)rsp->rsp_pkt;

		rsp->ring_index++;
		if (rsp->ring_index == rsp->length) {
			rsp->ring_index = 0;
			rsp->ring_ptr = rsp->ring;
		} else {
			rsp->ring_ptr++;
		}

		if (pkt->entry_status != 0 &&
		    pkt->entry_type != IOCTL_IOSB_TYPE_FX00) {
			qlafx00_error_entry(vha, rsp,
			    (struct sts_entry_fx00 *)pkt, pkt->entry_status,
			    pkt->entry_type);
			goto next_iter;
			continue;
		}

		switch (pkt->entry_type) {
		case STATUS_TYPE_FX00:
			qlafx00_status_entry(vha, rsp, pkt);
			break;

		case STATUS_CONT_TYPE_FX00:
			qlafx00_status_cont_entry(rsp, (sts_cont_entry_t *)pkt);
			break;

		case MULTI_STATUS_TYPE_FX00:
			qlafx00_multistatus_entry(vha, rsp, pkt);
			break;

		case ABORT_IOCB_TYPE_FX00:
			qlafx00_abort_iocb_entry(vha, rsp->req,
			   (abort_iocb_entry_fx00_t *)pkt);
			break;

		case IOCTL_IOSB_TYPE_FX00:
			qlafx00_ioctl_iosb_entry(vha, rsp->req,
			    (ioctl_iocb_entry_fx00_t *)pkt);
			break;
		default:
			/* Type Not Supported. */
			ql_dbg(ql_dbg_async, vha, 0x5042,
			    "Received unknown response pkt type %x "
			    "entry status=%x.\n",
			    pkt->entry_type, pkt->entry_status);
			break;
		}
next_iter:
		WRT_REG_DWORD(&lptr->signature, RESPONSE_PROCESSED);
		wmb();
	}

	/* Adjust ring index */
	WRT_REG_DWORD(rsp->rsp_q_out, rsp->ring_index);
}

/**
 * qlafx00_async_event() - Process aynchronous events.
 * @ha: SCSI driver HA context
 */
void
qlafx00_async_event(scsi_qla_host_t *vha)
{
	struct qla_hw_data *ha = vha->hw;
	struct device_reg_fx00 __iomem *reg;
	int data_size = 1;

	reg = &ha->iobase->ispfx00;
	/* Setup to process RIO completion. */
	switch (ha->aenmb[0]) {
	case QLAFX00_MBA_SYSTEM_ERR:		/* System Error */
		ql_log(ql_log_warn, vha, 0x5079,
		    "ISP System Error - mbx1=%x\n", ha->aenmb[0]);
		set_bit(ISP_ABORT_NEEDED, &vha->dpc_flags);
		break;

	case QLAFX00_MBA_SHUTDOWN_RQSTD:	/* Shutdown requested */
		ql_dbg(ql_dbg_async, vha, 0x5076,
		    "Asynchronous FW shutdown requested.\n");
		set_bit(ISP_ABORT_NEEDED, &vha->dpc_flags);
		qla2xxx_wake_dpc(vha);
		break;

	case QLAFX00_MBA_PORT_UPDATE:		/* Port database update */
		ha->aenmb[1] = RD_REG_WORD(&reg->aenmailbox1);
		ha->aenmb[2] = RD_REG_WORD(&reg->aenmailbox2);
		ha->aenmb[3] = RD_REG_WORD(&reg->aenmailbox3);
		ql_dbg(ql_dbg_async, vha, 0x5077,
		    "Asynchronous port Update received "
		    "aenmb[0]: %x, aenmb[1]: %x, aenmb[2]: %x, aenmb[3]: %x\n",
		    ha->aenmb[0], ha->aenmb[1], ha->aenmb[2], ha->aenmb[3]);
		data_size = 4;
		break;

	case QLAFX00_MBA_TEMP_OVER:	/* Over temperature event */
		ql_log(ql_log_info, vha, 0x5085,
		    "Asynchronous over temperature event received "
		    "aenmb[0]: %x\n",
		    ha->aenmb[0]);
		break;

	case QLAFX00_MBA_TEMP_NORM:	/* Normal temperature event */
		ql_log(ql_log_info, vha, 0x5086,
		    "Asynchronous normal temperature event received "
		    "aenmb[0]: %x\n",
		    ha->aenmb[0]);
		break;

	case QLAFX00_MBA_TEMP_CRIT:	/* Critical temperature event */
		ql_log(ql_log_info, vha, 0x5084,
		    "Asynchronous critical temperature event received "
		    "aenmb[0]: %x\n",
		ha->aenmb[0]);
		qlafx00_post_aenfx_work(vha, ha->aenmb[0],
		    (uint32_t *)ha->aenmb, 1);
		break;

	default:
		ha->aenmb[1] = RD_REG_WORD(&reg->aenmailbox1);
		ha->aenmb[2] = RD_REG_WORD(&reg->aenmailbox2);
		ha->aenmb[3] = RD_REG_WORD(&reg->aenmailbox3);
		ha->aenmb[4] = RD_REG_WORD(&reg->aenmailbox4);
		ha->aenmb[5] = RD_REG_WORD(&reg->aenmailbox5);
		ha->aenmb[6] = RD_REG_WORD(&reg->aenmailbox6);
		ha->aenmb[7] = RD_REG_WORD(&reg->aenmailbox7);
		ql_dbg(ql_dbg_async, vha, 0x5078,
		    "AEN:%04x %04x %04x %04x :%04x %04x %04x %04x\n",
		    ha->aenmb[0], ha->aenmb[1], ha->aenmb[2], ha->aenmb[3],
		    ha->aenmb[4], ha->aenmb[5], ha->aenmb[6], ha->aenmb[7]);
		break;
	}
	qlafx00_post_aenfx_work(vha, ha->aenmb[0],
	    (uint32_t *)ha->aenmb, data_size);
}

/**
 *
 * qlafx00x_mbx_completion() - Process mailbox command completions.
 * @ha: SCSI driver HA context
 * @mb16: Mailbox16 register
 */
static void
qlafx00_mbx_completion(scsi_qla_host_t *vha, uint32_t mb0)
{
	uint16_t	cnt;
	uint16_t __iomem *wptr;
	struct qla_hw_data *ha = vha->hw;
	struct device_reg_fx00 __iomem *reg = &ha->iobase->ispfx00;

	if (!ha->mcp32)
		ql_dbg(ql_dbg_async, vha, 0x507e, "MBX pointer ERROR.\n");

	/* Load return mailbox registers. */
	ha->flags.mbox_int = 1;
	ha->mailbox_out32[0] = mb0;
	wptr = (uint16_t __iomem *)&reg->mailbox17;

	for (cnt = 1; cnt < ha->mbx_count; cnt++) {
		ha->mailbox_out32[cnt] = RD_REG_WORD(wptr);
		wptr++;
	}
}

/**
 * qlafx00_intr_handler() - Process interrupts for the ISPFX00.
 * @irq:
 * @dev_id: SCSI driver HA context
 *
 * Called by system whenever the host adapter generates an interrupt.
 *
 * Returns handled flag.
 */
irqreturn_t
qlafx00_intr_handler(int irq, void *dev_id)
{
	scsi_qla_host_t	*vha;
	struct qla_hw_data *ha;
	struct device_reg_fx00 __iomem *reg;
	int		status;
	unsigned long	iter;
	uint32_t	stat;
	uint32_t	mb[8];
	struct rsp_que *rsp;
	unsigned long	flags;
	uint32_t clr_intr = 0;

	rsp = (struct rsp_que *) dev_id;
	if (!rsp) {
		ql_log(ql_log_info, NULL, 0x507d,
		    "%s: NULL response queue pointer.\n", __func__);
		return IRQ_NONE;
	}

	ha = rsp->hw;
	reg = &ha->iobase->ispfx00;
	status = 0;

	if (unlikely(pci_channel_offline(ha->pdev)))
		return IRQ_HANDLED;

	spin_lock_irqsave(&ha->hardware_lock, flags);
	vha = pci_get_drvdata(ha->pdev);
	for (iter = 50; iter--; clr_intr = 0) {
		stat = QLAFX00_RD_INTR_REG(ha);
		if (qla2x00_check_reg_for_disconnect(vha, stat))
			break;
		if ((stat & QLAFX00_HST_INT_STS_BITS) == 0)
			break;

		switch (stat & QLAFX00_HST_INT_STS_BITS) {
		case QLAFX00_INTR_MB_CMPLT:
		case QLAFX00_INTR_MB_RSP_CMPLT:
		case QLAFX00_INTR_MB_ASYNC_CMPLT:
		case QLAFX00_INTR_ALL_CMPLT:
			mb[0] = RD_REG_WORD(&reg->mailbox16);
			qlafx00_mbx_completion(vha, mb[0]);
			status |= MBX_INTERRUPT;
			clr_intr |= QLAFX00_INTR_MB_CMPLT;
			break;
		case QLAFX00_INTR_ASYNC_CMPLT:
		case QLAFX00_INTR_RSP_ASYNC_CMPLT:
			ha->aenmb[0] = RD_REG_WORD(&reg->aenmailbox0);
			qlafx00_async_event(vha);
			clr_intr |= QLAFX00_INTR_ASYNC_CMPLT;
			break;
		case QLAFX00_INTR_RSP_CMPLT:
			qlafx00_process_response_queue(vha, rsp);
			clr_intr |= QLAFX00_INTR_RSP_CMPLT;
			break;
		default:
			ql_dbg(ql_dbg_async, vha, 0x507a,
			    "Unrecognized interrupt type (%d).\n", stat);
			break;
		}
		QLAFX00_CLR_INTR_REG(ha, clr_intr);
		QLAFX00_RD_INTR_REG(ha);
	}

	qla2x00_handle_mbx_completion(ha, status);
	spin_unlock_irqrestore(&ha->hardware_lock, flags);

	return IRQ_HANDLED;
}

/** QLAFX00 specific IOCB implementation functions */

static inline cont_a64_entry_t *
qlafx00_prep_cont_type1_iocb(struct req_que *req,
    cont_a64_entry_t *lcont_pkt)
{
	cont_a64_entry_t *cont_pkt;

	/* Adjust ring index. */
	req->ring_index++;
	if (req->ring_index == req->length) {
		req->ring_index = 0;
		req->ring_ptr = req->ring;
	} else {
		req->ring_ptr++;
	}

	cont_pkt = (cont_a64_entry_t *)req->ring_ptr;

	/* Load packet defaults. */
	*((uint32_t *)(&lcont_pkt->entry_type)) =
	    __constant_cpu_to_le32(CONTINUE_A64_TYPE_FX00);

	return cont_pkt;
}

static inline void
qlafx00_build_scsi_iocbs(srb_t *sp, cmd_type_7_fx00_t *cmd_pkt,
    uint16_t tot_dsds, cmd_type_7_fx00_t *lcmd_pkt)
{
	uint16_t	avail_dsds;
	uint32_t	*cur_dsd;
	scsi_qla_host_t	*vha;
	struct scsi_cmnd *cmd;
	struct scatterlist *sg;
	int i, cont;
	struct req_que *req;
	cont_a64_entry_t lcont_pkt;
	cont_a64_entry_t *cont_pkt;

	vha = sp->fcport->vha;
	req = vha->req;

	cmd = GET_CMD_SP(sp);
	cont = 0;
	cont_pkt = NULL;

	/* Update entry type to indicate Command Type 3 IOCB */
	*((uint32_t *)(&lcmd_pkt->entry_type)) =
	    __constant_cpu_to_le32(FX00_COMMAND_TYPE_7);

	/* No data transfer */
	if (!scsi_bufflen(cmd) || cmd->sc_data_direction == DMA_NONE) {
		lcmd_pkt->byte_count = __constant_cpu_to_le32(0);
		return;
	}

	/* Set transfer direction */
	if (cmd->sc_data_direction == DMA_TO_DEVICE) {
		lcmd_pkt->cntrl_flags = (uint8_t)TMF_WRITE_DATA;
		vha->qla_stats.output_bytes += scsi_bufflen(cmd);
	} else if (cmd->sc_data_direction == DMA_FROM_DEVICE) {
		lcmd_pkt->cntrl_flags = (uint8_t)TMF_READ_DATA;
		vha->qla_stats.input_bytes += scsi_bufflen(cmd);
	}

	/* One DSD is available in the Command Type 3 IOCB */
	avail_dsds = 1;
	cur_dsd = (uint32_t *)&lcmd_pkt->dseg_0_address;

	/* Load data segments */
	scsi_for_each_sg(cmd, sg, tot_dsds, i) {
		dma_addr_t	sle_dma;

		/* Allocate additional continuation packets? */
		if (avail_dsds == 0) {
			/*
			 * Five DSDs are available in the Continuation
			 * Type 1 IOCB.
			 */
			memset(&lcont_pkt, 0, REQUEST_ENTRY_SIZE);
			cont_pkt = qlafx00_prep_cont_type1_iocb(req, &lcont_pkt);
			cur_dsd = (uint32_t *)lcont_pkt.dseg_0_address;
			avail_dsds = 5;
			cont = 1;
		}

		sle_dma = sg_dma_address(sg);
		*cur_dsd++ = cpu_to_le32(LSD(sle_dma));
		*cur_dsd++ = cpu_to_le32(MSD(sle_dma));
		*cur_dsd++ = cpu_to_le32(sg_dma_len(sg));
		avail_dsds--;
		if (avail_dsds == 0 && cont == 1) {
			cont = 0;
			memcpy_toio((void __iomem *)cont_pkt, &lcont_pkt,
			    REQUEST_ENTRY_SIZE);
		}

	}
	if (avail_dsds != 0 && cont == 1) {
		memcpy_toio((void __iomem *)cont_pkt, &lcont_pkt,
		    REQUEST_ENTRY_SIZE);
	}
}

/**
 * qlafx00_start_scsi() - Send a SCSI command to the ISP
 * @sp: command to send to the ISP
 *
 * Returns non-zero if a failure occurred, else zero.
 */
int
qlafx00_start_scsi(srb_t *sp)
{
	int		ret, nseg;
	unsigned long   flags;
	uint32_t        index;
	uint32_t	handle;
	uint16_t	cnt;
	uint16_t	req_cnt;
	uint16_t	tot_dsds;
	struct req_que *req = NULL;
	struct rsp_que *rsp = NULL;
	struct scsi_cmnd *cmd = GET_CMD_SP(sp);
	struct scsi_qla_host *vha = sp->fcport->vha;
	struct qla_hw_data *ha = vha->hw;
	cmd_type_7_fx00_t *cmd_pkt;
	cmd_type_7_fx00_t lcmd_pkt;
	struct scsi_lun llun;
	char		tag[2];

	/* Setup device pointers. */
	ret = 0;

	rsp = ha->rsp_q_map[0];
	req = vha->req;

	/* So we know we haven't pci_map'ed anything yet */
	tot_dsds = 0;

	/* Forcing marker needed for now */
	vha->marker_needed = 0;

	/* Send marker if required */
	if (vha->marker_needed != 0) {
		if (qla2x00_marker(vha, req, rsp, 0, 0, MK_SYNC_ALL) !=
		    QLA_SUCCESS)
			return QLA_FUNCTION_FAILED;
		vha->marker_needed = 0;
	}

	/* Acquire ring specific lock */
	spin_lock_irqsave(&ha->hardware_lock, flags);

	/* Check for room in outstanding command list. */
	handle = req->current_outstanding_cmd;
	for (index = 1; index < req->num_outstanding_cmds; index++) {
		handle++;
		if (handle == req->num_outstanding_cmds)
			handle = 1;
		if (!req->outstanding_cmds[handle])
			break;
	}
	if (index == req->num_outstanding_cmds)
		goto queuing_error;

	/* Map the sg table so we have an accurate count of sg entries needed */
	if (scsi_sg_count(cmd)) {
		nseg = dma_map_sg(&ha->pdev->dev, scsi_sglist(cmd),
		    scsi_sg_count(cmd), cmd->sc_data_direction);
		if (unlikely(!nseg))
			goto queuing_error;
	} else
		nseg = 0;

	tot_dsds = nseg;
	req_cnt = qla24xx_calc_iocbs(vha, tot_dsds);
	if (req->cnt < (req_cnt + 2)) {
		cnt = RD_REG_DWORD_RELAXED(req->req_q_out);

		if (req->ring_index < cnt)
			req->cnt = cnt - req->ring_index;
		else
			req->cnt = req->length -
				(req->ring_index - cnt);
		if (req->cnt < (req_cnt + 2))
			goto queuing_error;
	}

	/* Build command packet. */
	req->current_outstanding_cmd = handle;
	req->outstanding_cmds[handle] = sp;
	sp->handle = handle;
	cmd->host_scribble = (unsigned char *)(unsigned long)handle;
	req->cnt -= req_cnt;

	cmd_pkt = (struct cmd_type_7_fx00 *)req->ring_ptr;

	memset(&lcmd_pkt, 0, REQUEST_ENTRY_SIZE);

	lcmd_pkt.handle = MAKE_HANDLE(req->id, sp->handle);
	lcmd_pkt.handle_hi = 0;
	lcmd_pkt.dseg_count = cpu_to_le16(tot_dsds);
	lcmd_pkt.tgt_idx = cpu_to_le16(sp->fcport->tgt_id);

	int_to_scsilun(cmd->device->lun, &llun);
	host_to_adap((uint8_t *)&llun, (uint8_t *)&lcmd_pkt.lun,
	    sizeof(lcmd_pkt.lun));

	/* Update tagged queuing modifier -- default is TSK_SIMPLE (0). */
	if (scsi_populate_tag_msg(cmd, tag)) {
		switch (tag[0]) {
		case HEAD_OF_QUEUE_TAG:
			lcmd_pkt.task = TSK_HEAD_OF_QUEUE;
			break;
		case ORDERED_QUEUE_TAG:
			lcmd_pkt.task = TSK_ORDERED;
			break;
		}
	}

	/* Load SCSI command packet. */
	host_to_adap(cmd->cmnd, lcmd_pkt.fcp_cdb, sizeof(lcmd_pkt.fcp_cdb));
	lcmd_pkt.byte_count = cpu_to_le32((uint32_t)scsi_bufflen(cmd));

	/* Build IOCB segments */
	qlafx00_build_scsi_iocbs(sp, cmd_pkt, tot_dsds, &lcmd_pkt);

	/* Set total data segment count. */
	lcmd_pkt.entry_count = (uint8_t)req_cnt;

	/* Specify response queue number where completion should happen */
	lcmd_pkt.entry_status = (uint8_t) rsp->id;

	ql_dump_buffer(ql_dbg_io + ql_dbg_buffer, vha, 0x302e,
	    (uint8_t *)cmd->cmnd, cmd->cmd_len);
	ql_dump_buffer(ql_dbg_io + ql_dbg_buffer, vha, 0x3032,
	    (uint8_t *)&lcmd_pkt, REQUEST_ENTRY_SIZE);

	memcpy_toio((void __iomem *)cmd_pkt, &lcmd_pkt, REQUEST_ENTRY_SIZE);
	wmb();

	/* Adjust ring index. */
	req->ring_index++;
	if (req->ring_index == req->length) {
		req->ring_index = 0;
		req->ring_ptr = req->ring;
	} else
		req->ring_ptr++;

	sp->flags |= SRB_DMA_VALID;

	/* Set chip new ring index. */
	WRT_REG_DWORD(req->req_q_in, req->ring_index);
	QLAFX00_SET_HST_INTR(ha, ha->rqstq_intr_code);

	spin_unlock_irqrestore(&ha->hardware_lock, flags);
	return QLA_SUCCESS;

queuing_error:
	if (tot_dsds)
		scsi_dma_unmap(cmd);

	spin_unlock_irqrestore(&ha->hardware_lock, flags);

	return QLA_FUNCTION_FAILED;
}

void
qlafx00_tm_iocb(srb_t *sp, struct tsk_mgmt_entry_fx00 *ptm_iocb)
{
	struct srb_iocb *fxio = &sp->u.iocb_cmd;
	scsi_qla_host_t *vha = sp->fcport->vha;
	struct req_que *req = vha->req;
	struct tsk_mgmt_entry_fx00 tm_iocb;
	struct scsi_lun llun;

	memset(&tm_iocb, 0, sizeof(struct tsk_mgmt_entry_fx00));
	tm_iocb.entry_type = TSK_MGMT_IOCB_TYPE_FX00;
	tm_iocb.entry_count = 1;
	tm_iocb.handle = cpu_to_le32(MAKE_HANDLE(req->id, sp->handle));
	tm_iocb.handle_hi = 0;
	tm_iocb.timeout = cpu_to_le16(qla2x00_get_async_timeout(vha) + 2);
	tm_iocb.tgt_id = cpu_to_le16(sp->fcport->tgt_id);
	tm_iocb.control_flags = cpu_to_le32(fxio->u.tmf.flags);
	if (tm_iocb.control_flags == TCF_LUN_RESET) {
		int_to_scsilun(fxio->u.tmf.lun, &llun);
		host_to_adap((uint8_t *)&llun, (uint8_t *)&tm_iocb.lun,
		    sizeof(struct scsi_lun));
	}

	memcpy((void __iomem *)ptm_iocb, &tm_iocb,
	    sizeof(struct tsk_mgmt_entry_fx00));
	wmb();
}

void
qlafx00_abort_iocb(srb_t *sp, struct abort_iocb_entry_fx00 *pabt_iocb)
{
	struct srb_iocb *fxio = &sp->u.iocb_cmd;
	scsi_qla_host_t *vha = sp->fcport->vha;
	struct req_que *req = vha->req;
	struct abort_iocb_entry_fx00 abt_iocb;

	memset(&abt_iocb, 0, sizeof(struct abort_iocb_entry_fx00));
	abt_iocb.entry_type = ABORT_IOCB_TYPE_FX00;
	abt_iocb.entry_count = 1;
	abt_iocb.handle = cpu_to_le32(MAKE_HANDLE(req->id, sp->handle));
	abt_iocb.abort_handle =
	    cpu_to_le32(MAKE_HANDLE(req->id, fxio->u.abt.cmd_hndl));
	abt_iocb.tgt_id_sts = cpu_to_le16(sp->fcport->tgt_id);
	abt_iocb.req_que_no = cpu_to_le16(req->id);

	memcpy((void __iomem *)pabt_iocb, &abt_iocb,
	    sizeof(struct abort_iocb_entry_fx00));
	wmb();
}

void
qlafx00_fxdisc_iocb(srb_t *sp, struct fxdisc_entry_fx00 *pfxiocb)
{
	struct srb_iocb *fxio = &sp->u.iocb_cmd;
	qla_mt_iocb_rqst_fx00_t *piocb_rqst;
	struct fc_bsg_job *bsg_job;
	struct fxdisc_entry_fx00 fx_iocb;
	uint8_t entry_cnt = 1;

	memset(&fx_iocb, 0, sizeof(struct fxdisc_entry_fx00));
	fx_iocb.entry_type = FX00_IOCB_TYPE;
	fx_iocb.handle = cpu_to_le32(sp->handle);
	fx_iocb.entry_count = entry_cnt;

	if (sp->type == SRB_FXIOCB_DCMD) {
		fx_iocb.func_num =
		    cpu_to_le16(sp->u.iocb_cmd.u.fxiocb.req_func_type);
		fx_iocb.adapid = cpu_to_le32(fxio->u.fxiocb.adapter_id);
		fx_iocb.adapid_hi = cpu_to_le32(fxio->u.fxiocb.adapter_id_hi);
		fx_iocb.reserved_0 = cpu_to_le32(fxio->u.fxiocb.reserved_0);
		fx_iocb.reserved_1 = cpu_to_le32(fxio->u.fxiocb.reserved_1);
		fx_iocb.dataword_extra = cpu_to_le32(fxio->u.fxiocb.req_data_extra);

		if (fxio->u.fxiocb.flags & SRB_FXDISC_REQ_DMA_VALID) {
			fx_iocb.req_dsdcnt = cpu_to_le16(1);
			fx_iocb.req_xfrcnt =
			    cpu_to_le16(fxio->u.fxiocb.req_len);
			fx_iocb.dseg_rq_address[0] =
			    cpu_to_le32(LSD(fxio->u.fxiocb.req_dma_handle));
			fx_iocb.dseg_rq_address[1] =
			    cpu_to_le32(MSD(fxio->u.fxiocb.req_dma_handle));
			fx_iocb.dseg_rq_len =
			    cpu_to_le32(fxio->u.fxiocb.req_len);
		}

		if (fxio->u.fxiocb.flags & SRB_FXDISC_RESP_DMA_VALID) {
			fx_iocb.rsp_dsdcnt = cpu_to_le16(1);
			fx_iocb.rsp_xfrcnt =
			    cpu_to_le16(fxio->u.fxiocb.rsp_len);
			fx_iocb.dseg_rsp_address[0] =
			    cpu_to_le32(LSD(fxio->u.fxiocb.rsp_dma_handle));
			fx_iocb.dseg_rsp_address[1] =
			    cpu_to_le32(MSD(fxio->u.fxiocb.rsp_dma_handle));
			fx_iocb.dseg_rsp_len =
			    cpu_to_le32(fxio->u.fxiocb.rsp_len);
		}

		if (fxio->u.fxiocb.flags & SRB_FXDISC_REQ_DWRD_VALID) {
			fx_iocb.dataword =
			    cpu_to_le32(fxio->u.fxiocb.req_data);
		}
		fx_iocb.flags = fxio->u.fxiocb.flags;
	} else {
		struct scatterlist *sg;
		bsg_job = sp->u.bsg_job;
		piocb_rqst = (qla_mt_iocb_rqst_fx00_t *)
			&bsg_job->request->rqst_data.h_vendor.vendor_cmd[1];

		fx_iocb.func_num = piocb_rqst->func_type;
		fx_iocb.adapid = piocb_rqst->adapid;
		fx_iocb.adapid_hi = piocb_rqst->adapid_hi;
		fx_iocb.reserved_0 = piocb_rqst->reserved_0;
		fx_iocb.reserved_1 = piocb_rqst->reserved_1;
		fx_iocb.dataword_extra = piocb_rqst->dataword_extra;
		fx_iocb.dataword = piocb_rqst->dataword;
		fx_iocb.req_xfrcnt = cpu_to_le16(piocb_rqst->req_len);
		fx_iocb.rsp_xfrcnt = cpu_to_le16(piocb_rqst->rsp_len);

		if (piocb_rqst->flags & SRB_FXDISC_REQ_DMA_VALID) {
			int avail_dsds, tot_dsds;
			cont_a64_entry_t lcont_pkt;
			cont_a64_entry_t *cont_pkt = NULL;
			uint32_t *cur_dsd;
			int index = 0, cont = 0;

			fx_iocb.req_dsdcnt =
			    cpu_to_le16(bsg_job->request_payload.sg_cnt);
			tot_dsds = cpu_to_le32(bsg_job->request_payload.sg_cnt);
			cur_dsd = (uint32_t *)&fx_iocb.dseg_rq_address[0];
			avail_dsds = 1;
			for_each_sg(bsg_job->request_payload.sg_list, sg,
			    tot_dsds, index) {
				dma_addr_t sle_dma;

				/* Allocate additional continuation packets? */
				if (avail_dsds == 0) {
					  /*
					  * Five DSDs are available in the Cont.
					  * Type 1 IOCB.
					  */
					  memset(&lcont_pkt, 0, REQUEST_ENTRY_SIZE);
					  cont_pkt =
					      qlafx00_prep_cont_type1_iocb(
						sp->fcport->vha->req, &lcont_pkt);
					  cur_dsd = (uint32_t *)lcont_pkt.dseg_0_address;
					  avail_dsds = 5;
					  cont = 1;
					  entry_cnt++;
				   }

				   sle_dma = sg_dma_address(sg);
				   *cur_dsd++   = cpu_to_le32(LSD(sle_dma));
				   *cur_dsd++   = cpu_to_le32(MSD(sle_dma));
				   *cur_dsd++   = cpu_to_le32(sg_dma_len(sg));
				   avail_dsds--;

				   if (avail_dsds == 0 && cont == 1) {
					  cont = 0;
					  memcpy_toio((void __iomem *)cont_pkt, &lcont_pkt,
					      REQUEST_ENTRY_SIZE);
					  ql_dump_buffer(ql_dbg_user + ql_dbg_verbose,
					      sp->fcport->vha, 0x3042,
					      (uint8_t *)&lcont_pkt, REQUEST_ENTRY_SIZE);
				   }
			    }
			    if (avail_dsds != 0 && cont == 1) {
				   memcpy_toio((void __iomem *)cont_pkt, &lcont_pkt,
				       REQUEST_ENTRY_SIZE);
				   ql_dump_buffer(ql_dbg_user + ql_dbg_verbose,
				       sp->fcport->vha, 0x3043,
				       (uint8_t *)&lcont_pkt, REQUEST_ENTRY_SIZE);
			    }
		}

		if (piocb_rqst->flags & SRB_FXDISC_RESP_DMA_VALID) {
			int avail_dsds, tot_dsds;
			cont_a64_entry_t lcont_pkt;
			cont_a64_entry_t *cont_pkt = NULL;
			uint32_t *cur_dsd;
			int index = 0, cont = 0;

			fx_iocb.rsp_dsdcnt =
			   cpu_to_le16(bsg_job->reply_payload.sg_cnt);
			tot_dsds = cpu_to_le32(bsg_job->reply_payload.sg_cnt);
			cur_dsd = (uint32_t *)&fx_iocb.dseg_rsp_address[0];
			avail_dsds = 1;

			for_each_sg(bsg_job->reply_payload.sg_list, sg,
			    tot_dsds, index) {
				dma_addr_t sle_dma;

				/* Allocate additional continuation packets? */
				if (avail_dsds == 0) {
					/*
					* Five DSDs are available in the Cont.
					* Type 1 IOCB.
					*/
					memset(&lcont_pkt, 0, REQUEST_ENTRY_SIZE);
					cont_pkt =
					    qlafx00_prep_cont_type1_iocb(
						sp->fcport->vha->req, &lcont_pkt);
					cur_dsd = (uint32_t *)lcont_pkt.dseg_0_address;
					avail_dsds = 5;
					cont = 1;
					entry_cnt++;
				}

				sle_dma = sg_dma_address(sg);
				*cur_dsd++   = cpu_to_le32(LSD(sle_dma));
				*cur_dsd++   = cpu_to_le32(MSD(sle_dma));
				*cur_dsd++   = cpu_to_le32(sg_dma_len(sg));
				avail_dsds--;

				if (avail_dsds == 0 && cont == 1) {
					cont = 0;
					memcpy_toio((void __iomem *)cont_pkt, &lcont_pkt,
					    REQUEST_ENTRY_SIZE);
					ql_dump_buffer(ql_dbg_user + ql_dbg_verbose,
					    sp->fcport->vha, 0x3045,
					    (uint8_t *)&lcont_pkt, REQUEST_ENTRY_SIZE);
				}
			}
			if (avail_dsds != 0 && cont == 1) {
				memcpy_toio((void __iomem *)cont_pkt, &lcont_pkt,
				    REQUEST_ENTRY_SIZE);
				ql_dump_buffer(ql_dbg_user + ql_dbg_verbose,
				    sp->fcport->vha, 0x3046,
				    (uint8_t *)&lcont_pkt, REQUEST_ENTRY_SIZE);
			}
		}

		if (piocb_rqst->flags & SRB_FXDISC_REQ_DWRD_VALID)
			fx_iocb.dataword = cpu_to_le32(piocb_rqst->dataword);
		fx_iocb.flags = piocb_rqst->flags;
		fx_iocb.entry_count = entry_cnt;
	}

	ql_dump_buffer(ql_dbg_user + ql_dbg_verbose,
	    sp->fcport->vha, 0x3047,
	    (uint8_t *)&fx_iocb, sizeof(struct fxdisc_entry_fx00));

	memcpy((void __iomem *)pfxiocb, &fx_iocb,
	    sizeof(struct fxdisc_entry_fx00));
	wmb();
}
