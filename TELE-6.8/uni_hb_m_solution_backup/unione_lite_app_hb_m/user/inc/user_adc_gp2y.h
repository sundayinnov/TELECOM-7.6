#ifndef __USER_ADC_GP2Y_H__
#define __USER_ADC_GP2Y_H__

// 必须放在最前面，确保 GPIO_NUMBER 被定义
#include "user_gpio.h"
#include "type.h"

#ifdef __cplusplus
extern "C" {
#endif
void adc_init(void);
void adc_get(void);
int user_adc_gp2y_init(GPIO_NUMBER gpio_pin, uint16_t threshold_cm);
int user_adc_gp2y_start(void);
int user_adc_gp2y_read_once(void);
#ifdef __cplusplus
}
#endif

#endif