/*
    SPDX-FileCopyrightText: 2021 David Redondo <kde@david-redondo>

    SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#include "connection.h"

#include "backends/libeis/libeis_logging.h"
#include "device.h"

#include <QPointF>
#include <QSizeF>
#include <QSocketNotifier>
#include <QThread>

#include <libeis.h>

#include <linux/input-event-codes.h>

namespace KWin
{
namespace Libeis
{
static void eis_log_handler(eis *eis, eis_log_priority priority, const char *message, bool is_continuation)
{
   switch (priority) {
   case EIS_LOG_PRIORITY_DEBUG:
        qCDebug(KWIN_EIS) << "Libeis:" << message;
        break;
    case EIS_LOG_PRIORITY_INFO:
        qCInfo(KWIN_EIS) << "Libeis:" << message;
        break;
    case EIS_LOG_PRIORITY_WARNING:
        qCWarning(KWIN_EIS) << "Libeis:" << message;
        break;
    case EIS_LOG_PRIORITY_ERROR:
        qCritical(KWIN_EIS) << "Libeis:" << message;
        break;
    }
}

Connection::~Connection()
{
    m_thread->quit();
    m_thread->wait();
    eis_unref(m_eis);
}

Connection::Connection(eis *eis)
    : m_thread(std::make_unique<QThread>())
    , m_eis(eis)
{
    const auto fd = eis_get_fd(m_eis);
    auto m_notifier = new QSocketNotifier(fd, QSocketNotifier::Read, this);
    connect(m_notifier, &QSocketNotifier::activated, this, &Connection::handleEvents);

    eis_log_set_priority(eis, EIS_LOG_PRIORITY_DEBUG);
    eis_log_set_handler(eis, eis_log_handler);

    moveToThread(m_thread.get());
    m_thread->setObjectName("libeis thread");
    m_thread->start();
}

void Connection::handleEvents()
{
    eis_dispatch(m_eis);
    auto eventDevice = [](eis_event *event) {
        return static_cast<Device *>(eis_device_get_user_data(eis_event_get_device(event)));
    };
    while (eis_event *const event = eis_get_event(m_eis)) {
        switch (eis_event_get_type(event)) {
        case EIS_EVENT_CLIENT_CONNECT: {
            auto client = eis_event_get_client(event);
            eis_client_connect(client);
            auto seat = eis_client_new_seat(client, "seat");
            eis_client_set_user_data(client, seat);
            eis_seat_allow_capability(seat, EIS_DEVICE_CAP_POINTER);
            eis_seat_allow_capability(seat, EIS_DEVICE_CAP_POINTER_ABSOLUTE);
            eis_seat_allow_capability(seat, EIS_DEVICE_CAP_KEYBOARD);
            eis_seat_allow_capability(seat, EIS_DEVICE_CAP_TOUCH);
            eis_seat_add(seat);
            qCDebug(KWIN_EIS) << "new client" << eis_client_get_name(client);
            break;
        }
        case EIS_EVENT_CLIENT_DISCONNECT: {
            auto client = eis_event_get_client(event);
            eis_seat_unref(static_cast<eis_seat*>(eis_client_get_user_data(client)));
            qCDebug(KWIN_EIS) << "client disconnected" << eis_client_get_name(client);
            break;
        }
        case EIS_EVENT_DEVICE_ADDED: {
            auto device = eis_event_get_device(event);
            eis_device_allow_capability(device, EIS_DEVICE_CAP_POINTER);
            eis_device_allow_capability(device, EIS_DEVICE_CAP_POINTER_ABSOLUTE);
            eis_device_allow_capability(device, EIS_DEVICE_CAP_KEYBOARD);
            eis_device_allow_capability(device, EIS_DEVICE_CAP_TOUCH);
            // We don't handle different keymaps for now
            eis_device_keyboard_set_keymap(device, nullptr);
            eis_device_connect(device);
            eis_device_resume(device);
            auto inputDevice = new Device(device);
            qCDebug(KWIN_EIS) << "New device" << eis_device_get_name(device) << "client:" << eis_client_get_name(eis_event_get_client(event));
            eis_device_set_user_data(device, inputDevice);
            Q_EMIT deviceAdded(inputDevice);
            break;
        }
        case EIS_EVENT_DEVICE_REMOVED: {
            auto device = eventDevice(event);
            qCDebug(KWIN_EIS) << "device removed" << eis_device_get_name(eis_event_get_device(event));
            // Move device into main thread so it's only deleted once the main thread is done with it
            device->moveToThread(qApp->thread());
            Q_EMIT deviceRemoved(device);
            device->deleteLater();
            break;
        }
        case EIS_EVENT_POINTER_MOTION: {
            const auto x = eis_event_pointer_get_dx(event);
            const auto y = eis_event_pointer_get_dy(event);
            qCDebug(KWIN_EIS) << eis_client_get_name(eis_event_get_client(event)) << "pointer motion" << x << y;
            const QSizeF delta(x, y);
            auto device = eventDevice(event);
            // TODO fix  time
            Q_EMIT device->pointerMotion(delta, delta, 0, 0, device);
            break;
        }
        case EIS_EVENT_POINTER_MOTION_ABSOLUTE: {
            const auto x = eis_event_pointer_get_absolute_x(event);
            const auto y = eis_event_pointer_get_absolute_y(event);
            qCDebug(KWIN_EIS) << eis_client_get_name(eis_event_get_client(event)) << "pointer motion absolute" << x << y;
            auto device = eventDevice(event);
            // TODO fix time
            Q_EMIT device->pointerMotionAbsolute({x, y}, 0, device);
            break;
        }
        case EIS_EVENT_POINTER_BUTTON: {
            const auto button = eis_event_pointer_get_button(event);
            const auto press = eis_event_pointer_get_button_is_press(event);
            qCDebug(KWIN_EIS) << eis_client_get_name(eis_event_get_client(event)) << "pointer" << button << press;
            auto device = eventDevice(event);
            // TODO fix time
            Q_EMIT device->pointerButtonChanged(button, press ? InputRedirection::PointerButtonPressed : InputRedirection::PointerButtonReleased, 0, device);
            break;
        }
        case EIS_EVENT_POINTER_SCROLL: {
            const auto x = eis_event_pointer_get_scroll_x(event);
            const auto y = eis_event_pointer_get_scroll_y(event);
            qCDebug(KWIN_EIS) << eis_client_get_name(eis_event_get_client(event)) << "pointer scroll" << x << y;
            auto device = eventDevice(event);
            if (x > 0) {
                // TODO fix time
                Q_EMIT device->pointerAxisChanged(InputRedirection::PointerAxisHorizontal, x, 0, InputRedirection::PointerAxisSourceUnknown, 0, device);
            }
            if (y > 0) {
                // TODO fix time
                Q_EMIT device->pointerAxisChanged(InputRedirection::PointerAxisVertical, y, 0, InputRedirection::PointerAxisSourceUnknown, 0, device);
            }
            break;
        }
        case EIS_EVENT_POINTER_SCROLL_DISCRETE: {
            const auto x = eis_event_pointer_get_scroll_discrete_x(event);
            const auto y = eis_event_pointer_get_scroll_discrete_y(event);
            qCDebug(KWIN_EIS) << eis_client_get_name(eis_event_get_client(event)) << "pointer scroll discrete" << x << y;
            auto device = eventDevice(event);
            constexpr int clickAmount = 120;
            constexpr int anglePerClick = 15;
            if (x > 0) {
                const int steps = x / clickAmount;
                // TODO fix time
                Q_EMIT device->pointerAxisChanged(InputRedirection::PointerAxisHorizontal,
                                                  steps * anglePerClick,
                                                  steps,
                                                  InputRedirection::PointerAxisSourceUnknown,
                                                  0,
                                                  device);
            }
            if (y > 0) {
                const int steps = y / clickAmount;
                // TODO fix time
                Q_EMIT device->pointerAxisChanged(InputRedirection::PointerAxisVertical,
                                                  steps * anglePerClick,
                                                  steps,
                                                  InputRedirection::PointerAxisSourceUnknown,
                                                  0,
                                                  device);
            }
            break;
        }
        case EIS_EVENT_KEYBOARD_KEY: {
            const auto key = eis_event_keyboard_get_key(event);
            const auto press = eis_event_keyboard_get_key_is_press(event);
            auto device = eventDevice(event);
            // TODO fix time
            Q_EMIT device->keyChanged(key, press ? InputRedirection::KeyboardKeyPressed : InputRedirection::KeyboardKeyReleased, 0, device);
            break;
        }
        case EIS_EVENT_TOUCH_DOWN: {
            const auto x = eis_event_touch_get_x(event);
            const auto y = eis_event_touch_get_y(event);
            const auto id = eis_event_touch_get_id(event);
            qCDebug(KWIN_EIS) << eis_client_get_name(eis_event_get_client(event)) << "touch down" << id << x << y;
            auto device = eventDevice(event);
            // TODO fix time
            Q_EMIT device->touchDown(id, {x, y}, 0, device);
            break;
        }
        case EIS_EVENT_TOUCH_UP: {
            const auto id = eis_event_touch_get_id(event);
            qCDebug(KWIN_EIS) << eis_client_get_name(eis_event_get_client(event)) << "touch up" << id;
            auto device = eventDevice(event);
            // TODO fix time
            Q_EMIT device->touchUp(id, 0, device);
            break;
        }
        case EIS_EVENT_TOUCH_MOTION: {
            const auto x = eis_event_touch_get_x(event);
            const auto y = eis_event_touch_get_y(event);
            const auto id = eis_event_touch_get_id(event);
            qCDebug(KWIN_EIS) << eis_client_get_name(eis_event_get_client(event)) << "touch move" << id << x << y;
            auto device = eventDevice(event);
            // TODO fix time
            Q_EMIT device->touchMotion(id, {x, y}, 0, device);
            break;
        }
        }
    }
}

}
}
