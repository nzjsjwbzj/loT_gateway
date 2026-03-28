#ifndef OTA_H
#define OTA_H

/* ==================== OneNET OTA 服务器基础配置 ==================== */
#define OTA_FUSE_HOST           "iot-api.heclouds.com" // OneNET FUSE 平台的 API 地址
#define OTA_HTTP_PORT           80                     // HTTP 默认端口号

/* ==================== 产品与设备身份信息 ==================== */
#define OTA_PRODUCT_ID          "XHm5ltr9qA"           // 你的产品 ID
#define OTA_DEVICE_NAME         "d0"                   // 设备名称
//#define OTA_DEVICE_ID           "527776559"            // 设备 ID
#define OTA_CURRENT_VERSION     "V5.7"                 // 单片机当前运行的固件版本号（用于给云端对比，判定是否需要升级）

/* ==================== 鉴权与加密配置 (非常核心) ==================== */
//#define OTA_ACCESS_KEY_B64      "i62LG9nPQyMvk4cxqc3ndbmRT3PFWwENE5PTxV+"
#define OTA_ACCESS_KEY_B64      "958590fcdc584431b483b001c9979492" // OneNET API 的访问密钥 (主账号级别)
#define OTA_AUTH_FUSE_VER       "2022-05-01"           // OneNET 规定的鉴权算法版本
#define OTA_AUTH_FUSE_RES_RAW   "userid/498670"        // 资源路径 (鉴权必须指定的被访问资源)
#define OTA_AUTH_METHOD         "sha1"                 // 哈希签名方法：采用 SHA1
#define OTA_AUTH_UNIX_NOW_BASE  1774170000UL           // 初始的基础 UNIX 时间戳，如果本地时间不对，代码会自动从服务器返回的 HTTP Date 中截取并修正它
#define OTA_AUTH_ET_TTL_SEC     3600UL                 // 生成的鉴权 Token 有效期 (3600秒 = 1小时)

/* ==================== 下载策略与体验配置 ==================== */
#define OTA_DOWNLOAD_CHUNK_SIZE 1024U                  // 断点续传时的块大小 (1KB)。单片机每次只向云端 HTTP 拉取 1KB 固件，避免内存溢出
#define OTA_PROGRESS_STEP_PCT   10U                    // 进度条上报步长。每累计下载满 10% 才向云端发送一次进度报告，降低通信负担

/* ==================== 接口函数声明 ==================== */
// 将云端由 MQTT (或其它方式) 下发的任务 ID 存入内部变量
void OTA_SetTaskId(const char *tid);

// 正式启动 OTA 流程的入口函数，它包含拉取、下载、写Flash、重启的所有逻辑
void OTA_ProcessUpgrade(void);

#endif
