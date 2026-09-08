#include "nicdrv.h"
#include "eth.h"
#include "osal.h"
#include <string.h>

const uint16 priMAC[3] = {0x1102, 0x3322, 0x5544};
const uint16 secMAC[3] = {0x1104, 0x3322, 0x5544};

/* 单实例、单任务独占；不再经过 netif 回调、消息队列和动态互斥锁。 */
static ecx_portt *s_port;

/** @brief 从 p 读取未对齐的小端 16 位值；返回解码后的无符号整数。 */
static uint16 read_le16(const uint8 *p)
{
    return (uint16) ((uint16) p[0] | ((uint16) p[1] << 8));
}

/** @brief 检查 port 是否已绑定且 idx 有效；有效返回 1，否则返回 0。 */
static int valid_index(const ecx_portt *port, int idx)
{
    return (NULL != port) && (port == s_port) && (idx >= 0) && (idx < EC_MAXBUF);
}

/* 函数用途、参数及返回值见 nicdrv.h。 */
void ec_setupheader(void *p)
{
    uint8 *frame = p;
    if (NULL != frame)
    {
        memset(frame, 0xff, 6U);
        for (unsigned int i = 0U; i < 3U; i++)
        {
            frame[6U + 2U * i] = (uint8) priMAC[i];
            frame[7U + 2U * i] = (uint8) (priMAC[i] >> 8);
        }
        frame[12] = 0x88U;
        frame[13] = 0xa4U;
    }
}

/* 函数用途、参数及返回值见 nicdrv.h。 */
int ecx_setupnic(ecx_portt *port, const char *ifname, int secondary)
{
    gmac_link_status_t status;
    if ((NULL == port) || (NULL == ifname) || (0 != secondary) || (NULL != port->redport) ||
        (NULL != s_port) || (0 != strcmp(ifname, ETH_SOEM_IFNAME)) ||
        (FSP_SUCCESS != eth_get_link_status((uint8_t) ETH_SOEM_PORT, &status)))
    {
        return 0;
    }
    memset(port, 0, sizeof(*port));
    for (int i = 0; i < EC_MAXBUF; i++)
    {
        ec_setupheader(port->txbuf[i]);
    }
    port->sockhandle = 1;
    s_port = port;
    return 1;
}

/* 函数用途、参数及返回值见 nicdrv.h。 */
int ecx_closenic(ecx_portt *port)
{
    if ((NULL != port) && (port == s_port))
    {
        memset(port->rxbufstat, 0, sizeof(port->rxbufstat));
        port->sockhandle = 0;
        s_port = NULL;
    }
    return 0;
}

/* 函数用途、参数及返回值见 nicdrv.h。 */
void ecx_setbufstat(ecx_portt *port, int idx, int bufstat)
{
    if (valid_index(port, idx))
    {
        port->rxbufstat[idx] = bufstat;
    }
}

/* 函数用途、参数及返回值见 nicdrv.h。 */
int ecx_getindex(ecx_portt *port)
{
    int idx = port->lastidx;
    for (int count = 0; count < EC_MAXBUF; count++)
    {
        idx = (idx + 1) % EC_MAXBUF;
        if (EC_BUF_EMPTY == port->rxbufstat[idx])
        {
            break;
        }
    }
    /* 沿用 SOEM 环形索引约定，包括索引 0；上层须及时释放，不能超过 EC_MAXBUF 个在途帧。 */
    port->rxbufstat[idx] = EC_BUF_ALLOC;
    port->lastidx = idx;
    return idx;
}

/* 函数用途、参数及返回值见 nicdrv.h。 */
int ecx_outframe(ecx_portt *port, int idx, int sock)
{
    if (!valid_index(port, idx) || (0 != sock) ||
        (port->txbuflength[idx] < (int) (ETH_HEADERSIZE + EC_HEADERSIZE + EC_WKCSIZE)) ||
        (port->txbuflength[idx] > (int) ETH_FRAME_MAX_SIZE))
    {
        return EC_NOFRAME;
    }
    if (FSP_SUCCESS != eth_send_frame(port->txbuf[idx], (uint32) port->txbuflength[idx]))
    {
        port->rxbufstat[idx] = EC_BUF_ALLOC;
        return EC_NOFRAME;
    }
    port->rxbufstat[idx] = EC_BUF_TX;
    return port->txbuflength[idx];
}

/* 普通模式的 SOEM 核心也调用此入口；参数、返回值同 ecx_outframe(port, idx, 0)。 */
int ecx_outframe_red(ecx_portt *port, int idx)
{
    return ecx_outframe(port, idx, 0);
}

/**
 * @brief 轮询并缓存一帧响应；port 为已绑定上下文，无返回值。
 * 校验每个 datagram 的布局与请求相符，再允许核心按请求长度读取数据。
 */
static void receive_frame(ecx_portt *port)
{
    uint32 length = 0U;
    uint8 *frame = port->tempinbuf;
    if ((FSP_SUCCESS != eth_receive_frame(frame, sizeof(port->tempinbuf), &length)) ||
        (length < (uint32) (ETH_HEADERSIZE + EC_HEADERSIZE + EC_WKCSIZE)) ||
        (frame[12] != 0x88U) || (frame[13] != 0xa4U))
    {
        return;
    }
    const uint8 *rx = &frame[ETH_HEADERSIZE];
    uint16 header = read_le16(rx);
    uint32 payload_length = (uint32) (header & 0x07ffU) + EC_ELENGTHSIZE;
    int idx = rx[3];
    if (((header & 0xf800U) != EC_ECATTYPE) || (payload_length < EC_HEADERSIZE + EC_WKCSIZE) ||
        (payload_length > length - ETH_HEADERSIZE) || !valid_index(port, idx) ||
        (EC_BUF_TX != port->rxbufstat[idx]) ||
        (port->txbuflength[idx] != (int) (ETH_HEADERSIZE + payload_length)))
    {
        return;
    }
    const uint8 *tx = &port->txbuf[idx][ETH_HEADERSIZE];
    if (header != read_le16(tx))
    {
        return;
    }
    uint32 offset = EC_ELENGTHSIZE;
    while (offset < payload_length)
    {
        if (payload_length - offset < 12U)
        {
            return;
        }
        uint16 dlength = read_le16(&rx[offset + 6U]);
        uint32 next = offset + 12U + (uint32) (dlength & 0x07ffU);
        if ((next > payload_length) || (rx[offset] != tx[offset]) ||
            (rx[offset + 1U] != tx[offset + 1U]) ||
            ((dlength & 0x87ffU) != (read_le16(&tx[offset + 6U]) & 0x87ffU)) ||
            (((dlength & 0x8000U) != 0U) != (next < payload_length)))
        {
            return;
        }
        offset = next;
    }
    /* 以索引保存乱序返回的帧；rxbuf 从 EtherCAT 帧头开始，不含以太网头。 */
    memcpy(port->rxbuf[idx], rx, payload_length);
    port->rxbufstat[idx] = EC_BUF_RCVD;
}

/* 函数用途、参数及返回值见 nicdrv.h。 */
int ecx_waitinframe(ecx_portt *port, int idx, int timeout)
{
    osal_timert timer;
    if (!valid_index(port, idx) || (timeout < 0))
    {
        return EC_NOFRAME;
    }
    osal_timer_start(&timer, (uint32) timeout);
    do
    {
        if (EC_BUF_RCVD != port->rxbufstat[idx])
        {
            receive_frame(port);
        }
        if (EC_BUF_RCVD == port->rxbufstat[idx])
        {
            uint16 offset = read_le16(port->rxbuf[idx]) & 0x07ffU;
            port->rxbufstat[idx] = EC_BUF_COMPLETE;
            /* 返回末尾 datagram 的 WKC；WKC=0 也是有效响应，不能与未收到帧混淆。 */
            return (int) read_le16(&port->rxbuf[idx][offset]);
        }
    } while (!osal_timer_is_expired(&timer));
    return EC_NOFRAME;
}

/* 函数用途、参数及返回值见 nicdrv.h。 */
int ecx_srconfirm(ecx_portt *port, int idx, int timeout)
{
    osal_timert total_timer;
    osal_timert retry_timer;
    if (!valid_index(port, idx) || (timeout < 0))
    {
        return EC_NOFRAME;
    }
    osal_timer_start(&total_timer, (uint32) timeout);
    if (ecx_outframe(port, idx, 0) < 0)
    {
        return EC_NOFRAME;
    }
    osal_timer_start(&retry_timer, EC_TIMEOUTRET);
    do
    {
        /* 单次只读一次，所有重试共用总期限，避免每次重试重新消耗完整 timeout。 */
        int wkc = ecx_waitinframe(port, idx, 0);
        if (wkc >= 0)
        {
            return wkc;
        }
        if (osal_timer_is_expired(&total_timer))
        {
            break;
        }
        if (osal_timer_is_expired(&retry_timer))
        {
            if (ecx_outframe(port, idx, 0) < 0)
            {
                return EC_NOFRAME;
            }
            osal_timer_start(&retry_timer, EC_TIMEOUTRET);
        }
    } while (1);
    return EC_NOFRAME;
}

#ifdef EC_VER1
/* 全局上下文兼容入口：参数、返回值同对应 ecx_ 接口，port 固定为 &ecx_port。 */
int ec_setupnic(const char *ifname, int secondary) { return ecx_setupnic(&ecx_port, ifname, secondary); }
int ec_closenic(void) { return ecx_closenic(&ecx_port); }
void ec_setbufstat(int idx, int bufstat) { ecx_setbufstat(&ecx_port, idx, bufstat); }
int ec_getindex(void) { return ecx_getindex(&ecx_port); }
int ec_outframe(int idx, int sock) { return ecx_outframe(&ecx_port, idx, sock); }
int ec_outframe_red(int idx) { return ecx_outframe_red(&ecx_port, idx); }
int ec_waitinframe(int idx, int timeout) { return ecx_waitinframe(&ecx_port, idx, timeout); }
int ec_srconfirm(int idx, int timeout) { return ecx_srconfirm(&ecx_port, idx, timeout); }
#endif
