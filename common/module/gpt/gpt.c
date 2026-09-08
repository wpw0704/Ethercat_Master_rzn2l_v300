/***********************************************************************************************************************
 * Includes
 **********************************************************************************************************************/
#include "gpt.h"
/***********************************************************************************************************************
* Private constants
**********************************************************************************************************************/

/**********************************************************************************************************************
 * Typedef definitions
 **********************************************************************************************************************/

/***********************************************************************************************************************
 * Private function prototypes
 **********************************************************************************************************************/

/***********************************************************************************************************************
 * Imported function prototypes
 **********************************************************************************************************************/

/***********************************************************************************************************************
 * Private global variables
 **********************************************************************************************************************/
SemaphoreHandle_t s_gpt_cycle_semaphore = NULL;
static bool s_gpt_opened = false;
static bool s_gpt_started = false;
static StaticSemaphore_t s_cycle_buffer;
static bool initialized;
static int64_t target_phase_ns;
static int64_t integral;
/* ISR单写、任务只读的对齐32位计数；任务唤醒由信号量同步。 */
static volatile uint32_t s_cycle_count;

/***********************************************************************************************************************
* Functions
**********************************************************************************************************************/
/* 参数及返回值见头文件。 */
fsp_err_t gpt_init(void) {
    fsp_err_t err;

    if (s_gpt_cycle_semaphore == NULL) {
        s_gpt_cycle_semaphore = xSemaphoreCreateBinaryStatic(&s_cycle_buffer);
        if (s_gpt_cycle_semaphore == NULL) {
            USR_LOG_ERROR("GPT semaphore create failed");
            return FSP_ERR_NOT_INITIALIZED;
        }
    }

    if (!s_gpt_opened) {
        err = R_GPT_Open(&g_timer0_ctrl,
                         &g_timer0_cfg);

        if (err != FSP_SUCCESS) {
            USR_LOG_ERROR(
                    "GPT open failed: %d",
                    err);

            return err;
        }

        s_gpt_opened = true;
    }

    if (s_gpt_started) {
        return FSP_SUCCESS;
    }

    timer_info_t info;
    err = R_GPT_InfoGet(&g_timer0_ctrl, &info);
    if (err != FSP_SUCCESS) { return err; }
    if ((uint64_t) info.clock_frequency * ETHERCAT_DC_CYCLE_NS / 1000000000ULL != GPT_NOMINAL_PERIOD_COUNTS) {
        return FSP_ERR_INVALID_ARGUMENT;
    }
    /* PDO固定1 ms；停表时设置周期会复位计数，不修改自动生成配置。 */
    err = R_GPT_PeriodSet(&g_timer0_ctrl, GPT_NOMINAL_PERIOD_COUNTS);
    if (err != FSP_SUCCESS) { return err; }
    (void) xSemaphoreTake(s_gpt_cycle_semaphore, 0U);
    initialized = false;
    integral = 0;
    s_cycle_count = 0U;
    err = R_GPT_Start(&g_timer0_ctrl);

    if (err != FSP_SUCCESS) {
        USR_LOG_ERROR(
                "GPT start failed: %d",
                err);

        return err;
    }

    s_gpt_started = true;

    return FSP_SUCCESS;
}

/* 参数及返回值见头文件。 */
fsp_err_t gpt_stop(void) {
    fsp_err_t err;

    if (!s_gpt_started) {
        return FSP_SUCCESS;
    }

    err = R_GPT_Stop(&g_timer0_ctrl);

    if (err == FSP_SUCCESS) {
        s_gpt_started = false;
    }

    return err;
}

/** @brief GPT回调，p_args为事件，无返回值；仅ISR调用。 */
void gpt0_callback(timer_callback_args_t *p_args) {
    if (p_args != NULL && p_args->event == TIMER_EVENT_CYCLE_END && s_gpt_cycle_semaphore != NULL) {
        /* 中断只记录节拍、释放信号量，不进行PDO收发或日志。 */
        s_cycle_count++;
        BaseType_t higher_priority_task_woken =
                pdFALSE;
        xSemaphoreGiveFromISR(
                s_gpt_cycle_semaphore,
                &higher_priority_task_woken);
        portYIELD_FROM_ISR(
                higher_priority_task_woken);
    }
}

/**
 * @brief 根据 EtherCAT DC 时间微调 GPT 周期
 *
 * 第一次调用时记录当前相位，后续保持该相位不再缓慢漂移。
 * 只微调 GPT 周期，不改变 EtherCAT 的1ms通信周期。
 *
 * @param dc_time_ns SOEM提供的分布式时钟时间 ec_DCtime，单位ns
 * @return FSP_SUCCESS成功，否则为驱动错误。
 */
fsp_err_t gpt_dc_sync_adjust(int64_t dc_time_ns)
{
    if (!s_gpt_started) { return FSP_ERR_NOT_INITIALIZED; }

    int64_t phase_ns;
    int64_t error_ns;
    int64_t adjust_ns;
    int64_t period_counts;

    /* 计算当前PDO执行时刻在1ms DC周期内的位置 */
    phase_ns = dc_time_ns % ETHERCAT_DC_CYCLE_NS;
    if (phase_ns < 0) {
        phase_ns += ETHERCAT_DC_CYCLE_NS;
    }

    /*
     * 第一次运行时，以当前相位作为目标。
     * 这样不会突然把相位拉到其他位置，能够避免电机受到冲击。
     */
    if (!initialized) {
        target_phase_ns = phase_ns;
        integral = 0;
        initialized = true;
        return FSP_SUCCESS;
    }

    error_ns = phase_ns - target_phase_ns;

    /* 处理1ms周期边界，取得最短方向的相位误差 */
    if (error_ns > (ETHERCAT_DC_CYCLE_NS / 2)) {
        error_ns -= ETHERCAT_DC_CYCLE_NS;
    } else if (error_ns < -(ETHERCAT_DC_CYCLE_NS / 2)) {
        error_ns += ETHERCAT_DC_CYCLE_NS;
    }

    /*
     * 缓慢积分，用于补偿GPT和EtherCAT DC之间的长期频率误差。
     * 不直接累加error_ns，避免积分变化过快。
     */
    if (error_ns > 0) {
        integral++;
    } else if (error_ns < 0) {
        integral--;
    }

    /* 防止积分无限增长 */
    if (integral > 20000) {
        integral = 20000;
    } else if (integral < -20000) {
        integral = -20000;
    }

    /*
     * PI形式的周期修正：
     * 相位偏大时缩短下一周期，相位偏小时延长下一周期。
     */
    adjust_ns = -(error_ns / 100) - (integral / 20);

    /* 限制单周期修正量，避免PDO周期发生突变 */
    if (adjust_ns > GPT_MAX_ADJUST_NS) {
        adjust_ns = GPT_MAX_ADJUST_NS;
    } else if (adjust_ns < -GPT_MAX_ADJUST_NS) {
        adjust_ns = -GPT_MAX_ADJUST_NS;
    }

    /*
     * 当前配置中：
     * 1,000,000ns对应400,000个GPT计数。
     */
    period_counts =
            GPT_NOMINAL_PERIOD_COUNTS +
            ((adjust_ns * GPT_NOMINAL_PERIOD_COUNTS) /
             ETHERCAT_DC_CYCLE_NS);

    return R_GPT_PeriodSet(&g_timer0_ctrl, (uint32_t) period_counts);
}

/* 参数及返回值见头文件。 */
uint32_t gpt_cycle_count(void) {
    return s_cycle_count;
}
