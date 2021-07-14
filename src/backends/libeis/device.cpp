#include "device.h"

#include "libeis.h"

namespace KWin
{
namespace Libeis
{
Device::Device(eis_device *device, QObject *parent)
    : InputDevice(parent)
    , m_device(device)
{
    eis_device_ref(device);
}

Device::~Device()
{
    eis_device_unref(m_device);
}

QString Device::sysName() const
{
    return QString();
}

QString Device::name() const
{
    return QString::fromUtf8(eis_device_get_name(m_device));
}

bool Device::isEnabled() const
{
    return true;
}

void Device::setEnabled(bool enabled)
{
    enabled ? eis_device_resume(m_device) : eis_device_suspend(m_device);
}

LEDs Device::leds() const
{
    return LEDs();
}

void Device::setLeds(LEDs leds)
{
    Q_UNUSED(leds);
}

bool Device::isKeyboard() const
{
    return eis_device_has_capability(m_device, EIS_DEVICE_CAP_KEYBOARD);
}

bool Device::isAlphaNumericKeyboard() const
{
    return eis_device_has_capability(m_device, EIS_DEVICE_CAP_KEYBOARD);
}

bool Device::isPointer() const
{
    return eis_device_has_capability(m_device, EIS_DEVICE_CAP_POINTER) || eis_device_has_capability(m_device, EIS_DEVICE_CAP_POINTER_ABSOLUTE);
}

bool Device::isTouchpad() const
{
    return false;
}

bool Device::isTouch() const
{
    return eis_device_has_capability(m_device, EIS_DEVICE_CAP_TOUCH);
}

bool Device::isTabletTool() const
{
    return false;
}

bool Device::isTabletPad() const
{
    return false;
}

bool Device::isTabletModeSwitch() const
{
    return false;
}

bool Device::isLidSwitch() const
{
    return false;
}

}
}
