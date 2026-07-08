/**
 * @brief  AVI 文件解析器
 *
 * 解析 MJPEG 编码 + PCM 音频的 AVI 文件头，
 * 提取视频分辨率、帧率、音频采样率等信息。
 * 参考正点原子 ESP32-S3 视频播放器实验。
 */
#include "avi.h"
#include <string.h>
#include "esp_log.h"

#define TAG "avi"

/**
 * @brief  搜索 4 字节 ID 在 buf 中的位置
 * @param  buf  搜索缓冲区
 * @param  size 缓冲区大小
 * @param  id   要搜索的 4 字节字符串
 * @return 偏移量，0 表示未找到
 */
uint16_t avi_search_id(const uint8_t *buf, uint32_t size, const char *id)
{
    if (size < 4) return 0;
    size -= 4;
    for (uint32_t i = 0; i < size; i++) {
        if (buf[i] == id[0] &&
            buf[i + 1] == id[1] &&
            buf[i + 2] == id[2] &&
            buf[i + 3] == id[3]) {
            /* 帧大小必须 >16 才认为是有效数据 */
            uint32_t fsize = AVI_GET_DWORD(buf + i + 4);
            if (fsize > 0x10) {
                return (uint16_t)i;
            }
        }
    }
    return 0;
}

/**
 * @brief  获取当前数据流的类型和大小
 * @param  buf   流帧头指针 (xxdc/xxwb 开头)
 * @param  info  输出流信息
 * @return AVI_OK 成功
 */
AVISTATUS avi_get_streaminfo(const uint8_t *buf, AVI_INFO *info)
{
    info->StreamID   = AVI_GET_WORD(buf + 2);   /* 流类型码 */
    info->StreamSize = AVI_GET_DWORD(buf + 4);  /* 流数据大小 */

    /* 检查是否为已知流类型 */
    if (memcmp(buf, info->VideoFLAG, 4) == 0 ||
        memcmp(buf, info->AudioFLAG, 4) == 0) {
        return AVI_OK;
    }
    return AVI_STREAM_ERR;
}

/**
 * @brief  解析 AVI 文件头，提取视频/音频参数
 * @param  buf   文件头部数据 (至少 8KB)
 * @param  size  buf 大小
 * @param  info  解析结果
 * @return AVI_OK 成功
 */
AVISTATUS avi_init(const uint8_t *buf, uint32_t size, AVI_INFO *info)
{
    uint16_t offset;
    const uint8_t *ptr = buf;

    memset(info, 0, sizeof(AVI_INFO));

    /* 1. 检查 RIFF 头 */
    AVI_HEADER *avihdr = (AVI_HEADER *)ptr;
    if (avihdr->RiffID != AVI_RIFF_ID) {
        ESP_LOGE(TAG, "Invalid RIFF ID");
        return AVI_RIFF_ERR;
    }
    if (avihdr->AviID != AVI_AVI_ID) {
        ESP_LOGE(TAG, "Invalid AVI ID");
        return AVI_AVI_ERR;
    }
    ptr += sizeof(AVI_HEADER);

    /* 2. 检查 hdrl LIST */
    LIST_HEADER *list = (LIST_HEADER *)ptr;
    if (list->ListID != AVI_LIST_ID) return AVI_LIST_ERR;
    if (list->ListType != AVI_HDRL_ID) return AVI_HDRL_ERR;
    ptr += sizeof(LIST_HEADER);

    /* 3. 读取 avih */
    AVIH_HEADER *avih = (AVIH_HEADER *)ptr;
    if (avih->BlockID != AVI_AVIH_ID) return AVI_AVIH_ERR;
    info->SecPerFrame = avih->SecPerFrame;
    info->TotalFrame  = avih->TotalFrame;
    ESP_LOGI(TAG, "AVI: %lux%lu, %lu frames, %lu us/frame",
             avih->Width, avih->Height,
             avih->TotalFrame, avih->SecPerFrame);
    ptr += avih->BlockSize + 8;

    /* 4. 读取 strl 列表 */
    list = (LIST_HEADER *)ptr;
    if (list->ListID != AVI_LIST_ID) return AVI_LIST_ERR;
    if (list->ListType != AVI_STRL_ID) return AVI_STRL_ERR;

    /* 5. 读取 strh */
    STRH_HEADER *strh = (STRH_HEADER *)(ptr + 12);
    if (strh->BlockID != AVI_STRH_ID) return AVI_STRH_ERR;

    const uint8_t *strl_end = ptr + 8 + list->BlockSize; /* 当前 strl 块结束位置 */

    if (strh->StreamType == AVI_VIDS_STREAM) {
        /* 第一个流是视频 */
        if (strh->Handler != AVI_FORMAT_MJPG) {
            ESP_LOGE(TAG, "Unsupported codec: 0x%08lX (need MJPG)", strh->Handler);
            return AVI_FORMAT_ERR;
        }
        strcpy(info->VideoFLAG, AVI_VIDS_FLAG_TBL_00);   /* "00dc" */
        strcpy(info->AudioFLAG, AVI_AUDS_FLAG_TBL_01);   /* "01wb" -- 当前未使用 */

        /* 读取 strf (视频) */
        STRF_BMPHEADER *bmp = (STRF_BMPHEADER *)(ptr + 12 + strh->BlockSize + 8);
        if (bmp->BlockID != AVI_STRF_ID) return AVI_STRF_ERR;
        info->Width  = bmp->bmiHeader.Width;
        info->Height = bmp->bmiHeader.Height;
        ESP_LOGI(TAG, "Video: %lux%lu, codec=MJPG", info->Width, info->Height);

        ptr = (const uint8_t *)strl_end;

        /* 检查是否有第二个 strl (音频) */
        if ((uint32_t)(ptr - buf + 12) < size) {
            list = (LIST_HEADER *)ptr;
            if (list->ListID == AVI_LIST_ID && list->ListType == AVI_STRL_ID) {
                strh = (STRH_HEADER *)(ptr + 12);
                if (strh->BlockID == AVI_STRH_ID &&
                    strh->StreamType == AVI_AUDS_STREAM) {
                    strcpy(info->AudioFLAG, AVI_AUDS_FLAG_TBL_01);  /* "01wb" */

                    STRF_WAVHEADER *wav = (STRF_WAVHEADER *)(ptr + 12 + strh->BlockSize + 8);
                    if (wav->BlockID == AVI_STRF_ID) {
                        info->SampleRate = wav->SampleRate;
                        info->Channels   = wav->Channels;
                        info->AudioType  = wav->FormatTag;
                        ESP_LOGI(TAG, "Audio: %luHz, %dch, fmt=%d",
                                 info->SampleRate, info->Channels, info->AudioType);
                    }
                    ptr += 8 + list->BlockSize;
                } else {
                    ESP_LOGW(TAG, "No audio stream found");
                }
            } else {
                ESP_LOGW(TAG, "No audio stream (only video)");
            }
        }
    } else if (strh->StreamType == AVI_AUDS_STREAM) {
        /* 第一个流是音频 */
        strcpy(info->VideoFLAG, AVI_VIDS_FLAG_TBL_01);  /* "01dc" */
        strcpy(info->AudioFLAG, AVI_AUDS_FLAG_TBL_00);  /* "00wb" */

        STRF_WAVHEADER *wav = (STRF_WAVHEADER *)(ptr + 12 + strh->BlockSize + 8);
        if (wav->BlockID != AVI_STRF_ID) return AVI_STRF_ERR;
        info->SampleRate = wav->SampleRate;
        info->Channels   = wav->Channels;
        info->AudioType  = wav->FormatTag;
        ESP_LOGI(TAG, "Audio: %luHz, %dch, fmt=%d",
                 info->SampleRate, info->Channels, info->AudioType);

        ptr = (const uint8_t *)strl_end;

        /* 下一个 strl (视频) */
        list = (LIST_HEADER *)ptr;
        if (list->ListID == AVI_LIST_ID && list->ListType == AVI_STRL_ID) {
            strh = (STRH_HEADER *)(ptr + 12);
            if (strh->BlockID == AVI_STRH_ID &&
                strh->StreamType == AVI_VIDS_STREAM) {
                STRF_BMPHEADER *bmp = (STRF_BMPHEADER *)(ptr + 12 + strh->BlockSize + 8);
                if (bmp->BlockID == AVI_STRF_ID) {
                    if (bmp->bmiHeader.Compression != AVI_FORMAT_MJPG) {
                        ESP_LOGE(TAG, "Unsupported video codec");
                        return AVI_FORMAT_ERR;
                    }
                    info->Width  = bmp->bmiHeader.Width;
                    info->Height = bmp->bmiHeader.Height;
                    ESP_LOGI(TAG, "Video: %lux%lu, codec=MJPG", info->Width, info->Height);
                }
            }
        }
    } else {
        ESP_LOGE(TAG, "Unknown stream type");
        return AVI_STRL_ERR;
    }

    /* 6. 查找 movi ID */
    offset = avi_search_id(buf, size, "movi");
    if (offset == 0) {
        ESP_LOGE(TAG, "movi chunk not found");
        return AVI_MOVI_ERR;
    }
    info->MoviOffset = offset;
    ESP_LOGI(TAG, "movi at offset: 0x%X", offset);

    /* 7. 查找首帧音频流标记，获取音频帧大小 */
    if (info->SampleRate > 0) {
        const uint8_t *movi = buf + offset;
        uint32_t remain = size - offset;
        uint16_t aidx = avi_search_id(movi, remain, info->AudioFLAG);
        if (aidx > 0) {
            info->AudioBufSize = AVI_GET_DWORD(movi + aidx + 4);
            ESP_LOGI(TAG, "Audio buf size: %lu", info->AudioBufSize);
        }
    }

    ESP_LOGI(TAG, "AVI init OK");
    ESP_LOGI(TAG, "  SecPerFrame: %lu us (%.1f fps)",
             info->SecPerFrame, 1000000.0f / info->SecPerFrame);
    ESP_LOGI(TAG, "  TotalFrame:  %lu", info->TotalFrame);
    ESP_LOGI(TAG, "  Resolution:  %lux%lu", info->Width, info->Height);
    ESP_LOGI(TAG, "  Audio:       %lu Hz, %d ch", info->SampleRate, info->Channels);
    ESP_LOGI(TAG, "  VideoFLAG:   %s", info->VideoFLAG);
    ESP_LOGI(TAG, "  AudioFLAG:   %s", info->AudioFLAG);

    return AVI_OK;
}
