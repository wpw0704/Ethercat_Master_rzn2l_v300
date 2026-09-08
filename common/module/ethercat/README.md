# SOEM 应用层接入

当前为 RZ/N2L 单物理网口、单 SOEM 任务独占方式。收发路径为：

`应用层 ec_* / ecx_* → soem/oshw/nicdrv.c → ethernet/eth.c → R_GMAC_Read / R_GMAC_Write`

## 配置及启动顺序

1. 在 `../ethernet/eth.h` 的 `ETH_MONITOR_PORT` 选择 ETH0/1/2。
   `ETH_SOEM_PORT` 和 `ETH_SOEM_IFNAME` 自动跟随该选择，目前为 ETH0 / `"eth0"`。
2. 由现有初始化任务调用 `eth_init()`，再运行 `eth_monitor_task_entry()`。
   监控任务连续检测到 READY 超过 500 ms 后打印一次并挂起；它不会创建主站任务。
3. 后续应用层自行实现启动通知或调度，在确认监控任务已停止访问驱动后，由唯一主站任务调用
   `ec_init(ETH_SOEM_IFNAME)`。成功返回 1；此函数只绑定已初始化网口，成功不代表发现从站。
4. 在主站任务继续调用 `ec_config_init(FALSE)` 扫描从站，根据从站数量、PDO 和内存需求分配静态
   IOmap，然后使用 `ec_config_map()`、需要时的 `ec_configdc()`，以及状态切换和过程数据接口。
   应用层负责检查返回值、WKC、状态及异常恢复；本次未实现这些业务流程。
5. 退出时 `ec_close()` 只解绑 SOEM。确认所有收发已停止后，应用按需要调用 `eth_deinit()`。

推荐应用包含 `ethercat.h` 和 `eth.h`，使用标准 SOEM 接口即可，不需要自己添加接收任务、
回调队列或直接操作 GMAC 缓冲区。`app/ethercat_master.c/.h` 保留为应用实现位置。

## 保留与移除的内容

- 移除了旧 `um_ether_netif` 依赖、接收回调、8 帧软件队列、接收信号量和动态互斥锁。
- 保留 SOEM 必需的 16 组帧缓冲区、索引匹配、乱序缓存、WKC、超时及有限期限内重试。
- 修正 `ec_setupheader()` 为初始化一帧的 Ethernet 头；`ecx_waitinframe()` 返回实际 WKC，
  未收到响应返回 `EC_NOFRAME`，WKC 为 0 仍是有效响应。
- 不支持冗余网口；仅保留核心编译所需的类型和入口。`ec_init_redundant()` 返回失败。
- 未修改 SOEM `src` 协议核心、FSP 驱动、自动生成的实例配置、启动代码或链接脚本。

## 使用约束

- 所有 SOEM 调用、网口收发、链路维护必须由同一个任务串行执行。
  去掉锁的前提是单任务独占；ISR 不参与 SOEM 队列或缓冲区访问。
- 监控任务挂起后，应用需要接管周期性 `eth_link_process()` 和状态检查。
  请安排在过程数据事务之外；链路变化时 FSP 可能复位 MAC，不能保证该函数固定耗时。
  断链后应停止正常周期交换，处理恢复并重新检查从站状态。
- `ec_init()` 绑定现有实例，不重复打开硬件，也不等待 READY。自建 `ecx_portt` 首次使用前必须清零。
- SOEM 环形索引最多容纳 `EC_MAXBUF` 个在途帧。直接调用底层接口时要及时设回 `EC_BUF_EMPTY`；
  常规 `ec_*` API 自行管理索引，不要在另一个任务同时发送 mailbox 或 PDO。
- `eth_send_frame()` / `eth_receive_frame()` 使用普通以太网帧，不带 FCS 或交换机标签。
  前者成功仅表示提交驱动。后者无有效帧返回 `FSP_ERR_ETHER_ERROR_NO_DATA`，其他错误保留 FSP 返回码。
- 当前 FSP 配置启用了 8 字节交换机管理标签。`eth.c` 负责发送插入标签、指定端口以及接收去标签和
  入端口过滤，应用不处理标签。普通帧上限 1514 字节，短帧补齐到 60 字节；FCS 由硬件处理。
  标签字段依据 Renesas `um_ether_netif_ether.c` 的 `build_tx_management_tag()` /
  `fetch_rx_management_tag()`；驱动缓冲区要求见
  [Renesas GMAC 文档](https://renesas.github.io/rz-fsp/group___r_z_n___e_t_h_e_r.html)。
- 暂存数组为静态 CPU 缓冲区，FSP 使用非零拷贝模式；实际 DMA 描述符和缓冲区继续使用生成配置中的
  对齐和 `.noncache_buffer.eth` 非缓存区域。不要仅修改 `zerocopy` 配置，初始化会拒绝不匹配的模式。
- 等待响应为有期限的忙轮询，会占用主站任务运行时间；应用周期之间须自行让出 CPU。
  现有 OSAL 使用 1000 Hz FreeRTOS tick，不能保证微秒精度。
  非零超时向上取整并额外保留一个 tick，最大量化延长小于 2 个 tick，调度延迟还会进一步延长。
  `osal_current_time()` 沿用启动时间加 EtherCAT 纪元偏移，并非 RTC；32 位 tick 回绕后不保持长期单调。
  精确周期和 DC 同步需要后续接入并验证高精度时基。OSAL 线程创建接口仍返回不支持，由应用创建任务。

## 验证

代码检查和主机替身测试可验证接口逻辑，不能代替目标板测试。

- `cmake --build build/cmake-debug --parallel 4`：目标工程构建。
- `python build/soem-driver-tests/run_tests.py`：编译实际 eth/OSAL/nicdrv 和全部 SOEM 核心，
  验证初始化配置、管理标签、端口过滤、帧容量、真实 `ec_BRD()` 的 WKC、乱序、多 datagram、
  错误帧、发送失败、总超时和 tick 回绕，并严格检查修改文件的 UTF-8 编码。
- `python build/soem-driver-tests/check_target_link.py`：另行强制链接 9 个应用常用 SOEM 入口，
  避免尚未调用 SOEM 的主程序将它们裁剪后掩盖链接错误；产物位于测试目录，不覆盖正式固件。

上板后，按以下顺序确认：

1. 接入所选网口，观察连续 READY 超过 500 ms 的单次日志及监控任务挂起。
2. 单从站扫描应得到实际从站数量；抓包确认物理线上为 EtherType `0x88A4`，
   不携带内部 `0xE001` 管理标签，发送仅出现在所选端口。
3. 根据实际 PDO 配置计算期望 WKC，进入 OP 后持续对比实际值；记录周期耗时、抖动、最大收发时延、
   超时次数及丢帧率。短帧和接近最大帧长度都要覆盖。
4. 插拔所选端口及其他端口，确认错误检测、指定端口过滤以及应用的恢复流程。
