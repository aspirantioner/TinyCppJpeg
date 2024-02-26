#include "jpeg.hpp"

int main()
{
    string src_file_path = "./test.yuv";
    PhotoReader photo_re(std::move(src_file_path));
    photo_re.SureHandW(1280, 720);
    photo_re.SureFormat(kYuv420);
    ERROR_IF(!photo_re.Read(), "read error!", return -1);

    string dst_file_path = "./test.jpg";
    JpegDecoder jpeg_coder;
    jpeg_coder.Decode(photo_re, 100, dst_file_path);

    return 0;
}