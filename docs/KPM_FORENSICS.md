# KPM 取证观察器

本功能为授权 Android 设备上的记录型调试扩展，用于观察仍会被调度、但被
`/proc`、`ps` 或常规 task 链视图隐藏的任务，并审计 ARM64 硬件调试槽。
它不提供隐藏、持久化、绕过检测、任意寄存器写入或目标进程内存写入能力。

## 当前交付状态

- 原有默认行为不变：`--task-source=proc`、`--brk-backend=perf`。
- KPM 路径必须显式启用；stackplz 不会自动加载、安装或持久化 KPM。
- 已完成主机单元测试、ASan/UBSan、Go race、ARM64 交叉编译、ELF 导入白名单
  和反汇编寄存器指令检查。
- 已于 2026-08-31 在 OnePlus PLK110 真机完成可见/隐藏进程、syscall、uprobe、
  堆栈符号化、perf 断点、KPM-direct 断点和隐藏进程 maps 重建测试。详见
  [设备测试报告](../artifacts/kpm/device-test-report-20260831.md)和
  [设备验收清单](../tests/kpm/device_acceptance.md)。

现有 stackplz 功能可以继续按原方式使用；当前 KPM profile 已在报告所列的精确
设备和内核上验证。其他内核仍需单独适配和验收。

## 两条新增路径

| 选项 | 作用 | 不做什么 |
| --- | --- | --- |
| `--task-source=kpm` | 通过调度切换观察数值 PID，并取得 PID/TGID/UID/启动时间/comm 身份 | 不依赖 `/proc/<pid>` 或全局 task 链枚举 |
| `--brk-backend=kpm-direct` | 选择空闲 ARM64 BRP/WRP 槽，直接编程 DBGBVR/DBGBCR 或 DBGWVR/DBGWCR，并记录命中寄存器 | 不调用 perf_event 硬件断点注册路径，不写目标内存 |
| KPM maps 快照 | 对已绑定任务调用 profile 指定的 `show_map_vma`，分块导出带 CRC32 的不可变快照 | 不从目标的 `/proc/<pid>/maps` 读取，不猜测不同内核的 VMA ABI |

直接后端的事件包含 X0-X30、SP、PC、PSTATE、MDSCR、请求地址、观察地址和槽
状态。它不等价于旧 perf 后端的完整栈回溯；即使同时给出 `--stack`，也不能
把 KPM 寄存器记录描述成旧后端的 perf 栈记录。

## 支持范围与硬边界

当前闭合配置只支持：

- profile：`oneplus-plk110-a16-b4999618-d05`
- 设备：OnePlus PLK110 / OP60FFL1
- 内核：`6.12.23-android16-5-gb2a876903b49-ab14541642-4k`
- KPatch ABI：`kpver=d05`、`kver=60c17`
- 固定 KPatch SDK commit：
  `0fe6d142266b80e5aa445a7ea1534f88a8f33a35`

模块会核对精确 profile、内核发布串、页大小、CPU 数、ID_AA64DFR0_EL1 调试
能力、关键符号和已知内存布局。任何不匹配都应拒绝加载或保持不可变的 inert
状态；不允许猜测偏移后继续运行。
启动校验对候选内核地址使用 `copy_from_kernel_nofault`；错误偏移或无效指针会让
profile 失败关闭，而不是先用裸内存复制解引用该地址。

该观察器仍有明确边界：

- 目标必须继续经过调度器。如果任务已从调度运行路径移除、永远不再运行，观察
  器无法发现或命中它。
- 如果攻击者已经能任意修改内核、KPatch 本身或更早地劫持异常/调度路径，无法
  保证观察结果完整。
- 没有记录只表示“在当前观测窗口没有得到证据”，不能证明隐藏任务不存在。
- 断点 armed 期间禁止 CPU hotplug。CPU 拓扑改变会使配置失效，不应继续操作。
- 内核态访问触发 repeat watchpoint 时会降级为 one-shot，并设置
  `KERNEL_UACCESS` 事件标志，避免在内核单步路径中盲目重装。
- 发现槽所有者、值或控制寄存器与保存状态不一致时进入 `QUARANTINED`，只记录
  干扰并停止盲写，不覆盖第三方状态。

## 准备与加载

构建和适配步骤见 [KPM README](../kpm/README.md)。部署时必须由单一操作员串行
执行 KPM load/control/unload；固定的 KPatch d05 SDK 尚未把模块列表写操作改造
成一般并发安全接口。

先手工推送 stackplz 和 KPM 制品，再手工加载：

```sh
adb push bin/stackplz_arm64 /data/local/tmp/stackplz
adb push kpm/stackplz-kpm.kpm /data/local/tmp/stackplz-kpm.kpm
adb shell
su
chmod 0755 /data/local/tmp/stackplz

/data/adb/modules/KPatch-Next/bin/kpatch kpm load \
  /data/local/tmp/stackplz-kpm.kpm \
  "profile=oneplus-plk110-a16-b4999618-d05"
```

加载命令不是 stackplz 的一部分。加载失败时不要换成相近 profile，也不要跳过
校验。

## stackplz 使用

目标 PID 必须来自授权环境中的独立可信来源。KPM 模式要求一个数值
`--pid`，不支持用 `--name` 代替。

仅使用调度器身份解析、继续运行原有 syscall/uprobe 路径的示意命令：

```sh
/data/local/tmp/stackplz \
  --pid 31337 \
  --task-source kpm \
  --kpm-profile oneplus-plk110-a16-b4999618-d05 \
  --syscall openat
```

直接执行断点：

```sh
/data/local/tmp/stackplz \
  --pid 31337 \
  --task-source kpm \
  --brk-backend kpm-direct \
  --kpm-profile oneplus-plk110-a16-b4999618-d05 \
  --brk 0x7123456000:x \
  --brk-len 4 \
  --brk-mode once \
  --json \
  --regs
```

数据 watchpoint 的 kind 可为 `r`、`w` 或 `rw`，长度只能是 1、2、4、8，
且范围不能跨越一个 8 字节对齐块。执行断点必须 4 字节对齐且长度为 4。
`--json` 会保留事件中的数值 `Flags`，便于区分 kernel-uaccess 降级和干扰
记录。

若 `--brk` 是库内偏移并使用 `--brk-lib`，stackplz 会在准备阶段请求 KPM maps
快照并注入原有 maps 缓存，因此隐藏任务不再必须提供离线 maps：

```sh
/data/local/tmp/stackplz \
  --pid 31337 \
  --task-source kpm \
  --kpm-profile oneplus-plk110-a16-b4999618-d05 \
  --brk 0xef700:x \
  --brk-lib libc.so \
  --brk-backend perf \
  --regs
```

`--maps-file` 和 `--brk-base` 仍可作为明确的离线覆盖方式：

```sh
# 离线 maps 快照
--brk-lib libtarget.so --maps-file /data/local/tmp/target.maps

# 或显式库基址
--brk-lib libtarget.so --brk-base 0x7123400000
```

相关选项包括 `--kpm-control`、`--kpm-module` 和
`--kpm-bind-timeout`。默认控制程序为
`/data/adb/modules/KPatch-Next/bin/kpatch`，模块名为 `stackplz-kpm`，绑定
超时为 10 秒。

正常退出时，stackplz 会按 disable、clear 顺序清理自己建立的直接断点状态；
它不会替用户卸载 KPM。

## 手工控制与审计

控制命令格式为：

```sh
KPATCH=/data/adb/modules/KPatch-Next/bin/kpatch
$KPATCH kpm ctl0 stackplz-kpm "status"
$KPATCH kpm ctl0 stackplz-kpm "profile"
$KPATCH kpm ctl0 stackplz-kpm "bind pid=31337 mode=either"
$KPATCH kpm ctl0 stackplz-kpm "maps"
$KPATCH kpm ctl0 stackplz-kpm "maps-read snapshot=1 offset=0"
$KPATCH kpm ctl0 stackplz-kpm "break id=1 kind=x addr=0x7123456000 len=4 mode=once"
$KPATCH kpm ctl0 stackplz-kpm "enable id=1"
$KPATCH kpm ctl0 stackplz-kpm "poll after=0"
$KPATCH kpm ctl0 stackplz-kpm "audit"
$KPATCH kpm ctl0 stackplz-kpm "disable id=1"
$KPATCH kpm ctl0 stackplz-kpm "clear"
```

`bind` 还可增加 `uid=N`、`comm=NAME`、`start=N` 约束；这些字段用于防止
PID 复用被误认成原目标。`enable`、`disable`、`clear`、`audit` 是异步
请求，返回 `request=N` 后应轮询 `status`，直到同一个 request 的
`request_state=done` 且 `request_status=0`。

绑定后的原始 `status` 响应使用固定 16 字节的 `comm_hex=<32 个十六进制字符>`，
因此任务名包含空格时也不会破坏 `key=value` 分词。stackplz 的 Go 客户端会将它
解码回普通 `comm` 文本；旧模块仅在返回无空格的安全 `comm=` 字段时仍可兼容。

常见状态含义：

- `state=rejected` 或 load 失败：profile 校验未通过（PROFILE_REJECTED）。
- `binding=pending`：目标尚未在调度路径中被观察到；不是不存在证明。
- `binding=stale`：同一数值 PID 的启动身份发生变化。
- `binding=exited`：已绑定身份经过退出路径。
- `reason=busy`：已有异步请求、绑定或断点状态阻止本次变更。
- `reason=quarantined` 或带 `INTERFERENCE` 的完整性事件：现场状态与本模块
  的所有权记录不一致，模块拒绝覆盖。
- loss 事件：环形缓冲已丢记录；后续证据不完整。

`audit` 只读取硬件槽和内核 bookkeeping，报告可疑所有者/不一致；它不会注册
perf_event，也不会替换或关闭第三方槽。

## 清理和恢复

先停止 stackplz，再确认异步 clear 完成：

```sh
KPATCH=/data/adb/modules/KPatch-Next/bin/kpatch
$KPATCH kpm ctl0 stackplz-kpm "disable id=1"
$KPATCH kpm ctl0 stackplz-kpm "clear"
$KPATCH kpm ctl0 stackplz-kpm "status"
```

只在 `binding=none configured=0 enabled=0 maps_state=empty`、最后一个异步请求
`request_state=done request_status=0` 后结束操作。本次目标设备曾出现 KPatch d05
热卸载阻塞，因此当前发布不建议执行 `kpm unload stackplz-kpm`。替换或移除 KPM
时先 clear 并确认上述状态，然后重启设备，在重启后加载新制品。

若 clear 无法完成或状态无法证明安全，停止其他控制操作并重启设备恢复。重启是
恢复边界，不是“热卸载已经干净”的证据；保留 status、audit 和内核日志供复盘。
