/***********************************************************************************************************************
 * Includes
 **********************************************************************************************************************/
#include "main_thread.h"
#include "um_serial_io_api.h"
#include "um_common.h"
#include "eth.h"
#include "FreeRTOS.h"
#include "ethercat_master.h"
/***********************************************************************************************************************
* Private constants
**********************************************************************************************************************/

/**********************************************************************************************************************
 * Typedef definitions
 **********************************************************************************************************************/
#define KEYTEST 1
typedef struct {
    uint8_t pressed_count;
    bool latched;
} key_filter_t;

/***********************************************************************************************************************
 * Private function prototypes
 **********************************************************************************************************************/
static bool key_pressed(bsp_io_port_pin_t pin);

static bool key_press_event(bsp_io_port_pin_t pin, key_filter_t *filter);
/***********************************************************************************************************************
 * Imported function prototypes
 **********************************************************************************************************************/

/***********************************************************************************************************************
 * Private global variables
 **********************************************************************************************************************/
static TaskHandle_t eth_monitor_tsk = NULL;
/***********************************************************************************************************************
* Functions
**********************************************************************************************************************/
/**
 * @brief 初始化串口、网口并周期维护链路，仅在端口状态变化时输出日志。
 * @param[in] pvParameters FreeRTOS 线程参数，本函数不使用。
 * @return 不返回。
 */
_Noreturn void main_thread_entry(void *pvParameters) {
    FSP_PARAMETER_NOT_USED (pvParameters);
    /** Error status */
    usr_err_t usr_err = USR_SUCCESS;
    usr_err = um_serial_io_uart_open(&g_uart0);
    BaseType_t rtos_err; /** Rtos status */
    if (USR_SUCCESS != usr_err) {
        /** Note:Error Handling  */
        while (1) {
            vTaskSuspend(NULL);
        }
    }
    USR_LOG_INFO("Started Serial I/O interface.");
    USR_LOG_INFO("3333333");
    fsp_err_t eth_err = eth_init();
    if (FSP_SUCCESS != eth_err) {
        USR_LOG_ERROR("Ethernet initialization failed: %u", (unsigned int) eth_err);
        while (1) {
            vTaskSuspend(NULL);
        }
    } else {
        rtos_err = xTaskCreate(
                eth_monitor_task_entry, ETH_MONITOR_TASK_NAME,
                ETH_MONITOR_TASK_STACK_SIZE / sizeof(StackType_t),
                NULL, ETH_MONITOR_TASK_PRIORITY,
                &eth_monitor_tsk);
        if (pdTRUE != rtos_err) {
            USR_LOG_ERROR("Failed to create eth_monitor_task task.");
        }
        USR_LOG_INFO("Created sample eth_monitor_task task.");
    }
    while (1) {
#if  KEYTEST
        static key_filter_t key1_filter;
        static key_filter_t key2_filter;

        ethercat_motion_status_t status;
        int result;

        vTaskDelay(pdMS_TO_TICKS(100));

#if 0
        /*
         * KEY1：
         * 从当前位置正向移动1mm；
         * 速度2mm/s；
         * 加速度10mm/s²。
         */
        if (key_press_event(KEY1, &key1_filter)) {
            result = ethercat_motion_command_set(
                ETHERCAT_MOTION_MODE_MOVE_REL,
                10.0f,
                2.0f,
                10.0f);

            USR_LOG_INFO("KEY1 motion result=%d", result);
        }
#else
        /*
     * KEY1：启动持续往复运动。
     *
     * 行程：当前位置到正方向1mm
     * 速度：2mm/s
     * 加速度：10mm/s²
     * 到达终点等待：500ms
     * 返回起点后等待：500ms
     * 往返次数：0表示一直往返
     */
        /* 上电及重连后保持 OP，收到明确的使能请求才将标志置 1。 */
        if (key_press_event(KEY1, &key1_filter)) {
            if (s_servo_enable_request == 0){
                s_servo_enable_request = 1;
                continue;
            }
            result = ethercat_motion_recip_start(
                    20.0f,
                    40.0f,
                    80.0f,
                    CSP_LOCAL_JERK_MM_S3,
                    100U,
                    1000U,
                    ETHERCAT_MOTION_RECIP_FOREVER);

            if (result == ETHERCAT_MOTION_OK) {
                USR_LOG_INFO("KEY1 reciprocation started.");
            } else {
                USR_LOG_ERROR(
                        "KEY1 reciprocation start failed: %d",
                        result);
            }
        }
#endif

        /*
         * KEY2：
         * 如果正在运动则暂停；
         * 如果已经暂停则继续；
         * 如果当前空闲则不操作。
         */
        if (key_press_event(KEY2, &key2_filter)) {
            ethercat_motion_status_get(&status);

            if (status.busy && !status.paused) {
                ethercat_motion_stop();
                USR_LOG_INFO("KEY2 motion pause");
            } else if (status.busy && status.paused) {
                result = ethercat_motion_continue();
                USR_LOG_INFO(
                        "KEY2 motion continue result=%d",
                        result);
            } else {
                USR_LOG_INFO("KEY2 motion idle");
            }
        }
#else
        vTaskDelete(NULL);
# endif
    }
}

static bool key_press_event(bsp_io_port_pin_t pin,
                            key_filter_t *filter) {
    bool pressed = key_pressed(pin);

    if (pressed) {
        if (filter->pressed_count < 3U) {
            filter->pressed_count++;
        }

        /*
         * 连续检测到3次低电平，确认按下。
         * 一次按下只返回一次true。
         */
        if ((filter->pressed_count >= 3U) &&
            (!filter->latched)) {
            filter->latched = true;
            return true;
        }
    } else {
        filter->pressed_count = 0U;
        filter->latched = false;
    }

    return false;
}

static bool key_pressed(bsp_io_port_pin_t pin) {
    bsp_io_level_t level;

    if (R_IOPORT_PinRead(&g_ioport_ctrl,
                         pin,
                         &level) != FSP_SUCCESS) {
        return false;
    }

    /* 原理图外部上拉，按下时为低电平。 */
    return level == BSP_IO_LEVEL_LOW;
}
