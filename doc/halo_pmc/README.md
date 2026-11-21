# 大作业一：HaloPMC（PostgreSQL CPU 性能计数器插桩）

学号 2601210473 / 王震

## 1. 实现内容与作业要求的对应

| 作业要求 | 实现 |
| --- | --- |
| 新增 `halo_pmc.c/h`，可随 PG 源码编译 | `halo_pmc.c` / `halo_pmc.h` 放入 `src/backend/utils/misc/` 和 `src/include/utils/` |
| 按 11.5 视频方式初始化 | 后端内首次调用 `PmcInit()`：SPI 查询 `pmc_name` 表，用 `hash_create/hash_search` 建立"计数器名 → PMU 配置"哈希 |
| 在某处打开计数器、运行代码并输出 | 在 `MultiExecPrivateHash()`（Hash Join 建哈希表）中 `PmcOpen(1)` → `PmcEnable()` → 循环 → `PmcDisable()` → `PmcReadData(0)` → `PmcPrint()` |
| 扩展：低开销循环读取 | `PmcOpen(1)` 对每个事件 mmap 事件页；`PmcSampleBegin/End` 用 `rdpmc` 读计数（不可用时退回 `read()`），逐行累加，`PmcPrintSample()` 输出 |

计数器列表通过 GUC `halo_pmc_list` 指定，例如：

```sql
SET halo_pmc_list = 'Instructions,cycles,PERF_COUNT_HW_CACHE_L1D_RD,MEM_INST_RETIRED_ALL_STORES';
```

计数器名与 `perf_event_attr` 字段的对应关系存放在 `pmc_name` 表中（`pmc_name.sql` 创建）。表结构与课件附带的《创建PMC表.txt》一致：
`pmc_name(pcm_name varchar(40), pmutype int, pe_config int, config1 int, config2 int)`，
`PmcInit` 使用 `SELECT lower(pcm_name), pmutype, ... FROM pmc_name`，因此直接导入课件文件也能工作。

## 2. 文件说明

| 文件 | 作用 |
| --- | --- |
| `halo_pmc.h` | `struct read_format`、`struct pmc_data`、`PMCHashItem` 及函数声明 |
| `halo_pmc.c` | 全部实现：SPI 初始化、哈希查找、`perf_event_open`、读计数、rdpmc 采样 |
| `pmc_name.sql` | 计数器目录表及常用计数器（含软件事件 `task_clock`） |
| `install.sh` | 一键把上述代码接入 PG 源码树并编译安装 |

## 3. 使用方法

```bash
# 1) 准备 PG 源码（openHalo 亦可）
git clone -b REL_16_STABLE https://github.com/postgres/postgres.git postgresql-16
cd postgresql-16 && ./configure --prefix=/opt/pg16 && make -j$(nproc) && make install

# 2) 接入并编译
./install.sh /path/to/postgresql-16

# 3) 允许非 root 后端使用性能事件（需要 root）
sudo sysctl -w kernel.perf_event_paranoid=0

# 4) 建库并导入计数器目录
psql -d testdb -f pmc_name.sql

# 5) 运行被测 SQL（关闭并行，确保走普通 Hash Join）
psql -d testdb
SET max_parallel_workers_per_gather = 0;
SET halo_pmc_list = 'Instructions,cycles';
SELECT count(*) FROM t1 JOIN t2 ON t1.id = t2.id;
```

## 4. 已验证结果（WSL2 Ubuntu，软件事件）

WSL2 不暴露硬件 PMU，因此用 `task_clock`（PERF_TYPE_SOFTWARE）验证整条链路
（GUC → SPI 哈希 → perf_event_open → 采样 → 打印）：

```
t1、t2 各 20 万行，hash join 结果 2000 万行：

INFO:                                task_clock : 98659000.00 (100.00%) 98659000
INFO:                                task_clock : 64722500.00 (loop sample)
```

硬件事件在同一环境下按预期报错（`ENOENT`，内核未提供 PMU）：

```
INFO:  Halo PMC: open instructions failed: No such file or directory
INFO:  Halo PMC: open cycles failed: No such file or directory
```

硬件计数器（Instructions/cycles/L1D/分支等）需要物理机，或开启 vPMC 的虚拟机
（VMware：虚拟机设置 → 处理器 → 勾选"虚拟化 CPU 性能计数器"），且建议
`perf_event_paranoid <= 1`。

> 实测环境说明：在 Intel 混合架构 CPU（i7-14700HX，13/14 代 Raptor Lake）
> 上使用 VMware Workstation 17.5 + `vpmc.enable = "TRUE"`，guest 内可以看到
> `cpu_core` / `cpu_atom` PMU，但全部硬件计数恒为 0（`perf stat` 同样为 0）。
> 这是 VMware vPMC 在混合架构 CPU 上的已知限制（Broadcom KB 311342：
> 虚拟化 HW PMC 在 Workstation/Fusion/ESXi 中不受支持），与 HaloPMC 代码无关。
> 因此本项目在 WSL/VMware 混合架构环境下只验证了软件事件 `task_clock`，
> 硬件计数需在物理机（或非混合架构 CPU 的虚拟机）上验证。

## 5. 实现要点与踩坑记录

- **GUC**：`halo_pmc_list` 为 `PGC_USERSET`、`CUSTOM_OPTIONS`，常量默认值为 `""`。
- **哈希键**：dynahash 以定长 40 字节做 blob 比较，`strlcpy` 不补齐尾部，
  因此写键前必须 `memset` 清零，否则插入与查找永远不相等。
- **读取格式**：单事件（非 `PERF_FORMAT_GROUP`）`read()` 返回
  `time_enabled, time_running, value` 三个 `u64`，**不含** `nr`；
  要得到这三项需在 `perf_event_attr.read_format` 中设置
  `PERF_FORMAT_TOTAL_TIME_ENABLED|PERF_FORMAT_TOTAL_TIME_RUNNING`，
  否则内核只返回 8 字节计数值。
- **elog 与 errno**：PG 的 `elog()` 宏会把 `errno` 屏蔽掉，使用 `strerror(errno)`
  前需先保存到局部变量。
- **并行 Hash Join**：PG16 默认可能选择 Parallel Hash Join，走
  `MultiExecParallelHash()`，不会进入插桩的 `MultiExecPrivateHash()`；
  测试时 `SET max_parallel_workers_per_gather = 0`。
- **INFO 级别**：`elog(INFO)` 总会发送给客户端，不受 `client_min_messages` 影响。
- **rdpmc 路径**：`cap_user_rdpmc` 为 0（如软件事件、部分虚拟机）时自动退回
  `read()`，功能不受影响，只是循环内多一次系统调用。
