#include "mainwindow.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTcpSocket>
#include <QVBoxLayout>

MainWindow::MainWindow(QWidget *parent)
    : QWidget(parent)
{
    setWindowTitle("TCP Chat");
    resize(400, 120);

    // 위젯들에 this(부모)를 주면, 창이 삭제될 때 Qt가 자식들도 함께 delete 해준다.
    hostEdit = new QLineEdit("127.0.0.1", this);
    portSpin = new QSpinBox(this);
    portSpin->setRange(1, 65535);
    portSpin->setValue(5000);
    connectButton = new QPushButton("연결", this);
    disconnectButton = new QPushButton("해제", this);
    statusLabel = new QLabel(this);

    auto *panel = new QHBoxLayout;
    panel->addWidget(hostEdit);
    panel->addWidget(portSpin);
    panel->addWidget(connectButton);
    panel->addWidget(disconnectButton);

    auto *root = new QVBoxLayout(this);
    root->addLayout(panel);
    root->addWidget(statusLabel);
    root->addStretch();

    socket = new QTcpSocket(this);

    // 버튼 클릭 → 우리 슬롯
    connect(connectButton, &QPushButton::clicked, this, &MainWindow::onConnectClicked);
    connect(disconnectButton, &QPushButton::clicked, this, &MainWindow::onDisconnectClicked);

    // 소켓 상태 변화/오류 → 우리 슬롯
    connect(socket, &QTcpSocket::stateChanged, this, &MainWindow::onStateChanged);
    connect(socket, &QTcpSocket::errorOccurred, this, &MainWindow::onErrorOccurred);

    // 시작 시점의 화면도 같은 함수로 맞춘다.
    onStateChanged(socket->state());
}

void MainWindow::onConnectClicked()
{
    // 비동기: 여기서 바로 반환되고, 결과는 나중에 stateChanged/errorOccurred로 온다.
    socket->connectToHost(hostEdit->text(), static_cast<quint16>(portSpin->value()));
}

void MainWindow::onDisconnectClicked()
{
    socket->disconnectFromHost();
}

void MainWindow::onStateChanged(QAbstractSocket::SocketState state)
{
    switch (state) {
    case QAbstractSocket::UnconnectedState:
        statusLabel->setText("연결 안 됨");
        break;
    case QAbstractSocket::HostLookupState:
    case QAbstractSocket::ConnectingState:
        statusLabel->setText("연결 중...");
        break;
    case QAbstractSocket::ConnectedState:
        statusLabel->setText("연결됨");
        break;
    case QAbstractSocket::ClosingState:
        statusLabel->setText("해제 중...");
        break;
    default:
        break;
    }

    // 연결이 안 된 상태에서만 주소를 바꾸고 [연결]을 누를 수 있게 한다.
    const bool idle = (state == QAbstractSocket::UnconnectedState);
    hostEdit->setEnabled(idle);
    portSpin->setEnabled(idle);
    connectButton->setEnabled(idle);
    disconnectButton->setEnabled(state == QAbstractSocket::ConnectedState);
}

void MainWindow::onErrorOccurred(QAbstractSocket::SocketError error)
{
    Q_UNUSED(error);

    statusLabel->setText("오류: " + socket->errorString());
}
