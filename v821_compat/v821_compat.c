#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Antigravity");
MODULE_DESCRIPTION("Compatibility symbols for Allwinner V821 Audio/Storage");

unsigned long pa_msb = 0;
EXPORT_SYMBOL(pa_msb);

static int __init v821_compat_init(void)
{
    pr_info("v821_compat: loaded successfully, pa_msb=0 exported\n");
    return 0;
}

static void __exit v821_compat_exit(void)
{
    pr_info("v821_compat: unloaded\n");
}

module_init(v821_compat_init);
module_exit(v821_compat_exit);
