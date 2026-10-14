#include "TextBuffer.h"

#include <algorithm>

/// how many pieces we tolerate before we rework the whole document in one go
static const uint32_t MaxPieces = 512;

/// a big insert is cut into chunks of this size so a single piece never grows
/// huge -- otherwise one paste would leave us with a million byte piece nobody
/// wants to copy around
static const size_t ChunkSize = 64 * 1024;

/// clamp a byte offset into [0, limit]
static uint32_t clampTo(uint32_t value, uint32_t limit)
{
    return (value > limit) ? limit : value;
}

/// is this byte a UTF-8 continuation byte, that is 0b10xxxxxx?
static bool isContinuation(char c)
{
    return (static_cast<unsigned char>(c) & 0xC0) == 0x80;
}

/// is this a real node and not the sentinel?
/// the sentinel IS our empty subtree and it doubles as every leaf, so an "empty
/// child" is never a null pointer. since we force piece.length to 0 on it, this
/// one comparison tells the two apart, and it is cheap enough to sprinkle around
static bool isReal(const PieceNode *node)
{
    return node != nullptr && node->piece.length > 0;
}

TextBuffer::TextBuffer()
{
    // the sentinel: black (invariant 3) and pointing at itself everywhere, so we
    // never have to check for null while walking the tree
    _nil = new PieceNode();
    _nil->red = false;
    _nil->parent = _nil;
    _nil->left = _nil;
    _nil->right = _nil;
    _root = _nil;

    // buffer 0 is the (still empty) original text, and line 0 starts at byte 0
    _buffers.push_back(new Buffer());
    _buffers[0]->lineStart.push_back(0);
}

TextBuffer::~TextBuffer()
{
    // freeTree() throws away every node we ever parked in _released, then we drop
    // the buffers
    freeTree();

    for (size_t i = 0; i < _buffers.size(); i++) {
        delete _buffers[i];
    }

    // and do not forget the sentinel, it belongs to us too
    delete _nil;
}

void TextBuffer::freeTree()
{
    // every node we removed went into _released instead of being deleted, and a
    // node is never released twice, so this single pass frees the whole tree
    for (size_t i = 0; i < _released.size(); i++) {
        delete _released[i];
    }

    _released.clear();
    _root = _nil;
}

void TextBuffer::clear()
{
    freeTree();

    // buffer 0 survives, every buffer we appended along the way is dropped
    for (size_t i = 1; i < _buffers.size(); i++) {
        delete _buffers[i];
    }

    _buffers.resize(1);
    _buffers[0]->text.clear();
    _buffers[0]->lineStart.clear();
    _buffers[0]->lineStart.push_back(0);

    // an empty document is still one line, and it holds no pieces at all
    _bytes = 0;
    _lines = 1;
    _longest = 0;
    _count = 0;
}

// ===================== tree construction and maintenance =====================

PieceNode *TextBuffer::newNode(const Piece &piece)
{
    // a brand new node is always red, the insert fixup repaints it if it has to
    PieceNode *node = new PieceNode();

    node->piece = piece;
    node->red = true;

    // both children start as the sentinel, and so does the father -- whoever
    // calls attach() will give it a real parent
    node->parent = _nil;
    node->left = _nil;
    node->right = _nil;

    // no children yet, so the left side counters are 0 and the subtree is just us
    node->size = 0;
    node->lines = 0;
    node->totalSize = piece.length;
    node->totalLines = piece.lines;

    return node;
}

void TextBuffer::refresh(PieceNode *node)
{
    // the sentinel has no counters worth maintaining
    if (!isReal(node)) {
        return;
    }

    // size and lines describe the LEFT subtree only, so an empty child gives 0
    node->size = isReal(node->left) ? node->left->totalSize : 0;
    node->lines = isReal(node->left) ? node->left->totalLines : 0;

    node->totalSize = node->size + node->piece.length + (isReal(node->right) ? node->right->totalSize : 0);
    node->totalLines = node->lines + node->piece.lines + (isReal(node->right) ? node->right->totalLines : 0);
}

void TextBuffer::refreshUp(PieceNode *node)
{
    // node itself, and then every ancestor up to the root.
    // miss a single level and the counters above it go quietly stale, and nodeAt()
    // will happily walk into the wrong piece much later -- this is THE classic bug
    // in a piece table, so whenever the shape of the tree changes, call this
    refresh(node);

    for (PieceNode *walk = node; walk->parent != _nil; walk = walk->parent) {
        refresh(walk->parent);
    }
}

void TextBuffer::rotateLeft(PieceNode *node)
{
    PieceNode *pivot = node->right;

    // there is nothing to rotate with
    if (!isReal(pivot)) {
        return;
    }

    // pivot takes node's slot, node plus its left subtree becomes pivot's left child
    node->right = pivot->left;

    // the subtree that just changed hands gets a new father (the sentinel needs none)
    if (isReal(pivot->left)) {
        pivot->left->parent = node;
    }

    // let pivot inherit node's old parent
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

    // only these two nodes changed shape, so only these two need recounting.
    // node first: pivot's counters are computed from node's, the other way round
    // would leave pivot describing a tree that no longer exists
    refresh(node);
    refresh(pivot);
}

void TextBuffer::rotateRight(PieceNode *node)
{
    PieceNode *pivot = node->left;

    // mirror image of rotateLeft, nothing to do without a left child
    if (!isReal(pivot)) {
        return;
    }

    // pivot takes node's slot, node plus its right subtree becomes pivot's right child
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

    // again, node before pivot
    refresh(node);
    refresh(pivot);
}

void TextBuffer::transplant(PieceNode *u, PieceNode *v)
{
    // hand v over to u's parent, the caller has already fixed v itself
    if (u->parent == _nil) {
        _root = v;
    } else if (u->parent->left == u) {
        u->parent->left = v;
    } else {
        u->parent->right = v;
    }

    // safe even when v is the sentinel, its parent field is throwaway memory
    v->parent = u->parent;
}

PieceNode *TextBuffer::leftmostNode(PieceNode *node) const
{
    // keep going left while there is a left child
    while (isReal(node->left)) {
        node = node->left;
    }

    return node;
}

void TextBuffer::attach(PieceNode *node, PieceNode *parent, bool asLeft)
{
    // hang a fresh node under parent, fresh nodes have no children of their own
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

    // the new node is a leaf, but the counters of every ancestor just grew
    refreshUp(node);

    _count++;
    insertFixup(node);
}

void TextBuffer::insertNode(PieceNode *node)
{
    // a new piece always goes last, and "last" is the rightmost node of the tree
    if (!isReal(_root)) {
        // empty tree, so we are the root
        attach(node, _nil, false);

        return;
    }

    // walk right until there is nothing more to the right
    PieceNode *slot = _root;

    while (isReal(slot->right)) {
        slot = slot->right;
    }

    attach(node, slot, false);
}

void TextBuffer::insertBefore(PieceNode *where, PieceNode *node)
{
    // "before where" means "the rightmost node of where's left subtree"
    if (!isReal(where)) {
        insertNode(node);

        return;
    }

    if (!isReal(where->left)) {
        // no left subtree, so we simply become its left child
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
    // "after where" means "the leftmost node of where's right subtree"
    if (!isReal(where)) {
        insertNode(node);

        return;
    }

    if (!isReal(where->right)) {
        // no right subtree, we are its right child directly
        attach(node, where, false);

        return;
    }

    attach(node, leftmostNode(where->right), true);
}

void TextBuffer::insertFixup(PieceNode *node)
{
    // INVARIANT 4: both children of a red node must be black. our brand new node
    // is red, so if dad is red too the tree is broken and we have to climb up and
    // repair. the loop ends the moment the conflict is gone
    while (node->parent->red) {
        PieceNode *parent = node->parent;
        PieceNode *grand = parent->parent;

        if (parent == grand->left) {
            PieceNode *uncle = grand->right;

            if (uncle->red) {
                // case 1: uncle is red -- push the red down to grandpa and carry
                // on from there, the conflict may just move two levels up
                parent->red = false;
                uncle->red = false;
                grand->red = true;
                node = grand;
            } else {
                if (node == parent->right) {
                    // case 2: we are the right child -- rotate dad left first so
                    // that we end up in case 3, and follow the rotation
                    node = parent;
                    rotateLeft(node);
                }

                // case 3: we are the left child now -- repaint and rotate grandpa
                node->parent->red = false;
                node->parent->parent->red = true;
                rotateRight(node->parent->parent);
            }
        } else {
            // exactly the mirror image: uncle sits on the left and every rotation
            // flips direction
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

    // INVARIANT 2: the root must be black. the loop above can leave it red, so we
    // always force it back here
    _root->red = false;
}

void TextBuffer::deleteFixup(PieceNode *node)
{
    // node carries one extra black. we bubble that extra black up the tree until
    // somebody can absorb it -- stopping as soon as node is red or we hit the root
    while (node != _root && !node->red) {
        PieceNode *parent = node->parent;

        if (node == parent->left) {
            PieceNode *brother = parent->right;

            if (brother->red) {
                // case 1: brother is red -- paint brother black and dad red, then
                // rotate dad left, which turns the whole thing into a case below
                brother->red = false;
                parent->red = true;
                rotateLeft(parent);
                brother = parent->right;
            }

            if (!brother->left->red && !brother->right->red) {
                // case 2: both of brother's children are black -- paint brother
                // red and push the extra black up to dad
                brother->red = true;
                node = parent;
            } else {
                if (!brother->right->red) {
                    // case 3: brother's right child is black -- rotate brother
                    // right, that is exactly the shape case 4 wants
                    brother->left->red = false;
                    brother->red = true;
                    rotateRight(brother);
                    brother = parent->right;
                }

                // case 4: brother's right child is red -- swap the colours, rotate
                // dad left, and the extra black is finally absorbed
                brother->red = parent->red;
                parent->red = false;
                brother->right->red = false;
                rotateLeft(parent);
                node = _root;
            }
        } else {
            // the mirrored four cases, with brother on the left
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

    // whatever node we stopped on is black from here on
    node->red = false;
}

void TextBuffer::removeNode(PieceNode *node)
{
    // the sentinel is not a node we can delete
    if (!isReal(node)) {
        return;
    }

    PieceNode *moved = node;
    PieceNode *child = _nil;
    bool wasRed = moved->red;

    if (!isReal(node->left)) {
        // no left child: the right child (maybe the sentinel) simply takes over
        child = node->right;
        transplant(node, node->right);
    } else if (!isReal(node->right)) {
        // no right child: same story on the other side
        child = node->left;
        transplant(node, node->left);
    } else {
        // both children are there, so the successor (leftmost node of the right
        // subtree) takes node's place and node itself gets parked in _released
        moved = leftmostNode(node->right);
        wasRed = moved->red;
        child = moved->right;

        if (moved->parent == node) {
            // moved is node's own child and keeps its place, only child's father
            // has to be patched. this is where the sentinel pays off: if child is
            // _nil we would otherwise write a null pointer here
            child->parent = moved;
        } else {
            // pull moved out of where it was
            transplant(moved, moved->right);

            // moved changed position, so the counters along its OLD father chain
            // must be fixed BEFORE we hang it under node -- the rotations and the
            // delete fixup below all read those counters and trust them
            refreshUp(moved->parent == _nil ? moved : moved->parent);

            moved->right = node->right;
            moved->right->parent = moved;
        }

        // now moved really sits where node used to be
        transplant(node, moved);
        moved->left = node->left;
        moved->left->parent = moved;

        // moved borrows node's colour, which is what preserves the black height
        moved->red = node->red;
    }

    // moved occupies node's slot now, so every counter above it is stale, and
    // deleteFixup() reads them on the way up -- fix the whole chain before that
    refreshUp(moved);

    // only a black node leaving takes a black along with it, hence the fixup
    if (!wasRed) {
        deleteFixup(child);
    }

    // do NOT delete node here, park it in _released until freeTree() runs
    _released.push_back(node);
    _count--;
}

// ===================== locate =====================

PieceNode *TextBuffer::nodeAt(uint32_t offset, uint32_t *inside) const
{
    // the deal: an offset strictly smaller than the total gives back the piece
    // that holds it and inside reports the offset within that piece. an offset
    // equal to the total gives back the sentinel, meaning "right at the end"
    if (offset >= _bytes) {
        if (inside != nullptr) {
            *inside = 0;
        }

        return _nil;
    }

    PieceNode *node = _root;

    while (isReal(node)) {
        const uint32_t blockEnd = node->size + node->piece.length;

        // the left subtree and this piece both sit entirely in front of the
        // target, so go right. watch out for the tie: landing exactly on the
        // piece end only moves right when a right subtree EXISTS, otherwise the
        // target is the last byte of this piece and we stay right here
        if (offset > blockEnd || (offset == blockEnd && isReal(node->right))) {
            offset -= blockEnd;
            node = node->right;

            continue;
        }

        // the target is somewhere in the left subtree
        if (offset < node->size) {
            node = node->left;

            continue;
        }

        // good, it is inside this piece, so hand back the offset within the piece
        if (inside != nullptr) {
            *inside = offset - node->size;
        }

        return node;
    }

    // fell out of the tree without a match, the caller gets the sentinel
    if (inside != nullptr) {
        *inside = 0;
    }

    return _nil;
}

uint32_t TextBuffer::lineBeginAt(uint32_t line) const
{
    // the two easy ends. callers are expected to hand over a valid line number
    if (line == 0) {
        return 0;
    }

    if (line >= _lines) {
        return _bytes;
    }

    // walk down the tree, cursor accumulates the bytes before the target line
    uint32_t cursor = 0;
    PieceNode *walk = _root;

    while (isReal(walk)) {
        // the target line lives in the left subtree, the subtree counter tells us so
        if (walk->lines >= line) {
            walk = walk->left;

            continue;
        }

        // skip past the left subtree and this piece
        line -= walk->lines;
        cursor += walk->size;

        // the target line is the first line of this piece
        if (line == 0) {
            return cursor;
        }

        // it is inside this piece, so scan for the line-th '\n' by hand. the
        // buffer keeps its own line table, but scanning here stays local
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
                    // one byte past the newline is where the next line starts
                    return cursor + p + 1;
                }
            }
        }

        // no luck, this piece does not own that line, keep going right
        line -= walk->piece.lines;
        cursor += walk->piece.length;
        walk = walk->right;
    }

    // ran off the end, that means the end of the document
    return _bytes;
}

PieceNode *TextBuffer::nextNode(PieceNode *node) const
{
    // in-order successor: the leftmost node of the right subtree when there is
    // one, otherwise climb until we come up from a left child
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
    // walk down the right spine and keep adding the part of every node that is
    // guaranteed to sit in front of node, namely its left subtree plus its own
    // piece. we stop the moment we land on node itself
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
    // pre-order with two explicit stacks (node and its depth) so that a deep tree
    // cannot blow the call stack. diagnostics only, nobody calls this in a loop
    std::vector<PieceNode *> stack;
    std::vector<uint32_t> depth;
    uint32_t best = 0;

    if (isReal(_root)) {
        // the root sits at depth 1, an empty tree reports height 0
        stack.push_back(_root);
        depth.push_back(1);
    }

    while (!stack.empty()) {
        PieceNode *node = stack.back();
        const uint32_t level = depth.back();

        stack.pop_back();
        depth.pop_back();

        // update best whenever this node is deeper than anything we saw so far
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

// ===================== internal helpers =====================

uint32_t TextBuffer::countLines(const char *data, size_t length)
{
    // a plain byte scan. every '\n' is one line break and CRLF was normalised to
    // LF long before we get here, so we do not have to care about '\r'
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
    // make a fresh read only buffer that owns a copy of text.
    // we NEVER free a buffer in here: an old piece may still point into it, every
    // buffer we ever made lives until compact() (or the destructor) drops them
    Buffer *buffer = new Buffer();

    buffer->text = text;

    // line 0 always starts at byte 0
    buffer->lineStart.push_back(0);

    // index every line start once, later lookups are just an array read
    for (size_t i = 0; i < text.size(); i++) {
        if (text[i] == '\n') {
            buffer->lineStart.push_back(static_cast<uint32_t>(i + 1));
        }
    }

    _buffers.push_back(buffer);

    // the id of a buffer is simply its slot in _buffers
    return static_cast<uint32_t>(_buffers.size() - 1);
}

PieceNode *TextBuffer::splitNode(PieceNode *node, uint32_t inside)
{
    // no point cutting at either end of the piece, and the sentinel is not
    // something we can cut at all
    if (!isReal(node) || inside == 0 || inside >= node->piece.length) {
        return _nil;
    }

    const Piece piece = node->piece;
    const char *data = _buffers[piece.buffer]->text.data() + piece.begin;
    const uint32_t headLines = countLines(data, inside);

    // the back half becomes its own piece, the front half stays in node
    Piece back;
    back.buffer = piece.buffer;
    back.begin = piece.begin + inside;
    back.length = piece.length - inside;
    back.lines = piece.lines - headLines;

    Piece front = piece;
    front.length = inside;
    front.lines = headLines;

    // shrink node first and fix the counters before the new node shows up
    node->piece = front;
    refreshUp(node);

    PieceNode *tail = newNode(back);

    // slip it right behind node so the two halves stay neighbours in piece order
    insertAfter(node, tail);

    return tail;
}

void TextBuffer::removeBytes(uint32_t offset, uint32_t length)
{
    // eat the range piece by piece, every round removes at most one whole piece
    uint32_t left = length;

    while (left > 0) {
        uint32_t inside = 0;
        PieceNode *node = nodeAt(offset, &inside);

        // offset walked past the end of the document, nothing left to do
        if (!isReal(node)) {
            break;
        }

        // the deletion starts in the middle of a piece: cut it and look again,
        // now the range starts exactly on a piece boundary
        if (inside > 0) {
            splitNode(node, inside);
            node = nodeAt(offset, &inside);
        }

        const uint32_t take = std::min(node->piece.length, left);

        // the range also ends inside this piece, so cut once more and drop the
        // front half whole instead of fiddling with individual bytes
        if (take < node->piece.length) {
            splitNode(node, take);
            node = nodeAt(offset, &inside);
        }

        // node now lies completely inside the range, drop it
        removeNode(node);
        left -= take;
    }
}

void TextBuffer::merge()
{
    // one pair per round on purpose. removing a node rearranges the tree, so
    // collecting a batch of doomed nodes and deleting them afterwards would leave
    // us holding pointers to nodes that already moved somewhere else
    while (mergeOnePair()) {
    }
}

bool TextBuffer::mergeOnePair()
{
    std::vector<PieceNode *> stack;
    PieceNode *current = _root;
    PieceNode *last = _nil;

    // in-order walk, stop at the first pair we can glue together
    while (isReal(current) || !stack.empty()) {
        // dive left as far as we can
        while (isReal(current)) {
            stack.push_back(current);
            current = current->left;
        }

        current = stack.back();
        stack.pop_back();

        if (isReal(last)) {
            const Piece &a = last->piece;
            const Piece &b = current->piece;

            // glue them only when they are neighbours inside the SAME buffer:
            // a.begin + a.length == b.begin is exactly what makes the join seamless
            if (a.buffer == b.buffer && a.begin + a.length == b.begin) {
                Piece grown = a;

                grown.length += b.length;
                grown.lines += b.lines;

                // grow the front piece, then throw the other node away
                last->piece = grown;
                refreshUp(last);
                removeNode(current);

                return true;
            }
        }

        // that pair did not merge, keep the walk moving
        last = current;
        current = current->right;
    }

    // nothing left to glue
    return false;
}

void TextBuffer::compact()
{
    // recount FIRST: text() reads _bytes, and a stale value would happily cut off
    // whatever we just inserted
    recount();

    // pull the whole document out as one string, then drop every old buffer
    const std::string all = text();
    Buffer *buffer = new Buffer();

    buffer->text = all;

    // build the line table for the fresh buffer in one pass
    buffer->lineStart.push_back(0);

    for (size_t i = 0; i < all.size(); i++) {
        if (all[i] == '\n') {
            buffer->lineStart.push_back(static_cast<uint32_t>(i + 1));
        }
    }

    // kill the old tree first, nothing in it is worth keeping
    freeTree();

    for (size_t i = 0; i < _buffers.size(); i++) {
        delete _buffers[i];
    }

    _buffers.clear();
    _buffers.push_back(buffer);

    // one buffer, one piece, done -- unless the document happens to be empty
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
    // iterate the tree once and rebuild every global counter from scratch
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

    // the longest line is only needed when somebody asks, so we rescan by line.
    // this has to stay AFTER _lines is set, lineBegin() depends on it
    for (uint32_t line = 0; line < _lines; line++) {
        const uint32_t length = lineEnd(line) - lineBegin(line);

        if (length > _longest) {
            _longest = length;
        }
    }
}

// ===================== read =====================

std::string TextBuffer::text() const
{
    // the whole document is just a range query over everything
    return range(0, _bytes);
}

std::string TextBuffer::range(uint32_t from, uint32_t to) const
{
    std::string out;

    // empty or inverted range, nothing to copy
    if (from >= to) {
        return out;
    }

    // be nice and clip the end instead of trusting the caller
    to = clampTo(to, _bytes);

    if (from >= to) {
        return out;
    }

    // find the piece holding `from`, inside tells us where to start inside it
    uint32_t inside = 0;
    PieceNode *node = nodeAt(from, &inside);
    uint32_t reader = from;

    // hop from piece to piece in order until the range is full
    while (isReal(node) && reader < to) {
        const std::string &source = _buffers[node->piece.buffer]->text;
        const uint32_t takeFrom = node->piece.begin + inside;
        const uint32_t available = node->piece.length - inside;
        const uint32_t taken = std::min(available, to - reader);

        out.append(source.data() + takeFrom, taken);

        reader += taken;

        // only the very first piece can start mid-piece
        inside = 0;

        if (isReal(node->right)) {
            node = leftmostNode(node->right);

            continue;
        }

        // no right subtree, so climb until we come up from a left child -- that
        // ancestor is the next piece in document order
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
    // the two ends are clamped here, lineBeginAt() trusts a valid line number
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
    // the last line (or anything past it) simply ends with the document
    if (line + 1 >= _lines) {
        return _bytes;
    }

    // the byte right before the next line start is this line's newline
    return lineBegin(line + 1) - 1;
}

std::string TextBuffer::lineText(uint32_t line) const
{
    // out of range gives back an empty string
    if (line >= _lines) {
        return std::string();
    }

    // careful: lineEnd() already stops before the trailing newline
    return range(lineBegin(line), lineEnd(line));
}

uint32_t TextBuffer::offsetOf(uint32_t line, uint32_t column) const
{
    // a line past the end means the end of the document
    if (line >= _lines) {
        return _bytes;
    }

    // the column is a BYTE offset, and we clip it so it cannot walk off the end
    return clampTo(lineBegin(line) + column, _bytes);
}

Position TextBuffer::positionOf(uint32_t offset) const
{
    const uint32_t target = clampTo(offset, _bytes);

    Position position;

    // ---- pass one: count the newlines in front of the target, that is the line ----
    // the tie rule matches nodeAt() exactly: landing on a piece end with an empty
    // right subtree stays in this piece, so nudging the target one byte forward
    // never bumps the line number by accident
    uint32_t line = 0;
    uint32_t here = target;
    PieceNode *walk = _root;

    while (isReal(walk)) {
        const uint32_t blockEnd = walk->size + walk->piece.length;

        // the whole block sits in front of the target, skip it and count its lines
        if (here > blockEnd || (here == blockEnd && isReal(walk->right))) {
            here -= blockEnd;
            line += walk->lines + walk->piece.lines;
            walk = walk->right;

            continue;
        }

        // the target is in the left subtree: those newlines are deliberately not
        // counted yet, they belong to nodes we still have to walk through
        if (here < walk->size) {
            walk = walk->left;

            continue;
        }

        // the target sits in this piece (or right on its end): add the left
        // subtree first, then hand-count the newlines between the piece start and us
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

    // ---- pass two: where does that line start ----
    // the line start may well live in a completely different piece, so we have to
    // descend the tree by line number again -- looking at the piece we landed in
    // is not enough
    position.column = target - lineBeginAt(line);

    return position;
}

long TextBuffer::find(const std::string &needle, uint32_t from) const
{
    // nothing to look for, or we start at the very end
    if (needle.empty() || from >= _bytes) {
        return -1;
    }

    // this cut down version drags the whole document out and lets std::string do
    // the searching: short and obviously correct wins over peak speed here
    const std::string all = text();
    const size_t hit = all.find(needle, from);

    if (hit == std::string::npos) {
        return -1;
    }

    return static_cast<long>(hit);
}

// ===================== modify =====================

uint32_t TextBuffer::insert(uint32_t offset, const std::string &text)
{
    // inserting nothing is a no-op, and we do not want to register an empty buffer
    if (text.empty()) {
        return 0;
    }

    // keep the offset inside the document
    offset = clampTo(offset, _bytes);

    // ---- normalise the line endings to LF ----
    // CRLF and a lone CR both become one LF. that way a newline is never split
    // across a piece boundary, and every piece's newline count stays honest
    std::string clean;
    clean.reserve(text.size());

    for (size_t i = 0; i < text.size(); i++) {
        if (text[i] == '\r') {
            // CRLF is two bytes but only ONE line break, so swallow the second byte
            if (i + 1 < text.size() && text[i + 1] == '\n') {
                i++;
            }

            clean.push_back('\n');
        } else {
            clean.push_back(text[i]);
        }
    }

    // ---- work out where we land, ONCE ----
    // the piece plus the offset inside it, no need to search again and again
    uint32_t inside = 0;
    PieceNode *where = nodeAt(offset, &inside);

    if (isReal(where) && inside > 0) {
        if (inside < where->piece.length) {
            // landing mid-piece: cut it, the new text goes in front of the tail
            where = splitNode(where, inside);
        } else {
            // landing exactly on the piece end: the new text goes after this piece,
            // which is the same as "in front of the next one"
            inside = 0;
            where = nextNode(where);
        }
    }

    // ---- cut the payload into chunks and slip them in ----
    size_t cursor = 0;

    while (cursor < clean.size()) {
        size_t take = std::min(ChunkSize, clean.size() - cursor);
        size_t back = 0;

        // if the cut would land on a UTF-8 continuation byte, pull it back so we
        // never slice a multi-byte character in half
        while (back < 4 && cursor + take < clean.size() && isContinuation(clean[cursor + take])) {
            take--;
            back++;
        }

        const std::string chunk = clean.substr(cursor, take);

        // every chunk becomes its own buffer and its own piece
        Piece piece;
        piece.buffer = addBuffer(chunk);
        piece.begin = 0;
        piece.length = static_cast<uint32_t>(chunk.size());
        piece.lines = countLines(chunk.data(), chunk.size());

        // the first chunk lands in front of `where`, the rest queue up behind it
        if (isReal(where)) {
            insertBefore(where, newNode(piece));
        } else {
            insertNode(newNode(piece));
        }

        cursor += take;
    }

    // ---- tidy up ----
    // an insert in the middle of a piece leaves two pieces that plainly belong
    // together, so try to glue the neighbours back
    merge();

    // too many pieces? rework the whole document once (see compact()) and be done
    if (_count > MaxPieces) {
        compact();
    }

    // the counters moved, rebuild them
    recount();

    // note we report the NORMALISED length, CRLF may have shrunk the payload
    return static_cast<uint32_t>(clean.size());
}

uint32_t TextBuffer::erase(uint32_t from, uint32_t to)
{
    to = clampTo(to, _bytes);

    if (from >= to) {
        return 0;
    }

    // remember the amount now, _bytes moves under our feet once we start deleting
    const uint32_t removed = to - from;

    removeBytes(from, removed);

    // deletion leaves pieces that want to be merged back together
    merge();

    if (_count > MaxPieces) {
        compact();
    }

    recount();

    return removed;
}

uint32_t TextBuffer::replace(uint32_t from, uint32_t to, const std::string &text)
{
    // the lazy version: erase then insert. callers only want the byte count back
    const uint32_t removed = erase(from, to);

    insert(from, text);

    return removed;
}

// ===================== self check =====================

uint32_t TextBuffer::checkSubtree(PieceNode *node, const char *&where) const
{
    // the sentinel counts as one black node when we compute the black height
    if (!isReal(node)) {
        return 1;
    }

    // INVARIANT 4: both children of a red node must be black
    if (node->red && (node->left->red || node->right->red)) {
        where = "红结点的孩子是红的（违反不变量 4）";

        return 0;
    }

    const uint32_t leftSize = isReal(node->left) ? node->left->totalSize : 0;
    const uint32_t leftLines = isReal(node->left) ? node->left->totalLines : 0;
    const uint32_t rightSize = isReal(node->right) ? node->right->totalSize : 0;
    const uint32_t rightLines = isReal(node->right) ? node->right->totalLines : 0;

    // size and lines describe the left subtree only, so they must match it exactly
    if (node->size != leftSize || node->lines != leftLines) {
        where = "左子树统计不对";

        return 0;
    }

    // and the subtree totals must match left + us + right
    if (node->totalSize != leftSize + node->piece.length + rightSize) {
        where = "子树字节总量不对";

        return 0;
    }

    if (node->totalLines != leftLines + node->piece.lines + rightLines) {
        where = "子树行数总量不对";

        return 0;
    }

    // the piece must point into a buffer that actually exists
    if (node->piece.buffer >= _buffers.size()) {
        where = "块的缓冲区编号越界";

        return 0;
    }

    const std::string &source = _buffers[node->piece.buffer]->text;

    // and its byte range has to fit inside that buffer
    if (node->piece.begin + node->piece.length > source.size()) {
        where = "块的字节区间超出缓冲区";

        return 0;
    }

    // the cached newline count must match the bytes it claims to cover
    if (countLines(source.data() + node->piece.begin, node->piece.length) != node->piece.lines) {
        where = "块的换行计数不对";

        return 0;
    }

    // both children must point back at us (the sentinel has no father to check)
    if (isReal(node->left) && node->left->parent != node) {
        where = "左孩子的父指针不对";

        return 0;
    }

    if (isReal(node->right) && node->right->parent != node) {
        where = "右孩子的父指针不对";

        return 0;
    }

    // recurse left, then right, and stop at the first failure
    const uint32_t leftHeight = checkSubtree(node->left, where);

    if (where != nullptr) {
        return 0;
    }

    const uint32_t rightHeight = checkSubtree(node->right, where);

    if (where != nullptr) {
        return 0;
    }

    // INVARIANT 5: every path from here down to a leaf holds the same black count
    if (leftHeight != rightHeight) {
        where = "左右子树黑高不同（违反不变量 5）";

        return 0;
    }

    // add one for ourselves when we are black
    return leftHeight + (node->red ? 0 : 1);
}

bool TextBuffer::checkTree(const char *&where) const
{
    // INVARIANT 1 (every node is red or black) comes for free, `red` is a bool.
    // INVARIANT 3: the leaves, which are our sentinel, are black
    if (_nil->red) {
        where = "哨兵是红的（违反不变量 3）";

        return false;
    }

    // INVARIANT 2: the root is black
    if (_root != _nil && _root->red) {
        where = "根结点是红的（违反不变量 2）";

        return false;
    }

    // the root's father must be the sentinel, several fixup paths lean on that
    if (_root != _nil && _root->parent != _nil) {
        where = "根结点的父指针不是哨兵";

        return false;
    }

    // the recursion takes care of invariants 4 and 5 and of every counter on the way
    checkSubtree(_root, where);

    return where == nullptr;
}

bool TextBuffer::check(const char *&where) const
{
    where = nullptr;

    // invariants first: they are cheap and they catch the truly nasty bugs
    if (!checkTree(where)) {
        return false;
    }

    // then walk the tree and rebuild the three global counters by hand. if one of
    // them disagrees somebody forgot a refresh()/recount(), which is exactly the
    // kind of bug this whole file is written to avoid
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
    // convenience overload, the reason is thrown away
    const char *where = nullptr;

    return check(where);
}

void TextBuffer::dump() const
{
    // in-order dump. each node prints its OWN cached left side counters next to
    // the real position it sits at, so a counter that went stale is easy to spot
    std::vector<PieceNode *> stack;
    PieceNode *current = _root;
    uint32_t cursor = 0;
    uint32_t index = 0;

    std::printf("bytes=%u lines=%u pieces=%u height=%u\n", _bytes, _lines, _count, treeHeight());

    while (isReal(current) || !stack.empty()) {
        // same in-order walk as everywhere else, dive left first
        while (isReal(current)) {
            stack.push_back(current);
            current = current->left;
        }

        current = stack.back();
        stack.pop_back();

        std::printf("  [%2u] size=%7u lines=%4u total=%7u len=%6u start=%7u red=%d\n", index++, current->size,
                    current->lines, current->totalSize, current->piece.length, cursor, current->red ? 1 : 0);

        // cursor tracks where this piece really starts in the document
        cursor += current->piece.length;
        current = current->right;
    }

    std::fflush(stdout);
}
