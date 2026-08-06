#include "ota.h"
#include "sys.h"
#include "delay.h"
#include "transport.h"
#include "cJSON.h"
#include "md5.h"
#include "flash.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

// 此结构体用于暂存从云端获取到的升级包信息
typedef struct
{
    char tid[48];         // 升级任务ID (Task ID)，OneNET 用它来唯一标识某次任务
    char target[24];      // 目标固件版本号 (比如 "V5.8")
    char md5_hex[33];     // 服务器下发的 MD5 校验码 (字符串形式)
    uint8_t md5_bin[16];  // MD5 校验码 (由字符串转成的二进制数组形式，方便下载完后按位对比)
    uint32_t size;        // 固件压缩包的总字节数大小
} OtaPackageInfo;

// -------------------------------------------------------------------------
// OTA 运行过程中使用的大型全局缓存
// 这里全部加了 static，意味着它们被存放在单片机的全局 RAM 区（堆区）
// 这样做是为了避免巨大的数组占用 FreeRTOS 任务宝贵且有限的局部栈空间，防止死机
// -------------------------------------------------------------------------
static char g_ota_tid[48] = {0};           // 当前正在处理的任务号
static char g_ota_auth[256];               // 用于拼装最终合法的 Authorization 头部字符串
static char g_ota_req[768];                // 用于组装 HTTP 请求报文 (GET / POST)
static uint8_t g_ota_resp[1600];           // 用于接收服务器返回的 HTTP 响应报文 (包含报文头部和报文体数据)
static char g_ota_json[1024];              // 专门用来暂存并使用 cJSON 解析服务器返回的 JSON 字符串内容
static char g_ota_body[128];               // 临时拼装向云端 POST 过去的小型 JSON 载体 (如进度上报数据)
static uint8_t g_ota_read_buf[512];        // 专门给 MD5 校验时，从 Flash 中倒抽数据出来的读缓存区
static char g_sign_string[192];            // 签名的原始组合字符串 ("String to Sign")
static char g_sign_res_enc[96];            // URL 编码后的 Resource 参数
static char g_sign_b64[64];                // Base64 加密以后的签名缓冲区
static char g_sign_enc[128];               // URL 编码后的最终 Signature 缓冲区
static uint8_t g_sign_key_raw[96];         // Base64 解码原始 Access Key 后的密钥数组
static uint32_t g_ota_unix_now_base = OTA_AUTH_UNIX_NOW_BASE; // 全局系统时间基准 (若服务器反馈失效，会自动校准此时间)

// 下面的结构体及函数是标准 SHA1 加密和 Base64 解解码算法，属于通用数学逻辑，无需深究
typedef struct
{
    uint8_t data[64];
    uint32_t datalen;
    uint64_t bitlen;
    uint32_t state[5];
} Sha1Ctx;

static int ota_hex_to_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return (c - 'a') + 10;
    if (c >= 'A' && c <= 'F') return (c - 'A') + 10;
    return -1;
}

static int ota_md5_hex_to_bin(const char *hex, uint8_t out[16])
{
    if (hex == NULL || out == NULL) return 0;
    for (int i = 0; i < 16; i++)
    {
        int hi = ota_hex_to_nibble(hex[i * 2]);
        int lo = ota_hex_to_nibble(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return 0;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return 1;
}

static void ota_md5_bin_to_hex(const uint8_t in[16], char out[33])
{
    static const char *tbl = "0123456789abcdef";
    for (int i = 0; i < 16; i++)
    {
        out[i * 2] = tbl[(in[i] >> 4) & 0x0F];
        out[i * 2 + 1] = tbl[in[i] & 0x0F];
    }
    out[32] = '\0';
}

static uint32_t sha1_rotl(uint32_t v, uint32_t n)
{
    return (v << n) | (v >> (32U - n));
}

static void sha1_transform(Sha1Ctx *ctx, const uint8_t data[64])
{
    uint32_t w[80];
    for (int i = 0; i < 16; i++)
    {
        w[i] = ((uint32_t)data[i * 4] << 24) |
               ((uint32_t)data[i * 4 + 1] << 16) |
               ((uint32_t)data[i * 4 + 2] << 8) |
               (uint32_t)data[i * 4 + 3];
    }
    for (int i = 16; i < 80; i++)
    {
        w[i] = sha1_rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }

    uint32_t a = ctx->state[0];
    uint32_t b = ctx->state[1];
    uint32_t c = ctx->state[2];
    uint32_t d = ctx->state[3];
    uint32_t e = ctx->state[4];

    for (int i = 0; i < 80; i++)
    {
        uint32_t f;
        uint32_t k;
        if (i < 20)
        {
            f = (b & c) | ((~b) & d);
            k = 0x5A827999;
        }
        else if (i < 40)
        {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1;
        }
        else if (i < 60)
        {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDC;
        }
        else
        {
            f = b ^ c ^ d;
            k = 0xCA62C1D6;
        }
        uint32_t t = sha1_rotl(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = sha1_rotl(b, 30);
        b = a;
        a = t;
    }

    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
    ctx->state[4] += e;
}

static void sha1_init(Sha1Ctx *ctx)
{
    memset(ctx, 0, sizeof(Sha1Ctx));
    ctx->state[0] = 0x67452301;
    ctx->state[1] = 0xEFCDAB89;
    ctx->state[2] = 0x98BADCFE;
    ctx->state[3] = 0x10325476;
    ctx->state[4] = 0xC3D2E1F0;
}

static void sha1_update(Sha1Ctx *ctx, const uint8_t *data, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++)
    {
        ctx->data[ctx->datalen++] = data[i];
        if (ctx->datalen == 64U)
        {
            sha1_transform(ctx, ctx->data);
            ctx->bitlen += 512U;
            ctx->datalen = 0;
        }
    }
}

static void sha1_final(Sha1Ctx *ctx, uint8_t hash[20])
{
    uint32_t i = ctx->datalen;
    ctx->data[i++] = 0x80;
    if (i > 56U)
    {
        while (i < 64U) ctx->data[i++] = 0;
        sha1_transform(ctx, ctx->data);
        i = 0;
    }
    while (i < 56U) ctx->data[i++] = 0;
    ctx->bitlen += (uint64_t)ctx->datalen * 8ULL;
    ctx->data[56] = (uint8_t)(ctx->bitlen >> 56);
    ctx->data[57] = (uint8_t)(ctx->bitlen >> 48);
    ctx->data[58] = (uint8_t)(ctx->bitlen >> 40);
    ctx->data[59] = (uint8_t)(ctx->bitlen >> 32);
    ctx->data[60] = (uint8_t)(ctx->bitlen >> 24);
    ctx->data[61] = (uint8_t)(ctx->bitlen >> 16);
    ctx->data[62] = (uint8_t)(ctx->bitlen >> 8);
    ctx->data[63] = (uint8_t)(ctx->bitlen);
    sha1_transform(ctx, ctx->data);

    for (i = 0; i < 5; i++)
    {
        hash[i * 4] = (uint8_t)(ctx->state[i] >> 24);
        hash[i * 4 + 1] = (uint8_t)(ctx->state[i] >> 16);
        hash[i * 4 + 2] = (uint8_t)(ctx->state[i] >> 8);
        hash[i * 4 + 3] = (uint8_t)(ctx->state[i]);
    }
}

static void hmac_sha1(const uint8_t *key, uint32_t key_len, const uint8_t *msg, uint32_t msg_len, uint8_t out[20])
{
    uint8_t k_ipad[64];
    uint8_t k_opad[64];
    uint8_t tk[20];
    memset(k_ipad, 0, sizeof(k_ipad));
    memset(k_opad, 0, sizeof(k_opad));

    if (key_len > 64U)
    {
        Sha1Ctx tctx;
        sha1_init(&tctx);
        sha1_update(&tctx, key, key_len);
        sha1_final(&tctx, tk);
        key = tk;
        key_len = 20U;
    }

    memcpy(k_ipad, key, key_len);
    memcpy(k_opad, key, key_len);
    for (uint32_t i = 0; i < 64U; i++)
    {
        k_ipad[i] ^= 0x36;
        k_opad[i] ^= 0x5C;
    }

    uint8_t inner[20];
    Sha1Ctx ctx;
    sha1_init(&ctx);
    sha1_update(&ctx, k_ipad, 64U);
    sha1_update(&ctx, msg, msg_len);
    sha1_final(&ctx, inner);

    sha1_init(&ctx);
    sha1_update(&ctx, k_opad, 64U);
    sha1_update(&ctx, inner, 20U);
    sha1_final(&ctx, out);
}

static int b64_index(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static int base64_decode_bytes(const char *in, uint8_t *out, uint32_t out_cap, uint32_t *out_len)
{
    uint32_t len = (uint32_t)strlen(in);
    uint32_t i = 0;
    uint32_t o = 0;
    while (i < len)
    {
        int a = -1, b = -1, c = -2, d = -2;
        while (i < len && a < 0) a = b64_index(in[i++]);
        while (i < len && b < 0) b = b64_index(in[i++]);
        if (a < 0 || b < 0) break;
        while (i < len && c == -2)
        {
            if (in[i] == '=') { c = -1; i++; break; }
            c = b64_index(in[i++]);
        }
        while (i < len && d == -2)
        {
            if (in[i] == '=') { d = -1; i++; break; }
            d = b64_index(in[i++]);
        }
        if (o + 1U > out_cap) return 0;
        out[o++] = (uint8_t)((a << 2) | (b >> 4));
        if (c >= 0)
        {
            if (o + 1U > out_cap) return 0;
            out[o++] = (uint8_t)(((b & 0x0F) << 4) | (c >> 2));
            if (d >= 0)
            {
                if (o + 1U > out_cap) return 0;
                out[o++] = (uint8_t)(((c & 0x03) << 6) | d);
            }
        }
        if (c < 0 || d < 0) break;
    }
    *out_len = o;
    return 1;
}

static int base64_encode_bytes(const uint8_t *in, uint32_t in_len, char *out, uint32_t out_cap)
{
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    uint32_t olen = ((in_len + 2U) / 3U) * 4U;
    if (out_cap <= olen) return 0;
    uint32_t i = 0;
    uint32_t o = 0;
    while ((i + 3U) <= in_len)
    {
        uint32_t a = in[i++];
        uint32_t b = in[i++];
        uint32_t c = in[i++];
        out[o++] = tbl[(a >> 2) & 0x3F];
        out[o++] = tbl[((a & 0x03) << 4) | ((b >> 4) & 0x0F)];
        out[o++] = tbl[((b & 0x0F) << 2) | ((c >> 6) & 0x03)];
        out[o++] = tbl[c & 0x3F];
    }
    uint32_t rem = in_len - i;
    if (rem == 1U)
    {
        uint32_t a = in[i];
        out[o++] = tbl[(a >> 2) & 0x3F];
        out[o++] = tbl[(a & 0x03) << 4];
        out[o++] = '=';
        out[o++] = '=';
    }
    else if (rem == 2U)
    {
        uint32_t a = in[i++];
        uint32_t b = in[i];
        out[o++] = tbl[(a >> 2) & 0x3F];
        out[o++] = tbl[((a & 0x03) << 4) | ((b >> 4) & 0x0F)];
        out[o++] = tbl[(b & 0x0F) << 2];
        out[o++] = '=';
    }
    out[o] = '\0';
    return 1;
}

static int url_encode_ascii(const char *in, char *out, uint32_t out_cap)
{
    static const char hex[] = "0123456789ABCDEF";
    uint32_t o = 0;
    for (uint32_t i = 0; in[i] != '\0'; i++)
    {
        uint8_t c = (uint8_t)in[i];
        uint8_t keep = ((c >= 'A' && c <= 'Z') ||
                        (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') ||
                        c == '-' || c == '_' || c == '.' || c == '~');
        if (keep)
        {
            if (o + 1U >= out_cap) return 0;
            out[o++] = (char)c;
        }
        else
        {
            if (o + 3U >= out_cap) return 0;
            out[o++] = '%';
            out[o++] = hex[(c >> 4) & 0x0F];
            out[o++] = hex[c & 0x0F];
        }
    }
    if (o >= out_cap) return 0;
    out[o] = '\0';
    return 1;
}

// =========================================================================
// 函数：构建 OneNET 要求的动态鉴权 Token
// 作用：根据当前系统时间戳、签名方法、设备版和 Key，通过 HMAC-SHA1 加密算出鉴权字符串。
// 将在随后几乎每一次 HTTP 请求头部 (Authorization: ) 中使用，证明单片机的合法身份。
// =========================================================================
static int ota_build_auth_header(const char *version, const char *res_raw, char *out, uint32_t out_cap)
{
    uint32_t key_len = 0;
    uint8_t digest[20];
    char method_enc[24];
    char version_enc[32];
    char et_text[24];
    char et_enc[32];
    uint32_t now_unix = g_ota_unix_now_base + (HAL_GetTick() / 1000UL);
    uint32_t et = now_unix + OTA_AUTH_ET_TTL_SEC;

    int sig_len = snprintf(g_sign_string, sizeof(g_sign_string), "%lu\n%s\n%s\n%s",
                           (unsigned long)et, OTA_AUTH_METHOD, res_raw, version);
    if (sig_len <= 0 || sig_len >= (int)sizeof(g_sign_string)) return 0;
    if (!base64_decode_bytes(OTA_ACCESS_KEY_B64, g_sign_key_raw, sizeof(g_sign_key_raw), &key_len))
    {
        printf("OTA auth: b64 decode fail\r\n");
        return 0;
    }
    hmac_sha1(g_sign_key_raw, key_len, (const uint8_t *)g_sign_string, (uint32_t)sig_len, digest);
    if (!base64_encode_bytes(digest, sizeof(digest), g_sign_b64, sizeof(g_sign_b64))) return 0;
    if (!url_encode_ascii(g_sign_b64, g_sign_enc, sizeof(g_sign_enc))) return 0;
    if (!url_encode_ascii(res_raw, g_sign_res_enc, sizeof(g_sign_res_enc))) return 0;
    if (!url_encode_ascii(OTA_AUTH_METHOD, method_enc, sizeof(method_enc))) return 0;
    if (!url_encode_ascii(version, version_enc, sizeof(version_enc))) return 0;
    snprintf(et_text, sizeof(et_text), "%lu", (unsigned long)et);
    if (!url_encode_ascii(et_text, et_enc, sizeof(et_enc))) return 0;

    int auth_len = snprintf(out, out_cap, "version=%s&res=%s&et=%s&method=%s&sign=%s",
                            version_enc, g_sign_res_enc, et_enc, method_enc, g_sign_enc);
    if (auth_len <= 0 || auth_len >= (int)out_cap) return 0;
    printf("OTA auth: now=%lu et=%lu ttl=%lu key_len=%lu\r\n",
           (unsigned long)now_unix, (unsigned long)et, (unsigned long)OTA_AUTH_ET_TTL_SEC, (unsigned long)key_len);
    printf("OTA auth: sfs=%s\r\n", g_sign_string);
    printf("OTA auth: token=%s\r\n", out);
    return 1;
}

static int ota_http_status_code(const uint8_t *resp, uint32_t len)
{
    if (resp == NULL || len < 12) return -1;
    for (uint32_t i = 0; i + 11 < len; i++)
    {
        if (resp[i] == 'H' && resp[i + 1] == 'T' && resp[i + 2] == 'T' && resp[i + 3] == 'P' && resp[i + 4] == '/')
        {
            for (uint32_t j = i; j < len; j++)
            {
                if (resp[j] == ' ')
                {
                    if (j + 3 < len)
                    {
                        return atoi((const char *)&resp[j + 1]);
                    }
                    return -1;
                }
                if (resp[j] == '\n') break;
            }
            break;
        }
    }
    return -1;
}

static int ota_http_body(const uint8_t *resp, uint32_t len, const uint8_t **body, uint32_t *body_len)
{
    if (resp == NULL || body == NULL || body_len == NULL) return 0;
    for (uint32_t i = 0; i + 3 < len; i++)
    {
        if (resp[i] == '\r' && resp[i + 1] == '\n' && resp[i + 2] == '\r' && resp[i + 3] == '\n')
        {
            *body = &resp[i + 4];
            *body_len = len - (i + 4);
            return 1;
        }
    }
    return 0;
}

// =========================================================================
// 函数：与 OneNET FUSE 服务器建立 TCP，发送 HTTP 请求并接收响应
// 参数说明：
//   host       - 服务器域名 (如 "iot-api.heclouds.com")
//   port       - 服务器端口 (如 80)
//   req        - 已拼装好的 HTTP 请求报文 (GET / POST / HEAD 等)
//   req_len    - 请求报文的总字节数
//   resp       - 用来存放服务器返回数据的缓冲区 (包含 HTTP 报文头和报文体)
//   resp_cap   - 缓冲区的最大容量，防止溢出
//   timeout_ms - 网络操作的总超时时限 (毫秒)
// 返回值：
//   成功返回收到的总字节数 (>0)
//   失败返回 -1
// 核心逻辑：
//   1. 打开 TCP Socket 连接
//   2. 一口气发送整个 HTTP 请求报文
//   3. 轮询接收服务器数据，直到没有新数据来临超过 300ms 或缓冲满
//   4. 关闭连接并返回已接收的总数
// =========================================================================
static int ota_http_request(const char *host, uint16_t port, const uint8_t *req, uint32_t req_len, uint8_t *resp, uint32_t resp_cap, uint32_t timeout_ms)
{
    // 参数安全检查
    if (host == NULL || req == NULL || resp == NULL || resp_cap == 0) return -1;
    
    // 调用底层 ESP8266 驱动，打开指定主机和端口的 Socket 连接
    int sock = transport_open((char *)host, port);
    if (sock < 0) return -1;
    
    // 将整个 HTTP 请求报文通过 UART 透传模式一口气砸给 ESP8266
    // 如果发送的字节数与实际请求长度不符，说明网络异常或缓存不足，直接返回失败
    if (transport_sendPacketBuffer(sock, (unsigned char *)req, req_len) != (int)req_len)
    {
        transport_close(sock);
        return -1;
    }
    
    // 初始化接收循环的控制变量
    uint32_t used = 0;                    // 已经接收到的总字节数
    uint32_t start = HAL_GetTick();       // 记录发送请求时的系统时刻，用来计算超时
    uint32_t last_rx = start;             // 记录最后一次收到数据的时刻，用来检测数据流停止
    int saw_http = 0;                     // 标志位：是否已经看到合法的 HTTP 状态码
    
    // 轮询接收来自服务器的 HTTP 响应报文
    while ((HAL_GetTick() - start) < timeout_ms)
    {
        // 如果接收缓冲满了，立即停止接收
        if (used >= resp_cap) break;
        
        // 非阻塞读取：从 ESP8266 透传通道尽可能多地拉出数据 (不堵塞等待)
        // 如果网络还有数据，n > 0；如果暂时没数据，n = 0；如果出错，n < 0
        int n = transport_getdatanb(NULL, resp + used, (int)(resp_cap - used));
        
        if (n > 0)
        {
            // 成功收到新数据
            used += (uint32_t)n;                         // 更新已接收的总字节数
            last_rx = HAL_GetTick();                     // 更新"最后收到数据的时刻"
            
            // 检查是否已经看到 HTTP 状态行 (如 "HTTP/1.1 200 OK")
            // 一旦看到合法的状态码，标记 saw_http=1 (用于后续的调试打印)
            if (!saw_http && ota_http_status_code(resp, used) > 0) saw_http = 1;
            continue;
        }
        
        // n <= 0，说明暂时没新数据。此时检查是否超过了"数据流停止等待"的上限
        // 如果已经收到了至少一些数据，但已经 300ms 没有新数据流入，认为服务器已关闭连接或停止推送
        if (used > 0 && (HAL_GetTick() - last_rx) > 300U)
        {
            // 数据流停止，立即退出接收循环
            break;
        }
        
        // 如果既没收到新数据，也没达到停止条件，就休眠 10ms 让出 CPU，避免忙轮询吃 CPU
        delay_ms(10);
    }
    
    // 调试打印：如果接收到了数据但没找到 HTTP 状态码，说明可能是垃圾数据或网络问题
    if (used > 0 && ota_http_status_code(resp, used) < 0)
    {
        uint32_t dump = used;
        if (dump > 120U) dump = 120U;                    // 最多打印前 120 字节，防止串口过载
        printf("OTA http: no status host=%s used=%lu dump=", host, (unsigned long)used);
        for (uint32_t i = 0; i < dump; i++)
        {
            uint8_t c = resp[i];
            // 只打印可见 ASCII 字符，其他字节打印"."表示
            if (c >= 32U && c <= 126U) printf("%c", c);
            else printf(".");
        }
        printf("\r\n");
    }
    
    // 关闭 Socket 连接，释放网络资源
    transport_close(sock);
    
    // 返回成功接收的字节总数
    return (int)used;
}

static int ota_json_is_success(const uint8_t *body, uint32_t body_len)
{
    if (body == NULL || body_len == 0) return 0;
    
    // 把服务器发来的二进制 Body 截断并转成普通字符串，防溢出
    uint32_t copy_len = body_len;
    if (copy_len >= sizeof(g_ota_json)) copy_len = sizeof(g_ota_json) - 1U;
    memcpy(g_ota_json, body, copy_len);
    g_ota_json[copy_len] = '\0';
    
    // 调用 cJSON 解析字符串
    cJSON *root = cJSON_Parse(g_ota_json);
    if (root == NULL) return 0;
    
    int ok = 0;
    // 检查 "errno" 字段是否为 0
    cJSON *errno_node = cJSON_GetObjectItemCaseSensitive(root, "errno");
    if (cJSON_IsNumber(errno_node) && errno_node->valueint == 0) ok = 1;
    
    // 检查 "code" 字段是否为 0 (不同接口可能用的字段名不太一样)
    cJSON *code_node = cJSON_GetObjectItemCaseSensitive(root, "code");
    if (cJSON_IsNumber(code_node) && code_node->valueint == 0) ok = 1;
    
    // 一定要释放用完的 JSON 树，否则会发生严重的内存泄漏，把单片机的堆区吃光
    cJSON_Delete(root);
    return ok;
}

// =========================================================================
// 函数：安全地从 JSON 节点中拷贝任务编号 (tid)
// 逻辑：兼容云端可能是字符串格式 ("12345") 或者是纯数字格式 (12345) 的 tid
// =========================================================================
static int ota_json_copy_tid(cJSON *tid_node, char *out, uint32_t out_cap)
{
    if (out == NULL || out_cap == 0U) return 0;
    out[0] = '\0';
    
    // 分支 1：如果云端发来的是标准的字符串包裹名字，直接安全拷贝
    if (cJSON_IsString(tid_node) && tid_node->valuestring != NULL)
    {
        strncpy(out, tid_node->valuestring, out_cap - 1U);
        out[out_cap - 1U] = '\0';
        return out[0] != '\0';
    }
    
    // 分支 2：如果云端发来的是数字，先转成字符串再塞到缓冲区里
    if (cJSON_IsNumber(tid_node))
    {
        int n = snprintf(out, out_cap, "%d", tid_node->valueint);
        return (n > 0 && n < (int)out_cap);
    }
    return 0;
}

// =========================================================================
// 函数：专用于提取服务器返回的确切错误代码 code
// 逻辑：在报错时可以拿到 code，用来判断具体的出错原因（比如 10403 是时间鉴权过期报错）
// =========================================================================
static int ota_json_get_code(const uint8_t *body, uint32_t body_len, int *code_out)
{
    if (body == NULL || body_len == 0 || code_out == NULL) return 0;
    
    // 字符串化处理
    uint32_t copy_len = body_len;
    if (copy_len >= sizeof(g_ota_json)) copy_len = sizeof(g_ota_json) - 1U;
    memcpy(g_ota_json, body, copy_len);
    g_ota_json[copy_len] = '\0';
    
    cJSON *root = cJSON_Parse(g_ota_json);
    if (root == NULL) return 0;
    
    int ok = 0;
    cJSON *code_node = cJSON_GetObjectItemCaseSensitive(root, "code");
    if (cJSON_IsNumber(code_node))
    {
        *code_out = code_node->valueint; // 把错误码通过指针带回外面
        ok = 1;
    }
    
    cJSON_Delete(root);
    return ok;
}

// =========================================================================
// 函数：极具鲁棒性的容错机制 —— 云端时间自动同步
// 如果我们算出的 HMAC 鉴权 Token 发给 FUSE 后端，后端认为已过期返回错误码 10403，
// 服务端的报错 JSON 里通常会附带类似于 "now=1680000000" 的提示字样。
// 这个函数就是暴力从文本里抠出这串真正的云端时间，修正设备本地的时间基准 g_ota_unix_now_base，
// 这样在下一次重试时，算出来的鉴权码就绝对没问题了！
// =========================================================================
static int ota_try_sync_unix_base_from_text(const char *text)
{
    if (text == NULL) return 0;
    
    // 暴力搜索 "now=" 这个子串
    const char *pos = strstr(text, "now=");
    if (pos == NULL) return 0;
    
    pos += 4; // 跳过 "now=" 四个字母
    uint32_t now = 0;
    int has_digit = 0;
    
    // 不断把字符数字转成真正的整数 (类似 atoi 的原理)
    while (*pos >= '0' && *pos <= '9')
    {
        has_digit = 1;
        now = now * 10U + (uint32_t)(*pos - '0');
        pos++;
    }
    
    // 至少10位数字(10亿)，对应的时间远超出2001年，肯定是合法的 Unix Timestamp
    if (!has_digit || now < 1000000000UL) return 0;
    
    // 计算当前开机以后的总秒数
    uint32_t tick_s = HAL_GetTick() / 1000UL;
    
    // 新的基准 = 云端真实时间 - 系统开机走过的秒数
    if (now > tick_s)
    {
        g_ota_unix_now_base = now - tick_s;
        printf("OTA auth: sync server now=%lu tick=%lu new_base=%lu\r\n",
               (unsigned long)now, (unsigned long)tick_s, (unsigned long)g_ota_unix_now_base);
        return 1;
    }
    return 0;
}

// =========================================================================
// 函数：给外部提供设置任务 ID 的接口
// =========================================================================
void OTA_SetTaskId(const char *tid)
{
    if (tid == NULL || tid[0] == '\0')
    {
        g_ota_tid[0] = '\0';
        return;
    }
    strncpy(g_ota_tid, tid, sizeof(g_ota_tid) - 1U);
    g_ota_tid[sizeof(g_ota_tid) - 1U] = '\0';
}

// =========================================================================
// 函数：主动向云端 FUSE 平台上报当前的设备版本号
// 逻辑：向 /status 接口发送形如 {"s_version":"V5.7","f_version":"V5.7"} 的数据。
// 这个动作非常关键。如果你这里汇报的 V5.7 低于你在平台上创建升级任务填的目标版本(比如V5.8)，
// 云端就会在随后的查询中判定“恩，这台设备很老，需要给它派发新包裹”。
// 失败处理：如果返回 Token 失效或时间越界(10403错误)，会自动从 HTTP 反馈抽出时间并同步重试！
// =========================================================================
static int ota_report_version(void)
{
    for (int attempt = 0; attempt < 2; attempt++)
    {
        printf("OTA version: build auth\r\n");
        if (!ota_build_auth_header(OTA_AUTH_FUSE_VER, OTA_AUTH_FUSE_RES_RAW, g_ota_auth, sizeof(g_ota_auth)))
        {
            printf("OTA version: auth fail\r\n");
            return 0;
        }
        snprintf(g_ota_body, sizeof(g_ota_body), "{\"s_version\":\"%s\",\"f_version\":\"%s\"}", OTA_CURRENT_VERSION, OTA_CURRENT_VERSION);
        int req_len = snprintf(g_ota_req, sizeof(g_ota_req),
                               "POST /fuse-ota/%s/%s/version HTTP/1.1\r\n"
                               "Host: %s\r\n"
                               "Authorization: %s\r\n"
                               "Content-Type: application/json\r\n"
                               "Connection: close\r\n"
                               "Content-Length: %u\r\n\r\n"
                               "%s",
                               OTA_PRODUCT_ID, OTA_DEVICE_NAME, OTA_FUSE_HOST, g_ota_auth, (unsigned int)strlen(g_ota_body), g_ota_body);
        if (req_len <= 0)
        {
            printf("OTA version: build req fail\r\n");
            return 0;
        }
        printf("OTA version: send req len=%d\r\n", req_len);
        int n = ota_http_request(OTA_FUSE_HOST, OTA_HTTP_PORT, (const uint8_t *)g_ota_req, (uint32_t)req_len, g_ota_resp, sizeof(g_ota_resp), 3000);
        if (n <= 0)
        {
            printf("OTA version: http fail n=%d\r\n", n);
            return 0;
        }
        int status = ota_http_status_code(g_ota_resp, (uint32_t)n);
        printf("OTA version: http status=%d resp_len=%d\r\n", status, n);
        if (status < 200 || status >= 300)
        {
            printf("OTA version: bad status\r\n");
            return 0;
        }
        const uint8_t *res_body = NULL;
        uint32_t body_len = 0;
        if (!ota_http_body(g_ota_resp, (uint32_t)n, &res_body, &body_len))
        {
            printf("OTA version: no body\r\n");
            return 0;
        }
        int ok = ota_json_is_success(res_body, body_len);
        printf("OTA version: json ok=%d body_len=%lu\r\n", ok, (unsigned long)body_len);
        if (ok) return 1;
        int code = 0;
        if (ota_json_get_code(res_body, body_len, &code) && code == 10403 && ota_try_sync_unix_base_from_text(g_ota_json) && attempt == 0)
        {
            printf("OTA version: retry after time sync\r\n");
            continue;
        }
        return 0;
    }
    return 0;
}

// =========================================================================
// 函数：轮询检查是否有为本设备准备的 OTA 升级包裹
// 逻辑：直接发 GET 请求询问 FUSE 后端：这里有一台 V5.7 的叫 d0 的设备，有升级安排吗？
// 如果平台上有配对好的升级任务，服务器会直接回应包裹的 tid，总大小尺寸 size，以及 md5 校验码。
// =========================================================================
static int ota_check_upgrade(OtaPackageInfo *info)
{
    if (info == NULL) return 0;
    
    // 最多尝试发给平台两次 (防丢包)
    for (int attempt = 0; attempt < 2; attempt++)
    {
        // 每次发前先把接收的容器清零
        memset(info, 0, sizeof(OtaPackageInfo));
        printf("OTA check: build auth\r\n");
        
        // ===================================
        // 第一步：构建鉴权动态头部 (最重要的令牌)
        // ===================================
        if (!ota_build_auth_header(OTA_AUTH_FUSE_VER, OTA_AUTH_FUSE_RES_RAW, g_ota_auth, sizeof(g_ota_auth)))
        {
            printf("OTA check: auth fail\r\n");
            return 0;
        }
        
        // ===================================
        // 第二步：组装一个给 OneNET FUSE 的 GET 请求 
        // 报文结构里含有我们的版本号等核心参数
        // ===================================
        int req_len = snprintf(g_ota_req, sizeof(g_ota_req),
                               "GET /fuse-ota/%s/%s/check?type=2&version=%s HTTP/1.1\r\n"
                               "Host: %s\r\n"
                               "Authorization: %s\r\n"
                               "Connection: close\r\n\r\n",
                               OTA_PRODUCT_ID, OTA_DEVICE_NAME, OTA_CURRENT_VERSION, OTA_FUSE_HOST, g_ota_auth);
        if (req_len <= 0)
        {
            printf("OTA check: build req fail\r\n");
            return 0;
        }
        
        printf("OTA check: send req len=%d version=%s\r\n", req_len, OTA_CURRENT_VERSION);
        
        // ===================================
        // 第三步：将组合好的 GET 报文发给后端，并设定最长 8000ms 的等待周期
        // ===================================
        int n = ota_http_request(OTA_FUSE_HOST, OTA_HTTP_PORT, (const uint8_t *)g_ota_req, (uint32_t)req_len, g_ota_resp, sizeof(g_ota_resp), 8000);
        if (n <= 0)
        {
            printf("OTA check: http fail n=%d\r\n", n);
            return 0;
        }
        int http_status = ota_http_status_code(g_ota_resp, (uint32_t)n);
        printf("OTA check: http status=%d resp_len=%d\r\n", http_status, n);
        if (http_status != 200)
        {
            printf("OTA check: bad status\r\n");
            return 0;
        }
        const uint8_t *res_body = NULL;
        uint32_t body_len = 0;
        if (!ota_http_body(g_ota_resp, (uint32_t)n, &res_body, &body_len))
        {
            printf("OTA check: no body\r\n");
            return 0;
        }
        uint32_t copy_len = body_len;
        if (copy_len >= sizeof(g_ota_json)) copy_len = sizeof(g_ota_json) - 1U;
        memcpy(g_ota_json, res_body, copy_len);
        g_ota_json[copy_len] = '\0';
        cJSON *root = cJSON_Parse(g_ota_json);
        if (root == NULL)
        {
            printf("OTA check: parse json fail body=%s\r\n", g_ota_json);
            return 0;
        }
        int has_pkg = 0;
        cJSON *code_node = cJSON_GetObjectItemCaseSensitive(root, "code");
        if (cJSON_IsNumber(code_node))
        {
            printf("OTA check: code=%d\r\n", code_node->valueint);
            if (code_node->valueint == 10403 && ota_try_sync_unix_base_from_text(g_ota_json) && attempt == 0)
            {
                cJSON_Delete(root);
                printf("OTA check: retry after time sync\r\n");
                continue;
            }
        }
        if (cJSON_IsNumber(code_node) && code_node->valueint == 0)
        {
            cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
            cJSON *tid = cJSON_GetObjectItemCaseSensitive(data, "tid");
            cJSON *status = cJSON_GetObjectItemCaseSensitive(data, "status");
            cJSON *target = cJSON_GetObjectItemCaseSensitive(data, "target");
            cJSON *size = cJSON_GetObjectItemCaseSensitive(data, "size");
            cJSON *md5 = cJSON_GetObjectItemCaseSensitive(data, "md5");
            if (cJSON_IsNumber(status) &&
                (status->valueint == 1 || status->valueint == 2 || status->valueint == 3) &&
                cJSON_IsString(target) && cJSON_IsNumber(size) && cJSON_IsString(md5))
            {
                if ((uint32_t)size->valuedouble > 0U)
                {
                    strncpy(info->target, target->valuestring, sizeof(info->target) - 1U);
                    strncpy(info->md5_hex, md5->valuestring, sizeof(info->md5_hex) - 1U);
                    info->size = (uint32_t)size->valuedouble;
                    if (strlen(info->md5_hex) == 32U && ota_md5_hex_to_bin(info->md5_hex, info->md5_bin) && ota_json_copy_tid(tid, info->tid, sizeof(info->tid)))
                    {
                        has_pkg = 1;
                        printf("OTA check: has pkg tid=%s target=%s size=%lu status=%d\r\n",
                               info->tid, info->target, (unsigned long)info->size, status->valueint);
                    }
                }
            }
        }
        if (!has_pkg)
        {
            printf("OTA check: no valid pkg, body=%s\r\n", g_ota_json);
        }
        cJSON_Delete(root);
        return has_pkg;
    }
    return 0;
}

// =========================================================================
// 函数：验证下发的 OTA 任务的实际“准备状态”
// 逻辑：拿到 tid (任务号) 之后，再次查核 FUSE 云端的状态码。
// 数据流中的 "status" == 1 或者 5，代表云端的 OSS 文件链接已开放可供下载，我们才能安全拔掉网管。
// =========================================================================
static int ota_check_task_ready(const OtaPackageInfo *info)
{
    if (info == NULL || info->tid[0] == '\0') return 0;
    printf("OTA task: check tid=%s\r\n", info->tid);
    if (!ota_build_auth_header(OTA_AUTH_FUSE_VER, OTA_AUTH_FUSE_RES_RAW, g_ota_auth, sizeof(g_ota_auth)))
    {
        printf("OTA task: auth fail\r\n");
        return 0;
    }
    int req_len = snprintf(g_ota_req, sizeof(g_ota_req),
                           "GET /fuse-ota/%s/%s/%s/check?type=2&version=%s HTTP/1.1\r\n"
                           "Host: %s\r\n"
                           "Authorization: %s\r\n"
                           "Connection: close\r\n\r\n",
                           OTA_PRODUCT_ID, OTA_DEVICE_NAME, info->tid, OTA_CURRENT_VERSION, OTA_FUSE_HOST, g_ota_auth);
    if (req_len <= 0)
    {
        printf("OTA task: build req fail\r\n");
        return 0;
    }
    int n = ota_http_request(OTA_FUSE_HOST, OTA_HTTP_PORT, (const uint8_t *)g_ota_req, (uint32_t)req_len, g_ota_resp, sizeof(g_ota_resp), 4000);
    if (n <= 0)
    {
        printf("OTA task: http fail n=%d\r\n", n);
        return 0;
    }
    int http_status = ota_http_status_code(g_ota_resp, (uint32_t)n);
    if (http_status != 200)
    {
        printf("OTA task: bad status=%d\r\n", http_status);
        return 0;
    }
    const uint8_t *res_body = NULL;
    uint32_t body_len = 0;
    if (!ota_http_body(g_ota_resp, (uint32_t)n, &res_body, &body_len))
    {
        printf("OTA task: no body\r\n");
        return 0;
    }
    uint32_t copy_len = body_len;
    if (copy_len >= sizeof(g_ota_json)) copy_len = sizeof(g_ota_json) - 1U;
    memcpy(g_ota_json, res_body, copy_len);
    g_ota_json[copy_len] = '\0';
    cJSON *root = cJSON_Parse(g_ota_json);
    if (root == NULL)
    {
        printf("OTA task: parse json fail body=%s\r\n", g_ota_json);
        return 0;
    }
    int ready = 0;
    cJSON *code_node = cJSON_GetObjectItemCaseSensitive(root, "code");
    if (cJSON_IsNumber(code_node) && code_node->valueint == 0)
    {
        cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
        cJSON *status = cJSON_GetObjectItemCaseSensitive(data, "status");
        if (cJSON_IsNumber(status))
        {
            printf("OTA task: remote status=%d\r\n", status->valueint);
            if (status->valueint == 1 || status->valueint == 2 || status->valueint == 3) ready = 1;
        }
    }
    cJSON_Delete(root);
    printf("OTA task: ready=%d\r\n", ready);
    return ready;
}

// =========================================================================
// 函数：多状态的步骤和执行进度上报
// 作用：将单片机的升级状态告诉云端（如：0=开始下载，下载到了xx步，201=下载检验成功，206=报错等）。
// 用户在平台的页面上看到的百分比进度条，就是这个函数支撑的。
// =========================================================================
static int ota_report_result(const OtaPackageInfo *info, int result_code)
{
    if (info == NULL || info->tid[0] == '\0') return 0;
    if (!ota_build_auth_header(OTA_AUTH_FUSE_VER, OTA_AUTH_FUSE_RES_RAW, g_ota_auth, sizeof(g_ota_auth))) return 0;
    snprintf(g_ota_body, sizeof(g_ota_body), "{\"step\":%d}", result_code);
    int req_len = snprintf(g_ota_req, sizeof(g_ota_req),
                           "POST /fuse-ota/%s/%s/%s/status HTTP/1.1\r\n"
                           "Host: %s\r\n"
                           "Authorization: %s\r\n"
                           "Content-Type: application/json\r\n"
                           "Connection: close\r\n"
                           "Content-Length: %u\r\n\r\n"
                           "%s",
                           OTA_PRODUCT_ID, OTA_DEVICE_NAME, info->tid, OTA_FUSE_HOST, g_ota_auth, (unsigned int)strlen(g_ota_body), g_ota_body);
    if (req_len <= 0)
    {
        printf("OTA status: build req fail step=%d\r\n", result_code);
        return 0;
    }
    int n = ota_http_request(OTA_FUSE_HOST, OTA_HTTP_PORT, (const uint8_t *)g_ota_req, (uint32_t)req_len, g_ota_resp, sizeof(g_ota_resp), 3000);
    if (n <= 0)
    {
        printf("OTA status: http fail step=%d n=%d\r\n", result_code, n);
        return 0;
    }
    int status = ota_http_status_code(g_ota_resp, (uint32_t)n);
    if (status < 200 || status >= 300)
    {
        printf("OTA status: bad http step=%d status=%d\r\n", result_code, status);
        return 0;
    }
    const uint8_t *res_body = NULL;
    uint32_t body_len = 0;
    if (!ota_http_body(g_ota_resp, (uint32_t)n, &res_body, &body_len))
    {
        printf("OTA status: no body step=%d\r\n", result_code);
        return 0;
    }
    int code = 0;
    if (!ota_json_get_code(res_body, body_len, &code))
    {
        int ok = ota_json_is_success(res_body, body_len);
        printf("OTA status: step=%d parse_code_fail ok=%d\r\n", result_code, ok);
        return ok;
    }
    printf("OTA status: step=%d code=%d\r\n", result_code, code);
    if (code == 0 || code == 20) return 1;
    return 0;
}

// =========================================================================
// 核心函数：断点分块下载与固件真实烧录验证
// 逻辑流程：
// 1. 调用底层的 OTA_PrepareDownloadArea 清空 STM32 目标 Flash 扇区 (Application 2 区)。
// 2. 通过 HTTP Header `Range: bytes=offset-end` 的特性，每次请求服务器传 1024 字节内容。
// 3. 把收到的 1KB 实打实地写入 Flash 目标片区 (`OTA_WriteDownloadChunk`)。
// 4. 重复这个过程，指针游标 (`offset`) 步进累加，直到拉满几万个字节的总包。
// 5. 将刚写入 Flash 中的二进制数据再一点点抠出来，灌入 MD5 计算器里重新哈希一次。
// 6. 核对这管算出来的 MD5 跟此前平台一开始分配的 MD5 秘钥是否完全一致（防止电压或者网卡掉包烂片）。
// 7. 如果没毛病，调用 `OTA_SetPendingImage` 偷偷修改 Bootloader 的 META 启动头，通知它复位后有新包。
// =========================================================================
static int ota_download_and_verify(const OtaPackageInfo *info)
{
    if (info == NULL) return 0;
    if (!ota_build_auth_header(OTA_AUTH_FUSE_VER, OTA_AUTH_FUSE_RES_RAW, g_ota_auth, sizeof(g_ota_auth)))
    {
        printf("OTA download: auth fail\r\n");
        return 0;
    }
    if (info->size == 0U || info->size > OTA_GetDownloadMaxSize())
    {
        printf("OTA download: bad size=%lu max=%lu\r\n", (unsigned long)info->size, (unsigned long)OTA_GetDownloadMaxSize());
        return 0;
    }
    if (!OTA_PrepareDownloadArea(info->size))
    {
        printf("OTA download: prepare area fail\r\n");
        return 0;
    }
    const uint32_t chunk_size = OTA_DOWNLOAD_CHUNK_SIZE;
    uint32_t offset = 0;
    uint32_t last_progress = 0U;
    printf("OTA download: start tid=%s size=%lu target=%s\r\n", info->tid, (unsigned long)info->size, info->target);
    ota_report_result(info, 0);
    while (offset < info->size)
    {
        uint32_t end = offset + chunk_size - 1U;
        if (end >= info->size) end = info->size - 1U;
        int req_len = snprintf(g_ota_req, sizeof(g_ota_req),
                               "GET /fuse-ota/%s/%s/%s/download HTTP/1.1\r\n"
                               "Host: %s\r\n"
                               "Range: %lu-%lu\r\n"
                               "Authorization: %s\r\n"
                               "Connection: close\r\n\r\n",
                               OTA_PRODUCT_ID, OTA_DEVICE_NAME, info->tid,
                               OTA_FUSE_HOST,
                               (unsigned long)offset, (unsigned long)end,
                               g_ota_auth);
        if (req_len <= 0 || req_len >= (int)sizeof(g_ota_req))
        {
            printf("OTA download: build req fail offset=%lu end=%lu\r\n", (unsigned long)offset, (unsigned long)end);
            return 0;
        }
        int n = ota_http_request(OTA_FUSE_HOST, OTA_HTTP_PORT, (const uint8_t *)g_ota_req, (uint32_t)req_len, g_ota_resp, sizeof(g_ota_resp), 12000);
        if (n <= 0)
        {
            printf("OTA download: http fail offset=%lu n=%d\r\n", (unsigned long)offset, n);
            return 0;
        }
        int status = ota_http_status_code(g_ota_resp, (uint32_t)n);
        if (!(status == 206 || status == 200))
        {
            printf("OTA download: bad status=%d offset=%lu n=%d\r\n", status, (unsigned long)offset, n);
            return 0;
        }
        const uint8_t *body = NULL;
        uint32_t body_len = 0;
        if (!ota_http_body(g_ota_resp, (uint32_t)n, &body, &body_len))
        {
            printf("OTA download: no body offset=%lu\r\n", (unsigned long)offset);
            return 0;
        }
        uint32_t expect = end - offset + 1U;
        uint32_t write_len = body_len;
        if (write_len > expect) write_len = expect;
        if (write_len == 0U)
        {
            printf("OTA download: write_len=0 offset=%lu body_len=%lu expect=%lu\r\n",
                   (unsigned long)offset, (unsigned long)body_len, (unsigned long)expect);
            return 0;
        }
        if (!OTA_WriteDownloadChunk(offset, body, write_len))
        {
            printf("OTA download: flash write fail offset=%lu len=%lu\r\n", (unsigned long)offset, (unsigned long)write_len);
            return 0;
        }
        offset += write_len;
        uint32_t progress = (offset * 100U) / info->size;
        if (progress > 100U) progress = 100U;
        if (progress == 100U || progress >= (last_progress + OTA_PROGRESS_STEP_PCT))
        {
            ota_report_result(info, (int)progress);
            last_progress = progress;
        }
        printf("OTA download %lu/%lu\r\n", (unsigned long)offset, (unsigned long)info->size);
    }


    //验证
    uint8_t calc[16];
    MD5_CTX ctx;
    MD5Init(&ctx);
    offset = 0;
    while (offset < info->size)
    {
        uint32_t nread = info->size - offset;
        if (nread > sizeof(g_ota_read_buf)) nread = sizeof(g_ota_read_buf);
        OTA_ReadDownloadData(offset, g_ota_read_buf, nread);
        MD5Update(&ctx, g_ota_read_buf, nread);
        offset += nread;
    }
    MD5Final(&ctx, calc);
    if (memcmp(calc, info->md5_bin, 16) != 0)
    {
        char local_hex[33];
        ota_md5_bin_to_hex(calc, local_hex);
        printf("OTA md5 mismatch local=%s remote=%s\r\n", local_hex, info->md5_hex);
        return 0;
    }
    if (!OTA_SetPendingImage(info->size, info->md5_bin, info->target, info->tid))
    {
        printf("OTA download: set pending image fail\r\n");
        return 0;
    }
    printf("OTA download: verify ok\r\n");
    return 1;
}

// =========================================================================
// 主入口：OTA 任务引擎状态机 (也就是 USER/main.c 中网卡挂起后唯一的寄托主循环)
// 依次完成所有的业务逻辑。一旦其中一步出错就退出并上报 206 失败码。
// 走到最后就会下达 NVIC_SystemReset 重启这块 PCB。
// =========================================================================
void OTA_ProcessUpgrade(void)
{
    OtaPackageInfo info;
    memset(&info, 0, sizeof(info));
    printf("OTA start\r\n");
    if (!ota_report_version())
    {
        printf("OTA stop: report version fail\r\n");
    }
    if (!ota_check_upgrade(&info))
    {
        printf("OTA no package\r\n");
        return;
    }
    if (!ota_check_task_ready(&info))
    {
        printf("OTA task not ready\r\n");
        return;
    }
    if (!ota_download_and_verify(&info))
    {
        ota_report_result(&info, 206);
        printf("OTA failed\r\n");
        return;
    }
    ota_report_result(&info, 201);
    printf("OTA ready reboot\r\n");
    delay_ms(200);
    NVIC_SystemReset();
}
