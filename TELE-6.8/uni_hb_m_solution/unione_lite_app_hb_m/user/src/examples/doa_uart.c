// doa_uart.c
#include "doa_uart.h"
#include "user_uart.h"
#include "uni_iot.h"  
#include "user_gpio.h"   
#include <string.h>
#include <stdlib.h>
#include "irqn.h"
#include "user_config.h"       // 获取波特率等宏
#include "uni_hal_uart.h"      // 直接调用 HAL
#include "uni_uart.h"          // 获取 HAL 回调函数指针

#define TAG "doa_uart"

// ============ 配置宏 ============
#define RX_BUFFER_SIZE      128     // 接收缓冲区大小
#define DOA_ANGLE_MIN       0       // 最小角度
#define DOA_ANGLE_MAX       360     // 最大角度

// 二进制帧格式定义
#define DOA_FRAME_HEAD1     0x55    // 帧头1
#define DOA_FRAME_HEAD2     0xaa    // 帧头2
#define DOA_FRAME_TYPE      0x01    // 帧类型
#define DOA_FRAME_TAIL      0xed    // 帧尾

// ============ 全局变量 ============
static volatile int16_t g_latest_angle = -1;        // 最新角度
static volatile uint64_t g_last_time = 0;           // 最后更新时间
static volatile uint8_t g_data_valid = 0;           // 数据有效标志
static volatile uint8_t g_waiting_for_data = 0;     // 是否正在等待数据

// ============ 静态保存硬件配置（用于唤醒重配）============
static UART_PORT_T s_doa_port = UART_PORT1;   // 根据实际设备号
static uint32_t s_baud_rate = USER_UART_BAUD_RATE;
static uint32_t s_data_bit = USER_UART_DATA_BIT;
static uint32_t s_parity = USER_UART_PARITY;
static uint32_t s_stop = USER_UART_STOP_BIT;
static uint32_t s_mode = USER_UART_PIN_SELECT;  // 可能需转换，但直接使用
static void (*s_hal_rx_cb)(UART_PORT_T port) = NULL;  // HAL 接收回调

// 接收缓冲区（用于拼接多包数据）
static uint8_t g_rx_buffer[RX_BUFFER_SIZE];
static uint8_t g_rx_index = 0;

// 状态机状态
typedef enum {
    STATE_WAIT_HEAD1,   // 等待 0x55
    STATE_WAIT_HEAD2,   // 等待 0xaa
    STATE_WAIT_TYPE,    // 等待 0x01
    STATE_WAIT_HIGH,    // 等待角度高字节
    STATE_WAIT_LOW,     // 等待角度低字节
    STATE_WAIT_TAIL     // 等待 0xed
} frame_state_t;

static frame_state_t g_frame_state = STATE_WAIT_HEAD1;

// ============ 私有函数声明 ============
static void process_binary_frame(uint8_t high_byte, uint8_t low_byte);

// ============ GPIO 控制 ============
static void _gpio_init(void)
{
    user_gpio_set_value(DOA_TRIGGER_GPIO_PIN, 0);
    LOGT(TAG, "DOA trigger GPIO A26 initialized, default low");
}

int doa_trigger_start(void)
{
    user_gpio_set_value(DOA_TRIGGER_GPIO_PIN, DOA_TRIGGER_ACTIVE_LEVEL);
    g_waiting_for_data = 1;
    LOGT(TAG, "DOA trigger started (GPIO A26 high)");
    return 0;
}

int doa_trigger_stop(void)
{
    user_gpio_set_value(DOA_TRIGGER_GPIO_PIN, 0);
    LOGT(TAG, "DOA trigger stopped (GPIO A26 low)");
    return 0;
}

// ============ 处理二进制帧 ============
static void process_binary_frame(uint8_t high_byte, uint8_t low_byte)
{
    int16_t angle = (high_byte << 8) | low_byte;
    
    // 验证通过才停止DOA
    if (angle >= DOA_ANGLE_MIN && angle <= DOA_ANGLE_MAX) {
        if (g_waiting_for_data) {
            doa_trigger_stop();
            g_waiting_for_data = 0;
        }
        
        GIE_DISABLE();
        g_latest_angle = angle;
        g_last_time = uni_get_clock_time_ms();
        g_data_valid = 1;
        GIE_ENABLE();
        
        LOGT(TAG, "Valid angle: %d, DOA stopped", angle);
    } else {
        // 无效数据，不停止DOA，继续等待
        LOGW(TAG, "Invalid angle: %d, waiting for next frame", angle);
    }
    
    // 无论数据有效还是无效，都要重置状态机，准备接收下一帧
    g_frame_state = STATE_WAIT_HEAD1;
}

// ============ UART 接收回调（中断上下文）============
static void _doa_uart_recv(char *buf, int len)
{
    uint8_t *data = (uint8_t*)buf;
    int i;
    for (i = 0; i < len; i++) {
        uint8_t byte = data[i];
        
        // 状态机解析二进制帧
        switch (g_frame_state) {
            case STATE_WAIT_HEAD1:
                if (byte == DOA_FRAME_HEAD1) {
                    g_frame_state = STATE_WAIT_HEAD2;
                }
                break;
                
            case STATE_WAIT_HEAD2:
                if (byte == DOA_FRAME_HEAD2) {
                    g_frame_state = STATE_WAIT_TYPE;
                } else if (byte == DOA_FRAME_HEAD1) {
                    // 仍然是头1，保持状态
                    g_frame_state = STATE_WAIT_HEAD2;
                } else {
                    g_frame_state = STATE_WAIT_HEAD1;
                }
                break;
                
            case STATE_WAIT_TYPE:
                if (byte == DOA_FRAME_TYPE) {
                    g_frame_state = STATE_WAIT_HIGH;
                } else if (byte == DOA_FRAME_HEAD1) {
                    g_frame_state = STATE_WAIT_HEAD2;
                } else {
                    g_frame_state = STATE_WAIT_HEAD1;
                }
                break;
                
            case STATE_WAIT_HIGH:
                g_rx_buffer[0] = byte;  // 临时存储高字节
                g_frame_state = STATE_WAIT_LOW;
                break;
                
            case STATE_WAIT_LOW:
                g_rx_buffer[1] = byte;  // 临时存储低字节
                g_frame_state = STATE_WAIT_TAIL;
                break;
                
            case STATE_WAIT_TAIL:
                if (byte == DOA_FRAME_TAIL) {
                    // 接收到完整帧，处理角度数据
                    process_binary_frame(g_rx_buffer[0], g_rx_buffer[1]);
                }
                // 无论是否匹配，都回到初始状态
                g_frame_state = STATE_WAIT_HEAD1;
                break;
                
            default:
                g_frame_state = STATE_WAIT_HEAD1;
                break;
        }
    }
}

// ============ 等待角度数据（带超时）============
int doa_wait_for_angle(uint32_t timeout_ms)
{
    uint64_t start_time = uni_get_clock_time_ms();
    
    // 清空旧数据
    doa_reset_data();
    g_waiting_for_data = 1;
    
    // 触发DOA芯片（拉高GPIO A26）
    doa_trigger_start();
    
    // 等待数据到达
    while ((uni_get_clock_time_ms() - start_time) < timeout_ms) {
        if (doa_is_data_valid()) {
            LOGT(TAG, "Angle received within %d ms", 
                 (uint32_t)(uni_get_clock_time_ms() - start_time));
            return 0;  // 成功
        }
        uni_msleep(1);
    }
    
    // 超时，停止等待并拉低GPIO A26
    LOGE(TAG, "Wait for angle timeout after %d ms", timeout_ms);
    doa_trigger_stop();
    g_waiting_for_data = 0;
    return -1;  // 失败
}

// ============ 对外接口实现 ============

/**
 * @brief 获取最新的 DOA 角度
 */
int16_t doa_get_latest_angle(void)
{
    int16_t angle;
    
    GIE_DISABLE();
    angle = g_latest_angle;
    GIE_ENABLE();
    
    return angle;
}

/**
 * @brief 检查是否有有效数据
 */
uint8_t doa_is_data_valid(void)
{
    uint8_t valid;
    
    GIE_DISABLE();
    valid = g_data_valid;
    GIE_ENABLE();
    
    return valid;
}

uint8_t doa_is_data_fresh(uint32_t timeout_ms)
{
    if (!doa_is_data_valid()) {
        return 0;
    }
    
    uint64_t now = uni_get_clock_time_ms();
    uint64_t last = doa_get_last_time();
    return ((now - last) < timeout_ms);
}

/**
 * @brief 获取最后一次更新角度的时间戳
 */
uint64_t doa_get_last_time(void)
{
    uint32_t time;
    
    GIE_DISABLE();
    time = g_last_time;
    GIE_ENABLE();
    
    return time;
}

/**
 * @brief 重置 DOA 数据
 */
void doa_reset_data(void)
{
    GIE_DISABLE();
    g_latest_angle = -1;
    g_data_valid = 0;
    g_last_time = 0;
    GIE_ENABLE();
    
    // 清空接收缓冲区
    g_rx_index = 0;
    memset(g_rx_buffer, 0, RX_BUFFER_SIZE);
    
    // 重置状态机
    g_frame_state = STATE_WAIT_HEAD1;
    
    LOGT(TAG, "DOA data reset");
}

/**
 * @brief 初始化 DOA UART 接收
 */
int doa_uart_init(void)
{
    int ret;
    
    // 初始化GPIO A26
    _gpio_init();
    
    // 初始化 UART，注册接收回调
    ret = user_uart_init(_doa_uart_recv);
    if (ret != 0) {
        LOGE(TAG, "DOA UART init failed");
        return -1;
    }

      // 获取 HAL 接收回调（用于硬件重配）
    s_hal_rx_cb = (void (*)(UART_PORT_T))uni_uart_get_recv_cb();
    if (s_hal_rx_cb == NULL) {
        LOGE(TAG, "Failed to get HAL rx callback");
        return -1;
    }
    
    // 保存硬件参数（从宏定义获取）
    // 端口号可能需要根据实际配置调整，此处假设为 UART_PORT1
    s_doa_port = UART_PORT1;
    // 其他参数已从宏赋值
    
    // 重置所有数据（包含缓冲区、状态机、全局变量）
    doa_reset_data();
    
    LOGT(TAG, "DOA UART initialized successfully");
    return 0;
}

void doa_uart_reinit_hw(void)
{
    if (s_hal_rx_cb == NULL) {
        LOGE(TAG, "UART not initialized, cannot reinit");
        return;
    }
    
    // 直接调用底层 HAL 重新配置硬件（不触发软件注册）
    uni_hal_uart_init(s_doa_port, s_baud_rate, s_data_bit,
                      s_parity, s_stop, s_mode, s_hal_rx_cb);
    LOGT(TAG, "DOA UART hardware reinitialized");
}
