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
 * EvdevForceFeedback.cpp
 *
 * Implementation of CEvdevForceFeedback. See EvdevForceFeedback.h for the
 * design rationale and effect mapping.
 */

#include "EvdevForceFeedback.h"

#ifdef SUPERMODEL_EVDEV

#include "Supermodel.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/input.h>

// Fixed direction for every uploaded effect (only one axis is ever driven);
// left/right comes from the sign of the magnitude instead (see ConstantForce).
static const uint16_t kEffectDirection = 0x4000;

CEvdevForceFeedback::CEvdevForceFeedback()
  : m_fd(-1),
    m_constantId(-1),
    m_springId(-1),
    m_frictionId(-1),
    m_damperId(-1),
    m_rumbleId(-1),
    m_periodicId(-1),
    m_haveAutocenter(false),
    m_haveGain(false)
{
}

CEvdevForceFeedback::~CEvdevForceFeedback()
{
  Close();
}

bool CEvdevForceFeedback::SupportsEffect(int ffType) const
{
  unsigned char ffBits[(FF_MAX / 8) + 1] = {};
  if (ioctl(m_fd, EVIOCGBIT(EV_FF, sizeof(ffBits)), ffBits) < 0)
    return false;
  return (ffBits[ffType / 8] & (1 << (ffType % 8))) != 0;
}

bool CEvdevForceFeedback::Open(const std::string &devNode)
{
  Close();

  int fd = open(devNode.c_str(), O_RDWR | O_NONBLOCK);
  if (fd < 0)
  {
    DebugLog("evdev FF: unable to open %s for force feedback (%s)\n", devNode.c_str(), strerror(errno));
    return false;
  }

  m_fd = fd;

  m_haveGain = SupportsEffect(FF_GAIN);
  m_haveAutocenter = SupportsEffect(FF_AUTOCENTER);

  if (SupportsEffect(FF_CONSTANT))
    m_constantId = UploadConstant();

  if (!m_haveAutocenter && SupportsEffect(FF_SPRING))
    m_springId = UploadCondition(FF_SPRING);

  if (SupportsEffect(FF_FRICTION))
    m_frictionId = UploadCondition(FF_FRICTION);
  else if (SupportsEffect(FF_DAMPER))
    m_damperId = UploadCondition(FF_DAMPER);

  if (SupportsEffect(FF_RUMBLE))
    m_rumbleId = UploadRumble();
  else if (SupportsEffect(FF_PERIODIC) && SupportsEffect(FF_SINE))
    m_periodicId = UploadPeriodic();

  if (!HasAnyForce())
  {
    DebugLog("evdev FF: %s has no usable force feedback effects, falling back\n", devNode.c_str());
    Close();
    return false;
  }

  SetGain(100); // Supermodel scales intensity itself via the SDL*Max config settings

  DebugLog("evdev FF: opened %s (constant=%d self-center=%s friction=%s vibrate=%s)\n",
    devNode.c_str(), m_constantId, HasSelfCenter() ? "yes" : "no", HasFriction() ? "yes" : "no", HasVibrate() ? "yes" : "no");
  return true;
}

void CEvdevForceFeedback::Close()
{
  if (m_fd >= 0)
  {
    StopAll();
    if (m_constantId >= 0) ioctl(m_fd, EVIOCRMFF, m_constantId);
    if (m_springId >= 0)   ioctl(m_fd, EVIOCRMFF, m_springId);
    if (m_frictionId >= 0) ioctl(m_fd, EVIOCRMFF, m_frictionId);
    if (m_damperId >= 0)   ioctl(m_fd, EVIOCRMFF, m_damperId);
    if (m_rumbleId >= 0)   ioctl(m_fd, EVIOCRMFF, m_rumbleId);
    if (m_periodicId >= 0) ioctl(m_fd, EVIOCRMFF, m_periodicId);
    close(m_fd);
  }

  m_fd = -1;
  m_constantId = m_springId = m_frictionId = m_damperId = m_rumbleId = m_periodicId = -1;
  m_haveAutocenter = false;
  m_haveGain = false;
}

void CEvdevForceFeedback::WriteEvent(uint16_t type, uint16_t code, int32_t value)
{
  if (m_fd < 0)
    return;

  struct input_event ev {};
  ev.type = type;
  ev.code = code;
  ev.value = value;

  (void)!write(m_fd, &ev, sizeof(ev)); // best-effort; ignore errors (e.g. unplugged mid-frame)
}

void CEvdevForceFeedback::Play(int effectId)
{
  if (effectId >= 0)
    WriteEvent(EV_FF, (uint16_t)effectId, 1);
}

void CEvdevForceFeedback::StopEffect(int effectId)
{
  if (effectId >= 0)
    WriteEvent(EV_FF, (uint16_t)effectId, 0);
}

int CEvdevForceFeedback::UploadConstant()
{
  struct ff_effect eff {};
  eff.type = FF_CONSTANT;
  eff.id = -1;
  eff.direction = kEffectDirection;
  eff.replay.length = 0; // play until stopped
  eff.replay.delay = 0;
  eff.u.constant.level = 0;

  if (ioctl(m_fd, EVIOCSFF, &eff) < 0)
    return -1;
  return eff.id;
}

int CEvdevForceFeedback::UploadCondition(uint16_t type)
{
  struct ff_effect eff {};
  eff.type = type;
  eff.id = -1;
  eff.direction = kEffectDirection;
  eff.replay.length = 0;
  eff.replay.delay = 0;
  eff.u.condition[0].right_saturation = 0xFFFF;
  eff.u.condition[0].left_saturation = 0xFFFF;
  eff.u.condition[0].right_coeff = 0;
  eff.u.condition[0].left_coeff = 0;
  eff.u.condition[0].deadband = 0;
  eff.u.condition[0].center = 0;
  // condition[1] left zeroed: single-axis wheel, no force on the second axis

  if (ioctl(m_fd, EVIOCSFF, &eff) < 0)
    return -1;
  return eff.id;
}

int CEvdevForceFeedback::UploadRumble()
{
  struct ff_effect eff {};
  eff.type = FF_RUMBLE;
  eff.id = -1;
  eff.replay.length = 0;
  eff.replay.delay = 0;
  eff.u.rumble.strong_magnitude = 0;
  eff.u.rumble.weak_magnitude = 0;

  if (ioctl(m_fd, EVIOCSFF, &eff) < 0)
    return -1;
  return eff.id;
}

int CEvdevForceFeedback::UploadPeriodic()
{
  struct ff_effect eff {};
  eff.type = FF_PERIODIC;
  eff.id = -1;
  eff.direction = kEffectDirection;
  eff.replay.length = 0;
  eff.replay.delay = 0;
  eff.u.periodic.waveform = FF_SINE;
  eff.u.periodic.period = 50; // ms
  eff.u.periodic.magnitude = 0;
  eff.u.periodic.offset = 0;
  eff.u.periodic.phase = 0;

  if (ioctl(m_fd, EVIOCSFF, &eff) < 0)
    return -1;
  return eff.id;
}

void CEvdevForceFeedback::SetGain(unsigned percent)
{
  if (!m_haveGain)
    return;
  if (percent > 100)
    percent = 100;
  WriteEvent(EV_FF, FF_GAIN, (int32_t)(percent * 0xFFFFu / 100u));
}

void CEvdevForceFeedback::ConstantForce(float force)
{
  if (m_constantId < 0)
    return;

  if (force > 1.0f) force = 1.0f;
  if (force < -1.0f) force = -1.0f;

  struct ff_effect eff {};
  eff.type = FF_CONSTANT;
  eff.id = m_constantId;
  eff.direction = kEffectDirection;
  eff.replay.length = 0;
  eff.replay.delay = 0;
  // Negated: confirmed on a G923 that a positive raw level turns the wheel
  // left, not right, at this fixed direction.
  eff.u.constant.level = (int16_t)(-force * 0x7FFF);

  if (ioctl(m_fd, EVIOCSFF, &eff) < 0)
    return;

  if (force == 0.0f)
    StopEffect(m_constantId);
  else
    Play(m_constantId);
}

void CEvdevForceFeedback::SelfCenter(float force)
{
  if (force < 0.0f) force = 0.0f;
  if (force > 1.0f) force = 1.0f;

  if (m_haveAutocenter)
  {
    WriteEvent(EV_FF, FF_AUTOCENTER, (int32_t)(force * 0xFFFF));
    return;
  }

  if (m_springId < 0)
    return;

  struct ff_effect eff {};
  eff.type = FF_SPRING;
  eff.id = m_springId;
  eff.direction = kEffectDirection;
  eff.replay.length = 0;
  eff.replay.delay = 0;
  eff.u.condition[0].right_saturation = 0xFFFF;
  eff.u.condition[0].left_saturation = 0xFFFF;
  eff.u.condition[0].right_coeff = (int16_t)(force * 0x7FFF);
  eff.u.condition[0].left_coeff = (int16_t)(force * 0x7FFF);
  eff.u.condition[0].deadband = 0;
  eff.u.condition[0].center = 0;

  if (ioctl(m_fd, EVIOCSFF, &eff) < 0)
    return;

  if (force == 0.0f)
    StopEffect(m_springId);
  else
    Play(m_springId);
}

void CEvdevForceFeedback::Friction(float force)
{
  if (force < 0.0f) force = 0.0f;
  if (force > 1.0f) force = 1.0f;

  const bool useFriction = (m_frictionId >= 0);
  const int effectId = useFriction ? m_frictionId : m_damperId;
  if (effectId < 0)
    return;

  struct ff_effect eff {};
  eff.type = useFriction ? FF_FRICTION : FF_DAMPER;
  eff.id = effectId;
  eff.direction = kEffectDirection;
  eff.replay.length = 0;
  eff.replay.delay = 0;
  eff.u.condition[0].right_saturation = 0xFFFF;
  eff.u.condition[0].left_saturation = 0xFFFF;
  eff.u.condition[0].right_coeff = (int16_t)(force * 0x7FFF);
  eff.u.condition[0].left_coeff = (int16_t)(force * 0x7FFF);
  eff.u.condition[0].deadband = 0;
  eff.u.condition[0].center = 0;

  if (ioctl(m_fd, EVIOCSFF, &eff) < 0)
    return;

  if (force == 0.0f)
    StopEffect(effectId);
  else
    Play(effectId);
}

void CEvdevForceFeedback::Vibrate(float strength)
{
  if (strength < 0.0f) strength = 0.0f;
  if (strength > 1.0f) strength = 1.0f;

  if (m_rumbleId >= 0)
  {
    struct ff_effect eff {};
    eff.type = FF_RUMBLE;
    eff.id = m_rumbleId;
    eff.replay.length = 0;
    eff.replay.delay = 0;
    eff.u.rumble.strong_magnitude = (uint16_t)(strength * 0xFFFF);
    eff.u.rumble.weak_magnitude = (uint16_t)(strength * 0xFFFF);

    if (ioctl(m_fd, EVIOCSFF, &eff) < 0)
      return;

    if (strength == 0.0f)
      StopEffect(m_rumbleId);
    else
      Play(m_rumbleId);
    return;
  }

  if (m_periodicId < 0)
    return;

  struct ff_effect eff {};
  eff.type = FF_PERIODIC;
  eff.id = m_periodicId;
  eff.direction = kEffectDirection;
  eff.replay.length = 0;
  eff.replay.delay = 0;
  eff.u.periodic.waveform = FF_SINE;
  eff.u.periodic.period = 50;
  eff.u.periodic.magnitude = (int16_t)(strength * 0x7FFF);
  eff.u.periodic.offset = 0;
  eff.u.periodic.phase = 0;

  if (ioctl(m_fd, EVIOCSFF, &eff) < 0)
    return;

  if (strength == 0.0f)
    StopEffect(m_periodicId);
  else
    Play(m_periodicId);
}

void CEvdevForceFeedback::StopAll()
{
  StopEffect(m_constantId);
  StopEffect(m_springId);
  StopEffect(m_frictionId);
  StopEffect(m_damperId);
  StopEffect(m_rumbleId);
  StopEffect(m_periodicId);
  if (m_haveAutocenter)
    WriteEvent(EV_FF, FF_AUTOCENTER, 0);
}

#endif // SUPERMODEL_EVDEV
