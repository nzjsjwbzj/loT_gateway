/*******************************************************************************
 * MQTT Transport Layer - ESP8266 Transparent Transmission Mode Implementation
 *
 * 基于ESP8266的透传模式实现MQTT传输层接口。负责将MQTT协�??包通过ESP8266发送到MQTT服务器�?
 *******************************************************************************/

#include "transport.h"
#include "atk_mw8266d.h"
#include "atk_mw8266d_uart.h"
#include "delay.h"
#include <string.h>

// 引用外部UART句柄
extern UART_HandleTypeDef g_uart_handle;

/* ESP8266使用的虚拟socket ID */
#define MQTT_ESP8266_SOCK_ID  1

/* 透传模式标志�? */
static uint8_t transport_transparent_mode = 0;
/* 接收缓冲区偏移量，用于�?�理分包数据 */
static uint16_t transport_rx_offset = 0;

/**
 * @brief  通过ESP8266发送MQTT数据�?
 * @param  sock: socket ID（�?��?�未使用，兼容性参数）
 * @param  buf:  要发送的数据缓冲�?
 * @param  buflen: 数据长度
 * @retval 发送成功返回发送的字节数，失败返回-1
 */
int transport_sendPacketBuffer(int sock, unsigned char* buf, int buflen)
{
	(void)sock;
	
	if (buf == NULL || buflen <= 0)
		return -1;
	
	if (!transport_transparent_mode)
		return -1;
	
	// 在透传模式下，直接通过UART发送数�?
	// 使用HAL_UART_Transmit阻�?�发�?
	HAL_StatusTypeDef status = HAL_UART_Transmit(&g_uart_handle, buf, (uint16_t)buflen, 1000);
	if (status == HAL_OK)
		return buflen;
	return -1;
}

/**
 * @brief  从ESP8266接收MQTT数据包（阻�?�模式）
 * @param  buf:   接收缓冲�?
 * @param  count: 需要接收的字节�?
 * @retval 接收成功返回实际接收的字节数，超时返�?0，错�?返回-1
 */
int transport_getdata(unsigned char* buf, int count)
{
	uint16_t received = 0;
	uint8_t *rx_buf;
	uint32_t timeout = 5000; // 5秒超�?
	
	if (buf == NULL || count <= 0)
		return -1;
	
	if (!transport_transparent_mode)
		return -1;
	
	// �?�?接收直到满足请求的字节数或超�?
	while (timeout > 0 && received < count)
	{
		rx_buf = atk_mw8266d_uart_rx_get_frame();
		if (rx_buf != NULL)
		{
			uint16_t frame_len = atk_mw8266d_uart_rx_get_frame_len();
            
            // 如果偏移量超过帧长度，�?�明当前帧已处理完，重启接收
            if (transport_rx_offset >= frame_len)
            {
                atk_mw8266d_uart_rx_restart();
                transport_rx_offset = 0;
                // 继续等待下一�?
                continue;
            }

            uint16_t available = frame_len - transport_rx_offset;
			uint16_t needed = count - received;
			uint16_t copy_len = (available < needed) ? available : needed;
			
			memcpy(buf + received, rx_buf + transport_rx_offset, copy_len);
			received += copy_len;
            transport_rx_offset += copy_len;
			
			// 如果当前帧数�?已全部取走，重启接收以释放缓冲区
			if (transport_rx_offset >= frame_len)
			{
				atk_mw8266d_uart_rx_restart();
                transport_rx_offset = 0;
			}
			
			if (received >= count)
				break;
		}
		delay_ms(1);
		timeout--;
	}
	
	if (received > 0)
		return received;
	if (timeout == 0)
		return 0; // 超时
	return -1;
}

/**
 * @brief  非阻塞方式从ESP8266接收数据
 * @param  sck:   socket描述符（�?使用�?
 * @param  buf:   接收缓冲�?
 * @param  count: 最大接收字节数
 * @retval 返回实际接收到的字节数，无数�?返回0
 */
int transport_getdatanb(void *sck, unsigned char* buf, int count)
{
	uint8_t *rx_buf;
	uint16_t frame_len;
	
	(void)sck;
	
	if (buf == NULL || count <= 0)
		return 0;
	
	if (!transport_transparent_mode)
		return 0;
	
	// 获取当前接收�?
	rx_buf = atk_mw8266d_uart_rx_get_frame();
	if (rx_buf != NULL)
	{
		frame_len = atk_mw8266d_uart_rx_get_frame_len();
        
        if (transport_rx_offset >= frame_len)
        {
            atk_mw8266d_uart_rx_restart();
            transport_rx_offset = 0;
            return 0; 
        }

        uint16_t available = frame_len - transport_rx_offset;
		uint16_t copy_len = (available < count) ? available : count;
		
		memcpy(buf, rx_buf + transport_rx_offset, copy_len);
        transport_rx_offset += copy_len;
		
		// 如果当前帧数�?已全部取走，重启接收
		if (transport_rx_offset >= frame_len)
		{
			atk_mw8266d_uart_rx_restart();
            transport_rx_offset = 0;
		}
		
		return copy_len;
	}
	
	return 0;
}

/**
 * @brief  打开ESP8266 TCP连接并进入透传模式
 * @param  addr: MQTT服务器IP地址
 * @param  port: MQTT服务器�??口（通常�?1883�?
 * @retval >=0 成功返回socket ID�?<0 失败
 */
int transport_open(char* addr, int port)
{
	char port_str[8];
	
	if (addr == NULL)
		return -1;
	
	// �?换�??口号为字符串
	sprintf(port_str, "%d", port);
	
	// 1. 建立TCP连接到MQTT服务�?
	if (atk_mw8266d_connect_tcp_server(addr, port_str) != ATK_MW8266D_EOK)
	{
		return -1;
	}
	
	delay_ms(100); // 等待连接稳定
	

	
	// 2. 进入透传模式
	if (atk_mw8266d_enter_unvarnished() != ATK_MW8266D_EOK)
	{
		return -1;
	}
	
	delay_ms(100); // 等待模式切换
	
	transport_transparent_mode = 1;
    
    // 进入透传模式后，清除�?能存在的残留数据（例�? ">" 响应后的回车换�?�等�?
    // �?保后�?接收到的�?�?净�? MQTT 数据
    atk_mw8266d_uart_rx_restart();
    transport_rx_offset = 0;
	
	return MQTT_ESP8266_SOCK_ID;
}

/**
 * @brief  关闭连接并退出透传模式
 * @param  sock: socket ID（未使用�?
 * @retval 0 总是返回0
 */
int transport_close(int sock)
{
	(void)sock;
	
	if (transport_transparent_mode)
	{
		// 退出透传模式
		atk_mw8266d_exit_unvarnished();
		delay_ms(100);
		
		// 关闭TCP连接
		atk_mw8266d_send_at_cmd("AT+CIPCLOSE", "OK", 1000);
		
		transport_transparent_mode = 0;
        transport_rx_offset = 0;
	}
	
	return 0;
}
