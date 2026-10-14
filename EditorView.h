#ifndef EDITORVIEW_H
#define EDITORVIEW_H

#include "LineMap.h"
#include "TextBuffer.h"

#include <QAbstractScrollArea>
#include <QColor>
#include <QFont>
#include <QHash>
#include <QPoint>
#include <QTimer>

class Document;
class QScrollBar;

/// EditorView —— 自己画的文本视图。
///
/// 它只做三件事：把 TextBuffer 里的文字画出来、记住光标在哪、把鼠标点击换算成
/// 字节下标。文件读写完全交给 Document，这里一行都不碰文件。
///
/// 两个和性能有关的约定：
///   * 画一行时**从不**把整行取出来。先问 LineMap"这一段屏幕对应哪几个字节"，
///     再让 TextBuffer::range() 只切这一小段，所以一百万个字符的单行也不卡；
///   * 光标定位和鼠标定位都走 LineMap 的两级块索引，不扫全行，也不用前缀和数组。
class EditorView : public QAbstractScrollArea
{
    Q_OBJECT

public:
    explicit EditorView(QWidget *parent = nullptr);

    /// 接管一个文档（不取得所有权，文档由主窗口持有）。换了文档之后缓存全部作废
    void setDocument(Document *document);

    /// 在光标处插入文本（\r\n 和单独的 \r 都会被缓冲区折成 \n），光标随之移到末尾
    void insertText(const QString &text);

    /// 当前光标所在的行列（行列都从 0 开始，列按字节算）
    Position cursorPosition() const;

    /// 是否被改动过，主窗口用它判断"关窗口前要不要提醒保存"
    bool isModified() const { return m_modified; }

    /// 把"已改动"标记抹掉，存盘之后调用
    void clearModified() { m_modified = false; }

    /// 内容整个换掉了（打开文件）之后，重置光标、滚动条和全部缓存
    void resetView();

signals:
    /// 光标位置或改动状态变了，主窗口用它刷新状态栏
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

private slots:
    void blinkCaret();

private:
    /// document 里的文本缓冲区，没有文档时给 nullptr
    TextBuffer *buffer() const;

    /// 重新算字体宽度、行高、行号栏宽度和各种绘制尺寸
    void setupFont();

    /// 光标或选区被改小之后，把两个下标夹回文档范围内
    void clampCursor();

    /// 把光标滚进可视范围。横向只保证"看得见"，不居中，免得打字时光标乱跳
    void ensureCursorVisible();

    /// 重新设置两个滚动条的范围和可见性
    void updateScrollBars();

    /// 只在可见性真的变化时才动滚动条，否则会和我们自己的 reflow 守卫打架
    static void setScrollBarVisible(QScrollBar *bar, bool visible);

    /// 行 line 的定位表（按需构造）。调用方拿到引用要立刻用完：
    /// 期间再建别的行会让 QHash 重新散列，引用就悬了
    const LineMap &lineMap(quint32 line);

    /// 真的去构造某一行：只取这一行的字节交给 LineMap
    LineMap buildLineMap(quint32 line);

    /// 行 line 的像素宽度，会缓存住
    int lineWidth(quint32 line);

    /// 内容改过之后作废 offset 所在行及其后面所有行的缓存。
    /// 前面的行号没变、内容也没变，所以刻意保留
    void forgetFrom(quint32 offset);

    /// 字节下标 -> 行号。从 idea 行开始按指数扩张，不扫描整个文档
    quint32 findLine(quint32 offset, quint32 idea);

    /// 光标位置的缓存：行号 + 行内字节列。上下移动要靠它保住目标列
    struct CaretSpan
    {
        quint32 line = 0;    ///< 行号
        quint32 column = 0;  ///< 行内字节列
    };

    /// 取当前光标的行列（用一次 positionOf）
    CaretSpan caretSpan();

    /// 行 line 有多少字节（不含换行符）
    quint32 lineByteLength(quint32 line);

    /// 把光标放到"行 line 的第 column 个字节"上；keepSelection 为假时把锚点一起收过来
    void moveTo(quint32 line, quint32 column, bool keepSelection);

    /// 把光标挪到某个字节下标处
    void moveToOffset(quint32 offset, bool keepSelection);

    /// 光标动过了：重置闪烁、滚进可视区、重画
    void afterMove();

    /// 复制 / 剪切 / 粘贴所选内容
    void copyToClipboard(bool cut);

    /// 粘贴剪贴板里的文本
    void pasteFromClipboard();

    /// 删除从 from 到 to 的区间，光标停在 from 上
    void deleteRange(quint32 from, quint32 to);

    /// offset 是否落在 [begin, end] 这一行里
    static bool isLineOf(quint32 begin, quint32 end, quint32 offset);

    /// 行首 / 行尾字节下标，没有文档时给 0
    quint32 lineBeginOf(quint32 line);
    quint32 lineEndOf(quint32 line);

    /// 屏幕坐标（视口坐标系）-> 字节下标
    quint32 offsetAt(const QPoint &point);

    /// 左右方向键：行内走一个字符，走到行尾就换行。方向为 -1 表示左
    void moveCaret(TextBuffer *text, int direction, bool keepSelection);

    /// Ctrl + 左右：往后 / 往前跳一个"词"。方向感由 forward 决定
    quint32 caretWordOffset(TextBuffer *text, bool forward);

    /// 当前选中的文本，没选中就给空串（换行已经换成剪贴板要的 CRLF）
    QString selectionText();

    /// 光标画在行 line 第 column 个字节处的 x（视口坐标系里还要再加上文字左边距）
    int caretPixelX(quint32 line, quint32 column);

    /// 从一个字节下标往前走一个字符（跨过整个 UTF-8 序列）
    static quint32 stepBack(const std::string &text, quint32 offset);

    /// 从一个字节下标往后走一个字符
    static quint32 stepForward(const std::string &text, quint32 offset);

    /// UTF-16 转 UTF-8。自己写一遍，就为了不依赖 QTextCodec
    static std::string toUtf8(const QString &text);

    /// 把 v 夹进 [low, high]
    static quint32 clampU32(qint64 v, quint32 low, quint32 high);

    Document *m_document = nullptr;

    // ---- 字体与度量 ----
    QFont m_font;
    LineMap::Metrics m_metrics;
    int m_lineHeight = 16;
    int m_ascent = 12;
    int m_digitWidth = 8;
    int m_gutter = 48;
    int m_padding = 4;
    int m_caretWidth = 2;
    int m_caretTop = 2;
    int m_caretHeight = 14;

    /// updateScrollBars() 正在跑。滚动条可见性一变，视口尺寸就变，又会绕回来
    bool m_reflowGuard = false;

    // ---- 配色 ----
    QColor m_background;
    QColor m_gutterColor;
    QColor m_gutterText;
    QColor m_textColor;
    QColor m_selectionColor;
    QColor m_caretColor;

    // ---- 光标与选区 ----
    quint32 m_cursor = 0;  ///< 光标字节下标
    quint32 m_anchor = 0;  ///< 选区起点，与光标相等时表示没有选区
    bool m_selecting = false;
    bool m_modified = false;

    // ---- 光标闪烁 ----
    QTimer m_blinkTimer;
    bool m_blinkOn = true;

    // ---- 定位缓存 ----
    QHash<quint32, LineMap> m_lines;   ///< 行号 -> 定位表
    QHash<quint32, int> m_widths;      ///< 行号 -> 行宽（只在算滚动范围时用）
    quint32 m_lineHint = 0;            ///< 上次用过的行号，给 findLine 当出发点
};

#endif // EDITORVIEW_H
