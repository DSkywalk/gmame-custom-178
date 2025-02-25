// license:BSD-3-Clause
// copyright-holders:Aaron Giles
/***************************************************************************

    machine.c

    Controls execution of the core MAME system.

****************************************************************************

    Since there has been confusion in the past over the order of
    initialization and other such things, here it is, all spelled out
    as of January, 2008:

    main()
        - does platform-specific init
        - calls mame_execute() [mame.c]

        mame_execute() [mame.c]
            - calls mame_validitychecks() [validity.c] to perform validity checks on all compiled drivers
            - begins resource tracking (level 1)
            - calls create_machine [mame.c] to initialize the running_machine structure
            - calls init_machine() [mame.c]

            init_machine() [mame.c]
                - calls fileio_init() [fileio.c] to initialize file I/O info
                - calls config_init() [config.c] to initialize configuration system
                - calls input_init() [input.c] to initialize the input system
                - calls output_init() [output.c] to initialize the output system
                - calls state_init() [state.c] to initialize save state system
                - calls state_save_allow_registration() [state.c] to allow registrations
                - calls palette_init() [palette.c] to initialize palette system
                - calls render_init() [render.c] to initialize the rendering system
                - calls ui_init() [ui.c] to initialize the user interface
                - calls generic_machine_init() [machine/generic.c] to initialize generic machine structures
                - calls timer_init() [timer.c] to reset the timer system
                - calls osd_init() [osdepend.h] to do platform-specific initialization
                - calls input_port_init() [inptport.c] to set up the input ports
                - calls rom_init() [romload.c] to load the game's ROMs
                - calls memory_init() [memory.c] to process the game's memory maps
                - calls the driver's DRIVER_INIT callback
                - calls device_list_start() [devintrf.c] to start any devices
                - calls video_init() [video.c] to start the video system
                - calls tilemap_init() [tilemap.c] to start the tilemap system
                - calls crosshair_init() [crsshair.c] to configure the crosshairs
                - calls sound_init() [sound.c] to start the audio system
                - calls debugger_init() [debugger.c] to set up the debugger
                - calls the driver's MACHINE_START, SOUND_START, and VIDEO_START callbacks
                - calls cheat_init() [cheat.c] to initialize the cheat system
                - calls image_init() [image.c] to initialize the image system

            - calls config_load_settings() [config.c] to load the configuration file
            - calls nvram_load [machine/generic.c] to load NVRAM
            - calls ui_display_startup_screens() [ui.c] to display the startup screens
            - begins resource tracking (level 2)
            - calls soft_reset() [mame.c] to reset all systems

                -------------------( at this point, we're up and running )----------------------

            - calls scheduler->timeslice() [schedule.c] over and over until we exit
            - ends resource tracking (level 2), freeing all auto_mallocs and timers
            - calls the nvram_save() [machine/generic.c] to save NVRAM
            - calls config_save_settings() [config.c] to save the game's configuration
            - calls all registered exit routines [mame.c]
            - ends resource tracking (level 1), freeing all auto_mallocs and timers

        - exits the program

***************************************************************************/

#include "emu.h"
#include "emuopts.h"
#include "osdepend.h"
#include "config.h"
#include "debugger.h"
#include "render.h"
#include "uiinput.h"
#include "crsshair.h"
#include "unzip.h"
#include "debug/debugvw.h"
#include "debug/debugcpu.h"
#include "image.h"
#include "network.h"
#include "ui/uimain.h"
#include <time.h>

#if defined(EMSCRIPTEN)
#include <emscripten.h>

void js_set_main_loop(running_machine * machine);
#endif



//**************************************************************************
//  RUNNING MACHINE
//**************************************************************************

osd_interface &running_machine::osd() const
{
	return m_manager.osd();
}

//-------------------------------------------------
//  running_machine - constructor
//-------------------------------------------------

running_machine::running_machine(const machine_config &_config, machine_manager &manager)
	: firstcpu(nullptr),
		primary_screen(nullptr),
		debug_flags(0),
		m_config(_config),
		m_system(_config.gamedrv()),
		m_manager(manager),
		m_current_phase(MACHINE_PHASE_PREINIT),
		m_paused(false),
		m_hard_reset_pending(false),
		m_exit_pending(false),
		m_soft_reset_timer(nullptr),
		m_rand_seed(0x9d14abd7),
		m_ui_active(_config.options().ui_active()),
		m_basename(_config.gamedrv().name),
		m_sample_rate(_config.options().sample_rate()),
		m_saveload_schedule(SLS_NONE),
		m_saveload_schedule_time(attotime::zero),
		m_saveload_searchpath(nullptr),

		m_save(*this),
		m_memory(*this),
		m_ioport(*this),
		m_parameters(*this),
		m_scheduler(*this)
{
	memset(&m_base_time, 0, sizeof(m_base_time));
	memset(&switchres, 0, sizeof(switchres));

	// set the machine on all devices
	device_iterator iter(root_device());
	for (device_t &device : iter)
		device.set_machine(*this);

	// find devices
	for (device_t &device : iter)
		if (dynamic_cast<cpu_device *>(&device) != nullptr)
		{
			firstcpu = downcast<cpu_device *>(&device);
			break;
		}
	primary_screen = screen_device_iterator(root_device()).first();

	// fetch core options
	if (options().debug())
		debug_flags = (DEBUG_FLAG_ENABLED | DEBUG_FLAG_CALL_HOOK) | (DEBUG_FLAG_OSD_ENABLED);
}


//-------------------------------------------------
//  ~running_machine - destructor
//-------------------------------------------------

running_machine::~running_machine()
{
}


//-------------------------------------------------
//  describe_context - return a string describing
//  which device is currently executing and its
//  PC
//-------------------------------------------------

const char *running_machine::describe_context()
{
	device_execute_interface *executing = m_scheduler.currently_executing();
	if (executing != nullptr)
	{
		cpu_device *cpu = dynamic_cast<cpu_device *>(&executing->device());
		if (cpu != nullptr)
		{
			address_space &prg = cpu->space(AS_PROGRAM);
			m_context = string_format(prg.is_octal() ? "'%s' (%0*o)" :  "'%s' (%0*X)", cpu->tag(), prg.logaddrchars(), cpu->pc());
		}
	}
	else
		m_context.assign("(no context)");

	return m_context.c_str();
}


//-------------------------------------------------
//  start - initialize the emulated machine
//-------------------------------------------------

void running_machine::start()
{
	// initialize basic can't-fail systems here
	m_configuration = std::make_unique<configuration_manager>(*this);
	m_input = std::make_unique<input_manager>(*this);
	m_output = std::make_unique<output_manager>(*this);
	m_render = std::make_unique<render_manager>(*this);
	m_bookkeeping = std::make_unique<bookkeeping_manager>(*this);

	// allocate a soft_reset timer
	m_soft_reset_timer = m_scheduler.timer_alloc(timer_expired_delegate(FUNC(running_machine::soft_reset), this));

	// initialize UI input
	m_ui_input = make_unique_clear<ui_input_manager>(*this);

	// init the osd layer
	m_manager.osd().init(*this);

	// create the video manager
	m_video = std::make_unique<video_manager>(*this);
	m_ui = manager().create_ui(*this);

	// initialize the base time (needed for doing record/playback)
	::time(&m_base_time);

	// initialize the input system and input ports for the game
	// this must be done before memory_init in order to allow specifying
	// callbacks based on input port tags
	time_t newbase = m_ioport.initialize();
	if (newbase != 0)
		m_base_time = newbase;

	// initialize the streams engine before the sound devices start
	m_sound = std::make_unique<sound_manager>(*this);

	// first load ROMs, then populate memory, and finally initialize CPUs
	// these operations must proceed in this order
	m_rom_load = make_unique_clear<rom_load_manager>(*this);
	m_memory.initialize();

	// save the random seed or save states might be broken in drivers that use the rand() method
	save().save_item(NAME(m_rand_seed));

	// initialize image devices
	m_image = std::make_unique<image_manager>(*this);
	m_tilemap = std::make_unique<tilemap_manager>(*this);
	m_crosshair = make_unique_clear<crosshair_manager>(*this);
	m_network = std::make_unique<network_manager>(*this);

	// initialize the debugger
	if ((debug_flags & DEBUG_FLAG_ENABLED) != 0)
	{
		m_debug_view = std::make_unique<debug_view_manager>(*this);
		m_debugger = std::make_unique<debugger_manager>(*this);
	}

	m_render->resolve_tags();

	manager().create_custom(*this);

	// register callbacks for the devices, then start them
	add_notifier(MACHINE_NOTIFY_RESET, machine_notify_delegate(FUNC(running_machine::reset_all_devices), this));
	add_notifier(MACHINE_NOTIFY_EXIT, machine_notify_delegate(FUNC(running_machine::stop_all_devices), this));
	save().register_presave(save_prepost_delegate(FUNC(running_machine::presave_all_devices), this));
	start_all_devices();
	save().register_postload(save_prepost_delegate(FUNC(running_machine::postload_all_devices), this));

	// if we're coming in with a savegame request, process it now
	const char *savegame = options().state();
	if (savegame[0] != 0)
		schedule_load(savegame);

	// if we're in autosave mode, schedule a load
	else if (options().autosave() && (m_system.flags & MACHINE_SUPPORTS_SAVE) != 0)
		schedule_load("auto");


	manager().update_machine();
}


//-------------------------------------------------
//  run - execute the machine
//-------------------------------------------------

int running_machine::run(bool quiet)
{
	int error = EMU_ERR_NONE;

	// use try/catch for deep error recovery
	try
	{
		// move to the init phase
		m_current_phase = MACHINE_PHASE_INIT;

		// if we have a logfile, set up the callback
		if (options().log() && !quiet)
		{
			m_logfile = std::make_unique<emu_file>(OPEN_FLAG_WRITE | OPEN_FLAG_CREATE | OPEN_FLAG_CREATE_PATHS);
			osd_file::error filerr = m_logfile->open("error.log");
			assert_always(filerr == osd_file::error::NONE, "unable to open log file");

			using namespace std::placeholders;
			add_logerror_callback(std::bind(&running_machine::logfile_callback, this, _1));
		}

		// then finish setting up our local machine
		start();

		// load the configuration settings and NVRAM
		m_configuration->load_settings();

		// disallow save state registrations starting here.
		// Don't do it earlier, config load can create network
		// devices with timers.
		m_save.allow_registration(false);

		nvram_load();
		sound().ui_mute(false);
		if (!quiet)
			sound().start_recording();

		// initialize ui lists
		// display the startup screens
		manager().ui_initialize(*this);

		// perform a soft reset -- this takes us to the running phase
		soft_reset();

		// handle initial load
		if (m_saveload_schedule != SLS_NONE)
			handle_saveload();

		// run the CPUs until a reset or exit
		m_hard_reset_pending = false;
		while ((!m_hard_reset_pending && !m_exit_pending) || m_saveload_schedule != SLS_NONE)
		{
			g_profiler.start(PROFILER_EXTRA);

#if defined(EMSCRIPTEN)
			// break out to our async javascript loop and halt
			js_set_main_loop(this);
#endif

			// execute CPUs if not paused
			if (!m_paused)
			{
				m_scheduler.timeslice();
				emulator_info::periodic_check();
			}
			// otherwise, just pump video updates through
			else
				m_video->frame_update();

			// handle save/load
			if (m_saveload_schedule != SLS_NONE)
				handle_saveload();

			g_profiler.stop();
		}

		// and out via the exit phase
		m_current_phase = MACHINE_PHASE_EXIT;

		// save the NVRAM and configuration
		sound().ui_mute(true);
		nvram_save();
		m_configuration->save_settings();
	}
	catch (emu_fatalerror &fatal)
	{
		osd_printf_error("Fatal error: %s\n", fatal.string());
		error = EMU_ERR_FATALERROR;
		if (fatal.exitcode() != 0)
			error = fatal.exitcode();
	}
	catch (emu_exception &)
	{
		osd_printf_error("Caught unhandled emulator exception\n");
		error = EMU_ERR_FATALERROR;
	}
	catch (binding_type_exception &btex)
	{
		osd_printf_error("Error performing a late bind of type %s to %s\n", btex.m_actual_type.name(), btex.m_target_type.name());
		error = EMU_ERR_FATALERROR;
	}
	catch (tag_add_exception &aex)
	{
		osd_printf_error("Tag '%s' already exists in tagged map\n", aex.tag());
		error = EMU_ERR_FATALERROR;
	}
	catch (std::exception &ex)
	{
		osd_printf_error("Caught unhandled %s exception: %s\n", typeid(ex).name(), ex.what());
		error = EMU_ERR_FATALERROR;
	}
	catch (...)
	{
		osd_printf_error("Caught unhandled exception\n");
		error = EMU_ERR_FATALERROR;
	}

	// make sure our phase is set properly before cleaning up,
	// in case we got here via exception
	m_current_phase = MACHINE_PHASE_EXIT;

	// call all exit callbacks registered
	call_notifiers(MACHINE_NOTIFY_EXIT);
	util::archive_file::cache_clear();

	// close the logfile
	m_logfile.reset();
	return error;
}


//-------------------------------------------------
//  schedule_exit - schedule a clean exit
//-------------------------------------------------

void running_machine::schedule_exit()
{
	m_exit_pending = true;

	// if we're executing, abort out immediately
	m_scheduler.eat_all_cycles();

	// if we're autosaving on exit, schedule a save as well
	if (options().autosave() && (m_system.flags & MACHINE_SUPPORTS_SAVE) && this->time() > attotime::zero)
		schedule_save("auto");
}


//-------------------------------------------------
//  schedule_hard_reset - schedule a hard-reset of
//  the machine
//-------------------------------------------------

void running_machine::schedule_hard_reset()
{
	m_hard_reset_pending = true;

	// if we're executing, abort out immediately
	m_scheduler.eat_all_cycles();
}


//-------------------------------------------------
//  schedule_soft_reset - schedule a soft-reset of
//  the system
//-------------------------------------------------

void running_machine::schedule_soft_reset()
{
	m_soft_reset_timer->adjust(attotime::zero);

	// we can't be paused since the timer needs to fire
	resume();

	// if we're executing, abort out immediately
	m_scheduler.eat_all_cycles();
}


//-------------------------------------------------
//  get_statename - allow to specify a subfolder of
//  the state directory for state loading/saving,
//  very useful for MESS and consoles or computers
//  where you can have separate folders for diff
//  software
//-------------------------------------------------

std::string running_machine::get_statename(const char *option) const
{
	std::string statename_str("");
	if (option == nullptr || option[0] == 0)
		statename_str.assign("%g");
	else
		statename_str.assign(option);

	// strip any extension in the provided statename
	int index = statename_str.find_last_of('.');
	if (index != -1)
		statename_str = statename_str.substr(0, index);

	// handle %d in the template (for image devices)
	std::string statename_dev("%d_");
	int pos = statename_str.find(statename_dev);

	if (pos != -1)
	{
		// if more %d are found, revert to default and ignore them all
		if (statename_str.find(statename_dev.c_str(), pos + 3) != -1)
			statename_str.assign("%g");
		// else if there is a single %d, try to create the correct snapname
		else
		{
			int name_found = 0;

			// find length of the device name
			int end1 = statename_str.find("/", pos + 3);
			int end2 = statename_str.find("%", pos + 3);
			int end;

			if ((end1 != -1) && (end2 != -1))
				end = std::min(end1, end2);
			else if (end1 != -1)
				end = end1;
			else if (end2 != -1)
				end = end2;
			else
				end = statename_str.length();

			if (end - pos < 3)
				fatalerror("Something very wrong is going on!!!\n");

			// copy the device name to an std::string
			std::string devname_str;
			devname_str.assign(statename_str.substr(pos + 3, end - pos - 3));
			//printf("check template: %s\n", devname_str.c_str());

			// verify that there is such a device for this system
			for (device_image_interface &image : image_interface_iterator(root_device()))
			{
				// get the device name
				std::string tempdevname(image.brief_instance_name());
				//printf("check device: %s\n", tempdevname.c_str());

				if (devname_str.compare(tempdevname) == 0)
				{
					// verify that such a device has an image mounted
					if (image.basename_noext() != nullptr)
					{
						std::string filename(image.basename_noext());

						// setup snapname and remove the %d_
						strreplace(statename_str, devname_str.c_str(), filename.c_str());
						statename_str.erase(pos, 3);
						//printf("check image: %s\n", filename.c_str());

						name_found = 1;
					}
				}
			}

			// or fallback to default
			if (name_found == 0)
				statename_str.assign("%g");
		}
	}

	// substitute path and gamename up front
	strreplace(statename_str, "/", PATH_SEPARATOR);
	strreplace(statename_str, "%g", basename());

	return statename_str;
}


//-------------------------------------------------
//  compose_saveload_filename - composes a filename
//  for state loading/saving
//-------------------------------------------------

std::string running_machine::compose_saveload_filename(const char *filename, const char **searchpath)
{
	std::string result;

	// is this an absolute path?
	if (osd_is_absolute_path(filename))
	{
		// if so, this is easy
		if (searchpath != nullptr)
			*searchpath = nullptr;
		result = filename;
	}
	else
	{
		// this is a relative path; first specify the search path
		if (searchpath != nullptr)
			*searchpath = options().state_directory();

		// take into account the statename option
		const char *stateopt = options().state_name();
		std::string statename = get_statename(stateopt);
		result = string_format("%s%s%s.sta", statename, PATH_SEPARATOR, filename);
	}
	return result;
}


//-------------------------------------------------
//  set_saveload_filename - specifies the filename
//  for state loading/saving
//-------------------------------------------------

void running_machine::set_saveload_filename(const char *filename)
{
	// compose the save/load filename and persist it
	m_saveload_pending_file = compose_saveload_filename(filename, &m_saveload_searchpath);
}


//-------------------------------------------------
//  schedule_save - schedule a save to occur as
//  soon as possible
//-------------------------------------------------

void running_machine::schedule_save(const char *filename)
{
	// specify the filename to save or load
	set_saveload_filename(filename);

	// note the start time and set a timer for the next timeslice to actually schedule it
	m_saveload_schedule = SLS_SAVE;
	m_saveload_schedule_time = this->time();

	// we can't be paused since we need to clear out anonymous timers
	resume();
}


//-------------------------------------------------
//  immediate_save - save state.
//-------------------------------------------------

void running_machine::immediate_save(const char *filename)
{
	// specify the filename to save or load
	set_saveload_filename(filename);

	// set up some parameters for handle_saveload()
	m_saveload_schedule = SLS_SAVE;
	m_saveload_schedule_time = this->time();

	// jump right into the save, anonymous timers can't hurt us!
	handle_saveload();
}


//-------------------------------------------------
//  schedule_load - schedule a load to occur as
//  soon as possible
//-------------------------------------------------

void running_machine::schedule_load(const char *filename)
{
	// specify the filename to save or load
	set_saveload_filename(filename);

	// note the start time and set a timer for the next timeslice to actually schedule it
	m_saveload_schedule = SLS_LOAD;
	m_saveload_schedule_time = this->time();

	// we can't be paused since we need to clear out anonymous timers
	resume();
}


//-------------------------------------------------
//  immediate_load - load state.
//-------------------------------------------------

void running_machine::immediate_load(const char *filename)
{
	// specify the filename to save or load
	set_saveload_filename(filename);

	// set up some parameters for handle_saveload()
	m_saveload_schedule = SLS_LOAD;
	m_saveload_schedule_time = this->time();

	// jump right into the load, anonymous timers can't hurt us
	handle_saveload();
}


//-------------------------------------------------
//  pause - pause the system
//-------------------------------------------------

void running_machine::pause()
{
	// ignore if nothing has changed
	if (m_paused)
		return;
	m_paused = true;

	// call the callbacks
	call_notifiers(MACHINE_NOTIFY_PAUSE);
}


//-------------------------------------------------
//  resume - resume the system
//-------------------------------------------------

void running_machine::resume()
{
	// ignore if nothing has changed
	if (!m_paused)
		return;
	m_paused = false;

	// call the callbacks
	call_notifiers(MACHINE_NOTIFY_RESUME);
}


//-------------------------------------------------
//  toggle_pause - toggles the pause state
//-------------------------------------------------

void running_machine::toggle_pause()
{
	if (paused())
		resume();
	else
		pause();
}


//-------------------------------------------------
//  add_notifier - add a notifier of the
//  given type
//-------------------------------------------------

void running_machine::add_notifier(machine_notification event, machine_notify_delegate callback, bool first)
{
	assert_always(m_current_phase == MACHINE_PHASE_INIT, "Can only call add_notifier at init time!");

	if(first)
		m_notifier_list[event].push_front(std::make_unique<notifier_callback_item>(callback));

	// exit notifiers are added to the head, and executed in reverse order
	else if (event == MACHINE_NOTIFY_EXIT)
		m_notifier_list[event].push_front(std::make_unique<notifier_callback_item>(callback));

	// all other notifiers are added to the tail, and executed in the order registered
	else
		m_notifier_list[event].push_back(std::make_unique<notifier_callback_item>(callback));
}


//-------------------------------------------------
//  add_logerror_callback - adds a callback to be
//  called on logerror()
//-------------------------------------------------

void running_machine::add_logerror_callback(logerror_callback callback)
{
	assert_always(m_current_phase == MACHINE_PHASE_INIT, "Can only call add_logerror_callback at init time!");
		m_string_buffer.reserve(1024);
	m_logerror_list.push_back(std::make_unique<logerror_callback_item>(callback));
}


//-------------------------------------------------
//  strlog - send an error logging string to the
//  debugger and any OSD-defined output streams
//-------------------------------------------------

void running_machine::strlog(const char *str) const
{
	// log to all callbacks
	for (auto &cb : m_logerror_list)
		cb->m_func(str);
}


//-------------------------------------------------
//  debug_break - breaks into the debugger, if
//  enabled
//-------------------------------------------------

void running_machine::debug_break()
{
	if ((debug_flags & DEBUG_FLAG_ENABLED) != 0)
		debugger().debug_break();
}

//-------------------------------------------------
//  base_datetime - retrieve the time of the host
//  system; useful for RTC implementations
//-------------------------------------------------

void running_machine::base_datetime(system_time &systime)
{
	systime.set(m_base_time);
}


//-------------------------------------------------
//  current_datetime - retrieve the current time
//  (offset by the base); useful for RTC
//  implementations
//-------------------------------------------------

void running_machine::current_datetime(system_time &systime)
{
	systime.set(m_base_time + this->time().seconds());
}


//-------------------------------------------------
//  rand - standardized random numbers
//-------------------------------------------------

UINT32 running_machine::rand()
{
	m_rand_seed = 1664525 * m_rand_seed + 1013904223;

	// return rotated by 16 bits; the low bits have a short period
	// and are frequently used
	return (m_rand_seed >> 16) | (m_rand_seed << 16);
}


//-------------------------------------------------
//  call_notifiers - call notifiers of the given
//  type
//-------------------------------------------------

void running_machine::call_notifiers(machine_notification which)
{
	for (auto& cb : m_notifier_list[which])
		cb->m_func();
}


//-------------------------------------------------
//  handle_saveload - attempt to perform a save
//  or load
//-------------------------------------------------

void running_machine::handle_saveload()
{
	// if no name, bail
	if (!m_saveload_pending_file.empty())
	{
		const char *const opname = (m_saveload_schedule == SLS_LOAD) ? "load" : "save";

		// if there are anonymous timers, we can't save just yet, and we can't load yet either
		// because the timers might overwrite data we have loaded
		if (!m_scheduler.can_save())
		{
			// if more than a second has passed, we're probably screwed
			if ((this->time() - m_saveload_schedule_time) > attotime::from_seconds(1))
				popmessage("Unable to %s due to pending anonymous timers. See error.log for details.", opname);
			else
				return; // return without cancelling the operation
		}
		else
		{
			UINT32 const openflags = (m_saveload_schedule == SLS_LOAD) ? OPEN_FLAG_READ : (OPEN_FLAG_WRITE | OPEN_FLAG_CREATE | OPEN_FLAG_CREATE_PATHS);

			// open the file
			emu_file file(m_saveload_searchpath, openflags);
			auto const filerr = file.open(m_saveload_pending_file.c_str());
			if (filerr == osd_file::error::NONE)
			{
				const char *const opnamed = (m_saveload_schedule == SLS_LOAD) ? "loaded" : "saved";

				// read/write the save state
				save_error saverr = (m_saveload_schedule == SLS_LOAD) ? m_save.read_file(file) : m_save.write_file(file);

				// handle the result
				switch (saverr)
				{
				case STATERR_ILLEGAL_REGISTRATIONS:
					popmessage("Error: Unable to %s state due to illegal registrations. See error.log for details.", opname);
					break;

				case STATERR_INVALID_HEADER:
					popmessage("Error: Unable to %s state due to an invalid header. Make sure the save state is correct for this machine.", opname);
					break;

				case STATERR_READ_ERROR:
					popmessage("Error: Unable to %s state due to a read error (file is likely corrupt).", opname);
					break;

				case STATERR_WRITE_ERROR:
					popmessage("Error: Unable to %s state due to a write error. Verify there is enough disk space.", opname);
					break;

				case STATERR_NONE:
					if (!(m_system.flags & MACHINE_SUPPORTS_SAVE))
						popmessage("State successfully %s.\nWarning: Save states are not officially supported for this machine.", opnamed);
					else
						popmessage("State successfully %s.", opnamed);
					break;

				default:
					popmessage("Error: Unknown error during state %s.", opnamed);
					break;
				}

				// close and perhaps delete the file
				if (saverr != STATERR_NONE && m_saveload_schedule == SLS_SAVE)
					file.remove_on_close();
			}
			else
				popmessage("Error: Failed to open file for %s operation.", opname);
		}
	}

	// unschedule the operation
	m_saveload_pending_file.clear();
	m_saveload_searchpath = nullptr;
	m_saveload_schedule = SLS_NONE;
}


//-------------------------------------------------
//  soft_reset - actually perform a soft-reset
//  of the system
//-------------------------------------------------

void running_machine::soft_reset(void *ptr, INT32 param)
{
	logerror("Soft reset\n");

	// temporarily in the reset phase
	m_current_phase = MACHINE_PHASE_RESET;

	// call all registered reset callbacks
	call_notifiers(MACHINE_NOTIFY_RESET);

	// now we're running
	m_current_phase = MACHINE_PHASE_RUNNING;
}


//-------------------------------------------------
//  logfile_callback - callback for logging to
//  logfile
//-------------------------------------------------

void running_machine::logfile_callback(const char *buffer)
{
	if (m_logfile != nullptr)
	{
		m_logfile->puts(buffer);
		m_logfile->flush();
	}
}


//-------------------------------------------------
//  start_all_devices - start any unstarted devices
//-------------------------------------------------

void running_machine::start_all_devices()
{
	// iterate through the devices
	int last_failed_starts = -1;
	while (last_failed_starts != 0)
	{
		// iterate over all devices
		int failed_starts = 0;
		for (device_t &device : device_iterator(root_device()))
			if (!device.started())
			{
				// attempt to start the device, catching any expected exceptions
				try
				{
					// if the device doesn't have a machine yet, set it first
					if (device.m_machine == nullptr)
						device.set_machine(*this);

					// now start the device
					osd_printf_verbose("Starting %s '%s'\n", device.name(), device.tag());
					device.start();
				}

				// handle missing dependencies by moving the device to the end
				catch (device_missing_dependencies &)
				{
					// if we're the end, fail
					osd_printf_verbose("  (missing dependencies; rescheduling)\n");
					failed_starts++;
				}
			}

		// each iteration should reduce the number of failed starts; error if
		// this doesn't happen
		if (failed_starts == last_failed_starts)
			throw emu_fatalerror("Circular dependency in device startup!");
		last_failed_starts = failed_starts;
	}
}


//-------------------------------------------------
//  reset_all_devices - reset all devices in the
//  hierarchy
//-------------------------------------------------

void running_machine::reset_all_devices()
{
	// reset the root and it will reset children
	root_device().reset();
}


//-------------------------------------------------
//  stop_all_devices - stop all the devices in the
//  hierarchy
//-------------------------------------------------

void running_machine::stop_all_devices()
{
	// first let the debugger save comments
	if ((debug_flags & DEBUG_FLAG_ENABLED) != 0)
		debugger().cpu().comment_save();

	// iterate over devices and stop them
	for (device_t &device : device_iterator(root_device()))
		device.stop();
}


//-------------------------------------------------
//  presave_all_devices - tell all the devices we
//  are about to save
//-------------------------------------------------

void running_machine::presave_all_devices()
{
	for (device_t &device : device_iterator(root_device()))
		device.pre_save();
}


//-------------------------------------------------
//  postload_all_devices - tell all the devices we
//  just completed a load
//-------------------------------------------------

void running_machine::postload_all_devices()
{
	for (device_t &device : device_iterator(root_device()))
		device.post_load();
}


/***************************************************************************
    NVRAM MANAGEMENT
***************************************************************************/

/*-------------------------------------------------
    nvram_filename - returns filename of system's
    NVRAM depending of selected BIOS
-------------------------------------------------*/

std::string running_machine::nvram_filename(device_t &device) const
{
	// start with either basename or basename_biosnum
	std::ostringstream result;
	result << basename();
	if (root_device().system_bios() != 0 && root_device().default_bios() != root_device().system_bios())
		util::stream_format(result, "_%d", root_device().system_bios() - 1);

	// device-based NVRAM gets its own name in a subdirectory
	if (device.owner() != nullptr)
	{
		// add per software nvrams into one folder
		const char *software = nullptr;
		for (device_t *dev = &device; dev->owner() != nullptr; dev = dev->owner())
		{
			device_image_interface *intf;
			if (dev->interface(intf))
			{
				software = intf->basename_noext();
				break;
			}
		}
		if (software != nullptr && *software != '\0')
			result << PATH_SEPARATOR << software;

		std::string tag(device.tag());
		tag.erase(0, 1);
		strreplacechr(tag,':', '_');
		result << PATH_SEPARATOR << tag;
	}
	return result.str();
}

/*-------------------------------------------------
    nvram_load - load a system's NVRAM
-------------------------------------------------*/

void running_machine::nvram_load()
{
	for (device_nvram_interface &nvram : nvram_interface_iterator(root_device()))
	{
		emu_file file(options().nvram_directory(), OPEN_FLAG_READ);
		if (file.open(nvram_filename(nvram.device()).c_str()) == osd_file::error::NONE)
		{
			nvram.nvram_load(file);
			file.close();
		}
		else
			nvram.nvram_reset();
	}
}


/*-------------------------------------------------
    nvram_save - save a system's NVRAM
-------------------------------------------------*/

void running_machine::nvram_save()
{
	for (device_nvram_interface &nvram : nvram_interface_iterator(root_device()))
	{
		emu_file file(options().nvram_directory(), OPEN_FLAG_WRITE | OPEN_FLAG_CREATE | OPEN_FLAG_CREATE_PATHS);
		if (file.open(nvram_filename(nvram.device()).c_str()) == osd_file::error::NONE)
		{
			nvram.nvram_save(file);
			file.close();
		}
	}
}


//**************************************************************************
//  OUTPUT
//**************************************************************************

void running_machine::popup_clear() const
{
	ui().popup_time(0, " ");
}

void running_machine::popup_message(util::format_argument_pack<std::ostream> const &args) const
{
	std::string const temp(string_format(args));
	ui().popup_time(temp.length() / 40 + 2, "%s", temp);
}


//**************************************************************************
//  CALLBACK ITEMS
//**************************************************************************

//-------------------------------------------------
//  notifier_callback_item - constructor
//-------------------------------------------------

running_machine::notifier_callback_item::notifier_callback_item(machine_notify_delegate func)
	: m_func(std::move(func))
{
}


//-------------------------------------------------
//  logerror_callback_item - constructor
//-------------------------------------------------

running_machine::logerror_callback_item::logerror_callback_item(logerror_callback func)
	: m_func(func)
{
}



//**************************************************************************
//  SYSTEM TIME
//**************************************************************************

//-------------------------------------------------
//  system_time - constructor
//-------------------------------------------------

system_time::system_time()
{
	set(0);
}


//-------------------------------------------------
//  set - fills out a system_time structure
//-------------------------------------------------

void system_time::set(time_t t)
{
	time = t;
	local_time.set(*localtime(&t));
	utc_time.set(*gmtime(&t));
}


//-------------------------------------------------
//  get_tm_time - converts a tm struction to a
//  MAME mame_system_tm structure
//-------------------------------------------------

void system_time::full_time::set(struct tm &t)
{
	second  = t.tm_sec;
	minute  = t.tm_min;
	hour    = t.tm_hour;
	mday    = t.tm_mday;
	month   = t.tm_mon;
	year    = t.tm_year + 1900;
	weekday = t.tm_wday;
	day  = t.tm_yday;
	is_dst  = t.tm_isdst;
}



//**************************************************************************
//  JAVASCRIPT PORT-SPECIFIC
//**************************************************************************

#if defined(EMSCRIPTEN)

static running_machine * jsmess_machine;

void js_main_loop()
{
	device_scheduler * scheduler;
	scheduler = &(jsmess_machine->scheduler());
	attotime stoptime(scheduler->time() + attotime(0,HZ_TO_ATTOSECONDS(60)));
	while (scheduler->time() < stoptime) {
		scheduler->timeslice();
	}
}

void js_set_main_loop(running_machine * machine) {
	jsmess_machine = machine;
	EM_ASM (
		JSMESS.running = true;
	);
	emscripten_set_main_loop(&js_main_loop, 0, 1);
}

running_machine * js_get_machine() {
	return jsmess_machine;
}

ui_manager * js_get_ui() {
	return &(jsmess_machine->ui());
}

sound_manager * js_get_sound() {
	return &(jsmess_machine->sound());
}

#endif /* defined(EMSCRIPTEN) */
