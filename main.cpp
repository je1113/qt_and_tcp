#include <QApplication>
#include <QWidget>

int main(int argc, char *argv[])
{
    // GUI 프로그램 전체를 관리하는 객체. 반드시 위젯보다 먼저 만들어야 한다.
    QApplication app(argc, argv);

    QWidget window;
    window.resize(400, 300);

    window.setWindowTitle("TCP Chat");

    window.show();

    // 이벤트 루프 시작. 창이 닫힐 때까지 여기서 반환되지 않는다.
    return app.exec();
}
