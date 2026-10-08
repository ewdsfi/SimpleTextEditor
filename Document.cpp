#include "Document.h"

#include <cstdio>

Document::Document()
{
    _path.clear();
    _error.clear();
}

bool Document::load(const std::string &path)
{
    std::FILE *file = std::fopen(path.c_str(), "rb");

    if (file == nullptr) {
        _error = "打不开文件：" + path;

        return false;
    }

    std::string data;
    char chunk[8192];

    while (true) {
        const size_t got = std::fread(chunk, 1, sizeof(chunk), file);

        if (got > 0) {
            data.append(chunk, got);
        }

        if (got < sizeof(chunk)) {
            break;
        }
    }

    std::fclose(file);

    // 带 BOM 的 UTF-8 文件先去掉 BOM，正文按原样进入缓冲区
    if (data.size() >= 3 && static_cast<unsigned char>(data[0]) == 0xEF
        && static_cast<unsigned char>(data[1]) == 0xBB && static_cast<unsigned char>(data[2]) == 0xBF) {
        data.erase(0, 3);
    }

    _buffer.clear();
    _buffer.insert(0, data);
    _path = path;
    _error.clear();

    return true;
}

bool Document::save()
{
    if (_path.empty()) {
        _error = "尚未指定文件名，请先用 saveas";

        return false;
    }

    return saveAs(_path);
}

bool Document::saveAs(const std::string &path)
{
    std::FILE *file = std::fopen(path.c_str(), "wb");

    if (file == nullptr) {
        _error = "写不了文件：" + path;

        return false;
    }

    const std::string data = _buffer.text();

    if (!data.empty() && std::fwrite(data.data(), 1, data.size(), file) != data.size()) {
        std::fclose(file);
        _error = "写入过程中出错：" + path;

        return false;
    }

    std::fclose(file);

    _path = path;
    _error.clear();

    return true;
}

void Document::createNew()
{
    _buffer.clear();
    _path.clear();
    _error.clear();
}
