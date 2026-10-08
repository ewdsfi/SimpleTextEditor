#include "Document.h"

#include <cstdio>

/// Converts buffer text (pure LF) to on-disk text, which means padding LF into
/// CRLF when we are on Windows.
/// this is the only place in the whole program that knows about '\r', don't
/// spread the idea of CRLF anywhere else -- the buffer stays LF forever.
static std::string toDiskText(const std::string &text)
{
#ifdef _WIN32
    std::string out;

    // every '\n' grows by one byte, so this is the worst case estimate
    out.reserve(text.size() + text.size() / 16);

    // walk the text and inject the missing '\r' by hand, we can't use replace()
    // here because we are growing the string while reading it
    for (size_t i = 0; i < text.size(); i++) {
        if (text[i] == '\n') {
            out.push_back('\r');
        }

        out.push_back(text[i]);
    }

    return out;
#else
    // non-Windows platforms are happy with plain LF, so this is a no-op copy
    return text;
#endif
}

Document::Document()
{
    _path.clear();
    _error.clear();
}

bool Document::load(const std::string &path)
{
    // binary mode on purpose: if the runtime translated newlines for us we would
    // get CRLF in the buffer and every byte offset would be off
    std::FILE *file = std::fopen(path.c_str(), "rb");

    if (file == nullptr) {
        _error = "打不开文件：" + path;

        return false;
    }

    std::string data;
    char chunk[8192];

    // read the whole thing in chunks, we have no idea how big the file is and a
    // million-character single line is a perfectly normal thing to open here
    while (true) {
        const size_t got = std::fread(chunk, 1, sizeof(chunk), file);

        if (got > 0) {
            data.append(chunk, got);
        }

        // anything short of a full chunk means we hit EOF (or an error), either
        // way there is nothing more to read. note we never bail out on a read
        // error -- a truncated file just loads as a truncated buffer
        if (got < sizeof(chunk)) {
            break;
        }
    }

    std::fclose(file);

    // strip a UTF-8 BOM if there is one, it would otherwise show up as three
    // garbage bytes at the very start of the first line
    if (data.size() >= 3 && static_cast<unsigned char>(data[0]) == 0xEF
        && static_cast<unsigned char>(data[1]) == 0xBB && static_cast<unsigned char>(data[2]) == 0xBF) {
        data.erase(0, 3);
    }

    // we swap in the new content only after the whole file is in memory, so an
    // open that failed leaves the old document untouched. to be clear: this only
    // protects against fopen failing, a read error halfway through still replaces
    // the document with the partial text. we do NOT translate CRLF back to LF,
    // the buffer keeps whatever bytes were in the file
    _buffer.clear();
    _buffer.insert(0, data);
    _path = path;
    _error.clear();

    return true;
}

bool Document::save()
{
    // WATCH OUT: no name means no file to write to, and since there is no Save
    // As, the user has to run open (or new) before this can ever succeed
    if (_path.empty()) {
        _error = "尚未指定文件名，请先用 open 打开一个文件";

        return false;
    }

    // "wb" truncates the existing file the moment it opens, so an empty document
    // really does empty the file on disk
    std::FILE *file = std::fopen(_path.c_str(), "wb");

    if (file == nullptr) {
        _error = "写不了文件：" + _path;

        return false;
    }

    // expand to CRLF here, not a byte earlier -- the buffer only ever holds LF
    const std::string data = toDiskText(_buffer.text());

    // fwrite can come back short on a full disk, and a short write is a failed
    // save, so don't just trust the return value to be all-or-nothing
    if (!data.empty() && std::fwrite(data.data(), 1, data.size(), file) != data.size()) {
        std::fclose(file);
        _error = "写入过程中出错：" + _path;

        return false;
    }

    std::fclose(file);

    _error.clear();

    return true;
}

void Document::createNew()
{
    // drop the content, and drop the path with it -- an unnamed document can't
    // be saved, that is the whole point of "new"
    _buffer.clear();
    _path.clear();
    _error.clear();
}
