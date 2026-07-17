#include "user_config.h"
#include "user_event.h"
#include "user_gpio.h"
#include "user_player.h"
#include "user_pwm.h"
#include "user_timer.h"
#include "user_uart.h"
#include "doa_uart.h"
#include "uni_setting_session.h"
#include "uni_pcm_default.h"
#include "uni_hal_watchdog.h"
#include "user_sw_timer.h"
#include "uni_black_board.h"
#include "uni_media_player.h"
#include "user_flash.h"
#include "user_asr.h"
#include "uni_iot.h"
#include "uni_recog_service.h"
//#include "uni_vui_interface.h"   
#include "queue.h"      // FreeRTOS 队列头文件
#include "user_adc_gp2y.h"
#include "uni_hal_power.h"


#define TAG "auto_gpio"
#define DBG(...) ((void)0)



// ============ TTS 命令映射表 ============
typedef struct {
    uint8_t cmd;
    const char *reply_files;
} tts_mapping_t;

static const tts_mapping_t g_tts_mapping[] = {
    {0x01, "[500]"}, {0x02, "[501]"}, {0x03, "[502]"}, {0x04, "[503]"},
    {0x05, "[504]"}, {0x06, "[505]"}, {0x07, "[506]"}, {0x08, "[507]"},
    {0x09, "[508]"}, {0x0A, "[509]"}, {0x0B, "[510]"}, {0x0C, "[511]"},
    {0x0D, "[512]"}, {0x0E, "[513]"}, {0x0F, "[514]"}, {0x10, "[515]"}, 
    {0x11, "[516]"}, {0x00, "[0]"}
};

// ============ CRC 校验相关 ============
#define CRC_CMD_CODE        0xF0                 // CRC校验命令码
#define CRC_MODE_QUERY      0x00                 // 查询CRC校验值
#define CRC_VALUE_LOW       0x41                 // CRC低字节
#define CRC_VALUE_HIGH      0xC2                 // CRC高字节

// ============ 功耗控制相关（上位机休眠/唤醒）============
#define POWER_CMD_CODE        0xD0
#define POWER_MODE_ENTER      0x01     // 上位机请求进入休眠
#define POWER_MODE_EXIT       0x02     // 上位机请求退出休眠（唤醒确认）
#define WAKEUP_SEQ_LEN        4
#define WAKEUP_SEQ_BYTE       0xCC

static uni_sem_t g_wakeup_ack_sem = NULL;
static bool g_host_sleeping = false;
// ============ 聆听控制相关 ============
#define LISTEN_CMD_CODE      0xC0
#define LISTEN_MODE_END      0x01
// ============ 音量控制相关 ============
#define VOLUME_CMD_CODE      0xB0
#define VOLUME_MODE_QUERY    0x00
#define VOLUME_MODE_SET      0x01
 
#define FLASH_KEY_VOLUME   "user_volume"
#define DEFAULT_VOLUME     50
#define FLASH_KEY_MUTE      "user_mute"
// ============ LED 配置 ============
#define LED_RED_PIN             GPIO_NUM_B3          // 蓝灯引脚
#define LED_BLUE_PIN            GPIO_NUM_B2          // 红灯引脚
#define LED_CMD_CODE            0xA0                 // LED控制命令码
#define LED_MODE_OFF            0x00
#define LED_MODE_ON             0x01
#define LED_MODE_BLINK          0x02
#define LED_MODE_END            0x03
#define LED_COLOR_RED           0x00
#define LED_COLOR_BLUE          0x01

// ============ 软件虚拟 UART 配置 ============
#define SOFT_UART_RX_PIN        GPIO_NUM_A25      
#define SOFT_UART_BAUDRATE      9600     
#define HALF_BIT_TIME_US        52   // 核心：半个位宽（52us）作为心跳节拍
#define FRAME_TIMEOUT_US        5000

// 定时器配置
#define TIMER_SAMPLING          eTIMER5             // 位采样定时器
#define TIMER_TIMEOUT           eTIMER6             // 断帧超时定时器

// TTS 帧格式
#define TTS_FRAME_LEN           9
#define TTS_HEAD1               0xAA
#define TTS_HEAD2               0x55
#define TTS_TAIL1               0x55
#define TTS_TAIL2               0xAA

// LED 控制结构
typedef struct {
    uint8_t pin;
    uint8_t mode;
    uint8_t count;
    uint8_t state;
    bool is_active;              
    bool timer_initialized;      
    timer_handle_t timer;
    uint32_t blink_interval_ms;// DURATION
} led_ctrl_t;

// 接收状态
typedef enum {
    RX_STATE_IDLE,
    RX_STATE_RECEIVING
} rx_status_t;

// ============ 全局变量 ============
static led_ctrl_t g_red_led = {
    .pin = LED_RED_PIN,
    .mode = LED_MODE_OFF,
    .count = 0,
    .state = 0,
    .is_active = false,
    .timer_initialized = false,
    .timer = INVALID_TIMER_HANDLE,
    .blink_interval_ms = 0 
};

static led_ctrl_t g_blue_led = {
    .pin = LED_BLUE_PIN,
    .mode = LED_MODE_OFF,
    .count = 0,
    .state = 0,
    .is_active = false,
    .timer_initialized = false,
    .timer = INVALID_TIMER_HANDLE,
    .blink_interval_ms = 0 
};


static volatile uint8_t g_tick_count = 0;
static volatile rx_status_t g_rx_status = RX_STATE_IDLE;
static volatile uint8_t g_current_byte = 0;
static volatile uint8_t g_bit_index = 0;
static volatile uint8_t g_rx_buffer[TTS_FRAME_LEN];
static volatile uint16_t g_rx_len = 0;
static volatile bool g_rx_flag;

static uint8_t g_b1_power_state = 0;

static bool g_sleep_by_command = false;
  
static QueueHandle_t uart_tx_queue = NULL;

//volatile bool g_adc_enabled = true;


// ============ 函数声明 ============
static const char* get_reply_files_by_cmd(uint8_t tts_cmd);
static void play_tts_by_cmd(uint8_t tts_cmd);
static void send_volume_response(uint8_t mode, uint8_t volume, uint8_t flag, uint8_t range);
static void led_set(led_ctrl_t *led, uint8_t on);
static void led_off(led_ctrl_t *led);
static void led_on(led_ctrl_t *led);
static void led_blink_cb(timer_handle_t timer);
static void led_blink(led_ctrl_t *led, uint8_t count, uint8_t blink_mode);
static led_ctrl_t* get_led_by_color(uint8_t color);
static void send_led_feedback(uint8_t color, uint8_t mode);
static void process_led_command(uint8_t *frame);
static void process_volume_command(uint8_t *frame);
static void timeout_handler(eTIMER_IDX idx);
static void bit_sampling_handler(eTIMER_IDX idx);
static void gpio_intr_handler(GPIO_NUMBER num, uni_bool is_high);
static void tts_handler_task(void *args);
static void soft_uart_init(void);
static void soft_uart_hw_init(void);
static void led_init(void);
static void send_command_with_angle(uint8_t cmd_code, int16_t angle);
static void process_listen_command(uint8_t *frame);
static void _custom_setting_cb(USER_EVENT_TYPE event, user_event_context_t *context);
static void _goto_awakened_cb(USER_EVENT_TYPE event, user_event_context_t *context);
static void _goto_sleeping_cb (USER_EVENT_TYPE event, user_event_context_t *context);
static void _study_event_cb(USER_EVENT_TYPE event, user_event_context_t *context);
static void _register_event_callback(void);
static void process_power_command(uint8_t *frame);
void uart_send_safe(const char* buf, int len);
static void _wakeup_cb(int flag);
static void enter_deep_sleep_with_wakeup(void);
static void deep_sleep_restore(void);
static void process_crc_command(uint8_t *frame);

extern volatile int16_t g_last_doa_angle;  


// ============ 深度睡眠唤醒回调（中断上下文，尽量简单）============
static void _wakeup_cb(int flag) {
    // 该回调在中断上下文执行，不建议使用 LOGT（可能阻塞）
    // 可置标志或空实现，恢复工作放在 deep_sleep_restore 中
    // 此处仅做简单记录（若需调试，可使用 uni_printf）
    uni_hal_watchdog_feed();
    DBG("Woke up, flag=%d\n", flag);
}

// 保存静音状态到 Flash
void save_mute_to_flash(bool mute) {
    uint8_t mute_val = mute ? 1 : 0;
    user_flash_set_env_blob(FLASH_KEY_MUTE, &mute_val, sizeof(mute_val));
}

// 从 Flash 读取静音状态，并设置到 MediaPlayer
void restore_mute_from_flash(void) {
    uint8_t saved_mute = 0;  // 默认非静音
    int save_len = 0;
    if (user_flash_get_env_blob(FLASH_KEY_MUTE, &saved_mute, sizeof(saved_mute), &save_len) == sizeof(saved_mute)) {
        MediaPlayerSetMute(saved_mute == 1);
        LOGT(TAG, "Restored mute state from flash: %d", saved_mute);
    } else {
        MediaPlayerSetMute(false);  // 默认非静音
    }
}

// 保存音量到 Flash（立即写入）
void save_volume_to_flash(int volume) {
    user_flash_set_env_blob(FLASH_KEY_VOLUME, &volume, sizeof(volume));
}

void restore_audio_settings(void) {
    // 恢复静音状态
    restore_mute_from_flash();
    // 恢复音量（注意：恢复音量时可能会覆盖静音状态？不影响，因为音量设置不会改变静音标志）
    int saved_vol = DEFAULT_VOLUME;
    int save_len = 0;
    if (user_flash_get_env_blob(FLASH_KEY_VOLUME, &saved_vol, sizeof(saved_vol), &save_len) == sizeof(saved_vol)) {
        if (saved_vol >= MEDIA_VOLUME_MIN && saved_vol <= MEDIA_VOLUME_MAX) {
            MediaPlayerVolumeSet(saved_vol);
            LOGT(TAG, "Restored volume from flash: %d", saved_vol);
            return;
        }
    }
    MediaPlayerVolumeSet(DEFAULT_VOLUME);
    LOGT(TAG, "Use default volume: %d", DEFAULT_VOLUME);
}

// ============ CRC 校验响应函数 ============
static void process_crc_command(uint8_t *frame)
{
    // 直接构建CRC响应帧 (9字节)，回复固定的CRC值
    uint8_t response[9] = {
        0xAA, 0x55, CRC_CMD_CODE,    // 帧头 + 命令码
        0x00, 
        0x00,                       // 模式（固定为0x00）
        CRC_VALUE_HIGH,              // CRC高字节 (0x04)
        CRC_VALUE_LOW,               // CRC低字节 (0xF5)
        0x55, 0xAA                   // 帧尾
    };
    
    // 发送响应
    uart_send_safe((char*)response, 9);
    LOGT(TAG, "CRC response sent: 0x%02X 0x%02X", CRC_VALUE_HIGH, CRC_VALUE_LOW);
}

static void process_listen_command(uint8_t *frame)
{
    uint8_t mode = frame[3];
    if (mode == LISTEN_MODE_END) {
        g_sleep_by_command = true;               // 设置标志
        user_asr_goto_sleep();                   // 调用系统休眠接口
        LOGT(TAG, "Enter sleep by command 0xC0, mode=0x01");
    } else {
        LOGW(TAG, "Unknown listen mode: %d", mode);
    }
}

// ============ TTS 辅助函数 ============
static const char* get_reply_files_by_cmd(uint8_t tts_cmd)
{    
    int i;
    for (i = 0; g_tts_mapping[i].reply_files != NULL; i++) {
        if (g_tts_mapping[i].cmd == tts_cmd) {
            return g_tts_mapping[i].reply_files;
        }
    }
    return NULL;
}

static void play_tts_by_cmd(uint8_t tts_cmd)
{
    const char *reply_files = get_reply_files_by_cmd(tts_cmd);
    if (reply_files != NULL) {
        user_player_reply_list_random(reply_files);
        LOGT(TAG,"Play TTS for cmd 0x%02X\n", tts_cmd);
    } else {
        LOGE(TAG,"No reply files for TTS cmd: 0x%02X\n", tts_cmd);
    }
}

// 安全发送：将数据放入队列（不阻塞，关中断保护）
void uart_send_safe(const char* buf, int len) {
    if (len != 9) return;           // 协议固定9字节，可调整
    if (uart_tx_queue == NULL) return;

    // 复制数据到局部数组（防止 buf 被修改）
    uint8_t data[9];
    memcpy(data, buf, 9);

    // 关中断入队（极短时间，不影响其他中断）
    GIE_DISABLE();
    BaseType_t ret = xQueueSendToBack(uart_tx_queue, data, 0);  // 0 不等待
    GIE_ENABLE();

    if (ret != pdPASS) {
        LOGW(TAG, "UART queue full, drop packet");
    }
}

static void process_power_command(uint8_t *frame)
{
    uint8_t mode = frame[3];
    LOGT(TAG, "Power command: mode=0x%02X", mode);
    
    switch (mode) {
        case POWER_MODE_ENTER:   // 上位机请求进入休眠
            if (!g_host_sleeping) {
                g_host_sleeping = true;
                uart_send_safe((char*)frame, 9);
                LOGT(TAG, "Host enter sleep acknowledged");
                uni_msleep(50);
                enter_deep_sleep_with_wakeup();
            }
            break;
            
        case POWER_MODE_EXIT:    // 上位机请求退出休眠（唤醒确认）
            // 更新状态
            g_host_sleeping = false;
            // 上报确认
            uart_send_safe((char*)frame, 9);
            LOGT(TAG, "Host exit sleep acknowledged");
            // 释放信号量（如果有人在等待）
            if (g_wakeup_ack_sem != NULL) {
                uni_sem_post(g_wakeup_ack_sem);
                LOGE(TAG,"Wakeup ack semaphore posted");
            }
            break;
            
        default:
            LOGE(TAG, "Unknown power mode: %d", mode);
            break;
    }
}

// 发送音量响应帧
static void send_volume_response(uint8_t mode, uint8_t volume, uint8_t flag, uint8_t range)
{
    uint8_t buf[9] = {
        0xAA, 0x55, VOLUME_CMD_CODE, mode, volume,
        flag, range,
        0x55, 0xAA
    };
 //   user_uart_send((char*)buf, 9);
    uart_send_safe((char*)buf, 9);
    LOGT(TAG, "Send volume response: mode=%d, vol=%d, flag=%d, range=%d", mode, volume, flag, range);
}

static void process_volume_command(uint8_t *frame)
{
    uint8_t mode = frame[3];    // 0x00查询, 0x01设置
    uint8_t set_vol = frame[4]; // 音量值或静音开关标志
    uint8_t flag = frame[5];    // 1:静音, 0:非静音
    int range = MEDIA_VOLUME_MAX;  // 音量最大值（如100）

    LOGT(TAG, "Volume command: mode=%d, vol=0x%02X, flag=%d", mode, set_vol, flag);

    switch (mode) {
        case VOLUME_MODE_QUERY:
            // 查询模式：直接返回当前状态
            send_volume_response(VOLUME_MODE_QUERY,
                                 (uint8_t)MediaPlayerVolumeGet(),
                                 MediaPlayerIsMute() ? 1 : 0,
                                 range);
            break;

        case VOLUME_MODE_SET:
            // 处理设置模式
            if (set_vol == 0xFF) {
                // ----- 纯静音开关（不改变音量）-----
                if (flag == 0 || flag == 1) {
                    bool target_mute = (flag == 1);
                    if (MediaPlayerIsMute() != target_mute) {
                        MediaPlayerSetMute(target_mute);
                        save_mute_to_flash(target_mute);
                        LOGT(TAG, "Pure mute switch: %s", target_mute ? "ON" : "OFF");
                    }
                } else {
                    LOGW(TAG, "Invalid flag for mute switch: %d", flag);
                }
            } else if (set_vol >= 1 && set_vol <= range) {
                // ----- 正常音量设置（可以同时改变静音状态）-----
                // 1. 处理静音
                if (flag == 0 || flag == 1) {
                    bool target_mute = (flag == 1);
                    if (MediaPlayerIsMute() != target_mute) {
                        MediaPlayerSetMute(target_mute);
                        save_mute_to_flash(target_mute);
                        LOGT(TAG, "Mute changed to %d", target_mute);
                    }
                } else {
                    LOGW(TAG, "Invalid flag value: %d, ignore mute", flag);
                }
                // 2. 处理音量
                int target_vol = set_vol;
                if (target_vol != MediaPlayerVolumeGet()) {
                    MediaPlayerVolumeSet(target_vol);
                    save_volume_to_flash(target_vol);
                    LOGT(TAG, "Volume set to %d", target_vol);
                }
            } else {
                // ----- 无效的 set_vol 值（0 或 101~254 且非 0xFF）-----
                LOGW(TAG, "Invalid vol value: 0x%02X (valid: 1~%d, 0xFF for mute switch)", set_vol, range);
                // 不执行任何操作
            }

            // 无论是否修改，都返回当前最新状态
            send_volume_response(VOLUME_MODE_SET,
                                 (uint8_t)MediaPlayerVolumeGet(),
                                 MediaPlayerIsMute() ? 1 : 0,
                                 range);
            break;

        default:
            LOGE(TAG, "Unknown volume mode: %d", mode);
            break;
    }
}

// ============ LED 控制函数 ============
static void led_set(led_ctrl_t *led, uint8_t on)
{
    user_gpio_set_value(led->pin, on ? 1 : 0);
}

static void led_off(led_ctrl_t *led)
{
    if (!led->timer_initialized) return;
    
 
    led->is_active = false;
    led->mode = LED_MODE_OFF;
    led->state = 0;
    led_set(led, 0);
    
    LOGT(TAG,"LED off (timer still running)\n");
}

static void led_on(led_ctrl_t *led)
{
    if (!led->timer_initialized) return;
    
 
    led->is_active = false;
    led->mode = LED_MODE_ON;
    led->state = 1;
    led_set(led, 1);
    
    LOGT(TAG,"LED on (timer still running)\n");
}

static void led_blink_cb(timer_handle_t timer)
{
        
    if (timer == g_red_led.timer && g_red_led.timer_initialized) {
        if (!g_red_led.is_active) {
            return;
         }
    
        g_red_led.state = !g_red_led.state;
        led_set(&g_red_led, g_red_led.state);
        
        if (!g_red_led.state && g_red_led.count > 0) {
            g_red_led.count--;
            if (g_red_led.count == 0) {
                g_red_led.is_active = false;
                //g_red_led.mode = LED_MODE_OFF;
                send_led_feedback(LED_COLOR_RED, LED_MODE_END);
                led_off(&g_red_led);
            }
        }
        return;
    } 
    if (timer == g_blue_led.timer && g_blue_led.timer_initialized) {
        if (!g_blue_led.is_active) {
            return;
        }
    
        g_blue_led.state = !g_blue_led.state;
        led_set(&g_blue_led, g_blue_led.state);
        
        if (!g_blue_led.state && g_blue_led.count > 0) {
            g_blue_led.count--;
            if (g_blue_led.count == 0) {
                g_blue_led.is_active = false;
             //   g_blue_led.mode = LED_MODE_OFF;
                send_led_feedback(LED_COLOR_BLUE, LED_MODE_END);
                led_off(&g_blue_led);
            }
        }
        return;
    }
}

static void led_blink(led_ctrl_t *led, uint8_t count, uint8_t blink_mode)
{
    if (!led->timer_initialized) {
        LOGE(TAG,"ERROR: LED timer not initialized\n");
        return;
    }

    // 根据 blink_mode 确定周期（半周期，即翻转间隔）
    uint32_t interval_ms;
    switch (blink_mode) {
        case 0x01: interval_ms = 500; break;
        case 0x02: interval_ms = 1000; break;
        case 0x03: interval_ms = 1500; break;
        default:   interval_ms = 500; break;
    }

    // 判断是否需要重新创建定时器（周期变化 或 定时器句柄无效）
    bool need_recreate = (interval_ms != led->blink_interval_ms) ||
                         (led->timer == INVALID_TIMER_HANDLE);

    if (need_recreate) {
        if (led->timer != INVALID_TIMER_HANDLE) {
            user_sw_timer_remove(led->timer);
        }
        led->timer = user_sw_timer_add(interval_ms, false, led_blink_cb);
        if (led->timer == INVALID_TIMER_HANDLE) {
            LOGE(TAG,"ERROR: Failed to create timer for LED blink\n");
            return;
        }
        led->blink_interval_ms = interval_ms;
        led->timer_initialized = true;  // 确保标志
    }

    led->is_active = true;
    led->mode = LED_MODE_BLINK;
    led->count = count;
    led->state = 1;
    led_set(led, 1);

    LOGT(TAG,"LED blink started, count=%d, interval=%d ms (recreate=%d)\n",
        count, interval_ms, need_recreate);
}

static led_ctrl_t* get_led_by_color(uint8_t color)
{
    return (color == LED_COLOR_RED) ? &g_red_led : &g_blue_led;
}

static void send_led_feedback(uint8_t color, uint8_t mode)
{
    uint8_t buf[9] = {0xAA, 0x55, LED_CMD_CODE, color, mode, 0, 0, 0x55, 0xAA};
 //   user_uart_send((char*)buf, 9);
    uart_send_safe((char*)buf, 9);
}

static void process_led_command(uint8_t *frame)
{
    uint8_t color = frame[3];
    uint8_t mode = frame[4];
    uint8_t blink_mode = frame[5];   // 新增：闪烁模式
    uint8_t count = frame[6];
    
    // 添加调试输出
    LOGT(TAG,"=== process_led_command called ===\n");
    LOGT(TAG,"color=%d, mode=%d, blink_mode=%d, count=%d\n", color, mode, blink_mode, count);
    
    led_ctrl_t *led = get_led_by_color(color);
    if (!led) {
        LOGE(TAG,"ERROR: Invalid color %d\n", color);
        return;
    }
    
    LOGT(TAG,"led->pin=%d, current mode=%d\n", led->pin, led->mode);
    
    switch (mode) {
        case LED_MODE_OFF: 
            LOGT(TAG,"Execute LED_MODE_OFF\n");
            led_off(led);
            send_led_feedback(color, led->mode);
            break;
        case LED_MODE_ON:  
            LOGT(TAG,"Execute LED_MODE_ON\n");
            led_on(led);
            send_led_feedback(color, led->mode);
            break;
        case LED_MODE_BLINK: 
 	        LOGT(TAG,"Execute LED_MODE_BLINK, count=%d, blink_mode=%d\n", count, blink_mode);
            led_blink(led, count, blink_mode);  // 传递闪烁模式
            send_led_feedback(color, led->mode);  // 只发送开始闪烁
            break;
        default: 
            LOGE(TAG,"ERROR: Unknown mode %d\n", mode);
            return;
    }
    LOGT(TAG,"=== process_led_command done ===\n");
}


// ============ 软件 UART 回调函数, 5ms 超时回调：判定一帧数据结束 ============
static void timeout_handler(eTIMER_IDX idx)
{
    user_timer_pause(TIMER_TIMEOUT);
    g_rx_flag = true;
}

// ============ 位采样回调 ============
static void bit_sampling_handler(eTIMER_IDX idx)
{
    g_tick_count++; // 每 52us 滴答一次

    // 🚀 核心修正 1：把 3 改成 2！提前一个滴答（52us）采样，完美抵消单片机进入中断的 50us 物理延迟！
    // 现在规律是：在第 2, 4, 6, 8, 10, 12, 14, 16 个滴答时采样
    if (g_tick_count == (2 + 2 * g_bit_index) && g_bit_index < 8) {
        
        uint8_t bit = user_gpio_get_value(SOFT_UART_RX_PIN);
        if (bit) {
            g_current_byte |= (1 << g_bit_index);
        }
        g_bit_index++;
    } 
    // 🚀 核心修正 2：把 19 改成 18！提前结束，给下一个起始位留出充足的监听时间
    else if (g_tick_count >= 18) {
        
        // 1. 关掉节拍器
        user_timer_pause(TIMER_SAMPLING);

        // 2. 存入缓冲区
        if (g_rx_len < TTS_FRAME_LEN) {
            g_rx_buffer[g_rx_len++] = g_current_byte;
        }

        // 3. 状态归零
        g_rx_status = RX_STATE_IDLE;
        g_current_byte = 0;
        g_bit_index = 0;
        g_tick_count = 0;

        // 4. 重置超时打包闹钟
        user_timer_init(TIMER_TIMEOUT, FRAME_TIMEOUT_US, true, timeout_handler);
        user_timer_start(TIMER_TIMEOUT);

        // 5. 立刻开启 GPIO 中断，监听下一个起始位
        user_gpio_interrupt_enable();
    }
}

// ============ GPIO 中断回调：捕捉 UART 起始位（下降沿）============
static void gpio_intr_handler(GPIO_NUMBER num, uni_bool is_high)
{
    // 只有在空闲时，且确实是低电平（下降沿）才响应
    if (g_rx_status == RX_STATE_IDLE && is_high == 0) {
        g_rx_status = RX_STATE_RECEIVING;
        
        // 进门第一件事：立刻关闭 GPIO 中断！防止把数据位里的低电平当成起始位
        user_gpio_interrupt_disable();
        g_current_byte = 0;
        g_bit_index = 0;
        g_tick_count = 0;

        // 直接启动 52us 的自动周期定时器！中途绝不修改它！
        user_timer_init(TIMER_SAMPLING, HALF_BIT_TIME_US, false, bit_sampling_handler);
        user_timer_start(TIMER_SAMPLING);
    }
}

static void uart_tx_task(void *pvParameters) {
    uint8_t buf[9];
    while (1) {
        if (xQueueReceive(uart_tx_queue, buf, portMAX_DELAY) == pdPASS) {
            user_uart_send((char*)buf, 9);
        }
    }
}

// ============ TTS 帧处理任务（喂狗任务）============
static void tts_handler_task(void *args)
{
    uint32_t last_feed_time = 0;
 //   uint32_t last_adc_time = 0;
    uint32_t now;
    int i;
 
   // B8 检测状态变量（static 保证唤醒后值重置，但我们在每次进入检测分支时初始化）
    static int b8_state = 0;          // 0: 等待高电平, 1: 计时中
 //   static uint32_t last_adc_time = 0;
    static uint32_t high_start_time = 0;
    static bool last_sleeping_flag = false; // 用于检测 g_host_sleeping 变化
    
    while (1) {
        // 喂狗（每200ms喂一次）
        now = uni_get_clock_time_ms();
        if (now - last_feed_time >= 200) {
            uni_hal_watchdog_feed();
            last_feed_time = now;
        }
        
        // if (g_adc_enabled &&(now - last_adc_time >= 100)) {
        //     adc_get();
        //     last_adc_time = now;
        // }

        if(g_rx_flag == true)
        {
#if 1
            LOGT(TAG,"==========\n");
            LOGT(TAG,"len = %d, data: ", g_rx_len);
            
            // 收到多少个，就循环打印多少个 HEX
            for (i = 0; i < g_rx_len; i++) {
                LOGT(TAG,"%02X ", g_rx_buffer[i]); 
            }
            LOGT(TAG,"\r\n"); // 循环结束后统一换行
#endif
            //===================
			if(g_rx_len >= TTS_FRAME_LEN && 
                         g_rx_buffer[0] == TTS_HEAD1 && 
                         g_rx_buffer[1] == TTS_HEAD2 &&
                         g_rx_buffer[7] == TTS_TAIL1 && 
                         g_rx_buffer[8] == TTS_TAIL2)
			{
				uint8_t cmd = g_rx_buffer[2];
			        if (cmd == LED_CMD_CODE) {
                                   process_led_command((uint8_t*)g_rx_buffer);
                                } 
                                else if (cmd == VOLUME_CMD_CODE) {   // 新增音量命令处理
        				process_volume_command((uint8_t*)g_rx_buffer);
    				}
    				 else if (cmd == LISTEN_CMD_CODE) {   // 新增分支
        				process_listen_command((uint8_t*)g_rx_buffer);
    				}
                    else if (cmd == POWER_CMD_CODE) {          // 新增
                        process_power_command((uint8_t*)g_rx_buffer);
                    }
                    else if (cmd == CRC_CMD_CODE) {          // 新增CRC处理
                        process_crc_command((uint8_t*)g_rx_buffer);
                    }
                    else {
                        play_tts_by_cmd(cmd);
                        //user_uart_send((char*)g_rx_buffer, g_rx_len);
                        uart_send_safe((char*)g_rx_buffer, g_rx_len);
                        LOGT(TAG,"Echo back to host: %d bytes\n", g_rx_len);
                    }	
				  LOGT(TAG,"cmd = %d\n", cmd);
				
			}
            
            g_rx_flag = false;
            g_rx_len = 0;
        }

       // ----- 检测 g_host_sleeping 状态变化 -----
        bool current_sleeping = g_host_sleeping;
        if (current_sleeping != last_sleeping_flag) {
            // 状态发生变化，如果变为 false，则重置 B8 检测状态机
            if (!current_sleeping) {
                b8_state = 0;
                high_start_time = 0;
                LOGT(TAG, "g_host_sleeping changed to false, reset B8 detector");
            } else {
                LOGT(TAG, "g_host_sleeping changed to true, start B8 detection");
            }
            last_sleeping_flag = current_sleeping;
        }

        // ----- 仅在 g_host_sleeping == true 时执行 B8 检测 -----
        if (g_host_sleeping) {
            int current_level = user_gpio_get_value(GPIO_NUM_B8);

            if (b8_state == 0) {
                // 等待高电平
                if (current_level == 1) {
                    high_start_time = uni_get_clock_time_ms();
                    b8_state = 1;
                    LOGT(TAG, "B8 high detected, start 30s timer");
                }
            } else { // b8_state == 1
                if (current_level == 0) {
                    // 检测到低电平，重置计时
                    b8_state = 0;
                    LOGT(TAG, "B1 low detected, reset timer");
                } else {
                    // 持续高电平，检查超时
                    now = uni_get_clock_time_ms();
                    if (now - high_start_time >= 30000) { // 30 秒
                        LOGT(TAG, "B1 high for 30s, go to deep sleep");
                        b8_state = 0;   // 重置状态，避免唤醒后立即再进
                        enter_deep_sleep_with_wakeup();
                        // 唤醒后继续循环，b8_state 已为 0，重新开始检测
                    }
                }
            }
        } else {
            // 如果 g_host_sleeping == false，确保状态机处于空闲状态（防止残留）
            if (b8_state != 0) {
                b8_state = 0;
                high_start_time = 0;
                // 可选择性打印调试信息，但减少日志量
            }
        }

        // ----- 5. 短暂延时，降低 CPU 占用 -----
        uni_msleep(50); 
    }
}

// ============ 唤醒后恢复硬件（不创建任务）============
static void deep_sleep_restore(void) {
    uni_hal_watchdog_feed();
    GPIO_PortBModeSet(GPIOB8, 0);
    user_gpio_set_mode(GPIO_NUM_B8, GPIO_MODE_IN);
    user_gpio_set_pull_mode(GPIO_NUM_B8, GPIO_PULL_UP);

    DBG("Woke up, reinitializing hardware...");
      GIE_ENABLE();
 //   user_gpio_init();

 //   restore_audio_settings();

    // 恢复 GPIO 输出状态（根据实际需求设置）
    user_gpio_set_mode(GPIO_NUM_A26, GPIO_MODE_OUT);
    user_gpio_set_value(GPIO_NUM_A26, 0);
 //   user_gpio_set_value(GPIO_NUM_A27, 0);
    user_gpio_set_mode(GPIO_NUM_A28, GPIO_MODE_OUT);
    user_gpio_set_value(GPIO_NUM_A28, 0);
    user_gpio_set_mode(GPIO_NUM_B0, GPIO_MODE_OUT);
    user_gpio_set_value(GPIO_NUM_B0, 0);
    user_gpio_set_mode(GPIO_NUM_B1, GPIO_MODE_OUT);
    user_gpio_set_value(GPIO_NUM_B1, 1);
    g_b1_power_state = 0;

   
    // adc_init();
    // g_adc_enabled = true;

    // 重新初始化软件 UART 硬件（GPIO 中断 + 状态）
    soft_uart_hw_init();

    // 重新初始化 LED 定时器
    led_init();

    
    doa_uart_reinit_hw();   // 替换原来的 doa_uart_init()

    if (g_wakeup_ack_sem != NULL) {
    uni_sem_init(&g_wakeup_ack_sem, 0);
    
    
 
}

 // ！！！重要：上位机仍在休眠，g_host_sleeping 保持 true，不发送任何数据
    DBG( "Deep sleep wakeup complete, g_host_sleeping=%d", g_host_sleeping);
}

// ============ 进入深度睡眠（由上位机指令触发）============
static void enter_deep_sleep_with_wakeup(void) {
    LOGT(TAG, "Entering deep sleep, wakeup by GPIO B1 falling edge...");
    // 进入深度睡眠，唤醒后继续执行本函数后的代码
    // g_adc_enabled = false;
    // uni_msleep(50);

    // user_gpio_set_mode(GPIO_NUM_A27, GPIO_MODE_IN);   // 改为普通输入

    if (g_wakeup_ack_sem != NULL) {
    uni_sem_destroy(g_wakeup_ack_sem);
    g_wakeup_ack_sem = NULL;
}

    GIE_DISABLE();
    uni_msleep(100);
    uni_hal_enterdeepsleep(_wakeup_cb, WAKEUP_GPIOB8,  WAKEUP_GPIONEGE);
  //  uni_hal_enterdeepsleep(_wakeup_cb, WAKEUP_GPIOA25,  WAKEUP_GPIONEGE);
    // ---------- 唤醒后从这里继续 ----------
    deep_sleep_restore();
}

// ============ 初始化函数 ============

static void led_init(void)
{
    user_gpio_set_mode(LED_RED_PIN, GPIO_MODE_OUT);
    user_gpio_set_value(LED_RED_PIN, 0);
    user_gpio_set_mode(LED_BLUE_PIN, GPIO_MODE_OUT);
    user_gpio_set_value(LED_BLUE_PIN, 0);
    user_sw_timer_init(eTIMER2, 20);

    // 删除旧定时器（如果存在）
    if (g_red_led.timer != INVALID_TIMER_HANDLE) {
        user_sw_timer_remove(g_red_led.timer);
        g_red_led.timer = INVALID_TIMER_HANDLE;
    }
    //if (!g_red_led.timer_initialized) {
        g_red_led.timer = user_sw_timer_add(500, false, led_blink_cb);
        if (g_red_led.timer != INVALID_TIMER_HANDLE) {
            g_red_led.timer_initialized = true;
            g_red_led.blink_interval_ms = 500;
            LOGT(TAG, "Red LED timer created (handle=0x%08X)", g_red_led.timer);
        } else {
            LOGE(TAG, "Failed to create red LED timer");
        }
    //}
    
    if (g_blue_led.timer != INVALID_TIMER_HANDLE) {
        user_sw_timer_remove(g_blue_led.timer);
        g_blue_led.timer = INVALID_TIMER_HANDLE;
    }
    //if (!g_blue_led.timer_initialized) {
        g_blue_led.timer = user_sw_timer_add(500, false, led_blink_cb);
        if (g_blue_led.timer != INVALID_TIMER_HANDLE) {
            g_blue_led.timer_initialized = true;
            g_blue_led.blink_interval_ms = 500;
            LOGT(TAG, "Blue LED timer created (handle=0x%08X)", g_blue_led.timer);
        } else {
            LOGE(TAG, "Failed to create blue LED timer");
        }
    //}
    
    LOGT(TAG, "LED initialized with timer reuse pool");
}


// ============ 初始化软件 UART ============
static void soft_uart_init(void)
{
    // 1. 引脚配置
    user_gpio_set_mode(SOFT_UART_RX_PIN, GPIO_MODE_IN);
    user_gpio_set_pull_mode(SOFT_UART_RX_PIN, GPIO_PULL_UP);
    
    // 2. 注册下降沿中断
    user_gpio_set_interrupt(SOFT_UART_RX_PIN, GPIO_INT_NEG_EDGE, gpio_intr_handler);
    user_gpio_interrupt_enable();
    
    // 3. 重置状态
    g_rx_status = RX_STATE_IDLE;
    g_rx_len = 0;
    
    // 4. 创建喂狗任务（高优先级）
    uni_pthread_t pid;
    thread_param param;
    param.stack_size = STACK_SMALL_SIZE;
    param.priority = OS_PRIORITY_HIGH;
    uni_strncpy(param.task_name, "tts_task", sizeof(param.task_name) - 1);
    uni_pthread_create(&pid, &param, tts_handler_task, NULL);
}

// ============ 软件 UART 硬件初始化（不含任务创建）============
static void soft_uart_hw_init(void) {
    // 1. 引脚配置
    user_gpio_set_mode(SOFT_UART_RX_PIN, GPIO_MODE_IN);
    user_gpio_set_pull_mode(SOFT_UART_RX_PIN, GPIO_PULL_UP);

    // 2. 注册下降沿中断
    user_gpio_set_interrupt(SOFT_UART_RX_PIN, GPIO_INT_NEG_EDGE, gpio_intr_handler);
    user_gpio_interrupt_enable();

    // 3. 重置状态
    g_rx_status = RX_STATE_IDLE;
    g_rx_len = 0;
    // 定时器在需要时由中断动态启动，无需在此初始化
    LOGT(TAG, "Soft UART hardware initialized (no task created)");
}

// ============ 辅助函数：发送带角度的数据 ============
static void send_command_with_angle(uint8_t cmd_code, int16_t angle)
{
    uint8_t tx_buffer[9] = {
        0xAA, 0x55, cmd_code,
        0x00, 0x00,
        (angle >> 8) & 0xFF, angle & 0xFF,
        0x55, 0xAA
    };
 //   user_uart_send((char*)tx_buffer, 9);
    uart_send_safe((char*)tx_buffer, 9);
    LOGT(TAG, "Send: CMD=0x%02X, Angle=%d", cmd_code, angle);
    
    setting_session_clear_doa_angle();
    
    doa_reset_data();
}

// ============ 命令词回调 ============
static void _custom_setting_cb(USER_EVENT_TYPE event, user_event_context_t *context)
{
    event_custom_setting_t *setting = &context->custom_setting;
    int16_t angle = setting_session_get_last_doa_angle();
    
    LOGT(TAG, "user command: %s, DOA angle: %d", setting->cmd, angle);
    
    if (0 == uni_strcmp(setting->cmd, "come")) {
        send_command_with_angle(0x43, angle);
    } else if (0 == uni_strcmp(setting->cmd, "away")) {
        send_command_with_angle(0x47, angle);
    } else if (0 == uni_strcmp(setting->cmd, "gohome")) {
        send_command_with_angle(0x48, angle);
    } else if (0 == uni_strcmp(setting->cmd, "opencover")) {
        send_command_with_angle(0x49, angle);
    } else if (0 == uni_strcmp(setting->cmd, "closecover")) {
        send_command_with_angle(0x4A, angle);
    } else if (0 == uni_strcmp(setting->cmd, "stop")) {
        send_command_with_angle(0x53, angle);
    } else if (0 == uni_strcmp(setting->cmd, "volumeUpUni")) {
//      send_command_with_angle(0x54, angle);
    } else if (0 == uni_strcmp(setting->cmd, "volumeDownUni")) {
//  	send_command_with_angle(0x55, angle);
    } else if (0 == uni_strcmp(setting->cmd, "fowrad")) {
        send_command_with_angle(0x56, angle);
    } else if (0 == uni_strcmp(setting->cmd, "backward")) {
        send_command_with_angle(0x57, angle);
    } else if (0 == uni_strcmp(setting->cmd, "left")) {
        send_command_with_angle(0x58, angle);
    } else if (0 == uni_strcmp(setting->cmd, "right")) {
        send_command_with_angle(0x59, angle);
//    } else if (0 == uni_strcmp(setting->cmd, "forward_left")) {
//        send_command_with_angle(0x5A, angle);
//    } else if (0 == uni_strcmp(setting->cmd, "forward_right")) {
//        send_command_with_angle(0x5B, angle);
//    } else if (0 == uni_strcmp(setting->cmd, "backward_left")) {
//        send_command_with_angle(0x5C, angle);
//    } else if (0 == uni_strcmp(setting->cmd, "backward_right")) {
//        send_command_with_angle(0x5D, angle);
//    } else if (0 == uni_strcmp(setting->cmd, "little_left")) {
//        send_command_with_angle(0x5E, angle);
//    } else if (0 == uni_strcmp(setting->cmd, "little_right")) {
//        send_command_with_angle(0x5F, angle);
    } else if (0 == uni_strcmp(setting->cmd, "back_up")) {
        send_command_with_angle(0x60, angle);
    } else {
        LOGT(TAG, "Unconcerned command: %s", setting->cmd);
    }
    user_player_reply_list_random(setting->reply_files);
}

static void _goto_awakened_cb(USER_EVENT_TYPE event, user_event_context_t *context)
{
    event_goto_awakend_t *awkened = NULL;
    if (context) 
    {
        awkened = &context->goto_awakend;
        if (g_host_sleeping) 
        {
            LOGT(TAG, "Host is sleeping, sending wakeup sequence (0xCC x4)");
        
            // 发送唤醒序列
            uint8_t wakeup_seq[4] = {0xCC, 0xCC, 0xCC, 0xCC};
            user_uart_send((char*)wakeup_seq, 4);  // 直接发送，不经过队列
        
            // 等待上位机回复 0xD0,0x02，超时 2000ms
            if (g_wakeup_ack_sem != NULL) 
            {
               // uni_sem_wait_ms(g_wakeup_ack_sem, 0);
                if (uni_sem_wait_ms(g_wakeup_ack_sem, 500) == 0) 
                {
                    LOGT(TAG,"Got host wakeup ACK (0xD0,0x02)");
                } else {
                    LOGT(TAG,"Timeout waiting for host wakeup ACK");
                }
                LOGT(TAG,"After semaphore wait, going to play reply");
            }else {
                LOGE(TAG,"Wakeup semaphore not initialized");
            }
        }
        if(g_b1_power_state == 0)
        {
            user_gpio_set_value(GPIO_NUM_B1, 0);
            g_b1_power_state = 1;
            int16_t angle = setting_session_get_last_doa_angle();  
            send_command_with_angle(0x46, angle);
            uni_msleep(1000);
            user_player_reply_list_num(awkened->reply_files, 0);
        }else{
            int16_t angle = setting_session_get_last_doa_angle(); 
            send_command_with_angle(0x46, angle);
            user_player_reply_list_num(awkened->reply_files, 1);
        }
    }
}

static void _goto_sleeping_cb (USER_EVENT_TYPE event, user_event_context_t *context) 
{
    event_goto_sleeping_t *sleeping = NULL;
    uint8_t report_mode = 0xFF; 
    
    if (g_sleep_by_command) {
        report_mode = 0x01;        // 指令触发休眠
        g_sleep_by_command = false; // 重置标志
    } else {
        report_mode = 0xFF;        // 其他原因（超时等）
    }
    
    if (context) {
    sleeping = &context->goto_sleeping;
    // 播放休眠提示音（与"退下"命令相同的提示）
    
 //   user_player_reply_list_random(sleeping->reply_files);
    (void)sleeping;
    }
    user_gpio_set_value(GPIO_NUM_B1, 1);
    g_b1_power_state = 0;
    
    uint8_t report_buf[9] = {
    0xAA, 0x55, LISTEN_CMD_CODE,
    report_mode,
    0x00, 0x00, 0x00,
    0x55, 0xAA
    };
    uart_send_safe((char*)report_buf, 9);
//  LOGT(TAG, "Report sleep: cmd=0xC0, mode=0x%02X, A27 LOW", report_mode);
}

static void _study_event_cb(USER_EVENT_TYPE event, user_event_context_t *context) {
    uint8_t buf[9] = {0xAA, 0x55, 0xE0, 0x00, 0x00, 0x00, 0x00, 0x55, 0xAA};
    switch (event) {
        case USER_STUDY_START_EVENT:
            buf[3] = 0x01;
            uart_send_safe((char*)buf, 9);
            LOGT(TAG, "Study start reported via UART");
            break;
        case USER_STUDY_SUCCESS_EVENT:
            buf[3] = 0x02;
            uart_send_safe((char*)buf, 9);
            LOGT(TAG, "Study success reported via UART");
            break;
        case USER_STUDY_FAIL_EVENT:
            buf[3] = 0x03;
            uart_send_safe((char*)buf, 9);
            LOGT(TAG, "Study failure reported via UART");
            break;
        case USER_STUDY_RESET_EVENT:
            buf[3] = 0x04;
            uart_send_safe((char*)buf, 9);
            LOGT(TAG, "Study reset reported via UART");
            break;
        default:
            break;
    }
}

static void _register_event_callback(void)
{
    user_event_subscribe_event(USER_CUSTOM_SETTING, _custom_setting_cb);
    user_event_subscribe_event(USER_GOTO_AWAKENED, _goto_awakened_cb);
    user_event_subscribe_event(USER_GOTO_SLEEPING, _goto_sleeping_cb);

    user_event_subscribe_event(USER_STUDY_START_EVENT, _study_event_cb);
    user_event_subscribe_event(USER_STUDY_SUCCESS_EVENT, _study_event_cb);
    user_event_subscribe_event(USER_STUDY_FAIL_EVENT, _study_event_cb);
    user_event_subscribe_event(USER_STUDY_RESET_EVENT, _study_event_cb);
}

// ============ 主初始化函数 ============
int hb_auto_gpio(void)
{

    restore_audio_settings();
#if 0
    LogInitialize();
    LogLevelSet(N_LOG_DEBUG);
#endif
    user_gpio_init();
    
   // 初始化LED
    led_init();
    // 配置其他GPIO
    user_gpio_set_mode(GPIO_NUM_A26, GPIO_MODE_OUT);
    user_gpio_set_value(GPIO_NUM_A26, 0);
 //   user_gpio_set_mode(GPIO_NUM_A27, GPIO_MODE_OUT);
 //   user_gpio_set_value(GPIO_NUM_A27, 0); 
    user_gpio_set_mode(GPIO_NUM_A28, GPIO_MODE_OUT);
    user_gpio_set_value(GPIO_NUM_A28, 0);
    
    user_gpio_set_mode(GPIO_NUM_B0, GPIO_MODE_OUT);
    user_gpio_set_value(GPIO_NUM_B0, 0);
    
    user_gpio_set_mode(GPIO_NUM_B8, GPIO_MODE_IN);
    user_gpio_set_pull_mode(GPIO_NUM_B8, GPIO_PULL_UP);  

    g_b1_power_state = 0;
    user_gpio_set_mode(GPIO_NUM_B1, GPIO_MODE_OUT);
    user_gpio_set_value(GPIO_NUM_B1, 1);

  //  adc_init();

    // 初始化 DOA UART
    if (doa_uart_init() != 0) {
        LOGE(TAG, "DOA UART init failed");
    }
       
    // 初始化软件 UART
 //   soft_uart_init();
    
// ----- 软件 UART 硬件初始化（只配 GPIO 中断，不创建任务）-----
    soft_uart_hw_init();

    // 创建 UART 发送队列和任务
    uart_tx_queue = xQueueCreate(10, 9);
    if (uart_tx_queue == NULL) {
        LOGE(TAG, "Failed to create UART queue");
    } else {
        // 创建发送任务（优先级低于音频任务）
        xTaskCreate(uart_tx_task, "uart_tx", STACK_SMALL_SIZE, NULL,
                    OS_PRIORITY_LOW, NULL);
        LOGT(TAG, "UART sender task created");
    }

  // ----- 创建 TTS 处理任务（原在 soft_uart_init 中，现移到这里）-----
    uni_pthread_t pid;
    thread_param param;
    uni_memset(&param, 0, sizeof(param));
    param.stack_size = STACK_SMALL_SIZE;
    param.priority = OS_PRIORITY_HIGH;
    uni_strncpy(param.task_name, "tts_task", sizeof(param.task_name) - 1);
    if (uni_pthread_create(&pid, &param, tts_handler_task, NULL) != 0) {
        LOGE(TAG, "Create tts task failed");
    } else {
        uni_pthread_detach(pid);
        LOGT(TAG, "TTS handler task created");
    }

    // 初始化二进制信号量，初始值为 0（不可用）
    int ret = uni_sem_init(&g_wakeup_ack_sem, 0);
    LOGT(TAG,"uni_sem_init ret=%d, sem=%p\n", ret, g_wakeup_ack_sem);
    if (ret != 0) {      
        LOGE(TAG,"Failed to init semaphore\n");
        g_wakeup_ack_sem = NULL;
    } else {
        LOGT(TAG,"Semaphore initialized OK\n");
    }   
    g_host_sleeping = false;

    _register_event_callback();
    return 0;
}


