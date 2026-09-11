<div align="center">

# hid-hide-kpm

**拦截键鼠注册，隐藏相关节点 —— APatch 内核补丁模块（KPM）**

在内核层让目标键盘 / 鼠标设备从系统中"消失"：
无设备节点、无 `/proc` 条目、无输入事件派发。

![Platform](https://img.shields.io/badge/platform-APatch%20%2F%20KernelPatch-blue)
![Arch](https://img.shields.io/badge/arch-ARM64%20aarch64-orange)
![Kernel](https://img.shields.io/badge/kernel-5.x%20--%206.x-9cf)
![License](https://img.shields.io/badge/license-GPL%20v2-green)

</div>

---

## 目录

- [这是什么](#这是什么)
- [工作原理](#工作原理)
- [设备名解析策略（免偏移表）](#设备名解析策略免偏移表)
- [适用环境](#适用环境)
- [构建](#构建)
- [安装与加载](#安装与加载)
- [控制命令](#控制命令)
- [效果验证](#效果验证)
- [注意事项与已知限制](#注意事项与已知限制)
- [常见问题](#常见问题)
- [致谢](#致谢)
- [免责声明](#免责声明)

---

## 这是什么

`hid-hide-kpm` 是一个 [APatch](https://github.com/bmax121/APatch) 平台的 **KPM（Kernel Patch Module）** 内核模块。

它通过 KernelPatch 提供的内核函数 inline hook 机制，在**内核态**拦截键盘 / 鼠标（HID）设备的注册流程并隐藏相关痕迹。被隐藏的设备：

- ❌ 不会创建 `/dev/input/eventN` 设备节点
- ❌ 不会创建 `/dev/hidraw*`、`/dev/hiddev*` 节点
- ❌ 不会派发任何输入事件（`getevent` / 应用层均收不到）
- ❌ 不会出现在 `/proc/bus/input/devices` 列表中
- ✅ 仅物理设备本身还能正常输入给系统内核缓冲之外的部分（取决于拦截层级）

对比常见的用户态方案（修改 `/dev` 权限、隐藏 shell 输出等），内核态拦截对所有用户态进程**统一生效且不留痕迹**——任何 App、任何 API 查询到的都是"设备不存在"。

## 工作原理

模块加载后，通过 `kallsyms_lookup_name()` 定位 6 个内核函数地址，使用 KernelPatch 的 `hook_wrap` 链式 inline hook 安装 before 回调。回调中解析设备名，命中目标规则时设置 `skip_origin = 1` 直接跳过原函数并伪造返回值 `0`，从而静默拦截注册 / 事件。

| # | Hook 的内核函数 | 参数布局 | 拦截动作 |
|---|---|---|---|
| 1 | `hid_connect(hid, force)` | arg0 = `hid_device *` | 阻止 HID 设备建立连接 |
| 2 | `evdev_connect(handler, dev, id)` | arg1 = `input_dev *` | 阻止创建 `/dev/input/eventN` |
| 3 | `hidraw_connect(dev, hid)` | arg1 = `hid_device *` | 阻止创建 `/dev/hidraw*` |
| 4 | `hiddev_connect(hid, force)` | arg0 = `hid_device *` | 阻止创建 `/dev/hiddev*` |
| 5 | `input_handle_event(dev, type, code, value)` | arg0 = `input_dev *` | 丢弃目标设备的全部输入事件 |
| 6 | `input_devices_seq_show(seq, v)` | arg1 = `input_dev *` | 从 `/proc/bus/input/devices` 隐藏条目 |

卸载模块时逐一 `hook_unwrap` 还原全部钩子，不留残留。

### 模块结构

```
hid_hide.c
├── 元数据区      KPM_NAME / KPM_VERSION / ... / KPM_INIT / KPM_CTL0 / KPM_EXIT
├── 锁与内存读取  __atomic 自旋锁 + arm64 DAIF 关中断（回调运行于 atomic 上下文）
├── 设备名解析    input_dev 名称直读；hid_device 内嵌名称扫描
├── 目标管理      静态目标表（32 项），支持名称子串 / VID:PID
├── Hook 回调     6 个 before 回调
├── 控制接口      KPM_CTL0 字符串命令解析
└── 生命周期      init 安装钩子（符号缺失自动降级）/ exit 严格卸载
```

## 设备名解析策略（免偏移表）

不同内核版本结构体字段偏移不同，本项目采用**运行时自适应解析**，不维护版本偏移表：

- `struct input_dev` 的 `name` 是结构体首字段（`const char *` 指针，历代内核稳定）→ 直接读 offset 0 解引用
- `struct hid_device` 的 `name[128]` 是内嵌数组、偏移随版本漂移 → **扫描结构体前 0x100 字节**，取第一段 `≥3` 字符、NUL 结尾的连续可打印 ASCII 串
- 所有内核指针先做高半区校验（arm64 内核地址 `0xffff...`），解析失败一律放行并告警，**宁漏勿误杀**——绝不会因为误判隐藏正常设备

## 适用环境

| 项目 | 要求 |
|---|---|
| 平台 | 已安装 APatch（KernelPatch ≥ 0.10，模块 API 对应 main 分支）的 Android 设备 |
| 架构 | ARM64 |
| 内核 | Linux 5.x – 6.x（Android GKI / common kernel） |
| 内核配置 | `CONFIG_KALLSYMS=y`（KernelPatch 基本要求） |

## 构建

模块编译产物为裸机 ARM64 可重定位 ELF（`.kpm`）。

### 方式一：kpm-spore 脚手架（Windows 推荐）

```bash
git clone https://github.com/jiqiu2022/kpm-spore
cd kpm-spore
# 把本仓库的 hid_hide.c 放入 modules/hid-hide/，参照 modules/hello 补充 CMakeLists.txt
./build.bat        # 自动拉取 KernelPatch 源码与 NDK
# 产物：build/hid-hide/hid-hide.kpm
```

### 方式二：官方 Makefile（WSL / Linux）

需要 aarch64 裸机交叉工具链（`aarch64-none-elf-`）：

```bash
git clone https://github.com/bmax121/KernelPatch
export TARGET_COMPILE=aarch64-none-elf-
export KP_DIR=/path/to/KernelPatch
cd hid-hide-kpm
make
# 产物：hid_hide.kpm
```

## 安装与加载

**方式一：APatch 管理器** —— 在「内核模块」页面加载 `hid_hide.kpm`，可在加载参数框直接填控制命令（如 `auto on`）。

**方式二：kpmctl / supercall**：

```bash
adb push hid_hide.kpm /data/local/tmp/
adb shell
su
kpmctl load /data/local/tmp/hid_hide.kpm "auto on"
kpmctl list
```

模块 init 参数与控制命令语法完全一致，加载时可一次性下发多条命令（空格分隔）。

## 控制命令

通过 `sc_kpm_control()` / APatch 管理器 / kpmctl 下发：

| 命令 | 说明 |
|---|---|
| `on` / `off` | 全局开关（默认 `on`） |
| `auto on` / `auto off` | 自动检测开关（默认 `on`） |
| `add name=<子串>` | 按设备名子串添加目标（大小写不敏感） |
| `add vid=XXXX:XXXX` | 按 USB VID:PID 添加目标 |
| `del <序号>` | 删除指定序号的目标 |
| `clear` | 清空全部目标 |
| `list` | 查看当前状态与目标列表 |
| 其他 | 输出帮助 |

### 使用示例

```bash
# 自动模式：新接入的键盘/鼠标自动隐藏
auto on

# 手动模式：关闭自动，精确指定
auto off
add vid=04d9:1234          # 按 VID:PID 隐藏
add name=logitech          # 名称含 "logitech" 的设备全部隐藏
add name=keyboard          # 所有键盘
list
del 2                      # 删除 #2 号目标
```

### 自动检测

`auto` 开启时（默认），新 HID 设备的名称命中以下 token 之一即自动加入隐藏目标：

```
keyboard / mouse / consumer control / system control
```

这四个 token 覆盖了绝大多数 Android 键鼠设备（HID 标准集合名称）。需要排除个别设备时，用 `auto off` + 手动 `add`。

## 效果验证

```bash
# 1. 隐藏前记录
cat /proc/bus/input/devices
ls -l /dev/input/

# 2. 加载模块并添加目标后，重新插拔目标设备
add name=<你的设备名关键字>

# 3. 隐藏后确认
cat /proc/bus/input/devices        # 目标条目消失
ls /dev/input/                     # 无新增 eventN
getevent                           # 目标设备无任何输出

# 4. 内核日志
dmesg | grep 'KP I sys'
```

## 注意事项与已知限制

1. **只拦截注册阶段**：模块加载前已注册的设备不会消失，需要重新插拔（USB）或重启（蓝牙/内置）后才被隐藏。
2. **保留备用输入设备**：隐藏了唯一的键鼠后无法在设备上操作，请确保有触摸屏或另一输入设备兜底。
3. **紧急恢复**：`off` 命令立即停止全部拦截；卸载模块恢复一切。
4. **VID:PID 匹配**依赖 `hid_device` 中 `vendor/product` 字段的经典布局（Linux 5.x–6.x），若极个别内核布局不同，VID:PID 规则可能失配，名称规则不受影响。
5. **符号依赖**：若内核把某个 hook 目标静态内联（kallsyms 查不到），该路 hook 自动跳过并告警，其余功能不受影响。
6. 内核钩子属高风险操作，**仅在自己的设备上使用**，使用前了解卸载与恢复途径。

## 常见问题

<details>
<summary>加载时报符号 / 版本错误？</summary>

KPM 与 KernelPatch 核心版本需匹配。请使用与设备 APatch 版本相同的 KernelPatch 源码构建。
</details>

<details>
<summary>设备没有消失？</summary>

依次检查：① `list` 确认目标已添加、开关为 `on`；② 目标设备是否重新插拔过；③ `dmesg | grep 'KP'` 查看 hook 是否全部安装成功；④ 某些复合设备名不含预期关键字，用 `cat /proc/bus/input/devices` 找到真实名称后按子串添加。
</details>

<details>
<summary>和原版「键鼠隐藏.kpm」什么关系？</summary>

本项目是对一款闭源 OLLVM 混淆 KPM 的开源重写，功能与控制接口对齐，实现完全原创、可自行审计和修改。
</details>

## 致谢

- [KernelPatch](https://github.com/bmax121/KernelPatch) / [APatch](https://github.com/bmax121/APatch) —— bmax121
- [kpm-spore](https://github.com/jiqiu2022/kpm-spore) —— KPM 构建脚手架

## 免责声明

本项目仅供学习研究内核机制与个人设备自定义用途，请遵守当地法律法规。使用者自行承担一切风险，作者不对任何直接或间接损失负责。

---

**License**: [GPL-2.0](LICENSE)
