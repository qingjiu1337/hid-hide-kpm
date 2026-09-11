/* SPDX-License-Identifier: GPL-2.0 */
/*
 * hid-hide.c - APatch KPM: 拦截键鼠注册，隐藏相关节点
 *
 * 在内核层对目标键盘/鼠标设备：
 *   - 阻止 hid_connect / evdev_connect / hidraw_connect / hiddev_connect
 *     （不创建 /dev/input/eventN、/dev/hidraw*、/dev/hiddev* 节点）
 *   - 丢弃 input_handle_event（不派发输入事件）
 *   - 从 /proc/bus/input/devices 中隐藏条目 (input_devices_seq_show)
 *
 * 通过 KPM_CTL0 控制接口动态管理目标与开关。
 */

#include <compiler.h>
#include <kpmodule.h>
#include <hook.h>
#include <log.h>
#include <kputils.h>
#include <ktypes.h>
#include <linux/kernel.h>
#include <linux/string.h>

/* ============================ 常量与配置 ============================ */

#define HH_MODULE_NAME     "hid-hide"
#define HH_MODULE_VERSION  "1.0"

#define HH_TARGET_MAX   32     /* 最大目标数 */
#define HH_TOKEN_LEN    64     /* 名称 token 最长（含 NUL） */
#define HH_NAME_BUF_LEN 128    /* 设备名解析缓冲 */

/*
 * 结构体偏移（arm64 / LP64）：
 *
 * struct input_dev { const char *name; ... };            name 在 offset 0（历代内核稳定）
 * struct evdev {
 *     int open;                    // 0x00 + 4 pad
 *     struct input_handle handle;  // 0x08
 * };
 * struct input_handle {
 *     void *private;               // handle+0x00
 *     int open;                    // handle+0x08
 *     const char *name;            // handle+0x10
 *     struct input_dev *dev;       // handle+0x18
 * };                                              => evdev->handle.dev = 0x20
 *
 * struct hid_device {
 *     __u8 *dev_rdesc;             // 0x00（历代稳定）
 *     unsigned int dev_rsize;      // 0x08
 *     ...
 *     __u32 vendor;                // 0x20（Linux 5.x-6.x）
 *     __u32 product;               // 0x24
 *     ...
 *     char name[128];              // 内嵌，偏移随版本变化 => 扫描解析
 * };
 */
#define HH_INPUT_DEV_NAME_OFF    0x00
#define HH_EVDEV_HANDLE_DEV_OFF  0x20
#define HH_HID_VENDOR_OFF        0x20
#define HH_HID_PRODUCT_OFF       0x24
#define HH_HID_SCAN_RANGE        0x100

#define HH_MIN_NAME_LEN 3      /* 解析出的名称最短长度，避免指针字节误判 */

/* ============================ 数据结构 ============================ */

struct hh_target {
    int used;
    uint32_t vid;              /* 非 0 表示 VID:PID 目标 */
    uint32_t pid;
    char token[HH_TOKEN_LEN];  /* 小写名称子串，或 "04d9:1234" 风格（仅记录用） */
};

/* ============================ 运行状态 ============================ */

static struct hh_target g_targets[HH_TARGET_MAX];
static int g_enabled = 1;      /* 全局开关 */
static int g_auto = 1;         /* 自动检测开关 */
static volatile int g_state_lock = 0;

/* 已查到的内核函数地址（0 表示 hook 未安装） */
static unsigned long g_fn_hid_connect;
static unsigned long g_fn_evdev_connect;
static unsigned long g_fn_hiddev_connect;
static unsigned long g_fn_hidraw_connect;
static unsigned long g_fn_input_handle_event;
static unsigned long g_fn_input_devices_seq_show;

/* 默认自动检测 token（小写匹配） */
static const char *g_auto_tokens[] = {
    "keyboard",
    "mouse",
    "consumer control",
    "system control",
};
#define HH_AUTO_TOKEN_NUM (int)(sizeof(g_auto_tokens) / sizeof(g_auto_tokens[0]))

/* ============================ 锁与内存读取 ============================ */

/* arm64 DAIF 关中断，供 atomic 上下文的短临界区使用 */
static inline uint64_t hh_irq_save(void)
{
    uint64_t flags;
    __asm__ volatile("mrs %0, daif" : "=r"(flags));
    __asm__ volatile("msr daifset, #3");
    return flags;
}

static inline void hh_irq_restore(uint64_t flags)
{
    __asm__ volatile("msr daif, %0" : : "r"(flags));
}

static uint64_t hh_lock(void)
{
    uint64_t flags = hh_irq_save();
    while (__atomic_test_and_set(&g_state_lock, __ATOMIC_ACQUIRE))
        ;
    return flags;
}

static void hh_unlock(uint64_t flags)
{
    __atomic_clear(&g_state_lock, __ATOMIC_RELEASE);
    hh_irq_restore(flags);
}

/* arm64 内核指针位于高半区 0xffff... */
static inline int hh_kernel_ptr(uint64_t p)
{
    return p != 0 && (p >> 48) == 0xffff;
}

static inline uint64_t hh_read_u64(uint64_t addr)
{
    return *(volatile uint64_t *)addr;
}

static inline uint32_t hh_read_u32(uint64_t addr)
{
    return *(volatile uint32_t *)addr;
}

static inline uint8_t hh_read_u8(uint64_t addr)
{
    return *(volatile uint8_t *)addr;
}

/* ============================ 字符串工具 ============================ */

static inline int hh_is_print(int c)
{
    return c >= 0x20 && c < 0x7f;
}

static inline char hh_tolower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

/* 带上限的小写化拷贝 */
static int hh_lower_copy(const char *src, char *dst, int dstlen)
{
    int i;
    for (i = 0; i + 1 < dstlen && src[i]; i++)
        dst[i] = hh_tolower(src[i]);
    dst[i] = '\0';
    return i;
}

/* 忽略大小写的子串匹配 */
static int hh_stristr(const char *hay_lower, const char *needle_lower)
{
    int n = (int)strlen(needle_lower);
    int h = (int)strlen(hay_lower);
    int i, j;
    if (n == 0 || h < n)
        return 0;
    for (i = 0; i + n <= h; i++) {
        for (j = 0; j < n; j++) {
            if (hay_lower[i + j] != needle_lower[j])
                break;
        }
        if (j == n)
            return 1;
    }
    return 0;
}

/* 忽略大小写的子串匹配：hay 为原始大小写，needle 为小写 */
static int hh_name_matches_token(const char *name, const char *token_lower)
{
    char name_lower[HH_NAME_BUF_LEN];

    hh_lower_copy(name, name_lower, sizeof(name_lower));
    return hh_stristr(name_lower, token_lower);
}

/* 从内核地址拷贝可打印字符串，遇 NUL / 非打印字符终止。
 * 返回长度；长度不足 HH_MIN_NAME_LEN 视为失败返回 0。 */
static int hh_copy_printable(const volatile char *s, char *out, int outlen)
{
    int len = 0;
    while (len + 1 < outlen) {
        int c = s[len];
        if (c == '\0')
            break;
        if (!hh_is_print(c))
            return 0;
        out[len++] = (char)c;
    }
    out[len] = '\0';
    if (len < HH_MIN_NAME_LEN)
        return 0;
    return len;
}

/* ============================ 设备名解析 ============================ */

/* struct input_dev *dev -> dev->name (offset 0, 指针) */
static int hh_copy_input_dev_name(uint64_t dev, char *out, int outlen)
{
    uint64_t namep;
    if (!hh_kernel_ptr(dev))
        return 0;
    namep = hh_read_u64(dev + HH_INPUT_DEV_NAME_OFF);
    if (!hh_kernel_ptr(namep))
        return 0;
    return hh_copy_printable((const volatile char *)namep, out, outlen);
}

/* struct evdev *evdev -> evdev->handle.dev (input_dev *) 的偏移说明见文件头部注释 */

/* struct hid_device *hid -> 内嵌 name[128]，偏移随版本变化。
 * 在前 0x100 字节内扫描第一段 >= HH_MIN_NAME_LEN 的 NUL 结尾可打印串。 */
static int hh_copy_hid_dev_name(uint64_t hid, char *out, int outlen)
{
    int off, run;
    if (!hh_kernel_ptr(hid))
        return 0;
    for (off = 0; off < HH_HID_SCAN_RANGE; off++) {
        run = 0;
        while (off + run < HH_HID_SCAN_RANGE) {
            int c = hh_read_u8(hid + off + run);
            if (c == '\0')
                break;
            if (!hh_is_print(c))
                break;
            run++;
        }
        if (run >= HH_MIN_NAME_LEN && off + run < HH_HID_SCAN_RANGE &&
            hh_read_u8(hid + off + run) == '\0') {
            if (run + 1 > outlen)
                run = outlen - 1;
            memcpy(out, (const void *)(hid + off), run);
            out[run] = '\0';
            return run;
        }
        off += run; /* 跳过本轮已检查的字节 */
    }
    return 0;
}

/* ============================ 目标管理 ============================ */

static int hh_add_target(const char *token_lower, uint32_t vid, uint32_t pid)
{
    uint64_t flags = hh_lock();
    int i, ret = 0;
    int found = -1;

    /* 查重（表内允许有空洞，全表扫描） */
    for (i = 0; i < HH_TARGET_MAX; i++) {
        if (!g_targets[i].used)
            continue;
        if (vid != 0) {
            if (g_targets[i].vid == vid && g_targets[i].pid == pid) {
                found = i;
                break;
            }
        } else {
            if (token_lower && strcmp(g_targets[i].token, token_lower) == 0) {
                found = i;
                break;
            }
        }
    }

    if (found >= 0) {
        ret = found; /* 已存在 */
        goto out;
    }

    for (i = 0; i < HH_TARGET_MAX; i++) {
        if (!g_targets[i].used) {
            memset(&g_targets[i], 0, sizeof(g_targets[i]));
            g_targets[i].used = 1;
            g_targets[i].vid = vid;
            g_targets[i].pid = pid;
            if (token_lower)
                hh_lower_copy(token_lower, g_targets[i].token, HH_TOKEN_LEN);
            ret = i;
            goto out;
        }
    }
    ret = -1; /* 表满 */
out:
    hh_unlock(flags);
    return ret;
}

static void hh_del_target(int idx)
{
    uint64_t flags = hh_lock();
    if (idx >= 0 && idx < HH_TARGET_MAX && g_targets[idx].used)
        memset(&g_targets[idx], 0, sizeof(g_targets[idx]));
    hh_unlock(flags);
}

static void hh_clear_targets(void)
{
    uint64_t flags = hh_lock();
    memset(g_targets, 0, sizeof(g_targets));
    hh_unlock(flags);
}

/* 名称匹配任一名称类目标（name 为内核原始大小写） */
static int hh_match_target_name(const char *name)
{
    uint64_t flags = hh_lock();
    int i, m = 0;
    for (i = 0; i < HH_TARGET_MAX; i++) {
        if (g_targets[i].used && g_targets[i].vid == 0 &&
            g_targets[i].token[0] && hh_name_matches_token(name, g_targets[i].token)) {
            m = 1;
            break;
        }
    }
    hh_unlock(flags);
    return m;
}

/* VID:PID 匹配（仅 hid_device 类钩子可调用） */
static int hh_match_target_vidpid(uint64_t hid)
{
    uint32_t vendor, product;
    uint64_t flags;
    int i, m = 0;

    if (!hh_kernel_ptr(hid))
        return 0;
    vendor = hh_read_u32(hid + HH_HID_VENDOR_OFF);
    product = hh_read_u32(hid + HH_HID_PRODUCT_OFF);
    if (vendor == 0 && product == 0)
        return 0;

    flags = hh_lock();
    for (i = 0; i < HH_TARGET_MAX; i++) {
        if (g_targets[i].used && g_targets[i].vid != 0 &&
            g_targets[i].vid == vendor && g_targets[i].pid == product) {
            m = 1;
            break;
        }
    }
    hh_unlock(flags);
    return m;
}

/* 自动检测：命中默认 token 且未在表中时自动加入（name 为内核原始大小写） */
static int hh_auto_add_by_name(const char *name)
{
    int t;
    if (!g_auto)
        return 0;
    for (t = 0; t < HH_AUTO_TOKEN_NUM; t++) {
        if (hh_name_matches_token(name, g_auto_tokens[t])) {
            int idx;
            if (hh_match_target_name(name))
                return 1; /* 已跟踪，避免重复添加 */
            idx = hh_add_target(name, 0, 0);
            if (idx >= 0)
                logki("sys: auto target added: %s\n", name);
            return idx >= 0;
        }
    }
    return 0;
}

/* ============================ Hook 回调 ============================ */

/* int hid_connect(struct hid_device *hid, unsigned int force) */
static void before_hid_connect(hook_fargs2_t *args, void *udata)
{
    char name[HH_NAME_BUF_LEN];
    uint64_t hid = args->arg0;

    (void)udata;
    if (!g_enabled)
        return;
    if (hh_copy_hid_dev_name(hid, name, sizeof(name))) {
        hh_auto_add_by_name(name);
        if (hh_match_target_name(name) || hh_match_target_vidpid(hid)) {
            logki("sys: block hid_connect: %s\n", name);
            args->skip_origin = 1;
            args->ret = 0;
        }
    } else if (hh_match_target_vidpid(hid)) {
        logki("sys: block hid_connect by vid:pid\n");
        args->skip_origin = 1;
        args->ret = 0;
    }
}

/* static int evdev_connect(struct input_handler *handler,
 *                         struct input_dev *dev,
 *                         const struct input_device_id *id) */
static void before_evdev_connect(hook_fargs3_t *args, void *udata)
{
    char name[HH_NAME_BUF_LEN];
    uint64_t dev = args->arg1;

    (void)udata;
    if (!g_enabled)
        return;
    if (!hh_copy_input_dev_name(dev, name, sizeof(name)))
        return;
    if (hh_match_target_name(name)) {
        logki("sys: block evdev_connect: %s\n", name);
        args->skip_origin = 1;
        args->ret = 0;
    }
}

/* int hidraw_connect(struct device *dev, struct hid_device *hid) */
static void before_hidraw_connect(hook_fargs2_t *args, void *udata)
{
    char name[HH_NAME_BUF_LEN];
    uint64_t hid = args->arg1;

    (void)udata;
    if (!g_enabled)
        return;
    if (hh_copy_hid_dev_name(hid, name, sizeof(name))) {
        hh_auto_add_by_name(name);
        if (hh_match_target_name(name) || hh_match_target_vidpid(hid)) {
            logki("sys: block hidraw_connect: %s\n", name);
            args->skip_origin = 1;
            args->ret = 0;
        }
    }
}

/* static int hiddev_connect(struct hid_device *hid, unsigned int force) */
static void before_hiddev_connect(hook_fargs2_t *args, void *udata)
{
    char name[HH_NAME_BUF_LEN];
    uint64_t hid = args->arg0;

    (void)udata;
    if (!g_enabled)
        return;
    if (hh_copy_hid_dev_name(hid, name, sizeof(name))) {
        if (hh_match_target_name(name) || hh_match_target_vidpid(hid)) {
            logki("sys: block hiddev_connect: %s\n", name);
            args->skip_origin = 1;
            args->ret = 0;
        }
    }
}

/* static void input_handle_event(struct input_dev *dev,
 *                                unsigned int type, unsigned int code, int value) */
static void before_input_handle_event(hook_fargs4_t *args, void *udata)
{
    char name[HH_NAME_BUF_LEN];
    uint64_t dev = args->arg0;

    (void)udata;
    if (!g_enabled)
        return;
    if (!hh_copy_input_dev_name(dev, name, sizeof(name)))
        return;
    if (hh_match_target_name(name))
        args->skip_origin = 1; /* 丢弃事件 */
}

/* static int input_devices_seq_show(struct seq_file *seq, void *v)  v = input_dev * */
static void before_input_devices_seq_show(hook_fargs2_t *args, void *udata)
{
    char name[HH_NAME_BUF_LEN];
    uint64_t dev = args->arg1;

    (void)udata;
    if (!g_enabled)
        return;
    if (!hh_copy_input_dev_name(dev, name, sizeof(name)))
        return;
    if (hh_match_target_name(name)) {
        logki("sys: hide from input devices: %s\n", name);
        args->skip_origin = 1;
        args->ret = 0;
    }
}

/* ============================ Hook 安装/卸载 ============================ */

static int hh_hook2(const char *sym, void *before, void *after,
                    unsigned long *fn_out)
{
    unsigned long addr = kallsyms_lookup_name(sym);
    hook_err_t err;

    if (!addr) {
        logkw("sys: symbol %s not found, skip\n", sym);
        return 0; /* 降级：不视为致命错误 */
    }
    err = hook_wrap2((void *)addr, (hook_chain2_callback)before, (hook_chain2_callback)after, NULL);
    if (err != HOOK_NO_ERR) {
        logke("sys: hook %s failed: %d\n", sym, (int)err);
        return 0;
    }
    *fn_out = addr;
    logki("sys: hook %s ok\n", sym);
    return 1;
}

static int hh_hook3(const char *sym, void *before, void *after,
                    unsigned long *fn_out)
{
    unsigned long addr = kallsyms_lookup_name(sym);
    hook_err_t err;

    if (!addr) {
        logkw("sys: symbol %s not found, skip\n", sym);
        return 0;
    }
    err = hook_wrap3((void *)addr, (hook_chain3_callback)before, (hook_chain3_callback)after, NULL);
    if (err != HOOK_NO_ERR) {
        logke("sys: hook %s failed: %d\n", sym, (int)err);
        return 0;
    }
    *fn_out = addr;
    logki("sys: hook %s ok\n", sym);
    return 1;
}

static int hh_hook4(const char *sym, void *before, void *after,
                    unsigned long *fn_out)
{
    unsigned long addr = kallsyms_lookup_name(sym);
    hook_err_t err;

    if (!addr) {
        logkw("sys: symbol %s not found, skip\n", sym);
        return 0;
    }
    err = hook_wrap4((void *)addr, (hook_chain4_callback)before, (hook_chain4_callback)after, NULL);
    if (err != HOOK_NO_ERR) {
        logke("sys: hook %s failed: %d\n", sym, (int)err);
        return 0;
    }
    *fn_out = addr;
    logki("sys: hook %s ok\n", sym);
    return 1;
}

/* ============================ 控制接口 ============================ */

static const char *hh_skip_space(const char *s)
{
    while (*s == ' ' || *s == '\t')
        s++;
    return s;
}

/* 取一个空格分隔的词，前进 *pp，返回剩余串 */
static const char *hh_get_word(const char **pp, char *out, int outlen)
{
    int i = 0;
    const char *p = hh_skip_space(*pp);
    while (*p && *p != ' ' && *p != '\t' && i + 1 < outlen)
        out[i++] = *p++;
    out[i] = '\0';
    *pp = p;
    return p;
}

static int hh_parse_hex32(const char *s, uint32_t *out)
{
    uint32_t v = 0;
    int n = 0;
    while (s[n]) {
        char c = s[n];
        int d;
        if (c >= '0' && c <= '9')
            d = c - '0';
        else if (c >= 'a' && c <= 'f')
            d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F')
            d = c - 'A' + 10;
        else
            break;
        v = v * 16 + (uint32_t)d;
        n++;
        if (n > 8)
            return 0;
    }
    if (n == 0)
        return 0;
    *out = v;
    return 1;
}

/* "vid=04d9:1234" -> vid/pid */
static int hh_parse_vidpid(const char *p, uint32_t *vid, uint32_t *pid)
{
    char buf[HH_TOKEN_LEN];
    char *colon;

    p = hh_get_word(&p, buf, sizeof(buf));
    if (strncmp(buf, "vid=", 4) != 0)
        return 0;
    colon = strchr(buf + 4, ':');
    if (!colon)
        return 0;
    *colon = '\0';
    if (!hh_parse_hex32(buf + 4, vid))
        return 0;
    if (!hh_parse_hex32(colon + 1, pid))
        return 0;
    return 1;
}

/* "name=xxx" -> token（保持原大小写交由 add 时统一小写） */
static int hh_parse_name(const char *p, char *token, int tokenlen)
{
    char buf[HH_TOKEN_LEN];
    p = hh_get_word(&p, buf, sizeof(buf));
    if (strncmp(buf, "name=", 5) != 0 || buf[5] == '\0')
        return 0;
    hh_lower_copy(buf + 5, token, tokenlen);
    return 1;
}

static void hh_build_help(char *out, int outlen)
{
    snprintf(out, outlen,
        "commands:\n"
        "  on | off            global switch (now: %s)\n"
        "  auto on | auto off  auto detect (now: %s)\n"
        "  add name=<token>    add name-substring target\n"
        "  add vid=XXXX:XXXX   add VID:PID target\n"
        "  del <index>         remove target\n"
        "  clear               remove all targets\n"
        "  list                show targets\n",
        g_enabled ? "on" : "off", g_auto ? "on" : "off");
}

static long hh_control_targets(const char *ctl_args, char *__user out_msg, int outlen)
{
    char cmd[HH_TOKEN_LEN];
    char reply[2048];
    const char *p = ctl_args;
    int n = 0;

    p = hh_get_word(&p, cmd, sizeof(cmd));

    if (strcmp(cmd, "on") == 0) {
        g_enabled = 1;
        n = snprintf(reply, sizeof(reply), "hid-hide: enabled\n");
    } else if (strcmp(cmd, "off") == 0) {
        g_enabled = 0;
        n = snprintf(reply, sizeof(reply), "hid-hide: disabled\n");
    } else if (strcmp(cmd, "auto") == 0) {
        char sub[HH_TOKEN_LEN];
        p = hh_get_word(&p, sub, sizeof(sub));
        if (strcmp(sub, "on") == 0) {
            g_auto = 1;
            n = snprintf(reply, sizeof(reply), "hid-hide: auto detect on\n");
        } else if (strcmp(sub, "off") == 0) {
            g_auto = 0;
            n = snprintf(reply, sizeof(reply), "hid-hide: auto detect off\n");
        } else {
            n = snprintf(reply, sizeof(reply), "hid-hide: auto is %s (usage: auto on|off)\n",
                         g_auto ? "on" : "off");
        }
    } else if (strcmp(cmd, "add") == 0) {
        char token[HH_TOKEN_LEN];
        uint32_t vid = 0, pid = 0;
        int idx;
        if (hh_parse_vidpid(p, &vid, &pid)) {
            idx = hh_add_target(NULL, vid, pid);
            if (idx >= 0)
                n = snprintf(reply, sizeof(reply), "hid-hide: add vid=%04x:%04x as #%d\n",
                             vid, pid, idx);
            else
                n = snprintf(reply, sizeof(reply), "hid-hide: target table full\n");
        } else if (hh_parse_name(p, token, sizeof(token))) {
            idx = hh_add_target(token, 0, 0);
            if (idx >= 0)
                n = snprintf(reply, sizeof(reply), "hid-hide: add name=%s as #%d\n", token, idx);
            else
                n = snprintf(reply, sizeof(reply), "hid-hide: target table full\n");
        } else {
            n = snprintf(reply, sizeof(reply), "hid-hide: usage: add name=<token> | add vid=XXXX:XXXX\n");
        }
    } else if (strcmp(cmd, "del") == 0) {
        char sub[HH_TOKEN_LEN];
        int idx;
        p = hh_get_word(&p, sub, sizeof(sub));
        idx = 0;
        {
            int i, digits = 0;
            for (i = 0; sub[i]; i++) {
                if (sub[i] < '0' || sub[i] > '9') {
                    digits = -1;
                    break;
                }
                idx = idx * 10 + (sub[i] - '0');
                digits++;
            }
            if (digits <= 0) {
                n = snprintf(reply, sizeof(reply), "hid-hide: usage: del <index>\n");
            } else {
                hh_del_target(idx);
                n = snprintf(reply, sizeof(reply), "hid-hide: del #%d\n", idx);
            }
        }
    } else if (strcmp(cmd, "clear") == 0) {
        hh_clear_targets();
        n = snprintf(reply, sizeof(reply), "hid-hide: all targets cleared\n");
    } else if (strcmp(cmd, "list") == 0) {
        uint64_t flags = hh_lock();
        int i, cnt = 0, len = 0;
        for (i = 0; i < HH_TARGET_MAX; i++) {
            if (g_targets[i].used)
                cnt++;
        }
        len = snprintf(reply, sizeof(reply),
                       "hid-hide: %s, auto %s, %d targets\n",
                       g_enabled ? "on" : "off", g_auto ? "on" : "off", cnt);
        for (i = 0; i < HH_TARGET_MAX && len > 0 && len < (int)sizeof(reply) - 1; i++) {
            int r;
            if (!g_targets[i].used)
                continue;
            if (g_targets[i].vid != 0)
                r = snprintf(reply + len, sizeof(reply) - len, "  #%-2d vid=%04x:%04x\n",
                             i, g_targets[i].vid, g_targets[i].pid);
            else
                r = snprintf(reply + len, sizeof(reply) - len, "  #%-2d name=%s\n",
                             i, g_targets[i].token);
            if (r <= 0)
                break;
            len += r;
        }
        n = len;
        hh_unlock(flags);
    } else {
        hh_build_help(reply, sizeof(reply));
        n = (int)strlen(reply);
    }

    if (n >= (int)sizeof(reply))
        n = (int)sizeof(reply) - 1;
    if (out_msg && outlen > 0)
        compat_copy_to_user(out_msg, reply, n < outlen ? n + 1 : outlen);
    return 0;
}

/* ============================ 生命周期 ============================ */

static long hh_init(const char *args, const char *event, void *reserved)
{
    int ok = 0;

    (void)reserved;
    if (!event)
        event = "";
    if (!args)
        args = "";
    logki("sys: %s init, event: %s, args: %s\n", HH_MODULE_NAME, event, args);

    /* 处理加载参数（与控制命令同一套语法） */
    if (args && args[0])
        hh_control_targets(args, NULL, 0);

    ok += hh_hook2("hid_connect", before_hid_connect, NULL, &g_fn_hid_connect);
    ok += hh_hook3("evdev_connect", before_evdev_connect, NULL, &g_fn_evdev_connect);
    ok += hh_hook2("hiddev_connect", before_hiddev_connect, NULL, &g_fn_hiddev_connect);
    ok += hh_hook2("hidraw_connect", before_hidraw_connect, NULL, &g_fn_hidraw_connect);
    ok += hh_hook4("input_handle_event", before_input_handle_event, NULL,
                   &g_fn_input_handle_event);
    ok += hh_hook2("input_devices_seq_show", before_input_devices_seq_show, NULL,
                   &g_fn_input_devices_seq_show);

    logki("sys: %s init done, %d/6 hooks installed\n", HH_MODULE_NAME, ok);
    return 0;
}

static long hh_exit(void *reserved)
{
    (void)reserved;

    /* 逐个卸载，未安装的（地址为 0）自动跳过 */
    if (g_fn_hid_connect) {
        hook_unwrap((void *)g_fn_hid_connect, before_hid_connect, NULL);
        g_fn_hid_connect = 0;
    }
    if (g_fn_evdev_connect) {
        hook_unwrap((void *)g_fn_evdev_connect, before_evdev_connect, NULL);
        g_fn_evdev_connect = 0;
    }
    if (g_fn_hiddev_connect) {
        hook_unwrap((void *)g_fn_hiddev_connect, before_hiddev_connect, NULL);
        g_fn_hiddev_connect = 0;
    }
    if (g_fn_hidraw_connect) {
        hook_unwrap((void *)g_fn_hidraw_connect, before_hidraw_connect, NULL);
        g_fn_hidraw_connect = 0;
    }
    if (g_fn_input_handle_event) {
        hook_unwrap((void *)g_fn_input_handle_event, before_input_handle_event, NULL);
        g_fn_input_handle_event = 0;
    }
    if (g_fn_input_devices_seq_show) {
        hook_unwrap((void *)g_fn_input_devices_seq_show, before_input_devices_seq_show, NULL);
        g_fn_input_devices_seq_show = 0;
    }

    logki("sys: %s exit\n", HH_MODULE_NAME);
    return 0;
}

/* ============================ 模块元数据 ============================ */

KPM_NAME(HH_MODULE_NAME);
KPM_VERSION(HH_MODULE_VERSION);
KPM_LICENSE("GPL v2");
KPM_AUTHOR("倾九");
KPM_DESCRIPTION("https://github.com/qingjiu1337/hid-hide-kpm");

KPM_INIT(hh_init);
KPM_CTL0(hh_control_targets);
KPM_EXIT(hh_exit);
