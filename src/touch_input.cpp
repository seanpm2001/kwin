/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2013, 2016 Martin Gräßlin <mgraesslin@kde.org>
    SPDX-FileCopyrightText: 2018 Roman Gilg <subdiff@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "touch_input.h"
#include "abstract_client.h"
#include "pointer_input.h"
#include "input_event_spy.h"
#include "toplevel.h"
#include "wayland_server.h"
#include "workspace.h"
#include "decorations/decoratedclient.h"
// KDecoration
#include <KDecoration2/Decoration>
// KWayland
#include <KWaylandServer/seat_interface.h>
// screenlocker
#include <KScreenLocker/KsldApp>
// Qt
#include <QHoverEvent>
#include <QWindow>

namespace KWin
{

TouchInputRedirection::TouchInputRedirection(InputRedirection *parent)
    : InputDeviceHandler(parent)
{
}

TouchInputRedirection::~TouchInputRedirection() = default;

void TouchInputRedirection::init()
{
    Q_ASSERT(!inited());
    waylandServer()->seat()->setHasTouch(input()->hasTouch());
    connect(input(), &InputRedirection::hasTouchChanged,
            waylandServer()->seat(), &KWaylandServer::SeatInterface::setHasTouch);

    setInited(true);
    InputDeviceHandler::init();

    if (waylandServer()->hasScreenLockerIntegration()) {
        connect(ScreenLocker::KSldApp::self(), &ScreenLocker::KSldApp::lockStateChanged, this,
            [this] {
                cancel();
                // position doesn't matter
                update();
            }
        );
    }
    connect(workspace(), &QObject::destroyed, this, [this] { setInited(false); });
    connect(waylandServer(), &QObject::destroyed, this, [this] { setInited(false); });
}

bool TouchInputRedirection::focusUpdatesBlocked()
{
    if (m_windowUpdatedInCycle) {
        return true;
    }
    m_windowUpdatedInCycle = true;
    if (waylandServer()->seat()->isDragTouch()) {
        return true;
    }
    if (m_activeTouchPoints.count() > 1) {
        // first touch defines focus
        return true;
    }
    return false;
}

bool TouchInputRedirection::positionValid() const
{
    // we can only determine a position with at least one touch point
    return !m_activeTouchPoints.isEmpty();
}

void TouchInputRedirection::focusUpdate(Toplevel *focusOld, Toplevel *focusNow)
{
    // TODO: handle pointer grab aka popups

    if (AbstractClient *ac = qobject_cast<AbstractClient*>(focusOld)) {
        ac->pointerLeaveEvent();
    }
    disconnect(m_focusGeometryConnection);
    m_focusGeometryConnection = QMetaObject::Connection();

    if (AbstractClient *ac = qobject_cast<AbstractClient*>(focusNow)) {
        ac->pointerEnterEvent(m_lastPosition.toPoint());
    }

    auto seat = waylandServer()->seat();
    if (!focusNow || !focusNow->surface() || decoration()) {
        // no new surface or internal window or on decoration -> cleanup
        seat->setFocusedTouchSurface(nullptr);
        return;
    }

    // TODO: invalidate pointer focus?

    // FIXME: add input transformation API to KWaylandServer::SeatInterface for touch input
    seat->setFocusedTouchSurface(focusNow->surface(), -1 * focusNow->inputTransformation().map(focusNow->pos()) + focusNow->pos());
    m_focusGeometryConnection = connect(focusNow, &Toplevel::frameGeometryChanged, this,
        [this] {
            if (!focus()) {
                return;
            }
            auto seat = waylandServer()->seat();
            if (focus()->surface() != seat->focusedTouchSurface()) {
                return;
            }
            seat->setFocusedTouchSurfacePosition(-1 * focus()->inputTransformation().map(focus()->pos()) + focus()->pos());
        }
    );
}

void TouchInputRedirection::cleanupDecoration(Decoration::DecoratedClientImpl *old, Decoration::DecoratedClientImpl *now)
{
    Q_UNUSED(old);
    Q_UNUSED(now);

    // nothing to do
}

void TouchInputRedirection::processDown(qint32 id, const QPointF &pos, quint32 time, InputDevice *device)
{
    Q_UNUSED(device)
    if (!inited()) {
        return;
    }
    m_lastPosition = pos;
    m_windowUpdatedInCycle = false;
    m_activeTouchPoints.insert(id);
    if (m_activeTouchPoints.count() == 1) {
        update();
    }
    m_lastEventTime = time;
    input()->processSpies(std::bind(&InputEventSpy::touchDown, std::placeholders::_1, id, pos, time));
    input()->processFilters(std::bind(&InputEventFilter::touchDown, std::placeholders::_1, id, pos, time));
    m_windowUpdatedInCycle = false;
}

void TouchInputRedirection::processUp(qint32 id, quint32 time, InputDevice *device)
{
    Q_UNUSED(device)
    if (!inited()) {
        return;
    }
    if (!m_activeTouchPoints.remove(id)) {
        return;
    }
    m_lastEventTime = time;
    m_windowUpdatedInCycle = false;
    input()->processSpies(std::bind(&InputEventSpy::touchUp, std::placeholders::_1, id, time));
    input()->processFilters(std::bind(&InputEventFilter::touchUp, std::placeholders::_1, id, time));
    m_windowUpdatedInCycle = false;
    if (m_activeTouchPoints.count() == 0) {
        update();
    }
}

void TouchInputRedirection::processMotion(qint32 id, const QPointF &pos, quint32 time, InputDevice *device)
{
    Q_UNUSED(device)
    if (!inited()) {
        return;
    }
    if (!m_activeTouchPoints.contains(id)) {
        return;
    }
    m_lastEventTime = time;
    m_lastPosition = pos;
    m_windowUpdatedInCycle = false;
    input()->processSpies(std::bind(&InputEventSpy::touchMotion, std::placeholders::_1, id, pos, time));
    input()->processFilters(std::bind(&InputEventFilter::touchMotion, std::placeholders::_1, id, pos, time));
    m_windowUpdatedInCycle = false;
}

void TouchInputRedirection::cancel()
{
    if (!inited()) {
        return;
    }
    // If the touch sequence is artificially cancelled by the compositor, touch motion and touch
    // up events will be silently ignored and won't be passed down through the event filter chain.
    // If the touch sequence is cancelled because we received a TOUCH_CANCEL event from libinput,
    // the compositor will not receive any TOUCH_MOTION or TOUCH_UP events for that slot.
    if (!m_activeTouchPoints.isEmpty()) {
        m_activeTouchPoints.clear();
        waylandServer()->seat()->notifyTouchCancel();
    }
}

void TouchInputRedirection::frame()
{
    if (!inited() || !waylandServer()->seat()->hasTouch()) {
        return;
    }
    waylandServer()->seat()->notifyTouchFrame();
}

}
