/*
    SPDX-FileCopyrightText: 2021 David Redondo <kde@david-redondo>

    SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#pragma once

#include <QObject>
#include <QThread>

extern "C" {
struct eis;
}

namespace KWin
{
namespace Libeis
{
class Device;

class Connection : public QObject
{
    Q_OBJECT
public:
    Connection(eis *eis);
    ~Connection() override;
Q_SIGNALS:
    void deviceAdded(Device *device);
    void deviceRemoved(Device *device);
private Q_SLOTS:
    void handleEvents();
private:
    std::unique_ptr<QThread> m_thread;
    eis *m_eis;
};

}
}
