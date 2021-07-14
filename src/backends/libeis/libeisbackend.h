#pragma once

#include "inputbackend.h"

namespace KWin
{
namespace Libeis
{
class Connection;
}

class LibeisBackend : public InputBackend
{
    Q_OBJECT
public:
    explicit LibeisBackend(QObject *parent = nullptr);
    ~LibeisBackend() override;
    void initialize() override;

private:
    Libeis::Connection *m_connection;
};

}
