#include "user_adc_gp2y.h"
#include "user_gpio.h"
#include "uni_hal_adc.h"
#include "FreeRTOS.h"
#include "task.h"
#include <stdio.h>

#define TAG "GP2Y"

static GPIO_NUMBER s_gpio_pin = 0;
static uni_bool    s_initialized = false;
static volatile int s_last_adc_value = -1;
static volatile int s_adc_valid = 0;

#define ADC_CHANNEL_A27   2   // A27 对应的 ADC 通道（已验证可用）

// 后台保活任务：持续采样，保持 ADC 活跃
static void _adc_keepalive_task(void *arg) {
    (void)arg;
    printf("[GP2Y] ADC keepalive task started\n");
    while (1) {
       int raw;
         raw = ADC_SingleModeDataGet(ADC_CHANNEL_A27);
        if (raw >= 0) {
            s_last_adc_value = raw;
            s_adc_valid = 1;
            printf("ADC = %d\n", raw);
        }
        // 采样周期 500ms，可根据需要调整
        vTaskDelay(500 / portTICK_PERIOD_MS);
    }
}

static Result _create_keepalive_task(void) {
    thread_param param;
    uni_pthread_t pid;
    uni_memset(&param, 0, sizeof(param));
    param.stack_size = STACK_SMALL_SIZE;
    param.priority   = OS_PRIORITY_HIGH;
    uni_strncpy(param.task_name, "adc_a27", sizeof(param.task_name) - 1);
    if (0 != uni_pthread_create(&pid, &param, _adc_keepalive_task, NULL)) {
        printf("[GP2Y] create keepalive task failed\n");
        return E_FAILED;
    }
    uni_pthread_detach(pid);
    return E_OK;
}

// ========== 对外接口 ==========
int user_adc_gp2y_init(GPIO_NUMBER gpio_pin, uint16_t threshold_cm) {
    (void)threshold_cm;
    if (gpio_pin < 0 || gpio_pin >= GPIO_NUM_MAX) {
        printf("[GP2Y] Invalid GPIO pin %d\n", gpio_pin);
        return -1;
    }
    s_gpio_pin = gpio_pin;

    // 注意：user_gpio_init() 已经在 main 中调用，其中已包含 SarADC_Init()
    // 这里只需将引脚设置为 ADC 模式
    if (0 != user_gpio_set_mode(s_gpio_pin, GPIO_MODE_ADC)) {
        printf("[GP2Y] Failed to set GPIO %d to ADC mode\n", s_gpio_pin);
        return -1;
    }

    // 启动后台保活任务
    if (E_OK != _create_keepalive_task()) {
        printf("[GP2Y] Failed to start keepalive task\n");
        return -1;
    }

    s_initialized = true;
    printf("[GP2Y] A27 ADC keepalive mode initialized\n");
    // 临时测试：在初始化完成后，无限循环读取 ADC
// while (1) {
//     int raw = user_gpio_get_value(GPIO_NUM_A27);
//     printf("Test ADC = %d\n", raw);
//     uni_sleep(1);
// }
    return 0;
}

// 兼容旧代码，实际不需要再启动额外线程
int user_adc_gp2y_start(void) {
    if (!s_initialized) return -1;
    printf("[GP2Y] Already running (keepalive task active)\n");
    return 0;
}

// 语音回调中只调用此函数，无硬件访问
int user_adc_gp2y_read_once(void) {
    if (!s_initialized) return -1;
    if (!s_adc_valid)    return -1;
    return s_last_adc_value;
}

void adc_init(void)
{
    user_gpio_set_mode(GPIO_NUM_A27, GPIO_MODE_ADC);
}

void adc_get(void)
{
    int16_t raw = 0;
    SarADC_Init();
    user_gpio_set_mode(GPIO_NUM_A27, GPIO_MODE_ADC);
    raw = user_gpio_get_value(GPIO_NUM_A27);
    if (raw >= 0) {
        printf("adc_task = %d\n", raw);
    }
}
