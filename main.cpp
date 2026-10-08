#include "Document.h"

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

/// The console code pages we found on the way in, restored on the way out so we
/// don't leave the user's terminal in a weird state for the next program.
static unsigned int g_savedOutputCodePage = 0;
static unsigned int g_savedInputCodePage = 0;

/// Switches the console over to UTF-8, otherwise Chinese comes out as mojibake
/// because the console happily interprets our UTF-8 bytes as GBK.
static void useUtf8Console()
{
#ifdef _WIN32
    // remember what we are about to clobber, restoreConsole() needs it later
    g_savedOutputCodePage = GetConsoleOutputCP();
    g_savedInputCodePage = GetConsoleCP();

    // both directions, not just output: we also type UTF-8 paths and search text
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif
}

/// Restores the console code pages.
static void restoreConsole()
{
#ifdef _WIN32
    // the zero check guards against the case where useUtf8Console() never got to
    // save anything -- restoring a NULL code page would be worse than doing nothing
    if (g_savedOutputCodePage != 0) {
        SetConsoleOutputCP(g_savedOutputCodePage);
    }

    if (g_savedInputCodePage != 0) {
        SetConsoleCP(g_savedInputCodePage);
    }
#endif
}

/// Splits a command line on whitespace, but whitespace inside quotes does not
/// count as a separator.
/// the quote characters themselves are thrown away, so "a b" arrives as one
/// word and you can't get a literal quote into a filename this way. there is no
/// escaping in here -- backslashes are handled later, by unescape().
static std::vector<std::string> splitWords(const std::string &line)
{
    std::vector<std::string> words;
    std::string current;
    bool quoted = false;

    for (size_t i = 0; i < line.size(); i++) {
        const char c = line[i];

        // a quote just flips the state, it never ends up in a word
        if (c == '"') {
            quoted = !quoted;

            continue;
        }

        // only unquoted spaces and tabs break a word, quoted ones are content
        if (!quoted && (c == ' ' || c == '\t')) {
            if (!current.empty()) {
                words.push_back(current);
                current.clear();
            }

            continue;
        }

        current.push_back(c);
    }

    // the last word has no trailing separator to flush it, so flush it here
    if (!current.empty()) {
        words.push_back(current);
    }

    return words;
}

/// Turns escape sequences back into real characters, supporting \n \t \\ \"
/// and \r.
/// an unknown escape keeps the character after the backslash as-is (the
/// backslash is dropped), which is friendlier than refusing the whole command.
/// watch out: a trailing lone backslash survives as a backslash, there is no
/// "incomplete escape" error.
static std::string unescape(const std::string &text)
{
    std::string out;

    for (size_t i = 0; i < text.size(); i++) {
        // not an escape, or a backslash with nothing behind it -- copy as-is
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

/// Parses a "line:column" coordinate, and also accepts a bare line number.
/// both forms always succeed: a bare "3" is line 3 column 0, and anything
/// strtoul can't read simply becomes 0. so a typo like "abc" quietly means
/// 0:0 instead of complaining -- the buffer's own range checks are what keep
/// us honest.
static bool parsePoint(const std::string &text, uint32_t &line, uint32_t &column)
{
    const size_t colon = text.find(':');

    // no colon means the whole word is a line number, column defaults to 0
    if (colon == std::string::npos) {
        line = static_cast<uint32_t>(std::strtoul(text.c_str(), nullptr, 10));
        column = 0;

        return true;
    }

    line = static_cast<uint32_t>(std::strtoul(text.substr(0, colon).c_str(), nullptr, 10));
    column = static_cast<uint32_t>(std::strtoul(text.substr(colon + 1).c_str(), nullptr, 10));

    return true;
}

/// Prints a piece of text with its line ends marked, so you can tell where the
/// newlines actually are.
/// we escape '\n' and '\t' instead of printing them raw: a raw newline would
/// quietly turn one line of output into two and make the line count in your head
/// wrong, and a raw tab would wreck the terminal alignment.
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

/// Prints the usage text.
static void printHelp()
{
    std::printf("可用命令：\n");
    std::printf("  open <路径>              打开文件\n");
    std::printf("  save                     保存回当前文件\n");
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

/// Handles one command, returning false when it is time to quit.
static bool runCommand(Document &document, const std::string &line)
{
    const std::vector<std::string> words = splitWords(line);

    // a blank line (or one that was only spaces) is not an error, just nothing
    // to do -- this matters a lot for script files with trailing newlines
    if (words.empty()) {
        return true;
    }

    const std::string &command = words[0];
    TextBuffer *buffer = document.buffer();

    // "exit" is an alias people type by reflex, we may as well accept it
    if (command == "quit" || command == "exit") {
        return false;
    }

    if (command == "help") {
        printHelp();

        return true;
    }

    // ---- open ----
    // loads the file and replaces everything in the buffer, unsaved edits and all
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

    // ---- save ----
    // writes back to the current path only, there is no Save As
    if (command == "save") {
        // the usual reason this fails is that the document is still unnamed
        if (!document.save()) {
            std::printf("%s\n", document.error().c_str());

            return true;
        }

        std::printf("已保存到 %s\n", document.path().c_str());

        return true;
    }

    // ---- new ----
    // wipes the buffer AND forgets the path, so a stray save can't clobber the
    // file you were just working on
    if (command == "new") {
        document.createNew();
        std::printf("已清空\n");

        return true;
    }

    // ---- info ----
    // a quick health report for the piece tree: line/byte counts, piece count
    // and tree height are what you watch when you are hunting for a bad split
    if (command == "info") {
        std::printf("文件：%s\n", document.named() ? document.path().c_str() : "（未命名）");
        std::printf("行数：%u\n", buffer->lineCount());
        std::printf("字节数：%u\n", buffer->byteCount());
        std::printf("最长行：%u 字节\n", buffer->longestLine());
        std::printf("块数量：%u\n", buffer->pieceCount());
        std::printf("树高度：%u\n", buffer->treeHeight());

        return true;
    }

    // ---- show ----
    // dumps a range of lines with their starting byte offset; the whole point is
    // to eyeball offsets, so don't be surprised that it prints line numbers too
    if (command == "show") {
        const uint32_t total = buffer->lineCount();
        const uint32_t from = (words.size() >= 2) ? static_cast<uint32_t>(std::strtoul(words[1].c_str(), nullptr, 10)) : 0;
        uint32_t to = (words.size() >= 3) ? static_cast<uint32_t>(std::strtoul(words[2].c_str(), nullptr, 10)) : from + 39;

        // no end line given means "40 lines from the start", clamp it so we
        // don't run off the end of the document
        if (to >= total) {
            to = total - 1;
        }

        // the number < total guard matters: 'to' is uint32_t, so when the
        // document has 0 lines the clamp above underflows to UINT_MAX and only
        // this guard keeps us from printing forever
        for (uint32_t number = from; number <= to && number < total; number++) {
            const uint32_t begin = buffer->lineBegin(number);

            std::printf("%6u [%8u] %s\n", number, begin, buffer->lineText(number).c_str());
        }

        return true;
    }

    // ---- line ----
    // prints a single line through printQuoted() so invisible line ends show up
    if (command == "line") {
        if (words.size() < 2) {
            std::printf("用法：line <行号>\n");

            return true;
        }

        const uint32_t number = static_cast<uint32_t>(std::strtoul(words[1].c_str(), nullptr, 10));

        // we range check here ourselves, the buffer is only too happy to read
        // whatever offset you ask it for
        if (number >= buffer->lineCount()) {
            std::printf("行号超出范围，共 %u 行\n", buffer->lineCount());

            return true;
        }

        printQuoted(buffer->lineText(number));

        return true;
    }

    // ---- pos ----
    // line:column -> byte offset, the direction the buffer calls offsetOf()
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

    // ---- at ----
    // byte offset -> line:column, the inverse of pos, and it is what you reach
    // for after find hands you a raw offset
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

    // ---- find ----
    // searches for the unescaped text, so "\n" in the command line really does
    // search for a newline instead of the two characters backslash-n
    if (command == "find") {
        if (words.size() < 2) {
            std::printf("用法：find <文本> [起点]\n");

            return true;
        }

        const std::string needle = unescape(words[1]);
        const uint32_t from = (words.size() >= 3) ? static_cast<uint32_t>(std::strtoul(words[2].c_str(), nullptr, 10)) : 0;
        const long hit = buffer->find(needle, from);

        // find() reports "not found" as a negative offset, that is the whole
        // contract -- there is no separate bool
        if (hit < 0) {
            std::printf("未找到\n");

            return true;
        }

        // show both the raw offset and the human position, so you can copy
        // either one straight into at / pos
        const Position position = buffer->positionOf(static_cast<uint32_t>(hit));

        std::printf("字节下标 %ld，位置 %u:%u\n", hit, position.line, position.column);

        return true;
    }

    // ---- insert ----
    // the text comes as one single word, so quote it if it contains spaces;
    // use \n inside it to actually create new lines
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

    // ---- erase ----
    // both ends are line:column, not raw offsets (use pos first if you only have
    // offsets in hand)
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

    // ---- replace ----
    // erase + insert in one shot, so the interval never exists in a half-erased
    // state and the statistics stay consistent
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

    // ---- check ----
    // walks the whole tree and re-derives every count, so it is slow but it is
    // the thing you run after touching the rotations
    if (command == "check") {
        // 'where' is written by check() and points at a string literal, we only
        // read it when the check actually failed
        const char *where = nullptr;

        if (buffer->check(where)) {
            std::printf("红黑树不变量与统计量全部成立（%u 块，树高 %u）\n", buffer->pieceCount(), buffer->treeHeight());

            return true;
        }

        std::printf("校验失败：%s\n", where);

        return true;
    }

    // ---- unknown ----
    // everything else is a typo, don't guess at it -- just point at help
    std::printf("未知命令：%s，输入 help 查看用法\n", command.c_str());

    return true;
}

/// Reads one line of command, dropping a BOM that may be sitting at the head.
static bool readCommand(std::string &line)
{
    if (!std::getline(std::cin, line)) {
        return false;
    }

    // script files are often saved with a UTF-8 BOM, and we read stdin as a plain
    // byte stream -- left alone those three bytes would glue themselves to the
    // first command and make "open" look like "\xEF\xBB\xBFopen"
    if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF
        && static_cast<unsigned char>(line[1]) == 0xBB && static_cast<unsigned char>(line[2]) == 0xBF) {
        line.erase(0, 3);
    }

    return true;
}

/// Entry point: optionally takes a file name, or --script to read commands from
/// stdin.
int main(int argc, char *argv[])
{
    useUtf8Console();

    Document document;

    // a plain file name on the command line is opened right away; a failed open
    // is only reported, we still drop into the prompt
    if (argc >= 2 && std::string(argv[1]) != "--script") {
        if (!document.load(argv[1])) {
            std::printf("%s\n", document.error().c_str());
        }
    }

    // Script mode: read commands from stdin line by line, no prompt echo.
    // "--script" can be either the first or the second argument, the second slot
    // exists so that "editor <file> --script" also works
    const bool script = (argc >= 2 && std::string(argv[1]) == "--script") || (argc >= 3 && std::string(argv[2]) == "--script");

    if (script) {
        // in the "editor <file> --script" form --script sits in argv[2], so the
        // load above never ran; do it here before we start eating commands
        if (argc >= 3 && std::string(argv[1]) != "--script") {
            document.load(argv[1]);
        }

        std::string line;

        // no prompt, no fflush -- just run every command until EOF or quit
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

    // Interactive loop: print the prompt, flush it (stdout is buffered when it
    // is a pipe), then wait for a line. EOF on stdin ends us just as cleanly as
    // "quit" does
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
