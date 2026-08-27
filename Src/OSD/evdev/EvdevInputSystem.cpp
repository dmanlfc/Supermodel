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
 * EvdevInputSystem.cpp
 *
 * Linux evdev light gun input system. See EvdevInputSystem.h.
 */

#ifdef SUPERMODEL_EVDEV

#include "EvdevInputSystem.h"

#include "Supermodel.h"

#include <libudev.h>
#include <linux/input.h>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{
	const char EVENT_NODE_PREFIX[] = "/dev/input/event";

	bool IsAllDigits(const char *s)
	{
		if (*s == '\0')
			return false;
		for (; *s != '\0'; s++)
		{
			if (*s < '0' || *s > '9')
				return false;
		}
		return true;
	}

	/*
	 * Orders device nodes the way a person would: /dev/input/event9 sorts before
	 * /dev/input/event10. Gun numbering follows this order, so it has to be
	 * stable and not depend on the order udev happens to return devices in.
	 */
	bool DevNodeLess(const std::string &a, const std::string &b)
	{
		size_t common = 0;
		while (common < a.size() && common < b.size() && a[common] == b[common])
			common++;
		const char *tailA = a.c_str() + common;
		const char *tailB = b.c_str() + common;
		if (IsAllDigits(tailA) && IsAllDigits(tailB))
			return atoi(tailA) < atoi(tailB);
		return a < b;
	}
}

CEvdevInputSystem::CEvdevInputSystem(const Util::Config::Node& config, bool useGameController)
	: CSDLInputSystem(config, useGameController)
{
	m_guns.reserve(MAX_GUNS);
}

CEvdevInputSystem::~CEvdevInputSystem()
{
	CloseGuns();
}

void CEvdevInputSystem::CloseGuns()
{
	for (Gun &gun : m_guns)
	{
		if (gun.fd >= 0)
			close(gun.fd);
		gun.fd = -1;
	}
	m_guns.clear();
	m_mseDetails.clear();

	if (m_udev != nullptr)
	{
		udev_unref(m_udev);
		m_udev = nullptr;
	}
}

bool CEvdevInputSystem::InitializeSystem()
{
	m_udev = udev_new();
	if (m_udev == nullptr)
	{
		ErrorLog("evdev: unable to create a udev context; no light guns will be available.");
		return CSDLInputSystem::InitializeSystem();
	}

	// Guns are tagged ID_INPUT_GUN=1 by the udev light gun rules. A single
	// physical gun can expose several nodes (eventN, mouseN, jsN), all of which
	// inherit the property, so only the event nodes are of interest here.
	std::vector<std::string> devNodes;
	udev_enumerate *enumerate = udev_enumerate_new(m_udev);
	if (enumerate != nullptr)
	{
		udev_enumerate_add_match_subsystem(enumerate, "input");
		udev_enumerate_add_match_property(enumerate, "ID_INPUT_GUN", "1");
		udev_enumerate_scan_devices(enumerate);

		for (udev_list_entry *item = udev_enumerate_get_list_entry(enumerate); item != nullptr; item = udev_list_entry_get_next(item))
		{
			udev_device *dev = udev_device_new_from_syspath(m_udev, udev_list_entry_get_name(item));
			if (dev == nullptr)
				continue;
			const char *devNode = udev_device_get_devnode(dev);
			if (devNode != nullptr && strncmp(devNode, EVENT_NODE_PREFIX, sizeof(EVENT_NODE_PREFIX) - 1) == 0)
				devNodes.emplace_back(devNode);
			udev_device_unref(dev);
		}

		udev_enumerate_unref(enumerate);
	}

	std::sort(devNodes.begin(), devNodes.end(), DevNodeLess);

	for (const std::string &devNode : devNodes)
	{
		if (m_guns.size() >= MAX_GUNS)
		{
			ErrorLog("evdev: more than %u light guns attached, ignoring %s.", (unsigned)MAX_GUNS, devNode.c_str());
			break;
		}
		OpenGun(devNode);
	}

	if (m_guns.empty())
		ErrorLog("evdev: no light guns found, falling back to the SDL system mouse.");

	return CSDLInputSystem::InitializeSystem();
}

bool CEvdevInputSystem::OpenGun(const std::string &devNode)
{
	const int fd = open(devNode.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0)
	{
		ErrorLog("evdev: unable to open %s (%s).", devNode.c_str(), strerror(errno));
		return false;
	}

	// A gun has to report an absolute position. Anything else (a relative mouse
	// mis-tagged as a gun, a keyboard node belonging to the same device) is of no
	// use here and would otherwise sit in the gun list doing nothing.
	struct input_absinfo absX, absY;
	if (ioctl(fd, EVIOCGABS(ABS_X), &absX) < 0 || ioctl(fd, EVIOCGABS(ABS_Y), &absY) < 0 ||
	    absX.maximum <= absX.minimum || absY.maximum <= absY.minimum)
	{
		close(fd);
		return false;
	}

	Gun gun;
	gun.fd = fd;
	gun.rawMin[0] = absX.minimum;
	gun.rawRange[0] = absX.maximum - absX.minimum;
	gun.rawMin[1] = absY.minimum;
	gun.rawRange[1] = absY.maximum - absY.minimum;
	// Start centred rather than at the top left corner, which for The Lost World
	// would read as "pointing off screen" before the gun has been moved.
	gun.raw[0] = absX.minimum + gun.rawRange[0] / 2;
	gun.raw[1] = absY.minimum + gun.rawRange[1] / 2;

	MouseDetails mseDetails{};
	char name[MAX_NAME_LENGTH + 1];
	if (ioctl(fd, EVIOCGNAME(sizeof(name)), name) < 0)
		snprintf(mseDetails.name, sizeof(mseDetails.name), "%s", devNode.c_str());
	else
	{
		name[sizeof(name) - 1] = '\0';
		snprintf(mseDetails.name, sizeof(mseDetails.name), "%s", name);
	}
	mseDetails.isAbsolute = true;

	InfoLog("evdev: gun %u is %s (%s)", (unsigned)m_guns.size() + 1, mseDetails.name, devNode.c_str());

	m_guns.push_back(gun);
	m_mseDetails.push_back(mseDetails);
	return true;
}

const CEvdevInputSystem::Gun *CEvdevInputSystem::GetGun(int mseNum) const
{
	if (m_guns.empty())
		return nullptr;
	// The stock input mappings use the unnumbered "MOUSE_..." sources, which
	// arrive here as ANY_MOUSE. Point those at the first gun rather than at
	// nothing, and never index the vector out of range.
	if (mseNum == ANY_MOUSE)
		return &m_guns[0];
	if (mseNum < 0 || (size_t)mseNum >= m_guns.size())
		return nullptr;
	return &m_guns[mseNum];
}

int CEvdevInputSystem::GetNumMice() const
{
	if (m_guns.empty())
		return CSDLInputSystem::GetNumMice();
	return (int)m_guns.size();
}

const MouseDetails *CEvdevInputSystem::GetMouseDetails(int mseNum)
{
	if (mseNum < 0 || (size_t)mseNum >= m_mseDetails.size())
		return CSDLInputSystem::GetMouseDetails(mseNum);
	return &m_mseDetails[mseNum];
}

bool CEvdevInputSystem::Poll()
{
	bool res = false;

	struct input_event events[64];
	for (Gun &gun : m_guns)
	{
		for (;;)
		{
			const ssize_t len = read(gun.fd, events, sizeof(events));
			if (len <= 0)
				break;  // drained (EAGAIN) or the device went away
			const size_t count = (size_t)len / sizeof(events[0]);
			for (size_t i = 0; i < count; i++)
				HandleEvent(gun, events[i]);
			res = true;
			// A short read means the kernel buffer is empty, so skip the extra
			// read() syscall per gun per frame that would only return EAGAIN.
			if ((size_t)len < sizeof(events))
				break;
		}
	}

	if (CSDLInputSystem::Poll())
		res = true;
	return res;
}

void CEvdevInputSystem::HandleEvent(Gun &gun, const struct input_event &event)
{
	switch (event.type)
	{
	case EV_KEY:
	{
		// Mapped to Supermodel's five mouse buttons. The ordering matches the
		// buttons a light gun normally reports: trigger, then the auxiliary
		// buttons most gun drivers put on BTN_RIGHT/BTN_MIDDLE/BTN_1/BTN_2.
		int butNum;
		switch (event.code)
		{
		case BTN_LEFT:   butNum = 0; break;
		case BTN_2:      butNum = 1; break;
		case BTN_RIGHT:  butNum = 2; break;
		case BTN_MIDDLE: butNum = 3; break;
		case BTN_1:      butNum = 4; break;
		default:         return;
		}
		// Value 2 is auto-repeat, which counts as still held.
		if (event.value != 0)
			gun.buttons |= (uint32_t)1 << butNum;
		else
			gun.buttons &= ~((uint32_t)1 << butNum);
		break;
	}

	case EV_ABS:
		if (event.code == ABS_X)
			gun.raw[0] = event.value;
		else if (event.code == ABS_Y)
			gun.raw[1] = event.value;
		break;

	default:
		break;
	}
}

int CEvdevInputSystem::ScaleAxis(const Gun &gun, int axis, int extent) const
{
	if (extent <= 0)
		return 0;
	const int64_t offset = (int64_t)gun.raw[axis] - gun.rawMin[axis];
	int scaled = (int)((offset * extent) / gun.rawRange[axis]);
	// Guns can report slightly outside the range they advertise. This bounds the
	// value for the code that consumes a mouse axis directly, such as the
	// deadzone maths and ConfigMouseCentered(). It is not what makes the
	// off-screen reload work: CInputSource::Scale() clamps again on the way to
	// the game's 0-255 ADC range, so an out-of-range position saturates there
	// with or without this.
	if (scaled < 0)
		scaled = 0;
	else if (scaled > extent)
		scaled = extent;
	return scaled;
}

int CEvdevInputSystem::GetMouseAxisValue(int mseNum, int axisNum) const
{
	const Gun *gun = GetGun(mseNum);
	if (gun == nullptr)
		return CSDLInputSystem::GetMouseAxisValue(mseNum, axisNum);

	// Mouse axes are in window pixel coordinates, and the gun covers the whole
	// display, letterbox included: that is m_dispW wide plus the m_dispX border
	// on either side.
	switch (axisNum)
	{
	case AXIS_X: return ScaleAxis(*gun, 0, (int)(m_dispW + 2 * m_dispX));
	case AXIS_Y: return ScaleAxis(*gun, 1, (int)(m_dispH + 2 * m_dispY));
	default:     return 0;  // guns have no wheel
	}
}

int CEvdevInputSystem::GetMouseWheelDir(int mseNum) const
{
	if (m_guns.empty())
		return CSDLInputSystem::GetMouseWheelDir(mseNum);
	return 0;
}

bool CEvdevInputSystem::IsMouseButPressed(int mseNum, int butNum) const
{
	if (m_guns.empty())
		return CSDLInputSystem::IsMouseButPressed(mseNum, butNum);
	if (butNum < 0 || butNum >= NUM_MOUSE_BUTTONS)
		return false;

	const uint32_t mask = (uint32_t)1 << butNum;
	if (mseNum == ANY_MOUSE)
	{
		// "Any mouse" really does mean any of them here: this is what the input
		// configurator watches while waiting for a button to be pressed.
		for (const Gun &gun : m_guns)
		{
			if (gun.buttons & mask)
				return true;
		}
		return false;
	}

	const Gun *gun = GetGun(mseNum);
	return gun != nullptr && (gun->buttons & mask) != 0;
}

CInputSource *CEvdevInputSystem::CreateAnyMouseSource(EMousePart msePart)
{
	if (m_guns.empty())
		return CSDLInputSystem::CreateAnyMouseSource(msePart);
	// The default implementation merges every mouse into one source, which would
	// make both guns fight over player 1's aim. Keep ANY_MOUSE as a single
	// source; GetGun() resolves it to gun 1.
	return CreateMouseSource(ANY_MOUSE, msePart);
}

void CEvdevInputSystem::SetMouseVisibility(bool visible)
{
	// With guns attached the on-screen crosshair is the pointer, and the system
	// cursor only gets dragged around by whichever gun also drives it.
	CSDLInputSystem::SetMouseVisibility(m_guns.empty() ? visible : false);
}

#endif  // SUPERMODEL_EVDEV
