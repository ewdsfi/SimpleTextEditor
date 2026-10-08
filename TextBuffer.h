#ifndef TEXTBUFFER_H
#define TEXTBUFFER_H

#include <cstdint>
#include <string>
#include <vector>

/// 文本块：指向某个缓冲区中的一段连续字节区间。
/// 位置用「起始字节下标 + 字节长度」表示，比原版省掉了行号、列号、结束行列四组字段。
struct Piece
{
    uint32_t buffer = 0;  ///< 所属缓冲区编号
    uint32_t begin = 0;   ///< 在缓冲区中的起始字节下标
    uint32_t length = 0;  ///< 字节长度
    uint32_t lines = 0;   ///< 该区间内的换行符个数
};

/// 只读缓冲区：一块文本加上它的行首字节下标表。
struct Buffer
{
    std::string text;                 ///< 全部文本
    std::vector<uint32_t> lineStart;  ///< 每一行的起始字节下标
};

/// 文档中的一个位置，行号与列号均从 0 开始，列号按字节计。
struct Position
{
    uint32_t line = 0;
    uint32_t column = 0;
};

/// 红黑树结点：文本块按文档顺序组成一棵二叉搜索树。
/// 哨兵结点的 piece.length 恒为 0，用它判断空子树（空子树同时也是叶子）。
struct PieceNode
{
    Piece piece;                  ///< 本结点持有的文本块
    bool red = false;             ///< 是否为红结点
    PieceNode *parent = nullptr;  ///< 父结点，根结点的父结点是哨兵
    PieceNode *left = nullptr;    ///< 左孩子
    PieceNode *right = nullptr;   ///< 右孩子
    uint32_t size = 0;            ///< 左子树的总字节数
    uint32_t lines = 0;           ///< 左子树的总换行数
    uint32_t totalSize = 0;       ///< 整棵子树的总字节数
    uint32_t totalLines = 0;      ///< 整棵子树的总换行数
};

/// 精简版文本缓冲区：红黑树索引的块表（Piece Table）。
///
/// 与原版的关系与取舍：
///   - 保留原版的两条核心思路：
///       1）文本只追加、编辑只改块表，所以插入删除与文档总长无关；
///       2）块表用红黑树索引，靠左右旋维持平衡，子树统计量支持按下标二分下降；
///   - 去掉原版的行号列号双坐标体系，块只记字节下标与长度；
///   - 去掉原版的撤销栈、编码识别、换行风格记录，只保留能跑起来的最小功能；
///   - 红黑树用「单哨兵」的经典写法，五条不变量由 checkTree() 逐条校验。
///
/// 算法复杂度：任意操作 O(log 块数)，块数超过上限时整体整理一次。
class TextBuffer
{
public:
    /// 构造一个空缓冲区。
    TextBuffer();

    /// 释放缓冲区与全部结点。
    ~TextBuffer();

    /// 禁止拷贝，缓冲区独占底层存储。
    TextBuffer(const TextBuffer &) = delete;

    /// 禁止赋值，缓冲区独占底层存储。
    TextBuffer &operator=(const TextBuffer &) = delete;

    /// 清空全部内容。
    void clear();

    /// 读出全部文本。
    std::string text() const;

    /// 读出 [from, to) 区间内的文本。
    std::string range(uint32_t from, uint32_t to) const;

    /// 在 offset 处插入文本，返回实际插入的字节数。
    /// 传入的 CRLF 与单独的 CR 都会被归一成 LF，保证换行符不会被块边界劈开。
    uint32_t insert(uint32_t offset, const std::string &text);

    /// 删除 [from, to) 区间，返回实际删除的字节数。
    uint32_t erase(uint32_t from, uint32_t to);

    /// 用 text 替换 [from, to) 区间，返回被替换掉的字节数。
    uint32_t replace(uint32_t from, uint32_t to, const std::string &text);

    /// 行号、列号换算成字节下标。
    uint32_t offsetOf(uint32_t line, uint32_t column) const;

    /// 字节下标换算成行号与列号。
    Position positionOf(uint32_t offset) const;

    /// 第 line 行的起始字节下标。
    uint32_t lineBegin(uint32_t line) const;

    /// 第 line 行的结束字节下标（不含行尾换行符）。
    uint32_t lineEnd(uint32_t line) const;

    /// 第 line 行的内容（不含行尾换行符）。
    std::string lineText(uint32_t line) const;

    /// 查找 needle 第一次出现的位置，找不到返回 -1。
    long find(const std::string &needle, uint32_t from) const;

    /// 总行数，空文档也算 1 行。
    uint32_t lineCount() const;

    /// 总字节数。
    uint32_t byteCount() const { return _bytes; }

    /// 最长一行的字节数。
    uint32_t longestLine() const { return _longest; }

    /// 块数量，供观察整理效果。
    uint32_t pieceCount() const { return _count; }

    /// 树的高度，供观察平衡效果。
    uint32_t treeHeight() const;

    /// 自检：块区间、子树统计与红黑树五条不变量是否全部成立。
    bool check() const;

    /// 自检并把失败原因写入 where，便于定位。
    bool check(const char *&where) const;

    /// 按中序打印块表，用于排查统计量问题。
    void dump() const;

private:
    /// 建一个红结点（孩子先指向哨兵，颜色由插入修正决定）。
    PieceNode *newNode(const Piece &piece);

    /// 重算结点的子树统计量。
    void refresh(PieceNode *node);

    /// 重算结点自身及其全部祖先的子树统计量。
    void refreshUp(PieceNode *node);

    /// 以 node 为轴左旋。
    void rotateLeft(PieceNode *node);

    /// 以 node 为轴右旋。
    void rotateRight(PieceNode *node);

    /// 把 v 子树接到 u 的位置上。
    void transplant(PieceNode *u, PieceNode *v);

    /// 子树里的最左结点。
    PieceNode *leftmostNode(PieceNode *node) const;

    /// 块顺序上的下一个结点（中序后继），没有则返回哨兵。
    PieceNode *nextNode(PieceNode *node) const;

    /// 把 node 挂到 parent 的指定一侧，并沿父链重算统计、做插入修正。
    void attach(PieceNode *node, PieceNode *parent, bool asLeft);

    /// 按块顺序插到树尾（最后一块之后）。
    void insertNode(PieceNode *node);

    /// 按块顺序插到 where 之前。
    void insertBefore(PieceNode *where, PieceNode *node);

    /// 按块顺序插到 where 之后。
    void insertAfter(PieceNode *where, PieceNode *node);

    /// 插入修正：消除连续红结点。
    void insertFixup(PieceNode *node);

    /// 删除修正：消除双重黑结点。
    void deleteFixup(PieceNode *node);

    /// 删除结点（红黑树删除 + 修正），结点转入回收表。
    void removeNode(PieceNode *node);

    /// 按字节下标定位结点，inside 返回块内偏移。
    PieceNode *nodeAt(uint32_t offset, uint32_t *inside) const;

    /// 取第 line 行的行首字节下标（内部实现，行号必须有效）。
    uint32_t lineBeginAt(uint32_t line) const;

    /// 数出 node 前面的总字节数。
    uint32_t bytesBefore(PieceNode *node) const;

    /// 把 node 从块内偏移 inside 处切开，返回后半段（前半段留在原结点）。
    PieceNode *splitNode(PieceNode *node, uint32_t inside);

    /// 从 offset 起删除 length 个字节（内部自行处理块边界）。
    void removeBytes(uint32_t offset, uint32_t length);

    /// 合并相邻且首尾相接的块。
    void merge();

    /// 中序遍历中找到第一对可合并的相邻块并合并，成功返回 true。
    bool mergeOnePair();

    /// 整体整理：把全部内容写进一个新缓冲区，树收缩为单个结点。
    void compact();

    /// 重算总字节数、总行数、块数与最长行。
    void recount();

    /// 校验红黑树不变量，失败时把原因写入 where。
    bool checkTree(const char *&where) const;

    /// 递归校验子树：返回黑高，同时检查红结点孩子、统计量与块区间。
    uint32_t checkSubtree(PieceNode *node, const char *&where) const;

    /// 统计一段文本里的换行符个数。
    static uint32_t countLines(const char *data, size_t length);

    /// 把一段文本登记为新缓冲区并返回它的编号。
    uint32_t addBuffer(const std::string &text);

    /// 丢弃全部结点，把树复位成空。
    void freeTree();

    std::vector<Buffer *> _buffers;      ///< 全部只读缓冲区
    PieceNode *_nil = nullptr;           ///< 哨兵结点，全树共用一个
    PieceNode *_root = nullptr;          ///< 根结点
    std::vector<PieceNode *> _released;  ///< 删除后回收的结点，析构时统一释放
    uint32_t _bytes = 0;                 ///< 总字节数
    uint32_t _lines = 1;                 ///< 总行数
    uint32_t _longest = 0;               ///< 最长行的字节数
    uint32_t _count = 0;                 ///< 块数量
};

#endif // TEXTBUFFER_H
