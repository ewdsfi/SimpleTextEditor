#include "Document.h"

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

/// 进入程序前控制台的代码页，退出时还原，避免影响别的程序。
static unsigned int g_savedOutputCodePage = 0;
static unsigned int g_savedInputCodePage = 0;

/// 把控制台切到 UTF-8，否则中文会按 GBK 解释成乱码。
static void useUtf8Console()
{
#ifdef _WIN32
    // 先记下原来的代码页，退出时还原
    g_savedOutputCodePage = GetConsoleOutputCP();
    g_savedInputCodePage = GetConsoleCP();

    // 输入输出代码页都切成 UTF-8，让控制台按 UTF-8 解释程序写出的字节
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif
}

/// 还原控制台代码页。
static void restoreConsole()
{
#ifdef _WIN32
    if (g_savedOutputCodePage != 0) {
        SetConsoleOutputCP(g_savedOutputCodePage);
    }

    if (g_savedInputCodePage != 0) {
        SetConsoleCP(g_savedInputCodePage);
    }
#endif
}

/// 把一行命令按空白切开，引号内的空白不算分隔。
static std::vector<std::string> splitWords(const std::string &line)
{
    std::vector<std::string> words;
    std::string current;
    bool quoted = false;

    for (size_t i = 0; i < line.size(); i++) {
        const char c = line[i];

        if (c == '"') {
            quoted = !quoted;

            continue;
        }

        if (!quoted && (c == ' ' || c == '\t')) {
            if (!current.empty()) {
                words.push_back(current);
                current.clear();
            }

            continue;
        }

        current.push_back(c);
    }

    if (!current.empty()) {
        words.push_back(current);
    }

    return words;
}

/// 把转义写法还原成真实字符，支持 \n \t \\ \" 与 \r。
static std::string unescape(const std::string &text)
{
    std::string out;

    for (size_t i = 0; i < text.size(); i++) {
        if (text[i] != '\\' || i + 1 >= text.size()) {
            out.push_back(text[i]);

            continue;
        }

        i++;

        if (text[i] == 'n') {
            out.push_back('\n');
        } else if (text[i] == 't') {
            out.push_back('\t');
        } else if (text[i] == 'r') {
            out.push_back('\r');
        } else if (text[i] == '\\') {
            out.push_back('\\');
        } else if (text[i] == '"') {
            out.push_back('"');
        } else {
            out.push_back(text[i]);
        }
    }

    return out;
}

/// 解析 line:column 形式的坐标，同时兼容纯行号。
static bool parsePoint(const std::string &text, uint32_t &line, uint32_t &column)
{
    const size_t colon = text.find(':');

    if (colon == std::string::npos) {
        line = static_cast<uint32_t>(std::strtoul(text.c_str(), nullptr, 10));
        column = 0;

        return true;
    }

    line = static_cast<uint32_t>(std::strtoul(text.substr(0, colon).c_str(), nullptr, 10));
    column = static_cast<uint32_t>(std::strtoul(text.substr(colon + 1).c_str(), nullptr, 10));

    return true;
}

/// 打印一段文本，并把行尾标出来，便于确认换行位置。
static void printQuoted(const std::string &text)
{
    std::printf("\"");

    for (size_t i = 0; i < text.size(); i++) {
        if (text[i] == '\n') {
            std::printf("\\n");

            continue;
        }

        if (text[i] == '\t') {
            std::printf("\\t");

            continue;
        }

        std::printf("%c", text[i]);
    }

    std::printf("\"\n");
}

/// 打印用法。
static void printHelp()
{
    std::printf("可用命令：\n");
    std::printf("  open <路径>              打开文件\n");
    std::printf("  save [路径]              保存（带路径即另存为）\n");
    std::printf("  new                      清空并断开文件\n");
    std::printf("  info                     显示路径、行数、字节数、块数、树高\n");
    std::printf("  show [起行] [止行]       按行显示内容（行号从 0 开始）\n");
    std::printf("  line <行号>              显示某一行的内容\n");
    std::printf("  pos <行>:<列>            换算成字节下标\n");
    std::printf("  at <字节下标>            换算成行:列\n");
    std::printf("  find <文本> [起点]       查找并显示字节下标\n");
    std::printf("  insert <行>:<列> <文本>  在指定位置插入（\\n 表示换行）\n");
    std::printf("  erase <起点> <终点>      删除区间，坐标写法为 行:列\n");
    std::printf("  replace <起点> <终点> <文本>  替换区间\n");
    std::printf("  check                    校验红黑树不变量与全部统计量\n");
    std::printf("  help                     显示本说明\n");
    std::printf("  quit                     退出\n");
}

/// 处理一条命令，返回 false 表示要退出。
static bool runCommand(Document &document, const std::string &line)
{
    const std::vector<std::string> words = splitWords(line);

    if (words.empty()) {
        return true;
    }

    const std::string &command = words[0];
    TextBuffer *buffer = document.buffer();

    if (command == "quit" || command == "exit") {
        return false;
    }

    if (command == "help") {
        printHelp();

        return true;
    }

    if (command == "open") {
        if (words.size() < 2) {
            std::printf("用法：open <路径>\n");

            return true;
        }

        if (!document.load(words[1])) {
            std::printf("%s\n", document.error().c_str());

            return true;
        }

        std::printf("已打开 %s：%u 行 %u 字节\n", words[1].c_str(), buffer->lineCount(), buffer->byteCount());

        return true;
    }

    if (command == "save") {
        const bool ok = (words.size() >= 2) ? document.saveAs(words[1]) : document.save();

        if (!ok) {
            std::printf("%s\n", document.error().c_str());

            return true;
        }

        std::printf("已保存到 %s\n", document.path().c_str());

        return true;
    }

    if (command == "new") {
        document.createNew();
        std::printf("已清空\n");

        return true;
    }

    if (command == "info") {
        std::printf("文件：%s\n", document.named() ? document.path().c_str() : "（未命名）");
        std::printf("行数：%u\n", buffer->lineCount());
        std::printf("字节数：%u\n", buffer->byteCount());
        std::printf("最长行：%u 字节\n", buffer->longestLine());
        std::printf("块数量：%u\n", buffer->pieceCount());
        std::printf("树高度：%u\n", buffer->treeHeight());

        return true;
    }

    if (command == "show") {
        const uint32_t total = buffer->lineCount();
        const uint32_t from = (words.size() >= 2) ? static_cast<uint32_t>(std::strtoul(words[1].c_str(), nullptr, 10)) : 0;
        uint32_t to = (words.size() >= 3) ? static_cast<uint32_t>(std::strtoul(words[2].c_str(), nullptr, 10)) : from + 39;

        if (to >= total) {
            to = total - 1;
        }

        for (uint32_t number = from; number <= to && number < total; number++) {
            const uint32_t begin = buffer->lineBegin(number);

            std::printf("%6u [%8u] %s\n", number, begin, buffer->lineText(number).c_str());
        }

        return true;
    }

    if (command == "line") {
        if (words.size() < 2) {
            std::printf("用法：line <行号>\n");

            return true;
        }

        const uint32_t number = static_cast<uint32_t>(std::strtoul(words[1].c_str(), nullptr, 10));

        if (number >= buffer->lineCount()) {
            std::printf("行号超出范围，共 %u 行\n", buffer->lineCount());

            return true;
        }

        printQuoted(buffer->lineText(number));

        return true;
    }

    if (command == "pos") {
        if (words.size() < 2) {
            std::printf("用法：pos <行>:<列>\n");

            return true;
        }

        uint32_t row = 0;
        uint32_t column = 0;

        parsePoint(words[1], row, column);
        std::printf("字节下标：%u\n", buffer->offsetOf(row, column));

        return true;
    }

    if (command == "at") {
        if (words.size() < 2) {
            std::printf("用法：at <字节下标>\n");

            return true;
        }

        const uint32_t offset = static_cast<uint32_t>(std::strtoul(words[1].c_str(), nullptr, 10));
        const Position position = buffer->positionOf(offset);

        std::printf("%u:%u\n", position.line, position.column);

        return true;
    }

    if (command == "find") {
        if (words.size() < 2) {
            std::printf("用法：find <文本> [起点]\n");

            return true;
        }

        const std::string needle = unescape(words[1]);
        const uint32_t from = (words.size() >= 3) ? static_cast<uint32_t>(std::strtoul(words[2].c_str(), nullptr, 10)) : 0;
        const long hit = buffer->find(needle, from);

        if (hit < 0) {
            std::printf("未找到\n");

            return true;
        }

        const Position position = buffer->positionOf(static_cast<uint32_t>(hit));

        std::printf("字节下标 %ld，位置 %u:%u\n", hit, position.line, position.column);

        return true;
    }

    if (command == "insert") {
        if (words.size() < 3) {
            std::printf("用法：insert <行>:<列> <文本>\n");

            return true;
        }

        uint32_t row = 0;
        uint32_t column = 0;

        parsePoint(words[1], row, column);

        const std::string text = unescape(words[2]);
        const uint32_t offset = buffer->offsetOf(row, column);
        const uint32_t inserted = buffer->insert(offset, text);

        std::printf("已在 %u:%u（字节 %u）插入 %u 字节，文档共 %u 字节\n", row, column, offset, inserted,
                    buffer->byteCount());

        return true;
    }

    if (command == "erase") {
        if (words.size() < 3) {
            std::printf("用法：erase <起点> <终点>，坐标写法为 行:列\n");

            return true;
        }

        uint32_t fromRow = 0;
        uint32_t fromColumn = 0;
        uint32_t toRow = 0;
        uint32_t toColumn = 0;

        parsePoint(words[1], fromRow, fromColumn);
        parsePoint(words[2], toRow, toColumn);

        const uint32_t from = buffer->offsetOf(fromRow, fromColumn);
        const uint32_t to = buffer->offsetOf(toRow, toColumn);

        std::printf("已删除 %u 字节\n", buffer->erase(from, to));

        return true;
    }

    if (command == "replace") {
        if (words.size() < 4) {
            std::printf("用法：replace <起点> <终点> <文本>\n");

            return true;
        }

        uint32_t fromRow = 0;
        uint32_t fromColumn = 0;
        uint32_t toRow = 0;
        uint32_t toColumn = 0;

        parsePoint(words[1], fromRow, fromColumn);
        parsePoint(words[2], toRow, toColumn);

        const uint32_t from = buffer->offsetOf(fromRow, fromColumn);
        const uint32_t to = buffer->offsetOf(toRow, toColumn);
        const std::string text = unescape(words[3]);

        std::printf("已替换 %u 字节\n", buffer->replace(from, to, text));

        return true;
    }

    if (command == "check") {
        const char *where = nullptr;

        if (buffer->check(where)) {
            std::printf("红黑树不变量与统计量全部成立（%u 块，树高 %u）\n", buffer->pieceCount(), buffer->treeHeight());

            return true;
        }

        std::printf("校验失败：%s\n", where);

        return true;
    }

    std::printf("未知命令：%s，输入 help 查看用法\n", command.c_str());

    return true;
}

/// 读一行命令，并去掉可能存在于开头的 BOM。
static bool readCommand(std::string &line)
{
    if (!std::getline(std::cin, line)) {
        return false;
    }

    // 脚本文件常带 UTF-8 BOM，不去掉会污染第一条命令
    if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF
        && static_cast<unsigned char>(line[1]) == 0xBB && static_cast<unsigned char>(line[2]) == 0xBF) {
        line.erase(0, 3);
    }

    return true;
}

/// 程序入口：可以带一个文件名，也可以带 --script 从标准输入读命令。
int main(int argc, char *argv[])
{
    useUtf8Console();

    Document document;

    if (argc >= 2 && std::string(argv[1]) != "--script") {
        if (!document.load(argv[1])) {
            std::printf("%s\n", document.error().c_str());
        }
    }

    // 脚本模式：从标准输入逐行读命令，不回显提示符
    const bool script = (argc >= 2 && std::string(argv[1]) == "--script") || (argc >= 3 && std::string(argv[2]) == "--script");

    if (script) {
        if (argc >= 3 && std::string(argv[1]) != "--script") {
            document.load(argv[1]);
        }

        std::string line;

        while (readCommand(line)) {
            if (!runCommand(document, line)) {
                break;
            }
        }

        restoreConsole();

        return 0;
    }

    std::printf("精简版文本编辑器（红黑树块表）。输入 help 查看命令，quit 退出。\n");

    if (document.named()) {
        std::printf("已打开 %s：%u 行 %u 字节\n", document.path().c_str(), document.buffer()->lineCount(),
                    document.buffer()->byteCount());
    }

    std::string line;

    while (true) {
        std::printf("> ");
        std::fflush(stdout);

        if (!readCommand(line)) {
            break;
        }

        if (!runCommand(document, line)) {
            break;
        }
    }

    restoreConsole();

    return 0;
}
