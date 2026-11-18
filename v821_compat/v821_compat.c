#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/scatterlist.h>
#include <linux/blkdev.h>
#include <scsi/scsi.h>
#include <scsi/scsi_cmnd.h>
#include <scsi/scsi_eh.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Antigravity");
MODULE_DESCRIPTION("Compatibility symbols for Allwinner V821 Audio/Storage");

unsigned long pa_msb = 0;
EXPORT_SYMBOL(pa_msb);

const unsigned char scsi_command_size_tbl[8] = {
    6, 10, 10, 12, 16, 12, 10, 10
};
EXPORT_SYMBOL(scsi_command_size_tbl);

const char *scsi_device_type(unsigned type)
{
    if (type == 0) return "Direct-Access";
    if (type == 5) return "CD-ROM";
    return "Unknown";
}
EXPORT_SYMBOL(scsi_device_type);

u64 scsilun_to_int(struct scsi_lun *scsilun)
{
    int i;
    u64 lun = 0;
    for (i = 0; i < sizeof(lun); i += 2)
        lun = (lun << 16) | (scsilun->scsi_lun[i] << 8) | scsilun->scsi_lun[i + 1];
    return lun;
}
EXPORT_SYMBOL(scsilun_to_int);

bool scsi_normalize_sense(const u8 *sense_buffer, int sb_len, struct scsi_sense_hdr *sshdr)
{
    if (!sense_buffer || !sb_len)
        return false;
    memset(sshdr, 0, sizeof(struct scsi_sense_hdr));
    sshdr->response_code = (sense_buffer[0] & 0x7f);
    if (sshdr->response_code >= 0x70 && sshdr->response_code <= 0x73) {
        if (sb_len >= 3)
            sshdr->sense_key = (sense_buffer[2] & 0x0f);
        if (sb_len >= 13)
            sshdr->asc = sense_buffer[12];
        if (sb_len >= 14)
            sshdr->ascq = sense_buffer[13];
        return true;
    }
    return false;
}
EXPORT_SYMBOL(scsi_normalize_sense);

const u8 *scsi_sense_desc_find(const u8 *sense_buffer, int sb_len, int desc_type)
{
    return NULL;
}
EXPORT_SYMBOL(scsi_sense_desc_find);

void sg_free_table_chained(struct sg_table *table, unsigned int nents_first_chunk)
{
    __sg_free_table(table, SG_MAX_SINGLE_ALLOC, false, sg_kfree);
}
EXPORT_SYMBOL(sg_free_table_chained);

int sg_alloc_table_chained(struct sg_table *table, int nents, struct scatterlist *first_chunk, unsigned int nents_first_chunk)
{
    return __sg_alloc_table(table, nents, SG_MAX_SINGLE_ALLOC, first_chunk, nents_first_chunk, GFP_ATOMIC, sg_kmalloc);
}
EXPORT_SYMBOL(sg_alloc_table_chained);

void scsi_req_init(struct scsi_request *req)
{
    memset(req, 0, sizeof(*req));
}
EXPORT_SYMBOL(scsi_req_init);

int sg_scsi_ioctl(struct request_queue *q, struct gendisk *disk, fmode_t mode, struct scsi_ioctl_command __user *sic)
{
    return -ENOTTY;
}
EXPORT_SYMBOL(sg_scsi_ioctl);

static int __init v821_compat_init(void)
{
    pr_info("v821_compat: loaded successfully, audio & storage symbols exported\n");
    return 0;
}

static void __exit v821_compat_exit(void)
{
    pr_info("v821_compat: unloaded\n");
}

module_init(v821_compat_init);
module_exit(v821_compat_exit);
