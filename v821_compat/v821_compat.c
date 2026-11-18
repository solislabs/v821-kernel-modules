#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/types.h>
#include <linux/scatterlist.h>
#include <linux/mempool.h>
#include <linux/slab.h>
#include <scsi/scsi.h>
#include <scsi/scsi_proto.h>
#include <scsi/scsi_common.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Antigravity");
MODULE_DESCRIPTION("Compatibility symbols for Allwinner V821 Audio/Storage");

/* Andes A27L2 DMA coherency symbol for ALSA PCM */
phys_addr_t pa_msb = 0;
EXPORT_SYMBOL(pa_msb);

/* SCSI command size table (from block/scsi_ioctl.c) */
const unsigned char scsi_command_size_tbl[8] = {
	6, 10, 10, 12,
	16, 12, 10, 10
};
EXPORT_SYMBOL(scsi_command_size_tbl);

/* Include scsi_common implementations */
#include "scsi_common_impl.c"

/* Include sg_pool implementations */
#include "sg_pool_impl.c"

static int __init v821_compat_init(void)
{
    int ret = sg_pool_init();
    if (ret) {
        pr_err("v821_compat: failed to init sg_pool: %d\n", ret);
        return ret;
    }
    pr_info("v821_compat: loaded successfully (pa_msb, scsi_common, sg_pool, scsi_cmd_size)\n");
    return 0;
}

static void __exit v821_compat_exit(void)
{
    sg_pool_exit();
    pr_info("v821_compat: unloaded\n");
}

module_init(v821_compat_init);
module_exit(v821_compat_exit);
