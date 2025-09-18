#pragma once
#include <Arduino.h>

enum class LedPattern : uint8_t {
  IdleOff,        // LED stays off
  ShareFlash,     // N quick pulses, one-shot
  Disconnected    // Slow repeating blink until reconnected
};


class LedAnimator {
public:
  // activeLow=true for ESP8266 BUILTIN_LED (GPIO2) so "LOW" == on
  explicit LedAnimator(uint8_t pin, bool activeLow = true)
    : pin_(pin), activeLow_(activeLow) {}

  void begin() {
    pinMode(pin_, OUTPUT);
    setOff();
  }

  // Call every loop()
  // led.h  (replace pulse() and update())
  void update() {
    const uint32_t now = millis();

    // One-shot pulse has priority over everything else
    if (oneshot_active_) {
      // if time elapsed, end pulse and force OFF
      if ((int32_t)(now - oneshot_off_deadline_ms_) >= 0) {
        oneshot_active_ = false;
        setOff();
      }
      return; // while a pulse is active, ignore other patterns
    }

    switch (mode_) {
      case LedPattern::IdleOff:
        // nothing to do
        break;

      case LedPattern::ShareFlash:
        // (unused by one-shot; keep for compatibility)
        setOff();
        mode_ = LedPattern::IdleOff;
        break;

      case LedPattern::Disconnected:
        // Blink: 200ms on, 1800ms off
        if (stateOn_) {
          if (now - lastMs_ >= 200) { lastMs_ = now; setOff(); }
        } else {
          if (now - lastMs_ >= 1800) { lastMs_ = now; setOn(); }
        }
        break;
    }
  }


  void pulse(uint16_t duration_ms = 200) {
    oneshot_active_ = true;
    oneshot_off_deadline_ms_ = millis() + duration_ms;
    setOn();  // immediate visual feedback
  }

  private:
  // ... existing members ...
  bool     oneshot_active_ = false;
  uint32_t oneshot_off_deadline_ms_ = 0;

  // Use when you know the connection state changed
  void setDisconnected() {
    mode_ = LedPattern::Disconnected;
    lastMs_ = millis();
    setOn(); // start cycle with ON
  }

  void setConnected() {
    mode_ = LedPattern::IdleOff;
    setOff();
  }

private:
  void setOn()  { stateOn_ = true;  digitalWrite(pin_, activeLow_ ? LOW  : HIGH); }
  void setOff() { stateOn_ = false; digitalWrite(pin_, activeLow_ ? HIGH : LOW);  }
  void toggle() { stateOn_ ? setOff() : setOn(); }

  uint8_t  pin_;
  bool     activeLow_;
  LedPattern mode_ = LedPattern::IdleOff;

  // ShareFlash state
  uint8_t  pulseCount_ = 0;
  uint8_t  edgeCount_  = 0;
  uint16_t stepMs_     = 80;

  // Shared timing
  bool     stateOn_ = false;
  uint32_t lastMs_  = 0;
};
