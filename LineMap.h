#ifndef LINEMAP_H
#define LINEMAP_H

#include <QHash>
#include <QString>
#include <QVector>

#include <string>

class QFontMetrics;

/// Per-line coordinate index for the text view.
///
/// Why it exists: a line can hold hundreds of thousands of characters. Measuring
/// the whole line on every mouse click, or accumulating character advances from
/// the line start, would freeze the UI. This class therefore uses a two level
/// scheme instead of the obvious prefix-sum array:
///
///   1) coarse: the line is cut into fixed *byte* blocks (EntrySpan bytes each).
///      The block table only stores the block's left edge in pixels, its own
///      width, and the range of characters it covers. Its length is
///      "line bytes / 4096", unrelated to the character count.
///   2) fine: a block's width is measured lazily, only when it is needed, and
///      the per-character x offsets live inside that block only. A screen shows
///      a few dozen characters, so in practice one or two blocks ever get
///      measured.
///
/// Both directions stay cheap: byte column -> pixel is one division plus a
/// lookup inside the block, pixel -> byte column is a binary search over block
/// x plus a few steps inside the block.
///
/// Assumes a monospace font (Consolas here). Monospace is what makes "the x of
/// each block start" usable as a coarse anchor. Widths inside a block are still
/// accumulated per character, so CJK glyphs and tabs stay correct.
class LineMap
{
public:
    /// Shared per-line measuring context: the advance of each character.
    /// Built once for the whole view and reused by every line.
    struct Metrics
    {
        QVector<quint16> advance;  ///< advance of characters 0..127 (measured once)

        /// Widths of non-ASCII characters, measured on first use and then kept.
        /// mutable because it is a pure memo cache: the value only depends on the
        /// character itself, so filling it from a const method cannot be wrong.
        mutable QHash<quint32, quint16> wideAdvance;

        const QFontMetrics *font = nullptr;  ///< used to measure non-ASCII characters
        int codeWidth = 8;                   ///< width of one ASCII character
        int tabWidth = 32;                   ///< width of '\t' (4 ASCII cells)
    };

    /// Block span in bytes. A power of two, so "block k starts at byte
    /// k*EntrySpan" always holds and a block index is a shift. Block edges are
    /// snapped to UTF-8 character boundaries, so a character never crosses a
    /// block.
    static const quint32 EntrySpan = 4096;

    /// Measure the ASCII advances and the tab width once.
    static Metrics makeMetrics(const QFontMetrics &fm);

    /// Decode byte text into UTF-16. Called per line, never for the whole file.
    static QString decode(const std::string &text);

    /// Build the index for one line: its first byte in the document, its bytes
    /// (without the trailing newline) and the shared metrics. The text is copied
    /// in, so the caller may free its own string afterwards.
    static LineMap build(quint32 lineBeginByte, const std::string &text, const Metrics &metrics);

    /// Line content changed: drop all measurements and rebuild.
    void refresh(const std::string &text);

    /// Total pixel width of the line.
    int width() const;

    /// Number of layout units: one ASCII character or one CJK character counts
    /// as one. A zero width sentinel unit for the line end is appended, so the
    /// last unit index equals the line-end column.
    int entryCount() const { return m_byte.size(); }

    /// Byte column (relative to the line start) -> pixel x.
    int xAtColumn(quint32 byteColumn) const;

    /// Pixel x -> byte column (relative to the line start). Always lands on a
    /// character boundary.
    quint32 columnAtX(int x) const;

    /// Unit index whose left edge is the last one not right of x.
    int entryAtX(int x) const;

    /// Pixel window [leftX, rightX] -> the units that must be painted:
    /// firstEntry is the first visible character, lastEntry the one after it.
    ///
    /// Painting must use this and not entryAtByte() as the start: while a block
    /// is unmeasured entryAtByte() can only report the block head (up to 4096
    /// bytes earlier), which shifts the whole line on screen.
    void windowForX(int leftX, int rightX, int &firstEntry, int &lastEntry) const;

    /// Byte column where unit index starts.
    quint32 byteAtEntry(int index) const;

    /// Unit index containing byteColumn, in [0, entryCount()).
    int entryAtByte(quint32 byteColumn) const;

    /// Index of unit index inside the visible string.
    int charIndexAtEntry(int index) const;

    /// The whole line as the user sees it: characters with tabs expanded.
    QString visible() const { return m_visible; }

    /// Length of visible().
    int visibleLength() const { return m_visible.size(); }

    /// Line length in bytes, excluding the trailing newline.
    quint32 byteLength() const { return m_byteLength; }

private:
    /// One block. x and width are only meaningful once the block is measured,
    /// i.e. for index < m_built.
    struct Block
    {
        int x = -1;           ///< left edge in pixels
        int width = 0;        ///< width of the block itself
        int startEntry = -1;  ///< first unit of the block
        int endEntry = -1;    ///< one past the last unit of the block
    };

    /// Measurement of one unit, stored per block. Both offsets are relative to
    /// the block head, which also makes them the index inside the block.
    ///
    /// 4 bytes each: an in-block byte offset fits easily, but x is a pixel
    /// offset and 4096 characters can be tens of thousands of pixels wide, so
    /// 2 bytes would overflow.
    struct Loc
    {
        quint32 byte = 0;   ///< byte offset from the block head
        quint32 x = 0;      ///< pixel offset from the block head
        quint32 width = 0;  ///< width of this unit (0 for the end sentinel)
    };

    /// Byte column -> block index.
    static int blockIndexForColumn(quint32 byteColumn);

    /// Byte column one past the end of block index.
    static quint32 blockEndColumn(int index);

    /// First byte of block index.
    quint32 blockStartOf(int index) const;

    /// Index of the first unit whose byte is at or after byteColumn.
    /// Because blocks are power-of-two aligned this can jump straight to the
    /// block instead of scanning from the line start.
    int firstEntryAtOrAfter(quint32 byteColumn) const;

    /// Measure blocks 0..index. Blocks are filled left to right, so everything
    /// before m_built is always measured.
    void ensureBuilt(int index) const;

    /// Measurement of one unit. Only valid when its block is measured; there are
    /// asserts for that.
    const Loc &locAt(int index) const;

    /// Measure block index: walk it character by character, recording in-block
    /// byte offsets and x offsets, and fix up startEntry/endEntry.
    void buildBlock(int index) const;

    /// Left edge x of unit index (including the end sentinel).
    int xAtEntry(int index) const;

    /// Rebuild the unit table and the block skeletons from m_text. Measures no
    /// widths.
    void resetEntries();

    QVector<quint32> m_byte;  ///< first byte column of each unit
    QVector<quint32> m_char;  ///< index of each unit inside m_visible

    mutable QVector<Loc> m_loc;       ///< per-unit measurements of measured blocks
    mutable QVector<Block> m_blocks;  ///< one entry per EntrySpan bytes
    mutable int m_built = 0;          ///< number of measured blocks

    quint32 m_lineBegin = 0;   ///< first byte of this line in the document
    quint32 m_byteLength = 0;  ///< line length in bytes
    const Metrics *m_metrics = nullptr;
    QString m_visible;         ///< UTF-16 text, tabs already expanded
    std::string m_text;        ///< raw bytes, used to split the line into blocks
};

#endif // LINEMAP_H
