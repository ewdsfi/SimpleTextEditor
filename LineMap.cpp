#include "LineMap.h"

#include <QFontMetrics>

#include <algorithm>
#include <cassert>

/// ASCII advances are measured once, so no character lookup hits the font again.
static const int AsciiCount = 128;

/// Decode the character at bytes[pos], returning how many bytes it occupies.
/// Hand rolled instead of QTextCodec/QStringConverter to avoid the extra
/// dependency, and to guarantee that a broken byte only ever consumes one byte.
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
        // stray continuation byte or 0xF8 and above: treat as one bad byte
        code = 0xFFFD;

        return 1;
    }

    // not enough bytes left, do not read past the end
    if (pos + extra >= size) {
        code = 0xFFFD;

        return 1;
    }

    for (int k = 1; k <= extra; k++) {
        const unsigned char next = static_cast<unsigned char>(bytes[pos + k]);

        // continuation bytes must look like 10xxxxxx
        if ((next & 0xC0) != 0x80) {
            code = 0xFFFD;

            return 1;
        }

        value = (value << 6) | (next & 0x3Fu);
    }

    code = value;

    return extra + 1;
}

/// How many bytes the character starting with this lead byte occupies
/// (a bad byte counts as one).
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

/// Append one code point to a UTF-16 string.
static void appendCodePoint(QString &out, uint32_t code)
{
    // beyond the Unicode range or inside the surrogate area: use the replacement
    // character, never emit invalid UTF-16
    if (code > 0x10FFFFu || (code >= 0xD800u && code <= 0xDFFFu)) {
        code = 0xFFFD;
    }

    if (code >= 0x10000u) {
        // outside the BMP: two surrogate halves, but still ONE layout unit
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
        // the real width of a tab is meaningless, tabWidth handles it below
        int width = 0;

        if (c != '\t') {
            width = static_cast<int>(fm.horizontalAdvance(QChar(static_cast<char16_t>(c))));
        }

        width = std::max(0, std::min(width, 65535));

        metrics.advance[c] = static_cast<quint16>(width);
    }

    // in a monospace font 'M' and '0' are equally wide, prefer the safer one
    metrics.codeWidth = metrics.advance['M'] > 0 ? metrics.advance['M'] : metrics.advance['0'];

    if (metrics.codeWidth <= 0) {
        // the font reported nothing, fall back to a value that will not wreck the UI
        metrics.codeWidth = 8;
    }

    // tabs use a fixed width on purpose (not "align to column 4"): a fixed width
    // is what makes a character's width depend on itself alone, which is what
    // keeps the cached block offsets valid
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
        // fast path: ASCII is one byte per character
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

/// Width of a non-ASCII character: memoised, measured from the font on first use.
/// Extracted because buildBlock() meets the same CJK character over and over.
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

    // keep our own copy so the caller may free its string
    map.m_text = text;

    // build the unit table (and the visible text with it); no width is measured
    // here, that is ensureBuilt()'s job
    map.resetEntries();

    return map;
}

void LineMap::refresh(const std::string &text)
{
    // the line was edited: the unit table and every measurement are stale.
    // The block table is rebuilt too, since the byte length may have changed.
    m_text = text;
    m_byteLength = static_cast<quint32>(text.size());

    resetEntries();
}

void LineMap::resetEntries()
{
    // worst case one character per byte, reserve for that and never grow again
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

    // cursor: bytes processed so far, also the first byte of the next unit
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
            // a tab is one unit, but occupies tabWidth pixels on screen, i.e. it
            // expands to tabWidth / codeWidth spaces
            const int spaces = (m_metrics->codeWidth > 0) ? (m_metrics->tabWidth / m_metrics->codeWidth) : 4;

            m_visible.append(QString(std::max(0, spaces), QChar(u' ')));
        } else {
            appendCodePoint(m_visible, code);
        }

        m_byte.append(pos);
        m_char.append(static_cast<quint32>(m_visible.size()));

        pos += static_cast<uint32_t>(used);
    }

    // append a sentinel unit with width 0 whose byte column is the line end.
    // With it every column is a half open interval [m_byte[i], m_byte[i+1]),
    // so the last character needs no special case, and its left edge is the
    // line width.
    m_byte.append(m_byteLength);
    m_char.append(static_cast<quint32>(m_visible.size()));

    // block skeleton: length depends on the byte count only. When byteLength
    // lands exactly on a block edge this adds a trailing block, which is where
    // the sentinel lives.
    const int blockCount = blockIndexForColumn(m_byteLength) + 1;

    for (int i = 0; i < blockCount; i++) {
        Block block;

        m_blocks.push_back(block);
    }

    m_built = 0;
}

int LineMap::blockIndexForColumn(quint32 byteColumn)
{
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
    // EntrySpan is a power of two, so byte k*4096 is also character index
    // k*4096: every character with a lower index starts before it (multi-byte
    // characters only push indices further left). Hence we can jump straight to
    // the block head and walk a few steps.
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

    // the line end always lives in the last block, so clamp instead of skipping
    if (index >= m_blocks.size()) {
        index = m_blocks.size() - 1;
    }

    while (m_built <= index) {
        // block head x = previous block's left edge + its width (0 for block 0).
        // The character range of a block is derived from the byte edges inside
        // buildBlock(), so no cross-block cursor state is needed.
        m_blocks[m_built].x = (m_built == 0) ? 0 : (m_blocks[m_built - 1].x + m_blocks[m_built - 1].width);

        buildBlock(m_built);

        m_built++;
    }
}

void LineMap::buildBlock(int index) const
{
    const quint32 start = blockStartOf(index);
    const quint32 end = blockEndColumn(index);

    // First character of this block. A block edge may split a multi-byte
    // character; such a character belongs to the earlier block, which is why
    // this asks for "at or after".
    const int from = firstEntryAtOrAfter(start);

    // one past the last character of this block
    const int stop = firstEntryAtOrAfter(end);

    m_blocks[index].startEntry = from;
    m_blocks[index].endEntry = stop;

    // m_loc is indexed by the byte offset from the block head, size it first
    quint32 span = 0;

    if (stop > from && from < m_byte.size()) {
        span = m_byte[stop - 1] - start + 1;
    }

    if (m_loc.size() < static_cast<int>(span)) {
        m_loc.resize(static_cast<int>(span));
    }

    // accumulate widths character by character; x is a block relative offset
    int x = 0;

    for (int i = from; i < stop; i++) {
        const quint32 at = m_byte[i];

        Loc loc;

        loc.byte = at - start;
        loc.x = static_cast<quint32>(std::max(0, x));

        int width = 0;

        if (at >= m_byteLength) {
            // the sentinel: width must stay 0, its left edge already is the line
            // width; giving it a width would make width() one character too large
            width = 0;
        } else {
            const unsigned char lead = static_cast<unsigned char>(m_text[at]);

            if (lead == '\t') {
                width = m_metrics->tabWidth;
            } else if (lead < 0x80) {
                width = m_metrics->advance[lead];
            } else {
                // non-ASCII: ask the font once per distinct character
                uint32_t code = 0;

                decodeUtf8(m_text.data(), static_cast<int>(m_text.size()), static_cast<int>(at), code);

                width = widthOfWideChar(*m_metrics, code);
            }
        }

        width = std::max(0, width);

        loc.width = static_cast<quint32>(width);

        // the index in m_loc is always the block relative byte offset
        const int slot = static_cast<int>(loc.byte);

        if (slot >= m_loc.size()) {
            m_loc.resize(slot + 1);
        }

        m_loc[slot] = loc;

        // next character's left edge = this one's left edge + its width
        x += width;
    }

    // block width = relative x of the last character + that character's width
    if (stop > from && from < m_byte.size()) {
        const quint32 offset = m_byte[stop - 1] - start;

        m_blocks[index].width =
            (offset < static_cast<quint32>(m_loc.size()))
                ? static_cast<int>(m_loc[static_cast<int>(offset)].x + m_loc[static_cast<int>(offset)].width)
                : 0;
    } else {
        // empty block (the line was already covered): no width, but its x is
        // still the anchor the next block builds on
        m_blocks[index].width = 0;
    }
}

const LineMap::Loc &LineMap::locAt(int index) const
{
    // in-block lookup: one division to the block, then index by the byte offset
    // from the block head. No prefix sums anywhere.
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
        // line end (and the sentinel): the line width is its left edge
        return width();
    }

    const int block = blockIndexForColumn(m_byte[index]);

    if (block >= m_blocks.size()) {
        return width();
    }

    ensureBuilt(block);

    // the in-block offset is block relative, so add the block head
    return m_blocks[block].x + static_cast<int>(locAt(index).x);
}

int LineMap::width() const
{
    if (m_byte.size() <= 1 || m_blocks.isEmpty()) {
        return 0;
    }

    // the line width needs the last block, so this measures the whole line. It
    // only happens when the total width is really needed (horizontal scroll
    // range); point queries through xAtColumn()/columnAtX() never get here.
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
        // past the line end: the sentinel unit, i.e. the line-end column
        return total - 1;
    }

    ensureBuilt(block);

    int index = std::min(m_blocks[block].startEntry, total - 1);

    if (index < 0) {
        index = 0;
    }

    // a few steps inside the block are enough, its first character is recorded
    while (index < total - 1 && m_byte[index + 1] <= byteColumn) {
        index++;
    }

    return index;
}

quint32 LineMap::byteAtEntry(int index) const
{
    const int total = m_byte.size();

    if (total == 0 || index >= total - 1) {
        // the sentinel, or past the line end: the column is the line length
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
    const int entry = entryAtByte(byteColumn);

    if (entry >= m_byte.size() - 1) {
        // line end: ask for the width, the sentinel's own x is meaningless
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

    // coarse step: binary search the last block whose left edge is not right of
    // x. The table is "line bytes / 4096" long, orders of magnitude below the
    // character count.
    int low = 0;
    int high = static_cast<int>(m_blocks.size()) - 1;
    int block = 0;

    while (low <= high) {
        const int mid = low + (high - low) / 2;
        const int blockX = (mid < m_built) ? m_blocks[mid].x : -1;

        // unmeasured blocks come later, so search left
        if (blockX >= 0 && blockX <= x) {
            block = mid;
            low = mid + 1;
        } else {
            high = mid - 1;
        }
    }

    // fine step: measure this block only, then walk inside it
    ensureBuilt(block);

    const int from = std::max(0, m_blocks[block].startEntry);
    const int to = std::min(m_blocks[block].endEntry, total - 1);
    const int blockX = m_blocks[block].x;

    int index = from;

    // stop on the last character whose left edge is still left of x. The bound
    // must allow reaching the last character of the line; an earlier version
    // stopped at the second to last one, which shifted the caret by one.
    while (index + 1 < to && blockX + static_cast<int>(locAt(index + 1).x) <= x) {
        index++;
    }

    return index;
}

void LineMap::windowForX(int leftX, int rightX, int &firstEntry, int &lastEntry) const
{
    const int total = m_byte.size();
    const int last = total - 1;  // index of the sentinel unit

    if (total <= 1) {
        firstEntry = 0;
        lastEntry = total;

        return;
    }

    // start: first character whose left edge reaches leftX. This must go through
    // entryAtX(), which measures the block on demand and returns an exact
    // character index.
    int first = entryAtX(std::max(0, leftX));

    if (first < 0) {
        first = 0;
    }

    if (first > last) {
        first = last;
    }

    // end: the character whose right edge passes rightX, plus one
    int stop = entryAtX(std::max(0, rightX)) + 2;

    if (stop > last) {
        stop = last;
    }

    if (stop <= first) {
        stop = std::min(last, first + 1);
    }

    firstEntry = first;
    lastEntry = stop;
}

quint32 LineMap::columnAtX(int x) const
{
    const int index = entryAtX(x);
    const int total = m_byte.size();

    if (total <= 1) {
        return 0;
    }

    if (index >= total - 1) {
        // right of the last character: the caret goes to the line end
        return m_byteLength;
    }

    const int block = blockIndexForColumn(m_byte[index]);
    const Loc &loc = locAt(index);
    const int left = m_blocks[block].x + static_cast<int>(loc.x);

    // within half a character: snap to the left edge
    if (x - left < static_cast<int>(loc.width) / 2) {
        return m_byte[index];
    }

    // past half a character: snap right, i.e. to the next character's start
    // (for the last character that is the line end)
    if (index + 1 < total) {
        return m_byte[index + 1];
    }

    return m_byteLength;
}
