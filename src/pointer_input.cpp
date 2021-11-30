/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2013, 2016 Martin Gräßlin <mgraesslin@kde.org>
    SPDX-FileCopyrightText: 2018 Roman Gilg <subdiff@gmail.com>
    SPDX-FileCopyrightText: 2019 Vlad Zahorodnii <vlad.zahorodnii@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "pointer_input.h"
#include "abstract_output.h"
#include "platform.h"
#include "x11client.h"
#include "effects.h"
#include "input_event.h"
#include "input_event_spy.h"
#include "osd.h"
#include "screens.h"
#include "wayland_server.h"
#include "workspace.h"
#include "decorations/decoratedclient.h"
// KDecoration
#include <KDecoration2/Decoration>
// KWayland
#include <KWaylandServer/shmclientbuffer.h>
#include <KWaylandServer/datadevice_interface.h>
#include <KWaylandServer/display.h>
#include <KWaylandServer/pointer_interface.h>
#include <KWaylandServer/pointerconstraints_v1_interface.h>
#include <KWaylandServer/seat_interface.h>
#include <KWaylandServer/surface_interface.h>
// screenlocker
#include <KScreenLocker/KsldApp>

#include <KLocalizedString>

#include <QHoverEvent>
#include <QWindow>
#include <QPainter>

#include <linux/input.h>

namespace KWin
{

static const QHash<uint32_t, Qt::MouseButton> s_buttonToQtMouseButton = {
    { BTN_LEFT , Qt::LeftButton },
    { BTN_MIDDLE , Qt::MiddleButton },
    { BTN_RIGHT , Qt::RightButton },
    // in QtWayland mapped like that
    { BTN_SIDE , Qt::ExtraButton1 },
    // in QtWayland mapped like that
    { BTN_EXTRA , Qt::ExtraButton2 },
    { BTN_BACK , Qt::BackButton },
    { BTN_FORWARD , Qt::ForwardButton },
    { BTN_TASK , Qt::TaskButton },
    // mapped like that in QtWayland
    { 0x118 , Qt::ExtraButton6 },
    { 0x119 , Qt::ExtraButton7 },
    { 0x11a , Qt::ExtraButton8 },
    { 0x11b , Qt::ExtraButton9 },
    { 0x11c , Qt::ExtraButton10 },
    { 0x11d , Qt::ExtraButton11 },
    { 0x11e , Qt::ExtraButton12 },
    { 0x11f , Qt::ExtraButton13 },
};

uint32_t qtMouseButtonToButton(Qt::MouseButton button)
{
    return s_buttonToQtMouseButton.key(button);
}

static Qt::MouseButton buttonToQtMouseButton(uint32_t button)
{
    // all other values get mapped to ExtraButton24
    // this is actually incorrect but doesn't matter in our usage
    // KWin internally doesn't use these high extra buttons anyway
    // it's only needed for recognizing whether buttons are pressed
    // if multiple buttons are mapped to the value the evaluation whether
    // buttons are pressed is correct and that's all we care about.
    return s_buttonToQtMouseButton.value(button, Qt::ExtraButton24);
}

static bool screenContainsPos(const QPointF &pos)
{
    const auto outputs = kwinApp()->platform()->enabledOutputs();
    for (const AbstractOutput *output : outputs) {
        if (output->geometry().contains(pos.toPoint())) {
            return true;
        }
    }
    return false;
}

static QPointF confineToBoundingBox(const QPointF &pos, const QRectF &boundingBox)
{
    return QPointF(
        qBound(boundingBox.left(), pos.x(), boundingBox.right() - 1.0),
        qBound(boundingBox.top(), pos.y(), boundingBox.bottom() - 1.0)
    );
}

PointerInputRedirection::PointerInputRedirection(InputRedirection* parent)
    : InputDeviceHandler(parent)
    , m_cursor(nullptr)
{
}

PointerInputRedirection::~PointerInputRedirection() = default;

void PointerInputRedirection::init()
{
    Q_ASSERT(!inited());
    waylandServer()->seat()->setHasPointer(input()->hasPointer());
    connect(input(), &InputRedirection::hasPointerChanged,
            waylandServer()->seat(), &KWaylandServer::SeatInterface::setHasPointer);

    m_cursor = new CursorImage(this);
    setInited(true);
    InputDeviceHandler::init();

    if (!input()->hasPointer()) {
        kwinApp()->platform()->hideCursor();
    }
    connect(input(), &InputRedirection::hasPointerChanged, this, [this]() {
        if (input()->hasPointer()) {
            kwinApp()->platform()->showCursor();
        } else {
            kwinApp()->platform()->hideCursor();
        }
    });

    connect(m_cursor, &CursorImage::changed, Cursors::self()->mouse(), [this] {
        auto cursor = Cursors::self()->mouse();
        cursor->updateCursor(m_cursor->image(), m_cursor->hotSpot());
        updateCursorOutputs();
    });
    Q_EMIT m_cursor->changed();

    connect(Cursors::self()->mouse(), &Cursor::rendered, m_cursor, &CursorImage::markAsRendered);

    connect(screens(), &Screens::changed, this, &PointerInputRedirection::updateAfterScreenChange);
    if (waylandServer()->hasScreenLockerIntegration()) {
        connect(ScreenLocker::KSldApp::self(), &ScreenLocker::KSldApp::lockStateChanged, this,
            [this] {
                if (waylandServer()->seat()->hasPointer()) {
                    waylandServer()->seat()->cancelPointerPinchGesture();
                    waylandServer()->seat()->cancelPointerSwipeGesture();
                }
                update();
            }
        );
    }
    connect(workspace(), &QObject::destroyed, this, [this] { setInited(false); });
    connect(waylandServer(), &QObject::destroyed, this, [this] { setInited(false); });
    connect(waylandServer()->seat(), &KWaylandServer::SeatInterface::dragEnded, this,
        [this] {
            // need to force a focused pointer change
            setFocus(nullptr);
            update();
        }
    );
    // connect the move resize of all window
    auto setupMoveResizeConnection = [this] (AbstractClient *c) {
        connect(c, &AbstractClient::clientStartUserMovedResized, this, &PointerInputRedirection::updateOnStartMoveResize);
        connect(c, &AbstractClient::clientFinishUserMovedResized, this, &PointerInputRedirection::update);
    };
    const auto clients = workspace()->allClientList();
    std::for_each(clients.begin(), clients.end(), setupMoveResizeConnection);
    connect(workspace(), &Workspace::clientAdded, this, setupMoveResizeConnection);

    // warp the cursor to center of screen containing the workspace center
    if (const AbstractOutput *output = kwinApp()->platform()->outputAt(workspace()->geometry().center())) {
        warp(output->geometry().center());
    }
    updateAfterScreenChange();
}

void PointerInputRedirection::updateOnStartMoveResize()
{
    breakPointerConstraints(focus() ? focus()->surface() : nullptr);
    disconnectPointerConstraintsConnection();
    setFocus(nullptr);
}

void PointerInputRedirection::updateToReset()
{
    if (decoration()) {
        QHoverEvent event(QEvent::HoverLeave, QPointF(), QPointF());
        QCoreApplication::instance()->sendEvent(decoration()->decoration(), &event);
        setDecoration(nullptr);
    }
    if (focus()) {
        if (AbstractClient *c = qobject_cast<AbstractClient*>(focus())) {
            c->pointerLeaveEvent();
        }
        disconnect(m_focusGeometryConnection);
        m_focusGeometryConnection = QMetaObject::Connection();
        breakPointerConstraints(focus()->surface());
        disconnectPointerConstraintsConnection();
        setFocus(nullptr);
    }
}

class PositionUpdateBlocker
{
public:
    PositionUpdateBlocker(PointerInputRedirection *pointer)
        : m_pointer(pointer)
    {
        s_counter++;
    }
    ~PositionUpdateBlocker() {
        s_counter--;
        if (s_counter == 0) {
            if (!s_scheduledPositions.isEmpty()) {
                const auto pos = s_scheduledPositions.takeFirst();
                m_pointer->processMotionInternal(pos.pos, pos.delta, pos.deltaNonAccelerated, pos.time, pos.timeUsec, nullptr);
            }
        }
    }

    static bool isPositionBlocked() {
        return s_counter > 0;
    }

    static void schedulePosition(const QPointF &pos, const QSizeF &delta, const QSizeF &deltaNonAccelerated, uint32_t time, quint64 timeUsec) {
        s_scheduledPositions.append({pos, delta, deltaNonAccelerated, time, timeUsec});
    }

private:
    static int s_counter;
    struct ScheduledPosition {
        QPointF pos;
        QSizeF delta;
        QSizeF deltaNonAccelerated;
        quint32 time;
        quint64 timeUsec;
    };
    static QVector<ScheduledPosition> s_scheduledPositions;

    PointerInputRedirection *m_pointer;
};

int PositionUpdateBlocker::s_counter = 0;
QVector<PositionUpdateBlocker::ScheduledPosition> PositionUpdateBlocker::s_scheduledPositions;

void PointerInputRedirection::processMotionAbsolute(const QPointF &pos, uint32_t time, InputDevice *device)
{
    processMotionInternal(pos, QSizeF(), QSizeF(), time, 0, device);
}

void PointerInputRedirection::processMotion(const QSizeF &delta, const QSizeF &deltaNonAccelerated, uint32_t time, quint64 timeUsec, InputDevice *device)
{
    processMotionInternal(m_pos + QPointF(delta.width(), delta.height()), delta, deltaNonAccelerated, time, timeUsec, device);
}

void PointerInputRedirection::processMotionInternal(const QPointF &pos, const QSizeF &delta, const QSizeF &deltaNonAccelerated, uint32_t time, quint64 timeUsec, InputDevice *device)
{
    m_lastEventTime = time;
    if (!inited()) {
        return;
    }
    if (PositionUpdateBlocker::isPositionBlocked()) {
        PositionUpdateBlocker::schedulePosition(pos, delta, deltaNonAccelerated, time, timeUsec);
        return;
    }

    PositionUpdateBlocker blocker(this);
    updatePosition(pos);
    MouseEvent event(QEvent::MouseMove, m_pos, Qt::NoButton, m_qtButtons,
                     input()->keyboardModifiers(), time,
                     delta, deltaNonAccelerated, timeUsec, device);
    event.setModifiersRelevantForGlobalShortcuts(input()->modifiersRelevantForGlobalShortcuts());

    update();
    input()->processSpies(std::bind(&InputEventSpy::pointerEvent, std::placeholders::_1, &event));
    input()->processFilters(std::bind(&InputEventFilter::pointerEvent, std::placeholders::_1, &event, 0));
}

void PointerInputRedirection::processButton(uint32_t button, InputRedirection::PointerButtonState state, uint32_t time, InputDevice *device)
{
    m_lastEventTime = time;
    QEvent::Type type;
    switch (state) {
    case InputRedirection::PointerButtonReleased:
        type = QEvent::MouseButtonRelease;
        break;
    case InputRedirection::PointerButtonPressed:
        type = QEvent::MouseButtonPress;
        update();
        break;
    default:
        Q_UNREACHABLE();
        return;
    }

    updateButton(button, state);

    MouseEvent event(type, m_pos, buttonToQtMouseButton(button), m_qtButtons,
                     input()->keyboardModifiers(), time, QSizeF(), QSizeF(), 0, device);
    event.setModifiersRelevantForGlobalShortcuts(input()->modifiersRelevantForGlobalShortcuts());
    event.setNativeButton(button);

    input()->processSpies(std::bind(&InputEventSpy::pointerEvent, std::placeholders::_1, &event));

    if (!inited()) {
        return;
    }

    input()->processFilters(std::bind(&InputEventFilter::pointerEvent, std::placeholders::_1, &event, button));

    if (state == InputRedirection::PointerButtonReleased) {
        update();
    }
}

void PointerInputRedirection::processAxis(InputRedirection::PointerAxis axis, qreal delta, qint32 discreteDelta,
    InputRedirection::PointerAxisSource source, uint32_t time, InputDevice *device)
{
    m_lastEventTime = time;
    update();

    Q_EMIT input()->pointerAxisChanged(axis, delta);

    WheelEvent wheelEvent(m_pos, delta, discreteDelta,
                           (axis == InputRedirection::PointerAxisHorizontal) ? Qt::Horizontal : Qt::Vertical,
                           m_qtButtons, input()->keyboardModifiers(), source, time, device);
    wheelEvent.setModifiersRelevantForGlobalShortcuts(input()->modifiersRelevantForGlobalShortcuts());

    input()->processSpies(std::bind(&InputEventSpy::wheelEvent, std::placeholders::_1, &wheelEvent));

    if (!inited()) {
        return;
    }
    input()->processFilters(std::bind(&InputEventFilter::wheelEvent, std::placeholders::_1, &wheelEvent));
}

void PointerInputRedirection::processSwipeGestureBegin(int fingerCount, quint32 time, KWin::InputDevice *device)
{
    m_lastEventTime = time;
    Q_UNUSED(device)
    if (!inited()) {
        return;
    }

    input()->processSpies(std::bind(&InputEventSpy::swipeGestureBegin, std::placeholders::_1, fingerCount, time));
    input()->processFilters(std::bind(&InputEventFilter::swipeGestureBegin, std::placeholders::_1, fingerCount, time));
}

void PointerInputRedirection::processSwipeGestureUpdate(const QSizeF &delta, quint32 time, KWin::InputDevice *device)
{
    m_lastEventTime = time;
    Q_UNUSED(device)
    if (!inited()) {
        return;
    }
    update();

    input()->processSpies(std::bind(&InputEventSpy::swipeGestureUpdate, std::placeholders::_1, delta, time));
    input()->processFilters(std::bind(&InputEventFilter::swipeGestureUpdate, std::placeholders::_1, delta, time));
}

void PointerInputRedirection::processSwipeGestureEnd(quint32 time, KWin::InputDevice *device)
{
    m_lastEventTime = time;
    Q_UNUSED(device)
    if (!inited()) {
        return;
    }
    update();

    input()->processSpies(std::bind(&InputEventSpy::swipeGestureEnd, std::placeholders::_1, time));
    input()->processFilters(std::bind(&InputEventFilter::swipeGestureEnd, std::placeholders::_1, time));
}

void PointerInputRedirection::processSwipeGestureCancelled(quint32 time, KWin::InputDevice *device)
{
    m_lastEventTime = time;
    Q_UNUSED(device)
    if (!inited()) {
        return;
    }
    update();

    input()->processSpies(std::bind(&InputEventSpy::swipeGestureCancelled, std::placeholders::_1, time));
    input()->processFilters(std::bind(&InputEventFilter::swipeGestureCancelled, std::placeholders::_1, time));
}

void PointerInputRedirection::processPinchGestureBegin(int fingerCount, quint32 time, KWin::InputDevice *device)
{
    m_lastEventTime = time;
    Q_UNUSED(device)
    if (!inited()) {
        return;
    }
    update();

    input()->processSpies(std::bind(&InputEventSpy::pinchGestureBegin, std::placeholders::_1, fingerCount, time));
    input()->processFilters(std::bind(&InputEventFilter::pinchGestureBegin, std::placeholders::_1, fingerCount, time));
}

void PointerInputRedirection::processPinchGestureUpdate(qreal scale, qreal angleDelta, const QSizeF &delta, quint32 time, KWin::InputDevice *device)
{
    m_lastEventTime = time;
    Q_UNUSED(device)
    if (!inited()) {
        return;
    }
    update();

    input()->processSpies(std::bind(&InputEventSpy::pinchGestureUpdate, std::placeholders::_1, scale, angleDelta, delta, time));
    input()->processFilters(std::bind(&InputEventFilter::pinchGestureUpdate, std::placeholders::_1, scale, angleDelta, delta, time));
}

void PointerInputRedirection::processPinchGestureEnd(quint32 time, KWin::InputDevice *device)
{
    m_lastEventTime = time;
    Q_UNUSED(device)
    if (!inited()) {
        return;
    }
    update();

    input()->processSpies(std::bind(&InputEventSpy::pinchGestureEnd, std::placeholders::_1, time));
    input()->processFilters(std::bind(&InputEventFilter::pinchGestureEnd, std::placeholders::_1, time));
}

void PointerInputRedirection::processPinchGestureCancelled(quint32 time, KWin::InputDevice *device)
{
    m_lastEventTime = time;
    Q_UNUSED(device)
    if (!inited()) {
        return;
    }
    update();

    input()->processSpies(std::bind(&InputEventSpy::pinchGestureCancelled, std::placeholders::_1, time));
    input()->processFilters(std::bind(&InputEventFilter::pinchGestureCancelled, std::placeholders::_1, time));
}

void PointerInputRedirection::processHoldGestureBegin(int fingerCount, quint32 time, KWin::InputDevice *device)
{
    Q_UNUSED(device)
    if (!inited()) {
        return;
    }
    update();

    input()->processSpies(std::bind(&InputEventSpy::holdGestureBegin, std::placeholders::_1, fingerCount, time));
    input()->processFilters(std::bind(&InputEventFilter::holdGestureBegin, std::placeholders::_1, fingerCount, time));
}

void PointerInputRedirection::processHoldGestureEnd(quint32 time, KWin::InputDevice *device)
{
    Q_UNUSED(device)
    if (!inited()) {
        return;
    }
    update();

    input()->processSpies(std::bind(&InputEventSpy::holdGestureEnd, std::placeholders::_1, time));
    input()->processFilters(std::bind(&InputEventFilter::holdGestureEnd, std::placeholders::_1, time));
}

void PointerInputRedirection::processHoldGestureCancelled(quint32 time, KWin::InputDevice *device)
{
    Q_UNUSED(device)
    if (!inited()) {
        return;
    }
    update();

    input()->processSpies(std::bind(&InputEventSpy::holdGestureCancelled, std::placeholders::_1, time));
    input()->processFilters(std::bind(&InputEventFilter::holdGestureCancelled, std::placeholders::_1, time));
}

bool PointerInputRedirection::areButtonsPressed() const
{
    for (auto state : m_buttons) {
        if (state == InputRedirection::PointerButtonPressed) {
            return true;
        }
    }
    return false;
}

bool PointerInputRedirection::focusUpdatesBlocked()
{
    if (waylandServer()->seat()->isDragPointer()) {
        // ignore during drag and drop
        return true;
    }
    if (waylandServer()->seat()->isTouchSequence()) {
        // ignore during touch operations
        return true;
    }
    if (input()->isSelectingWindow()) {
        return true;
    }
    if (areButtonsPressed()) {
        return true;
    }
    return false;
}

void PointerInputRedirection::cleanupDecoration(Decoration::DecoratedClientImpl *old, Decoration::DecoratedClientImpl *now)
{
    disconnect(m_decorationGeometryConnection);
    m_decorationGeometryConnection = QMetaObject::Connection();

    disconnect(m_decorationDestroyedConnection);
    m_decorationDestroyedConnection = QMetaObject::Connection();

    if (old) {
        // send leave event to old decoration
        QHoverEvent event(QEvent::HoverLeave, QPointF(), QPointF());
        QCoreApplication::instance()->sendEvent(old->decoration(), &event);
    }
    if (!now) {
        // left decoration
        return;
    }

    auto pos = m_pos - now->client()->pos();
    QHoverEvent event(QEvent::HoverEnter, pos, pos);
    QCoreApplication::instance()->sendEvent(now->decoration(), &event);
    now->client()->processDecorationMove(pos.toPoint(), m_pos.toPoint());

    m_decorationGeometryConnection = connect(decoration()->client(), &AbstractClient::frameGeometryChanged, this,
        [this] {
            // ensure maximize button gets the leave event when maximizing/restore a window, see BUG 385140
            const auto oldDeco = decoration();
            update();
            if (oldDeco &&
                    oldDeco == decoration() &&
                    !decoration()->client()->isInteractiveMove() &&
                    !decoration()->client()->isInteractiveResize() &&
                    !areButtonsPressed()) {
                // position of window did not change, we need to send HoverMotion manually
                const QPointF p = m_pos - decoration()->client()->pos();
                QHoverEvent event(QEvent::HoverMove, p, p);
                QCoreApplication::instance()->sendEvent(decoration()->decoration(), &event);
            }
        }, Qt::QueuedConnection);

    // if our decoration gets destroyed whilst it has focus, we pass focus on to the same client
    m_decorationDestroyedConnection = connect(now, &QObject::destroyed, this, &PointerInputRedirection::update, Qt::QueuedConnection);
}

void PointerInputRedirection::focusUpdate(Toplevel *focusOld, Toplevel *focusNow)
{
    if (AbstractClient *ac = qobject_cast<AbstractClient*>(focusOld)) {
        ac->pointerLeaveEvent();
        breakPointerConstraints(ac->surface());
        disconnectPointerConstraintsConnection();
    }
    disconnect(m_focusGeometryConnection);
    m_focusGeometryConnection = QMetaObject::Connection();

    if (AbstractClient *ac = qobject_cast<AbstractClient*>(focusNow)) {
        ac->pointerEnterEvent(m_pos.toPoint());
    }

    auto seat = waylandServer()->seat();
    if (!focusNow || !focusNow->surface() || decoration()) {
        // Clean up focused pointer surface if there's no client to take focus,
        // or the pointer is on a client without surface or on a decoration.
        warpXcbOnSurfaceLeft(nullptr);
        seat->setFocusedPointerSurface(nullptr);
        return;
    }

    // TODO: add convenient API to update global pos together with updating focused surface
    warpXcbOnSurfaceLeft(focusNow->surface());

    seat->notifyPointerMotion(m_pos.toPoint());
    seat->setFocusedPointerSurface(focusNow->surface(), focusNow->inputTransformation());

    m_focusGeometryConnection = connect(focusNow, &Toplevel::inputTransformationChanged, this,
        [this] {
            // TODO: why no assert possible?
            if (!focus()) {
                return;
            }
            // TODO: can we check on the client instead?
            if (workspace()->moveResizeClient()) {
                // don't update while moving
                return;
            }
            auto seat = waylandServer()->seat();
            if (focus()->surface() != seat->focusedPointerSurface()) {
                return;
            }
            seat->setFocusedPointerSurfaceTransformation(focus()->inputTransformation());
        }
    );

    m_constraintsConnection = connect(focusNow->surface(), &KWaylandServer::SurfaceInterface::pointerConstraintsChanged,
                                      this, &PointerInputRedirection::updatePointerConstraints);
    m_constraintsActivatedConnection = connect(workspace(), &Workspace::clientActivated,
                                               this, &PointerInputRedirection::updatePointerConstraints);
    updatePointerConstraints();
}

void PointerInputRedirection::breakPointerConstraints(KWaylandServer::SurfaceInterface *surface)
{
    // cancel pointer constraints
    if (surface) {
        auto c = surface->confinedPointer();
        if (c && c->isConfined()) {
            c->setConfined(false);
        }
        auto l = surface->lockedPointer();
        if (l && l->isLocked()) {
            l->setLocked(false);
        }
    }
    disconnectConfinedPointerRegionConnection();
    m_confined = false;
    m_locked = false;
}

void PointerInputRedirection::disconnectConfinedPointerRegionConnection()
{
    disconnect(m_confinedPointerRegionConnection);
    m_confinedPointerRegionConnection = QMetaObject::Connection();
}

void PointerInputRedirection::disconnectLockedPointerAboutToBeUnboundConnection()
{
    disconnect(m_lockedPointerAboutToBeUnboundConnection);
    m_lockedPointerAboutToBeUnboundConnection = QMetaObject::Connection();
}

void PointerInputRedirection::disconnectPointerConstraintsConnection()
{
    disconnect(m_constraintsConnection);
    m_constraintsConnection = QMetaObject::Connection();

    disconnect(m_constraintsActivatedConnection);
    m_constraintsActivatedConnection = QMetaObject::Connection();
}

template <typename T>
static QRegion getConstraintRegion(Toplevel *t, T *constraint)
{
    const QRegion windowShape = t->inputShape();
    const QRegion intersected = constraint->region().isEmpty() ? windowShape : windowShape.intersected(constraint->region());
    return intersected.translated(t->pos() + t->clientPos());
}

void PointerInputRedirection::setEnableConstraints(bool set)
{
    if (m_enableConstraints == set) {
        return;
    }
    m_enableConstraints = set;
    updatePointerConstraints();
}

void PointerInputRedirection::updatePointerConstraints()
{
    if (!focus()) {
        return;
    }
    const auto s = focus()->surface();
    if (!s) {
        return;
    }
    if (s != waylandServer()->seat()->focusedPointerSurface()) {
        return;
    }
    if (!supportsWarping()) {
        return;
    }
    const bool canConstrain = m_enableConstraints && focus() == workspace()->activeClient();
    const auto cf = s->confinedPointer();
    if (cf) {
        if (cf->isConfined()) {
            if (!canConstrain) {
                cf->setConfined(false);
                m_confined = false;
                disconnectConfinedPointerRegionConnection();
            }
            return;
        }
        const QRegion r = getConstraintRegion(focus(), cf);
        if (canConstrain && r.contains(m_pos.toPoint())) {
            cf->setConfined(true);
            m_confined = true;
            m_confinedPointerRegionConnection = connect(cf, &KWaylandServer::ConfinedPointerV1Interface::regionChanged, this,
                [this] {
                    if (!focus()) {
                        return;
                    }
                    const auto s = focus()->surface();
                    if (!s) {
                        return;
                    }
                    const auto cf = s->confinedPointer();
                    if (!getConstraintRegion(focus(), cf).contains(m_pos.toPoint())) {
                        // pointer no longer in confined region, break the confinement
                        cf->setConfined(false);
                        m_confined = false;
                    } else {
                        if (!cf->isConfined()) {
                            cf->setConfined(true);
                            m_confined = true;
                        }
                    }
                }
            );
            return;
        }
    } else {
        m_confined = false;
        disconnectConfinedPointerRegionConnection();
    }
    const auto lock = s->lockedPointer();
    if (lock) {
        if (lock->isLocked()) {
            if (!canConstrain) {
                const auto hint = lock->cursorPositionHint();
                lock->setLocked(false);
                m_locked = false;
                disconnectLockedPointerAboutToBeUnboundConnection();
                if (! (hint.x() < 0 || hint.y() < 0) && focus()) {
                    processMotionAbsolute(focus()->mapFromLocal(hint), waylandServer()->seat()->timestamp());
                }
            }
            return;
        }
        const QRegion r = getConstraintRegion(focus(), lock);
        if (canConstrain && r.contains(m_pos.toPoint())) {
            lock->setLocked(true);
            m_locked = true;

            // The client might cancel pointer locking from its side by unbinding the LockedPointerInterface.
            // In this case the cached cursor position hint must be fetched before the resource goes away
            m_lockedPointerAboutToBeUnboundConnection = connect(lock, &KWaylandServer::LockedPointerV1Interface::aboutToBeDestroyed, this,
                [this, lock]() {
                    const auto hint = lock->cursorPositionHint();
                    if (hint.x() < 0 || hint.y() < 0 || !focus()) {
                        return;
                    }
                    auto globalHint = focus()->mapFromLocal(hint);

                    // When the resource finally goes away, reposition the cursor according to the hint
                    connect(lock, &KWaylandServer::LockedPointerV1Interface::destroyed, this,
                        [this, globalHint]() {
                            processMotionAbsolute(globalHint, waylandServer()->seat()->timestamp());
                    });
                }
            );
            // TODO: connect to region change - is it needed at all? If the pointer is locked it's always in the region
        }
    } else {
        m_locked = false;
        disconnectLockedPointerAboutToBeUnboundConnection();
    }
}

void PointerInputRedirection::warpXcbOnSurfaceLeft(KWaylandServer::SurfaceInterface *newSurface)
{
    auto xc = waylandServer()->xWaylandConnection();
    if (!xc) {
        // No XWayland, no point in warping the x cursor
        return;
    }
    const auto c = kwinApp()->x11Connection();
    if (!c) {
        return;
    }
    static bool s_hasXWayland119 = xcb_get_setup(c)->release_number >= 11900000;
    if (s_hasXWayland119) {
        return;
    }
    if (newSurface && newSurface->client() == xc) {
        // new window is an X window
        return;
    }
    auto s = waylandServer()->seat()->focusedPointerSurface();
    if (!s || s->client() != xc) {
        // pointer was not on an X window
        return;
    }
    // warp pointer to 0/0 to trigger leave events on previously focused X window
    xcb_warp_pointer(c, XCB_WINDOW_NONE, kwinApp()->x11RootWindow(), 0, 0, 0, 0, 0, 0),
    xcb_flush(c);
}

QPointF PointerInputRedirection::applyPointerConfinement(const QPointF &pos) const
{
    if (!focus()) {
        return pos;
    }
    auto s = focus()->surface();
    if (!s) {
        return pos;
    }
    auto cf = s->confinedPointer();
    if (!cf) {
        return pos;
    }
    if (!cf->isConfined()) {
        return pos;
    }

    const QRegion confinementRegion = getConstraintRegion(focus(), cf);
    if (confinementRegion.contains(pos.toPoint())) {
        return pos;
    }
    QPointF p = pos;
    // allow either x or y to pass
    p = QPointF(m_pos.x(), pos.y());
    if (confinementRegion.contains(p.toPoint())) {
        return p;
    }
    p = QPointF(pos.x(), m_pos.y());
    if (confinementRegion.contains(p.toPoint())) {
        return p;
    }

    return m_pos;
}

void PointerInputRedirection::updatePosition(const QPointF &pos)
{
    if (m_locked) {
        // locked pointer should not move
        return;
    }
    // verify that at least one screen contains the pointer position
    QPointF p = pos;
    if (!screenContainsPos(p)) {
        const QRectF unitedScreensGeometry = workspace()->geometry();
        p = confineToBoundingBox(p, unitedScreensGeometry);
        if (!screenContainsPos(p)) {
            const AbstractOutput *currentOutput = kwinApp()->platform()->outputAt(m_pos.toPoint());
            p = confineToBoundingBox(p, currentOutput->geometry());
        }
    }
    p = applyPointerConfinement(p);
    if (p == m_pos) {
        // didn't change due to confinement
        return;
    }
    // verify screen confinement
    if (!screenContainsPos(p)) {
        return;
    }

    m_pos = p;

    updateCursorOutputs();

    Q_EMIT input()->globalPointerChanged(m_pos);
}

void PointerInputRedirection::updateCursorOutputs()
{
    KWaylandServer::PointerInterface *pointer = waylandServer()->seat()->pointer();
    if (!pointer) {
        return;
    }

    KWaylandServer::Cursor *cursor = pointer->cursor();
    if (!cursor) {
        return;
    }

    KWaylandServer::SurfaceInterface *surface = cursor->surface();
    if (!surface) {
        return;
    }

    const QRectF cursorGeometry(m_pos - m_cursor->hotSpot(), surface->size());
    surface->setOutputs(waylandServer()->display()->outputsIntersecting(cursorGeometry.toAlignedRect()));
}

void PointerInputRedirection::updateButton(uint32_t button, InputRedirection::PointerButtonState state)
{
    m_buttons[button] = state;

    // update Qt buttons
    m_qtButtons = Qt::NoButton;
    for (auto it = m_buttons.constBegin(); it != m_buttons.constEnd(); ++it) {
        if (it.value() == InputRedirection::PointerButtonReleased) {
            continue;
        }
        m_qtButtons |= buttonToQtMouseButton(it.key());
    }

    Q_EMIT input()->pointerButtonStateChanged(button, state);
}

void PointerInputRedirection::warp(const QPointF &pos)
{
    if (supportsWarping()) {
        kwinApp()->platform()->warpPointer(pos);
        processMotionAbsolute(pos, waylandServer()->seat()->timestamp());
    }
}

bool PointerInputRedirection::supportsWarping() const
{
    if (!inited()) {
        return false;
    }
    return kwinApp()->platform()->supportsPointerWarping();
}

void PointerInputRedirection::updateAfterScreenChange()
{
    if (!inited()) {
        return;
    }
    if (screenContainsPos(m_pos)) {
        // pointer still on a screen
        return;
    }
    // pointer no longer on a screen, reposition to closes screen
    const AbstractOutput *output = kwinApp()->platform()->outputAt(m_pos.toPoint());
    // TODO: better way to get timestamps
    processMotionAbsolute(output->geometry().center(), waylandServer()->seat()->timestamp());
}

QPointF PointerInputRedirection::position() const
{
    return m_pos.toPoint();
}

void PointerInputRedirection::setEffectsOverrideCursor(Qt::CursorShape shape)
{
    if (!inited()) {
        return;
    }
    // current pointer focus window should get a leave event
    update();
    m_cursor->setEffectsOverrideCursor(shape);
}

void PointerInputRedirection::removeEffectsOverrideCursor()
{
    if (!inited()) {
        return;
    }
    // cursor position might have changed while there was an effect in place
    update();
    m_cursor->removeEffectsOverrideCursor();
}

void PointerInputRedirection::setWindowSelectionCursor(const QByteArray &shape)
{
    if (!inited()) {
        return;
    }
    // send leave to current pointer focus window
    updateToReset();
    m_cursor->setWindowSelectionCursor(shape);
}

void PointerInputRedirection::removeWindowSelectionCursor()
{
    if (!inited()) {
        return;
    }
    update();
    m_cursor->removeWindowSelectionCursor();
}

CursorImage::CursorImage(PointerInputRedirection *parent)
    : QObject(parent)
    , m_pointer(parent)
{
    connect(waylandServer()->seat(), &KWaylandServer::SeatInterface::hasPointerChanged,
            this, &CursorImage::handlePointerChanged);
    connect(waylandServer()->seat(), &KWaylandServer::SeatInterface::dragStarted, this, &CursorImage::updateDrag);
    connect(waylandServer()->seat(), &KWaylandServer::SeatInterface::dragEnded, this,
        [this] {
            disconnect(m_drag.connection);
            reevaluteSource();
        }
    );
    if (waylandServer()->hasScreenLockerIntegration()) {
        connect(ScreenLocker::KSldApp::self(), &ScreenLocker::KSldApp::lockStateChanged, this, &CursorImage::reevaluteSource);
    }
    connect(m_pointer, &PointerInputRedirection::decorationChanged, this, &CursorImage::updateDecoration);
    // connect the move resize of all window
    auto setupMoveResizeConnection = [this] (AbstractClient *c) {
        connect(c, &AbstractClient::moveResizedChanged, this, &CursorImage::updateMoveResize);
        connect(c, &AbstractClient::moveResizeCursorChanged, this, &CursorImage::updateMoveResize);
    };
    const auto clients = workspace()->allClientList();
    std::for_each(clients.begin(), clients.end(), setupMoveResizeConnection);
    connect(workspace(), &Workspace::clientAdded, this, setupMoveResizeConnection);
    loadThemeCursor(Qt::ArrowCursor, &m_fallbackCursor);

    m_surfaceRenderedTimer.start();

    connect(&m_waylandImage, &WaylandCursorImage::themeChanged, this, [this] {
        loadThemeCursor(Qt::ArrowCursor, &m_fallbackCursor);
        updateDecorationCursor();
        updateMoveResize();
        // TODO: update effects
    });

    handlePointerChanged();
}

CursorImage::~CursorImage() = default;

void CursorImage::markAsRendered()
{
    if (m_currentSource == CursorSource::DragAndDrop) {
        // always sending a frame rendered to the drag icon surface to not freeze QtWayland (see https://bugreports.qt.io/browse/QTBUG-51599 )
        if (const KWaylandServer::DragAndDropIcon *icon = waylandServer()->seat()->dragIcon()) {
            icon->surface()->frameRendered(m_surfaceRenderedTimer.elapsed());
        }
    }
    if (m_currentSource != CursorSource::LockScreen
            && m_currentSource != CursorSource::PointerSurface
            && m_currentSource != CursorSource::DragAndDrop) {
        return;
    }
    auto p = waylandServer()->seat()->pointer();
    if (!p) {
        return;
    }
    auto c = p->cursor();
    if (!c) {
        return;
    }
    auto cursorSurface = c->surface();
    if (!cursorSurface) {
        return;
    }
    cursorSurface->frameRendered(m_surfaceRenderedTimer.elapsed());
}

void CursorImage::handlePointerChanged()
{
    KWaylandServer::PointerInterface *pointer = waylandServer()->seat()->pointer();

    if (pointer) {
        connect(pointer, &KWaylandServer::PointerInterface::focusedSurfaceChanged,
                this, &CursorImage::handleFocusedSurfaceChanged);
    }
}

void CursorImage::handleFocusedSurfaceChanged()
{
    KWaylandServer::PointerInterface *pointer = waylandServer()->seat()->pointer();
    disconnect(m_serverCursor.connection);

    if (pointer->focusedSurface()) {
        m_serverCursor.connection = connect(pointer, &KWaylandServer::PointerInterface::cursorChanged,
                                            this, &CursorImage::updateServerCursor);
    } else {
        m_serverCursor.connection = QMetaObject::Connection();
        reevaluteSource();
    }
}

void CursorImage::updateDecoration()
{
    disconnect(m_decorationConnection);
    auto deco = m_pointer->decoration();
    AbstractClient *c = deco ? deco->client() : nullptr;
    if (c) {
        m_decorationConnection = connect(c, &AbstractClient::moveResizeCursorChanged, this, &CursorImage::updateDecorationCursor);
    } else {
        m_decorationConnection = QMetaObject::Connection();
    }
    updateDecorationCursor();
}

void CursorImage::updateDecorationCursor()
{
    m_decorationCursor = {};
    auto deco = m_pointer->decoration();
    if (AbstractClient *c = deco ? deco->client() : nullptr) {
        loadThemeCursor(c->cursor(), &m_decorationCursor);
        if (m_currentSource == CursorSource::Decoration) {
            Q_EMIT changed();
        }
    }
    reevaluteSource();
}

void CursorImage::updateMoveResize()
{
    m_moveResizeCursor = {};
    if (AbstractClient *c = workspace()->moveResizeClient()) {
        loadThemeCursor(c->cursor(), &m_moveResizeCursor);
        if (m_currentSource == CursorSource::MoveResize) {
            Q_EMIT changed();
        }
    }
    reevaluteSource();
}

void CursorImage::updateServerCursor()
{
    m_serverCursor.cursor = {};
    reevaluteSource();
    const bool needsEmit = m_currentSource == CursorSource::LockScreen || m_currentSource == CursorSource::PointerSurface;
    auto p = waylandServer()->seat()->pointer();
    if (!p) {
        if (needsEmit) {
            Q_EMIT changed();
        }
        return;
    }
    auto c = p->cursor();
    if (!c) {
        if (needsEmit) {
            Q_EMIT changed();
        }
        return;
    }
    auto cursorSurface = c->surface();
    if (!cursorSurface) {
        if (needsEmit) {
            Q_EMIT changed();
        }
        return;
    }
    auto buffer = qobject_cast<KWaylandServer::ShmClientBuffer *>(cursorSurface->buffer());
    if (!buffer) {
        if (needsEmit) {
            Q_EMIT changed();
        }
        return;
    }
    m_serverCursor.cursor.hotspot = c->hotspot();
    m_serverCursor.cursor.image = buffer->data().copy();
    m_serverCursor.cursor.image.setDevicePixelRatio(cursorSurface->bufferScale());
    if (needsEmit) {
        Q_EMIT changed();
    }
}

void CursorImage::setEffectsOverrideCursor(Qt::CursorShape shape)
{
    loadThemeCursor(shape, &m_effectsCursor);
    if (m_currentSource == CursorSource::EffectsOverride) {
        Q_EMIT changed();
    }
    reevaluteSource();
}

void CursorImage::removeEffectsOverrideCursor()
{
    reevaluteSource();
}

void CursorImage::setWindowSelectionCursor(const QByteArray &shape)
{
    if (shape.isEmpty()) {
        loadThemeCursor(Qt::CrossCursor, &m_windowSelectionCursor);
    } else {
        loadThemeCursor(shape, &m_windowSelectionCursor);
    }
    if (m_currentSource == CursorSource::WindowSelector) {
        Q_EMIT changed();
    }
    reevaluteSource();
}

void CursorImage::removeWindowSelectionCursor()
{
    reevaluteSource();
}

void CursorImage::updateDrag()
{
    using namespace KWaylandServer;
    disconnect(m_drag.connection);
    m_drag.cursor = {};
    reevaluteSource();
    if (waylandServer()->seat()->isDragPointer()) {
        KWaylandServer::PointerInterface *pointer = waylandServer()->seat()->pointer();
        m_drag.connection = connect(pointer, &PointerInterface::cursorChanged, this, &CursorImage::updateDragCursor);
    } else {
        m_drag.connection = QMetaObject::Connection();
    }
    updateDragCursor();
}

void CursorImage::updateDragCursor()
{
    m_drag.cursor = {};
    const bool needsEmit = m_currentSource == CursorSource::DragAndDrop;
    QImage additionalIcon;
    if (const KWaylandServer::DragAndDropIcon *dragIcon = waylandServer()->seat()->dragIcon()) {
        if (auto buffer = qobject_cast<KWaylandServer::ShmClientBuffer *>(dragIcon->surface()->buffer())) {
            additionalIcon = buffer->data().copy();
            additionalIcon.setDevicePixelRatio(dragIcon->surface()->bufferScale());
            additionalIcon.setOffset(dragIcon->position());
        }
    }
    auto p = waylandServer()->seat()->pointer();
    if (!p) {
        if (needsEmit) {
            Q_EMIT changed();
        }
        return;
    }
    auto c = p->cursor();
    if (!c) {
        if (needsEmit) {
            Q_EMIT changed();
        }
        return;
    }
    auto cursorSurface = c->surface();
    if (!cursorSurface) {
        if (needsEmit) {
            Q_EMIT changed();
        }
        return;
    }
    auto buffer = qobject_cast<KWaylandServer::ShmClientBuffer *>(cursorSurface->buffer());
    if (!buffer) {
        if (needsEmit) {
            Q_EMIT changed();
        }
        return;
    }

    QImage cursorImage = buffer->data();
    cursorImage.setDevicePixelRatio(cursorSurface->bufferScale());

    if (additionalIcon.isNull()) {
        m_drag.cursor.image = cursorImage.copy();
        m_drag.cursor.hotspot = c->hotspot();
    } else {
        QRect cursorRect(QPoint(0, 0), cursorImage.size() / cursorImage.devicePixelRatio());
        QRect iconRect(QPoint(0, 0), additionalIcon.size() / additionalIcon.devicePixelRatio());

        if (-c->hotspot().x() < additionalIcon.offset().x()) {
            iconRect.moveLeft(c->hotspot().x() - additionalIcon.offset().x());
        } else {
            cursorRect.moveLeft(-additionalIcon.offset().x() - c->hotspot().x());
        }
        if (-c->hotspot().y() < additionalIcon.offset().y()) {
            iconRect.moveTop(c->hotspot().y() - additionalIcon.offset().y());
        } else {
            cursorRect.moveTop(-additionalIcon.offset().y() - c->hotspot().y());
        }

        const QRect viewport = cursorRect.united(iconRect);
        const qreal scale = cursorSurface->bufferScale();

        m_drag.cursor.image = QImage(viewport.size() * scale, QImage::Format_ARGB32_Premultiplied);
        m_drag.cursor.image.setDevicePixelRatio(scale);
        m_drag.cursor.image.fill(Qt::transparent);
        m_drag.cursor.hotspot = cursorRect.topLeft() + c->hotspot();

        QPainter p(&m_drag.cursor.image);
        p.drawImage(iconRect, additionalIcon);
        p.drawImage(cursorRect, cursorImage);
        p.end();
    }

    if (needsEmit) {
        Q_EMIT changed();
    }
    // TODO: add the cursor image
}

void CursorImage::loadThemeCursor(CursorShape shape, WaylandCursorImage::Image *image)
{
    m_waylandImage.loadThemeCursor(shape, image);
}

void CursorImage::loadThemeCursor(const QByteArray &shape, WaylandCursorImage::Image *image)
{
    m_waylandImage.loadThemeCursor(shape, image);
}

WaylandCursorImage::WaylandCursorImage(QObject *parent)
    : QObject(parent)
{
    Cursor *pointerCursor = Cursors::self()->mouse();

    connect(pointerCursor, &Cursor::themeChanged, this, &WaylandCursorImage::invalidateCursorTheme);
    connect(screens(), &Screens::maxScaleChanged, this, &WaylandCursorImage::invalidateCursorTheme);
}

bool WaylandCursorImage::ensureCursorTheme()
{
    if (!m_cursorTheme.isEmpty()) {
        return true;
    }

    const Cursor *pointerCursor = Cursors::self()->mouse();
    const qreal targetDevicePixelRatio = screens()->maxScale();

    m_cursorTheme = KXcursorTheme::fromTheme(pointerCursor->themeName(), pointerCursor->themeSize(),
                                             targetDevicePixelRatio);
    if (!m_cursorTheme.isEmpty()) {
        return true;
    }

    m_cursorTheme = KXcursorTheme::fromTheme(Cursor::defaultThemeName(), Cursor::defaultThemeSize(),
                                             targetDevicePixelRatio);
    if (!m_cursorTheme.isEmpty()) {
        return true;
    }

    return false;
}

void WaylandCursorImage::invalidateCursorTheme()
{
    m_cursorTheme = KXcursorTheme();
}

void WaylandCursorImage::loadThemeCursor(const CursorShape &shape, Image *cursorImage)
{
    loadThemeCursor(shape.name(), cursorImage);
}

void WaylandCursorImage::loadThemeCursor(const QByteArray &name, Image *cursorImage)
{
    if (!ensureCursorTheme()) {
        return;
    }

    if (loadThemeCursor_helper(name, cursorImage)) {
        return;
    }

    const auto alternativeNames = Cursor::cursorAlternativeNames(name);
    for (const QByteArray &alternativeName : alternativeNames) {
        if (loadThemeCursor_helper(alternativeName, cursorImage)) {
            return;
        }
    }

    qCWarning(KWIN_CORE) << "Failed to load theme cursor for shape" << name;
}

bool WaylandCursorImage::loadThemeCursor_helper(const QByteArray &name, Image *cursorImage)
{
    const QVector<KXcursorSprite> sprites = m_cursorTheme.shape(name);
    if (sprites.isEmpty()) {
        return false;
    }

    cursorImage->image = sprites.first().data();
    cursorImage->hotspot = sprites.first().hotspot();

    return true;
}

void CursorImage::reevaluteSource()
{
    if (waylandServer()->seat()->isDragPointer()) {
        // TODO: touch drag?
        setSource(CursorSource::DragAndDrop);
        return;
    }
    if (waylandServer()->isScreenLocked()) {
        setSource(CursorSource::LockScreen);
        return;
    }
    if (input()->isSelectingWindow()) {
        setSource(CursorSource::WindowSelector);
        return;
    }
    if (effects && static_cast<EffectsHandlerImpl*>(effects)->isMouseInterception()) {
        setSource(CursorSource::EffectsOverride);
        return;
    }
    if (workspace() && workspace()->moveResizeClient()) {
        setSource(CursorSource::MoveResize);
        return;
    }
    if (m_pointer->decoration()) {
        setSource(CursorSource::Decoration);
        return;
    }
    const KWaylandServer::PointerInterface *pointer = waylandServer()->seat()->pointer();
    if (pointer && pointer->focusedSurface()) {
        setSource(CursorSource::PointerSurface);
        return;
    }
    setSource(CursorSource::Fallback);
}

void CursorImage::setSource(CursorSource source)
{
    if (m_currentSource == source) {
        return;
    }
    m_currentSource = source;
    Q_EMIT changed();
}

QImage CursorImage::image() const
{
    switch (m_currentSource) {
    case CursorSource::EffectsOverride:
        return m_effectsCursor.image;
    case CursorSource::MoveResize:
        return m_moveResizeCursor.image;
    case CursorSource::LockScreen:
    case CursorSource::PointerSurface:
        // lockscreen also uses server cursor image
        return m_serverCursor.cursor.image;
    case CursorSource::Decoration:
        return m_decorationCursor.image;
    case CursorSource::DragAndDrop:
        return m_drag.cursor.image;
    case CursorSource::Fallback:
        return m_fallbackCursor.image;
    case CursorSource::WindowSelector:
        return m_windowSelectionCursor.image;
    default:
        Q_UNREACHABLE();
    }
}

QPoint CursorImage::hotSpot() const
{
    switch (m_currentSource) {
    case CursorSource::EffectsOverride:
        return m_effectsCursor.hotspot;
    case CursorSource::MoveResize:
        return m_moveResizeCursor.hotspot;
    case CursorSource::LockScreen:
    case CursorSource::PointerSurface:
        // lockscreen also uses server cursor image
        return m_serverCursor.cursor.hotspot;
    case CursorSource::Decoration:
        return m_decorationCursor.hotspot;
    case CursorSource::DragAndDrop:
        return m_drag.cursor.hotspot;
    case CursorSource::Fallback:
        return m_fallbackCursor.hotspot;
    case CursorSource::WindowSelector:
        return m_windowSelectionCursor.hotspot;
    default:
        Q_UNREACHABLE();
    }
}

InputRedirectionCursor::InputRedirectionCursor(QObject *parent)
    : Cursor(parent)
    , m_currentButtons(Qt::NoButton)
{
    Cursors::self()->setMouse(this);
    connect(input(), &InputRedirection::globalPointerChanged,
            this, &InputRedirectionCursor::slotPosChanged);
    connect(input(), &InputRedirection::pointerButtonStateChanged,
            this, &InputRedirectionCursor::slotPointerButtonChanged);
#ifndef KCMRULES
    connect(input(), &InputRedirection::keyboardModifiersChanged,
            this, &InputRedirectionCursor::slotModifiersChanged);
#endif
}

InputRedirectionCursor::~InputRedirectionCursor()
{
}

void InputRedirectionCursor::doSetPos()
{
    if (input()->supportsPointerWarping()) {
        input()->warpPointer(currentPos());
    }
    slotPosChanged(input()->globalPointer());
    Q_EMIT posChanged(currentPos());
}

void InputRedirectionCursor::slotPosChanged(const QPointF &pos)
{
    const QPoint oldPos = currentPos();
    updatePos(pos.toPoint());
    Q_EMIT mouseChanged(pos.toPoint(), oldPos, m_currentButtons, m_currentButtons,
                      input()->keyboardModifiers(), input()->keyboardModifiers());
}

void InputRedirectionCursor::slotModifiersChanged(Qt::KeyboardModifiers mods, Qt::KeyboardModifiers oldMods)
{
    Q_EMIT mouseChanged(currentPos(), currentPos(), m_currentButtons, m_currentButtons, mods, oldMods);
}

void InputRedirectionCursor::slotPointerButtonChanged()
{
    const Qt::MouseButtons oldButtons = m_currentButtons;
    m_currentButtons = input()->qtButtonStates();
    const QPoint pos = currentPos();
    Q_EMIT mouseChanged(pos, pos, m_currentButtons, oldButtons, input()->keyboardModifiers(), input()->keyboardModifiers());
}

void InputRedirectionCursor::doStartCursorTracking()
{
#ifndef KCMRULES
//     connect(Cursors::self(), &Cursors::currentCursorChanged, this, &Cursor::cursorChanged);
#endif
}

void InputRedirectionCursor::doStopCursorTracking()
{
#ifndef KCMRULES
//     disconnect(kwinApp()->platform(), &Platform::cursorChanged, this, &Cursor::cursorChanged);
#endif
}

}
