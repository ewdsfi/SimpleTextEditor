#include "LineMap.h"

#include <QFontMetrics>

#include <algorithm>
#include <cassert>

/// ASCII 的推进量一次全部测好；之后每碰到一个西文字符都不用再问字体
static const int AsciiCount = 128;

/// 解出 bytes[pos] 处的字符，返回它占几个字节。
/// 自己动手解码而不是用 QTextCodec / QStringConverter：少一个依赖，也顺手保证
/// "坏字节只吃一个字节"，绝不会读到半个字符上去
static int decodeUtf8(const char *bytes, int size, int pos, uint32_t &code)
{
    const unsigned char lead = static_cast<unsigned char>(bytes[pos]);

    if (lead < 0x80) {
        code = lead;

        return 1;
    }

    int extra = 0;
    uint32_t value = 0;

    if ((lead & 0xE0) == 0xC0) {
        extra = 1;
        value = lead & 0x1Fu;
    } else if ((lead & 0xF0) == 0xE0) {
        extra = 2;
        value = lead & 0x0Fu;
    } else if ((lead & 0xF8) == 0xF0) {
        extra = 3;
        value = lead & 0x07u;
    } else {
        // 孤立的续字节或者 0xF8 以上：当坏字节，只吃一个字节
        code = 0xFFFD;

        return 1;
    }

    // 尾巴不够长就直接认坏，不要越界读
    if (pos + extra >= size) {
        code = 0xFFFD;

        return 1;
    }

    for (int k = 1; k <= extra; k++) {
        const unsigned char next = static_cast<unsigned char>(bytes[pos + k]);

        // 续字节必须形如 10xxxxxx
        if ((next & 0xC0) != 0x80) {
            code = 0xFFFD;

            return 1;
        }

        value = (value << 6) | (next & 0x3Fu);
    }

    code = value;

    return extra + 1;
}

/// 一个 UTF-8 字符占几个字节，输入是它的引导字节（坏字节也算一个）
static int utf8Length(unsigned char lead)
{
    if (lead < 0x80) {
        return 1;
    }

    if ((lead & 0xE0) == 0xC0) {
        return 2;
    }

    if ((lead & 0xF0) == 0xE0) {
        return 3;
    }

    if ((lead & 0xF8) == 0xF0) {
        return 4;
    }

    return 1;
}

/// 把一个码位追加到 UTF-16 字符串上
static void appendCodePoint(QString &out, uint32_t code)
{
    // 越过 Unicode 上限或者落在代理区：一律按替换字符处理，绝不产出非法 UTF-16
    if (code > 0x10FFFFu || (code >= 0xD800u && code <= 0xDFFFu)) {
        code = 0xFFFD;
    }

    if (code >= 0x10000u) {
        // 基本平面之外，拆成一对代理项。注意这里是两个 QChar，
        // 它们在 visible 里仍然属于同一个定位单位（同一个 Loc）
        const uint32_t rest = code - 0x10000u;

        out.append(QChar(static_cast<char16_t>(0xD800u + (rest >> 10))));
        out.append(QChar(static_cast<char16_t>(0xDC00u + (rest & 0x3FFu))));

        return;
    }

    out.append(QChar(static_cast<char16_t>(code)));
}

LineMap::Metrics LineMap::makeMetrics(const QFontMetrics &fm)
{
    Metrics metrics;

    metrics.font = &fm;
    metrics.advance.resize(AsciiCount);

    for (int c = 0; c < AsciiCount; c++) {
        // 制表符的"真实宽度"没有意义，位置交给下面的 tabWidth 统一处理
        int width = 0;

        if (c != '\t') {
            width = static_cast<int>(fm.horizontalAdvance(QChar(static_cast<char16_t>(c))));
        }

        width = std::max(0, std::min(width, 65535));

        metrics.advance[c] = static_cast<quint16>(width);
    }

    // 等宽字体里 'M' 和 '0' 一样宽，挑一个稳的当半角宽度
    metrics.codeWidth = metrics.advance['M'] > 0 ? metrics.advance['M'] : metrics.advance['0'];

    if (metrics.codeWidth <= 0) {
        // 字体什么都没量出来（极端情况），退回一个不会把界面撑坏的默认值
        metrics.codeWidth = 8;
    }

    // 制表符按 4 个半角宽度算。故意用固定宽度而不是"对齐到第 4 列"：
    // 固定宽度才能保证"一个字符占多宽"只由它自己决定，缓存才立得住
    metrics.tabWidth = metrics.codeWidth * 4;

    return metrics;
}

QString LineMap::decode(const std::string &text)
{
    const char *bytes = text.data();
    const int size = static_cast<int>(text.size());

    QString out;

    out.reserve(size);

    int pos = 0;

    while (pos < size) {
        // 快车道：ASCII 一字节一字符，超长纯英文行走的就是这里
        const unsigned char lead = static_cast<unsigned char>(bytes[pos]);

        if (lead < 0x80) {
            out.append(QChar(static_cast<char16_t>(lead)));
            pos++;

            continue;
        }

        uint32_t code = 0;

        pos += decodeUtf8(bytes, size, pos, code);

        appendCodePoint(out, code);
    }

    return out;
}

/// 表外字符的宽度：先查记忆表，没有再问字体，问完记住。
/// 抽出来是因为 buildBlock() 里会反复量同一个汉字
static int widthOfWideChar(const LineMap::Metrics &metrics, quint32 code)
{
    if (metrics.wideAdvance.contains(code)) {
        return metrics.wideAdvance.value(code);
    }

    int width = 0;

    if (metrics.font != nullptr) {
        QString one;

        appendCodePoint(one, code);

        width = static_cast<int>(metrics.font->horizontalAdvance(one));
    }

    width = std::max(0, width);

    metrics.wideAdvance.insert(code, static_cast<quint16>(std::min(width, 65535)));

    return width;
}

LineMap LineMap::build(quint32 lineBeginByte, const std::string &text, const Metrics &metrics)
{
    LineMap map;

    map.m_lineBegin = lineBeginByte;
    map.m_byteLength = static_cast<quint32>(text.size());
    map.m_metrics = &metrics;

    // 留一份自己的副本：视图随时来问定位，都不会读到已经被释放的字符串
    map.m_text = text;

    // 铺字符表（顺便把可见文本铺出来）。宽度一个都不量，那是 ensureBuilt() 的事
    map.resetEntries();

    return map;
}

void LineMap::refresh(const std::string &text)
{
    // 同一行被编辑过：文本换了，字符表和所有量测结果都得作废重建。
    // 注意块表也要重建 —— 行的字节长度可能变了，块的数量跟着变
    m_text = text;
    m_byteLength = static_cast<quint32>(text.size());

    resetEntries();
}

void LineMap::resetEntries()
{
    // 最坏情况每个字节一个字符，按字节数预留，后面不再扩容
    const int rough = static_cast<int>(m_byteLength) + 1;

    m_byte.clear();
    m_char.clear();
    m_loc.clear();
    m_blocks.clear();
    m_visible.clear();

    m_byte.reserve(rough);
    m_char.reserve(rough);
    m_loc.reserve(rough);
    m_visible.reserve(rough);

    const char *bytes = m_text.data();
    const int size = static_cast<int>(m_text.size());

    // 游标：已经处理到第几个字节，同时也是下一个定位单位的起始字节
    uint32_t pos = 0;

    while (static_cast<int>(pos) < size) {
        const unsigned char lead = static_cast<unsigned char>(bytes[pos]);

        uint32_t code = 0;
        int used = 1;

        if (lead < 0x80) {
            code = lead;
        } else {
            used = decodeUtf8(bytes, size, static_cast<int>(pos), code);
        }

        if (code == '\t') {
            // 制表符：定位单位还是它自己，但屏幕上占 tabWidth 个像素，
            // 也就是展开成 tabWidth / 半角宽度 个空格
            const int spaces = (m_metrics->codeWidth > 0) ? (m_metrics->tabWidth / m_metrics->codeWidth) : 4;

            m_visible.append(QString(std::max(0, spaces), QChar(u' ')));
        } else {
            appendCodePoint(m_visible, code);
        }

        m_byte.append(pos);
        m_char.append(static_cast<quint32>(m_visible.size()));

        pos += static_cast<uint32_t>(used);
    }

    // 末尾补一个"收尾单位"：宽度固定为 0，字节列号就是行尾。
    // 有了它，任何一列都能写成 [m_byte[i], m_byte[i+1]) 这样的半开区间，
    // 全行最后一个字符也不用特殊照顾；它的左边缘正好就是这一行的宽度
    m_byte.append(m_byteLength);
    m_char.append(static_cast<quint32>(m_visible.size()));

    // 块表的骨架：长度只跟字节数有关，跟字符数无关。
    // 注意 byteLength 正好等于块边界时会多出最后一块，那是给收尾单位准备的
    const int blockCount = blockIndexForColumn(m_byteLength) + 1;

    for (int i = 0; i < blockCount; i++) {
        Block block;

        m_blocks.push_back(block);
    }

    m_built = 0;
}

int LineMap::blockIndexForColumn(quint32 byteColumn)
{
    // 块的起始字节 -> 块序号（按 EntrySpan 对齐，整除就是）
    return static_cast<int>(byteColumn / EntrySpan);
}

quint32 LineMap::blockEndColumn(int index)
{
    if (index < 0) {
        return 0;
    }

    return static_cast<quint32>(index + 1) * EntrySpan;
}

quint32 LineMap::blockStartOf(int index) const
{
    return static_cast<quint32>(index) * EntrySpan;
}

int LineMap::firstEntryAtOrAfter(quint32 byteColumn) const
{
    // 因为 EntrySpan 是 2 的幂，"第 k*4096 个字节"这个位置正好就是第 k*4096 个
    // 字符下标：下标小的那些字符必然都在它前面（多字节字符只会让下标更小）。
    // 于是这里不需要从头扫，直接跳到块头再往右走几步
    int index = blockIndexForColumn(byteColumn) * static_cast<int>(EntrySpan);

    if (index > m_byte.size()) {
        index = m_byte.size();
    }

    while (index > 0 && index - 1 < m_byte.size() && m_byte[index - 1] >= byteColumn) {
        index--;
    }

    while (index < m_byte.size() && m_byte[index] < byteColumn) {
        index++;
    }

    return index;
}

void LineMap::ensureBuilt(int index) const
{
    if (m_blocks.isEmpty()) {
        return;
    }

    // 行尾所在的块一定是最后一块，一次量到那儿
    if (index >= m_blocks.size()) {
        index = m_blocks.size() - 1;
    }

    while (m_built <= index) {
        // 块头 x = 上一块的左边缘 + 上一块的宽度（第一块从 0 开始）。
        // 块的起止字符由 buildBlock() 自己按字节边界算，不依赖任何跨块游标
        m_blocks[m_built].x = (m_built == 0) ? 0 : (m_blocks[m_built - 1].x + m_blocks[m_built - 1].width);

        buildBlock(m_built);

        m_built++;
    }
}

void LineMap::buildBlock(int index) const
{
    const quint32 start = blockStartOf(index);
    const quint32 end = blockEndColumn(index);

    // 这一块从哪个字符开始：第一个字节能达到块起点的字符。
    // 块边界可能切在汉字中间，那种字符归前面那一块，所以这里要的是"达到"
    const int from = firstEntryAtOrAfter(start);

    // 到哪里为止：第一个字节能达到块终点的字符
    const int stop = firstEntryAtOrAfter(end);

    m_blocks[index].startEntry = from;
    m_blocks[index].endEntry = stop;

    // m_loc 用"相对块头的字节偏移"当下标，所以这一块要开多大先算清楚
    quint32 span = 0;

    if (stop > from && from < m_byte.size()) {
        span = m_byte[stop - 1] - start + 1;
    }

    if (m_loc.size() < static_cast<int>(span)) {
        m_loc.resize(static_cast<int>(span));
    }

    // 逐字符累加宽度。x 只记"相对块头"的偏移，Loc 里两个字段都是 4 字节，稳
    int x = 0;

    for (int i = from; i < stop; i++) {
        const quint32 at = m_byte[i];

        Loc loc;

        loc.byte = at - start;
        loc.x = static_cast<quint32>(std::max(0, x));

        int width = 0;

        if (at >= m_byteLength) {
            // 收尾单位（行尾那一项）：宽度必须是 0。它的左边缘就是行宽，
            // 再给它宽度的话 width() 会凭空多出一个字符
            width = 0;
        } else {
            const unsigned char lead = static_cast<unsigned char>(m_text[at]);

            if (lead == '\t') {
                width = m_metrics->tabWidth;
            } else if (lead < 0x80) {
                // 半角字符查表，一次数组读取
                width = m_metrics->advance[lead];
            } else {
                // 多字节字符（汉字、全角标点…）才去问字体，而且问一次就记进表里
                uint32_t code = 0;

                decodeUtf8(m_text.data(), static_cast<int>(m_text.size()), static_cast<int>(at), code);

                width = widthOfWideChar(*m_metrics, code);
            }
        }

        width = std::max(0, width);

        loc.width = static_cast<quint32>(width);

        // m_loc 的下标恒等于"相对块头的字节偏移"，所以块内定位是一次直接查表
        const int slot = static_cast<int>(loc.byte);

        if (slot >= m_loc.size()) {
            m_loc.resize(slot + 1);
        }

        m_loc[slot] = loc;

        // 下一个字符的左边缘 = 这个字符的左边缘 + 它自己的宽度
        x += width;
    }

    // 这一块的宽度 = 最后一个字符的相对 x + 它的宽度
    if (stop > from && from < m_byte.size()) {
        const quint32 offset = m_byte[stop - 1] - start;

        m_blocks[index].width = (offset < static_cast<quint32>(m_loc.size()))
                                    ? static_cast<int>(m_loc[static_cast<int>(offset)].x + m_loc[static_cast<int>(offset)].width)
                                    : 0;
    } else {
        // 空块（整行已被前面的块覆盖完）：没有宽度，但 x 仍然会被下一块接上
        m_blocks[index].width = 0;
    }
}

const LineMap::Loc &LineMap::locAt(int index) const
{
    // 块内定位：先整除到块，再按"相对块头的字节偏移"直接查表，全程没有前缀和
    const int block = blockIndexForColumn(m_byte[index]);

    assert(block >= 0 && block < m_built);
    assert(block < m_blocks.size());

    const quint32 offset = m_byte[index] - blockStartOf(block);

    assert(offset < static_cast<quint32>(m_loc.size()));

    return m_loc[static_cast<int>(offset)];
}

int LineMap::xAtEntry(int index) const
{
    const int total = m_byte.size();

    if (total == 0 || index <= 0) {
        return 0;
    }

    if (index >= total - 1) {
        // 行尾（含收尾单位）：行宽就是它的左边缘
        return width();
    }

    const int block = blockIndexForColumn(m_byte[index]);

    if (block >= m_blocks.size()) {
        return width();
    }

    ensureBuilt(block);

    // 块内 x 是"相对块头"的，所以这里是块头 x 加块内偏移
    return m_blocks[block].x + static_cast<int>(locAt(index).x);
}

int LineMap::width() const
{
    if (m_byte.size() <= 1 || m_blocks.isEmpty()) {
        return 0;
    }

    // 行宽要到最后一块才数得出来，所以这里会把整行量完。只在一行的"总宽"真的
    // 被需要时才发生（算横向滚动范围）；定点定位 xAtColumn / columnAtX 不会走到这里
    ensureBuilt(static_cast<int>(m_blocks.size()) - 1);

    int total = 0;

    for (int i = 0; i < m_built; i++) {
        total += m_blocks[i].width;
    }

    return total;
}

int LineMap::entryAtByte(quint32 byteColumn) const
{
    const int total = m_byte.size();

    if (total <= 1) {
        return 0;
    }

    const int block = blockIndexForColumn(byteColumn);

    if (block >= m_blocks.size()) {
        // 行尾之后：落在收尾单位上，也就是行尾列号
        return total - 1;
    }

    ensureBuilt(block);

    int index = std::min(m_blocks[block].startEntry, total - 1);

    if (index < 0) {
        index = 0;
    }

    // 块内走几步就够：块的起止字符是记好的，不用从行首找
    while (index < total - 1 && m_byte[index + 1] <= byteColumn) {
        index++;
    }

    return index;
}

quint32 LineMap::byteAtEntry(int index) const
{
    const int total = m_byte.size();

    if (total == 0 || index >= total - 1) {
        // 收尾单位，或者说行尾之后：列号就是整行字节数
        return m_byteLength;
    }

    if (index <= 0) {
        return 0;
    }

    return m_byte[index];
}

int LineMap::charIndexAtEntry(int index) const
{
    const int total = m_char.size();

    if (total == 0) {
        return 0;
    }

    if (index <= 0) {
        return 0;
    }

    if (index >= total) {
        return m_visible.size();
    }

    return static_cast<int>(m_char[index]);
}

int LineMap::xAtColumn(quint32 byteColumn) const
{
    // 一次整除跳到目标所在的块，块内查一次。全程没有前缀和数组
    const int entry = entryAtByte(byteColumn);

    if (entry >= m_byte.size() - 1) {
        // 行尾：直接要行宽，别去问那个收尾单位（它的 x 没有意义）
        return width();
    }

    return xAtEntry(entry);
}

int LineMap::entryAtX(int x) const
{
    const int total = m_byte.size();

    if (total <= 1 || m_blocks.isEmpty()) {
        return 0;
    }

    // ---- 粗定位：在块表上二分，找最后一个"左边缘不超过目标"的块 ----
    // 块表长度是 行字节数/4096，比字符数小了好几个数量级
    int low = 0;
    int high = static_cast<int>(m_blocks.size()) - 1;
    int block = 0;

    while (low <= high) {
        const int mid = low + (high - low) / 2;
        const int blockX = (mid < m_built) ? m_blocks[mid].x : -1;

        // 还没量到的块一定是空的，往左找
        if (blockX >= 0 && blockX <= x) {
            block = mid;
            low = mid + 1;
        } else {
            high = mid - 1;
        }
    }

    // ---- 细定位：只把这一块量出来，然后在块内走 ----
    ensureBuilt(block);

    const int from = std::max(0, m_blocks[block].startEntry);
    const int to = std::min(m_blocks[block].endEntry, total - 1);
    const int blockX = m_blocks[block].x;

    int index = from;

    // 停在"最后一个左边缘仍在 x 左边的字符"上。这里必须能停在最后那个字符上：
    // 早先写成 index + 1 < to 配合"右边缘超过 x 就 break"，结果点在一行最右边
    // 时永远停在倒数第二个字符上，整行光标右移一位
    while (index + 1 < to && blockX + static_cast<int>(locAt(index + 1).x) <= x) {
        index++;
    }

    return index;
}

quint32 LineMap::columnAtX(int x) const
{
    const int index = entryAtX(x);
    const int total = m_byte.size();

    if (total <= 1) {
        return 0;
    }

    if (index >= total - 1) {
        // 点在这一行最后一个字符的右边：光标停在行尾
        return m_byteLength;
    }

    const int block = blockIndexForColumn(m_byte[index]);
    const Loc &loc = locAt(index);
    const int left = m_blocks[block].x + static_cast<int>(loc.x);

    // 半个字符以内贴左边缘
    if (x - left < static_cast<int>(loc.width) / 2) {
        return m_byte[index];
    }

    // 超过半个字符：贴右边缘，也就是下一个字符的起点（最后一个字符就是行尾）
    if (index + 1 < total) {
        return m_byte[index + 1];
    }

    return m_byteLength;
}
