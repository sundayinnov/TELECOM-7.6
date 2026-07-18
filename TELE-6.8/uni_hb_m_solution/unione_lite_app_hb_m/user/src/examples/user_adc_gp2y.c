#include "user_adc_gp2y.h"
#include "user_gpio.h"
#include "uni_hal_adc.h"
#include "FreeRTOS.h"
#include "task.h"
#include <stdio.h>

#define TAG "GP2Y"
#define DBG(fmt, ...) ((void)0)
// ============ 静态变量 ============
static volatile bool s_hw_initialized = false;

// ADC状态机变量
static volatile uint16_t g_adc_raw_value = 0;        
static volatile uint16_t g_adc_filtered_value = 0;   
static volatile uint16_t g_adc_report_value = 0;     
static volatile uint16_t g_adc_threshold = ADC_DEFAULT_THRESHOLD;
static volatile adc_state_t g_adc_state = ADC_STATE_NORMAL;
//static volatile uint32_t g_adc_trigger_start_time = 0;
//static volatile uint32_t g_adc_last_report_time = 0;
//static volatile bool g_adc_trigger_reported = false;
//static volatile uint32_t g_adc_trigger_hold_time = 0;
static volatile bool g_adc_enabled = false;
static volatile bool g_filter_enabled = false;        // 滤波开关，默认关闭

// 滤波缓冲区（用于移动平均滤波）
static uint16_t g_filter_buffer[ADC_FILTER_SAMPLES] = {0};
static uint8_t g_filter_index = 0;
static uint8_t g_filter_count = 0;

// ============ 内部函数声明 ============
static void _adc_reset_filter_buffer(void);

// ============ ADC 硬件控制函数 ============
/**
 * @brief 初始化ADC硬件
 */
void adc_init(void)
{
    if (s_hw_initialized) {
        DBG("[GP2Y] ADC already initialized\n");
        return;
    }
    
    //printf("[GP2Y] ADC hardware initializing...\n");
    
    // 1. 先初始化ADC硬件
    SarADC_Init();
    
    // 2. 等待ADC稳定
    uni_msleep(10);
    
    // 3. 设置GPIO为ADC模式
    if (0 != user_gpio_set_mode(GPIO_NUM_A27, GPIO_MODE_ADC)) {
     //   printf("[GP2Y] Failed to set GPIO A27 to ADC mode\n");
        // 如果GPIO设置失败，应该反初始化ADC硬件
        // 但由于没有对应的反初始化函数，这里只打印错误
        return;
    }
    
    // 4. 进行一次空读，确保ADC就绪（使用 user_gpio_get_value）
    int dummy = user_gpio_get_value(GPIO_NUM_A27);
    (void)dummy;
    
    // 5. 标记为已初始化
    s_hw_initialized = true;
    g_adc_enabled = true;
    
    // 6. 重置状态机
    g_adc_state = ADC_STATE_NORMAL;
    //g_adc_trigger_reported = false;
    //g_adc_trigger_hold_time = 0;
    g_adc_report_value = 0;
    
    // 7. 重置滤波缓冲区
    _adc_reset_filter_buffer();
    
    //printf("[GP2Y] ADC hardware initialized successfully, threshold=%d\n", g_adc_threshold);
}

/**
 * @brief 反初始化ADC硬件
 */
void adc_deinit(void)
{
    if (!s_hw_initialized) {
     //   DBG("[GP2Y] ADC not initialized, skip deinit\n");
        return;
    }
    
   // printf("[GP2Y] ADC hardware deinitializing...\n");
    
    // 1. 停止ADC采样（标记禁用）
    g_adc_enabled = false;
    
   // 2. 将A27设置为输入模式（关键！不能设置为输出）
    // 传感器需要驱动这个引脚，设置为输入不会产生冲突
    if (0 != user_gpio_set_mode(GPIO_NUM_A27, GPIO_MODE_IN)) {
    //    printf("[GP2Y] Failed to set GPIO A27 to INPUT mode\n");
    }
    
    // 3. 可选：配置上拉电阻，避免浮空
    // 根据传感器类型选择 GPIO_PULL_UP 或 GPIO_PULL_DOWN
    user_gpio_set_pull_mode(GPIO_NUM_A27, GPIO_PULL_UP);
    
    // 4. 标记为未初始化
    s_hw_initialized = false;
    
    // 5. 重置状态
    g_adc_state = ADC_STATE_NORMAL;
    //g_adc_trigger_reported = false;
    //g_adc_trigger_hold_time = 0;
    g_adc_report_value = 0;
    g_adc_raw_value = 0;
    g_adc_filtered_value = 0;
    
    // 6. 重置滤波缓冲区
    _adc_reset_filter_buffer();
    
   // printf("[GP2Y] ADC hardware deinitialized, A27 set LOW\n");
}

/**
 * @brief 重置滤波缓冲区
 */
static void _adc_reset_filter_buffer(void)
{
    int i;
    for ( i = 0; i < ADC_FILTER_SAMPLES; i++) {
        g_filter_buffer[i] = 0;
    }
    g_filter_index = 0;
    g_filter_count = 0;
}


/**
 * @brief 读取ADC原始值
 * @return 原始ADC值（0-4095），失败返回-1
 */
int adc_get(void)
{
    if (!s_hw_initialized || !g_adc_enabled) {
        printf("[GP2Y] ADC not ready for sampling\n");
        return -1;
    }
    
    // 使用 user_gpio_get_value 读取ADC值
    int raw = user_gpio_get_value(GPIO_NUM_A27);
    if (raw >= 0) {
        g_adc_raw_value = (uint16_t)raw;
        return raw;
    } else {
        printf("[GP2Y] ADC read failed: %d\n", raw);
        return -1;
    }
}

// ============ 滤波函数（预留，暂未使用） ============
static uint16_t _adc_apply_filter(uint16_t raw_value)
{
    if (!g_filter_enabled) {
        // 滤波关闭，直接返回原始值
        return raw_value;
    }
    
    // 如果滤波启用，执行移动平均滤波
    g_filter_buffer[g_filter_index++] = raw_value;
    if (g_filter_index >= ADC_FILTER_SAMPLES) {
        g_filter_index = 0;
    }
    if (g_filter_count < ADC_FILTER_SAMPLES) {
        g_filter_count++;
    }
    
    uint32_t sum = 0;
    int i;
    for ( i = 0; i < g_filter_count; i++) {
        sum += g_filter_buffer[i];
    }
    return (uint16_t)(sum / g_filter_count);
}

// ============ 状态机处理函数 ============

void adc_process_sample(void)
{
    if (!s_hw_initialized || !g_adc_enabled) {
        return;
    }
    
    // 读取ADC原始值
    int raw = adc_get();
    if (raw < 0) {
        return;
    }
    g_adc_raw_value = (uint16_t)raw;
    
    // 应用滤波（当前关闭，直接返回原始值）
    uint16_t processed_value = _adc_apply_filter((uint16_t)raw);
    g_adc_filtered_value = processed_value;  // 保存处理后的值
    
   // uint32_t now = uni_get_clock_time_ms();
    
    // 状态机处理
    switch (g_adc_state) {
        case ADC_STATE_NORMAL:
            // 检查是否触发（值超过阈值）
            if (processed_value > g_adc_threshold) {
                // 进入触发状态
                g_adc_state = ADC_STATE_TRIGGERED;
            //    g_adc_trigger_start_time = now;
            //    g_adc_trigger_hold_time = 0;
                g_adc_report_value = processed_value;
                
             //   printf("[GP2Y] TRIGGERED! raw=%d, value=%d > threshold=%d\n", 
             //          raw, processed_value, g_adc_threshold);
                
                // 触发时立即上报（由外部通过状态查询实现）
            }
            break;
            
        case ADC_STATE_TRIGGERED:
            // 计算触发持续时间
         //   g_adc_trigger_hold_time = now - g_adc_trigger_start_time;
            g_adc_report_value = processed_value;
            
            // 检查是否回到正常（值低于阈值）
            if (processed_value <= g_adc_threshold) {
                // 恢复正常状态
                g_adc_state = ADC_STATE_NORMAL;
                g_adc_report_value = processed_value;
                
            //    printf("[GP2Y] NORMAL: raw=%d, value=%d <= threshold=%d, hold_time=%dms\n", 
            //           raw, processed_value, g_adc_threshold, g_adc_trigger_hold_time);
                
            //    g_adc_trigger_hold_time = 0;
            }
            // 触发期间不额外打印，由外部定时上报
            break;
            
        default:
            break;
    }
}

// ============ 对外接口 ============

bool adc_is_initialized(void)
{
    return s_hw_initialized;
}

void adc_set_threshold(uint16_t threshold)
{
    if (threshold > 0 && threshold < 4096) {
        g_adc_threshold = threshold;
    //    printf("[GP2Y] ADC threshold updated to %d\n", threshold);
    } else {
     //   printf("[GP2Y] Invalid threshold value: %d (must be 1-4095)\n", threshold);
    }
}

uint16_t adc_get_threshold(void)
{
    return g_adc_threshold;
}

void adc_reset_state(void)
{
    g_adc_state = ADC_STATE_NORMAL;
 //   g_adc_trigger_start_time = 0;
 //   g_adc_trigger_hold_time = 0;
    g_adc_report_value = 0;
    _adc_reset_filter_buffer();
 //   printf("[GP2Y] ADC state reset to NORMAL\n");
}

adc_state_t adc_get_state(void)
{
    return g_adc_state;
}

int adc_get_filtered_value(void)
{
    if (!s_hw_initialized) {
        return -1;
    }
    return g_adc_filtered_value;
}

bool adc_is_triggered(void)
{
    return (g_adc_state == ADC_STATE_TRIGGERED);
}

// uint32_t adc_get_trigger_hold_time(void)
// {
//     if (g_adc_state != ADC_STATE_TRIGGERED) {
//         return 0;
//     }
//     return g_adc_trigger_hold_time;
// }

int adc_get_report_value(void)
{
    if (!s_hw_initialized) {
        return -1;
    }
    return g_adc_report_value;
}

void adc_set_filter_enable(bool enable)
{
    g_filter_enabled = enable;
    if (!enable) {
        _adc_reset_filter_buffer();
    }
  //  printf("[GP2Y] Filter %s\n", enable ? "ENABLED" : "DISABLED");
}




