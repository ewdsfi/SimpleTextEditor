#ifndef EDITORWINDOW_H
#define EDITORWINDOW_H

#include <QMainWindow>

class Document;
class EditorView;

/// EditorWindow: menu bar, status bar and one EditorView.
///
/// This layer only wires the menu actions to Document's read/write and shows the
/// caret position in the status bar. How text is painted and how the caret is
/// resolved is entirely EditorView's business.
class EditorWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit EditorWindow(QWidget *parent = nullptr);

    /// Open a file; this is the path a command line argument takes.
    bool openPath(const QString &path);

protected:
    void closeEvent(QCloseEvent *event) override;

private slots:
    void openFile();
    void saveFile();
    void saveFileAs();

    /// The caret moved: refresh the status bar.
    void refreshStatus();

private:
    /// Ask before throwing away unsaved changes; false means the user cancelled.
    bool confirmDiscard();

    /// Load path into the view and report the result in the status bar.
    void loadInto(const QString &path);

    /// Save to path (an empty path means "keep the current one").
    bool saveTo(const QString &path);

    /// Window title: file name plus a star while there are unsaved changes.
    void refreshTitle();

    /// Conversion between UTF-8 std::string and QString, hand rolled to avoid
    /// depending on QTextCodec.
    static QString fromUtf8(const std::string &text);
    static std::string toUtf8(const QString &text);

    Document *m_document = nullptr;
    EditorView *m_view = nullptr;
};

#endif // EDITORWINDOW_H
