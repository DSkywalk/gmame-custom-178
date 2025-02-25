/**************************************************************

   custom_video_ati.cpp - ATI legacy library
   ---------------------------------------------------------

   SwitchRes   Modeline generation engine for emulation

   GroovyMAME  Integration of SwitchRes into the MAME project
               Some reworked patches from SailorSat's CabMAME

   License     GPL-2.0+
   Copyright   2010-2016 - Chris Kennedy, Antonio Giner

 **************************************************************/

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "emu.h"
#include "custom_video_ati.h"

#define CRTC_DOUBLE_SCAN                    0x0001
#define CRTC_INTERLACED                     0x0002
#define CRTC_H_SYNC_POLARITY                0x0004
#define CRTC_V_SYNC_POLARITY                0x0008

static int get_DWORD(int i, char *lp_data);
static int get_DWORD_BCD(int i, char *lp_data);
static void set_DWORD(char *data_string, UINT32 data_word, int offset);
static void set_DWORD_BCD(char *data_string, UINT32 data_word, int offset);
static int os_version(void);
static bool is_elevated();
static int win_interlace_factor(modeline *mode);

char m_device_name[32];
char m_device_key[256];
int win_version;

//============================================================
//  ati_custom_video_init
//============================================================

bool ati_init(char *device_name, char *device_key, char *device_id)
{
	osd_printf_verbose("ATI legacy init\n");

	// Get Windows version
	win_version = os_version();

	if (win_version > 5 && !is_elevated())
	{
		osd_printf_error("ATI legacy error: the program needs administrator rights.\n");
		return false;
	}

	memcpy(m_device_name, device_name, sizeof(m_device_name));
	memcpy(m_device_key, device_key, sizeof(m_device_key));

	return true;
}

//============================================================
//  ati_get_modeline
//============================================================

bool ati_get_modeline(modeline *mode)
{
	HKEY hKey;
	char lp_name[1024];
	char lp_data[68];
	DWORD length;
	bool found = false;
	int refresh_label = mode->refresh_label? mode->refresh_label : mode->refresh * win_interlace_factor(mode);
	int vfreq_incr = 0;

	if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, m_device_key, 0, KEY_ALL_ACCESS, &hKey) == ERROR_SUCCESS)
	{
		sprintf(lp_name, "DALDTMCRTBCD%dx%dx0x%d", mode->width, mode->height, refresh_label);
		length = sizeof(lp_data);

		if (RegQueryValueExA(hKey, lp_name, NULL, NULL, (LPBYTE)lp_data, &length) == ERROR_SUCCESS && length == sizeof(lp_data))
			found = true;
		else if (win_version > 5 && mode->interlace)
		{
			vfreq_incr = 1;
			sprintf(lp_name, "DALDTMCRTBCD%dx%dx0x%d", mode->width, mode->height, refresh_label + vfreq_incr);
			if (RegQueryValueExA(hKey, lp_name, NULL, NULL, (LPBYTE)lp_data, &length) == ERROR_SUCCESS && length == sizeof(lp_data))
				found = true;
		}
		if (found)
		{
			mode->pclock  = get_DWORD_BCD(36, lp_data) * 10000;
			mode->hactive = get_DWORD_BCD(8, lp_data);
			mode->hbegin  = get_DWORD_BCD(12, lp_data);
			mode->hend    = get_DWORD_BCD(16, lp_data) + mode->hbegin;
			mode->htotal  = get_DWORD_BCD(4, lp_data);
			mode->vactive = get_DWORD_BCD(24, lp_data);
			mode->vbegin  = get_DWORD_BCD(28, lp_data);
			mode->vend    = get_DWORD_BCD(32, lp_data) + mode->vbegin;
			mode->vtotal  = get_DWORD_BCD(20, lp_data);
			mode->interlace = (get_DWORD(0, lp_data) & CRTC_INTERLACED)?1:0;
			mode->hsync     = (get_DWORD(0, lp_data) & CRTC_H_SYNC_POLARITY)?0:1;
			mode->vsync     = (get_DWORD(0, lp_data) & CRTC_V_SYNC_POLARITY)?0:1;
			mode->hfreq = mode->pclock / mode->htotal;
			mode->vfreq = mode->hfreq / mode->vtotal * (mode->interlace?2:1);
			mode->refresh_label = refresh_label;

			int checksum = 65535 - get_DWORD(0, lp_data) - mode->htotal - mode->hactive - mode->hend
						- mode->vtotal - mode->vactive - mode->vend - mode->pclock/10000;
			if (checksum != get_DWORD(64, lp_data))
				osd_printf_verbose("bad checksum! ");
		}
		RegCloseKey(hKey);
		return (found);
	}
	osd_printf_info("Failed opening registry entry for mode.\n");
	return false;
}

//============================================================
//  ati_set_modeline
//============================================================

bool ati_set_modeline(modeline *mode)
{
	HKEY hKey;
	char lp_name[1024];
	char lp_data[68];
	long checksum;
	bool found = false;
	int refresh_label = mode->refresh_label? mode->refresh_label : mode->refresh * win_interlace_factor(mode);
	int vfreq_incr = 0;

	memset(lp_data, 0, sizeof(lp_data));
	set_DWORD_BCD(lp_data, (int)mode->pclock/10000, 36);
	set_DWORD_BCD(lp_data, mode->hactive, 8);
	set_DWORD_BCD(lp_data, mode->hbegin, 12);
	set_DWORD_BCD(lp_data, mode->hend - mode->hbegin, 16);
	set_DWORD_BCD(lp_data, mode->htotal, 4);
	set_DWORD_BCD(lp_data, mode->vactive, 24);
	set_DWORD_BCD(lp_data, mode->vbegin, 28);
	set_DWORD_BCD(lp_data, mode->vend - mode->vbegin, 32);
	set_DWORD_BCD(lp_data, mode->vtotal, 20);
	set_DWORD(lp_data, (mode->interlace?CRTC_INTERLACED:0) | (mode->hsync?0:CRTC_H_SYNC_POLARITY) | (mode->vsync?0:CRTC_V_SYNC_POLARITY), 0);

	checksum = 65535 - get_DWORD(0, lp_data) - mode->htotal - mode->hactive - mode->hend
			- mode->vtotal - mode->vactive - mode->vend - mode->pclock/10000;
	set_DWORD(lp_data, checksum, 64);

	if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, m_device_key, 0, KEY_ALL_ACCESS, &hKey) == ERROR_SUCCESS)
	{
		sprintf (lp_name, "DALDTMCRTBCD%dx%dx0x%d", mode->width, mode->height, refresh_label);

		if (RegQueryValueExA(hKey, lp_name, NULL, NULL, NULL, NULL) == ERROR_SUCCESS)
			found = true;
		else if (win_version > 5 && mode->interlace)
		{
			vfreq_incr = 1;
			sprintf(lp_name, "DALDTMCRTBCD%dx%dx0x%d", mode->width, mode->height, refresh_label + vfreq_incr);
			if (RegQueryValueExA(hKey, lp_name, NULL, NULL, NULL, NULL) == ERROR_SUCCESS)
				found = true;
		}

		if (!(found && RegSetValueExA(hKey, lp_name, 0, REG_BINARY, (LPBYTE)lp_data, 68) == ERROR_SUCCESS))
			osd_printf_info("Failed saving registry entry %s\n", lp_name);

		RegCloseKey(hKey);
		return (found);
	}

	osd_printf_info("Failed updating registry entry for mode.\n");
	return 0;
}

//============================================================
//  ati_refresh_timings
//============================================================

void ati_refresh_timings(void)
{
	int iModeNum = 0;
	DEVMODEA lpDevMode;

	memset(&lpDevMode, 0, sizeof(DEVMODEA));
	lpDevMode.dmSize = sizeof(DEVMODEA);

	while (EnumDisplaySettingsExA(m_device_name, iModeNum, &lpDevMode, 0) != 0)
		iModeNum++;
}

//============================================================
// get_DWORD
//============================================================

static int get_DWORD(int i, char *lp_data)
{
	char out[32] = "";
	UINT32 x;

	sprintf(out, "%02X%02X%02X%02X", lp_data[i]&0xFF, lp_data[i+1]&0xFF, lp_data[i+2]&0xFF, lp_data[i+3]&0xFF);
	sscanf(out, "%08X", &x);
	return x;
}

//============================================================
// get_DWORD_BCD
//============================================================

static int get_DWORD_BCD(int i, char *lp_data)
{
	char out[32] = "";
	UINT32 x;

	sprintf(out, "%02X%02X%02X%02X", lp_data[i]&0xFF, lp_data[i+1]&0xFF, lp_data[i+2]&0xFF, lp_data[i+3]&0xFF);
	sscanf(out, "%d", &x);
	return x;
}

//============================================================
// set_DWORD
//============================================================

static void set_DWORD(char *data_string, UINT32 data_dword, int offset)
{
	char *p_dword = (char*)&data_dword;

	data_string[offset]   = p_dword[3]&0xFF;
	data_string[offset+1] = p_dword[2]&0xFF;
	data_string[offset+2] = p_dword[1]&0xFF;
	data_string[offset+3] = p_dword[0]&0xFF;
}

//============================================================
// set_DWORD_BCD
//============================================================

static void set_DWORD_BCD(char *data_string, UINT32 data_dword, int offset)
{
	if (data_dword < 100000000)
	{
		int low_word, high_word;
		int a, b, c, d;
		char out[32] = "";

		low_word = data_dword % 10000;
		high_word = data_dword / 10000;

		sprintf(out, "%d %d %d %d", high_word / 100, high_word % 100 , low_word / 100, low_word % 100);
		sscanf(out, "%02X %02X %02X %02X", &a, &b, &c, &d);

		data_string[offset]   = a;
		data_string[offset+1] = b;
		data_string[offset+2] = c;
		data_string[offset+3] = d;
	}
}

//============================================================
// os_version
//============================================================

static int os_version(void)
{
	OSVERSIONINFOA lpVersionInfo;

	memset(&lpVersionInfo, 0, sizeof(OSVERSIONINFOA));
	lpVersionInfo.dwOSVersionInfoSize = sizeof(OSVERSIONINFOA);
	GetVersionExA (&lpVersionInfo);

	return lpVersionInfo.dwMajorVersion;
}

//============================================================
//  is_elevated
//============================================================

static bool is_elevated()
{
	HANDLE htoken;
	bool result = false;

	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &htoken))
		return false;

	TOKEN_ELEVATION te = {0};
	DWORD dw_return_length;

	if (GetTokenInformation(htoken, TokenElevation, &te, sizeof(te), &dw_return_length))
	{
		if (te.TokenIsElevated)
		{
			result = true;
		}
	}

	CloseHandle(htoken);
	return (result);
}

//============================================================
// win_interlace_factor
//============================================================

static int win_interlace_factor(modeline *mode)
{
	if (win_version > 5 && mode->interlace)
		return 2;

	return 1;
}
