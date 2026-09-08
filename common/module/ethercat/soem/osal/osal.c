#include "osal.h"
#include "FreeRTOS.h"
#include "task.h"

#define UNIX_TO_ECAT_EPOCH_SECONDS    (946684800ULL)


/** @brief 启动 self 定时器，timeout_us 为微秒数；无返回值，NULL 忽略。
 * @note 实际精度为 FreeRTOS tick；额外一个 tick 避免在当前 tick 尾部启动时过早超时。
 */
void osal_timer_start(osal_timert *self, uint32 timeout_us)
{
    TickType_t timeout_ticks;

    if (self == NULL)
    {
        return;
    }

    /* 64 位中间值避免微秒加法及换算乘法溢出；timeout=0 仍表示立即到期。 */
    timeout_ticks = (TickType_t) ((((uint64) timeout_us * configTICK_RATE_HZ) + 999999ULL) / 1000000ULL);
    if (timeout_us > 0U)
    {
        timeout_ticks++;
    }

    /*
     * stop_time.sec  保存启动tick
     * stop_time.usec 保存持续tick
     */
    self->stop_time.sec  = (uint32)xTaskGetTickCount();
    self->stop_time.usec = (uint32)timeout_ticks;
}


/** @brief 查询 self 是否到期；返回 TRUE/FALSE，NULL 按到期处理，无符号差值支持 tick 回绕。 */
boolean osal_timer_is_expired(osal_timert *self)
{
    TickType_t now;
    TickType_t start;
    TickType_t timeout_ticks;
    TickType_t elapsed;

    if (self == NULL)
    {
        return TRUE;
    }

    now           = xTaskGetTickCount();
    start         = (TickType_t)self->stop_time.sec;
    timeout_ticks = (TickType_t)self->stop_time.usec;
    elapsed       = now - start;

    return (elapsed >= timeout_ticks) ? TRUE : FALSE;
}

/* usec 为延时微秒数，向上取整到 tick；返回 1。仅供任务使用，不用于精确周期定时。 */
int osal_usleep(uint32 usec) {
    TickType_t ticks = (TickType_t) ((((uint64) usec * configTICK_RATE_HZ) + 999999ULL) / 1000000ULL);
    vTaskDelay((usec > 0U) ? ticks + 1U : 0U);
    return 1;
}

/**
 * @brief 获取供 SOEM 使用的当前时间。
 *
 * 当前系统没有提供真实 Unix/RTC 时间，因此使用：
 *
 * Unix到EtherCAT纪元偏移 + FreeRTOS启动运行时间
 *
 * ecx_configdc() 内部会再减去 946684800 秒，
 * 最终得到以系统启动时刻为零点的 EtherCAT DC 时间。
 */
ec_timet osal_current_time(void)
{
    ec_timet t;
    TickType_t ticks;
    uint64 uptime_us;
    uint64 time_sec;

    ticks = xTaskGetTickCount();

    /* FreeRTOS tick转换成系统启动后的微秒数 */
    uptime_us =
        ((uint64)ticks * 1000000ULL) /
        (uint64)configTICK_RATE_HZ;

    /*
     * 提前加上Unix到EtherCAT纪元的偏移。
     * ecx_configdc()会减去相同的946684800秒。
     */
    time_sec =
        UNIX_TO_ECAT_EPOCH_SECONDS +
        (uptime_us / 1000000ULL);

    t.sec  = (uint32)time_sec;
    t.usec = (uint32)(uptime_us % 1000000ULL);

    return t;
}

/* 计算两个 SOEM 时间点之间的时间差。 */
void osal_time_diff(ec_timet *start, ec_timet *end, ec_timet *diff) {
    uint64 s = (uint64)start->sec * 1000000ULL + start->usec;
    uint64 e = (uint64)end->sec   * 1000000ULL + end->usec;
    uint64 d = (e >= s) ? (e - s) : 0;
    diff->sec = (uint32)(d / 1000000ULL);
    diff->usec = (uint32)(d % 1000000ULL);
}

/* 创建普通线程的 OSAL 接口，本工程当前未启用线程创建功能。 */
int osal_thread_create(void *thandle, int stacksize, void *func, void *param) {
    (void)thandle; (void)stacksize; (void)func; (void)param;
    return 0; // Not supported / Not needed for pure loop
}

/* 创建实时线程的 OSAL 接口，本工程当前未启用实时线程创建功能。 */
int osal_thread_create_rt(void *thandle, int stacksize, void *func, void *param) {
    (void)thandle; (void)stacksize; (void)func; (void)param;
    return 0;
}
