#include <iostream>
using namespace std;

/*将一定bit长度的码值写入缓冲区*/
void WriteBitsStream(uint16_t value, uint8_t code_len)
{
    // value = htobe16(value);
    if (code_len > 8)
    {
        std::cout << "code" << value << " len is " << (int)code_len << std::endl;
    }
    /*写入的码值bit长度大于字节剩余的bit长度*/
    while (bit_rest_ < code_len)
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
int main(int argc, char *argv[])
{

    return 0;
}
