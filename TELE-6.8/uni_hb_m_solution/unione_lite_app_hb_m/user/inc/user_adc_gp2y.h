#ifndef __USER_ADC_GP2Y_H__
#define __USER_ADC_GP2Y_H__

// 必须放在最前面，确保 GPIO_NUMBER 被定义
#include "user_gpio.h"
#include "type.h"

#ifdef __cplusplus
extern "C" {
#endif
// ADC 通道定义
#define ADC_CHANNEL_GP2Y     GPIO_NUM_A27   // A27 对应的 ADC 通道

// ADC 默认阈值
#define ADC_DEFAULT_THRESHOLD   1200

// ADC 滤波采样数量
#define ADC_FILTER_SAMPLES      5

// ADC 上报间隔（毫秒）
//#define ADC_REPORT_INTERVAL_MS  200

// ADC 状态机定义
typedef enum {
    ADC_STATE_NORMAL = 0,      // 正常状态（值低于阈值）
    ADC_STATE_TRIGGERED = 1,   // 触发状态（值高于阈值）
    ADC_STATE_RECOVERING = 2   // 恢复中
} adc_state_t;

/**
 * @brief 初始化ADC硬件
 * @note 将A27设置为ADC模式，并初始化ADC模块
 */
void adc_init(void);

/**
 * @brief 反初始化ADC硬件
 * @note 将A27设置为输出模式并置低电平，释放ADC资源
 */
void adc_deinit(void);

/**
 * @brief 读取ADC原始值
 * @return 原始ADC值（0-4095），失败返回-1
 */
int adc_get(void);

/**
 * @brief 获取ADC硬件初始化状态
 * @return true已初始化，false未初始化
 */
bool adc_is_initialized(void);

/**
 * @brief 设置ADC阈值
 * @param threshold 新的阈值（0-4095）
 */
void adc_set_threshold(uint16_t threshold);

/**
 * @brief 获取当前ADC阈值
 * @return 当前阈值
 */
uint16_t adc_get_threshold(void);

/**
 * @brief 重置ADC状态机
 * @note 将状态机重置为正常状态，清除触发标志
 */
void adc_reset_state(void);

/**
 * @brief 获取当前ADC状态
 * @return ADC状态（ADC_STATE_NORMAL 或 ADC_STATE_TRIGGERED）
 */
adc_state_t adc_get_state(void);

/**
 * @brief 获取滤波后的ADC值
 * @return 滤波后的ADC值
 */
int adc_get_filtered_value(void);

/**
 * @brief 获取ADC触发状态
 * @return true已触发，false未触发
 */
bool adc_is_triggered(void);

/**
 * @brief 获取ADC触发持续时间（毫秒）
 * @return 触发持续时间，未触发返回0
 */
//uint32_t adc_get_trigger_hold_time(void);

/**
 * @brief 获取上报用的ADC值
 * @return 上报用的ADC值
 */
int adc_get_report_value(void);

/**
 * @brief 处理ADC采样和状态机
 * @note 此函数应在主循环中周期性调用（建议50ms）
 */
void adc_process_sample(void);
#ifdef __cplusplus
}
#endif

#endif