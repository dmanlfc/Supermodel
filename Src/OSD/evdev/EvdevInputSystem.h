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
 * EvdevInputSystem.h
 *
 * Header file for the Linux evdev input system.
 *
 * SDL exposes a single, merged system pointer, so it cannot tell two light guns
 * apart. This input system reads absolute-positioning guns straight from their
 * /dev/input/eventN nodes, which keeps each gun on its own mouse number, and
 * inherits everything else (keyboard, joysticks, force feedback) from the SDL
 * input system.
 *
 * Guns are discovered through udev by the ID_INPUT_GUN property, the same tag
 * the udev light gun rules set, and are ordered by event node so that gun
 * numbering is stable across runs.
 *
 * Based on the "evdev for guns" patch by Nicolas Adenis-Lamarre.
 *
 * Only built on Linux, and only when libudev is available (SUPERMODEL_EVDEV).
 */

#ifndef INCLUDED_EVDEVINPUTSYSTEM_H
#define INCLUDED_EVDEVINPUTSYSTEM_H

#ifdef SUPERMODEL_EVDEV

#include "SDLInputSystem.h"

#include <cstdint>
#include <string>
#include <vector>

struct udev;
struct input_event;

/*
 * Input system that reads light guns directly from evdev, layered on top of the
 * SDL input system.
 */
class CEvdevInputSystem : public CSDLInputSystem
{
public:
	/*
	 * Maximum number of guns that will be opened. The Model 3 hardware never has
	 * more than two, the extra headroom just avoids surprises with hubs that
	 * present several nodes.
	 */
	static const size_t MAX_GUNS = 8;

	/*
	 * Constructs an evdev input system.
	 */
	CEvdevInputSystem(const Util::Config::Node& config, bool useGameController = false);

	~CEvdevInputSystem();

	int GetNumMice() const;

	const MouseDetails *GetMouseDetails(int mseNum);

	bool Poll();

	void SetMouseVisibility(bool visible);

protected:
	bool InitializeSystem();

	int GetMouseAxisValue(int mseNum, int axisNum) const;

	int GetMouseWheelDir(int mseNum) const;

	bool IsMouseButPressed(int mseNum, int butNum) const;

	CInputSource *CreateAnyMouseSource(EMousePart msePart);

private:
	struct Gun
	{
		int fd = -1;              // open, non-blocking file descriptor of the event node
		int32_t rawMin[2] = { 0, 0 };   // ABS_X/ABS_Y minimum reported by the driver
		int32_t rawRange[2] = { 1, 1 }; // ABS_X/ABS_Y range (maximum - minimum), never zero
		int32_t raw[2] = { 0, 0 };      // last raw ABS_X/ABS_Y position
		uint32_t buttons = 0;           // one bit per Supermodel mouse button
	};

	/*
	 * Returns the gun backing the given mouse number, or nullptr if there is
	 * none. ANY_MOUSE maps to the first gun so that the default "MOUSE_XAXIS"
	 * style mappings drive player 1.
	 */
	const Gun *GetGun(int mseNum) const;

	/*
	 * Opens one /dev/input/eventN node and appends it to m_guns. Nodes that do
	 * not report absolute X and Y are rejected.
	 */
	bool OpenGun(const std::string &devNode);

	/*
	 * Closes every open gun and releases the udev context.
	 */
	void CloseGuns();

	/*
	 * Applies a single evdev event to a gun's state.
	 */
	static void HandleEvent(Gun &gun, const struct input_event &event);

	/*
	 * Scales a gun's raw axis position into window pixel coordinates, which is
	 * what CInputSystem expects a mouse axis to be expressed in.
	 */
	int ScaleAxis(const Gun &gun, int axis, int extent) const;

	struct udev *m_udev = nullptr;
	std::vector<Gun> m_guns;
	std::vector<MouseDetails> m_mseDetails;
};

#endif  // SUPERMODEL_EVDEV

#endif  // INCLUDED_EVDEVINPUTSYSTEM_H
