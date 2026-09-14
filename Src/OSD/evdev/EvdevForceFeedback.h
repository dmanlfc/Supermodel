/**
 ** Supermodel
 ** A Sega Model 3 Arcade Emulator.
 ** Copyright 2003-2026 The Supermodel Team
 **
 ** This file is part of Supermodel.
 **
 ** Supermodel is free software: you can redistribute it and/or modify it under
 ** the terms of the GNU General Public License as published by the Free
 ** Software Foundation, either version 3 of the License, or (at your option)
 ** any later version.
 **
 ** Supermodel is distributed in the hope that it will be useful, but WITHOUT
 ** ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 ** FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 ** more details.
 **
 ** You should have received a copy of the GNU General Public License along
 ** with Supermodel.  If not, see <http://www.gnu.org/licenses/>.
 **/

/*
 * EvdevForceFeedback.h
 *
 * Native Linux force feedback for a single joystick/wheel, driven directly
 * through the kernel's EV_FF ioctl API instead of SDL_Haptic, which is known
 * to misreport axis count and effect support on Linux (see HasBasicForce()
 * in SDLInputSystem.cpp).
 *
 * Maps Supermodel's ForceFeedbackCmd (Inputs/Input.h) to FF_* effects, with
 * fallbacks for drivers missing the primary type: FFConstantForce ->
 * FF_CONSTANT; FFSelfCenter -> FF_AUTOCENTER or FF_SPRING; FFFriction ->
 * FF_FRICTION or FF_DAMPER; FFVibrate -> FF_RUMBLE or FF_PERIODIC (sine).
 *
 * Effects are uploaded once at Open() and just re-uploaded with a new
 * magnitude on every command, rather than allocated per command.
 *
 * Only meaningful on Linux, built only when libudev is available
 * (SUPERMODEL_EVDEV), matching EvdevInputSystem's build gating.
 */

#ifndef INCLUDED_EVDEVFORCEFEEDBACK_H
#define INCLUDED_EVDEVFORCEFEEDBACK_H

#ifdef SUPERMODEL_EVDEV

#include <string>
#include <cstdint>

/*
 * Drives the EV_FF effects of a single evdev device node.
 */
class CEvdevForceFeedback
{
public:
  CEvdevForceFeedback();
  ~CEvdevForceFeedback();

  // Opens the given /dev/input/eventN node. Returns false (and leaves the
  // object closed) if it has no usable force feedback effect, so the caller
  // can fall back to SDL_Haptic.
  bool Open(const std::string &devNode);

  void Close();

  bool HasConstantForce() const { return m_constantId >= 0; }
  bool HasSelfCenter() const { return m_haveAutocenter || m_springId >= 0; }
  bool HasFriction() const { return m_frictionId >= 0 || m_damperId >= 0; }
  bool HasVibrate() const { return m_rumbleId >= 0 || m_periodicId >= 0; }
  bool HasAnyForce() const { return HasConstantForce() || HasSelfCenter() || HasFriction() || HasVibrate(); }

  void SetGain(unsigned percent); // percent of max, via FF_GAIN; no-op if unsupported

  void ConstantForce(float force); // -1.0 (left) .. +1.0 (right), 0.0 = off
  void SelfCenter(float force);    // 0.0 (off) .. 1.0 (strongest)
  void Friction(float force);      // 0.0 (off) .. 1.0 (strongest)
  void Vibrate(float strength);    // 0.0 (off) .. 1.0 (strongest)

  void StopAll();

private:
  int m_fd;

  // Uploaded effect ids, -1 if unsupported
  int m_constantId;
  int m_springId;
  int m_frictionId;
  int m_damperId;
  int m_rumbleId;
  int m_periodicId;

  bool m_haveAutocenter;
  bool m_haveGain;

  bool SupportsEffect(int ffType) const;

  int UploadConstant();
  int UploadCondition(uint16_t type);
  int UploadRumble();
  int UploadPeriodic();

  void Play(int effectId);
  void StopEffect(int effectId);
  void WriteEvent(uint16_t type, uint16_t code, int32_t value);
};

#endif // SUPERMODEL_EVDEV

#endif // INCLUDED_EVDEVFORCEFEEDBACK_H
