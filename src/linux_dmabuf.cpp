/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2019 Roman Gilg <subdiff@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "linux_dmabuf.h"

#include "wayland_server.h"

namespace KWin
{

LinuxDmaBufV1ClientBuffer::LinuxDmaBufV1ClientBuffer(const QVector<KWaylandServer::LinuxDmaBufV1Plane> &planes,
                                                     quint32 format,
                                                     const QSize &size,
                                                     quint32 flags)
    : KWaylandServer::LinuxDmaBufV1ClientBuffer(size, format, flags, planes)
{
    waylandServer()->addLinuxDmabufBuffer(this);
}

LinuxDmaBufV1ClientBuffer::~LinuxDmaBufV1ClientBuffer()
{
    if (waylandServer()) {
        waylandServer()->removeLinuxDmabufBuffer(this);
    }
}

LinuxDmaBufV1RendererInterface::LinuxDmaBufV1RendererInterface()
{
    Q_ASSERT(waylandServer());
    waylandServer()->linuxDmabuf()->setRendererInterface(this);
}

LinuxDmaBufV1RendererInterface::~LinuxDmaBufV1RendererInterface()
{
    waylandServer()->linuxDmabuf()->setRendererInterface(nullptr);
}

KWaylandServer::LinuxDmaBufV1ClientBuffer *LinuxDmaBufV1RendererInterface::importBuffer(const QVector<KWaylandServer::LinuxDmaBufV1Plane> &planes,
                                                                                        quint32 format,
                                                                                        const QSize &size,
                                                                                        quint32 flags)
{
    Q_UNUSED(planes)
    Q_UNUSED(format)
    Q_UNUSED(size)
    Q_UNUSED(flags)

    return nullptr;
}

void LinuxDmaBufV1RendererInterface::setSupportedFormatsAndModifiers(dev_t device, const QHash<uint32_t, QSet<uint64_t>> &set)
{
    waylandServer()->linuxDmabuf()->setSupportedFormatsWithModifiers(device, set);
}

}
