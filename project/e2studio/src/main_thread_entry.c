/***********************************************************************************************************************
 * Includes
 **********************************************************************************************************************/
#include "main_thread.h"
#include "um_serial_io_api.h"
#include "um_common.h"
#include "eth.h"
#include "FreeRTOS.h"
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
                eth_monitor_tsk, IO_TASK_PRIORITY,
                NULL);
        if (pdTRUE != rtos_err) {
            USR_LOG_ERROR("Failed to create eth_monitor_task task.");
        }
        USR_LOG_INFO("Created sample eth_monitor_task task.");
    }
    while (1) {
        vTaskDelay(1);
    }
}
