#ifndef _nicdrvh_
#define _nicdrvh_

#include "ethercattype.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 仅保留核心编译需要的冗余类型占位；本移植不支持双网口冗余。 */
typedef struct ecx_redportt_t
{
    int sockhandle;
} ecx_redportt;

/* 一个 SOEM 任务独占此上下文及 g_ether0；不要跨任务并发调用 ec_/ecx_ 接口。 */
typedef struct ecx_portt_t
{
    int sockhandle;
    ec_bufT rxbuf[EC_MAXBUF];
    int rxbufstat[EC_MAXBUF];
    ec_bufT tempinbuf;
    ec_bufT txbuf[EC_MAXBUF];
    int txbuflength[EC_MAXBUF];
    int lastidx;
    /* 保留核心 ecx_init_redundant 的结构引用，初始化冗余网口仍返回失败。 */
    ec_bufT txbuf2;
    int txbuflength2;
    ecx_redportt *redport;
} ecx_portt;

extern const uint16 priMAC[3];
extern const uint16 secMAC[3];

/** @brief 向 p 指向的单帧写入 14 字节以太网头；p 至少可写 14 字节，无返回值。 */
void ec_setupheader(void *p);
/** @brief 将 port 绑定到 ifname；仅支持 ETH_SOEM_IFNAME、secondary=0。
 * @return 1 成功，0 失败。要求应用先调用 eth_init；自建 port 首次使用前须清零。
 *         不创建任务、不等待链路；拒绝冗余初始化，不占用硬件或活动上下文。
 */
int ecx_setupnic(ecx_portt *port, const char *ifname, int secondary);
/** @brief 解绑 port，释放软件接收状态；返回 0，不关闭应用管理的 GMAC 硬件。 */
int ecx_closenic(ecx_portt *port);
/** @brief 将 port 的 idx 状态设为 bufstat；非法 port/idx 忽略，无返回值。 */
void ecx_setbufstat(ecx_portt *port, int idx, int bufstat);
/** @brief 为有效 port 分配环形索引；返回 0..EC_MAXBUF-1。
 * @note SOEM 核心约定：调用者必须保证有空闲项，并在使用完后设 EC_BUF_EMPTY。
 */
int ecx_getindex(ecx_portt *port);
/** @brief 发送 port 的 idx 帧，sock 必须为 0；返回帧长，失败返回 EC_NOFRAME。 */
int ecx_outframe(ecx_portt *port, int idx, int sock);
/** @brief 核心兼容发送入口；参数和返回值同 ecx_outframe(port, idx, 0)。 */
int ecx_outframe_red(ecx_portt *port, int idx);
/** @brief 等待 port 的 idx 响应；timeout 单位微秒，0 表示只轮询一次。
 * @return WKC（包括 0），无帧/错误返回 EC_NOFRAME；按 OSAL tick 精度有界忙轮询。
 */
int ecx_waitinframe(ecx_portt *port, int idx, int timeout);
/** @brief 发送 port 的 idx 并在 timeout 微秒总期限内重试；返回 WKC 或 EC_NOFRAME。 */
int ecx_srconfirm(ecx_portt *port, int idx, int timeout);

#ifdef EC_VER1
extern ecx_portt ecx_port;
extern ecx_redportt ecx_redport;
/* 以下全局上下文入口的参数、返回值同对应 ecx_ 接口，省略 port 参数。 */
int ec_setupnic(const char *ifname, int secondary);
int ec_closenic(void);
void ec_setbufstat(int idx, int bufstat);
int ec_getindex(void);
int ec_outframe(int idx, int sock);
int ec_outframe_red(int idx);
int ec_waitinframe(int idx, int timeout);
int ec_srconfirm(int idx, int timeout);
#endif

#ifdef __cplusplus
}
#endif
#endif
