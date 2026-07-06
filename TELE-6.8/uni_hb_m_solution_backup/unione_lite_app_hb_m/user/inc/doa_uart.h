// doa_uart.h
#ifndef __DOA_UART_H__
#define __DOA_UART_H__

#include "uni_types.h"


// 使用 A26 作为 DOA 触发引脚
#define DOA_TRIGGER_GPIO_PIN    GPIO_NUM_A26
#define DOA_TRIGGER_ACTIVE_LEVEL 1   // 高电平有效

// 触发DOA芯片获取角度（拉高GPIO）
int doa_trigger_start(void);

// 停止DOA芯片（拉低GPIO）
int doa_trigger_stop(void);

/**
 * @brief 获取最新的 DOA 角度
 * @return 角度值（0-360），-1 表示无效
 */
int16_t doa_get_latest_angle(void);

/**
 * @brief 检查是否有有效数据
 * @return 1: 有效，0: 无效
 */
uint8_t doa_is_data_valid(void);

// 检查数据是否新鲜（ms内）
uint8_t doa_is_data_fresh(uint32_t timeout_ms);

/**
 * @brief 获取最后一次更新角度的时间戳（毫秒）
 * @return 时间戳
 */
uint64_t doa_get_last_time(void);

/**
 * @brief 重置 DOA 数据
 */
void doa_reset_data(void);

// 等待接收角度数据（带超时）
int doa_wait_for_angle(uint32_t timeout_ms);

void doa_uart_reinit_hw(void);   // 新增：仅重新配置硬件

/**
 * @brief 初始化 DOA UART 接收
 * @return 0: 成功，-1: 失败
 */
int doa_uart_init(void);

#endif /* __DOA_UART_H__ */
