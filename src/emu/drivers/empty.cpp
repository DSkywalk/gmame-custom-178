// license:BSD-3-Clause
// copyright-holders:Aaron Giles
/*************************************************************************

    empty.c

    Empty driver.

**************************************************************************/

#include "emu.h"
#include "emuopts.h"
#include "render.h"

//**************************************************************************
//  DRIVER STATE
//**************************************************************************

class empty_state : public driver_device
{
public:
	// constructor
	empty_state(const machine_config &mconfig, device_type type, const char *tag)
		: driver_device(mconfig, type, tag)
	{
	}

	virtual void machine_start() override
	{
		emulator_info::display_ui_chooser(machine());
	}

	UINT32 screen_update(screen_device &screen, bitmap_rgb32 &bitmap, const rectangle &cliprect)
	{
		bitmap.fill(rgb_t::black);
		return 0;
	}
};



//**************************************************************************
//  MACHINE DRIVERS
//**************************************************************************

static MACHINE_CONFIG_START( ___empty, empty_state )

	// video hardware
	MCFG_SCREEN_ADD("screen", RASTER)
	MCFG_SCREEN_UPDATE_DRIVER(empty_state, screen_update)
	MCFG_SCREEN_SIZE(640,480)
	MCFG_SCREEN_VISIBLE_AREA(0,639, 0,479)
	MCFG_SCREEN_REFRESH_RATE(61)
MACHINE_CONFIG_END



//**************************************************************************
//  ROM DEFINITIONS
//**************************************************************************

ROM_START( ___empty )
	ROM_REGION( 0x10, "user1", ROMREGION_ERASEFF )
ROM_END



//**************************************************************************
//  GAME DRIVERS
//**************************************************************************

GAME( 2007, ___empty, 0, ___empty, 0, driver_device, 0, ROT0, "MAME", "No Driver Loaded", MACHINE_NO_SOUND )
