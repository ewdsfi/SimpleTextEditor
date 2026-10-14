#include "EditorWindow.h"

#include "Document.h"
#include "EditorView.h"

#include <QAction>
#include <QCloseEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QKeySequence>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QStatusBar>
#include <QString>
#include <QStringDecoder>

namespace
{
/// QByteArray（UTF-8 字节）-> QString。
/// 这里显式用 QStringDecoder，不碰 QTextCodec：那个类在 Qt6 里已经被挪到
/// Qt5Compat 里去了，而我们只想安安静静读一个 UTF-8 文件
QString decodeUtf8(const QByteArray &bytes)
{
    QStringDecoder decoder(QStringDecoder::Utf8);

    return decoder.decode(bytes);
}
}

EditorWindow::EditorWindow(QWidget *parent)
    : QMainWindow(parent)
{
    m_document = new Document();
    m_view = new EditorView(this);

    m_view->setDocument(m_document);
    setCentralWidget(m_view);

    // ---- 文件菜单 ----
    QMenu *file = menuBar()->addMenu(QStringLiteral("文件(&F)"));

    QAction *open = file->addAction(QStringLiteral("打开(&O)..."));
    open->setShortcut(QKeySequence::Open);

    QAction *save = file->addAction(QStringLiteral("保存(&S)"));
    save->setShortcut(QKeySequence::Save);

    QAction *saveAs = file->addAction(QStringLiteral("另存为(&A)..."));
    saveAs->setShortcut(QKeySequence::SaveAs);

    file->addSeparator();

    QAction *quit = file->addAction(QStringLiteral("退出(&Q)"));
    quit->setShortcut(QKeySequence::Quit);

    connect(open, &QAction::triggered, this, &EditorWindow::openFile);
    connect(save, &QAction::triggered, this, &EditorWindow::saveFile);
    connect(saveAs, &QAction::triggered, this, &EditorWindow::saveFileAs);
    connect(quit, &QAction::triggered, this, &EditorWindow::close);

    connect(m_view, &EditorView::cursorMoved, this, &EditorWindow::refreshStatus);

    // 状态栏先摆好标签，省得光标一动就要重新拼字符串
    statusBar()->showMessage(QStringLiteral("就绪"));

    resize(1000, 680);

    refreshTitle();
    refreshStatus();
}

bool EditorWindow::openPath(const QString &path)
{
    if (path.isEmpty()) {
        return false;
    }

    // 有没保存的东西就先问一句，免得白白丢掉
    if (!confirmDiscard()) {
        return false;
    }

    loadInto(path);

    return true;
}

void EditorWindow::loadInto(const QString &path)
{
    const std::string narrow = path.toUtf8().toStdString();

    if (!m_document->load(narrow)) {
        QMessageBox::warning(this, QStringLiteral("打开失败"),
                             QStringLiteral("打不开文件：\n%1\n\n%2").arg(path, fromUtf8(m_document->error())));

        return;
    }

    // 文档整个换了，光标、滚动条、定位缓存都得回到初始状态
    m_view->resetView();

    statusBar()->showMessage(QStringLiteral("已打开 %1：%2 行 %3 字节")
                                 .arg(path)
                                 .arg(m_document->buffer()->lineCount())
                                 .arg(m_document->buffer()->byteCount()),
                             5000);

    refreshTitle();
    refreshStatus();
}

void EditorWindow::openFile()
{
    if (!confirmDiscard()) {
        return;
    }

    const QString start = m_document->named() ? fromUtf8(m_document->path()) : QString();

    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("打开文件"), start,
                                                      QStringLiteral("所有文件 (*)"));

    if (path.isEmpty()) {
        return;
    }

    loadInto(path);
}

void EditorWindow::saveFile()
{
    if (m_document->named()) {
        saveTo(QString());

        return;
    }

    // 还没有文件名就退化成"另存为"
    saveFileAs();
}

void EditorWindow::saveFileAs()
{
    const QString start = m_document->named() ? fromUtf8(m_document->path()) : QStringLiteral("未命名.txt");

    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("保存文件"), start,
                                                      QStringLiteral("文本文件 (*.txt);;所有文件 (*)"));

    if (path.isEmpty()) {
        return;
    }

    saveTo(path);
}

bool EditorWindow::saveTo(const QString &path)
{
    // 空 path 表示"沿用当前路径"，也就是普通的保存
    if (!path.isEmpty()) {
        // Document 只会写到 _path 上，所以"另存为"就是把路径换掉再写
        m_document->setPath(path.toUtf8().toStdString());
    }

    if (!m_document->save()) {
        QMessageBox::warning(this, QStringLiteral("保存失败"), fromUtf8(m_document->error()));

        return false;
    }

    m_view->clearModified();

    statusBar()->showMessage(QStringLiteral("已保存到 %1").arg(fromUtf8(m_document->path())), 5000);

    refreshTitle();

    return true;
}

bool EditorWindow::confirmDiscard()
{
    if (!m_view->isModified()) {
        return true;
    }

    const QMessageBox::StandardButton answer =
        QMessageBox::question(this, QStringLiteral("尚未保存"),
                              QStringLiteral("当前内容还没有保存，要放弃这些改动吗？"),
                              QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);

    if (answer == QMessageBox::Cancel) {
        return false;
    }

    if (answer == QMessageBox::Save) {
        saveFile();

        // 保存本身可能被取消（比如另存为对话框被关掉），那就当作不想继续
        return !m_view->isModified();
    }

    return true;
}

void EditorWindow::closeEvent(QCloseEvent *event)
{
    if (confirmDiscard()) {
        event->accept();

        return;
    }

    event->ignore();
}

void EditorWindow::refreshStatus()
{
    const Position position = m_view->cursorPosition();

    const QString name = m_document->named() ? fromUtf8(m_document->path()) : QStringLiteral("（未命名）");

    statusBar()->showMessage(QStringLiteral("%1    行 %2，列 %3    共 %4 字节")
                                 .arg(name)
                                 .arg(position.line + 1)
                                 .arg(position.column + 1)
                                 .arg(m_document->buffer()->byteCount()));
}

void EditorWindow::refreshTitle()
{
    const QString name = m_document->named() ? QFileInfo(fromUtf8(m_document->path())).fileName()
                                             : QStringLiteral("未命名");

    setWindowTitle(QStringLiteral("%1%2 - 简易文本编辑器").arg(name, m_view->isModified() ? QStringLiteral("*") : QString()));
}

QString EditorWindow::fromUtf8(const std::string &text)
{
    return decodeUtf8(QByteArray(text.data(), static_cast<int>(text.size())));
}

std::string EditorWindow::toUtf8(const QString &text)
{
    // 走 QString::toUtf8() 而不是手写编码：这里已经在 Qt 的地盘上，不用再绕
    const QByteArray bytes = text.toUtf8();

    return std::string(bytes.constData(), static_cast<size_t>(bytes.size()));
}
