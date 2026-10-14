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

#include <algorithm>

namespace
{
/// 光标闪烁的半周期，和 Windows 自带的一致
const int CaretBlinkMs = 530;

/// 行号栏和正文之间空一点
const int GutterGap = 8;

/// 算横向滚动范围时最多采多少行。见 updateScrollBars() 里的说明
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
    // 自己编码，不依赖 QTextCodec / QStringConverter：那两个在 Qt5 和 Qt6 之间
    // 换了名字，而 UTF-8 的规则一共就那几条
    std::string out;

    out.reserve(static_cast<size_t>(text.size()) * 3 / 2 + 4);

    for (int i = 0; i < text.size(); i++) {
        uint32_t code = text.at(i).unicode();

        // 高低代理项凑成一个码位，缺一半就当替换字符
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

    // 往回吃掉续字节（10xxxxxx），一直退到这个字符的起始字节
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

EditorView::EditorView(QWidget *parent)
    : QAbstractScrollArea(parent)
{
    // 滚动条我们自己在 updateScrollBars() 里管，Qt 默认会按 sizeHint 乱冒
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setFocusPolicy(Qt::StrongFocus);

    viewport()->setCursor(Qt::IBeamCursor);

    // 配色：白底黑字，行号栏浅灰，选中蓝，和常见编辑器一致
    m_background = QColor(0xFF, 0xFF, 0xFF);
    m_gutterColor = QColor(0xF3, 0xF3, 0xF3);
    m_gutterText = QColor(0x99, 0x99, 0x99);
    m_textColor = QColor(0x1E, 0x1E, 0x1E);
    m_selectionColor = QColor(0xCF, 0xE2, 0xFF);
    m_caretColor = QColor(0x00, 0x00, 0x00);

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
    // 题目的硬性要求：只用 Windows 自带的 Consolas。
    // 这里的 styleHint / fixedPitch 只是让字体匹配更稳，不是跨平台逻辑
    m_font = QFont(QStringLiteral("Consolas"), 11);
    m_font.setStyleHint(QFont::Monospace);
    m_font.setFixedPitch(true);

    setFont(m_font);

    const QFontMetrics fm(m_font);

    // ASCII 的推进量一次测好；汉字之类的等真正量到那一块时再问字体
    m_metrics = LineMap::makeMetrics(fm);

    m_ascent = fm.ascent();
    m_lineHeight = fm.height() + 2;
    m_digitWidth = std::max(1, fm.horizontalAdvance(QChar(u'0')));
    m_gutter = m_digitWidth * 6 + GutterGap * 2;

    // 光标比一整行矮一点，上下各留一个像素，看着像个竖线而不是一根柱子
    m_caretHeight = std::min(m_lineHeight - 2, fm.ascent() + fm.descent());
    m_caretTop = (m_lineHeight - m_caretHeight) / 2;

    // 字号一变，所有按像素算出来的东西都得重来
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

    // 有选中就用新内容替掉，这正是所有编辑器里"打字覆盖选中"的行为
    if (m_anchor != m_cursor) {
        const quint32 from = std::min(m_anchor, m_cursor);
        const quint32 to = std::max(m_anchor, m_cursor);

        text_->replace(from, to, bytes);
        m_cursor = from;
    } else {
        text_->insert(m_cursor, bytes);
        m_cursor += static_cast<quint32>(bytes.size());
    }

    m_anchor = m_cursor;
    m_selecting = false;
    m_modified = true;

    // 内容变了，这一行的定位表（块索引）和宽度都不能再信。
    // 注意：不能整体清空缓存 —— 行号没变的那些行内容也没变，清掉纯属浪费
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

    // 被改动的那一行：它自己的定位表必须作废（长度可能都变了）
    const quint32 line = findLine(offset, m_lineHint);

    m_lines.remove(line);
    m_widths.remove(line);

    // 后面的行：即使内容没变，行号也可能整体错位了，统统丢掉让它们按需重建。
    // 前缀部分（0 .. line-1）保持不动，所以在一百万行的长文档里编辑一行，
    // 不会被无关的行拖累
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

// ============================== 定位与缓存 ==============================

const LineMap &EditorView::lineMap(quint32 line)
{
    // QHash 里已经有了就直接用。注意：调用方必须"拿到引用立刻用完"，
    // 因为下面可能插入新的行，那会让 QHash 重新散列
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

    // 只取这一行的字节；行内的块划分都在 LineMap 里完成
    return LineMap::build(text->lineBegin(line), text->lineText(line), m_metrics);
}

void EditorView::setScrollBarVisible(QScrollBar *bar, bool visible)
{
    // 只在真的要变的时候才动它：setVisible 会让 QAbstractScrollArea 重算视口边距，
    // 而视口一变又会回到这里，来回设同一个值就可能绕成死循环
    if (bar->isVisible() != visible) {
        bar->setVisible(visible);
    }
}

int EditorView::lineWidth(quint32 line)
{
    const QHash<quint32, int>::const_iterator it = m_widths.constFind(line);

    if (it != m_widths.constEnd()) {
        return it.value();
    }

    // width() 会把这一行整行量完，所以结果一定要缓存住
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

    // 先看出发点对不对，这是绝大多数情况（光标没跨行）
    if (isLineOf(lineBeginOf(idea), lineEndOf(idea), offset)) {
        return idea;
    }

    // 不对就从一行的窗口开始往两边翻倍，直到把 offset 套住。
    // 上下移动几行只要看几行，跳到文件另一头也只是 O(log 行数) 次
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
            // 头尾都到边了还没套住，那一定落在最后一行
            return total - 1;
        }

        span *= 2;
    }

    // 窗口不大（最多 span 行），逐行找就行
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

    // findLine 有 m_lineHint 当出发点，光标没跨行时基本是常数时间
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

// ============================== 键盘 ==============================

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

        // 目标列取"原来的列"和"目标行的字节长度"里小的那个。
        // 因为列号每次都是从光标现算的，列号不会被短行永久吃掉
        const quint32 line = clampU32(static_cast<qint64>(span.line) + step, 0, text->lineCount() - 1);
        const quint32 column = std::min(span.column, lineByteLength(line));

        moveTo(line, column, keep);
        afterMove();

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

    case Qt::Key_PageUp:
    case Qt::Key_PageDown: {
        // 翻一屏，同时保持横向列不动（和上下移动一个道理）
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

        // 在行首按退格 = 把上一行的换行符吃掉，也就是两行接上。
        // 行内退格则只退一个字符（UTF-8 的多字节字符一起退）
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

    // ---- 快捷键 ----
    if (control) {
        if (key == Qt::Key_A) {
            m_anchor = 0;
            m_cursor = text->byteCount();

            afterMove();

            return;
        }

        if (key == Qt::Key_C) {
            copyToClipboard(false);

            return;
        }

        if (key == Qt::Key_X) {
            copyToClipboard(true);

            return;
        }

        if (key == Qt::Key_V) {
            pasteFromClipboard();

            return;
        }
    }

    // ---- 普通输入 ----
    // 这里只看 text()：Ctrl 组合键的 text() 是控制字符，直接忽略。
    // 唯一要放行的是 Ctrl+Alt（Windows 上 AltGr 就是这个组合），
    // 有些键盘布局要靠它打字符
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
            // 行内左移：只取光标左边那一小段，跨过整个 UTF-8 字符
            const quint32 begin = text->lineBegin(span.line);
            const std::string part = text->range(begin, m_cursor);

            moveTo(span.line, stepBack(part, span.column), keepSelection);
        } else if (span.line > 0) {
            // 行首左移：跳到上一行的行尾
            const quint32 previous = span.line - 1;

            moveTo(previous, lineByteLength(previous), keepSelection);
        } else {
            moveTo(0, 0, keepSelection);
        }

        return;
    }

    const quint32 length = lineByteLength(span.line);

    if (span.column < length) {
        // 行内右移：只取光标右边那一小段
        const quint32 begin = text->lineBegin(span.line);
        const std::string part = text->range(m_cursor, begin + length + 1);

        moveTo(span.line, stepForward(part, 0), keepSelection);

        return;
    }

    if (span.line + 1 < text->lineCount()) {
        // 行尾右移：跳到下一行的行首
        moveTo(span.line + 1, 0, keepSelection);
    }
}

quint32 EditorView::caretWordOffset(TextBuffer *text, bool forward)
{
    // 单词跳转：把字符分成"空白 / 词字符 / 其它标点"三类，同类连续算一段。
    // 只看光标附近的那一段文本，不整份文档拖出来
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

    // 先退掉紧跟在光标左边的续字节，保证起点落在字符边界上
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

    // 改动点之后的行号可能整体错位，缓存必须从这一行开始作废
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

    // 缓冲区里只有 '\n'，剪贴板里的换行要的是 CRLF，这里补上
    QString out = LineMap::decode(text->range(from, to));

    out.replace(QStringLiteral("\n"), QStringLiteral("\r\n"));

    return out;
}

void EditorView::copyToClipboard(bool cut)
{
    const QString selected = selectionText();

    if (selected.isEmpty()) {
        return;
    }

    QGuiApplication::clipboard()->setText(selected);

    if (cut) {
        deleteRange(std::min(m_anchor, m_cursor), std::max(m_anchor, m_cursor));
    }
}

void EditorView::pasteFromClipboard()
{
    const QString content = QGuiApplication::clipboard()->text();

    if (!content.isEmpty()) {
        // 粘贴的内容可能带 CRLF，交给 TextBuffer::insert() 折成 LF
        insertText(content);
    }
}

int EditorView::caretPixelX(quint32 line, quint32 column)
{
    // 光标画在字符的左边缘：一次整除跳到块里，块内查一次就出 x。
    // 这一行几百万个字符也无所谓，因为被量过的块一直只有屏幕上那一两块
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

    // ---- 竖向：上下各留一行的余量 ----
    QScrollBar *vertical = verticalScrollBar();

    const int lineTop = static_cast<int>(line) * m_lineHeight;
    const int top = vertical->value();

    if (lineTop - m_lineHeight < top) {
        vertical->setValue(std::max(0, lineTop - m_lineHeight));
    } else if (lineTop + m_lineHeight * 2 > top + viewHeight) {
        vertical->setValue(std::max(0, lineTop + m_lineHeight * 2 - viewHeight));
    }

    // ---- 横向：只保证"看得见"，不居中，免得打字时光标乱跳 ----
    QScrollBar *horizontal = horizontalScrollBar();

    const int left = horizontal->value();
    const int margin = m_metrics.codeWidth * 2;

    if (x - margin < left) {
        horizontal->setValue(std::max(0, x - margin));
    } else if (x + margin > left + textArea) {
        horizontal->setValue(std::max(0, x + margin - textArea));
    }
}

// ============================== 滚动 ==============================

void EditorView::updateScrollBars()
{
    // 这个函数会改滚动条可见性，从而改变视口大小；视口变化又会触发 resizeEvent
    // 再调这里。加个守卫，避免两个方向来回打架
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
        setScrollBarVisible(vertical, false);
        setScrollBarVisible(horizontal, false);

        m_reflowGuard = false;

        return;
    }

    const int viewWidth = viewport()->width();
    const int viewHeight = viewport()->height();

    // ---- 竖向 ----
    const qint64 documentHeight = static_cast<qint64>(text->lineCount()) * m_lineHeight;
    const int lineCount = static_cast<int>(text->lineCount());

    // 行号栏宽度按"最大行号有几位"留出来，行数变了就跟着变
    const int digits = QString::number(std::max(1, lineCount)).size();

    m_gutter = std::max(m_digitWidth * 3, m_digitWidth * digits + GutterGap * 2);

    vertical->setRange(0, static_cast<int>(std::max<qint64>(0, documentHeight - viewHeight)));
    vertical->setPageStep(std::max(1, viewHeight));
    vertical->setSingleStep(m_lineHeight);
    setScrollBarVisible(vertical, documentHeight > viewHeight);

    // ---- 横向 ----
    // 这里没有维护"全文档最宽行"的全局统计（那需要改动 TextBuffer，或者每次编辑
    // 都全量重算）。改成采样：按均匀步长取 WidthSampleTarget 行，再单独算上最后
    // 一行。超长单行的文件通常就那么一两行，必然被采到；普通源码文件里各行宽度本来
    // 就差不多，采样出来的滚动范围足够用
    // 代价说清楚：采样那一行如果很长，lineWidth() 会把它整行量完，一次约几十毫秒
    // （一百万个字符）。这是"不改动核心数据结构"换来的取舍
    const quint32 lines = text->lineCount();
    int widest = 0;

    if (lines > 0) {
        const quint32 step = std::max<quint32>(1, lines / WidthSampleTarget);

        for (quint32 line = 0; line < lines; line += step) {
            widest = std::max(widest, lineWidth(line));
        }

        widest = std::max(widest, lineWidth(lines - 1));
    }

    const qint64 contentWidth = static_cast<qint64>(widest) + m_padding * 2;
    const int textArea = std::max(1, viewWidth - m_gutter);

    horizontal->setRange(0, static_cast<int>(std::max<qint64>(0, contentWidth - textArea)));
    horizontal->setPageStep(textArea);
    horizontal->setSingleStep(std::max(1, m_metrics.codeWidth * 2));
    setScrollBarVisible(horizontal, contentWidth > textArea);

    if (documentHeight <= viewHeight) {
        vertical->setValue(0);
    }

    if (contentWidth <= textArea) {
        horizontal->setValue(0);
    }

    m_reflowGuard = false;
}

// ============================== 事件 ==============================

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

    // 横向滚动条会从视口底部吃掉一条，可见行数要按真正剩下的高度算
    const int usableHeight = area.height() - (horizontal->isVisible() ? horizontal->height() : 0);
    const int visibleLines = std::max(1, usableHeight / m_lineHeight + 2);

    const int textLeft = m_gutter + m_padding;
    const int numberRight = m_gutter - GutterGap;

    // 文字区的宽度：从 textLeft 一直到视口右边缘。正文裁剪、切可见段都用它，
    // 和横向滚动条的范围计算保持同一套算法
    const int textWidth = std::max(1, area.width() - textLeft);

    const bool hasSelection = (m_anchor != m_cursor);
    const quint32 selectionFrom = std::min(m_anchor, m_cursor);
    const quint32 selectionTo = std::max(m_anchor, m_cursor);

    // 光标的位置只问一次，循环里拿它比对行号
    const Position caret = text->positionOf(m_cursor);

    const bool caretVisible = m_blinkOn && hasFocus();

    for (int offset = 0; offset < visibleLines; offset++) {
        const int number = firstLine + offset;

        if (number < 0 || static_cast<quint32>(number) >= total) {
            break;
        }

        const quint32 line = static_cast<quint32>(number);
        const int y = number * m_lineHeight - scrollY;

        // ---- 行号 ----
        painter.setPen(m_gutterText);
        painter.drawText(QRect(0, y, numberRight, m_lineHeight), Qt::AlignRight | Qt::AlignVCenter,
                         QString::number(number + 1));

        const quint32 lineBegin = text->lineBegin(line);
        const quint32 lineEnd = text->lineEnd(line);

        // 这一个循环体里只会为"当前这一行"建一次定位表，所以拿到的引用
        // 在本轮结束前不会再被别的 lineMap() 调用弄失效
        const LineMap &map = lineMap(line);

        const int units = map.entryCount();

        // ---- 选区 ----
        // 选区可能横跨几百万个字符，但这里只画"落在屏幕里的那一段"：
        // x1 / x2 是整条选区的两端，真正的裁剪交给 max / min，免得构造出一个
        // 几百万像素宽的矩形
        if (hasSelection && lineBegin <= selectionTo && lineEnd >= selectionFrom) {
            const quint32 from = std::max(lineBegin, selectionFrom);
            const quint32 to = std::min(lineEnd, selectionTo) + 1;

            // 算减法的两个操作数都是行内字节列，天然不会有下溢
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

        // ---- 正文 ----
        // 屏幕上第一个可见字符：把"已经被横向滚掉的那段宽度"换算成字符下标。
        // 这一步一次整除就能跳到对应的块，块内走几步，不扫全行
        const quint32 wanted = static_cast<quint32>(std::max(0, scrollX - textLeft));
        const int unit = map.entryAtByte(wanted);

        // 最后一个可见字符：直接用 x 去问，不再靠"再多取几个字符"来兜底
        int last = map.entryAtX(std::max(0, scrollX + textWidth) - textLeft) + 1;

        last = std::min(units, std::max(unit + 1, last));

        const quint32 firstByte = map.byteAtEntry(unit);
        const quint32 lastByte = map.byteAtEntry(last);

        if (last > unit && lastByte > firstByte) {
            const int beginChar = map.charIndexAtEntry(unit);
            const int endChar = map.charIndexAtEntry(last);

            // 关键的一步：只从 TextBuffer 里切这一小段，绝不整行取出。
            // 一百万个字符的单行在这里也只复制几十个字节
            const std::string piece = text->range(lineBegin + firstByte, lineBegin + lastByte);

            const QString shown = LineMap::decode(piece).left(endChar - beginChar);

            const int x = textLeft + map.xAtColumn(firstByte) - scrollX;

            painter.setPen(m_textColor);

            // 裁到文字区：切出来的这一段的左边缘通常落在屏幕左边之外
            // （它对齐的是块头），不裁剪的话多出来的部分会画到行号栏和
            // 窗口右边界外面去，看起来就像"显示区域错位"
            painter.save();
            painter.setClipRect(QRect(textLeft, y, textWidth, m_lineHeight));
            painter.drawText(x, y + m_ascent, shown);
            painter.restore();
        }

        // ---- 光标 ----
        if (caretVisible && caret.line == line) {
            // caretPixelX 内部就是 LineMap::xAtColumn，一次整除 + 块内查一次
            const int caretX = textLeft + caretPixelX(line, caret.column) - scrollX;

            if (caretX >= area.left() - 2 && caretX <= area.right()) {
                painter.fillRect(QRect(caretX, y + m_caretTop, m_caretWidth, m_caretHeight), m_caretColor);
            }
        }
    }

    // 滚动条和行号栏的交角，涂一下免得露出底色
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
        // 先把锚点落到新位置，等真的拖起来再决定要不要留一个"选中起点"
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
        // 同一个位置一直在动鼠标，不用重画
        return;
    }

    m_cursor = offset;

    // 拖到窗口外面就自己滚，省得用户一边拖一边找滚动条
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

    // 点一下（没有拖动）等于把光标放这儿并清掉选区
    if (m_anchor == m_cursor) {
        m_anchor = m_cursor;
    }

    viewport()->update();
}

void EditorView::mouseDoubleClickEvent(QMouseEvent *event)
{
    // 双击选中光标下的那一串"同类字符"：字母数字下划线算一类，其余各自成类。
    // 用 TextBuffer::range() 只取当前这一行来扫，不整份文档拖出来
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
    // Ctrl + 滚轮留给"缩放"这种高级功能，这里只按行滚
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

    // 屏幕坐标 -> 文档坐标
    const int documentY = point.y() + vertical->value();
    const int documentX = point.x() + horizontal->value() - m_gutter - m_padding;

    const qint64 rawLine = documentY / m_lineHeight;

    const quint32 line = clampU32(rawLine, 0, text->lineCount() - 1);

    // 鼠标定位：把 x 交给这一行的定位表。它会先在块表上二分，再在块内走几步，
    // 所以哪怕这一行有一百万个字符，也只量了屏幕上看得见的那一块
    const quint32 column = lineMap(line).columnAtX(documentX);

    m_lineHint = line;

    return clampU32(static_cast<qint64>(text->lineBegin(line)) + column, 0, text->byteCount());
}
