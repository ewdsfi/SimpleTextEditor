#ifndef DOCUMENT_H
#define DOCUMENT_H

#include "TextBuffer.h"

#include <string>

/// 文档：把文本缓冲区包装成可读写的文件实体。
/// 只做最基础的文件读写，不做编码识别、不做换行风格记录、不做撤销，
/// 因此读写都是「原样进出」——保存出去的字节与打开时读到的内容一一对应。
class Document
{
public:
    /// 创建一个空文档。
    Document();

    /// 读取文件并替换当前内容。
    bool load(const std::string &path);

    /// 保存到当前路径，未命名时返回 false。
    bool save();

    /// 另存到指定路径，成功后切换当前路径。
    bool saveAs(const std::string &path);

    /// 清空内容并解除文件关联。
    void createNew();

    /// 文本缓冲区。
    TextBuffer *buffer() { return &_buffer; }

    /// 当前文件路径，未命名时为空串。
    const std::string &path() const { return _path; }

    /// 读写过程中的错误信息，供控制台显示。
    const std::string &error() const { return _error; }

    /// 是否已关联文件。
    bool named() const { return !_path.empty(); }

private:
    TextBuffer _buffer;  ///< 文本缓冲区
    std::string _path;   ///< 当前文件路径
    std::string _error;  ///< 最近一次错误
};

#endif // DOCUMENT_H
