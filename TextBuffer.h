#ifndef TEXTBUFFER_H
#define TEXTBUFFER_H

#include <cstdint>
#include <string>
#include <vector>

/// a piece: one contiguous run of bytes inside one of the buffers
/// we describe it by "start byte + byte length" only. the original piece also
/// carried the line and column of both ends, we dropped all four of those fields
struct Piece
{
    uint32_t buffer = 0;  /// which buffer we point into
    uint32_t begin  = 0;   /// first byte of our run inside that buffer
    uint32_t length = 0;  /// how many bytes the run has
    uint32_t lines  = 0;   /// how many '\n' sit inside those bytes
};

/// a read only buffer: the text plus the byte offset of every line start
/// we never edit a buffer in place, we only ever add new ones (the original text
/// and the text the user typed), so an old piece can keep pointing into one
struct Buffer
{
    std::string text;                 /// the whole text of this buffer
    std::vector<uint32_t> lineStart;  /// byte offset where each line begins
};

/// a place in the document, line and column both start at 0
/// watch out: the column is counted in BYTES here, not in characters
struct Position
{
    uint32_t line = 0;
    uint32_t column = 0;
};

/// a red black tree node. pieces hang in this tree in document order, so a plain
/// in-order walk hands the document back
/// the sentinel's piece.length is always 0, and isReal() uses exactly that to
/// tell a real node from an empty subtree -- the sentinel doubles as every leaf,
/// so an "empty child" is never a null pointer you can follow by accident
struct PieceNode
{
    Piece piece;                  /// the piece this node holds
    bool red = false;             /// red or black, that is the whole colour
    PieceNode *parent = nullptr;  /// father, the root's father is the sentinel
    PieceNode *left = nullptr;    /// left child, sentinel when empty
    PieceNode *right = nullptr;   /// right child, sentinel when empty
    uint32_t size = 0;            /// total bytes of the LEFT subtree only
    uint32_t lines = 0;           /// total newlines of the LEFT subtree only
    uint32_t totalSize = 0;       /// total bytes of the whole subtree
    uint32_t totalLines = 0;      /// total newlines of the whole subtree
};

/// a cut down piece table backed by a red black tree
///
/// how it relates to the original, and what we gave up on purpose:
///   - the two ideas that really matter are kept:
///       1) text is append only and editing only rewrites the piece table, so an
///          insert or an erase costs O(log pieces) no matter how long the
///          document is -- that is the whole point of a piece table
///       2) the piece table is indexed by a red black tree, rotations keep it
///          balanced and the subtree counters let us walk down by byte offset
///   - the original's twin line/column coordinate system is gone, a piece now
///     remembers a byte offset and a byte length and nothing else
///   - the undo stack, the encoding detection and the line ending bookkeeping are
///     gone too, only the minimum that still runs is left
///   - the tree uses the classic "one single sentinel" trick, and checkTree()
///     verifies the five red black invariants one by one
///
/// every operation is O(log pieces), and once we hold too many pieces we rework
/// the whole document in one go (compact())
class TextBuffer
{
public:
    /// build an empty buffer
    TextBuffer();

    /// release every node and every buffer we own
    ~TextBuffer();

    /// throw everything away and go back to an empty document
    void clear();

    /// returns the whole document
    std::string text() const;

    /// returns the bytes in [from, to)
    std::string range(uint32_t from, uint32_t to) const;

    /// insert text at offset, returns how many bytes really went in
    /// CRLF and a lone CR are both normalised to LF first, so a newline never
    /// ends up split across a piece boundary
    uint32_t insert(uint32_t offset, const std::string &text);

    uint32_t erase(uint32_t from, uint32_t to);
    uint32_t replace(uint32_t from, uint32_t to, const std::string &text);

    /// (line, column) -> byte offset
    /// byte offset -> (line, column)
    uint32_t offsetOf(uint32_t line, uint32_t column) const;
    Position positionOf(uint32_t offset) const;

    /// byte offset where line `line` starts
    uint32_t lineBegin(uint32_t line) const;

    /// byte offset where line `line` ends, the trailing newline is NOT included
    uint32_t lineEnd(uint32_t line) const;

    /// the text of line `line`, again without its trailing newline
    std::string lineText(uint32_t line) const;

    /// first occurrence of needle at or after `from`, -1 when there is none
    long find(const std::string &needle, uint32_t from) const;

    /// how many lines we have, an empty document still counts as one
    uint32_t lineCount() const;
    uint32_t byteCount() const   { return _bytes;   }
    uint32_t longestLine() const { return _longest; }
    uint32_t pieceCount() const  { return _count;   }
    uint32_t treeHeight() const;

    /// check yourself: piece ranges, subtree counters and the five red black
    /// invariants, all of them
    bool check() const;

    /// the same check, but tell me which one broke through `where`
    bool check(const char *&where) const;

    /// print the piece table in order, for chasing down a counter that went wrong
    void dump() const;

private:
    /// make a red node (children start at the sentinel, the insert fixup decides
    /// the colour afterwards)
    PieceNode *newNode(const Piece &piece);

    /// recompute one node's subtree counters
    void refresh(PieceNode *node);

    /// recompute a node and every ancestor up to the root
    void refreshUp(PieceNode *node);
    void rotateLeft(PieceNode *node);
    void rotateRight(PieceNode *node);

    /// put v where u used to sit
    void transplant(PieceNode *u, PieceNode *v);

    /// the leftmost node of a subtree
    PieceNode *leftmostNode(PieceNode *node) const;

    /// the next node in piece order (in-order successor), sentinel when there is none
    PieceNode *nextNode(PieceNode *node) const;

    /// hang node on one side of parent, recount up the father chain and run the
    /// insert fixup
    void attach(PieceNode *node, PieceNode *parent, bool asLeft);

    /// insert at the very end of the piece order
    void insertNode(PieceNode *node);

    /// insert right before `where` in piece order
    void insertBefore(PieceNode *where, PieceNode *node);

    /// insert right after `where` in piece order
    void insertAfter(PieceNode *where, PieceNode *node);

    /// insert fixup: get rid of two reds in a row
    void insertFixup(PieceNode *node);

    /// delete fixup: get rid of a double black
    void deleteFixup(PieceNode *node);

    /// drop a node for real (red black delete plus fixup), the node gets parked in
    /// the released list instead of being freed right away
    void removeNode(PieceNode *node);

    /// find the node holding this byte offset, inside returns the offset in the piece
    PieceNode *nodeAt(uint32_t offset, uint32_t *inside) const;

    /// byte offset of a line start (internal, the line number must already be valid)
    uint32_t lineBeginAt(uint32_t line) const;

    /// count the bytes in front of node
    uint32_t bytesBefore(PieceNode *node) const;

    /// cut node at offset `inside` and give back the back half, the front half
    /// stays in node
    PieceNode *splitNode(PieceNode *node, uint32_t inside);

    /// delete length bytes starting at offset, piece boundaries are handled inside
    void removeBytes(uint32_t offset, uint32_t length);

    /// glue neighbouring pieces that are contiguous
    void merge();

    /// find the first pair of neighbouring pieces that can merge, glue them and
    /// return true; false when there is nothing left to glue
    bool mergeOnePair();

    /// rework everything: dump the document into one fresh buffer and shrink the
    /// tree down to a single node
    void compact();

    /// rebuild total bytes, total lines, piece count and the longest line
    void recount();

    /// check the red black invariants, the broken one is written into `where`
    bool checkTree(const char *&where) const;

    /// recursively check a subtree: returns its black height and checks the red
    /// children, the counters and the piece range on the way
    uint32_t checkSubtree(PieceNode *node, const char *&where) const;

    /// count the newlines inside a chunk of text
    static uint32_t countLines(const char *data, size_t length);

    /// register a chunk of text as a new buffer and hand back its id
    uint32_t addBuffer(const std::string &text);

    /// drop every node and reset the tree to empty
    void freeTree();

    std::vector<Buffer *> _buffers;      /// every read only buffer we ever made
    PieceNode *_nil    = nullptr;           /// the sentinel, one single one for the tree
    PieceNode *_root   = nullptr;          /// the root
    std::vector<PieceNode *> _released;  /// deleted nodes, all freed by freeTree()
    uint32_t _bytes     =  0;                 /// total bytes
    uint32_t _lines     =  1;                 /// total lines
    uint32_t _longest   =  0;               /// bytes of the longest line
    uint32_t _count     =  0;                 /// how many pieces we hold
};

#endif // TEXTBUFFER_H
