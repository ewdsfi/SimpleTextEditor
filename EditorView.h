#ifndef EDITORVIEW_H
#define EDITORVIEW_H

#include "LineMap.h"
#include "TextBuffer.h"

#include <QAbstractScrollArea>
#include <QColor>
#include <QFont>
#include <QFontMetrics>
#include <QHash>
#include <QPoint>
#include <QTimer>

class Document;
class QScrollBar;

/// EditorView: a text view that paints itself.
///
/// It does three things only: paint the text held by TextBuffer, remember where
/// the caret is, and translate mouse clicks into byte offsets. File access
/// belongs to Document; nothing here touches a file.
///
/// Two performance rules:
///   * painting a line never pulls out the whole line. It first asks LineMap
///     which bytes the visible window maps to, then slices just those bytes out
///     of TextBuffer, so a one-million-character line stays smooth;
///   * caret and mouse resolution both go through the two level block index of
///     LineMap: no full line scan and no prefix-sum array.
class EditorView : public QAbstractScrollArea
{
    Q_OBJECT

public:
    explicit EditorView(QWidget *parent = nullptr);

    /// Attach a document (not owned; the main window owns it). Drops all caches.
    void setDocument(Document *document);

    /// Insert text at the caret (\r\n and lone \r are folded to \n by the
    /// buffer) and move the caret past it.
    void insertText(const QString &text);

    /// Caret position, both row and column 0 based, column counted in bytes.
    Position cursorPosition() const;

    bool isModified() const { return m_modified; }
    void clearModified() { m_modified = false; }

    /// Reset caret, scrollbars and every cache after the content was replaced.
    void resetView();

signals:
    /// Caret or modified state changed; the main window refreshes its status bar.
    void cursorMoved();

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void focusInEvent(QFocusEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;

    /// Committed IME text arrives here, not through keyPressEvent. Without this
    /// override Chinese input is silently dropped.
    void inputMethodEvent(QInputMethodEvent *event) override;

private slots:
    void blinkCaret();

private:
    /// Text buffer of the document, nullptr when there is none.
    TextBuffer *buffer() const;

    /// Recompute font metrics, line height, gutter width and paint geometry.
    void setupFont();

    /// Clamp caret and anchor back into the document.
    void clampCursor();

    /// Move the caret to the nearest character boundary. Every document changing
    /// operation goes through this: a caret inside a multi-byte UTF-8 character
    /// would split that character and the text would look misplaced.
    void snapCursorToCharBoundary();

    /// Scroll the caret into view. Horizontally it only keeps the caret visible
    /// instead of centring it, so typing does not make the view jump around.
    void ensureCursorVisible();

    /// Recompute both scrollbar ranges / page steps. Visibility is left to Qt's
    /// AsNeeded policy.
    void updateScrollBars();

    /// Index of one line, built on demand. The caller must use the reference
    /// immediately: building another line can rehash the QHash and invalidate it.
    const LineMap &lineMap(quint32 line);

    /// Build one line index from that line's bytes only.
    LineMap buildLineMap(quint32 line);

    /// Pixel width of a line, cached.
    int lineWidth(quint32 line);

    /// Drop the cache of the line containing offset and of every line after it.
    /// Earlier lines keep their number and content, so they are left alone.
    void forgetFrom(quint32 offset);

    /// Byte offset -> line number: expand a window from the hint line instead of
    /// scanning the document.
    quint32 findLine(quint32 offset, quint32 idea);

    /// Cached caret position: line number plus byte column inside that line.
    /// Vertical movement uses it to keep the target column.
    struct CaretSpan
    {
        quint32 line = 0;    ///< line number
        quint32 column = 0;  ///< byte column inside the line
    };

    /// Current caret span, computed with one positionOf().
    CaretSpan caretSpan();

    /// Length of a line in bytes, excluding the newline.
    quint32 lineByteLength(quint32 line);

    /// Put the caret at byte column of line. keepSelection false also collapses
    /// the anchor onto the caret.
    void moveTo(quint32 line, quint32 column, bool keepSelection);

    /// Move the caret to a byte offset.
    void moveToOffset(quint32 offset, bool keepSelection);

    /// Caret moved: restart blinking, scroll into view, repaint.
    void afterMove();

    /// Delete [from, to) and leave the caret at from.
    void deleteRange(quint32 from, quint32 to);

    /// Is offset inside the line [begin, end]?
    static bool isLineOf(quint32 begin, quint32 end, quint32 offset);

    /// First / last byte of a line, 0 when there is no document.
    quint32 lineBeginOf(quint32 line);
    quint32 lineEndOf(quint32 line);

    /// Viewport coordinates -> byte offset.
    quint32 offsetAt(const QPoint &point);

    /// Left/right arrow: one character inside the line, wrapping at the ends.
    /// direction -1 means left.
    void moveCaret(TextBuffer *text, int direction, bool keepSelection);

    /// Ctrl+left/right: jump one word, forward or backward.
    quint32 caretWordOffset(TextBuffer *text, bool forward);

    /// Selected text, empty when nothing is selected (newlines are already
    /// converted to the CRLF the clipboard wants).
    QString selectionText();

    /// Caret x inside a line, before the left text margin is added.
    int caretPixelX(quint32 line, quint32 column);

    /// Step back one character (over the whole UTF-8 sequence).
    static quint32 stepBack(const std::string &text, quint32 offset);

    /// Step forward one character.
    static quint32 stepForward(const std::string &text, quint32 offset);

    /// UTF-16 to UTF-8, hand rolled to avoid depending on QTextCodec.
    static std::string toUtf8(const QString &text);

    /// Clamp v into [low, high].
    static quint32 clampU32(qint64 v, quint32 low, quint32 high);

    Document *m_document = nullptr;

    QFont m_font;

    /// Must be a member, not a local of setupFont(): LineMap::Metrics stores a
    /// pointer to it and dereferences it when measuring CJK glyphs (see
    /// widthOfWideChar). A local QFontMetrics dies at the end of the constructor,
    /// which leaves that pointer dangling: pure ASCII files still work (they use
    /// the advance table), but the first Chinese character crashes deep inside Qt.
    QFontMetrics m_fontMetrics;

    LineMap::Metrics m_metrics;
    int m_lineHeight = 16;
    int m_ascent = 12;
    int m_digitWidth = 8;
    int m_gutter = 48;
    int m_padding = 4;
    int m_caretWidth = 2;
    int m_caretTop = 2;
    int m_caretHeight = 14;

    /// updateScrollBars() is running: showing a scrollbar resizes the viewport,
    /// which would call back into it.
    bool m_reflowGuard = false;

    QColor m_background;
    QColor m_gutterColor;
    QColor m_gutterText;
    QColor m_textColor;
    QColor m_selectionColor;
    QColor m_caretColor;

    quint32 m_cursor = 0;  ///< caret byte offset
    quint32 m_anchor = 0;  ///< selection anchor, equal to the caret means no selection
    bool m_selecting = false;
    bool m_modified = false;

    QTimer m_blinkTimer;
    bool m_blinkOn = true;

    QHash<quint32, LineMap> m_lines;  ///< line number -> index
    QHash<quint32, int> m_widths;     ///< line number -> width, for the scroll range
    quint32 m_lineHint = 0;           ///< last used line, the start point of findLine
};

#endif // EDITORVIEW_H
