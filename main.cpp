#include "EditorWindow.h"

#include <QApplication>

/// Qt GUI entry point. The original console test driver still exists, moved to
/// main_console.cpp and built as the second target SimpleEditorCli -- that is
/// where the red-black tree self check (check / dump) can still be run.

namespace
{
/// Command line argument -> QString.
/// On Windows an argument may arrive as ANSI or in another encoding;
/// fromLocal8Bit() interprets it using the current code page, which beats
/// guessing the encoding ourselves.
QString argumentText(const char *value)
{
    return QString::fromLocal8Bit(value);
}
}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    EditorWindow window;

    window.show();

    // a file name on the command line is opened right away, as the console
    // version did. openPath() shows its own warning if that fails.
    if (argc >= 2) {
        window.openPath(argumentText(argv[1]));
    }

    return app.exec();
}
