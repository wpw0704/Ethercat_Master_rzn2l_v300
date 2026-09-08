#ifndef ETH_H
#define ETH_H
/***********************************************************************************************************************
 * Includes
 **********************************************************************************************************************/
#include "r_gmac.h"
#include "r_ether_phy.h"
#include "main_thread.h"
#include "um_common.h"
#include <string.h>
#include "ethercat_master.h"
FSP_HEADER

/**********************************************************************************************************************
 * Macro definitions
 **********************************************************************************************************************/
/* 监控的物理网口：0=ETH0，1=ETH1，2=ETH2；时间单位为毫秒。 */
#define ETH_MONITOR_PORT          (1U)
#define ETH_MONITOR_STABLE_MS     (500U)
#define ETH_MONITOR_POLL_MS       (10U)

#define ETH_MONITOR_TASK_NAME "eth_monitor_task"
#define ETH_MONITOR_TASK_STACK_SIZE      (1024)
#define ETH_MONITOR_TASK_PRIORITY        (3)

/* SOEM 独占的物理网口与监控网口一致，应用层使用 ETH_SOEM_IFNAME 打开。 */
#define ETH_SOEM_PORT             ETH_MONITOR_PORT
#if ETH_SOEM_PORT == 0
#define ETH_SOEM_IFNAME           "eth0"
#elif ETH_SOEM_PORT == 1
#define ETH_SOEM_IFNAME           "eth1"
#else
#define ETH_SOEM_IFNAME           "eth2"
#endif
/* 不含 FCS 的普通以太网帧；GMAC 缓冲区还需容纳交换机的 8 字节管理标签。 */
#define ETH_FRAME_MAX_SIZE        (1514U)
#define ETH_FRAME_MIN_SIZE        (60U)
#define ETH_MANAGEMENT_TAG_SIZE   (8U)
#define ETH_GMAC_BUFFER_SIZE      (1524U)

#if (ETH_MONITOR_PORT < 0) || (ETH_MONITOR_PORT >= BSP_FEATURE_GMAC_MAX_PORTS)
#error "ETH_MONITOR_PORT must select an existing physical port."
#endif

/**********************************************************************************************************************
 * Typedef definitions
 **********************************************************************************************************************/

/**********************************************************************************************************************
 * Exported global variables
 **********************************************************************************************************************/

/***********************************************************************************************************************
 * Function prototypes
 **********************************************************************************************************************/
/**
 * @brief 打开 g_ether0 及其配置的交换机、PHY、RGMII selector。
 * @param 无。
 * @return FSP_SUCCESS 表示驱动已打开，不代表网线已连接；其他值为 FSP 错误码。
 * @note 在调度器运行、BSP 引脚初始化完成后，由唯一的网口管理任务调用。
 *       包含现有 PHY/驱动复位等待，不得在 ISR、临界区或 EtherCAT 周期内调用。
 *       当前沿用三个 PHY 的自动协商和交换机专用标签配置。
 */
fsp_err_t eth_init(void);

/**
 * @brief 关闭网口，停止驱动收发并关闭其 PHY 和交换机。
 * @param 无。
 * @return FSP_SUCCESS：关闭成功；FSP_ERR_NOT_OPEN：未打开；其他值为驱动错误。
 * @note 由同一管理任务调用，调用前须停止所有收发操作。
 */
fsp_err_t eth_deinit(void);

/**
 * @brief 执行一次链路维护，使驱动处理协商结果、插拔及 DMA 收发启停。
 * @param 无。
 * @return FSP_SUCCESS 仅表示本次维护完成，链路状态须另行查询；其他值为驱动错误。
 * @note 由同一管理任务周期调用，不等待网线连接；不得与收发操作并发调用。
 *       FSP 在链路变化时可能复位 MAC，因此不可视为具有确定执行时间的周期函数。
 */
fsp_err_t eth_link_process(void);

/**
 * @brief 查询上次链路维护得到的指定端口状态。
 * @param[in] port 物理端口编号 0、1、2，不是 GMAC 实例编号。
 * @param[out] p_status DOWN：断开；UP：检测到连接；READY：驱动完成链路建立。
 * @return FSP_SUCCESS：查询成功；无效参数、未打开或驱动错误返回对应 FSP 错误码。
 * @note 与其他网口接口在同一任务串行调用；READY 不代表已验证 EtherCAT 通信。
 */
fsp_err_t eth_get_link_status(uint8_t port, gmac_link_status_t *p_status);

/**
 * @brief 从 ETH_SOEM_PORT 发送一帧，补齐短帧并插入交换机管理标签。
 * @param[in] p_frame 普通以太网帧，包含 MAC 地址和 EtherType，不含 FCS/管理标签。
 * @param[in] length 帧长度，范围 14..ETH_FRAME_MAX_SIZE 字节。
 * @return FSP_SUCCESS 表示已交给驱动发送；其他值为参数、链路或驱动错误。
 * @note eth_init 成功后使用；与接收、链路维护在唯一 SOEM 任务串行调用。
 *       当前只支持 FSP 非零拷贝及专用标签模式；返回后调用者可以复用发送缓冲区。
 */
fsp_err_t eth_send_frame(const uint8_t *p_frame, uint32_t length);

/**
 * @brief 非阻塞读取一帧，检查入端口并去除交换机管理标签。
 * @param[out] p_frame 存放普通以太网帧的缓冲区，不含 FCS/管理标签。
 * @param[in] capacity 缓冲区容量，单位字节；不足时丢弃该帧并返回错误。
 * @param[out] p_length 成功时为实际长度；失败或无可用帧时为 0。
 * @return FSP_SUCCESS：收到帧；FSP_ERR_ETHER_ERROR_NO_DATA：无帧或已丢弃其他端口帧；其他为错误。
 * @note 每次最多返回一帧，FSP 内部会丢弃错误帧；只允许 SOEM 任务调用，无额外队列或动态分配。
 */
fsp_err_t eth_receive_frame(uint8_t *p_frame, uint32_t capacity, uint32_t *p_length);

/**
 * @brief 监控 ETH_MONITOR_PORT，连续 READY 超过 ETH_MONITOR_STABLE_MS 后创建PDO主任务并等待清理通知。
 * @param[in] pvParameters FreeRTOS 任务参数，当前不使用，创建任务时传 NULL。
 * @return 不返回；收到PDO退出通知后重新识别链路。
 * @note 这里只提供任务入口，不创建任务；应在 eth_init() 成功后创建。
 *       本任务负责链路维护，运行时其他任务不得并发维护链路或调用网口收发接口。
 *       从首次检测到 READY 开始计时，非 READY 或查询错误会清零计时。
 *       按周期采样判断稳定；交接后由PDO任务维护链路，禁止两任务并发访问驱动。
 */
_Noreturn void eth_monitor_task_entry(void *pvParameters);

FSP_FOOTER
#endif
