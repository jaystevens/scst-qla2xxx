/*
 * QLogic Fibre Channel HBA Driver
 * Copyright (c)  2003-2014 QLogic Corporation
 *
 * See LICENSE.qla2xxx for copyright and licensing details.
 */
#include "qla_def.h"

#include <linux/debugfs.h>
#include <linux/seq_file.h>

static struct dentry *qla2x00_dfs_root;
static atomic_t qla2x00_dfs_root_count;

static int
qla2x00_dfs_fce_show(struct seq_file *s, void *unused)
{
	scsi_qla_host_t *vha = s->private;
	uint32_t cnt;
	uint32_t *fce;
	uint64_t fce_start;
	struct qla_hw_data *ha = vha->hw;

	mutex_lock(&ha->fce_mutex);

	seq_printf(s, "FCE Trace Buffer\n");
	seq_printf(s, "In Pointer = %llx\n\n", (unsigned long long)ha->fce_wr);
	seq_printf(s, "Base = %llx\n\n", (unsigned long long) ha->fce_dma);
	seq_printf(s, "FCE Enable Registers\n");
	seq_printf(s, "%08x %08x %08x %08x %08x %08x\n",
	    ha->fce_mb[0], ha->fce_mb[2], ha->fce_mb[3], ha->fce_mb[4],
	    ha->fce_mb[5], ha->fce_mb[6]);

	fce = (uint32_t *) ha->fce;
	fce_start = (unsigned long long) ha->fce_dma;
	for (cnt = 0; cnt < fce_calc_size(ha->fce_bufs) / 4; cnt++) {
		if (cnt % 8 == 0)
			seq_printf(s, "\n%llx: ",
			    (unsigned long long)((cnt * 4) + fce_start));
		else
			seq_printf(s, " ");
		seq_printf(s, "%08x", *fce++);
	}

	seq_printf(s, "\nEnd\n");

	mutex_unlock(&ha->fce_mutex);

	return 0;
}

static int
qla2x00_dfs_fce_open(struct inode *inode, struct file *file)
{
	scsi_qla_host_t *vha = inode->i_private;
	struct qla_hw_data *ha = vha->hw;
	int rval;

	if (!ha->flags.fce_enabled)
		goto out;

	mutex_lock(&ha->fce_mutex);

	/* Pause tracing to flush FCE buffers. */
	rval = qla2x00_disable_fce_trace(vha, &ha->fce_wr, &ha->fce_rd);
	if (rval)
		ql_dbg(ql_dbg_user, vha, 0x705c,
		    "DebugFS: Unable to disable FCE (%d).\n", rval);

	ha->flags.fce_enabled = 0;

	mutex_unlock(&ha->fce_mutex);
out:
	return single_open(file, qla2x00_dfs_fce_show, vha);
}

static int
qla2x00_dfs_fce_release(struct inode *inode, struct file *file)
{
	scsi_qla_host_t *vha = inode->i_private;
	struct qla_hw_data *ha = vha->hw;
	int rval;

	if (ha->flags.fce_enabled)
		goto out;

	mutex_lock(&ha->fce_mutex);

	/* Re-enable FCE tracing. */
	ha->flags.fce_enabled = 1;
	memset(ha->fce, 0, fce_calc_size(ha->fce_bufs));
	rval = qla2x00_enable_fce_trace(vha, ha->fce_dma, ha->fce_bufs,
	    ha->fce_mb, &ha->fce_bufs);
	if (rval) {
		ql_dbg(ql_dbg_user, vha, 0x700d,
		    "DebugFS: Unable to reinitialize FCE (%d).\n", rval);
		ha->flags.fce_enabled = 0;
	}

	mutex_unlock(&ha->fce_mutex);
out:
	return single_release(inode, file);
}

static const struct file_operations dfs_fce_ops = {
	.open		= qla2x00_dfs_fce_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= qla2x00_dfs_fce_release,
};

#ifdef QLA_QRATE
/* q_rate related */
static int
qla_dfs_q_rate_show(struct seq_file *s, void *unused)
{
	struct scsi_qla_host *vha = s->private;
	seq_printf(s, "Queue Rate: ATIO/REQ/RSP = %6d/%6d/%6d.\n",
	    atomic_read(&vha->qrate.io_rate.value),
	    atomic_read(&vha->qrate.req_rate.value),
	    atomic_read(&vha->qrate.rsp_rate.value));
	return 0;
}
static int
qla_dfs_q_rate_open(struct inode *inode, struct file *file)
{
	struct scsi_qla_host *vha = inode->i_private;
	return single_open(file, qla_dfs_q_rate_show, vha);
}
static const struct file_operations dfs_q_rate_ops = {
	.open		= qla_dfs_q_rate_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

/* q_rate_hist related */
static int
qla_dfs_q_rate_hist_show(struct seq_file *s, void *unused)
{
	struct scsi_qla_host *vha = s->private;
	uint32_t i, num_ent;
	if (!vha->qrate.collect_hist) {
		seq_printf(s, "No queue rate history available.\n");
		return 0;
	}
	num_ent = vha->qrate.index;
	smp_mb();
	seq_printf(s, "Queue rate history for %u seconds (starts "
	    "when IO rate hits qla_q_rate_limit).\n", QLA_QR_NUM_HIST);
	for (i=0; i<num_ent; i++)
		seq_printf(s, "[%02d] ATIO/REQ/RSP = %6u/%6u/%6u. (%lu)\n",
		    i, vha->qrate.io_rate.hist[i], vha->qrate.req_rate.hist[i],
		    vha->qrate.req_rate.hist[i], vha->qrate.jiff_hist[i]);
	return 0;
}

static ssize_t
qla_dfs_q_rate_hist_write(struct file *file, const char __user *buf,
    size_t size, loff_t *ppos)
{
	struct scsi_qla_host *vha =
	    ((struct seq_file *)file->private_data)->private;
	memset(&vha->qrate.io_rate, 0, sizeof(struct qla_qrate_stat));
	memset(&vha->qrate.req_rate, 0, sizeof(struct qla_qrate_stat));
	memset(&vha->qrate.rsp_rate, 0, sizeof(struct qla_qrate_stat));
	vha->qrate.collect_hist = 0;
	return size;
}

static int
qla_dfs_q_rate_hist_open(struct inode *inode, struct file *file)
{
	struct scsi_qla_host *vha = inode->i_private;
	return single_open(file, qla_dfs_q_rate_hist_show, vha);
}
static const struct file_operations dfs_q_rate_hist_ops = {
	.open		= qla_dfs_q_rate_hist_open,
	.read		= seq_read,
	.write		= qla_dfs_q_rate_hist_write,
	.llseek		= seq_lseek,
	.release	= single_release,
};
#endif /* QLA_QRATE */

int
qla2x00_dfs_setup(scsi_qla_host_t *vha)
{
	struct qla_hw_data *ha = vha->hw;

	if (!IS_QLA25XX(ha) && !IS_QLA81XX(ha) && !IS_QLA83XX(ha) &&
	    !IS_QLA27XX(ha))
		goto out;
	if (!ha->fce)
		goto out;

	if (qla2x00_dfs_root)
		goto create_dir;

	atomic_set(&qla2x00_dfs_root_count, 0);
	qla2x00_dfs_root = debugfs_create_dir(QLA2XXX_DRIVER_NAME, NULL);
	if (!qla2x00_dfs_root) {
		ql_log(ql_log_warn, vha, 0x00f7,
		    "Unable to create debugfs root directory.\n");
		goto out;
	}

create_dir:
	if (ha->dfs_dir)
		goto create_nodes;

	mutex_init(&ha->fce_mutex);
	ha->dfs_dir = debugfs_create_dir(vha->host_str, qla2x00_dfs_root);
	if (!ha->dfs_dir) {
		ql_log(ql_log_warn, vha, 0x00f8,
		    "Unable to create debugfs ha directory.\n");
		goto out;
	}

	atomic_inc(&qla2x00_dfs_root_count);

create_nodes:
	ha->dfs_fce = debugfs_create_file("fce", S_IRUSR, ha->dfs_dir, vha,
	    &dfs_fce_ops);
	if (!ha->dfs_fce) {
		ql_log(ql_log_warn, vha, 0x00f9,
		    "Unable to create debugfs fce node.\n");
		goto out;
	}

#ifdef QLA_QRATE
	ha->dfs_q_rate = debugfs_create_file("q_rate", S_IRUSR,
	    ha->dfs_dir, vha, &dfs_q_rate_ops);
	if (!ha->dfs_q_rate) {
		ql_log(ql_log_warn, vha, 0x00fa,
		    "Unable to create debugfs q_rate node.\n");
		goto out;
	}
	ha->dfs_q_rate_hist = debugfs_create_file("q_rate_hist",
	    S_IRUSR|S_IWUSR, ha->dfs_dir, vha, &dfs_q_rate_hist_ops);
	if (!ha->dfs_q_rate_hist) {
		ql_log(ql_log_warn, vha, 0x00fb,
		    "Unable to create debugfs q_rate_hist node.\n");
		goto out;
	}
#endif /* QLA_QRATE */

out:
	return 0;
}

int
qla2x00_dfs_remove(scsi_qla_host_t *vha)
{
	struct qla_hw_data *ha = vha->hw;
	if (ha->dfs_fce) {
		debugfs_remove(ha->dfs_fce);
		ha->dfs_fce = NULL;
	}

	if (ha->dfs_dir) {
		debugfs_remove_recursive(ha->dfs_dir);
		ha->dfs_dir = NULL;
		atomic_dec(&qla2x00_dfs_root_count);
	}

	if (atomic_read(&qla2x00_dfs_root_count) == 0 &&
	    qla2x00_dfs_root) {
		debugfs_remove(qla2x00_dfs_root);
		qla2x00_dfs_root = NULL;
	}

	return 0;
}
