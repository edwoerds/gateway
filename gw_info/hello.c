/*
 * gw_info — 网关内核信息模块
 *
 * 在 /proc/gateway/info 暴露系统运行信息
 * 供网关用户态程序读取并推送给客户端
 *
 * 编译（在 Buildroot/QEMU 或目标板上）：
 *   make KDIR=/path/to/buildroot/output/build/linux-custom
 *
 * 加载：
 *   insmod gw_info.ko
 *   cat /proc/gateway/info
 *
 * 卸载：
 *   rmmod gw_info
 */

#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/sched/signal.h>
#include <linux/mm.h>
#include <linux/utsname.h>
#include <linux/version.h>

#define PROC_DIR  "gateway"
#define PROC_FILE "info"
#define PROC_PATH "gateway/info"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Cao Bin");
MODULE_DESCRIPTION("Gateway kernel info provider");
MODULE_VERSION("1.0");

/*

 * /proc/gateway/info 的 show 回调
 * 输出：
 *   kernel: 6.x.y
 *   processes: NNN
 *   free_pages: NNN
 *   uptime_secs: NNN
 */

static int gw_show(struct seq_file *m, void *v)
{
    struct sysinfo si;
    si_meminfo(&si);

    seq_printf(m, "kernel: %s\n", init_utsname()->release);
    seq_printf(m, "processes: %d\n", nr_threads);
    seq_printf(m, "free_pages: %lu\n", nr_free_pages());
    seq_printf(m, "total_pages: %lu\n", si.totalram);
    seq_printf(m, "uptime_secs: %lld\n", ktime_get_seconds());

    return 0;
}

static int gw_open(struct inode *inode, struct file *file)
{
    return single_open(file, gw_show, NULL);
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,6,0)
static const struct proc_ops gw_fops = {
    .proc_open    = gw_open,
    .proc_read    = seq_read,
    .proc_lseek   = seq_lseek,
    .proc_release = single_release,
};
#else
static const struct file_operations gw_fops = {
    .open    = gw_open,
    .read    = seq_read,
    .llseek  = seq_lseek,
    .release = single_release,
};
#endif

static int __init gw_init(void)
{
    struct proc_dir_entry *parent;

    /* 创建 /proc/gateway 目录 */
    parent = proc_mkdir(PROC_DIR, NULL);
    if (!parent) {
        pr_err("[gw_info] failed to create /proc/%s\n", PROC_DIR);
        return -ENOMEM;
    }

    /* 创建 /proc/gateway/info 文件 */
    if (!proc_create(PROC_FILE, 0444, parent, &gw_fops)) {
        pr_err("[gw_info] failed to create /proc/%s\n", PROC_PATH);
        remove_proc_entry(PROC_DIR, NULL);
        return -ENOMEM;
    }

    pr_info("[gw_info] loaded, /proc/%s created\n", PROC_PATH);
    return 0;
}

static void __exit gw_exit(void)
{
    remove_proc_subtree(PROC_DIR, NULL);
    pr_info("[gw_info] unloaded\n");
}

module_init(gw_init);
module_exit(gw_exit);
