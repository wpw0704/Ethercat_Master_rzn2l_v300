/***********************************************************************************************************************
 * Includes
 **********************************************************************************************************************/
#include "eth.h"
#include "main_thread.h"
#include "um_common.h"
#include <string.h>
/***********************************************************************************************************************
* Private constants
**********************************************************************************************************************/
/* 以下页号、寄存器和位操作沿用原有 phy_8211 实现，不新增 PHY 时序配置。 */
#define RTL_8211F_PAGE_SELECT       (0x1FU)
#define RTL_8211F_EEELCR_ADDR       (0x11U)
#define RTL_8211F_LED_PAGE          (0xD04U)
#define RTL_8211F_LCR_ADDR          (0x10U)
#define RTL_8211F_STANDARD_PAGE     (0xA42U)
/* 与 r_ether_phy.c 的 ETHER_PHY_REGISTER_READ_ERROR 保持一致。 */
#define ETH_PHY_READ_ERROR         (0xFFFFU)

/**********************************************************************************************************************
 * Typedef definitions
 **********************************************************************************************************************/

/***********************************************************************************************************************
 * Private function prototypes
 **********************************************************************************************************************/
static fsp_err_t eth_cleanup_partial_open(void);
static fsp_err_t eth_phy_read(ether_phy_instance_ctrl_t *p_ctrl, uint32_t reg, uint32_t *p_value);

/***********************************************************************************************************************
 * Imported function prototypes
 **********************************************************************************************************************/

/***********************************************************************************************************************
 * Private global variables
 **********************************************************************************************************************/
/* 仅在同一管理任务的初始化调用链中访问，不由 ISR 读写。 */
static bool g_eth_initialized = false;
static fsp_err_t g_eth_phy_init_error = FSP_SUCCESS;
/* 非零拷贝模式下仅供 CPU 暂存；实际 DMA 缓冲区仍由 FSP 放在非缓存区。 */
static uint8_t g_eth_tx_frame[ETH_GMAC_BUFFER_SIZE];
static uint8_t g_eth_rx_frame[ETH_GMAC_BUFFER_SIZE];

static ether_phy_instance_ctrl_t * const gp_eth_phy_ctrl[] =
{
    &g_ether_phy0_ctrl, &g_ether_phy1_ctrl, &g_ether_phy2_ctrl
};

static ether_selector_instance_ctrl_t * const gp_eth_selector_ctrl[] =
{
    &g_ether_selector0_ctrl, &g_ether_selector1_ctrl, &g_ether_selector2_ctrl
};

/***********************************************************************************************************************
* Functions
**********************************************************************************************************************/

/* 函数用途、参数及返回值见 eth.h。 */
fsp_err_t eth_init(void)
{
    fsp_err_t err;

    if (g_eth_initialized || (0U != g_ether0_ctrl.open) || (0U != g_ethsw0_ctrl.open))
    {
        return FSP_ERR_ALREADY_OPEN;
    }

    /* 保证失败清理只处理本次初始化取得的实例，不关闭其他模块已打开的资源。 */
    for (uint32_t i = 0U; i < BSP_FEATURE_GMAC_MAX_PORTS; i++)
    {
        if ((0U != gp_eth_phy_ctrl[i]->open) || (0U != gp_eth_selector_ctrl[i]->open))
        {
            return FSP_ERR_ALREADY_OPEN;
        }
    }

    if (taskSCHEDULER_RUNNING != xTaskGetSchedulerState())
    {
        return FSP_ERR_INVALID_MODE;
    }

    const ethsw_extend_cfg_t *p_switch_cfg = g_ethsw0.p_cfg->p_extend;
    /* R_GMAC_Read 没有容量参数，须在打开前确认暂存区能容纳驱动配置的最大帧。 */
    if ((ETHER_ZEROCOPY_DISABLE != g_ether0.p_cfg->zerocopy) ||
        (g_ether0.p_cfg->ether_buffer_size > ETH_GMAC_BUFFER_SIZE) ||
        (g_ether0.p_cfg->ether_buffer_size < ETH_FRAME_MAX_SIZE + ETH_MANAGEMENT_TAG_SIZE) ||
        (NULL == p_switch_cfg) || (ETHSW_SPECIFIC_TAG_ENABLE != p_switch_cfg->specific_tag))
    {
        return FSP_ERR_INVALID_MODE;
    }

    g_eth_phy_init_error = FSP_SUCCESS;
    /* FSP 负责模块时钟、复位、PHY/selector 和 DMA 配置，不重复操作寄存器。 */
    err = R_GMAC_Open(g_ether0.p_ctrl, g_ether0.p_cfg);
    if (FSP_SUCCESS != err)
    {
        fsp_err_t cleanup_err = eth_cleanup_partial_open();
        return (FSP_SUCCESS == cleanup_err) ? err : cleanup_err;
    }

    /* FSP 的自定义 PHY 回调没有返回值，必须额外检查其中的读写结果。 */
    if (FSP_SUCCESS != g_eth_phy_init_error)
    {
        err = R_GMAC_Close(g_ether0.p_ctrl);
        return (FSP_SUCCESS == err) ? g_eth_phy_init_error : err;
    }

    g_eth_initialized = true;
    return FSP_SUCCESS;
}

/* 函数用途、参数及返回值见 eth.h。 */
fsp_err_t eth_deinit(void)
{
    fsp_err_t err;

    if (!g_eth_initialized)
    {
        return FSP_ERR_NOT_OPEN;
    }

    err = R_GMAC_Close(g_ether0.p_ctrl);
    if (FSP_SUCCESS == err)
    {
        g_eth_initialized = false;
    }
    return err;
}

/* 函数用途、参数及返回值见 eth.h。 */
fsp_err_t eth_link_process(void)
{
    if (!g_eth_initialized)
    {
        return FSP_ERR_NOT_OPEN;
    }

    /* LinkProcess 成功时也可能所有端口都断开，不能据此报告 Link Ready。 */
    return R_GMAC_LinkProcess(g_ether0.p_ctrl);
}

/* 函数用途、参数及返回值见 eth.h。 */
fsp_err_t eth_get_link_status(uint8_t port, gmac_link_status_t *p_status)
{
    if (NULL == p_status)
    {
        return FSP_ERR_INVALID_POINTER;
    }
    *p_status = GMAC_LINK_STATUS_DOWN;

    if (port >= BSP_FEATURE_GMAC_MAX_PORTS)
    {
        return FSP_ERR_INVALID_ARGUMENT;
    }
    if (!g_eth_initialized)
    {
        return FSP_ERR_NOT_OPEN;
    }

    return R_GMAC_GetLinkStatus(g_ether0.p_ctrl, port, p_status);
}

/* 函数用途、参数及返回值见 eth.h。 */
fsp_err_t eth_send_frame(const uint8_t *p_frame, uint32_t length)
{
    gmac_link_status_t status;
    if (NULL == p_frame)
    {
        return FSP_ERR_INVALID_POINTER;
    }
    if ((length < 14U) || (length > ETH_FRAME_MAX_SIZE))
    {
        return FSP_ERR_INVALID_ARGUMENT;
    }
    fsp_err_t err = eth_get_link_status((uint8_t) ETH_SOEM_PORT, &status);
    if (FSP_SUCCESS != err)
    {
        return err;
    }
    if (GMAC_LINK_STATUS_READY != status)
    {
        return FSP_ERR_ETHER_ERROR_LINK;
    }

    const ethsw_extend_cfg_t *p_cfg = g_ethsw0.p_cfg->p_extend;
    uint32_t padded_length = (length < ETH_FRAME_MIN_SIZE) ? ETH_FRAME_MIN_SIZE : length;
    memcpy(g_eth_tx_frame, p_frame, 12U);
    /* 依据 Renesas um_ether_netif_ether.c/build_tx_management_tag：
     * 标签按网络字节序，control_data bit0 强制转发，control_data2 bit[2:0] 选择端口。 */
    g_eth_tx_frame[12] = (uint8_t) (p_cfg->specific_tag_id >> 8);
    g_eth_tx_frame[13] = (uint8_t) p_cfg->specific_tag_id;
    g_eth_tx_frame[14] = 0U;
    g_eth_tx_frame[15] = 1U;
    memset(&g_eth_tx_frame[16], 0, 3U);
    g_eth_tx_frame[19] = (uint8_t) (1U << ETH_SOEM_PORT);
    memcpy(&g_eth_tx_frame[20], &p_frame[12], length - 12U);
    memset(&g_eth_tx_frame[length + ETH_MANAGEMENT_TAG_SIZE], 0, padded_length - length);
    return R_GMAC_Write(g_ether0.p_ctrl, g_eth_tx_frame, padded_length + ETH_MANAGEMENT_TAG_SIZE);
}

/* 函数用途、参数及返回值见 eth.h。 */
fsp_err_t eth_receive_frame(uint8_t *p_frame, uint32_t capacity, uint32_t *p_length)
{
    uint32_t length = 0U;
    if (NULL == p_length)
    {
        return FSP_ERR_INVALID_POINTER;
    }
    *p_length = 0U;
    if (NULL == p_frame)
    {
        return FSP_ERR_INVALID_POINTER;
    }
    if (!g_eth_initialized)
    {
        return FSP_ERR_NOT_OPEN;
    }
    fsp_err_t err = R_GMAC_Read(g_ether0.p_ctrl, g_eth_rx_frame, &length);
    if (FSP_SUCCESS != err)
    {
        return err;
    }
    const ethsw_extend_cfg_t *p_cfg = g_ethsw0.p_cfg->p_extend;
    if ((length < 14U + ETH_MANAGEMENT_TAG_SIZE) ||
        (length > ETH_FRAME_MAX_SIZE + ETH_MANAGEMENT_TAG_SIZE) ||
        (g_eth_rx_frame[12] != (uint8_t) (p_cfg->specific_tag_id >> 8)) ||
        (g_eth_rx_frame[13] != (uint8_t) p_cfg->specific_tag_id))
    {
        return FSP_ERR_INVALID_ARGUMENT;
    }
    /* 接收标签 control_data bit[1:0] 是端口编号，而非发送时使用的端口掩码。 */
    if ((g_eth_rx_frame[15] & 3U) != ETH_SOEM_PORT)
    {
        return FSP_ERR_ETHER_ERROR_NO_DATA;
    }
    length -= ETH_MANAGEMENT_TAG_SIZE;
    if (length > capacity)
    {
        return FSP_ERR_INVALID_ARGUMENT;
    }
    memcpy(p_frame, g_eth_rx_frame, 12U);
    memcpy(&p_frame[12], &g_eth_rx_frame[20], length - 12U);
    *p_length = length;
    return FSP_SUCCESS;
}

/* 函数用途、参数及返回值见 eth.h。 */
void eth_monitor_task_entry(void *pvParameters)
{
    FSP_PARAMETER_NOT_USED(pvParameters);
    bool ready_timing = false;
    TickType_t ready_start_tick = 0U;
    fsp_err_t previous_error = FSP_SUCCESS;
    /* 门限向上取整并使用严格大于比较，避免 tick 取整使等待时间不足。 */
    const TickType_t stable_ticks = (TickType_t) ((((uint64_t) ETH_MONITOR_STABLE_MS *
                                                   (uint64_t) configTICK_RATE_HZ) + 999U) / 1000U);
    const TickType_t poll_ticks = (pdMS_TO_TICKS(ETH_MONITOR_POLL_MS) > 0U) ?
                                  pdMS_TO_TICKS(ETH_MONITOR_POLL_MS) : 1U;

    while (1)
    {
        gmac_link_status_t status = GMAC_LINK_STATUS_DOWN;
        fsp_err_t err = eth_link_process();
        if (FSP_SUCCESS == err)
        {
            err = eth_get_link_status((uint8_t) ETH_MONITOR_PORT, &status);
        }
        const TickType_t now = xTaskGetTickCount();

        if ((FSP_SUCCESS != err) || (GMAC_LINK_STATUS_READY != status))
        {
            /* 断链、仅 UP 或读取失败都打断连续稳定时间，下一次 READY 重新计时。 */
            ready_timing = false;
            if ((FSP_SUCCESS != err) && (err != previous_error))
            {
                USR_LOG_ERROR("ETH%u monitor error: %u", (unsigned int) ETH_MONITOR_PORT, (unsigned int) err);
            }
        }
        else if (!ready_timing)
        {
            ready_start_tick = now;
            ready_timing = true;
        }
        else if ((TickType_t) (now - ready_start_tick) > stable_ticks)
        {
            /* 无符号 tick 差值允许计时跨越 tick 回绕；满足条件后只打印一次。 */
            USR_LOG_INFO("ETH%u READY for more than %u ms; monitor task suspended.",
                         (unsigned int) ETH_MONITOR_PORT, (unsigned int) ETH_MONITOR_STABLE_MS);
            while (1)
            {
                vTaskSuspend(NULL);
            }
        }

        previous_error = err;
        /* 周期让出 CPU，期间持续检查连接状态，不直接阻塞等待 500 ms。 */
        vTaskDelay(poll_ticks);
    }
}

/**
 * @brief 清理 GMAC Open 失败后残留的子实例；不要求 GMAC 已标记为打开。
 * @param 无。
 * @return 首个清理错误；全部清理成功返回 FSP_SUCCESS。
 */
static fsp_err_t eth_cleanup_partial_open(void)
{
    fsp_err_t result = FSP_SUCCESS;
    fsp_err_t err;

    for (uint32_t i = 0U; i < BSP_FEATURE_GMAC_MAX_PORTS; i++)
    {
        if (0U != gp_eth_phy_ctrl[i]->open)
        {
            err = R_ETHER_PHY_Close(gp_eth_phy_ctrl[i]);
            if (FSP_SUCCESS == result)
            {
                result = err;
            }
        }
        /* PHY Open 可能在 selector 打开后失败，此时 PHY Close 不会关闭 selector。 */
        if (0U != gp_eth_selector_ctrl[i]->open)
        {
            err = R_ETHER_SELECTOR_Close(gp_eth_selector_ctrl[i]);
            if (FSP_SUCCESS == result)
            {
                result = err;
            }
        }
    }

    if (0U != g_ethsw0_ctrl.open)
    {
        err = R_ETHSW_Close(&g_ethsw0_ctrl);
        if (FSP_SUCCESS == result)
        {
            result = err;
        }
    }
    return result;
}

/**
 * @brief 检查 PHY 读取返回码及 FSP 使用的 0xFFFF 错误值。
 * @param[in,out] p_ctrl 正在初始化的 PHY 控制块。
 * @param[in] reg PHY 寄存器地址。
 * @param[out] p_value 读取到的值。
 * @return FSP_SUCCESS：有效读取；其他值为读取错误。
 */
static fsp_err_t eth_phy_read(ether_phy_instance_ctrl_t *p_ctrl, uint32_t reg, uint32_t *p_value)
{
    fsp_err_t err = R_ETHER_PHY_Read(p_ctrl, reg, p_value);

    if ((FSP_SUCCESS == err) && (ETH_PHY_READ_ERROR == (*p_value & ETH_PHY_READ_ERROR)))
    {
        err = FSP_ERR_ETHER_ERROR_PHY_COMMUNICATION;
    }
    return err;
}

/* 函数用途、参数及返回值见 eth.h。 */
void phy_8211(ether_phy_instance_ctrl_t *p_instance_ctrl)
{
    uint32_t value = 0U;
    fsp_err_t err;
    fsp_err_t restore_err;

    if (NULL == p_instance_ctrl)
    {
        if (FSP_SUCCESS == g_eth_phy_init_error)
        {
            g_eth_phy_init_error = FSP_ERR_INVALID_POINTER;
        }
        return;
    }

    /* 沿用现有稳定等待，仅在初始化任务中执行，不属于周期收发路径。 */
    vTaskDelay(pdMS_TO_TICKS(100));
    err = R_ETHER_PHY_Write(p_instance_ctrl, RTL_8211F_PAGE_SELECT, RTL_8211F_LED_PAGE);
    if (FSP_SUCCESS == err)
    {
        err = eth_phy_read(p_instance_ctrl, RTL_8211F_LCR_ADDR, &value);
    }
    if (FSP_SUCCESS == err)
    {
        /* 保留原有 LED 位配置，读取失败时禁止使用无效数据进行读改写。 */
        value |= (1U << 5) | (1U << 8) | (1U << 10) | (1U << 11);
        value &= ~(1U << 9);
        err = R_ETHER_PHY_Write(p_instance_ctrl, RTL_8211F_LCR_ADDR, value);
    }
    if (FSP_SUCCESS == err)
    {
        err = eth_phy_read(p_instance_ctrl, RTL_8211F_EEELCR_ADDR, &value);
    }
    if (FSP_SUCCESS == err)
    {
        value &= ~(1U << 2);
        err = R_ETHER_PHY_Write(p_instance_ctrl, RTL_8211F_EEELCR_ADDR, value);
    }

    /* 成功和失败路径均尝试恢复原工程使用的 0xA42 页，供后续协商读取使用。 */
    restore_err = R_ETHER_PHY_Write(p_instance_ctrl, RTL_8211F_PAGE_SELECT, RTL_8211F_STANDARD_PAGE);
    if (FSP_SUCCESS == err)
    {
        err = restore_err;
    }
    if (FSP_SUCCESS == g_eth_phy_init_error)
    {
        g_eth_phy_init_error = err;
    }
}

/* 函数用途、参数及返回值见 eth.h。 */
void vEtherISRCallback(ether_callback_args_t *p_args)
{
    /* FSP ISR 已处理硬件中断。后续 SOEM 可在任务中轮询 DMA 接收描述符。
     * 此处不打印、不等待、不调用链路维护，也不创建 ISR/任务共享状态。 */
    FSP_PARAMETER_NOT_USED(p_args);
}
