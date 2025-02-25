/**************************************************************

   monitor.cpp - Monitor presets and custom monitor definition

   ---------------------------------------------------------

   SwitchRes   Modeline generation engine for emulation

   GroovyMAME  Integration of SwitchRes into the MAME project
               Some reworked patches from SailorSat's CabMAME

   License     GPL-2.0+
   Copyright   2010-2016 - Chris Kennedy, Antonio Giner

 **************************************************************/

#include "emu.h"

//============================================================
//  CONSTANTS
//============================================================

#define HFREQ_MIN  14000
#define HFREQ_MAX  100000
#define VFREQ_MIN  40
#define VFREQ_MAX  200
#define PROGRESSIVE_LINES_MIN 128

//============================================================
//  monitor_fill_range
//============================================================

int monitor_fill_range(monitor_range *range, const char *specs_line)
{
	monitor_range new_range;

	if (strcmp(specs_line, "auto")) {
		int e = sscanf(specs_line, "%lf-%lf,%lf-%lf,%lf,%lf,%lf,%lf,%lf,%lf,%d,%d,%d,%d,%d,%d",
			&new_range.hfreq_min, &new_range.hfreq_max,
			&new_range.vfreq_min, &new_range.vfreq_max,
			&new_range.hfront_porch, &new_range.hsync_pulse, &new_range.hback_porch,
			&new_range.vfront_porch, &new_range.vsync_pulse, &new_range.vback_porch,
			&new_range.hsync_polarity, &new_range.vsync_polarity,
			&new_range.progressive_lines_min, &new_range.progressive_lines_max,
			&new_range.interlaced_lines_min, &new_range.interlaced_lines_max);

		if (e != 16) {
			osd_printf_error("SwitchRes: Error trying to fill monitor range with\n  %s\n", specs_line);
			return -1;
		}

		new_range.vfront_porch /= 1000;
		new_range.vsync_pulse /= 1000;
		new_range.vback_porch /= 1000;
		new_range.vertical_blank = (new_range.vfront_porch + new_range.vsync_pulse + new_range.vback_porch);

		if (monitor_evaluate_range(&new_range))
		{
			osd_printf_error("SwitchRes: Error in monitor range (ignoring): %s\n", specs_line);
			return -1;
		}
		else
		{
			memcpy(range, &new_range, sizeof(struct monitor_range));
			monitor_show_range(range);
		}
	}
	return 0;
}

//============================================================
//  monitor_fill_lcd_range
//============================================================

int monitor_fill_lcd_range(monitor_range *range, const char *specs_line)
{
	if (strcmp(specs_line, "auto"))
	{
		if (sscanf(specs_line, "%lf-%lf", &range->vfreq_min, &range->vfreq_max) == 2)
		{
			osd_printf_verbose("SwitchRes: LCD vfreq range set by user as %f-%f\n", range->vfreq_min, range->vfreq_max);
			return true;
		}
		else
			osd_printf_error("SwitchRes: Error trying to fill LCD range with\n  %s\n", specs_line);
	}
	// Use default values
	range->vfreq_min = 59;
	range->vfreq_max = 61;
	osd_printf_verbose("SwitchRes: Using default vfreq range for LCD %f-%f\n", range->vfreq_min, range->vfreq_max);

	return 0;
}

//============================================================
//  monitor_fill_vesa_gtf
//============================================================

int monitor_fill_vesa_gtf(monitor_range *range, const char *max_lines)
{
	int lines = 0;
	sscanf(max_lines, "vesa_%d", &lines);

	if (!lines)
		return 0;

	int i = 0;
	if (lines >= 480)
		i += monitor_fill_vesa_range(&range[i], 384, 480);
	if (lines >= 600)
		i += monitor_fill_vesa_range(&range[i], 480, 600);
	if (lines >= 768)
		i += monitor_fill_vesa_range(&range[i], 600, 768);
	if (lines >= 1024)
		i += monitor_fill_vesa_range(&range[i], 768, 1024);

	return i;
}

//============================================================
//  monitor_fill_vesa_range
//============================================================

int monitor_fill_vesa_range(monitor_range *range, int lines_min, int lines_max)
{
	modeline mode;
	memset(&mode, 0, sizeof(modeline));

	mode.width = real_res(STANDARD_CRT_ASPECT * lines_max);
	mode.height = lines_max;
	mode.refresh = 60;
	range->vfreq_min = 50;
	range->vfreq_max = 65;

	modeline_vesa_gtf(&mode);
	modeline_to_monitor_range(range, &mode);

	range->progressive_lines_min = lines_min;
	range->hfreq_min = mode.hfreq - 500;
	range->hfreq_max = mode.hfreq + 500;
	monitor_show_range(range);

	return 1;
}

//============================================================
//  monitor_show_range
//============================================================

int monitor_show_range(monitor_range *range)
{
	osd_printf_verbose("SwitchRes: Monitor range %.2f-%.2f,%.2f-%.2f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%d,%d,%d,%d,%d,%d\n",
		range->hfreq_min, range->hfreq_max,
		range->vfreq_min, range->vfreq_max,
		range->hfront_porch, range->hsync_pulse, range->hback_porch,
		range->vfront_porch * 1000, range->vsync_pulse * 1000, range->vback_porch * 1000,
		range->hsync_polarity, range->vsync_polarity,
		range->progressive_lines_min, range->progressive_lines_max,
		range->interlaced_lines_min, range->interlaced_lines_max);

	return 0;
}

//============================================================
//  monitor_set_preset
//============================================================

int monitor_set_preset(char *type, monitor_range *range)
{
	// PAL TV - 50 Hz/625
	if (!strcmp(type, "pal"))
	{
		monitor_fill_range(&range[0], "15625.00-15625.00, 50.00-50.00, 1.500, 4.700, 5.800, 0.064, 0.160, 1.056, 0, 0, 192, 288, 448, 576");
		return 1;
	}
	// NTSC TV - 60 Hz/525
	else if (!strcmp(type, "ntsc"))
	{
		monitor_fill_range(&range[0], "15734.26-15734.26, 59.94-59.94, 1.500, 4.700, 4.700, 0.191, 0.191, 0.953, 0, 0, 192, 240, 448, 480");
		return 1;
	}
	// Generic 15.7 kHz
	else if (!strcmp(type, "generic_15"))
	{
		monitor_fill_range(&range[0], "15625-15750, 49.50-65.00, 2.000, 4.700, 8.000, 0.064, 0.192, 1.024, 0, 0, 192, 288, 448, 576");
		return 1;
	}
	// Arcade 15.7 kHz - standard resolution
	else if (!strcmp(type, "arcade_15"))
	{
		monitor_fill_range(&range[0], "15625-16200, 49.50-65.00, 2.000, 4.700, 8.000, 0.064, 0.192, 1.024, 0, 0, 192, 288, 448, 576");
		return 1;
	}
	// Arcade 15.7-16.5 kHz - extended resolution
	else if (!strcmp(type, "arcade_15ex"))
	{
		monitor_fill_range(&range[0], "15625-16500, 49.50-65.00, 2.000, 4.700, 8.000, 0.064, 0.192, 1.024, 0, 0, 192, 288, 448, 576");
		return 1;
	}
	// Arcade 25.0 kHz - medium resolution
	else if (!strcmp(type, "arcade_25"))
	{
		monitor_fill_range(&range[0], "24960-24960, 49.50-65.00, 0.800, 4.000, 3.200, 0.080, 0.200, 1.000, 0, 0, 384, 400, 768, 800");
		return 1;
	}
	// Arcade 31.5 kHz - medium resolution
	else if (!strcmp(type, "arcade_31"))
	{
		monitor_fill_range(&range[0], "31400-31500, 49.50-65.00, 0.940, 3.770, 1.890, 0.349, 0.064, 1.017, 0, 0, 400, 512, 0, 0");
		return 1;
	}
	// Arcade 15.7/25.0 kHz - dual-sync
	else if (!strcmp(type, "arcade_15_25"))
	{
		monitor_fill_range(&range[0], "15625-16200, 49.50-65.00, 2.000, 4.700, 8.000, 0.064, 0.192, 1.024, 0, 0, 192, 288, 448, 576");
		monitor_fill_range(&range[1], "24960-24960, 49.50-65.00, 0.800, 4.000, 3.200, 0.080, 0.200, 1.000, 0, 0, 384, 400, 768, 800");
		return 2;
	}
	// Arcade 15.7/25.0/31.5 kHz - tri-sync
	else if (!strcmp(type, "arcade_15_25_31"))
	{
		monitor_fill_range(&range[0], "15625-16200, 49.50-65.00, 2.000, 4.700, 8.000, 0.064, 0.192, 1.024, 0, 0, 192, 288, 448, 576");
		monitor_fill_range(&range[1], "24960-24960, 49.50-65.00, 0.800, 4.000, 3.200, 0.080, 0.200, 1.000, 0, 0, 384, 400, 768, 800");
		monitor_fill_range(&range[2], "31400-31500, 49.50-65.00, 0.940, 3.770, 1.890, 0.349, 0.064, 1.017, 0, 0, 400, 512, 0, 0");
		return 3;
	}
	// Makvision 2929D
	else if (!strcmp(type, "m2929"))
	{
		monitor_fill_range(&range[0], "30000-40000, 47.00-90.00, 0.600, 2.500, 2.800, 0.032, 0.096, 0.448, 0, 0, 384, 640, 0, 0");
		return 1;
	}
	// Wells Gardner D9800, D9400
	else if (!strcmp(type, "d9800") || !strcmp(type, "d9400"))
	{
		monitor_fill_range(&range[0], "15250-18000, 40-80, 2.187, 4.688, 6.719, 0.190, 0.191, 1.018, 0, 0, 224, 288, 448, 576");
		monitor_fill_range(&range[1], "18001-19000, 40-80, 2.187, 4.688, 6.719, 0.140, 0.191, 0.950, 0, 0, 288, 320, 0, 0");
		monitor_fill_range(&range[2], "20501-29000, 40-80, 2.910, 3.000, 4.440, 0.451, 0.164, 1.048, 0, 0, 320, 384, 0, 0");
		monitor_fill_range(&range[3], "29001-32000, 40-80, 0.636, 3.813, 1.906, 0.318, 0.064, 1.048, 0, 0, 384, 480, 0, 0");
		monitor_fill_range(&range[4], "32001-34000, 40-80, 0.636, 3.813, 1.906, 0.020, 0.106, 0.607, 0, 0, 480, 576, 0, 0");
		monitor_fill_range(&range[5], "34001-38000, 40-80, 1.000, 3.200, 2.200, 0.020, 0.106, 0.607, 0, 0, 576, 600, 0, 0");
		return 6;
	}
	// Wells Gardner D9200
	else if (!strcmp(type, "d9200"))
	{
		monitor_fill_range(&range[0], "15250-16500, 40-80, 2.187, 4.688, 6.719, 0.190, 0.191, 1.018, 0, 0, 224, 288, 448, 576");
		monitor_fill_range(&range[1], "23900-24420, 40-80, 2.910, 3.000, 4.440, 0.451, 0.164, 1.148, 0, 0, 384, 400, 0, 0");
		monitor_fill_range(&range[2], "31000-32000, 40-80, 0.636, 3.813, 1.906, 0.318, 0.064, 1.048, 0, 0, 400, 512, 0, 0");
		monitor_fill_range(&range[3], "37000-38000, 40-80, 1.000, 3.200, 2.200, 0.020, 0.106, 0.607, 0, 0, 512, 600, 0, 0");
		return 4;
	}
	// Wells Gardner K7000
	else if (!strcmp(type, "k7000"))
	{
		monitor_fill_range(&range[0], "15625-15800, 49.50-63.00, 2.000, 4.700, 8.000, 0.064, 0.160, 1.056, 0, 0, 192, 288, 448, 576");
		return 1;
	}
	// Wells Gardner 25K7131
	else if (!strcmp(type, "k7131"))
	{
		monitor_fill_range(&range[0], "15625-16670, 49.5-65, 2.000, 4.700, 8.000, 0.064, 0.160, 1.056, 0, 0, 192, 288, 448, 576");
		return 1;
	}
	// Wei-Ya M3129
	else if (!strcmp(type, "m3129"))
	{
		monitor_fill_range(&range[0], "15250-16500, 40-80, 2.187, 4.688, 6.719, 0.190, 0.191, 1.018, 1, 1, 192, 288, 448, 576");
		monitor_fill_range(&range[1], "23900-24420, 40-80, 2.910, 3.000, 4.440, 0.451, 0.164, 1.048, 1, 1, 384, 400, 0, 0");
		monitor_fill_range(&range[2], "31000-32000, 40-80, 0.636, 3.813, 1.906, 0.318, 0.064, 1.048, 1, 1, 400, 512, 0, 0");
		return 3;
	}
	// Hantarex MTC 9110
	else if (!strcmp(type, "h9110") || !strcmp(type, "polo"))
	{
		monitor_fill_range(&range[0], "15625-16670, 49.5-65, 2.000, 4.700, 8.000, 0.064, 0.160, 1.056, 0, 0, 192, 288, 448, 576");
		return 1;
	}
	// Hantarex Polostar 25
	else if (!strcmp(type, "pstar"))
	{
		monitor_fill_range(&range[0], "15700-15800, 50-65, 1.800, 0.400, 7.400, 0.064, 0.160, 1.056, 0, 0, 192, 256, 0, 0");
		monitor_fill_range(&range[1], "16200-16300, 50-65, 0.200, 0.400, 8.000, 0.040, 0.040, 0.640, 0, 0, 256, 264, 512, 528");
		monitor_fill_range(&range[2], "25300-25400, 50-65, 0.200, 0.400, 8.000, 0.040, 0.040, 0.640, 0, 0, 384, 400, 768, 800");
		monitor_fill_range(&range[3], "31500-31600, 50-65, 0.170, 0.350, 5.500, 0.040, 0.040, 0.640, 0, 0, 400, 512, 0, 0");
		return 4;
	}
	// Nanao MS-2930, MS-2931
	else if (!strcmp(type, "ms2930"))
	{
		monitor_fill_range(&range[0], "15450-16050, 50-65, 3.190, 4.750, 6.450, 0.191, 0.191, 1.164, 0, 0, 192, 288, 448, 576");
		monitor_fill_range(&range[1], "23900-24900, 50-65, 2.870, 3.000, 4.440, 0.451, 0.164, 1.148, 0, 0, 384, 400, 0, 0");
    	monitor_fill_range(&range[2], "31000-32000, 50-65, 0.330, 3.580, 1.750, 0.316, 0.063, 1.137, 0, 0, 480, 512, 0, 0");
		return 3;
	}
	// Nanao MS9-29
	else if (!strcmp(type, "ms929"))
	{
		monitor_fill_range(&range[0], "15450-16050, 50-65, 3.910, 4.700, 6.850, 0.190, 0.191, 1.018, 0, 0, 192, 288, 448, 576");
		monitor_fill_range(&range[1], "23900-24900, 50-65, 2.910, 3.000, 4.440, 0.451, 0.164, 1.048, 0, 0, 384, 400, 0, 0");
		return 2;
	}
	// Rodotron 666B-29
	else if (!strcmp(type, "r666b"))
	{
		monitor_fill_range(&range[0], "15450-16050, 50-65, 3.190, 4.750, 6.450, 0.191, 0.191, 1.164, 0, 0, 192, 288, 448, 576");
		monitor_fill_range(&range[1], "23900-24900, 50-65, 2.870, 3.000, 4.440, 0.451, 0.164, 1.148, 0, 0, 384, 400, 0, 0");
		monitor_fill_range(&range[2], "31000-32500, 50-65, 0.330, 3.580, 1.750, 0.316, 0.063, 1.137, 0, 0, 400, 512, 0, 0");
		return 3;
	}
	// PC CRT 70kHz/120Hz
	else if (!strcmp(type, "pc_31_120"))
	{
		monitor_fill_range(&range[0], "31400-31600, 100-130, 0.671, 2.683, 3.353, 0.034, 0.101, 0.436, 0, 0, 200, 256, 0, 0");
		monitor_fill_range(&range[1], "31400-31600, 50-65, 0.671, 2.683, 3.353, 0.034, 0.101, 0.436, 0, 0, 400, 512, 0, 0");
		return 2;
	}
	// PC CRT 70kHz/120Hz
	else if (!strcmp(type, "pc_70_120"))
	{
		monitor_fill_range(&range[0], "30000-70000, 100-130, 2.201, 0.275, 4.678, 0.063, 0.032, 0.633, 0, 0, 192, 320, 0, 0");
		monitor_fill_range(&range[1], "30000-70000, 50-65, 2.201, 0.275, 4.678, 0.063, 0.032, 0.633, 0, 0, 400, 1024, 0, 0");
		return 2;
	}
	// VESA GTF
	else if (!strcmp(type, "vesa_480") || !strcmp(type, "vesa_600") || !strcmp(type, "vesa_768") || !strcmp(type, "vesa_1024"))
	{
		return monitor_fill_vesa_gtf(&range[0], type);
	}

	osd_printf_error("SwitchRes: Monitor type unknown: %s\n", type);
	return 0;
}

//============================================================
//  monitor_evaluate_range
//============================================================

int monitor_evaluate_range(monitor_range *range)
{
	// First we check that all frequency ranges are reasonable
	if (range->hfreq_min < HFREQ_MIN || range->hfreq_min > HFREQ_MAX)
	{
		osd_printf_error("SwitchRes: hfreq_min %.2f out of range\n", range->hfreq_min);
		return 1;
	}
	if (range->hfreq_max < HFREQ_MIN || range->hfreq_max < range->hfreq_min || range->hfreq_max > HFREQ_MAX)
	{
		osd_printf_error("SwitchRes: hfreq_max %.2f out of range\n", range->hfreq_max);
		return 1;
	}
	if (range->vfreq_min < VFREQ_MIN || range->vfreq_min > VFREQ_MAX)
	{
		osd_printf_error("SwitchRes: vfreq_min %.2f out of range\n", range->vfreq_min);
		return 1;
	}
	if (range->vfreq_max < VFREQ_MIN || range->vfreq_max < range->vfreq_min || range->vfreq_max > VFREQ_MAX)
	{
		osd_printf_error("SwitchRes: vfreq_max %.2f out of range\n", range->vfreq_max);
		return 1;
	}

	// line_time in �s. We check that no horizontal value is longer than a whole line
	double line_time = 1 / range->hfreq_max * 1000000;

	if (range->hfront_porch <= 0 || range->hfront_porch > line_time)
	{
		osd_printf_error("SwitchRes: hfront_porch %.3f out of range\n", range->hfront_porch);
		return 1;
	}
	if (range->hsync_pulse <= 0 || range->hsync_pulse > line_time)
	{
		osd_printf_error("SwitchRes: hsync_pulse %.3f out of range\n", range->hsync_pulse);
		return 1;
	}
	if (range->hback_porch <= 0 || range->hback_porch > line_time)
	{
		osd_printf_error("SwitchRes: hback_porch %.3f out of range\n", range->hback_porch);
		return 1;
	}

	// frame_time in ms. We check that no vertical value is longer than a whole frame
	double frame_time = 1 / range->vfreq_max * 1000;

	if (range->vfront_porch <= 0 || range->vfront_porch > frame_time)
	{
		osd_printf_error("SwitchRes: vfront_porch %.3f out of range\n", range->vfront_porch);
		return 1;
	}
	if (range->vsync_pulse <= 0 || range->vsync_pulse > frame_time)
	{
		osd_printf_error("SwitchRes: vsync_pulse %.3f out of range\n", range->vsync_pulse);
		return 1;
	}
	if (range->vback_porch <= 0 || range->vback_porch > frame_time)
	{
		osd_printf_error("SwitchRes: vback_porch %.3f out of range\n", range->vback_porch);
		return 1;
	}

	// Now we check sync polarities
	if (range->hsync_polarity != 0 && range->hsync_polarity != 1)
	{
		osd_printf_error("SwitchRes: Hsync polarity can be only 0 or 1\n");
		return 1;
	}
	if (range->vsync_polarity != 0 && range->vsync_polarity != 1)
	{
		osd_printf_error("SwitchRes: Vsync polarity can be only 0 or 1\n");
		return 1;
	}

	// Finally we check that the line limiters are reasonable
	// Progressive range:
	if (range->progressive_lines_min > 0 && range->progressive_lines_min < PROGRESSIVE_LINES_MIN)
	{
		osd_printf_error("SwitchRes: progressive_lines_min must be greater than %d\n", PROGRESSIVE_LINES_MIN);
		return 1;
	}
	if ((range->progressive_lines_min + range->hfreq_max * range->vertical_blank) * range->vfreq_min > range->hfreq_max)
	{
		osd_printf_error("SwitchRes: progressive_lines_min %d out of range\n", range->progressive_lines_min);
		return 1;
	}
	if (range->progressive_lines_max < range->progressive_lines_min)
	{
		osd_printf_error("SwitchRes: progressive_lines_max must greater than progressive_lines_min\n");
		return 1;
	}
	if ((range->progressive_lines_max + range->hfreq_max * range->vertical_blank) * range->vfreq_min > range->hfreq_max)
	{
		osd_printf_error("SwitchRes: progressive_lines_max %d out of range\n", range->progressive_lines_max);
		return 1;
	}

	// Interlaced range:
	if (range->interlaced_lines_min != 0)
	{
		if (range->interlaced_lines_min < range->progressive_lines_max)
		{
			osd_printf_error("SwitchRes: interlaced_lines_min must greater than progressive_lines_max\n");
			return 1;
		}
		if (range->interlaced_lines_min < PROGRESSIVE_LINES_MIN * 2)
		{
			osd_printf_error("SwitchRes: interlaced_lines_min must be greater than %d\n", PROGRESSIVE_LINES_MIN * 2);
			return 1;
		}
		if ((range->interlaced_lines_min / 2 + range->hfreq_max * range->vertical_blank) * range->vfreq_min > range->hfreq_max)
		{
			osd_printf_error("SwitchRes: interlaced_lines_min %d out of range\n", range->interlaced_lines_min);
			return 1;
		}
		if (range->interlaced_lines_max < range->interlaced_lines_min)
		{
			osd_printf_error("SwitchRes: interlaced_lines_max must greater than interlaced_lines_min\n");
			return 1;
		}
		if ((range->interlaced_lines_max / 2 + range->hfreq_max * range->vertical_blank) * range->vfreq_min > range->hfreq_max)
		{
			osd_printf_error("SwitchRes: interlaced_lines_max %d out of range\n", range->interlaced_lines_max);
			return 1;
		}
	}
	else
	{
		if (range->interlaced_lines_max != 0)
		{
			osd_printf_error("SwitchRes: interlaced_lines_max must be zero if interlaced_lines_min is not defined\n");
			return 1;
		}
	}
	return 0;
}
