/*
 *  qla2x00t.h
 *
 *  Copyright (C) 2004 - 2014 Vladislav Bolkhovitin <vst@vlnb.net>
 *  Copyright (C) 2004 - 2005 Leonid Stoljar
 *  Copyright (C) 2006 Nathaniel Clark <nate@misrule.us>
 *  Copyright (C) 2007 - 2014 Fusion-io, Inc.
 *
 *  QLogic 22xx+ FC target driver.
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation, version 2
 *  of the License.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 */

#ifndef __QLA2X00T_H
#define __QLA2X00T_H

#include <qla_def.h>
#include <qla2x_tgt.h>
#include <qla2x_tgt_def.h>

#include <scst_debug.h>

/* Version numbers, the same as for the kernel */
#define Q2T_VERSION(a, b, c, d)	(((a) << 030) + ((b) << 020) + (c) << 010 + (d))
#define Q2T_VERSION_CODE	Q2T_VERSION(8, 0, 0, 0)
#define Q2T_VERSION_STRING	"8.0.0-pre1-TRUNK-15"
#define Q2T_PROC_VERSION_NAME	"version"

#define Q2T_MAX_CDB_LEN             16
#define Q2T_TIMEOUT                 10	/* in seconds */

#define Q2T_MAX_HW_PENDING_TIME	    60 /* in seconds */

/* Immediate notify status constants */
#define IMM_NTFY_LIP_RESET          0x000E
#define IMM_NTFY_LIP_LINK_REINIT    0x000F
#define IMM_NTFY_IOCB_OVERFLOW      0x0016
#define IMM_NTFY_ABORT_TASK         0x0020
#define IMM_NTFY_PORT_LOGOUT        0x0029
#define IMM_NTFY_PORT_CONFIG        0x002A
#define IMM_NTFY_GLBL_TPRLO         0x002D
#define IMM_NTFY_GLBL_LOGO          0x002E
#define IMM_NTFY_RESOURCE           0x0034
#define IMM_NTFY_MSG_RX             0x0036
#define IMM_NTFY_SRR                0x0045
#define IMM_NTFY_ELS                0x0046
#define IMM_NTFY_VN2VN_FIP	    0x0047

/* Immediate notify task flags */
#define IMM_NTFY_TASK_MGMT_SHIFT    8

#define Q2T_CLEAR_ACA               0x40
#define Q2T_TARGET_RESET            0x20
#define Q2T_LUN_RESET               0x10
#define Q2T_CLEAR_TS                0x04
#define Q2T_ABORT_TS                0x02
#define Q2T_ABORT_ALL_SESS          0xFFFF
#define Q2T_ABORT_ALL               0xFFFE
#define Q2T_NEXUS_LOSS_SESS         0xFFFD
#define Q2T_NEXUS_LOSS              0xFFFC

/* Notify Acknowledge flags */
#define NOTIFY_ACK_RES_COUNT        BIT_8
#define NOTIFY_ACK_CLEAR_LIP_RESET  BIT_5
#define NOTIFY_ACK_TM_RESP_CODE_VALID BIT_4

/* Command's states */
#define Q2T_STATE_NEW               0	/* New command and SCST processing it */
#define Q2T_STATE_NEED_DATA         1	/* SCST needs data to continue */
#define Q2T_STATE_DATA_IN           2	/* Data arrived and SCST processing it */
#define Q2T_STATE_PROCESSED         3	/* SCST done processing */
#define Q2T_STATE_ABORTED           4	/* Command aborted */

/*
 * Special handles.
 * !! Changing them here, make sure that mask in the beginning of
 * !! q2t_ctio_to_cmd() changed accordingly!
 */
#define Q2T_NULL_HANDLE             0
#define Q2T_SKIP_HANDLE             (0xFFFFFFFF & ~(CTIO_COMPLETION_HANDLE_MARK|CTIO_INTERMEDIATE_HANDLE_MARK))

/* ATIO task_codes field */
#define ATIO_SIMPLE_QUEUE           0
#define ATIO_HEAD_OF_QUEUE          1
#define ATIO_ORDERED_QUEUE          2
#define ATIO_ACA_QUEUE              4
#define ATIO_UNTAGGED               5

/* TM failed response codes, see FCP (9.4.11 FCP_RSP_INFO) */
#define	FC_TM_SUCCESS               0
#define	FC_TM_BAD_FCP_DATA          1
#define	FC_TM_BAD_CMD               2
#define	FC_TM_FCP_DATA_MISMATCH     3
#define	FC_TM_REJECT                4
#define FC_TM_FAILED                5

#ifndef scst_sense_internal_failure
#define scst_sense_internal_failure     ABORTED_COMMAND, 0x44, 0
#endif


/*
 * Error code of q2t_pre_xmit_response() meaning that cmd's exchange was
 * terminated, so no more actions is needed and success should be returned
 * to SCST. Must be different from any SCST_TGT_RES_* codes.
 */
#define Q2T_PRE_XMIT_RESP_CMD_ABORTED	0x1717

#if (BITS_PER_LONG > 32) || defined(CONFIG_HIGHMEM64G)
#define pci_dma_lo32(a) (a & 0xffffffff)
#define pci_dma_hi32(a) ((((a) >> 16)>>16) & 0xffffffff)
#else
#define pci_dma_lo32(a) (a & 0xffffffff)
#define pci_dma_hi32(a) 0
#endif

struct q2t_tgt {
	struct scst_tgt *scst_tgt;
	scsi_qla_host_t *ha;

	/*
	 * To sync between IRQ handlers and q2t_target_release(). Needed,
	 * because req_pkt() can drop/reacquire HW lock inside. Protected by
	 * HW lock.
	 */
	int irq_cmd_count;

	int datasegs_per_cmd, datasegs_per_cont;

#ifdef QLT_LOOP_BACK
	uint8_t *data_buf;
	struct scatterlist data_sg;
#endif /* QLT_LOOP_BACK */

	/* Target's flags, serialized by pha->hardware_lock */
	unsigned int tgt_enable_64bit_addr:1;	/* 64-bits PCI addressing enabled */
	unsigned int link_reinit_iocb_pending:1;
	unsigned int tm_to_unknown:1; /* TM to unknown session was sent */
	unsigned int sess_works_pending:1; /* there are sess_work entries */

	/*
	 * Protected by tgt_mutex AND hardware_lock for writing and tgt_mutex
	 * OR hardware_lock for reading.
	 */
	unsigned long tgt_stop; /* the driver is being stopped */

	/* Count of sessions refering q2t_tgt. Protected by hardware_lock. */
	int sess_count;

	/*
	 * Protected by hardware_lock. Adding new sessions (not undelete)
	 * also protected by tgt_mutex.
	 */
	struct list_head sess_list;

	/* Protected by hardware_lock */
	struct list_head del_sess_list;
	struct delayed_work sess_del_work;

	spinlock_t sess_work_lock;
	struct list_head sess_works_list;
	struct work_struct sess_work;

	notify24xx_entry_t link_reinit_iocb;
	wait_queue_head_t waitQ;
	int notify_ack_expected;
	int abts_resp_expected;
	int modify_lun_expected;

	int ctio_srr_id;
	int imm_srr_id;
	spinlock_t srr_lock;
	struct list_head srr_ctio_list;
	struct list_head srr_imm_list;
	struct work_struct srr_work;

	atomic_t tgt_global_resets_count;

	struct list_head tgt_list_entry;
};

/* Login states */
#define Q2T_LOGIN_STATE_UNINITIALIZED	    0
#define Q2T_LOGIN_STATE_FLOGI_COMPLETED	    1
#define Q2T_LOGIN_STATE_PLOGI_COMPLETED	    2
#define Q2T_LOGIN_STATE_PRLI_COMPLETED	    3
#define Q2T_LOGIN_STATE_NONE		    0xFF

typedef enum {
	Q2T_CFLAG_CMD_ALLOC = BIT_0,
	Q2T_CFLAG_EXEC_SESS_WK = BIT_1,
	Q2T_CFLAG_XMIT_RSP = BIT_2,
	Q2T_CFLAG_ABORTED  = BIT_3,
	Q2T_CFLAG_RDY_2XFER = BIT_4,
	Q2T_CFLAG_ON_FREE = BIT_5,
	Q2T_CFLAG_HW_TO = BIT_6,
} cmd_flags_t;
/*
 * Equivalent to IT Nexus (Initiator-Target)
 */
struct q2t_sess {
	uint16_t loop_id;
	port_id_t s_id;

	unsigned int conf_compl_supported:1;
	unsigned int deleted:1;
	unsigned int local:1;
	unsigned int logout_acc_pending:1;  /* ACC to LOGO or PRLO is pending */
	int login_state;    /* Used in the error handling of FCP_CMDs without */
			    /* proper login session. Values defined above. */
	struct scst_session *scst_sess;
	struct q2t_tgt *tgt;

	int sess_ref; /* protected by hardware_lock */

	struct list_head sess_list_entry;
	unsigned long expires;
	struct list_head del_list_entry;

	uint8_t port_name[WWN_SIZE];
	fc_port_t *qla_fcport;
};

#define MAX_QFULL_CMDS_ALLOC	8192
#define Q_FULL_THRESH_HOLD_PERCENT 100
#define Q_FULL_THRESH_HOLD(ha) \
	((ha->fw_xcb_count/100)* Q_FULL_THRESH_HOLD_PERCENT)

#define LEAK_EXCHG_THRESH_HOLD_PERCENT 75	/* 75 percent */

typedef enum {
	QFULL_NOOP=0,		/* place holder */
	QFULL_SCSI_BUSY,
	QFULL_TERM_EXCHG,
	QFULL_TSK_MGMT,
	QFULL_ABTS,
} qfull_cmd_type_t;

struct qfull_arg {
	uint16_t status; 		/* scsi status/respond_code/ */
	uint16_t loop_id;
	uint32_t resp_code;
	uint32_t ids_reversed:1;
};


struct q2t_cmd {
	struct q2t_sess *sess;
	uint32_t state;

	unsigned int conf_compl_supported:1;/* to save extra sess dereferences */
	unsigned int sg_mapped:1;
	unsigned int free_sg:1;
	unsigned int aborted:1; /* Needed in case of SRR */
	unsigned int write_data_transferred:1;
	unsigned int cmd_sent_to_fw:1;
	unsigned int q_full_ids_reversed:1;

#ifdef QLT_LOOP_BACK
	unsigned int qlb_io:1;
#endif /* QLT_LOOP_BACK */

	qfull_cmd_type_t qftype;
	cmd_flags_t cmd_flags;
	uint64_t alloc_jiff;
	uint64_t free_jiff;

	struct scatterlist *sg;	/* cmd data buffer SG vector */
	int sg_cnt;		/* SG segments count */
	int bufflen;		/* cmd buffer length */
	int offset;
	scst_data_direction data_direction;
	uint32_t tag;
	dma_addr_t dma_handle;
	enum dma_data_direction dma_data_direction;
	uint32_t reset_count;

	uint16_t loop_id;		    /* to save extra sess dereferences */
	struct q2t_tgt *tgt;		    /* to save extra sess dereferences */

	union {
		atio7_entry_t atio7;
		atio_entry_t atio2x;
	} __packed atio;
	struct list_head cmd_list;

#ifdef QLA_RSPQ_NOLOCK
	struct list_head list_entry;	// rx_pendq_list
	response_t	rsp_pkt;	// ctio for error processing
	uint32_t	status;
#endif
	struct scst_cmd scst_cmd;
};

struct q2t_sess_work_param {
	struct list_head sess_works_list_entry;

#define Q2T_SESS_WORK_CMD	0
#define Q2T_SESS_WORK_ABORT	1
#define Q2T_SESS_WORK_TM	2
#define Q2T_SESS_WORK_TERM_WCMD	3	//terminate with & notify scst of cmd.
#define Q2T_SESS_WORK_TERM	4	//terminate without cmd reference
#define Q2T_SESS_WORK_LOGIN     5
	int type;
	int reset_count;

	union {
		struct q2t_cmd *cmd;
		abts24_recv_entry_t abts;
		notify_entry_t tm_iocb;
		atio7_entry_t tm_iocb2;
		notify83xx_entry_t inot;
	};
};

struct q2t_mgmt_cmd {
	struct q2t_sess *sess;
	unsigned int flags;
	uint32_t reset_count;
#define Q24_MGMT_SEND_NACK	1
	union {
		atio7_entry_t atio7;
		notify_entry_t notify_entry;
		notify24xx_entry_t notify_entry24;
		abts24_recv_entry_t abts;
	} __attribute__((packed)) orig_iocb;
};

struct q2t_prm {
	struct q2t_cmd *cmd;
	struct q2t_tgt *tgt;
	void *pkt;
	struct scatterlist *sg;	/* cmd data buffer SG vector */
	int seg_cnt;
	int req_cnt;
	uint16_t rq_result;
	uint16_t scsi_status;
	unsigned char *sense_buffer;
	int sense_buffer_len;
	int residual;
	int add_status_pkt;
};

struct srr_imm {
	struct list_head srr_list_entry;
	int srr_id;
	union {
		notify_entry_t notify_entry;
		notify24xx_entry_t notify_entry24;
	} __attribute__((packed)) imm;
};

struct srr_ctio {
	struct list_head srr_list_entry;
	int srr_id;
	struct q2t_cmd *cmd;
};

#define Q2T_XMIT_DATA		1
#define Q2T_XMIT_STATUS		2
#define Q2T_XMIT_ALL		(Q2T_XMIT_STATUS|Q2T_XMIT_DATA)

#ifdef QLT_LOOP_BACK
extern int qlb_enabled, qlb_lba_low, qlb_lba_high, qlb_debug;

#define QLB_LBA_LOW	(qlb_lba_low)
#define QLB_LBA_HIGH	(qlb_lba_high)
#define QLB_BUFFER_SIZE	(4096)

#define QLB_CMD_CDB(__cmd) (__cmd->atio.atio7.fcp_cmnd.cdb)
#define QLB_CMD_LBA(__cmd) (get_unaligned_be32(&QLB_CMD_CDB(__cmd)[2]))
#define QLB_CMD_SIZE(__cmd) (get_unaligned_be16(&QLB_CMD_CDB(__cmd)[7]) * 512)

#define QLB_READ(__cmd) (QLB_CMD_CDB(__cmd)[0] == 0x28)
#define QLB_WRITE(__cmd) (QLB_CMD_CDB(__cmd)[0] == 0x2a)

#define QLB_LOOP_CMD(__cmd) (QLB_READ(__cmd) || QLB_WRITE(__cmd))
#define QLB_LOOP_LBA(__cmd) ((QLB_CMD_LBA(__cmd) >= QLB_LBA_LOW) && \
				(QLB_CMD_LBA(__cmd) <= QLB_LBA_HIGH))
#define QLB_LOOP_SIZE(__cmd) (QLB_CMD_SIZE(__cmd) <= QLB_BUFFER_SIZE)
#define QLB_ENABLED() (qlb_enabled)
#define QLB_CMD(__cmd) (__cmd->qlb_io)

static inline int
qlb_loop_back_io(struct scsi_qla_host *vha, struct q2t_cmd *cmd)
{
	atio7_entry_t *atio;
	uint8_t *cdb;
	struct qla_hw_data *ha = vha->hw;

	if (!QLB_ENABLED() || !IS_FWI2_CAPABLE(ha))
		return 0;

	atio = &cmd->atio.atio7;
	cdb = atio->fcp_cmnd.cdb;

	if (!QLB_LOOP_CMD(cmd) || !QLB_LOOP_SIZE(cmd) || !QLB_LOOP_LBA(cmd))
		return 0;

	return 1;
}
#endif /* QLT_LOOP_BACK */

extern void q2t_host_reset_handler(struct qla_hw_data *ha);
extern int q2t_free_leak_exchange(struct scsi_qla_host *);
extern void q2t_alloc_term_exchange(struct scsi_qla_host *, atio7_entry_t *);
extern int q2t_free_qfull_cmds(struct scsi_qla_host *);
extern void q2t_init_term_exchange(struct scsi_qla_host *);

#endif /* __QLA2X00T_H */
