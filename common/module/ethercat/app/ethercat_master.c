/***********************************************************************************************************************
 * Includes
 **********************************************************************************************************************/
#include "ethercat_master.h"
#include "eth.h"
#include <stddef.h>
#include <limits.h>
/***********************************************************************************************************************
* Private constants
**********************************************************************************************************************/

/**********************************************************************************************************************
 * Typedef definitions
 **********************************************************************************************************************/

/***********************************************************************************************************************
 * Private function prototypes
 **********************************************************************************************************************/
static int ecat_init(void);

static void ethercat_pdo_monitor_log_task(void *pvParameters);

static int ethercat_servo_enable_process(uint16_t slave, int8 op_mode);

static int ethercat_master_pdo_process_check(int wkc);

static int ethercat_master_read_state(void);

static int ethercat_master_startup_pdo_exchange(void);

static int ethercat_master_safe_op_warmup(void);

static int ethercat_master_slave_request_op(uint16_t slave);
/***********************************************************************************************************************
 * Imported function prototypes
 **********************************************************************************************************************/

/***********************************************************************************************************************
 * Private global variables
 **********************************************************************************************************************/
/* 单轴沿用例子的PDO布局；只缩小主站映射区，不修改运动模块。 */
static char IOmap[sizeof(PDO_Output) + sizeof(PDO_Input)];
#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "PDO requires little-endian target"
#endif
typedef char pdo_output_layout_check[(sizeof(PDO_Output) == 13U && offsetof(PDO_Output, TargetPos) == 2U) ? 1 : -1];
typedef char pdo_input_layout_check[(sizeof(PDO_Input) == 17U && offsetof(PDO_Input, CurrentPosition) == 2U) ? 1 : -1];
static int s_expected_wkc;
static int s_last_wkc;
static bool s_mapping_ok;
static bool s_nic_open;
static bool s_master_op;
static uint32_t s_cycle_count;
static uint32_t s_last_gpt_cycle;
static ethercat_pdo_monitor_t s_pdo_monitor;
static int32 s_last_position;
static uint16 s_last_status_word;
static int8 s_last_mode;
static uint8_t s_has_last_sample;
static servo_enable_state_t s_enable_state;
static uint32_t s_enable_wait_count;
static int32 s_servo_enable_hold_pos;
static int8_t s_enable_result;
static TaskHandle_t monitor_log = NULL;

TaskHandle_t ethercat_task_handle = NULL;
PDO_Output *output1s = NULL;
PDO_Input *input1s = NULL;
uint8_t s_servo_enable_request = 0U;
/***********************************************************************************************************************
* Functions
**********************************************************************************************************************/
/*
 * SDO 写封装函数。
 * 用于在 PRE-OP 到 SAFE-OP 阶段配置 PDO 映射对象，例如 0x1600、0x1A00、0x1C12、0x1C13。
 * 注意：SDO 是邮箱通信，不能放在 1ms PDO 周期任务中频繁调用。
 */
/** @brief 写8位SDO；slave/index/subindex为地址，value为值；返回WKC，<=0失败。 */
int write8(uint16 slave, uint16 index, uint8 subindex, unsigned char value) {
    uint8 temp = value;

    int rtn = ec_SDOwrite(slave, index, subindex, FALSE, sizeof(temp), &temp, EC_TIMEOUTRXM);

    if (rtn <= 0) {
        printf("[Axis %u][SDO8] write failed: index=0x%04x sub=0x%02x\r\n",
               (unsigned int) slave,
               (unsigned int) index,
               (unsigned int) subindex);
    } else {
        printf("[Axis %u][SDO8] index=0x%04x sub=0x%02x value=0x%02x OK\r\n",
               (unsigned int) slave,
               (unsigned int) index,
               (unsigned int) subindex,
               (unsigned int) temp);
    }
    return rtn;
}

/** @brief 写16位SDO；slave/index/subindex为地址，value为值；返回WKC，<=0失败。 */
int write16(uint16 slave, uint16 index, uint8 subindex, unsigned short value) {
    uint16 temp = value;

    int rtn = ec_SDOwrite(slave, index, subindex, FALSE, sizeof(temp), &temp, EC_TIMEOUTRXM * 20);
    // int rtn = ec_SDOwrite(slave, index, subindex, FALSE, sizeof(temp), &temp, EC_TIMEOUTRXM);

    if (rtn <= 0) {
        printf("[Axis %u][SDO16] write failed: index=0x%04x sub=0x%02x\r\n",
               (unsigned int) slave,
               (unsigned int) index,
               (unsigned int) subindex);
    } else {
        printf("[Axis %u][SDO16] index=0x%04x sub=0x%02x value=0x%04x OK\r\n",
               (unsigned int) slave,
               (unsigned int) index,
               (unsigned int) subindex,
               (unsigned int) temp);
    }
    return rtn;
}

/** @brief 写32位SDO；slave/index/subindex为地址，value为值；返回WKC，<=0失败。 */
int write32(uint16 slave, uint16 index, uint8 subindex, unsigned long value) {
    uint32 temp = value;

    // int rtn = ec_SDOwrite(slave, index, subindex, FALSE, sizeof(temp), &temp, EC_TIMEOUTRXM * 20);
    int rtn = ec_SDOwrite(slave, index, subindex, FALSE, sizeof(temp), &temp, EC_TIMEOUTRXM);
    if (rtn <= 0) {
        printf("[Axis %u][SDO32] write failed: index=0x%04x sub=0x%02x\r\n",
               (unsigned int) slave,
               (unsigned int) index,
               (unsigned int) subindex);
    } else {
        printf("[Axis %u][SDO32] index=0x%04x sub=0x%02x value=0x%08lx OK\r\n",
               (unsigned int) slave,
               (unsigned int) index,
               (unsigned int) subindex,
               (unsigned long) temp);
    }
    return rtn;
}

/** @brief 配置现有SV630N映射；slvcnt为从站号，全部成功返回1，否则返回0。 */
static int Servosetup(uint16 slvcnt) {
    printf("[Axis %u][PDO] configuring RxPDO/TxPDO mapping\r\n",
           (unsigned int) slvcnt);
    if (write8(slvcnt, 0x1C12, 00, 0) <= 0) { return 0; } // 清空0x1c12
    if (write8(slvcnt, 0x1600, 00, 0) <= 0) { return 0; } // 清空0x1600
    if (write32(slvcnt, 0x1600, 01, 0x60400010) <= 0) { return 0; } // 写入0x1600
    if (write32(slvcnt, 0x1600, 02, 0x607A0020) <= 0) { return 0; } // 写入0x1600
    if (write32(slvcnt, 0x1600, 03, 0x60FF0020) <= 0) { return 0; } // 写入0x1600
    if (write32(slvcnt, 0x1600, 04, 0x60600008) <= 0) { return 0; } // 写入0x1600
    if (write32(slvcnt, 0x1600, 05, 0x60B80010) <= 0) { return 0; } // 写入0x1600
    if (write8(slvcnt, 0x1600, 00, 5) <= 0) { return 0; }

    if (write16(slvcnt, 0x1C12, 01, 0x1600) <= 0) { return 0; } // 设定RxPDO映射
    if (write8(slvcnt, 0x1C12, 00, 1) <= 0) { return 0; }

    if (write8(slvcnt, 0x1C13, 00, 00) <= 0) { return 0; } // 清空0x1C12计数
    if (write8(slvcnt, 0x1A00, 00, 00) <= 0) { return 0; } // 清空0x1600计数
    if (write32(slvcnt, 0x1A00, 01, 0x60410010) <= 0) { return 0; } // 写入0x1A00
    if (write32(slvcnt, 0x1A00, 02, 0x60640020) <= 0) { return 0; } // 写入0x1A00
    if (write32(slvcnt, 0x1A00, 03, 0x60610008) <= 0) { return 0; } // 写入0x1A00
    if (write32(slvcnt, 0x1A00, 04, 0x60B90010) <= 0) { return 0; } // 写入0x1A00
    if (write32(slvcnt, 0x1A00, 05, 0x60BA0020) <= 0) { return 0; } // 写入0x1A00
    if (write32(slvcnt, 0x1A00, 06, 0x60FD0020) <= 0) { return 0; } // 写入0x1A00
    if (write8(slvcnt, 0x1A00, 00, 06) <= 0) { return 0; }

    if (write16(slvcnt, 0x1C13, 01, 0x1A00) <= 0) { return 0; }
    if (write8(slvcnt, 0x1C13, 00, 01) <= 0) { return 0; }

    s_mapping_ok = true;
    return 1;
}

/**
 * @brief 沿用例子的扫描、映射、SAFE-OP预热和OP确认，固定为1号从站。
 * @param 无。
 * @return 单轴稳定OP返回1，否则返回0，由主任务统一清理。
 */
static int ecat_init(void) {
    if (!ec_init(ETH_SOEM_IFNAME)) { return 0; }
    s_nic_open = true;
    int const scan_result = ec_config_init(TRUE);
    USR_LOG_INFO("[Master] scan=%d slavecount=%d.", scan_result, ec_slavecount);
    if ((scan_result != 1) || (ec_slavecount != 1)) {
        USR_LOG_ERROR("[Master] exactly one servo is required.");
        return 0;
    }
    if ((ec_slave[ETHERCAT_SLAVE].eep_man != 0x100000UL) ||
        (ec_slave[ETHERCAT_SLAVE].eep_id != 0xc0112UL)) {
        USR_LOG_ERROR("[Master] unsupported servo vendor/product.");
        return 0;
    }
    ec_slave[ETHERCAT_SLAVE].PO2SOconfig = &Servosetup;
    if (!ec_configdc() || !ec_slave[ETHERCAT_SLAVE].hasdc) { return 0; }
    ec_dcsync0(ETHERCAT_SLAVE, TRUE, ETHERCAT_DC_SYNC0_CYCLE_NS, 0);
    int const map_size = ec_config_map(IOmap);
    if (!s_mapping_ok || (map_size != (int) sizeof(IOmap)) ||
        (ec_slave[ETHERCAT_SLAVE].Obits != sizeof(PDO_Output) * 8U) ||
        (ec_slave[ETHERCAT_SLAVE].Ibits != sizeof(PDO_Input) * 8U) ||
        (ec_slave[ETHERCAT_SLAVE].Ostartbit != 0U) || (ec_slave[ETHERCAT_SLAVE].Istartbit != 0U) ||
        (ec_slave[ETHERCAT_SLAVE].outputs != (uint8 *) IOmap) ||
        (ec_slave[ETHERCAT_SLAVE].inputs != (uint8 *) IOmap + sizeof(PDO_Output))) {
        USR_LOG_ERROR("[Master] PDO mapping invalid: size=%d SDO=%d.", map_size, (int) s_mapping_ok);
        return 0;
    }
    output1s = (PDO_Output *) ec_slave[ETHERCAT_SLAVE].outputs;
    input1s = (PDO_Input *) ec_slave[ETHERCAT_SLAVE].inputs;
    output1s->ControlWord = CIA402_CW_DISABLE_VOLTAGE;
    output1s->TargetVelocity = 0;
    output1s->OpModeSet = 8;
    output1s->TouchProbe = 0U;
    s_expected_wkc = ec_group[0].outputsWKC * 2 + ec_group[0].inputsWKC;
    if (!ec_group[0].outputsWKC || !ec_group[0].inputsWKC) { return 0; }
    (void) ec_statecheck(ETHERCAT_SLAVE, EC_STATE_SAFE_OP, EC_TIMEOUTSTATE);
    if (!ethercat_master_read_state() || ec_slave[ETHERCAT_SLAVE].state != EC_STATE_SAFE_OP) { return 0; }
    if (gpt_init() != FSP_SUCCESS) { return 0; }
    if (!ethercat_master_safe_op_warmup() || !ethercat_master_slave_request_op(ETHERCAT_SLAVE)) { return 0; }
    return 1;
}

/** @brief 读取1号从站实际AL状态并刷新SOEM缓存；无参数，读取成功返回1，否则返回0。 */
static int ethercat_master_read_state(void) {
    ec_alstatust status = {0};
    int const wkc = ec_FPRD(ec_slave[ETHERCAT_SLAVE].configadr, ECT_REG_ALSTAT,
                            (uint16) sizeof(status), &status, EC_TIMEOUTRET);
    ec_slave[ETHERCAT_SLAVE].state = (wkc == 1) ? etohs(status.alstatus) : EC_STATE_NONE;
    ec_slave[ETHERCAT_SLAVE].ALstatuscode = (wkc == 1) ? etohs(status.alstatuscode) : 0U;
    return wkc == 1;
}

/*
 * 使用GPT信号量完成一次启动阶段PDO交换。
 * 参数：无。
 * 返回值：ec_receive_processdata()返回的实际WKC；GPT未启动或等待超时返回-1。
 */
static int ethercat_master_startup_pdo_exchange(void) {
    if (s_gpt_cycle_semaphore == NULL ||
        xSemaphoreTake(s_gpt_cycle_semaphore, pdMS_TO_TICKS(ETHERCAT_GPT_WAIT_MS)) != pdTRUE) {
        return -1;
    }
    uint32_t const cycle = gpt_cycle_count();
    uint32_t const gap = cycle - s_last_gpt_cycle;
    s_last_gpt_cycle = cycle;
    /* PDO和运动每1 ms推进；节拍已丢失时退出，不能追赶执行旧轨迹。 */
    if (gap != 1U) { return -1; }
    int const sent = ec_send_processdata();
    int const received = ec_receive_processdata(EC_TIMEOUTRET); // 即使发送失败也回收SOEM帧索引
    s_last_wkc = sent > 0 ? received : EC_NOFRAME;
    s_cycle_count++;
    if (s_last_wkc == s_expected_wkc && gpt_dc_sync_adjust(ec_DCtime) != FSP_SUCCESS) { return -1; }
    /* 识别任务已阻塞，收发结束后由PDO任务独占维护链路。 */
    if ((s_cycle_count % ETHERCAT_STATE_CHECK_INTERVAL == 0U) || (s_last_wkc != s_expected_wkc)) {
        gmac_link_status_t status = GMAC_LINK_STATUS_DOWN;
        if (eth_link_process() != FSP_SUCCESS ||
            eth_get_link_status(ETH_SOEM_PORT, &status) != FSP_SUCCESS ||
            status != GMAC_LINK_STATUS_READY || !ethercat_master_read_state()) { return -1; }
    }
    return s_last_wkc;
}

/*
 * 在SAFE_OP阶段按1 ms周期预热PDO。
 * 参数：无。
 * 返回值：连续完成预热返回1；GPT等待失败或WKC连续为0返回0。
 */
static int ethercat_master_safe_op_warmup(void) {
    uint32_t cycle;
    uint32_t zero_wkc_count = 0U;
    int wkc;

    for (cycle = 0U; cycle < ETHERCAT_SAFE_OP_WARMUP_CYCLES; cycle++) {
        wkc = ethercat_master_startup_pdo_exchange();
        if (wkc < 0) {
            return 0;
        }

        if (wkc == 0) {
            zero_wkc_count++;
            if (zero_wkc_count > ETHERCAT_ZERO_WKC_LIMIT) {
                return 0;
            }
        } else {
            zero_wkc_count = 0U;
        }
    }

    /*
     * SAFE_OP下已经取得有效输入，把单轴的当前位置作为后续使能前目标值，
     * 避免目标位置保持为IOmap初始化时的0。
     */
    s_servo_enable_hold_pos = input1s->CurrentPosition;
    output1s->TargetPos = s_servo_enable_hold_pos;

    return 1;
}

/*
 * 请求指定从站进入EtherCAT OP，并在GPT周期PDO不中断的条件下等待稳定。
 * 参数slave：SOEM从站号，仅接受1号从站。
 * 返回值：该轴进入OP并稳定返回1；参数错误、超时、AL错误或PDO异常返回0。
 */
static int ethercat_master_slave_request_op(uint16_t slave) {
    uint32_t cycle;
    uint32_t stable_count = 0U;
    uint32_t zero_wkc_count = 0U;
    uint16 actual_state = 0U;
    uint8_t state_confirmed = 0U;
    int wkc;
    int wkc_ready;

    if ((slave != ETHERCAT_SLAVE) || (input1s == NULL)) {
        return 0;
    }

    ec_slave[slave].state = EC_STATE_OPERATIONAL;
    if (ec_writestate(slave) != 1) { return 0; }

    for (cycle = 0U; cycle < ETHERCAT_AXIS_OP_TIMEOUT_CYCLES; cycle++) {
        wkc = ethercat_master_startup_pdo_exchange();
        if (wkc < 0) {
            return 0;
        }

        if (wkc == 0) {
            zero_wkc_count++;
            stable_count = 0U;
            if (zero_wkc_count > ETHERCAT_ZERO_WKC_LIMIT) {
                return 0;
            }
        } else {
            zero_wkc_count = 0U;
        }

        /*
         * 状态寄存器低频读取，避免在每个1 ms周期执行阻塞FPRD。
         * 直接FPRD避免状态不符时ec_statecheck内部附加sleep。
         */
        if ((cycle % ETHERCAT_STATE_CHECK_INTERVAL) == 0U) {
            if (!ethercat_master_read_state()) { return 0; }
            actual_state = ec_slave[slave].state;
            state_confirmed =
                    ((actual_state == EC_STATE_OPERATIONAL) &&
                     (ec_slave[slave].state == EC_STATE_OPERATIONAL))
                    ? 1U
                    : 0U;

            if ((ec_slave[slave].state & EC_STATE_ERROR) != 0U) {
                return 0;
            }
        }

        /* 单轴每个合格周期都必须完整WKC；不再保留中间轴的放宽判断。 */
        wkc_ready = (s_expected_wkc > 0) && (wkc == s_expected_wkc);

        if ((state_confirmed != 0U) && (wkc_ready != 0)) {
            stable_count++;
        } else {
            stable_count = 0U;
        }

        if (stable_count >= ETHERCAT_AXIS_OP_STABLE_CYCLES) {
            if (!ethercat_master_read_state()) { return 0; }
            actual_state = ec_slave[slave].state;
            if ((actual_state == EC_STATE_OPERATIONAL) &&
                (ec_slave[slave].state == EC_STATE_OPERATIONAL)) {
                return 1;
            }

            stable_count = 0U;
            state_confirmed = 0U;
        }
    }

    return 0;
}

/** @brief PDO主任务，参数为网口识别任务句柄；保持例子的收发/使能/运动顺序，清理后不返回。 */
void ethercat_task_entry(void *pvParameters) {
    TaskHandle_t const port_monitor = (TaskHandle_t) pvParameters;
    uint8_t previous_request = 0U;
    static StaticTask_t log_task_buffer;
    static StackType_t log_stack[4096U / sizeof(StackType_t)];
    if (monitor_log == NULL) {
        monitor_log = xTaskCreateStatic(ethercat_pdo_monitor_log_task, "PDO monitor",
                                        4096U / sizeof(StackType_t), NULL, tskIDLE_PRIORITY + 1U,
                                        log_stack, &log_task_buffer);
    }
    if (!ecat_init()) { goto cleanup; }
    /* 复用运动模块的同步接口，清除上次连接的轨迹及待处理命令。 */
    if (ethercat_motion_position_sync() != ETHERCAT_MOTION_OK ||
        ethercat_motion_motor_params_set(262144, 5, 1, 1, 3000) != ETHERCAT_MOTION_OK) { goto cleanup; }
    taskENTER_CRITICAL();
    s_master_op = true;
    s_servo_enable_request = 0U;
    taskEXIT_CRITICAL();
    for (;;) {
        int const wkc = ethercat_master_startup_pdo_exchange();
        if (wkc < 0 || !ethercat_master_pdo_process_check(wkc)) { break; }
        taskENTER_CRITICAL();
        uint8_t const request = s_servo_enable_request;
        taskEXIT_CRITICAL();
        if (request != previous_request) {
            s_enable_state = SERVO_ENABLE_IDLE;
            s_enable_wait_count = 0U;
            previous_request = request;
        }
        if (request != 0U) {
            /* 已运行后丢失使能/CSP反馈时退出，避免例子保持位置覆盖正在运行的轨迹。 */
            if (s_enable_state == SERVO_ENABLE_DONE &&
                ((input1s->StatusWord & CIA402_SW_MASK) != CIA402_SW_OPERATION_ENABLED || input1s->OpModeNow != 8)) {
                break;
            }
            s_enable_result = (int8_t) ethercat_servo_enable_process(ETHERCAT_SLAVE, 8);
            if (s_enable_result < 0) { break; }
            /* 每1 ms收发后推进一次轨迹；运动算法只调整周期参数。 */
            if (s_enable_result == 1) { ethercat_motion_process(); }
        } else {
            output1s->TargetPos = input1s->CurrentPosition;
            output1s->TargetVelocity = 0;
            output1s->ControlWord = CIA402_CW_SHUTDOWN;
            s_enable_result = 0;
            if ((input1s->StatusWord & CIA402_SW_MASK) == CIA402_SW_READY_TO_SWITCH_ON) {
                s_enable_state = SERVO_ENABLE_IDLE;
                s_enable_wait_count = 0U;
            }
        }
    }
    cleanup:
    taskENTER_CRITICAL();
    s_master_op = false;
    s_servo_enable_request = 0U;
    taskEXIT_CRITICAL();
    fsp_err_t const stop_result = gpt_stop();
    if (input1s != NULL && output1s != NULL) { (void) ethercat_motion_position_sync(); }
    if (s_nic_open) {
        if (ec_slavecount == 1 && ec_slave[ETHERCAT_SLAVE].configadr != 0U) {
            ec_slave[ETHERCAT_SLAVE].state = EC_STATE_SAFE_OP;
            (void) ec_writestate(ETHERCAT_SLAVE); // 断线时只能尽力发送，不能保证送达
        }
        ec_close();
    }
    USR_LOG_INFO("[Master] session stopped; WKC=%d/%d.", s_last_wkc, s_expected_wkc);
    taskENTER_CRITICAL();
    input1s = NULL;
    output1s = NULL;
    s_expected_wkc = 0;
    s_last_wkc = 0;
    s_mapping_ok = false;
    s_nic_open = false;
    s_cycle_count = 0U;
    s_last_gpt_cycle = 0U;
    s_enable_state = SERVO_ENABLE_IDLE;
    s_enable_wait_count = 0U;
    s_servo_enable_hold_pos = 0;
    s_enable_result = 0;
    s_last_position = 0;
    s_last_status_word = 0U;
    s_last_mode = 0;
    s_has_last_sample = 0U;
    s_pdo_monitor = (ethercat_pdo_monitor_t) {0};
    ec_slavecount = 0;
    ethercat_task_handle = NULL;
    taskEXIT_CRITICAL();
    memset(IOmap, 0, sizeof(IOmap));
    if (stop_result != FSP_SUCCESS) {
        USR_LOG_ERROR("[Master] GPT stop failed: %d.", (int) stop_result);
        for (;;) { vTaskSuspend(NULL); } // 未确认停止时不允许重启GPT
    }
    /* 计数通知允许识别任务稍后才开始等待；此后不再访问网口。 */
    xTaskNotifyGive(port_monitor);
    vTaskDelete(NULL);
}

/************************** 任务日志 **************************/
/*
 * 低优先级 PDO 监控日志任务。
 * 该任务每 1 秒打印一次缓存数据，不直接调用 SOEM 收发函数，避免影响 PDO 实时周期。
 */
static void ethercat_pdo_monitor_log_task(void *pvParameters) {
    ethercat_pdo_monitor_t mon;
    int8_t enable_result;
    servo_enable_state_t enable_state;

    (void) pvParameters;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));

        taskENTER_CRITICAL();
        mon = s_pdo_monitor;
        enable_state = s_enable_state;
        enable_result = s_enable_result;
        taskEXIT_CRITICAL();

        USR_LOG_INFO("[Axis %u][PDO] enable=%d step=%d ok=%d cyc=%lu good=%lu bad=%lu "
                     "wkc=%d/%d ec=0x%04x status=0x%04x ctrl=0x%04x "
                     "mode=%d pos=%ld dpos=%ld target=%ld.",
                     (unsigned int) ETHERCAT_SLAVE,
                     (int) enable_result,
                     (int) enable_state,
                     mon.pdo_ok,
                     (unsigned long) mon.cycle_count,
                     (unsigned long) mon.good_count,
                     (unsigned long) mon.bad_count,
                     mon.wkc,
                     mon.expected_wkc,
                     mon.state,
                     mon.status_word,
                     mon.control_word,
                     mon.mode,
                     (long) mon.position,
                     (long) mon.position_delta,
                     (long) mon.target_pos);
    }
}

/** @brief 移植例子的CiA402状态机；slave仅接受1，op_mode为模式；返回1已使能、0进行中、-1失败。 */
static int ethercat_servo_enable_process(uint16_t slave, int8 op_mode) {
    PDO_Input *input;
    PDO_Output *output;
    uint16 status_word;
    uint16 status_state;

    if (slave != ETHERCAT_SLAVE) {
        return -1;
    }

    input = input1s;
    output = output1s;
    if ((input == NULL) || (output == NULL)) {
        return -1;
    }

    status_word = input->StatusWord;
    status_state = status_word & CIA402_SW_MASK;

    if (op_mode != 0) {
        output->OpModeSet = op_mode;
    }

    /*
     * 驱动器已经使能时继续保持该轴锁定位置。
     * 首次发现已使能状态时先采用当前位置，避免目标值仍为0导致突跳。
     */
    if (status_state == CIA402_SW_OPERATION_ENABLED) {
        if (s_enable_state != SERVO_ENABLE_DONE) {
            s_servo_enable_hold_pos = input->CurrentPosition;
        }

        output->TargetPos = s_servo_enable_hold_pos;
        output->TargetVelocity = 0;
        output->ControlWord = CIA402_CW_ENABLE_OPERATION;

        if ((op_mode != 0) && (input->OpModeNow != op_mode)) {
            return 0;
        }

        s_enable_state = SERVO_ENABLE_DONE;
        s_enable_wait_count = 0U;
        return 1;
    }

    switch (s_enable_state) {
        case SERVO_ENABLE_IDLE:
            s_enable_wait_count = 0U;
            output->ControlWord = CIA402_CW_DISABLE_VOLTAGE;

            if ((status_word & CIA402_SW_FAULT_MASK) == CIA402_SW_FAULT) {
                s_enable_state = SERVO_ENABLE_FAULT_RESET_PULSE;
            } else {
                s_enable_state = SERVO_ENABLE_SHUTDOWN;
            }
            break;

        case SERVO_ENABLE_FAULT_RESET_PULSE:
            /* Fault Reset只保持一个PDO周期。 */
            output->ControlWord = CIA402_CW_FAULT_RESET;
            s_enable_state = SERVO_ENABLE_WAIT_FAULT_CLEAR;
            s_enable_wait_count = 0U;
            break;

        case SERVO_ENABLE_WAIT_FAULT_CLEAR:
            output->ControlWord = CIA402_CW_DISABLE_VOLTAGE;

            if ((status_word & CIA402_SW_FAULT_MASK) != CIA402_SW_FAULT) {
                s_enable_state = SERVO_ENABLE_SHUTDOWN;
                s_enable_wait_count = 0U;
            } else if (++s_enable_wait_count > ETHERCAT_SERVO_FAULT_TIMEOUT_CYCLES) {
                s_enable_state = SERVO_ENABLE_FAILED;
                return -1;
            }
            break;

        case SERVO_ENABLE_SHUTDOWN:
            output->ControlWord = CIA402_CW_SHUTDOWN;

            if (status_state == CIA402_SW_READY_TO_SWITCH_ON) {
                s_enable_state = SERVO_ENABLE_SWITCH_ON;
                s_enable_wait_count = 0U;
            } else if (++s_enable_wait_count > ETHERCAT_SERVO_ENABLE_TIMEOUT_CYCLES) {
                s_enable_state = SERVO_ENABLE_FAILED;
                return -1;
            }
            break;

        case SERVO_ENABLE_SWITCH_ON:
            output->ControlWord = CIA402_CW_SWITCH_ON;

            if (status_state == CIA402_SW_SWITCHED_ON) {
                s_servo_enable_hold_pos = input->CurrentPosition;
                output->TargetPos = s_servo_enable_hold_pos;
                output->TargetVelocity = 0;
                if (op_mode != 0) {
                    output->OpModeSet = op_mode;
                }

                s_enable_wait_count = 0U;
                s_enable_state = SERVO_ENABLE_CSP_PREPARE;
            } else if (++s_enable_wait_count > ETHERCAT_SERVO_ENABLE_TIMEOUT_CYCLES) {
                s_enable_state = SERVO_ENABLE_FAILED;
                return -1;
            }
            break;

        case SERVO_ENABLE_CSP_PREPARE:
            /*
             * 保持0x0007并连续发送当前位置，确认CSP模式后再输出0x000F。
             */
            output->ControlWord = CIA402_CW_SWITCH_ON;
            if (op_mode != 0) {
                output->OpModeSet = op_mode;
            }
            s_servo_enable_hold_pos = input->CurrentPosition;
            output->TargetPos = s_servo_enable_hold_pos;
            output->TargetVelocity = 0;
            s_enable_wait_count++;

            if (((op_mode == 0) || (input->OpModeNow == op_mode)) &&
                (s_enable_wait_count >= ETHERCAT_SERVO_CSP_PREPARE_CYCLES)) {
                s_enable_wait_count = 0U;
                s_enable_state = SERVO_ENABLE_ENABLE_OPERATION;
            } else if (s_enable_wait_count > ETHERCAT_SERVO_ENABLE_TIMEOUT_CYCLES) {
                s_enable_state = SERVO_ENABLE_FAILED;
                return -1;
            }
            break;

        case SERVO_ENABLE_ENABLE_OPERATION:
            output->TargetPos = s_servo_enable_hold_pos;
            output->TargetVelocity = 0;
            output->ControlWord = CIA402_CW_ENABLE_OPERATION;

            if (++s_enable_wait_count > ETHERCAT_SERVO_ENABLE_TIMEOUT_CYCLES) {
                s_enable_state = SERVO_ENABLE_FAILED;
                return -1;
            }
            break;

        case SERVO_ENABLE_DONE:
            /*
             * 状态字已离开Operation Enabled，先撤销控制字并重新进入状态机。
             */
            output->ControlWord = CIA402_CW_DISABLE_VOLTAGE;
            s_enable_state = SERVO_ENABLE_IDLE;
            s_enable_wait_count = 0U;
            break;

        case SERVO_ENABLE_FAILED:
        default:
            output->ControlWord = CIA402_CW_DISABLE_VOLTAGE;
            return -1;
    }

    return 0;
}

/** @brief 检查单轴PDO并更新原日志快照；wkc为本周期值，返回1正常、0异常。 */
static int ethercat_master_pdo_process_check(int wkc) {
    int const pdo_ok = s_expected_wkc > 0 && wkc == s_expected_wkc &&
                       ec_slave[ETHERCAT_SLAVE].state == EC_STATE_OPERATIONAL;
    taskENTER_CRITICAL();
    s_pdo_monitor.cycle_count++;
    s_pdo_monitor.wkc = wkc;
    s_pdo_monitor.expected_wkc = s_expected_wkc;
    s_pdo_monitor.pdo_ok = pdo_ok;
    s_pdo_monitor.state = ec_slave[ETHERCAT_SLAVE].state;
    if (pdo_ok) {
        int64_t const delta = s_has_last_sample ? (int64_t) input1s->CurrentPosition - s_last_position : 0;
        s_pdo_monitor.position_delta = delta > INT32_MAX ? INT32_MAX : delta < INT32_MIN ? INT32_MIN : (int32_t) delta;
        s_pdo_monitor.good_count++;
        s_pdo_monitor.unchanged_count = s_has_last_sample && input1s->CurrentPosition == s_last_position &&
                                        input1s->StatusWord == s_last_status_word && input1s->OpModeNow == s_last_mode ?
                                        s_pdo_monitor.unchanged_count + 1U : 0U;
        s_pdo_monitor.status_word = input1s->StatusWord;
        s_pdo_monitor.position = input1s->CurrentPosition;
        s_pdo_monitor.mode = input1s->OpModeNow;
        s_pdo_monitor.control_word = output1s->ControlWord;
        s_pdo_monitor.target_pos = output1s->TargetPos;
        s_last_position = input1s->CurrentPosition;
        s_last_status_word = input1s->StatusWord;
        s_last_mode = input1s->OpModeNow;
        s_has_last_sample = 1U;
    } else {
        s_pdo_monitor.bad_count++;
    }
    taskEXIT_CRITICAL();
    return pdo_ok;
}

/* 用途、参数及返回值见头文件；沿用例子接口，标志与PDO任务通过短临界区同步。 */
uint8_t servo_enable_allowed(void) {
    uint8_t result = 2U;
    ethercat_motion_status_t status;
    taskENTER_CRITICAL();
    ethercat_motion_status_get(&status);
    if (s_master_op && s_pdo_monitor.pdo_ok && input1s != NULL && output1s != NULL) {
        uint16 const state = input1s->StatusWord & CIA402_SW_MASK;
        if (state == CIA402_SW_READY_TO_SWITCH_ON && s_servo_enable_request == 0U) {
            /* 沿用例子：先清旧轨迹再置标志，随后提交的新运动命令不会被主任务清掉。 */
            (void) ethercat_motion_position_sync();
            s_servo_enable_request = 1U;
        } else if (state == CIA402_SW_OPERATION_ENABLED && s_servo_enable_request == 1U &&
                   status.busy == 0U && status.done != 0U && get_motion_request_pending() == 0U) {
            s_servo_enable_request = 0U;
        }
        result = s_servo_enable_request;
    }
    taskEXIT_CRITICAL();
    return result;
}
