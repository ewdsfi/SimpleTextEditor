#ifndef EDITORWINDOW_H
#define EDITORWINDOW_H

#include <QMainWindow>

class Document;
class EditorView;

/// EditorWindow —— 菜单 + 状态栏 + 一个 EditorView。
///
/// 这一层只干两件事：把菜单动作接到 Document 的读写上，把光标位置显示到状态栏。
/// 文本怎么画、光标怎么定位，全在 EditorView 里，这里一概不管。
class EditorWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit EditorWindow(QWidget *parent = nullptr);

    /// 打开一个文件（命令行参数走的就是这条路）
    bool openPath(const QString &path);

protected:
    void closeEvent(QCloseEvent *event) override;

private slots:
    void openFile();
    void saveFile();
    void saveFileAs();

    /// 光标动过了：更新状态栏
    void refreshStatus();

private:
    /// 有未保存的改动时问一句，返回 false 表示用户按了取消
    bool confirmDiscard();

    /// 把 path 塞进视图并在状态栏显示结果
    void loadInto(const QString &path);

    /// 保存到 path（空串表示沿用当前路径）
    bool saveTo(const QString &path);

    /// 窗口标题：文件名 + 星号（有未保存改动时）
    void refreshTitle();

    /// std::string（UTF-8）与 QString 之间的转换。自己转，不依赖 QTextCodec
    static QString fromUtf8(const std::string &text);
    static std::string toUtf8(const QString &text);

    Document *m_document = nullptr;
    EditorView *m_view = nullptr;
};

#endif // EDITORWINDOW_H
