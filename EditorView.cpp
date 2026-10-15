#include "EditorView.h"

#include "Document.h"

#include <QClipboard>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QScrollBar>
#include <QWheelEvent>

#include <QtGlobal>

// QInputMethodEvent lives in QtGui on Qt6 and in QtCore on Qt5;
// this include works for either
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include <QInputMethodEvent>
#else
#include <QtCore/QInputMethodEvent>
#endif

#include <algorithm>

namespace
{
/// Half period of the caret blink, same as the Windows default
const int CaretBlinkMs = 530;

/// Gap between the gutter and the text
const int GutterGap = 8;

/// Lines sampled when computing the horizontal scroll range (see updateScrollBars)
const quint32 WidthSampleTarget = 64;
}

quint32 EditorView::clampU32(qint64 v, quint32 low, quint32 high)
{
    if (v < static_cast<qint64>(low)) {
        return low;
    }

    if (v > static_cast<qint64>(high)) {
        return high;
    }

    return static_cast<quint32>(v);
}

std::string EditorView::toUtf8(const QString &text)
{
    // Hand rolled to avoid QTextCodec / QStringConverter: they changed name
    // between Qt5 and Qt6, and UTF-8 has only a handful of rules.
    std::string out;

    out.reserve(static_cast<size_t>(text.size()) * 3 / 2 + 4);

    for (int i = 0; i < text.size(); i++) {
        uint32_t code = text.at(i).unicode();

        // combine a surrogate pair into one code point, a lone half is invalid
        if (code >= 0xD800u && code <= 0xDBFFu) {
            if (i + 1 < text.size()) {
                const uint32_t low = text.at(i + 1).unicode();

                if (low >= 0xDC00u && low <= 0xDFFFu) {
                    code = 0x10000u + ((code - 0xD800u) << 10) + (low - 0xDC00u);
                    i++;
                } else {
                    code = 0xFFFDu;
                }
            } else {
                code = 0xFFFDu;
            }
        } else if (code >= 0xDC00u && code <= 0xDFFFu) {
            code = 0xFFFDu;
        }

        if (code < 0x80u) {
            out.push_back(static_cast<char>(code));
        } else if (code < 0x800u) {
            out.push_back(static_cast<char>(0xC0u | (code >> 6)));
            out.push_back(static_cast<char>(0x80u | (code & 0x3Fu)));
        } else if (code < 0x10000u) {
            out.push_back(static_cast<char>(0xE0u | (code >> 12)));
            out.push_back(static_cast<char>(0x80u | ((code >> 6) & 0x3Fu)));
            out.push_back(static_cast<char>(0x80u | (code & 0x3Fu)));
        } else {
            out.push_back(static_cast<char>(0xF0u | (code >> 18)));
            out.push_back(static_cast<char>(0x80u | ((code >> 12) & 0x3Fu)));
            out.push_back(static_cast<char>(0x80u | ((code >> 6) & 0x3Fu)));
            out.push_back(static_cast<char>(0x80u | (code & 0x3Fu)));
        }
    }

    return out;
}

quint32 EditorView::stepBack(const std::string &text, quint32 offset)
{
    if (offset == 0) {
        return 0;
    }

    quint32 at = offset - 1;

    // eat the continuation bytes (10xxxxxx) back to the leading byte
    while (at > 0 && (static_cast<unsigned char>(text[at]) & 0xC0) == 0x80) {
        at--;
    }

    return at;
}

quint32 EditorView::stepForward(const std::string &text, quint32 offset)
{
    if (offset >= text.size()) {
        return static_cast<quint32>(text.size());
    }

    const unsigned char lead = static_cast<unsigned char>(text[offset]);

    int used = 1;

    if ((lead & 0xE0) == 0xC0) {
        used = 2;
    } else if ((lead & 0xF0) == 0xE0) {
        used = 3;
    } else if ((lead & 0xF8) == 0xF0) {
        used = 4;
    }

    const quint32 next = offset + static_cast<quint32>(used);

    return (next > text.size()) ? static_cast<quint32>(text.size()) : next;
}

/// Build the body font. Consolas only, as required.
/// A function because QFontMetrics has no default constructor: it must be
/// constructed in EditorView's init list from an already configured QFont, so
/// the font cannot be configured inside the constructor body. This also keeps
/// m_font and m_fontMetrics on the same font.
static QFont makeEditorFont()
{
    QFont font(QStringLiteral("Consolas"), 11);

    font.setStyleHint(QFont::Monospace);
    font.setFixedPitch(true);

    return font;
}

EditorView::EditorView(QWidget *parent)
    : QAbstractScrollArea(parent)
    , m_font(makeEditorFont())
    , m_fontMetrics(m_font)
{
    // Visibility is Qt's business: the policy is AsNeeded and Qt decides from
    // range / pageStep. Do NOT switch to AlwaysOff and call setVisible() by hand:
    // under AlwaysOff Qt's own _q_showOrHideScrollBars() keeps hiding the bar and
    // overrides our call, so the scrollbar would never appear.
    setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    setFocusPolicy(Qt::StrongFocus);

    viewport()->setCursor(Qt::IBeamCursor);

    // Tell the input method this view accepts committed text, otherwise a Chinese
    // IME never sends QInputMethodEvent here and CJK input is lost
    viewport()->setAttribute(Qt::WA_InputMethodEnabled, true);

    // the usual editor palette: white page, light gutter, blue selection
    m_background = QColor(0xFF, 0xFF, 0xFF);
    m_gutterColor = QColor(0xF3, 0xF3, 0xF3);
    m_gutterText = QColor(0x99, 0x99, 0x99);
    m_textColor = QColor(0x1E, 0x1E, 0x1E);
    m_selectionColor = QColor(0xCF, 0xE2, 0xFF);
    m_caretColor = QColor(0x00, 0x00, 0x00);

    // the font is ready, this only derives the geometry from it
    setupFont();

    connect(verticalScrollBar(), &QScrollBar::valueChanged, this, [this](int) {
        viewport()->update();
    });

    connect(horizontalScrollBar(), &QScrollBar::valueChanged, this, [this](int) {
        viewport()->update();
    });

    m_blinkTimer.setInterval(CaretBlinkMs);
    connect(&m_blinkTimer, &QTimer::timeout, this, &EditorView::blinkCaret);
}

void EditorView::setupFont()
{
    // The font comes from the init list (see makeEditorFont): m_font and
    // m_fontMetrics must not be reassigned here, QFontMetrics has no default
    // constructor and can only be built in an init list. To change the size,
    // edit the 11 inside makeEditorFont().
    setFont(m_font);

    // ASCII advances are measured once; CJK glyphs are measured when their block
    // is first needed. LineMap keeps a pointer to m_fontMetrics, so this member
    // must outlive every LineMap -- never hand it a local object.
    m_metrics = LineMap::makeMetrics(m_fontMetrics);

    m_ascent = m_fontMetrics.ascent();
    m_lineHeight = m_fontMetrics.height() + 2;
    m_digitWidth = std::max(1, m_fontMetrics.horizontalAdvance(QChar(u'0')));
    m_gutter = m_digitWidth * 6 + GutterGap * 2;

    // a caret slightly shorter than the line reads as a bar, not a column
    m_caretHeight = std::min(m_lineHeight - 2, m_fontMetrics.ascent() + m_fontMetrics.descent());
    m_caretTop = (m_lineHeight - m_caretHeight) / 2;

    // everything measured in pixels depends on the font, so drop the caches
    m_lines.clear();
    m_widths.clear();

    updateScrollBars();
    viewport()->update();
}

TextBuffer *EditorView::buffer() const
{
    return m_document != nullptr ? m_document->buffer() : nullptr;
}

void EditorView::setDocument(Document *document)
{
    m_document = document;

    resetView();
}

void EditorView::resetView()
{
    m_lines.clear();
    m_widths.clear();

    m_cursor = 0;
    m_anchor = 0;
    m_selecting = false;
    m_modified = false;
    m_lineHint = 0;

    verticalScrollBar()->setValue(0);
    horizontalScrollBar()->setValue(0);
    updateScrollBars();
    m_blinkOn = true;
    m_blinkTimer.start();
    viewport()->update();
    emit cursorMoved();
}

void EditorView::clampCursor()
{
    TextBuffer *text = buffer();

    if (text == nullptr) {
        m_cursor = 0;
        m_anchor = 0;

        return;
    }

    m_cursor = clampU32(m_cursor, 0, text->byteCount());
    m_anchor = clampU32(m_anchor, 0, text->byteCount());
}

Position EditorView::cursorPosition() const
{
    TextBuffer *text = buffer();

    if (text == nullptr) {
        return Position();
    }

    return text->positionOf(m_cursor);
}

void EditorView::snapCursorToCharBoundary()
{
    TextBuffer *text = buffer();

    if (text == nullptr) {
        return;
    }

    m_cursor = clampU32(m_cursor, 0, text->byteCount());

    if (m_cursor == 0) {
        return;
    }

    // one byte to the left is enough: a continuation byte means the caret sits
    // inside a character, so step back to that character's leading byte
    const std::string left = text->range(m_cursor - 1, m_cursor);

    if (!left.empty() && (static_cast<unsigned char>(left[0]) & 0xC0) == 0x80) {
        m_cursor = stepBack(text->range(0, m_cursor), m_cursor);
    }
}

void EditorView::inputMethodEvent(QInputMethodEvent *event)
{
    // A Chinese IME: the preedit (uncommitted pinyin) is not painted here, the
    // committed text goes straight in. Only commitString is handled; the preedit
    // is dropped, so candidates are invisible but CJK text does reach the buffer.
    if (!event->commitString().isEmpty()) {
        insertText(event->commitString());

        event->accept();

        return;
    }

    // preedit-only update: nothing to insert, but accept it instead of passing
    // an unhandled event up the chain
    event->accept();
}

void EditorView::insertText(const QString &text)
{
    m_blinkOn = true;
    m_blinkTimer.start();

    const std::string bytes = toUtf8(text);

    Document *document = m_document;

    if (document == nullptr || bytes.empty()) {
        return;
    }

    TextBuffer *text_ = document->buffer();

    // the caret must sit on a character boundary first, or the insert point lands
    // inside a CJK character
    snapCursorToCharBoundary();

    // a selection is replaced by the new text: the usual "typing overwrites the
    // selection" behaviour
    if (m_anchor != m_cursor) {
        const quint32 from = std::min(m_anchor, m_cursor);
        const quint32 to = std::max(m_anchor, m_cursor);

        text_->replace(from, to, bytes);
        m_cursor = from;
    } else {
        // move the caret by what the buffer really inserted. bytes.size() is not
        // reliable: CRLF is normalised to LF and shrinks (defensive here, the GUI
        // inserts one character at a time)
        const quint32 added = text_->insert(m_cursor, bytes);

        m_cursor += (added > 0) ? added : static_cast<quint32>(bytes.size());
    }

    m_anchor = m_cursor;
    m_selecting = false;
    m_modified = true;

    // the index and width of this line are stale now. Do not clear the whole
    // cache: lines before it keep their number and content
    forgetFrom(m_cursor);
    updateScrollBars();
    ensureCursorVisible();

    viewport()->update();

    emit cursorMoved();
}

void EditorView::forgetFrom(quint32 offset)
{
    TextBuffer *text = buffer();

    if (text == nullptr) {
        return;
    }

    // the edited line: its index is invalid (its length may have changed)
    const quint32 line = findLine(offset, m_lineHint);

    m_lines.remove(line);
    m_widths.remove(line);

    // Later lines: their content may be unchanged but their numbers shifted, so
    // drop them all and rebuild on demand. The prefix 0..line-1 is kept, so
    // editing one line in a million line document costs nothing for the rest.
    for (QHash<quint32, LineMap>::iterator it = m_lines.begin(); it != m_lines.end();) {
        if (it.key() > line) {
            it = m_lines.erase(it);
        } else {
            ++it;
        }
    }

    for (QHash<quint32, int>::iterator it = m_widths.begin(); it != m_widths.end();) {
        if (it.key() > line) {
            it = m_widths.erase(it);
        } else {
            ++it;
        }
    }

    m_lineHint = line;
}

// ============================== lookup and caches ==============================

const LineMap &EditorView::lineMap(quint32 line)
{
    // Reuse the cached index. The caller must consume the reference immediately:
    // inserting another line can rehash the QHash and invalidate it.
    QHash<quint32, LineMap>::iterator it = m_lines.find(line);

    if (it != m_lines.end()) {
        return it.value();
    }

    const LineMap map = buildLineMap(line);

    it = m_lines.insert(line, map);

    return it.value();
}

LineMap EditorView::buildLineMap(quint32 line)
{
    TextBuffer *text = buffer();

    if (text == nullptr) {
        return LineMap::build(0, std::string(), m_metrics);
    }

    // only this line's bytes; the block split happens inside LineMap
    return LineMap::build(text->lineBegin(line), text->lineText(line), m_metrics);
}

int EditorView::lineWidth(quint32 line)
{
    const QHash<quint32, int>::const_iterator it = m_widths.constFind(line);

    if (it != m_widths.constEnd()) {
        return it.value();
    }

    // width() measures the whole line, so the result must be cached
    const int width = lineMap(line).width();

    m_widths.insert(line, width);

    return width;
}

quint32 EditorView::findLine(quint32 offset, quint32 idea)
{
    TextBuffer *text = buffer();

    if (text == nullptr) {
        return 0;
    }

    const quint32 total = text->lineCount();

    offset = clampU32(offset, 0, text->byteCount());
    idea = std::min(idea, total - 1);

    // the hint line is right in the common case (the caret did not leave the line)
    if (isLineOf(lineBeginOf(idea), lineEndOf(idea), offset)) {
        return idea;
    }

    // otherwise grow a window from one line outwards until it brackets offset.
    // moving a few lines touches a few lines, crossing the file is O(log lines)
    quint32 low = idea;
    quint32 high = idea;
    quint32 span = 1;

    while (true) {
        bool grown = false;

        if (low > 0) {
            low = (low > span) ? (low - span) : 0;
            grown = true;

            if (text->lineBegin(low) <= offset && offset <= text->lineEnd(low)) {
                break;
            }
        }

        if (high + 1 < total) {
            high = std::min(total - 1, high + span);
            grown = true;

            if (text->lineBegin(high) <= offset && offset <= text->lineEnd(high)) {
                break;
            }
        }

        if (!grown) {
            // both ends reached without a hit: offset must be in the last line
            return total - 1;
        }

        span *= 2;
    }

    // the window is small (at most span lines), a linear scan is fine
    for (quint32 line = low; line <= high; line++) {
        if (offset <= text->lineEnd(line)) {
            return line;
        }
    }

    return high;
}

bool EditorView::isLineOf(quint32 begin, quint32 end, quint32 offset)
{
    return offset >= begin && offset <= end;
}

quint32 EditorView::lineBeginOf(quint32 line)
{
    TextBuffer *text = buffer();

    return text != nullptr ? text->lineBegin(line) : 0;
}

quint32 EditorView::lineEndOf(quint32 line)
{
    TextBuffer *text = buffer();

    return text != nullptr ? text->lineEnd(line) : 0;
}

EditorView::CaretSpan EditorView::caretSpan()
{
    TextBuffer *text = buffer();

    CaretSpan span;

    if (text == nullptr) {
        return span;
    }

    // findLine starts from m_lineHint, so inside one line this is near constant time
    span.line = findLine(m_cursor, m_lineHint);

    m_lineHint = span.line;

    span.column = m_cursor - text->lineBegin(span.line);

    return span;
}

quint32 EditorView::lineByteLength(quint32 line)
{
    return lineMap(line).byteLength();
}

void EditorView::moveTo(quint32 line, quint32 column, bool keepSelection)
{
    TextBuffer *text = buffer();

    if (text == nullptr) {
        return;
    }

    const quint32 begin = text->lineBegin(line);

    m_cursor = clampU32(static_cast<qint64>(begin) + column, begin, text->lineEnd(line));

    if (!keepSelection) {
        m_anchor = m_cursor;
    }
}

void EditorView::moveToOffset(quint32 offset, bool keepSelection)
{
    TextBuffer *text = buffer();

    if (text == nullptr) {
        return;
    }

    m_cursor = clampU32(offset, 0, text->byteCount());

    if (!keepSelection) {
        m_anchor = m_cursor;
    }
}

void EditorView::afterMove()
{
    m_blinkOn = true;
    m_blinkTimer.start();

    ensureCursorVisible();
    viewport()->update();

    emit cursorMoved();
}

// ============================== keyboard ==============================

void EditorView::keyPressEvent(QKeyEvent *event)
{
    TextBuffer *text = buffer();

    if (text == nullptr) {
        QAbstractScrollArea::keyPressEvent(event);

        return;
    }

    const int key = event->key();
    const bool keep = (event->modifiers() & Qt::ShiftModifier) != 0;
    const bool control = (event->modifiers() & Qt::ControlModifier) != 0;

    switch (key) {
    case Qt::Key_Left:
        if (control) {
            moveToOffset(caretWordOffset(text, false), keep);
        } else {
            moveCaret(text, -1, keep);
        }

        afterMove();

        return;

    case Qt::Key_Right:
        if (control) {
            moveToOffset(caretWordOffset(text, true), keep);
        } else {
            moveCaret(text, 1, keep);
        }

        afterMove();

        return;

    case Qt::Key_Up:
    case Qt::Key_Down: {
        const int step = (key == Qt::Key_Up) ? -1 : 1;
        const CaretSpan span = caretSpan();

        // target column = min(old column, byte length of the target line).
        // The column is derived from the caret every time, so a short line cannot eat it.
        const quint32 line = clampU32(static_cast<qint64>(span.line) + step, 0, text->lineCount() - 1);
        const quint32 column = std::min(span.column, lineByteLength(line));

        moveTo(line, column, keep);
        afterMove();

        return;
    }

    case Qt::Key_PageUp:
    case Qt::Key_PageDown: {
        // one screen up/down, keeping the column like the arrow keys
        const int rows = std::max(1, viewport()->height() / m_lineHeight);
        const int step = (key == Qt::Key_PageUp) ? -rows : rows;
        const CaretSpan span = caretSpan();

        int top = verticalScrollBar()->value() + step * m_lineHeight;

        top = std::max(0, std::min(top, verticalScrollBar()->maximum()));

        verticalScrollBar()->setValue(top);

        const quint32 line = clampU32(static_cast<qint64>(span.line) + step, 0, text->lineCount() - 1);
        const quint32 column = std::min(span.column, lineByteLength(line));

        moveTo(line, column, keep);
        afterMove();

        return;
    }

    case Qt::Key_Backspace: {
        m_blinkOn = true;
        m_blinkTimer.start();

        if (m_anchor != m_cursor) {
            deleteRange(std::min(m_anchor, m_cursor), std::max(m_anchor, m_cursor));

            return;
        }

        if (m_cursor == 0) {
            return;
        }

        const quint32 line = findLine(m_cursor, m_lineHint);
        const quint32 begin = text->lineBegin(line);

        // backspace at the line start eats the newline and joins two lines;
        // inside a line it removes exactly one character (a whole UTF-8 sequence)
        const quint32 previous = (m_cursor > begin) ? stepBack(text->range(begin, m_cursor), m_cursor - begin) + begin
                                                    : m_cursor - 1;

        deleteRange(previous, m_cursor);

        return;
    }

    case Qt::Key_Delete: {
        m_blinkOn = true;
        m_blinkTimer.start();

        if (m_anchor != m_cursor) {
            deleteRange(std::min(m_anchor, m_cursor), std::max(m_anchor, m_cursor));

            return;
        }

        if (m_cursor >= text->byteCount()) {
            return;
        }

        const quint32 line = findLine(m_cursor, m_lineHint);
        const quint32 end = text->lineEnd(line);

        const quint32 next = (m_cursor < end) ? stepForward(text->range(m_cursor, end), 0) + m_cursor : m_cursor + 1;

        deleteRange(m_cursor, next);

        return;
    }

    case Qt::Key_Home: {
        const quint32 line = control ? 0 : caretSpan().line;

        moveTo(line, 0, keep);
        afterMove();

        return;
    }

    case Qt::Key_End: {
        const quint32 line = control ? (text->lineCount() - 1) : caretSpan().line;

        moveTo(line, lineByteLength(line), keep);
        afterMove();

        return;
    }

    case Qt::Key_Return:
    case Qt::Key_Enter:
        insertText(QStringLiteral("\n"));

        return;

    case Qt::Key_Tab:
        insertText(QStringLiteral("\t"));

        return;

    default:
        break;
    }

    // ---- plain text input ----
    // Only text() matters here: a Ctrl chord carries a control character and is
    // ignored. The one exception is Ctrl+Alt (AltGr on Windows), which some
    // keyboard layouts need in order to type characters.
    const bool textInput = !event->text().isEmpty()
                           && (!control || (event->modifiers() & Qt::AltModifier) != 0)
                           && event->text().at(0).unicode() >= 0x20;

    if (textInput) {
        insertText(event->text());

        return;
    }

    QAbstractScrollArea::keyPressEvent(event);
}

void EditorView::moveCaret(TextBuffer *text, int direction, bool keepSelection)
{
    const CaretSpan span = caretSpan();

    if (direction < 0) {
        if (span.column > 0) {
            // one character left inside the line, over the whole UTF-8 sequence
            const quint32 begin = text->lineBegin(span.line);
            const std::string part = text->range(begin, m_cursor);

            moveTo(span.line, stepBack(part, span.column), keepSelection);
        } else if (span.line > 0) {
            // at the line start: jump to the end of the previous line
            const quint32 previous = span.line - 1;

            moveTo(previous, lineByteLength(previous), keepSelection);
        } else {
            moveTo(0, 0, keepSelection);
        }

        return;
    }

    const quint32 length = lineByteLength(span.line);

    if (span.column < length) {
        // one character right inside the line
        const quint32 begin = text->lineBegin(span.line);
        const std::string part = text->range(m_cursor, begin + length + 1);

        moveTo(span.line, stepForward(part, 0), keepSelection);

        return;
    }

    if (span.line + 1 < text->lineCount()) {
        // at the line end: jump to the start of the next line
        moveTo(span.line + 1, 0, keepSelection);
    }
}

quint32 EditorView::caretWordOffset(TextBuffer *text, bool forward)
{
    // Word jump: characters are blank / word / punctuation, and a run of one
    // class is one word. Only the text around the caret is examined.
    const auto classify = [](unsigned char c) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            return 0;
        }

        if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_') {
            return 1;
        }

        return 2;
    };

    const quint32 total = text->byteCount();

    if (forward) {
        if (m_cursor >= total) {
            return total;
        }

        const quint32 end = clampU32(static_cast<qint64>(m_cursor) + 256, 0, total);
        const std::string part = text->range(m_cursor, end);
        const unsigned char kind = static_cast<unsigned char>(classify(static_cast<unsigned char>(part[0])));

        quint32 at = 0;

        while (at < part.size() && static_cast<unsigned char>(classify(static_cast<unsigned char>(part[at]))) == kind) {
            at++;
        }

        return m_cursor + at;
    }

    if (m_cursor == 0) {
        return 0;
    }

    const quint32 begin = (m_cursor > 256) ? (m_cursor - 256) : 0;
    const std::string part = text->range(begin, m_cursor);

    quint32 at = static_cast<quint32>(part.size());

    if (at == 0) {
        return m_cursor;
    }

    // step over the continuation bytes left of the caret to land on a boundary
    const unsigned char kind = static_cast<unsigned char>(classify(static_cast<unsigned char>(part[at - 1])));

    while (at > 0 && static_cast<unsigned char>(classify(static_cast<unsigned char>(part[at - 1]))) == kind) {
        at--;
    }

    return begin + at;
}

void EditorView::deleteRange(quint32 from, quint32 to)
{
    Document *document = m_document;

    if (document == nullptr) {
        return;
    }

    TextBuffer *text = document->buffer();

    from = clampU32(from, 0, text->byteCount());
    to = clampU32(to, 0, text->byteCount());

    if (to <= from) {
        return;
    }

    text->erase(from, to);

    m_cursor = from;
    m_anchor = from;
    m_selecting = false;
    m_modified = true;

    // line numbers after the edit may shift, so drop the cache from this line on
    forgetFrom(from);
    updateScrollBars();
    afterMove();
}

QString EditorView::selectionText()
{
    TextBuffer *text = buffer();

    if (text == nullptr || m_anchor == m_cursor) {
        return QString();
    }

    const quint32 from = std::min(m_anchor, m_cursor);
    const quint32 to = std::max(m_anchor, m_cursor);

    // the buffer holds only \n, the clipboard wants CRLF
    QString out = LineMap::decode(text->range(from, to));

    out.replace(QStringLiteral("\n"), QStringLiteral("\r\n"));

    return out;
}

int EditorView::caretPixelX(quint32 line, quint32 column)
{
    // The caret sits on the left edge of a character: one division into the block
    // plus one lookup. Millions of characters per line are fine, only the blocks
    // on screen were ever measured.
    return lineMap(line).xAtColumn(column);
}

void EditorView::ensureCursorVisible()
{
    TextBuffer *text = buffer();

    if (text == nullptr) {
        return;
    }

    const int viewHeight = viewport()->height();
    const int viewWidth = viewport()->width();
    const int textArea = std::max(1, viewWidth - m_gutter);

    const quint32 line = findLine(m_cursor, m_lineHint);

    m_lineHint = line;

    const Position position = text->positionOf(m_cursor);

    const int x = caretPixelX(line, position.column);

    // vertical: keep one line of slack above and below
    QScrollBar *vertical = verticalScrollBar();

    const int lineTop = static_cast<int>(line) * m_lineHeight;
    const int top = vertical->value();

    if (lineTop - m_lineHeight < top) {
        vertical->setValue(std::max(0, lineTop - m_lineHeight));
    } else if (lineTop + m_lineHeight * 2 > top + viewHeight) {
        vertical->setValue(std::max(0, lineTop + m_lineHeight * 2 - viewHeight));
    }

    // horizontal: keep the caret merely visible, do not centre it, so typing does
    // not make the view jump
    QScrollBar *horizontal = horizontalScrollBar();

    const int left = horizontal->value();
    const int margin = m_metrics.codeWidth * 2;

    if (x - margin < left) {
        horizontal->setValue(std::max(0, x - margin));
    } else if (x + margin > left + textArea) {
        horizontal->setValue(std::max(0, x + margin - textArea));
    }
}

// ============================== scrolling ==============================

void EditorView::updateScrollBars()
{
    // Changing range / pageStep can make Qt show a scrollbar, which resizes the
    // viewport and calls back into resizeEvent -> here. The guard stops the loop.
    if (m_reflowGuard) {
        return;
    }

    m_reflowGuard = true;

    TextBuffer *text = buffer();

    QScrollBar *vertical = verticalScrollBar();
    QScrollBar *horizontal = horizontalScrollBar();

    if (text == nullptr) {
        vertical->setRange(0, 0);
        horizontal->setRange(0, 0);

        m_reflowGuard = false;

        return;
    }

    const int viewWidth = viewport()->width();
    const int viewHeight = viewport()->height();

    // ---- vertical ----
    const qint64 documentHeight = static_cast<qint64>(text->lineCount()) * m_lineHeight;
    const int lineCount = static_cast<int>(text->lineCount());

    // the gutter must fit the widest line number, so it tracks the line count
    const int digits = QString::number(std::max(1, lineCount)).size();

    m_gutter = std::max(m_digitWidth * 3, m_digitWidth * digits + GutterGap * 2);

    // visibility is Qt's business: AsNeeded decides from range / pageStep
    vertical->setRange(0, static_cast<int>(std::max<qint64>(0, documentHeight - viewHeight)));
    vertical->setPageStep(std::max(1, viewHeight));
    vertical->setSingleStep(m_lineHeight);

    // ---- horizontal ----
    // There is no global widest-line statistic: keeping one would either modify
    // TextBuffer or recompute on every edit. So we sample: every step-th line plus
    // the last one, and the caret line becomes the widest one that scrolls.
    // The cost, stated honestly: sampling a very long line makes lineWidth()
    // measure it whole, tens of milliseconds for a million characters.
    const quint32 lines = text->lineCount();
    int widest = 0;

    if (lines > 0) {
        const quint32 step = std::max<quint32>(1, lines / WidthSampleTarget);

        for (quint32 line = 0; line < lines; line += step) {
            widest = std::max(widest, lineWidth(line));
        }

        widest = std::max(widest, lineWidth(lines - 1));

        // Always include the caret line: the user is looking at it, and if it is
        // wider than every sampled line, dragging a selection would scroll past its end.
        const quint32 caretLine = findLine(m_cursor, m_lineHint);

        m_lineHint = caretLine;

        widest = std::max(widest, lineWidth(caretLine));
    }

    const qint64 contentWidth = static_cast<qint64>(widest) + m_padding * 2;
    const int textArea = std::max(1, viewWidth - m_gutter);

    horizontal->setRange(0, static_cast<int>(std::max<qint64>(0, contentWidth - textArea)));
    horizontal->setPageStep(textArea);
    horizontal->setSingleStep(std::max(1, m_metrics.codeWidth * 2));

    if (documentHeight <= viewHeight) {
        vertical->setValue(0);
    }

    if (contentWidth <= textArea) {
        horizontal->setValue(0);
    }

    m_reflowGuard = false;
}

// ============================== events ==============================

void EditorView::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);

    QPainter painter(viewport());

    const QRect area = viewport()->rect();

    painter.fillRect(area, m_background);
    painter.fillRect(QRect(0, 0, m_gutter, area.height()), m_gutterColor);

    TextBuffer *text = buffer();

    if (text == nullptr) {
        return;
    }

    painter.setFont(m_font);

    const QScrollBar *vertical = verticalScrollBar();
    const QScrollBar *horizontal = horizontalScrollBar();

    const int scrollY = vertical->value();
    const int scrollX = horizontal->value();

    const quint32 total = text->lineCount();
    const int firstLine = std::max(0, scrollY / m_lineHeight);

    // a visible horizontal scrollbar eats a strip at the bottom of the viewport
    const int usableHeight = area.height() - (horizontal->isVisible() ? horizontal->height() : 0);
    const int visibleLines = std::max(1, usableHeight / m_lineHeight + 2);

    const int textLeft = m_gutter + m_padding;
    const int numberRight = m_gutter - GutterGap;

    // width of the text area, from textLeft to the right edge of the viewport.
    // Used to clip the text and to pick the visible slice, so it matches the
    // horizontal scroll range computation.
    const int textWidth = std::max(1, area.width() - textLeft);

    const bool hasSelection = (m_anchor != m_cursor);
    const quint32 selectionFrom = std::min(m_anchor, m_cursor);
    const quint32 selectionTo = std::max(m_anchor, m_cursor);

    // ask for the caret position once and compare lines inside the loop
    const Position caret = text->positionOf(m_cursor);

    const bool caretVisible = m_blinkOn && hasFocus();

    for (int offset = 0; offset < visibleLines; offset++) {
        const int number = firstLine + offset;

        if (number < 0 || static_cast<quint32>(number) >= total) {
            break;
        }

        const quint32 line = static_cast<quint32>(number);
        const int y = number * m_lineHeight - scrollY;

        // ---- line number ----
        painter.setPen(m_gutterText);
        painter.drawText(QRect(0, y, numberRight, m_lineHeight), Qt::AlignRight | Qt::AlignVCenter,
                         QString::number(number + 1));

        const quint32 lineBegin = text->lineBegin(line);
        const quint32 lineEnd = text->lineEnd(line);

        // exactly one lineMap() call per iteration, so this reference stays valid
        // until the end of the round
        const LineMap &map = lineMap(line);

        // ---- selection ----
        // A selection can span millions of characters, but only the on-screen part
        // is painted: x1 / x2 are the ends of the whole selection and max / min do
        // the clipping, so no multi-million pixel rectangle is ever built.
        if (hasSelection && lineBegin <= selectionTo && lineEnd >= selectionFrom) {
            const quint32 from = std::max(lineBegin, selectionFrom);
            const quint32 to = std::min(lineEnd, selectionTo) + 1;

            // both operands are in-line byte columns, so this cannot underflow
            const quint32 fromColumn = std::max(from, lineBegin) - lineBegin;
            const quint32 toColumn = std::max(to, lineBegin) - lineBegin;

            const int x1 = textLeft + map.xAtColumn(fromColumn) - scrollX;
            const int x2 = textLeft + map.xAtColumn(toColumn) - scrollX;

            const int left = std::max(area.left(), x1);
            const int right = std::min(area.right(), x2);

            if (right > left) {
                painter.fillRect(QRect(left, y, right - left, m_lineHeight), m_selectionColor);
            }
        }

        // ---- text ----
        // Character range actually on screen: both ends come from an exact
        // pixel -> character query. Using entryAtByte(scrollX - textLeft) as the
        // start was wrong: an unmeasured block can only report its head (4096
        // bytes in a long line), which pushed the start far off the left edge and
        // shifted the whole line, most visibly while dragging a selection.
        int firstUnit = 0;
        int lastUnit = 0;

        map.windowForX(scrollX - textLeft, scrollX + textWidth - textLeft, firstUnit, lastUnit);

        const quint32 firstByte = map.byteAtEntry(firstUnit);
        const quint32 lastByte = map.byteAtEntry(lastUnit);

        if (lastUnit > firstUnit && lastByte > firstByte) {
            const int beginChar = map.charIndexAtEntry(firstUnit);
            const int endChar = map.charIndexAtEntry(lastUnit);

            // the key step: slice only this small range out of TextBuffer, never the
            // whole line. A million character line copies a few dozen bytes here.
            const std::string piece = text->range(lineBegin + firstByte, lineBegin + lastByte);

            const QString shown = LineMap::decode(piece).left(endChar - beginChar);

            const int x = textLeft + map.xAtColumn(firstByte) - scrollX;

            painter.setPen(m_textColor);

            // clip to the text area: the slice may start left of the viewport and
            // would otherwise spill over the gutter and past the right edge
            painter.save();
            painter.setClipRect(QRect(textLeft, y, textWidth, m_lineHeight));
            painter.drawText(x, y + m_ascent, shown);
            painter.restore();
        }

        // ---- caret ----
        // caretPixelX is LineMap::xAtColumn: one division into the block, one lookup
        if (caretVisible && caret.line == line) {
            const int caretX = textLeft + caretPixelX(line, caret.column) - scrollX;

            if (caretX >= area.left() - 2 && caretX <= area.right()) {
                painter.fillRect(QRect(caretX, y + m_caretTop, m_caretWidth, m_caretHeight), m_caretColor);
            }
        }
    }

    // paint the corner where the scrollbars meet the gutter
    if (horizontal->isVisible() && vertical->isVisible()) {
        painter.fillRect(QRect(0, area.height() - horizontal->height(), m_gutter, horizontal->height()),
                         m_gutterColor);
    }
}

void EditorView::resizeEvent(QResizeEvent *event)
{
    QAbstractScrollArea::resizeEvent(event);

    updateScrollBars();
    ensureCursorVisible();
}

void EditorView::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) {
        QAbstractScrollArea::mousePressEvent(event);

        return;
    }

    setFocus(Qt::MouseFocusReason);

    m_blinkOn = true;
    m_blinkTimer.start();

    const quint32 offset = offsetAt(event->position().toPoint());

    const bool extend = (event->modifiers() & Qt::ShiftModifier) != 0;

    if (!extend) {
        // drop the anchor here; a real drag decides whether a selection survives
        m_anchor = offset;
    }

    m_cursor = offset;
    m_selecting = true;

    viewport()->update();

    emit cursorMoved();
}

void EditorView::mouseMoveEvent(QMouseEvent *event)
{
    if (!m_selecting) {
        QAbstractScrollArea::mouseMoveEvent(event);

        return;
    }

    const quint32 offset = offsetAt(event->position().toPoint());

    if (offset == m_cursor) {
        // same position, nothing to repaint
        return;
    }

    m_cursor = offset;

    // auto-scroll while the drag leaves the viewport
    const QPoint at = event->position().toPoint();
    const QRect area = viewport()->rect();

    if (at.y() < 0) {
        verticalScrollBar()->setValue(std::max(0, verticalScrollBar()->value() - m_lineHeight));
    } else if (at.y() > area.height()) {
        verticalScrollBar()->setValue(verticalScrollBar()->value() + m_lineHeight);
    }

    if (at.x() < 0) {
        horizontalScrollBar()->setValue(std::max(0, horizontalScrollBar()->value() - m_metrics.codeWidth * 2));
    } else if (at.x() > area.width()) {
        horizontalScrollBar()->setValue(horizontalScrollBar()->value() + m_metrics.codeWidth * 2);
    }

    viewport()->update();

    emit cursorMoved();
}

void EditorView::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) {
        QAbstractScrollArea::mouseReleaseEvent(event);

        return;
    }

    m_selecting = false;

    // a click without a drag just puts the caret here
    if (m_anchor == m_cursor) {
        m_anchor = m_cursor;
    }

    viewport()->update();
}

void EditorView::mouseDoubleClickEvent(QMouseEvent *event)
{
    // Double click selects the run of same-class characters under the cursor:
    // alphanumerics and _ form one class, everything else its own. Only the
    // current line is pulled out of TextBuffer.
    TextBuffer *text = buffer();

    if (text == nullptr) {
        return;
    }

    const quint32 offset = offsetAt(event->position().toPoint());

    m_cursor = offset;

    const quint32 line = findLine(offset, m_lineHint);
    const quint32 lineBegin = text->lineBegin(line);
    const quint32 lineEnd = text->lineEnd(line);

    const std::string content = text->range(lineBegin, lineEnd);

    const quint32 column = offset - lineBegin;

    const auto classify = [](unsigned char c) {
        if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_') {
            return 1;
        }

        if (c >= 0x80) {
            return 2;
        }

        return 0;
    };

    if (column < content.size()) {
        const unsigned char kind = static_cast<unsigned char>(classify(static_cast<unsigned char>(content[column])));

        quint32 from = column;
        quint32 to = column + 1;

        while (from > 0 && classify(static_cast<unsigned char>(content[from - 1])) == kind) {
            from--;
        }

        while (to < content.size() && classify(static_cast<unsigned char>(content[to])) == kind) {
            to++;
        }

        m_anchor = lineBegin + from;
        m_cursor = lineBegin + to;
    } else {
        m_anchor = offset;
    }

    m_blinkOn = true;
    m_blinkTimer.start();

    viewport()->update();

    emit cursorMoved();
}

void EditorView::wheelEvent(QWheelEvent *event)
{
    // Ctrl + wheel is left for zoom; here it just scrolls by lines
    const int steps = event->angleDelta().y() / 120;

    if (steps != 0) {
        verticalScrollBar()->setValue(verticalScrollBar()->value() - steps * m_lineHeight * 3);
    }

    event->accept();
}

void EditorView::focusInEvent(QFocusEvent *event)
{
    QAbstractScrollArea::focusInEvent(event);

    m_blinkOn = true;
    m_blinkTimer.start();

    viewport()->update();
}

void EditorView::focusOutEvent(QFocusEvent *event)
{
    QAbstractScrollArea::focusOutEvent(event);

    m_blinkTimer.stop();
    m_blinkOn = false;

    viewport()->update();
}

void EditorView::blinkCaret()
{
    m_blinkOn = !m_blinkOn;

    viewport()->update();
}

quint32 EditorView::offsetAt(const QPoint &point)
{
    TextBuffer *text = buffer();

    if (text == nullptr) {
        return 0;
    }

    const QScrollBar *vertical = verticalScrollBar();
    const QScrollBar *horizontal = horizontalScrollBar();

    // screen coordinates -> document coordinates
    const int documentY = point.y() + vertical->value();
    const int documentX = point.x() + horizontal->value() - m_gutter - m_padding;

    const qint64 rawLine = documentY / m_lineHeight;

    const quint32 line = clampU32(rawLine, 0, text->lineCount() - 1);

    // Hand x to this line index: it binary searches the block table and then walks
    // a few characters, so a million character line only measures the block on screen.
    const quint32 column = lineMap(line).columnAtX(documentX);

    m_lineHint = line;

    return clampU32(static_cast<qint64>(text->lineBegin(line)) + column, 0, text->byteCount());
}
