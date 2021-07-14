#include "libeisbackend.h"

#include "backends/libeis/libeis_logging.h"
#include "connection.h"
#include "device.h"
#include "main.h"
#include "input.h"



#include <libeis.h>


namespace KWin
{

LibeisBackend::LibeisBackend(QObject *parent)
    : InputBackend(parent)
{
    qRegisterMetaType<KWin::InputRedirection::PointerButtonState>();
    qRegisterMetaType<InputRedirection::PointerAxis>();
    qRegisterMetaType<InputRedirection::PointerAxisSource>();
    qRegisterMetaType<InputRedirection::KeyboardKeyState>();
}

LibeisBackend::~LibeisBackend()
{
    delete m_connection;
}

void LibeisBackend::initialize()
{
    qDebug() << "initliaze";
    constexpr int maxSocketNumber = 32;
    QByteArray socketName;
    int socketNum = 0;
    eis *eis = eis_new(nullptr);
    do {
        if (socketNum == maxSocketNumber) {
            return;
        }
        socketName = QByteArrayLiteral("eis-") + QByteArray::number(socketNum++);
    } while (eis_setup_backend_socket(eis, socketName));

    qDebug() << socketName;

    qputenv("LIBEI_SOCKET", socketName);
    auto env = kwinApp()->processStartupEnvironment();
    env.insert("LIBEI_SOCKET", socketName);
    static_cast<ApplicationWaylandAbstract *>(kwinApp())->setProcessStartupEnvironment(env);

    m_connection = new Libeis::Connection(std::move(eis));
    connect(m_connection, &Libeis::Connection::deviceAdded, this, &InputBackend::deviceAdded);
    connect(m_connection, &Libeis::Connection::deviceRemoved, this, &InputBackend::deviceRemoved);
}

}
