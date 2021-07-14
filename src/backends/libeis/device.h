#pragma once

#include "inputdevice.h"

struct eis_device;

namespace KWin
{
namespace Libeis
{
class Device : public InputDevice
{
    Q_OBJECT
public:
    explicit Device(eis_device *device, QObject *parent = nullptr);
    ~Device() override;
    virtual QString sysName() const override;
    virtual QString name() const override;

    virtual bool isEnabled() const override;
    virtual void setEnabled(bool enabled) override;

    virtual LEDs leds() const override;
    virtual void setLeds(LEDs leds) override;

    virtual bool isKeyboard() const override;
    virtual bool isAlphaNumericKeyboard() const override;
    virtual bool isPointer() const override;
    virtual bool isTouchpad() const override;
    virtual bool isTouch() const override;
    virtual bool isTabletTool() const override;
    virtual bool isTabletPad() const override;
    virtual bool isTabletModeSwitch() const override;
    virtual bool isLidSwitch() const override;

private:
    eis_device *m_device;
};

}
}
