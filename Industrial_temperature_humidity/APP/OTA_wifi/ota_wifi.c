#include "ota_wifi.h"
#include "ota_Statemachine.h"
#include "ui.h"
#include "FreeRTOS.h"
#include "task.h"

static void ota_delay_ms(uint32_t delay)
{
    if (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING)
    {
        vTaskDelay(pdMS_TO_TICKS(delay));
    }
    else
    {
        delay_ms((uint16_t)delay);
    }
}
/**
 * @description: ESP8266硬件初始化(复位引脚GPIO)
 * @return {*}
 */
static void ota_esp8266_hw_init(void)
{
    GPIO_InitTypeDef gpio_init_struct;

    ota_esp8266_RST_GPIO_CLK_ENABLE();

    gpio_init_struct.Pin = ota_esp8266_RST_GPIO_PIN;
    gpio_init_struct.Mode = GPIO_MODE_OUTPUT_PP;
    gpio_init_struct.Pull = GPIO_NOPULL;
    gpio_init_struct.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(ota_esp8266_RST_GPIO_PORT, &gpio_init_struct);
}

/**
 * @description: ESP8266硬件复位(拉低100ms再拉高500ms)
 * @return {*}
 */
void ota_esp8266_hw_reset(void)
{
    ota_8266D_RST(0);
    ota_delay_ms(100);
    ota_8266D_RST(1);
    ota_delay_ms(500);
}

/**
 * @description: ESP8266初始化: 硬件复位 + 串口初始化(115200, 模块默认波特率) + AT测试 + 设置Station模式
 * 由于我RST引脚有问题
 * 流程做了双保险：硬件复位失效没关系，后面 ota_esp8266_ensure_at_mode() 用软件方式（发 +++ → 发 AT）把模块拉回 AT 指令模式——这个不依赖 RST。
 * 注意: 连WiFi不放这里, 放在状态机的WIFI_CONNECT状态(失败可重试)
 * @return {uint8_t}  OTA_ESP8266_EOK 成功 / OTA_ESP8266_ERROR 失败
 */
uint8_t ota_esp8266_init(void)
{
    /* 1. 硬件初始化(复位引脚GPIO) */
    ota_esp8266_hw_init();

    /* 2. 串口初始化(115200, 模块默认波特率) */
    ota_esp8266_uart_init(115200);

    /* 3. 硬件复位: 模块复位后会发ready(波特率正确时) */
    ota_esp8266_hw_reset();

    /* 4. 确保模块在AT指令模式: 本硬件RST引脚浮空(hw_reset无效),
       若上次透传失败没退出, 模块会把AT指令当数据转发, 这里用+++强制退出 */
    ota_esp8266_ensure_at_mode();

    /* 5. AT测试 */
    if (ota_esp8266_at_test() != OTA_ESP8266_EOK)   //最多6s
    {
        printf("ESP8266 AT测试失败, 请检查模块供电/串口/复位引脚\r\n");
        return OTA_ESP8266_ERROR;
    }
    printf("ESP8266初始化成功\r\n");

    /* 4. 设置Station模式 */
    ota_esp8266_set_mode(1);

    return OTA_ESP8266_EOK;
}

/**
 * @description: 发送AT指令并等待应答
 *               接收改为stream_read积累到缓冲再strstr匹配(应答可能跨多个环形段)
 * @param {char*}     cmd     要发送的AT指令
 * @param {char*}     ack     等待的应答内容
 * @param {uint32_t}  timeout 等待超时时间(毫秒)
 * @return {uint8_t}  OTA_ESP8266_EOK 成功 / OTA_ESP8266_ETIMEOUT 超时
 */
uint8_t ota_esp8266_send_at_cmd(char *cmd, char *ack, uint32_t timeout)
{
    static char rx_buf[512];     /* 应答积累缓冲(static避免占大块栈) */
    uint16_t rx_len = 0;
    uint16_t len;

    /* 清除当前接收缓冲(读指针一次性追平): 依赖ISR“先写数据后W++”纪律,快照后新到字节落在R之后下轮可读, 因此这个无需临界区 */
    U3CB.URxReadCnt = U3CB.URxWriteCnt;

    ota_esp8266_uart_printf("%s\r\n", cmd);

    if ((ack == NULL) || (timeout == 0))
    {
        return OTA_ESP8266_EOK;
    }
    else
    {
        while (timeout > 0) //手动计时
        {
            len = ota_esp8266_uart_stream_read((uint8_t *)(rx_buf + rx_len), (sizeof(rx_buf) - 1) - rx_len);
            rx_len += len;
            rx_buf[rx_len] = '\0';  /* 时刻保持结尾有\0, 后面的strstr才不会越界 */

            if (strstr((const char *)rx_buf, ack) != NULL) /* 找到关键字就成功 */
            {
                return OTA_ESP8266_EOK;
            }

            timeout--;
            ota_delay_ms(1);
        }

        return OTA_ESP8266_ETIMEOUT;
    }
}

/**
 * @description: AT指令测试(设置了最多试10次；也可以加次数) 
 * @return {uint8_t}  OTA_ESP8266_EOK 成功 / OTA_ESP8266_ETIMEOUT 失败
 */
uint8_t ota_esp8266_at_test(void)
{
    uint8_t i;

    for (i = 0; i < 10; i++)
    {
        if (ota_esp8266_send_at_cmd("AT", "OK", 500) == OTA_ESP8266_EOK)
        {
            return OTA_ESP8266_EOK;
        }
        ota_delay_ms(100);
    }

    return OTA_ESP8266_ETIMEOUT;
}

/**
 * @description: 设置WiFi工作模式
 * @param {uint8_t} mode 1=Station, 2=AP, 3=AP+Station 
 * @return {uint8_t}  OTA_ESP8266_EOK 成功 / OTA_ESP8266_ETIMEOUT 超时
 */
uint8_t ota_esp8266_set_mode(uint8_t mode)
{
    char cmd[32];

    sprintf(cmd, "AT+CWMODE=%d", mode);
    return ota_esp8266_send_at_cmd(cmd, "OK", 500);
}

/**
 * @description: 连接WiFi热点(等"WIFI GOT IP")
 * @param {char*} ssid WiFi名称
 * @param {char*} pwd  WiFi密码
 * @return {uint8_t}  OTA_ESP8266_EOK 成功 / OTA_ESP8266_ETIMEOUT 超时
 */
uint8_t ota_esp8266_join_ap(char *ssid, char *pwd)
{
    char cmd[64];

    sprintf(cmd, "AT+CWJAP=\"%s\",\"%s\"", ssid, pwd);
    return ota_esp8266_send_at_cmd(cmd, "OK", 20000); /* 20s: 密码错或信号弱时模块尝试连接时间可能超过10s */
}

/**
 * @description: 获取模块IP地址(发AT+CIFSR解析引号内IP)
 * @param {char*} buf 存放IP的缓冲(至少16字节)
 * @return {uint8_t}  OTA_ESP8266_EOK 成功 / OTA_ESP8266_ETIMEOUT 超时 / OTA_ESP8266_ERROR 解析失败
 */
uint8_t ota_esp8266_get_ip(char *buf)
{
    static char rx_buf[512];     /* 应答积累缓冲 */
    char *p_start;
    char *p_end;
    uint16_t rx_len = 0;
    uint16_t len;
    uint32_t timeout = 2000;

    /* 清除接收缓冲(读指针一次性追平), 避免残留应答干扰匹配 */
    U3CB.URxReadCnt = U3CB.URxWriteCnt;;

    ota_esp8266_uart_printf("AT+CIFSR\r\n");

    while (timeout > 0)
    {
        len = ota_esp8266_uart_stream_read((uint8_t *)(rx_buf + rx_len), (sizeof(rx_buf) - 1) - rx_len);
        rx_len += len;
        rx_buf[rx_len] = '\0';  /* 时刻保持结尾有\0, strstr才不会越界 */

        if (strstr((const char *)rx_buf, "OK") != NULL)  /* 应答以OK结尾 */
        {
            break;
        }

        timeout--;
        ota_delay_ms(1);
    }

    if (timeout == 0)
    {
        return OTA_ESP8266_ETIMEOUT;
    }

    /* 解析第一个引号对内的内容(STAIP在STAMAC之前, 第一个引号对就是IP) */
    p_start = strchr(rx_buf, '\"');
    if (p_start == NULL)
    {
        return OTA_ESP8266_ERROR;
    }
    p_start++;
    p_end = strchr(p_start, '\"');
    if (p_end == NULL)
    {
        return OTA_ESP8266_ERROR;
    }
    *p_end = '\0';
    strcpy(buf, p_start);

    return OTA_ESP8266_EOK;
}

/**
 * @description: 连接TCP服务器(支持域名)
 * @param {char*} server_ip   服务器IP或域名
 * @param {char*} server_port 服务器端口(字符串形式)
 * @return {uint8_t}  OTA_ESP8266_EOK 成功 / OTA_ESP8266_ETIMEOUT 超时
 */
uint8_t ota_esp8266_connect_tcp_server(char *server_ip, char *server_port)
{
    char cmd[64];

    sprintf(cmd, "AT+CIPSTART=\"TCP\",\"%s\",%s", server_ip, server_port);
    return ota_esp8266_send_at_cmd(cmd, "CONNECT", 5000);
}

/**
 * @description: 进入透传模式(CIPMODE=1后CIPSEND, 等">")
 *               透传后模块把TCP收发的数据直接映射到串口, OTA固件流就这样进来
 * @return {uint8_t}  OTA_ESP8266_EOK 成功 / OTA_ESP8266_ERROR 失败
 */
uint8_t ota_esp8266_enter_unvarnished(void)
{
    if (ota_esp8266_send_at_cmd("AT+CIPMODE=1", "OK", 500) != OTA_ESP8266_EOK)
    {
        return OTA_ESP8266_ERROR;
    }

    return ota_esp8266_send_at_cmd("AT+CIPSEND", ">", 500);
}

/**
 * @description: 退出透传模式(发"+++", 等模块退出)
 *               本方案用不到, 保留备用
 * @return {*}
 */
void ota_esp8266_exit_unvarnished(void)
{
    ota_esp8266_uart_printf("+++");
    ota_delay_ms(1000);
}

/**
 * @description: 确保模块回到AT指令模式(退出透传模式+关闭遗留TCP连接) 原因：二次升级时 ESP8266 仍处于透传模式，AT 指令发不进去。
 *               ota回滚 和 失败/超时回待机后调用
 * 
 *               透传(CIPMODE=1)状态下模块不执行AT指令, 串口数据全转发TCP:
 *               上次透传没退出就重试, AT指令会被吞掉(表现为AT超时, 收到的都是残留数据)
 * 
 *               由于我本硬件RST引脚浮空(hw_reset无效), 只能用"+++"主动退出透传
 *               设定步骤: +++(无CRLF)+1.5s空闲 → 冲掉+++残留行 → 清缓冲 → CIPCLOSE关TCP → AT同步确认
 * @return {*}
 */
#define OTA_TIME_RETRY_MS       (5UL * 60UL * 1000UL)
#define OTA_TIME_RESYNC_MS      (6UL * 60UL * 60UL * 1000UL)
#define OTA_TIME_START_DELAY_MS 2000UL

static ota_datetime_t s_network_time;
static uint32_t s_network_time_tick;
static uint8_t s_network_time_valid;
static volatile uint32_t s_next_time_sync;
static volatile uint8_t s_time_sync_scheduled;

static uint8_t ota_time_is_leap_year(uint16_t year)
{
    return (uint8_t)(((year % 4U) == 0U && (year % 100U) != 0U) || (year % 400U) == 0U);
}

static uint8_t ota_time_days_in_month(uint16_t year, uint8_t month)
{
    static const uint8_t days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

    if(month == 2U && ota_time_is_leap_year(year)) return 29U;
    if(month < 1U || month > 12U) return 31U;
    return days[month - 1U];
}

static void ota_time_add_seconds(ota_datetime_t *datetime, uint32_t seconds)
{
    uint32_t value;
    uint32_t carry;

    value = (uint32_t)datetime->second + seconds;
    datetime->second = (uint8_t)(value % 60U);
    carry = value / 60U;

    value = (uint32_t)datetime->minute + carry;
    datetime->minute = (uint8_t)(value % 60U);
    carry = value / 60U;

    value = (uint32_t)datetime->hour + carry;
    datetime->hour = (uint8_t)(value % 24U);
    carry = value / 24U;

    while(carry > 0U)
    {
        datetime->day++;
        if(datetime->day > ota_time_days_in_month(datetime->year, datetime->month))
        {
            datetime->day = 1U;
            datetime->month++;
            if(datetime->month > 12U)
            {
                datetime->month = 1U;
                datetime->year++;
            }
        }
        carry--;
    }
}

static uint8_t ota_time_month_from_name(const char *name)
{
    static const char month_names[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    uint8_t month;

    for(month = 0U; month < 12U; month++)
    {
        if(strncmp(name, &month_names[month * 3U], 3U) == 0) return (uint8_t)(month + 1U);
    }
    return 0U;
}

static uint8_t ota_esp8266_query_sntp_time(ota_datetime_t *datetime)
{
    static char rx_buf[256];
    char week_name[4];
    char month_name[4];
    char *time_line;
    uint16_t rx_len = 0U;
    uint16_t len;
    uint32_t timeout = 5000U;
    unsigned int day;
    unsigned int hour;
    unsigned int minute;
    unsigned int second;
    unsigned int year;
    uint8_t month;

    U3CB.URxReadCnt = U3CB.URxWriteCnt;
    ota_esp8266_uart_printf("AT+CIPSNTPTIME?\r\n");

    while(timeout > 0U)
    {
        len = ota_esp8266_uart_stream_read((uint8_t *)(rx_buf + rx_len),
                                           (uint16_t)((sizeof(rx_buf) - 1U) - rx_len));
        rx_len += len;
        rx_buf[rx_len] = '\0';

        if(strstr(rx_buf, "ERROR") != NULL) return OTA_ESP8266_ERROR;

        time_line = strstr(rx_buf, "+CIPSNTPTIME:");
        if(time_line != NULL)
        {
            time_line += strlen("+CIPSNTPTIME:");
            if(sscanf(time_line, "%3s %3s %u %u:%u:%u %u",
                      week_name, month_name, &day, &hour, &minute, &second, &year) == 7)
            {
                month = ota_time_month_from_name(month_name);
                if(year >= 2020U && year <= 2099U && month != 0U &&
                   day >= 1U && day <= ota_time_days_in_month((uint16_t)year, month) &&
                   hour < 24U && minute < 60U && second < 60U)
                {
                    datetime->year = (uint16_t)year;
                    datetime->month = month;
                    datetime->day = (uint8_t)day;
                    datetime->hour = (uint8_t)hour;
                    datetime->minute = (uint8_t)minute;
                    datetime->second = (uint8_t)second;
                    return OTA_ESP8266_EOK;
                }
            }
        }

        timeout--;
        ota_delay_ms(1U);
    }

    return OTA_ESP8266_ETIMEOUT;
}

static uint8_t ota_esp8266_sync_network_time(ota_datetime_t *datetime)
{
    uint8_t retry;

    if(ota_esp8266_send_at_cmd("AT+CIPSNTPCFG=1,8,\"ntp.aliyun.com\"", "OK", 2000U) != OTA_ESP8266_EOK)
    {
        return OTA_ESP8266_ERROR;
    }

    for(retry = 0U; retry < 3U; retry++)
    {
        ota_delay_ms(1500U);
        if(ota_esp8266_query_sntp_time(datetime) == OTA_ESP8266_EOK)
        {
            return OTA_ESP8266_EOK;
        }
    }

    return OTA_ESP8266_ETIMEOUT;
}

void ota_network_time_request_sync(void)
{
    s_next_time_sync = HAL_GetTick() + 5000U;
    s_time_sync_scheduled = 1U;
}

uint8_t ota_network_time_get(ota_datetime_t *datetime)
{
    uint32_t now;
    uint32_t elapsed_seconds;
    uint8_t valid;

    if(datetime == NULL) return 0U;

    taskENTER_CRITICAL();
    valid = s_network_time_valid;
    if(valid)
    {
        now = HAL_GetTick();
        elapsed_seconds = (now - s_network_time_tick) / 1000U;
        if(elapsed_seconds > 0U)
        {
            ota_time_add_seconds(&s_network_time, elapsed_seconds);
            s_network_time_tick += elapsed_seconds * 1000U;
        }
        *datetime = s_network_time;
    }
    taskEXIT_CRITICAL();

    return valid;
}

void ota_network_time_process(void)
{
    ota_datetime_t datetime;
    uint32_t now = HAL_GetTick();

    if(!s_time_sync_scheduled)
    {
        s_next_time_sync = now + OTA_TIME_START_DELAY_MS;
        s_time_sync_scheduled = 1U;
    }

    if(ota_state != OTA_STATE_IDLE || (int32_t)(now - s_next_time_sync) < 0) return;

    s_next_time_sync = now + OTA_TIME_RETRY_MS;

    if(data_wifi_ssid_get()[0] == '\0' || strcmp(data_wifi_ssid_get(), "your_ssid") == 0)
    {
        return;
    }

    if(ota_esp8266_join_ap(data_wifi_ssid_get(), data_wifi_pwd_get()) != OTA_ESP8266_EOK)
    {
        return;
    }

    if(ota_state != OTA_STATE_IDLE) return;

    if(ota_esp8266_sync_network_time(&datetime) == OTA_ESP8266_EOK)
    {
        taskENTER_CRITICAL();
        s_network_time = datetime;
        s_network_time_tick = HAL_GetTick();
        s_network_time_valid = 1U;
        taskEXIT_CRITICAL();

        s_next_time_sync = HAL_GetTick() + OTA_TIME_RESYNC_MS;
        printf("Network time synchronized: %04u-%02u-%02u %02u:%02u:%02u\r\n",
               datetime.year, datetime.month, datetime.day,
               datetime.hour, datetime.minute, datetime.second);
    }
}
void ota_esp8266_ensure_at_mode(void)
{

    /* 1. 发"+++"(不加CRLF)后保持1.5s空闲: 模块检测到透传退出条件, 退出透传 */
    ota_esp8266_uart_printf("+++");
    ota_delay_ms(1500);

    /* 2. 冲掉可能残留的"+++"半行
    模块在AT模式时+++是无效行, 会被这行AT冲掉
    而对刚退出的透传是"收尾"
    */
    ota_esp8266_uart_printf("AT\r\n");
    ota_delay_ms(200);

    /* 3.清除接收缓冲(读指针一次性追平), 避免残留应答干扰匹配 */
    U3CB.URxReadCnt = U3CB.URxWriteCnt;;

    /* 4. 关闭透传遗留的TCP连接, 否则下次AT+CIPSTART报ALREADY CONNECTED */
    (void)ota_esp8266_send_at_cmd("AT+CIPCLOSE", "OK", 800);

    /* 5. AT同步确认 */
    (void)ota_esp8266_send_at_cmd("AT", "OK", 500);
}
