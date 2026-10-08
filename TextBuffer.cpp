#include "TextBuffer.h"

#include <algorithm>

/// 块数量上限：超过就整体整理一次。
static const uint32_t MaxPieces = 512;

/// 单次插入的切块大小，超过就分成多块，避免单块过大。
static const size_t ChunkSize = 64 * 1024;

/// 把字节下标限制在 [0, limit] 之内。
static uint32_t clampTo(uint32_t value, uint32_t limit)
{
    return (value > limit) ? limit : value;
}

/// 判断一个字节是否为 UTF-8 续字节。
static bool isContinuation(char c)
{
    return (static_cast<unsigned char>(c) & 0xC0) == 0x80;
}

/// 判断结点是否为真实结点（哨兵代表空子树，同时充当叶子）。
static bool isReal(const PieceNode *node)
{
    return node != nullptr && node->piece.length > 0;
}

TextBuffer::TextBuffer()
{
    _nil = new PieceNode();
    _nil->red = false;
    _nil->parent = _nil;
    _nil->left = _nil;
    _nil->right = _nil;
    _root = _nil;

    _buffers.push_back(new Buffer());
    _buffers[0]->lineStart.push_back(0);
}

TextBuffer::~TextBuffer()
{
    freeTree();

    for (size_t i = 0; i < _buffers.size(); i++) {
        delete _buffers[i];
    }

    delete _nil;
}

void TextBuffer::freeTree()
{
    for (size_t i = 0; i < _released.size(); i++) {
        delete _released[i];
    }

    _released.clear();
    _root = _nil;
}

void TextBuffer::clear()
{
    freeTree();

    for (size_t i = 1; i < _buffers.size(); i++) {
        delete _buffers[i];
    }

    _buffers.resize(1);
    _buffers[0]->text.clear();
    _buffers[0]->lineStart.clear();
    _buffers[0]->lineStart.push_back(0);
    _bytes = 0;
    _lines = 1;
    _longest = 0;
    _count = 0;
}

// ---------------------------------------------------------------------------
// 树的构造与维护
// ---------------------------------------------------------------------------

PieceNode *TextBuffer::newNode(const Piece &piece)
{
    PieceNode *node = new PieceNode();

    node->piece = piece;
    node->red = true;
    node->parent = _nil;
    node->left = _nil;
    node->right = _nil;
    node->size = 0;
    node->lines = 0;
    node->totalSize = piece.length;
    node->totalLines = piece.lines;

    return node;
}

void TextBuffer::refresh(PieceNode *node)
{
    if (!isReal(node)) {
        return;
    }

    node->size = isReal(node->left) ? node->left->totalSize : 0;
    node->lines = isReal(node->left) ? node->left->totalLines : 0;
    node->totalSize = node->size + node->piece.length + (isReal(node->right) ? node->right->totalSize : 0);
    node->totalLines = node->lines + node->piece.lines + (isReal(node->right) ? node->right->totalLines : 0);
}

void TextBuffer::refreshUp(PieceNode *node)
{
    // 结点自身及其全部祖先都要重算，漏掉一层就会让上层的子树统计失真
    refresh(node);

    for (PieceNode *walk = node; walk->parent != _nil; walk = walk->parent) {
        refresh(walk->parent);
    }
}

void TextBuffer::rotateLeft(PieceNode *node)
{
    PieceNode *pivot = node->right;

    if (!isReal(pivot)) {
        return;
    }

    // pivot 顶替 node 的位置，node 连同其左子树成为 pivot 的左子树
    node->right = pivot->left;

    if (isReal(pivot->left)) {
        pivot->left->parent = node;
    }

    pivot->parent = node->parent;

    if (node->parent == _nil) {
        _root = pivot;
    } else if (node->parent->left == node) {
        node->parent->left = pivot;
    } else {
        node->parent->right = pivot;
    }

    pivot->left = node;
    node->parent = pivot;

    refresh(node);
    refresh(pivot);
}

void TextBuffer::rotateRight(PieceNode *node)
{
    PieceNode *pivot = node->left;

    if (!isReal(pivot)) {
        return;
    }

    // pivot 顶替 node 的位置，node 连同其右子树成为 pivot 的右子树
    node->left = pivot->right;

    if (isReal(pivot->right)) {
        pivot->right->parent = node;
    }

    pivot->parent = node->parent;

    if (node->parent == _nil) {
        _root = pivot;
    } else if (node->parent->left == node) {
        node->parent->left = pivot;
    } else {
        node->parent->right = pivot;
    }

    pivot->right = node;
    node->parent = pivot;

    refresh(node);
    refresh(pivot);
}

void TextBuffer::transplant(PieceNode *u, PieceNode *v)
{
    if (u->parent == _nil) {
        _root = v;
    } else if (u->parent->left == u) {
        u->parent->left = v;
    } else {
        u->parent->right = v;
    }

    v->parent = u->parent;
}

PieceNode *TextBuffer::leftmostNode(PieceNode *node) const
{
    while (isReal(node->left)) {
        node = node->left;
    }

    return node;
}

void TextBuffer::attach(PieceNode *node, PieceNode *parent, bool asLeft)
{
    // 把 node 挂到 parent 的指定一侧，并沿父链重算子树统计
    node->left = _nil;
    node->right = _nil;
    node->parent = parent;

    if (parent == _nil) {
        _root = node;
    } else if (asLeft) {
        parent->left = node;
    } else {
        parent->right = node;
    }

    refreshUp(node);

    _count++;
    insertFixup(node);
}

void TextBuffer::insertNode(PieceNode *node)
{
    // 新块一律排在最后，也就是树的最右端
    if (!isReal(_root)) {
        attach(node, _nil, false);

        return;
    }

    PieceNode *slot = _root;

    while (isReal(slot->right)) {
        slot = slot->right;
    }

    attach(node, slot, false);
}

void TextBuffer::insertBefore(PieceNode *where, PieceNode *node)
{
    // 排在 where 之前，就是放到 where 左子树的最右端
    if (!isReal(where)) {
        insertNode(node);

        return;
    }

    if (!isReal(where->left)) {
        attach(node, where, true);

        return;
    }

    PieceNode *slot = where->left;

    while (isReal(slot->right)) {
        slot = slot->right;
    }

    attach(node, slot, false);
}

void TextBuffer::insertAfter(PieceNode *where, PieceNode *node)
{
    // 排在 where 之后，就是放到 where 右子树的最左端
    if (!isReal(where)) {
        insertNode(node);

        return;
    }

    if (!isReal(where->right)) {
        attach(node, where, false);

        return;
    }

    attach(node, leftmostNode(where->right), true);
}

void TextBuffer::insertFixup(PieceNode *node)
{
    // 不变量 4：红结点的两个孩子必须是黑的。新结点是红的，若父亲也是红的就冲突
    while (node->parent->red) {
        PieceNode *parent = node->parent;
        PieceNode *grand = parent->parent;

        if (parent == grand->left) {
            PieceNode *uncle = grand->right;

            if (uncle->red) {
                // 情况 1：叔叔是红的 —— 把红下推到祖父，冲突可能继续向上
                parent->red = false;
                uncle->red = false;
                grand->red = true;
                node = grand;
            } else {
                if (node == parent->right) {
                    // 情况 2：自己是右孩子 —— 先左旋，转成情况 3
                    node = parent;
                    rotateLeft(node);
                }

                // 情况 3：自己是左孩子 —— 右旋祖父并重新着色
                node->parent->red = false;
                node->parent->parent->red = true;
                rotateRight(node->parent->parent);
            }
        } else {
            PieceNode *uncle = grand->left;

            if (uncle->red) {
                parent->red = false;
                uncle->red = false;
                grand->red = true;
                node = grand;
            } else {
                if (node == parent->left) {
                    node = parent;
                    rotateRight(node);
                }

                node->parent->red = false;
                node->parent->parent->red = true;
                rotateLeft(node->parent->parent);
            }
        }
    }

    // 不变量 2：根结点必须是黑的
    _root->red = false;
}

void TextBuffer::deleteFixup(PieceNode *node)
{
    // node 带着一重额外的黑色，向上搬运直到能消掉
    while (node != _root && !node->red) {
        PieceNode *parent = node->parent;

        if (node == parent->left) {
            PieceNode *brother = parent->right;

            if (brother->red) {
                // 情况 1：兄弟是红的 —— 兄弟涂黑、父亲涂红，左旋后转为其余情况
                brother->red = false;
                parent->red = true;
                rotateLeft(parent);
                brother = parent->right;
            }

            if (!brother->left->red && !brother->right->red) {
                // 情况 2：兄弟的两个孩子都是黑的 —— 兄弟涂红，额外黑色上移
                brother->red = true;
                node = parent;
            } else {
                if (!brother->right->red) {
                    // 情况 3：兄弟的右孩子是黑的 —— 右旋兄弟，转为情况 4
                    brother->left->red = false;
                    brother->red = true;
                    rotateRight(brother);
                    brother = parent->right;
                }

                // 情况 4：兄弟的右孩子是红的 —— 交换颜色并左旋，额外黑色消掉
                brother->red = parent->red;
                parent->red = false;
                brother->right->red = false;
                rotateLeft(parent);
                node = _root;
            }
        } else {
            PieceNode *brother = parent->left;

            if (brother->red) {
                brother->red = false;
                parent->red = true;
                rotateRight(parent);
                brother = parent->left;
            }

            if (!brother->right->red && !brother->left->red) {
                brother->red = true;
                node = parent;
            } else {
                if (!brother->left->red) {
                    brother->right->red = false;
                    brother->red = true;
                    rotateLeft(brother);
                    brother = parent->left;
                }

                brother->red = parent->red;
                parent->red = false;
                brother->left->red = false;
                rotateRight(parent);
                node = _root;
            }
        }
    }

    node->red = false;
}

void TextBuffer::removeNode(PieceNode *node)
{
    if (!isReal(node)) {
        return;
    }

    PieceNode *moved = node;
    PieceNode *child = _nil;
    bool wasRed = moved->red;

    if (!isReal(node->left)) {
        // 没有左孩子：直接用右孩子顶替
        child = node->right;
        transplant(node, node->right);
    } else if (!isReal(node->right)) {
        // 没有右孩子：直接用左孩子顶替
        child = node->left;
        transplant(node, node->left);
    } else {
        // 两个孩子都在：用后继（右子树最左结点）顶替
        moved = leftmostNode(node->right);
        wasRed = moved->red;
        child = moved->right;

        if (moved->parent == node) {
            child->parent = moved;
        } else {
            transplant(moved, moved->right);

            // moved 换位置了，它的新父链统计要在挂到 node 位置之前先修好
            refreshUp(moved->parent == _nil ? moved : moved->parent);

            moved->right = node->right;
            moved->right->parent = moved;
        }

        transplant(node, moved);
        moved->left = node->left;
        moved->left->parent = moved;
        moved->red = node->red;
    }

    // moved 顶到了 node 的位置，它的整条祖先链都要在旋转之前重算：
    // 旋转与修正都依赖子树统计量
    refreshUp(moved);

    if (!wasRed) {
        deleteFixup(child);
    }

    _released.push_back(node);
    _count--;
}

// ---------------------------------------------------------------------------
// 定位
// ---------------------------------------------------------------------------

PieceNode *TextBuffer::nodeAt(uint32_t offset, uint32_t *inside) const
{
    // 约定：offset 严格小于总长时返回所在结点，inside 是块内偏移；
    // offset 等于总长时返回哨兵，表示落在文档末尾
    if (offset >= _bytes) {
        if (inside != nullptr) {
            *inside = 0;
        }

        return _nil;
    }

    PieceNode *node = _root;

    while (isReal(node)) {
        const uint32_t blockEnd = node->size + node->piece.length;

        // 左子树与本块都不含目标就跳到右子树。
        // 落点正好等于块尾时只有右子树非空才往右走：否则目标就是本块最后一个字节
        if (offset > blockEnd || (offset == blockEnd && isReal(node->right))) {
            offset -= blockEnd;
            node = node->right;

            continue;
        }

        // 目标落在左子树里
        if (offset < node->size) {
            node = node->left;

            continue;
        }

        if (inside != nullptr) {
            *inside = offset - node->size;
        }

        return node;
    }

    if (inside != nullptr) {
        *inside = 0;
    }

    return _nil;
}

uint32_t TextBuffer::lineBeginAt(uint32_t line) const
{
    if (line == 0) {
        return 0;
    }

    if (line >= _lines) {
        return _bytes;
    }

    // 一边下降一边累计目标前面的字节数，找到行首所在块
    uint32_t cursor = 0;
    PieceNode *walk = _root;

    while (isReal(walk)) {
        // 目标行落在左子树里
        if (walk->lines >= line) {
            walk = walk->left;

            continue;
        }

        line -= walk->lines;
        cursor += walk->size;

        // 目标行就是本块的第一行
        if (line == 0) {
            return cursor;
        }

        // 落在本块内部：在块里找第 line 个换行符，它后面就是行首
        if (line <= walk->piece.lines) {
            const std::string &source = _buffers[walk->piece.buffer]->text;
            const char *data = source.data() + walk->piece.begin;
            uint32_t found = 0;

            for (uint32_t p = 0; p < walk->piece.length; p++) {
                if (data[p] != '\n') {
                    continue;
                }

                found++;

                if (found == line) {
                    return cursor + p + 1;
                }
            }
        }

        line -= walk->piece.lines;
        cursor += walk->piece.length;
        walk = walk->right;
    }

    return _bytes;
}

PieceNode *TextBuffer::nextNode(PieceNode *node) const
{
    // 中序后继：有右子树就取右子树最左结点，否则向上找到第一个「从左拐上来」的祖先
    if (!isReal(node)) {
        return _nil;
    }

    if (isReal(node->right)) {
        return leftmostNode(node->right);
    }

    PieceNode *up = node->parent;

    while (isReal(up) && up->right == node) {
        node = up;
        up = up->parent;
    }

    return isReal(up) ? up : _nil;
}

uint32_t TextBuffer::bytesBefore(PieceNode *node) const
{
    uint32_t bytes = 0;
    PieceNode *walk = _root;

    while (isReal(walk) && walk != node) {
        bytes += walk->size + walk->piece.length;
        walk = walk->right;
    }

    return bytes;
}

uint32_t TextBuffer::treeHeight() const
{
    // 用显式栈做前序统计，避免递归
    std::vector<PieceNode *> stack;
    std::vector<uint32_t> depth;
    uint32_t best = 0;

    if (isReal(_root)) {
        stack.push_back(_root);
        depth.push_back(1);
    }

    while (!stack.empty()) {
        PieceNode *node = stack.back();
        const uint32_t level = depth.back();

        stack.pop_back();
        depth.pop_back();

        if (level > best) {
            best = level;
        }

        if (isReal(node->left)) {
            stack.push_back(node->left);
            depth.push_back(level + 1);
        }

        if (isReal(node->right)) {
            stack.push_back(node->right);
            depth.push_back(level + 1);
        }
    }

    return best;
}

// ---------------------------------------------------------------------------
// 内部工具
// ---------------------------------------------------------------------------

uint32_t TextBuffer::countLines(const char *data, size_t length)
{
    uint32_t count = 0;

    for (size_t i = 0; i < length; i++) {
        if (data[i] == '\n') {
            count++;
        }
    }

    return count;
}

uint32_t TextBuffer::addBuffer(const std::string &text)
{
    Buffer *buffer = new Buffer();

    buffer->text = text;
    buffer->lineStart.push_back(0);

    for (size_t i = 0; i < text.size(); i++) {
        if (text[i] == '\n') {
            buffer->lineStart.push_back(static_cast<uint32_t>(i + 1));
        }
    }

    _buffers.push_back(buffer);

    return static_cast<uint32_t>(_buffers.size() - 1);
}

PieceNode *TextBuffer::splitNode(PieceNode *node, uint32_t inside)
{
    if (!isReal(node) || inside == 0 || inside >= node->piece.length) {
        return _nil;
    }

    const Piece piece = node->piece;
    const char *data = _buffers[piece.buffer]->text.data() + piece.begin;
    const uint32_t headLines = countLines(data, inside);

    // 后半段另立一块，先把 node 截短，再把后半段插到它正后方
    Piece back;
    back.buffer = piece.buffer;
    back.begin = piece.begin + inside;
    back.length = piece.length - inside;
    back.lines = piece.lines - headLines;

    Piece front = piece;
    front.length = inside;
    front.lines = headLines;

    node->piece = front;
    refreshUp(node);

    PieceNode *tail = newNode(back);

    insertAfter(node, tail);

    return tail;
}

void TextBuffer::removeBytes(uint32_t offset, uint32_t length)
{
    uint32_t left = length;

    while (left > 0) {
        uint32_t inside = 0;
        PieceNode *node = nodeAt(offset, &inside);

        if (!isReal(node)) {
            break;
        }

        // 需要删掉的内容从块中间开始：先切开，再删后半段
        if (inside > 0) {
            splitNode(node, inside);
            node = nodeAt(offset, &inside);
        }

        const uint32_t take = std::min(node->piece.length, left);

        // 只删本块的一部分：先切开，再删前一段
        if (take < node->piece.length) {
            splitNode(node, take);
            node = nodeAt(offset, &inside);
        }

        removeNode(node);
        left -= take;
    }
}

void TextBuffer::merge()
{
    // 每次只合并第一对可合并的相邻块：删除结点会改动树的结构，
    // 一次遍历里攒下一批「待删结点」再统一删，后面的结点可能已经不在原位了
    while (mergeOnePair()) {
    }
}

bool TextBuffer::mergeOnePair()
{
    std::vector<PieceNode *> stack;
    PieceNode *current = _root;
    PieceNode *last = _nil;

    // 中序遍历，遇到第一对同缓冲区首尾相接的块就合并
    while (isReal(current) || !stack.empty()) {
        while (isReal(current)) {
            stack.push_back(current);
            current = current->left;
        }

        current = stack.back();
        stack.pop_back();

        if (isReal(last)) {
            const Piece &a = last->piece;
            const Piece &b = current->piece;

            if (a.buffer == b.buffer && a.begin + a.length == b.begin) {
                Piece grown = a;

                grown.length += b.length;
                grown.lines += b.lines;

                last->piece = grown;
                refreshUp(last);
                removeNode(current);

                return true;
            }
        }

        last = current;
        current = current->right;
    }

    return false;
}

void TextBuffer::compact()
{
    // 先刷新统计量：text() 依赖 _bytes，用旧值取会把新加的内容截掉
    recount();

    const std::string all = text();
    Buffer *buffer = new Buffer();

    buffer->text = all;
    buffer->lineStart.push_back(0);

    for (size_t i = 0; i < all.size(); i++) {
        if (all[i] == '\n') {
            buffer->lineStart.push_back(static_cast<uint32_t>(i + 1));
        }
    }

    freeTree();

    for (size_t i = 0; i < _buffers.size(); i++) {
        delete _buffers[i];
    }

    _buffers.clear();
    _buffers.push_back(buffer);

    if (!all.empty()) {
        Piece piece;
        piece.buffer = 0;
        piece.begin = 0;
        piece.length = static_cast<uint32_t>(all.size());
        piece.lines = countLines(all.data(), all.size());

        insertNode(newNode(piece));
    }

    _count = all.empty() ? 0 : 1;
}

void TextBuffer::recount()
{
    uint32_t bytes = 0;
    uint32_t lines = 1;
    uint32_t count = 0;

    std::vector<PieceNode *> stack;

    if (isReal(_root)) {
        stack.push_back(_root);
    }

    while (!stack.empty()) {
        PieceNode *node = stack.back();

        stack.pop_back();

        bytes += node->piece.length;
        lines += node->piece.lines;
        count++;

        if (isReal(node->left)) {
            stack.push_back(node->left);
        }

        if (isReal(node->right)) {
            stack.push_back(node->right);
        }
    }

    _bytes = bytes;
    _lines = lines;
    _count = count;
    _longest = 0;

    // 最长行只在需要时才用，这里按行扫一遍；行首换算依赖 _lines，所以放在最后
    for (uint32_t line = 0; line < _lines; line++) {
        const uint32_t length = lineEnd(line) - lineBegin(line);

        if (length > _longest) {
            _longest = length;
        }
    }
}

// ---------------------------------------------------------------------------
// 读取
// ---------------------------------------------------------------------------

std::string TextBuffer::text() const
{
    return range(0, _bytes);
}

std::string TextBuffer::range(uint32_t from, uint32_t to) const
{
    std::string out;

    if (from >= to) {
        return out;
    }

    to = clampTo(to, _bytes);

    if (from >= to) {
        return out;
    }

    uint32_t inside = 0;
    PieceNode *node = nodeAt(from, &inside);
    uint32_t reader = from;

    // 中序后继依次取块，直到凑够区间
    while (isReal(node) && reader < to) {
        const std::string &source = _buffers[node->piece.buffer]->text;
        const uint32_t takeFrom = node->piece.begin + inside;
        const uint32_t available = node->piece.length - inside;
        const uint32_t taken = std::min(available, to - reader);

        out.append(source.data() + takeFrom, taken);

        reader += taken;
        inside = 0;

        if (isReal(node->right)) {
            node = leftmostNode(node->right);

            continue;
        }

        // 没有右子树就向上找第一个「从左拐上来」的祖先
        PieceNode *up = node->parent;

        while (isReal(up) && up->right == node) {
            node = up;
            up = up->parent;
        }

        node = isReal(up) ? up : _nil;
    }

    return out;
}

uint32_t TextBuffer::lineCount() const
{
    return _lines;
}

uint32_t TextBuffer::lineBegin(uint32_t line) const
{
    if (line == 0) {
        return 0;
    }

    if (line >= _lines) {
        return _bytes;
    }

    return lineBeginAt(line);
}

uint32_t TextBuffer::lineEnd(uint32_t line) const
{
    if (line + 1 >= _lines) {
        return _bytes;
    }

    // 下一行行首回退一个字节就是本行的换行符
    return lineBegin(line + 1) - 1;
}

std::string TextBuffer::lineText(uint32_t line) const
{
    if (line >= _lines) {
        return std::string();
    }

    return range(lineBegin(line), lineEnd(line));
}

uint32_t TextBuffer::offsetOf(uint32_t line, uint32_t column) const
{
    if (line >= _lines) {
        return _bytes;
    }

    return clampTo(lineBegin(line) + column, _bytes);
}

Position TextBuffer::positionOf(uint32_t offset) const
{
    const uint32_t target = clampTo(offset, _bytes);

    Position position;

    // 第一遍：数出目标之前有多少个换行，也就是行号。
    // 边界约定与 nodeAt 一致：落点等于块尾且右子树为空时留在本块，
    // 这样 target 往后推一个字节行号不会跳。
    uint32_t line = 0;
    uint32_t here = target;
    PieceNode *walk = _root;

    while (isReal(walk)) {
        const uint32_t blockEnd = walk->size + walk->piece.length;

        // 左子树与本块都排在目标前面，整块跳过
        if (here > blockEnd || (here == blockEnd && isReal(walk->right))) {
            here -= blockEnd;
            line += walk->lines + walk->piece.lines;
            walk = walk->right;

            continue;
        }

        // 目标落在左子树里：左子树的换行还没算，留在后面数
        if (here < walk->size) {
            walk = walk->left;

            continue;
        }

        // 目标在本块内（或正好停在块尾）：先补上左子树的换行
        line += walk->lines;

        if (here > walk->size) {
            const uint32_t inside = here - walk->size;
            const std::string &source = _buffers[walk->piece.buffer]->text;

            for (uint32_t p = 0; p < inside; p++) {
                if (source[walk->piece.begin + p] == '\n') {
                    line++;
                }
            }
        }

        break;
    }

    position.line = line;

    // 第二遍：求该行行首的字节下标。行首可能落在别的块里，
    // 所以必须按行号在树上下降，不能只看目标所在的那一块
    position.column = target - lineBeginAt(line);

    return position;
}

long TextBuffer::find(const std::string &needle, uint32_t from) const
{
    if (needle.empty() || from >= _bytes) {
        return -1;
    }

    // 整篇取出后查找：精简版优先保证逻辑短，不追求大文件下的极限性能
    const std::string all = text();
    const size_t hit = all.find(needle, from);

    if (hit == std::string::npos) {
        return -1;
    }

    return static_cast<long>(hit);
}

// ---------------------------------------------------------------------------
// 修改
// ---------------------------------------------------------------------------

uint32_t TextBuffer::insert(uint32_t offset, const std::string &text)
{
    if (text.empty()) {
        return 0;
    }

    offset = clampTo(offset, _bytes);

    // 换行统一成 LF：换行符永远不会被块边界劈开，块的换行计数才自洽
    std::string clean;
    clean.reserve(text.size());

    for (size_t i = 0; i < text.size(); i++) {
        if (text[i] == '\r') {
            if (i + 1 < text.size() && text[i + 1] == '\n') {
                i++;
            }

            clean.push_back('\n');
        } else {
            clean.push_back(text[i]);
        }
    }

    // 先决定新块的落点：offset 所在的块与块内偏移，插入位置只算一次
    uint32_t inside = 0;
    PieceNode *where = nodeAt(offset, &inside);

    if (isReal(where) && inside > 0) {
        if (inside < where->piece.length) {
            // 落点在块内部：切开，新内容插到后半段之前
            where = splitNode(where, inside);
        } else {
            // 落点正好在本块末尾：新内容排到本块之后
            inside = 0;
            where = nextNode(where);
        }
    }

    // 大段文本先切块，再依次插到 where 之前
    size_t cursor = 0;

    while (cursor < clean.size()) {
        size_t take = std::min(ChunkSize, clean.size() - cursor);
        size_t back = 0;

        // 落点若压在 UTF-8 续字节上就往前退，避免把一个字符切成两半
        while (back < 4 && cursor + take < clean.size() && isContinuation(clean[cursor + take])) {
            take--;
            back++;
        }

        const std::string chunk = clean.substr(cursor, take);

        Piece piece;
        piece.buffer = addBuffer(chunk);
        piece.begin = 0;
        piece.length = static_cast<uint32_t>(chunk.size());
        piece.lines = countLines(chunk.data(), chunk.size());

        if (isReal(where)) {
            insertBefore(where, newNode(piece));
        } else {
            insertNode(newNode(piece));
        }

        cursor += take;
    }

    merge();

    if (_count > MaxPieces) {
        compact();
    }

    recount();

    return static_cast<uint32_t>(clean.size());
}

uint32_t TextBuffer::erase(uint32_t from, uint32_t to)
{
    if (from >= to) {
        return 0;
    }

    to = clampTo(to, _bytes);

    if (from >= to) {
        return 0;
    }

    const uint32_t removed = to - from;

    removeBytes(from, removed);
    merge();

    if (_count > MaxPieces) {
        compact();
    }

    recount();

    return removed;
}

uint32_t TextBuffer::replace(uint32_t from, uint32_t to, const std::string &text)
{
    const uint32_t removed = erase(from, to);

    insert(from, text);

    return removed;
}

// ---------------------------------------------------------------------------
// 自检
// ---------------------------------------------------------------------------

uint32_t TextBuffer::checkSubtree(PieceNode *node, const char *&where) const
{
    if (!isReal(node)) {
        return 1;
    }

    // 不变量 4：红结点的两个孩子都必须是黑的
    if (node->red && (node->left->red || node->right->red)) {
        where = "红结点的孩子是红的（违反不变量 4）";

        return 0;
    }

    const uint32_t leftSize = isReal(node->left) ? node->left->totalSize : 0;
    const uint32_t leftLines = isReal(node->left) ? node->left->totalLines : 0;
    const uint32_t rightSize = isReal(node->right) ? node->right->totalSize : 0;
    const uint32_t rightLines = isReal(node->right) ? node->right->totalLines : 0;

    // 子树统计量必须与实际一致
    if (node->size != leftSize || node->lines != leftLines) {
        where = "左子树统计不对";

        return 0;
    }

    if (node->totalSize != leftSize + node->piece.length + rightSize) {
        where = "子树字节总量不对";

        return 0;
    }

    if (node->totalLines != leftLines + node->piece.lines + rightLines) {
        where = "子树行数总量不对";

        return 0;
    }

    // 块的字节区间必须落在缓冲区里，换行计数必须与内容一致
    if (node->piece.buffer >= _buffers.size()) {
        where = "块的缓冲区编号越界";

        return 0;
    }

    const std::string &source = _buffers[node->piece.buffer]->text;

    if (node->piece.begin + node->piece.length > source.size()) {
        where = "块的字节区间超出缓冲区";

        return 0;
    }

    if (countLines(source.data() + node->piece.begin, node->piece.length) != node->piece.lines) {
        where = "块的换行计数不对";

        return 0;
    }

    if (isReal(node->left) && node->left->parent != node) {
        where = "左孩子的父指针不对";

        return 0;
    }

    if (isReal(node->right) && node->right->parent != node) {
        where = "右孩子的父指针不对";

        return 0;
    }

    const uint32_t leftHeight = checkSubtree(node->left, where);

    if (where != nullptr) {
        return 0;
    }

    const uint32_t rightHeight = checkSubtree(node->right, where);

    if (where != nullptr) {
        return 0;
    }

    // 不变量 5：任一点到其所有后代叶子的路径上黑结点数相同
    if (leftHeight != rightHeight) {
        where = "左右子树黑高不同（违反不变量 5）";

        return 0;
    }

    return leftHeight + (node->red ? 0 : 1);
}

bool TextBuffer::checkTree(const char *&where) const
{
    // 不变量 1：每个结点非红即黑（用 bool 表示，天然满足）
    // 不变量 3：叶子（哨兵）是黑的
    if (_nil->red) {
        where = "哨兵是红的（违反不变量 3）";

        return false;
    }

    // 不变量 2：根结点是黑的
    if (_root != _nil && _root->red) {
        where = "根结点是红的（违反不变量 2）";

        return false;
    }

    if (_root != _nil && _root->parent != _nil) {
        where = "根结点的父指针不是哨兵";

        return false;
    }

    checkSubtree(_root, where);

    return where == nullptr;
}

bool TextBuffer::check(const char *&where) const
{
    where = nullptr;

    if (!checkTree(where)) {
        return false;
    }

    uint32_t bytes = 0;
    uint32_t lines = 1;
    uint32_t count = 0;

    std::vector<PieceNode *> stack;

    if (isReal(_root)) {
        stack.push_back(_root);
    }

    while (!stack.empty()) {
        PieceNode *node = stack.back();

        stack.pop_back();

        bytes += node->piece.length;
        lines += node->piece.lines;
        count++;

        if (isReal(node->left)) {
            stack.push_back(node->left);
        }

        if (isReal(node->right)) {
            stack.push_back(node->right);
        }
    }

    if (bytes != _bytes) {
        where = "总字节数与树不一致";

        return false;
    }

    if (lines != _lines) {
        where = "总行数与树不一致";

        return false;
    }

    if (count != _count) {
        where = "块数量与树不一致";

        return false;
    }

    return true;
}

bool TextBuffer::check() const
{
    const char *where = nullptr;

    return check(where);
}

void TextBuffer::dump() const
{
    // 中序打印，同时标出每个结点自己记的左子树统计与所在位置
    std::vector<PieceNode *> stack;
    PieceNode *current = _root;
    uint32_t cursor = 0;
    uint32_t index = 0;

    std::printf("bytes=%u lines=%u pieces=%u height=%u\n", _bytes, _lines, _count, treeHeight());

    while (isReal(current) || !stack.empty()) {
        while (isReal(current)) {
            stack.push_back(current);
            current = current->left;
        }

        current = stack.back();
        stack.pop_back();

        std::printf("  [%2u] size=%7u lines=%4u total=%7u len=%6u start=%7u red=%d\n", index++, current->size,
                    current->lines, current->totalSize, current->piece.length, cursor, current->red ? 1 : 0);

        cursor += current->piece.length;
        current = current->right;
    }

    std::fflush(stdout);
}
