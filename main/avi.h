#pragma once

#include <stdint.h>
#include "esp_err.h"

/* AVI 四字符码 */
#define AVI_RIFF_ID      0x46464952  /* "RIFF" */
#define AVI_AVI_ID       0x20495641  /* "AVI " */
#define AVI_LIST_ID      0x5453494C  /* "LIST" */
#define AVI_HDRL_ID      0x6C726468  /* "hdrl" */
#define AVI_STRL_ID      0x6C727473  /* "strl" */
#define AVI_AVIH_ID      0x68697661  /* "avih" */
#define AVI_STRH_ID      0x68727473  /* "strh" */
#define AVI_STRF_ID      0x66727473  /* "strf" */
#define AVI_MOVI_ID      0x69766F6D  /* "movi" */

/* 流类型 */
#define AVI_VIDS_STREAM  0x73646976  /* "vids" */
#define AVI_AUDS_STREAM  0x73647561  /* "auds" */

/* 压缩格式 */
#define AVI_FORMAT_MJPG  0x47504A4D  /* "MJPG" */
#define AVI_FORMAT_PCM   0x00000001

/* 音视频流标记 */
#define AVI_VIDS_FLAG_TBL_00  "00dc"
#define AVI_VIDS_FLAG_TBL_01  "01dc"
#define AVI_AUDS_FLAG_TBL_00  "00wb"
#define AVI_AUDS_FLAG_TBL_01  "01wb"

/* AVI 帧最大尺寸 */
#define AVI_MAX_FRAME_SIZE  (320 * 240 * 3)

/* RIFF 文件头 */
typedef struct {
    uint32_t RiffID;     /* "RIFF" */
    uint32_t FileSize;   /* 文件大小 - 8 */
    uint32_t AviID;      /* "AVI " */
} AVI_HEADER;

/* LIST 块 */
typedef struct {
    uint32_t ListID;     /* "LIST" */
    uint32_t BlockSize;  /* 块大小 */
    uint32_t ListType;   /* "hdrl"/"strl"/"movi" */
} LIST_HEADER;

/* avih 子块 */
typedef struct {
    uint32_t BlockID;            /* "avih" */
    uint32_t BlockSize;          /* 块大小(不含 BlockID+BlockSize) */
    uint32_t SecPerFrame;        /* 帧间隔时间 (us) */
    uint32_t MaxByteSec;         /* 最大数据传输率 */
    uint32_t PaddingGranularity;
    uint32_t Flags;
    uint32_t TotalFrame;         /* 总帧数 */
    uint32_t InitFrames;
    uint32_t Streams;            /* 数据流种类数 (通常 2) */
    uint32_t RefBufSize;
    uint32_t Width;              /* 视频宽 */
    uint32_t Height;             /* 视频高 */
    uint32_t Reserved[4];
} AVIH_HEADER;

/* strh 子块 */
typedef struct {
    uint32_t BlockID;      /* "strh" */
    uint32_t BlockSize;    /* 块大小 */
    uint32_t StreamType;   /* "vids"/"auds" */
    uint32_t Handler;      /* 解码器 */
    uint32_t Flags;
    uint16_t Priority;
    uint16_t Language;
    uint32_t InitFrames;
    uint32_t Scale;
    uint32_t Rate;
    uint32_t Start;
    uint32_t Length;
    uint32_t RefBufSize;
    uint32_t Quality;
    uint32_t SampleSize;
    struct {
        short Left;
        short Top;
        short Right;
        short Bottom;
    } Frame;
} STRH_HEADER;

/* BMP 信息头 */
typedef struct {
    uint32_t BmpSize;        /* 结构体大小 */
    long     Width;
    long     Height;
    uint16_t Planes;
    uint16_t BitCount;
    uint32_t Compression;    /* MJPG 等 */
    uint32_t SizeImage;
    long     XpixPerMeter;
    long     YpixPerMeter;
    uint32_t ClrUsed;
    uint32_t ClrImportant;
} BMP_HEADER;

/* 视频流 strf 块 */
typedef struct {
    uint32_t BlockID;         /* "strf" */
    uint32_t BlockSize;
    BMP_HEADER bmiHeader;
} STRF_BMPHEADER;

/* 音频流 strf 块 */
typedef struct {
    uint32_t BlockID;      /* "strf" */
    uint32_t BlockSize;
    uint16_t FormatTag;    /* 0x0001=PCM */
    uint16_t Channels;     /* 声道数 */
    uint32_t SampleRate;   /* 采样率 */
    uint32_t BaudRate;     /* 波特率 */
    uint16_t BlockAlign;
    uint16_t Size;
} STRF_WAVHEADER;

/* AVI 解析结果 */
typedef struct {
    uint32_t SecPerFrame;    /* 帧间隔 (us) */
    uint32_t TotalFrame;     /* 总帧数 */
    uint32_t Width;          /* 视频宽 */
    uint32_t Height;         /* 视频高 */
    uint32_t SampleRate;     /* 音频采样率 */
    uint16_t Channels;       /* 声道数 */
    uint16_t AudioType;      /* 音频格式 */
    char     VideoFLAG[5];   /* "00dc" 或 "01dc" */
    char     AudioFLAG[5];   /* "00wb" 或 "01wb" */
    uint32_t StreamID;       /* 当前流 ID */
    uint32_t StreamSize;     /* 当前流大小 */
    uint32_t AudioBufSize;   /* 首帧音频大小 */
    uint32_t MoviOffset;     /* movi 列表在文件中的偏移 */
} AVI_INFO;

/* 返回值 */
typedef enum {
    AVI_OK = 0,
    AVI_RIFF_ERR,
    AVI_AVI_ERR,
    AVI_LIST_ERR,
    AVI_HDRL_ERR,
    AVI_AVIH_ERR,
    AVI_STRL_ERR,
    AVI_STRH_ERR,
    AVI_STRF_ERR,
    AVI_FORMAT_ERR,
    AVI_MOVI_ERR,
    AVI_STREAM_ERR,
} AVISTATUS;

/* 工具宏: 小端读取 WORD/DWORD */
static inline uint16_t AVI_GET_WORD(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

static inline uint32_t AVI_GET_DWORD(const uint8_t *p) {
    return (uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24));
}

/* 函数声明 */
AVISTATUS avi_init(const uint8_t *buf, uint32_t size, AVI_INFO *info);
uint16_t  avi_search_id(const uint8_t *buf, uint32_t size, const char *id);
AVISTATUS avi_get_streaminfo(const uint8_t *buf, AVI_INFO *info);
