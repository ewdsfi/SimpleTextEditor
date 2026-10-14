#include "EditorWindow.h"

#include <QApplication>

/// Qt GUI 版的入口。原来的控制台测试程序还在，只是挪到了 main_console.cpp，
/// 由 CMake 里的第二个目标 SimpleEditorCli 编出来 —— 红黑树那一整套自检
/// （check / dump）还是靠它来跑。

namespace
{
/// 命令行参数 -> QString。
/// Windows 上进程拿到的参数本来可能是 ANSI 也可能是别的编码，Qt 的
/// fromLocal8Bit() 会按当前代码页正确解释，比我们自己猜编码靠谱
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

    // 命令行上给的文件名直接打开，和原来的控制台版行为一致。
    // 打开失败时 openPath() 自己弹过提示框，这里不用再报一遍
    if (argc >= 2) {
        window.openPath(argumentText(argv[1]));
    }

    return app.exec();
}
