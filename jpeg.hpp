#include <string>
#include <vector>
#include <unordered_map>
#include <error_deal.h>
#pragma pack(1) // 各标志字段紧凑写入

extern "C"
{
#include <sys/stat.h>
#include <endian.h>
#include <unistd.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <string.h>
#include <sys/uio.h>
}

using namespace std;

#define DCT_BLOCK_LEN 8                                // 数据块长度
#define DCT_BLOCK_SIZE (DCT_BLOCK_LEN * DCT_BLOCK_LEN) // 数据块大小
#define DC_TABLE_SIZE 28                               // DC系数哈夫曼表长度
#define AC_TABLE_SIZE 178                              // AC系数哈夫曼表长度
#define MAX_ZERO_LEN 15                                // 允许最大0的游程
#define Y_QT_FLAG 0                                    // Y量化表标志
#define UV_QT_FLAG 1                                   // UV量化表标志

/*各种原图格式*/
uint8_t kYuv420 = 0x00;
uint8_t kYuv422 = 0x01;
uint8_t kYuv444 = 0x02;

/*各种哈夫曼表标志*/
uint8_t kDcYDht = 0x00;
uint8_t kDcUVDht = 0x01;
uint8_t kAcYDht = 0x10;
uint8_t kAcUVDht = 0x11;

static uint16_t kSoi = 0xD8FF; // jpeg开始字段标志
static uint16_t kEoi = 0xD9FF; // jpeg结束字段标志

static uint8_t kVliTable[4096]; // 各数值的bit长度映射表
static uint8_t *kVliTable_p;    // 表基址,指向0

/*Z字形表*/
static uint8_t kZTable[64] = {
    0, 1, 5, 6, 14, 15, 27, 28,
    2, 4, 7, 13, 16, 26, 29, 42,
    3, 8, 12, 17, 25, 30, 41, 43,
    9, 11, 18, 24, 31, 40, 44, 53,
    10, 19, 23, 32, 39, 45, 52, 54,
    20, 22, 33, 38, 46, 51, 55, 60,
    21, 34, 37, 47, 50, 56, 59, 61,
    35, 36, 48, 49, 57, 58, 62, 63};

/*标准Y量化表*/
static uint8_t kStdYQT[64] = {
    16, 11, 10, 16, 24, 40, 51, 61,
    12, 12, 14, 19, 26, 58, 60, 55,
    14, 13, 16, 24, 40, 57, 69, 56,
    14, 17, 22, 29, 51, 87, 80, 62,
    18, 22, 37, 56, 68, 109, 103, 77,
    24, 35, 55, 64, 81, 104, 113, 92,
    49, 64, 78, 87, 103, 121, 120, 101,
    72, 92, 95, 98, 112, 100, 103, 99};

/*标准UV量化表*/
static uint8_t kStdUVQT[64] = {
    17, 18, 24, 47, 99, 99, 99, 99,
    18, 21, 26, 66, 99, 99, 99, 99,
    24, 26, 56, 99, 99, 99, 99, 99,
    47, 66, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99};

/*各哈夫曼数组前16表示几bit的码值有多少个,16后为映射码值对应的数值*/
/*DC系数Y哈夫曼表*/
static uint8_t kStdDCYDht[28] = {0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};

/*DC系数UV哈夫曼表*/
static uint8_t kStdDCUVDht[28] = {0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};

/*AC系数Y哈夫曼表*/
static uint8_t kStdACYDht[178] = {0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0X7D, 0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07,
                                  0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xa1, 0x08,
                                  0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0,
                                  0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0a, 0x16,
                                  0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28,
                                  0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39,
                                  0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49,
                                  0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59,
                                  0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69,
                                  0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79,
                                  0x7a, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
                                  0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98,
                                  0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7,
                                  0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6,
                                  0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5,
                                  0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4,
                                  0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2,
                                  0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea,
                                  0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa};

/*AC系数UV哈夫曼表*/
static uint8_t kStdACUVDht[178] = {0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0X77, 0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21,
                                   0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71,
                                   0x13, 0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91,
                                   0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0,
                                   0x15, 0x62, 0x72, 0xd1, 0x0a, 0x16, 0x24, 0x34,
                                   0xe1, 0x25, 0xf1, 0x17, 0x18, 0x19, 0x1a, 0x26,
                                   0x27, 0x28, 0x29, 0x2a, 0x35, 0x36, 0x37, 0x38,
                                   0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48,
                                   0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58,
                                   0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68,
                                   0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78,
                                   0x79, 0x7a, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
                                   0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96,
                                   0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5,
                                   0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4,
                                   0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3,
                                   0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2,
                                   0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda,
                                   0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9,
                                   0xea, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8,
                                   0xf9, 0xfa};

/*DCT后量化矩阵高低频系数因子*/
static double kAanScaleFactor[8] = {1.0, 1.387039845, 1.306562965, 1.175875602, 1.0, 0.785694958, 0.541196100, 0.275899379}; // 高低频系数优化

static uint16_t kBitMask[9] = {0, 1, 3, 7, 15, 31, 63, 127, 255};

using namespace std;

/*段标志固定头部*/
struct Segment
{
    uint16_t segment_tag;
    uint16_t segment_len;
};

struct App0 : public Segment
{
    App0()
    {
        segment_tag = 0XE0FF;
        segment_len = 0x1000;
        memcpy(id, "JFIF\0", 5);
        version = 0x0101;
        pix_unit_dens = 0x00;
        level_pix_dens = 0x0100;
        vertical_pix_dens = 0x0100;
        image_thumbnail_level_num = 0x00;
        image_thumbnail_vertical_num = 0x00;
    }
    char id[5];                           // 图像文件格式
    uint16_t version;                     // 版本默认1.1
    uint8_t pix_unit_dens;                // 像素单位密度:0 无单位，1像素/英尺，2像素/cm
    uint16_t level_pix_dens;              // 水平像素密度
    uint16_t vertical_pix_dens;           // 垂直像素密度
    uint8_t image_thumbnail_level_num;    // 图像缩略图水平像素数
    uint8_t image_thumbnail_vertical_num; // 图像缩略图的垂直像素数
};

/*编码使用的量化表字段*/
struct QT_8Bit : public Segment
{
    QT_8Bit(uint8_t yuv_flag, int quality = 1)
    {
        segment_tag = 0xDBFF;
        segment_len = 0X4300;
        table_info += yuv_flag;
        SetQTable(quality);
    }

    /*系数化并z字形排列量化表以便写入*/
    void SetQTable(int quality)
    {
        int tmpVal = 0;
        int i;
        uint8_t *table_p = (table_info & 1) ? kStdUVQT : kStdYQT; // 根据标志确定Y还是UV量化表
        for (i = 0; i < DCT_BLOCK_SIZE; ++i)
        {
            tmpVal = (table_p[i] * quality + 50L) / 100L;

            if (tmpVal < 1)
            {
                tmpVal = 1L;
            }
            if (tmpVal > 255)
            {
                tmpVal = 255L;
            }
            table[kZTable[i]] = (uint8_t)tmpVal;
        }
    }
    uint8_t table_info = 0; // 前4bit表精度(0表8bit,1表16bit),后4bit表量化表ID(0是Y表,1是UV表)
    uint8_t table[64];      // 编码压缩使用的量化表
};

/*用户评论字段*/
struct Comment : public Segment
{
    Comment(string &&comstr)
    {
        segment_tag = 0xFEFF;
        segment_len = htobe16(com_str.length() + 2);
        com_str = comstr;
    }
    string com_str;
};

/*哈夫曼表字段*/
struct Dht : public Segment
{
    Dht(uint8_t tableinfo)
    {
        table_info = tableinfo;
        segment_tag = 0XC4FF;

        /*default is 00(DC-Y)*/
        dht_len = DC_TABLE_SIZE;
        dht_p = kStdDCYDht;

        /*01 (DC-UV)*/
        if (table_info & 0x01)
        {
            dht_p = kStdDCUVDht;
        }

        /*10/11 (AC)*/
        if (table_info & 0x10)
        {
            /*default 10 (AC-Y)*/
            dht_len = AC_TABLE_SIZE;
            dht_p = kStdACYDht;
            /*11 (AC-UV)*/
            if (table_info & 0x01)
            {
                dht_p = kStdACUVDht;
            }
        }

        segment_len = htobe16(3 + dht_len);
    }
    uint8_t table_info; // 前4bit表DC(0)还是AC(1),后4bit表HT ID(0-Y,1-UV)
    uint8_t *dht_p;
    uint32_t dht_len;
};

/*哈夫曼编码单位*/
struct HuffCode
{
    uint16_t code;  // 数值映射的码值
    uint8_t length; // 码值bit长度
};

/*帧开始段*/
struct Sof : public Segment
{
    Sof(uint16_t h, uint16_t w, uint8_t format)
    {
        segment_tag = 0xC0FF;
        frame_accuracy = 8; // 默认精度8bit

        /*高度、宽度大端写入*/
        height = htobe16(h);
        width = htobe16(w);

        /*YUV组件*/
        if ((format & 0xf0) == 0)
        {
            segment_len = 0x1100;
            comp_num = 3;
            comps_flag[0] = 0x01;
            comps_flag[2] = 0x00;
            comps_flag[3] = 0x02;
            comps_flag[5] = 0x01;
            comps_flag[6] = 0x03;
            comps_flag[8] = 0x01;
            uint8_t val = format & 0x0f;
            comps_flag[4] = 0x11;
            comps_flag[7] = 0x11;
            comps_flag[1] = 0x22; // 默认YUV420
            if (val == 1)         // YUV422
            {
                comps_flag[1] = 0x21;
            }
            else if (val == 2) // YUV44
            {
                comps_flag[1] = 0x11;
            }
        }
    }
    uint8_t frame_accuracy; // 帧数据精度(bit为单位)
    uint16_t height;        // 原图像高度
    uint16_t width;         // 原图像宽度
    uint8_t comp_num;       // 组件数量
    uint8_t comps_flag[15]; // 组件标识,3*n,第一个1字节标识组件ID,第二个字节表示采样事实(前4bit表示水平采样，后4bit表示垂直采样),第三个字节表示组件使用的量化表
};

/*扫描段*/
struct Sos : public Segment
{
    Sos(uint8_t format)
    {
        segment_tag = 0XDAFF;
        /*YUV组件*/
        if ((format & 0xf0) == 0)
        {
            comps_num = 0x03;
            memcpy(comp_info, "\x01\x00\x02\x11\x03\x11", 6);
            memcpy((uint8_t *)comp_info + 6, "\x00\x3f\x00", 3); // 强制跳过字节数
            segment_len = 0X0C00;
        }
    }
    uint8_t comps_num;     // 组件数量
    uint64_t comp_info[2]; // 组件信息,第一字节表示组件ID,第二字节前4bit表示使用的DC哈夫曼表ID,后4bit表示使用的AC哈夫曼表ID
};

class PhotoReader
{
public:
    friend class JpegDecoder;

    PhotoReader() {}

    PhotoReader(string &file_path) : file_path_(file_path) {}

    PhotoReader(string &&file_path) : file_path_(file_path) {}

    void SureFormat(uint8_t format)
    {
        photo_format_ = format;
    }

    void SureHandW(uint32_t width, uint32_t height)
    {
        photo_width_ = width;
        photo_height_ = height;
    }

    uint32_t CalPhotoSize()
    {
        uint32_t photo_size = photo_width_ * photo_height_;
        if (photo_format_ == kYuv420)
        {
            photo_size *= 3;
            photo_size /= 2;
        }
        else if (photo_format_ == kYuv422)
        {
            photo_size *= 2;
        }
        else if (photo_format_ == kYuv444)
        {
            photo_size *= 3;
        }
        return photo_size;
    }

    bool Read()
    {
        file_fd_ = open(file_path_.c_str(), O_DIRECT | O_RDONLY);
        ERROR_PRINT(file_fd_ == -1, perror("open jpeg fail!"), return false);
        uint32_t photo_size = CalPhotoSize();
        ERROR_IF(photo_size != GetFileSize(), "photo size not match!", return false);
        uint16_t rest_size = photo_size % 512;
        photo_size += rest_size != 0 ? 512 - rest_size : 0;
        posix_memalign((void **)&data_bufff_, 512, photo_size);
        ERROR_PRINT(read(file_fd_, data_bufff_, photo_size) == -1, perror("jpeg file read fail!"), return false);
        return true;
    }

    bool Read(string file_path)
    {
        file_path_ = file_path;
        return Read();
    }

    long GetFileSize()
    {
        struct stat statbuf;
        stat(this->file_path_.c_str(), &statbuf);
        return statbuf.st_size;
    }

private:
    string file_path_;
    int file_fd_ = -1;
    uint8_t *data_bufff_;
    uint32_t photo_width_ = 0;
    uint32_t photo_height_ = 0;
    uint8_t photo_format_ = -1;
};

class PhotoWriter
{
public:
    friend class JpegDecoder;

    PhotoWriter() {}

    PhotoWriter(string &file_path) : file_path_(file_path) {}

    PhotoWriter(string &&file_path) : file_path_(file_path) {}

    bool Write()
    {
        file_fd_ = open(file_path_.c_str(), O_DIRECT | O_WRONLY);
        ERROR_PRINT(file_fd_ == -1, perror("open jpeg fail!"), return false);
        ERROR_IF(write(file_fd_, data_bufff_.data(), data_len_) == -1, "jpeg file write fail!", return false);
        return true;
    }

    bool Write(string file_path)
    {
        file_path = file_path;
        return Write();
    }

private:
    string file_path_;
    int file_fd_ = -1;
    vector<char> data_bufff_;
    uint32_t data_len_ = 0;
};

class JpegDecoder
{
public:
    /*h*w矩阵数据内,从左到右,从上到下按x_len*y_len矩阵划分排列数据*/
    void DivBuff(uint8_t *data_bufff, uint32_t width, uint32_t height, uint32_t x_len, uint32_t y_len)
    {
        uint32_t xbufs = width / x_len;
        uint32_t ybufs = height / y_len;
        uint32_t tmpbuf_len = xbufs * x_len * y_len;
        uint8_t *tmpbuf = (uint8_t *)malloc(tmpbuf_len);
        uint32_t i;
        uint32_t j;
        uint32_t k;
        uint32_t n;
        uint32_t buff_offset = 0;
        for (i = 0; i < ybufs; ++i)
        {
            n = 0;
            for (j = 0; j < xbufs; ++j)
            {
                buff_offset = y_len * i * width + j * x_len;
                for (k = 0; k < y_len; ++k)
                {
                    memcpy(tmpbuf + n, data_bufff + buff_offset, x_len);
                    n += x_len;
                    buff_offset += width;
                }
            }
            memcpy(data_bufff + i * tmpbuf_len, tmpbuf, tmpbuf_len);
        }
        free(tmpbuf);
    }

    /*根据用户输入的量化值确定压缩质量*/
    void QualityScaling(int32_t &quality)
    {
        if (quality <= 0)
            quality = 1;
        else if (quality > 100)
            quality = 100;
        else if (quality < 50)
            quality = 5000 / quality;
        else
            quality = 200 - quality * 2;
    }

    /*生成数值bit长度映射表*/
    void BuildVliTable()
    {
        kVliTable_p = kVliTable + 2048;
        kVliTable_p[0] = 0;
        kVliTable_p[-1] = 1;
        kVliTable_p[1] = 1;
        kVliTable_p[-2048] = 0;
        uint16_t cpy_len = 1;
        for (uint8_t len = 2; len <= 11; len++)
        {
            cpy_len = cpy_len << 1;
            memset(kVliTable_p + cpy_len, len, cpy_len);
            memset(kVliTable_p - (cpy_len << 1) + 1, len, cpy_len);
        }
    }

    /*根据用户输入的量化系数确定编码使用的量化表*/
    void SetQuantTable(int32_t quality)
    {
        int32_t tmpVal = 0;
        uint16_t i;

        /*设置Y量化表*/
        for (i = 0; i < DCT_BLOCK_SIZE; ++i)
        {
            tmpVal = (kStdYQT[i] * quality + 50L) / 100L;
            if (tmpVal < 1)
            {
                tmpVal = 1L;
            }
            if (tmpVal > 255)
            {
                tmpVal = 255L;
            }
            quality_y_table_[kZTable[i]] = (uint8_t)tmpVal;
        }

        /*设置UV量化表*/
        for (i = 0; i < DCT_BLOCK_SIZE; ++i)
        {
            tmpVal = (kStdUVQT[i] * quality + 50L) / 100L;
            if (tmpVal < 1)
            {
                tmpVal = 1L;
            }
            if (tmpVal > 255)
            {
                tmpVal = 255L;
            }
            quality_uv_table_[kZTable[i]] = (uint8_t)tmpVal;
        }
    }

    /*设置DCT后的量化系数表*/
    void SetDctTable()
    {
        uint8_t i = 0;
        uint8_t j = 0;
        uint8_t k = 0;
        for (i = 0; i < DCT_BLOCK_LEN; i++)
        {
            for (j = 0; j < DCT_BLOCK_LEN; j++)
            {
                dct_y_table_[k] = (float)(1.0 / ((double)quality_y_table_[kZTable[k]] * kAanScaleFactor[i] * kAanScaleFactor[j] * 8.0));
                dct_uv_table_[k] = (float)(1.0 / ((double)quality_uv_table_[kZTable[k]] * kAanScaleFactor[i] * kAanScaleFactor[j] * 8.0));
                ++k;
            }
        }
    }

    /*建立哈夫曼表*/
    void BuildHuffTab()
    {
        /*建立 Y DC系数 哈夫曼表*/
        uint8_t i = 0;
        uint8_t j = 0;
        uint8_t k = 0;
        uint16_t code = 0;

        auto p = kStdDCYDht - 1;
        auto q = kStdDCYDht + 16;
        for (i = 1; i <= 16; i++)
        {
            for (j = 1; j <= p[i]; j++)
            {
                dc_y_dht_[q[k]].code = code;
                dc_y_dht_[q[k++]].length = i;
                ++code;
            }
            code *= 2;
        }

        /*建立 UV DC系数哈夫曼表*/
        i = 0;
        j = 0;
        k = 0;
        code = 0;
        p = kStdDCUVDht - 1;
        q = kStdDCUVDht + 16;
        for (i = 1; i <= 16; i++)
        {
            for (j = 1; j <= p[i]; j++)
            {
                dc_uv_dht_[q[k]].code = code;
                dc_uv_dht_[q[k++]].length = i;
                ++code;
            }
            code *= 2;
        }

        /*建立 Y AC 系数哈夫曼表*/
        i = 0;
        j = 0;
        k = 0;
        code = 0;
        p = kStdACYDht - 1;
        q = kStdACYDht + 16;
        for (i = 1; i <= 16; i++)
        {
            for (j = 1; j <= p[i]; j++)
            {
                ac_y_dht_[q[k]].code = code;
                ac_y_dht_[q[k++]].length = i;
                ++code;
            }
            code *= 2;
        }

        /*建立 UV AC 系数哈夫曼表*/
        i = 0;
        j = 0;
        k = 0;
        code = 0;
        p = kStdACUVDht - 1;
        q = kStdACUVDht + 16;
        for (i = 1; i <= 16; i++)
        {
            for (j = 1; j <= p[i]; j++)
            {
                ac_uv_dht_[q[k]].code = code;
                ac_uv_dht_[q[k++]].length = i;
                ++code;
            }
            code *= 2;
        }
    }

    /*对数据块做离散余弦变换*/
    void Fdct(float *data_bufff)
    {
        float tmp0, tmp1, tmp2, tmp3, tmp4, tmp5, tmp6, tmp7; // 第n行,偶数:i列与7-i列相加,奇数:7-i列与i列相减
        float tmp10, tmp11, tmp12, tmp13;
        float z1, z2, z3, z4, z5, z11, z13;
        float *dataptr;
        int8_t ctr;

        dataptr = data_bufff;
        for (ctr = DCT_BLOCK_LEN - 1; ctr >= 0; ctr--)
        {
            tmp0 = dataptr[0] + dataptr[7];
            tmp7 = dataptr[0] - dataptr[7];
            tmp1 = dataptr[1] + dataptr[6];
            tmp6 = dataptr[1] - dataptr[6];
            tmp2 = dataptr[2] + dataptr[5];
            tmp5 = dataptr[2] - dataptr[5];
            tmp3 = dataptr[3] + dataptr[4];
            tmp4 = dataptr[3] - dataptr[4];

            tmp10 = tmp0 + tmp3;
            tmp13 = tmp0 - tmp3;
            tmp11 = tmp1 + tmp2;
            tmp12 = tmp1 - tmp2;

            dataptr[0] = tmp10 + tmp11; /* phase 3 */
            dataptr[4] = tmp10 - tmp11;

            z1 = (float)((tmp12 + tmp13) * (0.707106781)); /* c4 */
            dataptr[2] = tmp13 + z1;                       /* phase 5 */
            dataptr[6] = tmp13 - z1;

            tmp10 = tmp4 + tmp5; /* phase 2 */
            tmp11 = tmp5 + tmp6;
            tmp12 = tmp6 + tmp7;

            z5 = (float)((tmp10 - tmp12) * (0.382683433)); /* c6 */
            z2 = (float)((0.541196100) * tmp10 + z5);      /* c2-c6 */
            z4 = (float)((1.306562965) * tmp12 + z5);      /* c2+c6 */
            z3 = (float)(tmp11 * (0.707106781));           /* c4 */

            z11 = tmp7 + z3;
            z13 = tmp7 - z3;

            dataptr[5] = z13 + z2; /* phase 6 */
            dataptr[3] = z13 - z2;
            dataptr[1] = z11 + z4;
            dataptr[7] = z11 - z4;

            dataptr += DCT_BLOCK_LEN;
        }

        dataptr = data_bufff;
        for (ctr = DCT_BLOCK_LEN - 1; ctr >= 0; ctr--)
        {
            tmp0 = dataptr[DCT_BLOCK_LEN * 0] + dataptr[DCT_BLOCK_LEN * 7];
            tmp7 = dataptr[DCT_BLOCK_LEN * 0] - dataptr[DCT_BLOCK_LEN * 7];
            tmp1 = dataptr[DCT_BLOCK_LEN * 1] + dataptr[DCT_BLOCK_LEN * 6];
            tmp6 = dataptr[DCT_BLOCK_LEN * 1] - dataptr[DCT_BLOCK_LEN * 6];
            tmp2 = dataptr[DCT_BLOCK_LEN * 2] + dataptr[DCT_BLOCK_LEN * 5];
            tmp5 = dataptr[DCT_BLOCK_LEN * 2] - dataptr[DCT_BLOCK_LEN * 5];
            tmp3 = dataptr[DCT_BLOCK_LEN * 3] + dataptr[DCT_BLOCK_LEN * 4];
            tmp4 = dataptr[DCT_BLOCK_LEN * 3] - dataptr[DCT_BLOCK_LEN * 4];

            tmp10 = tmp0 + tmp3;
            tmp13 = tmp0 - tmp3;
            tmp11 = tmp1 + tmp2;
            tmp12 = tmp1 - tmp2;

            dataptr[DCT_BLOCK_LEN * 0] = tmp10 + tmp11; /* phase 3 */
            dataptr[DCT_BLOCK_LEN * 4] = tmp10 - tmp11;

            z1 = (float)((tmp12 + tmp13) * (0.707106781)); /* c4 */
            dataptr[DCT_BLOCK_LEN * 2] = tmp13 + z1;       /* phase 5 */
            dataptr[DCT_BLOCK_LEN * 6] = tmp13 - z1;

            tmp10 = tmp4 + tmp5; /* phase 2 */
            tmp11 = tmp5 + tmp6;
            tmp12 = tmp6 + tmp7;

            z5 = (float)((tmp10 - tmp12) * (0.382683433)); /* c6 */
            z2 = (float)((0.541196100) * tmp10 + z5);      /* c2-c6 */
            z4 = (float)((1.306562965) * tmp12 + z5);      /* c2+c6 */
            z3 = (float)(tmp11 * (0.707106781));           /* c4 */

            z11 = tmp7 + z3; /* phase 5 */
            z13 = tmp7 - z3;

            dataptr[DCT_BLOCK_LEN * 5] = z13 + z2; /* phase 6 */
            dataptr[DCT_BLOCK_LEN * 3] = z13 - z2;
            dataptr[DCT_BLOCK_LEN * 1] = z11 + z4;
            dataptr[DCT_BLOCK_LEN * 7] = z11 - z4;

            ++dataptr;
        }
    }

    /*将一定bit长度的码值写入缓冲区*/
    void WriteBitsStream(uint16_t value, uint8_t code_len)
    {
        /*写入的码值bit长度大于字节剩余的bit长度*/
        while (bit_rest_ <= code_len)
        {
            int gap = code_len - bit_rest_;
            code_len -= bit_rest_;                 // 剩余写入的bit长度
            *(byte_p_) += (value >> gap) & 0X00FF; // 写入byte值
            /*写入0X00防0xFF段标志位字节竞争*/
            if (*byte_p_ == 0xFF)
            {
                *(++byte_p_) = 0x00;
            }
            bit_rest_ = 8;    // 从最高位开始
            *(++byte_p_) = 0; // 下一个待写入字节初始值设为0
        }

        if (code_len > 0)
        {
            value = (value & kBitMask[code_len]) << (bit_rest_ - code_len);
            *byte_p_ += value;
            bit_rest_ -= code_len;
        }
    }

    /*得到数值的Vli映射值*/
    int16_t GetVliCode(int16_t value)
    {
        if (value & 0x8000)
        {
            value = (1 << kVliTable_p[value]) - 1 + value;
        }
        return value;
    }

    /*处理自小基本单元数据块*/
    void ProcessBlock(float *data_buff, int16_t *prev_dc, float *dct_table, HuffCode *dc_dht, HuffCode *ac_dht)
    {
        uint8_t i;
        int16_t sig_buff[DCT_BLOCK_SIZE];
        Fdct(data_buff); // 数据块做DCT变换

        /*QT量化后的Z字形写入*/
        for (i = 0; i < DCT_BLOCK_SIZE; i++)
        {
            sig_buff[kZTable[i]] = (short)((data_buff[i] * dct_table[i] + 16384.5) - 16384);
        }

        /*计算与前DC系数的差值*/
        int16_t diff_val = sig_buff[0] - *prev_dc;
        *prev_dc = sig_buff[0];

        /*写入DC差值*/
        HuffCode *code_p = dc_dht + kVliTable_p[diff_val]; // 找到差值bit长度所对应的哈夫曼编码
        WriteBitsStream(code_p->code, code_p->length);
        /*若差值bit长度不为0则写入对应数值的VLI映射值*/
        if (diff_val != 0)
        {
            WriteBitsStream(GetVliCode(diff_val), kVliTable_p[diff_val]);
        }

        /*找到第一个不为零的值*/
        int8_t eob_pos;
        for (eob_pos = 63; (eob_pos > 0) && (sig_buff[eob_pos] == 0); eob_pos--)
        {
        }

        uint8_t zero_num = 0;
        uint8_t code_len = 0;

        /*从第一个AC系数开始遍历至EOB部分前一个*/
        for (i = 1; i <= eob_pos; i++)
        {
            if (sig_buff[i] == 0 && zero_num < MAX_ZERO_LEN)
            {
                ++zero_num;
            }
            else
            {
                /*先写入0数量(前4bit)与非零值对应bit长度(后4bit)构成的字节值得哈夫曼编码*/
                code_len = kVliTable_p[sig_buff[i]];
                uint8_t index = (zero_num << 4) + code_len;
                code_p = ac_dht + index;
                WriteBitsStream(code_p->code, code_p->length);

                /*若第16个值不为0,即非零值的bit长度不为零写入对应的Vli映射值*/
                if (code_len != 0)
                {
                    WriteBitsStream(GetVliCode(sig_buff[i]), code_len);
                }

                zero_num = 0;
            }
        }

        /*存在EOB,末尾写入*/
        if (eob_pos != 63)
        {
            WriteBitsStream(ac_dht[0x00].code, ac_dht[0x00].length);
        }
    }

    /*按宏块顺序处理数据块*/
    void ProcessMcu(uint8_t *y_buf, uint8_t *u_buf, uint8_t *v_buf, int mcu_num, uint8_t *yuv_counter)
    {
        float dct_ybuf[DCT_BLOCK_SIZE];
        float dct_ubuf[DCT_BLOCK_SIZE];
        float dct_vbuf[DCT_BLOCK_SIZE];

        int16_t y_prev_dc = 0;
        int16_t u_prev_dc = 0;
        int16_t v_prev_dc = 0;

        uint8_t y_counter = 0;
        uint8_t u_counter = 0;
        uint8_t v_counter = 0;

        uint32_t i = 0;
        uint32_t j = 0;
        uint32_t k = 0;
        uint32_t p = 0;
        uint32_t m = 0;
        uint32_t n = 0;
        uint32_t s = 0;

        for (p = 0; p < mcu_num; p++)
        {
            /*MCU 中 yuv 顺序Y,U,V block依次处理*/
            y_counter = yuv_counter[0];
            u_counter = yuv_counter[1];
            v_counter = yuv_counter[2];

            /*一定量的Y数据块先处理*/
            while (y_counter > 0)
            {
                for (j = 0; j < DCT_BLOCK_SIZE; j++)
                {
                    dct_ybuf[j] = (float)(y_buf[i + j] - 128);
                }
                --y_counter;
                ProcessBlock(dct_ybuf, &y_prev_dc, dct_y_table_, dc_y_dht_, ac_y_dht_);
                i += DCT_BLOCK_SIZE;
            }

            /*U第二处理*/
            while (u_counter > 0)
            {
                for (n = 0; n < DCT_BLOCK_SIZE; n++)
                {
                    dct_ubuf[n] = (float)(u_buf[m + n] - 128);
                }
                --u_counter;
                ProcessBlock(dct_ubuf, &u_prev_dc, dct_uv_table_, dc_uv_dht_, ac_uv_dht_);
                m += DCT_BLOCK_SIZE;
            }

            /*V最后处理*/
            while (v_counter > 0)
            {
                for (k = 0; k < DCT_BLOCK_SIZE; k++)
                {
                    dct_vbuf[k] = (float)(v_buf[s + k] - 128);
                }
                --v_counter;
                ProcessBlock(dct_vbuf, &v_prev_dc, dct_uv_table_, dc_uv_dht_, ac_uv_dht_);
                s += DCT_BLOCK_SIZE;
            }
        }
    }

    /*根据Y组件首地址得到UV组件数据首地址*/
    void SplitData(uint8_t *&py_buf, uint8_t *&pu_buf, uint8_t *&pv_buf, uint8_t format, uint32_t y_sum)
    {
        pu_buf = py_buf + y_sum;
        if (format == kYuv420)
        {
            y_sum = y_sum >> 2;
        }
        else if (format == kYuv422)
        {
            y_sum = y_sum >> 1;
        }
        pv_buf = pu_buf + y_sum; // 默认采用YUV444格式
    }

    bool Decode(PhotoReader &reader, int32_t quality, string &dst_file_path)
    {
        /*打开编码目标文件路径*/
        int file_fd = open(dst_file_path.c_str(), O_WRONLY | O_TRUNC);
        ERROR_PRINT(file_fd == -1, perror("open jpeg fail!"), return false);

        /*得到YUV各组件数据首地址*/
        uint8_t *py_buf = reader.data_bufff_;
        auto y_sum = reader.photo_width_ * reader.photo_height_;
        uint8_t *pu_buf;
        uint8_t *pv_buf;
        SplitData(py_buf, pu_buf, pv_buf, reader.photo_format_, y_sum);

        /*分配带写入的缓冲数据值*/
        byte_start_p_ = new uint8_t[y_sum];
        byte_p_ = byte_start_p_;
        bit_rest_ = 8;

        /*对用户输入的压缩质量规范化为合理值*/
        QualityScaling(quality);

        /*根据用户输入的压缩质量确定量化表各数值*/
        SetQuantTable(quality);

        /*确定数据DCT变换后Z字形量化的数值*/
        SetDctTable();

        /*建立Vli表*/
        BuildVliTable();

        /*建立YUV的DC,AC哈夫曼表*/
        BuildHuffTab();

        /*划分宏块以及基本单元数据块*/
        uint8_t w_scale = 1; // 宽度缩放因子
        uint8_t h_scale = 1; // 高度缩放因子
        if (reader.photo_format_ == kYuv420)
        {
            w_scale = w_scale << 1;
            h_scale = h_scale << 1;
        }
        else if (reader.photo_format_ == kYuv422)
        {
            w_scale = w_scale << 1;
        }
        uint16_t mcu_size = DCT_BLOCK_SIZE * w_scale * h_scale;
        uint32_t mcu_num = y_sum / mcu_size;
        DivBuff(py_buf, reader.photo_width_, reader.photo_height_, DCT_BLOCK_LEN * w_scale, DCT_BLOCK_LEN * h_scale);
        if (w_scale != 1 || h_scale != 1)
        {
            int offset = 0;
            for (int i = 0; i < mcu_num; i++)
            {
                DivBuff(py_buf + offset, DCT_BLOCK_LEN * w_scale, DCT_BLOCK_LEN * h_scale, DCT_BLOCK_LEN, DCT_BLOCK_LEN);
                offset += mcu_size;
            }
        }
        DivBuff(pu_buf, reader.photo_width_ / w_scale, reader.photo_height_ / h_scale, DCT_BLOCK_LEN, DCT_BLOCK_LEN);
        DivBuff(pv_buf, reader.photo_width_ / w_scale, reader.photo_height_ / h_scale, DCT_BLOCK_LEN, DCT_BLOCK_LEN);

        /*按宏块顺序处理数据块*/
        uint8_t yuv_counter[3] = {1, 1, 1};
        if (reader.photo_format_ == kYuv420)
        {
            yuv_counter[0] = 4;
        }
        else if (reader.photo_format_ == kYuv422)
        {
            yuv_counter[0] = 2;
        }
        ProcessMcu(py_buf, pu_buf, pv_buf, mcu_num, yuv_counter);

        /*带写入的各标志字段以及压缩编码后的数据*/
        App0 app;
        QT_8Bit y_qt(Y_QT_FLAG, quality);
        QT_8Bit uv_qt(UV_QT_FLAG, quality);
        Sof sof(reader.photo_height_, reader.photo_width_, reader.photo_format_);
        Dht dcy_dht(kDcYDht);
        Dht dcuv_dht(kDcUVDht);
        Dht acy_dht(kAcYDht);
        Dht acuv_dht(kAcUVDht);
        string str = "lio's tiny jpeg!";
        Comment com(std::move(str));
        Sos sos(reader.photo_format_);

        /*write SOI*/
        struct iovec iov[16];
        iov[0].iov_base = &kSoi;
        iov[0].iov_len = sizeof(kSoi);

        /*write APP0*/
        iov[1].iov_base = &app;
        iov[1].iov_len = sizeof(App0);

        /*write Y and UV quality table*/
        iov[2].iov_base = &y_qt;
        iov[2].iov_len = sizeof(QT_8Bit);
        iov[3].iov_base = &uv_qt;
        iov[3].iov_len = sizeof(QT_8Bit);

        /*write SOF*/
        iov[4].iov_base = &sof;
        iov[4].iov_len = sof.comp_num * 3 + 10;

        /*write DC and AC YUV diff-huffman table*/
        iov[5].iov_base = &dcy_dht;
        iov[5].iov_len = 5;
        iov[6].iov_base = dcy_dht.dht_p;
        iov[6].iov_len = dcy_dht.dht_len;

        iov[7].iov_base = &dcuv_dht;
        iov[7].iov_len = 5;
        iov[8].iov_base = dcuv_dht.dht_p;
        iov[8].iov_len = dcuv_dht.dht_len;

        iov[9].iov_base = &acy_dht;
        iov[9].iov_len = 5;
        iov[10].iov_base = acy_dht.dht_p;
        iov[10].iov_len = acy_dht.dht_len;

        iov[11].iov_base = &acuv_dht;
        iov[11].iov_len = 5;
        iov[12].iov_base = acuv_dht.dht_p;
        iov[12].iov_len = acuv_dht.dht_len;

        /*write SOS*/
        iov[13].iov_base = &sos;
        iov[13].iov_len = 8 + 2 * sos.comps_num;

        /*write jpeg code data*/
        iov[14].iov_base = byte_start_p_;
        iov[14].iov_len = byte_p_ - byte_start_p_;
        std::cout << "data len is " << iov[14].iov_len << std::endl;

        /*write EOI*/
        iov[15].iov_base = &kEoi;
        iov[15].iov_len = sizeof(kEoi);

        writev(file_fd, iov, 16);

        delete[] byte_start_p_;
        return true;
    }

private:
    uint8_t quality_y_table_[64];
    uint8_t quality_uv_table_[64];
    float dct_y_table_[64];
    float dct_uv_table_[64];
    HuffCode dc_y_dht_[12];
    HuffCode dc_uv_dht_[12];
    HuffCode ac_y_dht_[251] = {0};
    HuffCode ac_uv_dht_[251] = {0};
    uint8_t *byte_start_p_;
    uint8_t *byte_p_;
    uint8_t bit_rest_;
};
