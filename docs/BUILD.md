# 编译文档

可参考[workflow](.github/workflows/build.yml)或下面的步骤：

本项目依赖于[ehids/ebpfmanager](https://github.com/ehids/ebpfmanager)和[cilium/ebpf](https://github.com/cilium/ebpf)，但是做出了一些修改

所以目前编译需要使用修改过的版本，三个项目需要放在同一目录下

```bash
git clone https://github.com/SeeFlowerX/ebpf
git clone https://github.com/SeeFlowerX/ebpfmanager
git clone https://github.com/SeeFlowerX/stackplz
```

本项目在linux x86_64环境下编译，编译时先进入本项目根目录

准备必要的外部代码，记得挂全局代理或者使用`proxychains`等工具

```bash
./build_env.sh
```

然后下载ndk并解压，这里选的是`android-ndk-r25b`，解压后修改`build.sh`中的`NDK_ROOT`路径

本项目还需要使用golang，版本要求为`1.18`，建议通过snap安装，**或者**使用如下方法安装

```bash
wget "https://golang.org/dl/go1.18.7.linux-amd64.tar.gz"
tar -C /usr/local -xvf "go1.18.7.linux-amd64.tar.gz"
```

设置环境变量

```bash
nano ~/.bashrc
```

在末尾添加如下内容

```bash
export GOPATH=$HOME/go
export PATH=/usr/local/go/bin:$PATH:$GOPATH/bin
export GOPROXY=https://goproxy.cn,direct
export GO111MODULE=on
```

对单个项目来说，似乎要用下面的命令手动操作下，再重新用vscode打开才不会报错

```bash
go env -w GO111MODULE=on
go env -w GOPROXY=https://goproxy.cn,direct
```

使环境变量立即生效

```bash
source ~/.bashrc
```

执行`./build.sh`即可完成编译，产物在`bin`目录下

将可执行文件推送到手机上后就可以开始使用了

```bash
adb push bin/stackplz /data/local/tmp
```

## KPM取证扩展

KPM是独立的、默认关闭的可选制品；它不会改变普通stackplz构建和默认
`proc + perf`行为。先阅读[KPM操作说明](./KPM_FORENSICS.md)和
[KPM构建说明](../kpm/README.md)。

主机侧生成检查和C测试：

```bash
make kpm-generate-check
make kpm-host-test
```

使用固定KPatch-Next SDK和AArch64交叉工具链构建：

```bash
export KP_DIR=/absolute/path/to/KPatch-Next
make -C kpm clean
make -C kpm \
  TARGET_COMPILE=aarch64-linux-gnu- \
  KP_DIR="$KP_DIR" \
  all
```

对最终KPM再次执行独立制品门禁：

```bash
make -C kpm/scripts verify \
  ARTIFACT=../stackplz-kpm.kpm \
  KP_DIR="$KP_DIR" \
  TARGET_COMPILE=aarch64-linux-gnu-
```

该门禁会核对精确KPatch提交和安全卸载补丁、ELF架构/节、固定导入白名单及
KPatch导出证明，并从反汇编中确认直接调试寄存器访问。当前 profile 的物理设备
结果见[真机测试报告](../artifacts/kpm/device-test-report-20260831.md)和
[验收清单](../tests/kpm/device_acceptance.md)；其他 profile 仍必须单独验收。
