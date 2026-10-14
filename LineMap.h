#ifndef LINEMAP_H
#define LINEMAP_H

#include <QHash>
#include <QString>
#include <QVector>

#include <string>

class QFontMetrics;

/// LineMap —— 给"一行文本"做坐标定位的缓存。
///
/// 为什么需要它：超长单行（几十万乃至上百万字符）如果每次鼠标点击都把整行从头
/// 量一遍，或者每次都从头累加字符宽度，定位就会卡死。这里**不**使用前缀和数组
/// 那种一眼可见的教科书做法，而是把定位拆成两级：
///
///   1) 粗定位：把一行按**字节**切成固定大小的块（EntrySpan 字节一块）。块表里
///      每项只记"这一块从第几个字符到第几个字符""这一块的左边缘在屏幕上的 x"
///      "这一块自己多宽"。块表长度是"行字节数 / 4096"，跟字符数不在一个量级；
///   2) 细定位：块的宽度**用到才量**，块内按字符累加，x 只在这个块里记一份。
///      人一屏最多看几十个字符，所以真正被量过的块往往只有可见的那一两块。
///
/// 于是两个方向都不用扫全行：
///   字节列号 -> 屏幕 x   ：一次整除跳到目标块，块内查一次（摊还 O(1)）
///   屏幕 x   -> 字节列号 ：对块表的 x 二分（O(log 块数)），再块内走几步
///
/// 前提：等宽字体（本工程固定用 Consolas）。正因为等宽，"每块开头的 x"才足以
/// 当粗定位的锚点；块内宽度仍然逐字符累加，所以全角汉字、制表符也不会算错。
class LineMap
{
public:
    /// 一行的构造上下文：每个字符的像素推进量。
    /// 整个视图只算一份，建多少行都复用
    struct Metrics
    {
        QVector<quint16> advance;           /// 0..127 号字符 -> 推进量（ASCII 一次测好）

        /// 汉字之类的宽度，第一次遇到量一次就记住。
        /// mutable 是因为它纯粹是个记忆化缓存：值只由字符本身决定，不改它也不会
        /// 算错，只是慢一点，所以放进 const 方法里改是安全的
        mutable QHash<quint32, quint16> wideAdvance;

        const QFontMetrics *font = nullptr; /// 非 ASCII 字符现测时要用
        int codeWidth = 8;                  /// 一个西文半角字符的宽度
        int tabWidth  = 32;                 /// 一个制表符占的宽度（4 个半角）
    };

    /// 块的字节跨度。必须是 2 的幂：这样"第 k 块从第 k*EntrySpan 个字节开始"
    /// 永远成立，量到哪块都不影响别的块，块序号也能用位运算拿到。
    /// 块的起止都对齐到 UTF-8 字符边界，所以一个字符永远不会跨块
    static const quint32 EntrySpan = 4096;

    /// 用一次实测得到的字体宽度，把 ASCII 的推进量和制表符宽度算好
    static Metrics makeMetrics(const QFontMetrics &fm);

    /// 把一行文本全部编码成 UTF-16。只对"要用到的那一行"调用，不是整个文件
    static QString decode(const std::string &text);

    /// 造一行的定位表：行首字节下标 + 整行字节内容（不含换行符）+ 共用的度量。
    /// 内部会留一份文本副本，之后调用方怎么改自己的字符串都不影响这里
    static LineMap build(quint32 lineBeginByte, const std::string &text, const Metrics &metrics);

    /// 内容变了（同一行被编辑过）：把量测结果全部丢掉重建
    void refresh(const std::string &text);

    /// 这一行在屏幕上一共多宽
    int width() const;

    /// 这一行有多少个"定位单位"。一个西文字符算一个，一个全角字符也算一个。
    /// 末尾额外有一个宽度为 0 的收尾单位代表行尾，所以它的下标就是行尾列号
    int entryCount() const { return m_byte.size(); }

    /// 字节列号（相对行首）-> 屏幕 x
    int xAtColumn(quint32 byteColumn) const;

    /// 屏幕 x -> 字节列号（相对行首）。结果一定落在字符边界上
    quint32 columnAtX(int x) const;

    /// 屏幕 x 落在第几个定位单位上。画一行时用它切"屏幕上可见的那一段"，
    /// 免得为了最后多取一两个字符就把整块多量出来
    int entryAtX(int x) const;

    /// 第 index 个定位单位开头的字节列号
    quint32 byteAtEntry(int index) const;

    /// 字节列号落在第几个定位单位上，范围 [0, entryCount())
    int entryAtByte(quint32 byteColumn) const;

    /// 第 index 个定位单位在整行字符串里的下标
    int charIndexAtEntry(int index) const;

    /// 用户能看见的整行字符串：字符 + 制表符展开成的空格
    QString visible() const { return m_visible; }

    /// 整行字符串的长度
    int visibleLength() const { return m_visible.size(); }

    /// 整行有多少字节（不含行尾换行符）
    quint32 byteLength() const { return m_byteLength; }

private:
    /// 块表里的一项。x 与 width 只在"这一块已经量过"（index < m_built）时才有意义
    struct Block
    {
        int x        = -1;  /// 这一块左边缘在屏幕上的 x
        int width    = 0;   /// 这一块自己多宽
        int startEntry = -1;/// 这一块从第几个字符开始（算上它前面的块）
        int endEntry   = -1;/// 这一块到第几个字符为止（不含）
    };

    /// 量测结果，按块分段存。字节和 x 都只记"相对块头"的偏移，
    /// 所以它们同时也就是这个字符在块内的下标，块内定位是一次直接查表。
    ///
    /// 两个字段都是 4 字节：块内偏移虽然不会超过块的字节跨度，但 x 是像素，
    /// 一块 4096 个字符最宽能到几万像素，用 2 字节会溢出
    struct Loc
    {
        quint32 byte  = 0;  /// 相对块头的字节偏移
        quint32 x     = 0;  /// 相对块头的像素偏移
        quint32 width = 0;  /// 这个单位的宽度（收尾单位固定是 0）
    };

    /// 块的起始字节 -> 块序号（按 EntrySpan 对齐，整除就是）
    static int blockIndexForColumn(quint32 byteColumn);

    /// 第 index 块结束后的字节列号
    static quint32 blockEndColumn(int index);

    /// 第 index 块的起始字节列号
    quint32 blockStartOf(int index) const;

    /// 第一个字节能达到 byteColumn 的字符序号（在 m_byte 里的下标）。
    /// 因为块按 2 的幂对齐，直接用块序号往回推，不用从头扫
    int firstEntryAtOrAfter(quint32 byteColumn) const;

    /// 保证前 index+1 块都已经量过。前缀性质：量过第 k 块，前面必然都量过
    void ensureBuilt(int index) const;

    /// 第 index 个字符的量测结果。只在"所在块已经量过"时可用（locAt 里有断言）。
    /// 实现上是"整除到块 + 按块内字节偏移查表"两步，没有前缀和
    const Loc &locAt(int index) const;

    /// 量第 index 块：从块自己的起点走到终点，把每个字符的块内字节偏移和块内 x
    /// 记进 m_loc，并把这一块的 startEntry/endEntry 定下来
    void buildBlock(int index) const;

    /// 第 index 个字符（含行尾那个收尾单位）的左边缘 x
    int xAtEntry(int index) const;

    /// 按当前 m_text 重铺字符表和块表骨架（不量任何宽度）
    void resetEntries();

    QVector<quint32> m_byte;   /// 每个定位单位开头的行内字节列号
    QVector<quint32> m_char;   /// 每个定位单位在 m_visible 里的下标

    mutable QVector<Loc> m_loc;      /// 已量过的块里，逐字符的块内偏移
    mutable QVector<Block> m_blocks; /// 块表，每 EntrySpan 字节一项
    mutable int m_built = 0;         /// 已量过的块数（m_blocks 里有效的前缀长度）

    quint32 m_lineBegin  = 0;        /// 这一行在文档里的起始字节
    quint32 m_byteLength = 0;        /// 这一行有多少字节
    const Metrics *m_metrics = nullptr;
    QString m_visible;               /// UTF-16 文本，制表符已经展开成空格
    std::string m_text;              /// 这一行的原始字节，量块时要按它切分
};

#endif // LINEMAP_H
