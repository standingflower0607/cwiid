/* Copyright (C) 2007 L. Donnie Smith <wiimote@abstrakraft.org>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 *  ChangeLog:
 *  2007-04-09 L. Donnie Smith <cwiid@abstrakraft.org>
 *  * updated for libcwiid rename
 *
 *  2007-04-04 L. Donnie Smith <cwiid@abstrakraft.org>
 *  * exit on cwiid_error
 *
 *  2007-03-03 L. Donnie Smith <cwiid@abstrakraft.org>
 *  * Initial ChangeLog
 *  * type audit (stdint, const, char booleans)
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <pthread.h>
#include <signal.h>
#include <sys/types.h>
#include <unistd.h>

#include <cwiid.h>

#include "conf.h"
#include "util.h"
#include "wmplugin.h"

struct conf conf;

/* GetOpt */
#define OPTSTRING	"hwc:"
extern char *optarg;
extern int optind, opterr, optopt;

/* Prototypes */
cwiid_mesg_callback_t cwiid_callback;
int wminput_set_report_mode();
void process_btn_mesg(struct cwiid_btn_mesg *mesg);
void process_nunchuk_mesg(struct cwiid_nunchuk_mesg *mesg);
void process_classic_mesg(struct cwiid_classic_mesg *mesg);
void process_plugin(struct plugin *, int, union cwiid_mesg * []);

/* Globals */
cwiid_wiimote_t *wiimote;
char init;

#define DEFAULT_CONFIG_FILE	"default"

#define USAGE "usage:%s [-h] [-w] [-c config] [bdaddr]\n"

#define HOME_DIR_LEN	128
int main(int argc, char *argv[])
{
	char wait_forever = 0;
	char *config_search_dirs[3], *plugin_search_dirs[3];
	char *config_filename = DEFAULT_CONFIG_FILE;
	char home_config_dir[HOME_DIR_LEN];
	char home_plugin_dir[HOME_DIR_LEN];
	char *tmp;
	int c, i;
	char *str_addr;
	bdaddr_t bdaddr;
	sigset_t sigset;
	int signum, ret=0;
	struct uinput_listen_data uinput_listen_data;
	pthread_t uinput_listen_thread;

	init = 1;

	/* Parse Options */
	while ((c = getopt(argc, argv, OPTSTRING)) != -1) {
		switch (c) {
		case 'h':
			printf(USAGE, argv[0]);
			return 0;
			break;
		case 'w':
			wait_forever = 1;
			break;
		case 'c':
			config_filename = optarg;
			break;
		case '?':
		default:
			return -1;
			break;
		}
	}

	/* Load Config */
	if ((tmp = getenv("HOME")) == NULL) {
		wminput_err("unable to find home directory");
		config_search_dirs[0] = WMINPUT_CONFIG_DIR;
		plugin_search_dirs[0] = CWIID_PLUGINS_DIR;
		config_search_dirs[1] = plugin_search_dirs[1] = NULL;
	}
	else {
		snprintf(home_config_dir, HOME_DIR_LEN, "%s/.cwiid/wminput", tmp);
		snprintf(home_plugin_dir, HOME_DIR_LEN, "%s/.cwiid/plugins", tmp);
		config_search_dirs[0] = home_config_dir;
		plugin_search_dirs[0] = home_plugin_dir;
		config_search_dirs[1] = WMINPUT_CONFIG_DIR;
		plugin_search_dirs[1] = CWIID_PLUGINS_DIR;
		config_search_dirs[2] = plugin_search_dirs[2] = NULL;
	}

	if (conf_load(&conf, config_filename, config_search_dirs,
	  plugin_search_dirs)) {
		return -1;
	}

	/* BDADDR */
	if (optind < argc) {
		if (str2ba(argv[optind], &bdaddr)) {
			wminput_err("invalid bdaddr");
			bdaddr = *BDADDR_ANY;
		}
		optind++;
		if (optind < argc) {
			wminput_err("invalid command-line");
			printf(USAGE, argv[0]);
			conf_unload(&conf);
			return -1;
		}
	}
	else if ((str_addr = getenv(CWIID_BDADDR)) != NULL) {
		if (str2ba(str_addr, &bdaddr)) {
			wminput_err("invalid address in %s", CWIID_BDADDR);
			bdaddr = *BDADDR_ANY;
		}
	}
	else {
		bdaddr = *BDADDR_ANY;
	}

	/* Wiimote connect */
	printf("Put Wiimote in discoverable mode now (press 1+2)...\n");
	if (wait_forever) {
		if (cwiid_find_wiimote(&bdaddr, -1)) {
			wminput_err("error finding wiimote");
			conf_unload(&conf);
			return -1;
		}
	}
	if ((wiimote = cwiid_connect(&bdaddr, cwiid_callback, NULL)) == NULL) {
		wminput_err("unable to connect");
		conf_unload(&conf);
		return -1;
	}

	/* init plugins */
	for (i=0; (i < CONF_MAX_PLUGINS) && conf.plugins[i].name; i++) {
		if ((*conf.plugins[i].init)(i, wiimote)) {
			wminput_err("error on %s init", conf.plugins[i].name);
			conf_unload(&conf);
			cwiid_disconnect(wiimote);
			return -1;
		}
	}

	if (wminput_set_report_mode()) {
		conf_unload(&conf);
		cwiid_disconnect(wiimote);
		return -1;
	}

	uinput_listen_data.wiimote = wiimote;
	uinput_listen_data.conf = &conf;
	if (pthread_create(&uinput_listen_thread, NULL,
	                   (void *(*)(void *))uinput_listen,
	                   &uinput_listen_data)) {
		wminput_err("error starting uinput listen thread");
		conf_unload(&conf);
		cwiid_disconnect(wiimote);
		return -1;
	}


	printf("Ready.\n");

	init = 0;

	/* wait */
	sigemptyset(&sigset);
	sigaddset(&sigset, SIGTERM);
	sigaddset(&sigset, SIGINT);
	sigprocmask(SIG_BLOCK, &sigset, NULL);
	sigwait(&sigset, &signum);

	printf("Exiting.\n");

	if (pthread_cancel(uinput_listen_thread)) {
		wminput_err("error canceling uinput listen thread");
		ret = -1;
	}
	else if (pthread_join(uinput_listen_thread, NULL)) {
		wminput_err("error joing uinput listen thread");
		ret = -1;
	}

	/* disconnect */
	if (cwiid_disconnect(wiimote)) {
		wminput_err("error on disconnect");
		ret = -1;
	}

	if (conf_unload(&conf)) {
		ret = -1;
	}

	return ret;
}

int wmplugin_set_report_mode(int id, uint8_t flags)
{
	conf.plugins[id].rpt_mode_flags = flags;

	if (!init) {
		wminput_set_report_mode();
	}

	return 0;
}

int wminput_set_report_mode()
{
	unsigned char rpt_mode_flags;
	int i;

	rpt_mode_flags = conf.rpt_mode_flags;

	for (i=0; (i < CONF_MAX_PLUGINS) && conf.plugins[i].name; i++) {
		rpt_mode_flags |= conf.plugins[i].rpt_mode_flags;
	}

	if (cwiid_command(wiimote, CWIID_CMD_RPT_MODE, rpt_mode_flags)) {
		wminput_err("error setting report mode");
		return -1;
	}

	return 0;
}

void cwiid_callback(int id, int mesg_count, union cwiid_mesg *mesg[])
{
	int i;

	for (i=0; i < mesg_count; i++) {
		switch (mesg[i]->type) {
		case CWIID_MESG_BTN:
			process_btn_mesg((struct cwiid_btn_mesg *) mesg[i]);
			break;
		case CWIID_MESG_NUNCHUK:
			process_nunchuk_mesg((struct cwiid_nunchuk_mesg *) mesg[i]);
			break;
		case CWIID_MESG_CLASSIC:
			process_classic_mesg((struct cwiid_classic_mesg *) mesg[i]);
			break;
		case CWIID_MESG_ERROR:
			if (kill(getpid(),SIGINT)) {
				wminput_err("error sending SIGINT");
			}
			break;
		default:
			break;
		}
	}
	for (i=0; (i < CONF_MAX_PLUGINS) && conf.plugins[i].name; i++) {
		process_plugin(&conf.plugins[i], mesg_count, mesg);
	}
	send_event(&conf, EV_SYN, SYN_REPORT, 0);
}

void process_btn_mesg(struct cwiid_btn_mesg *mesg)
{
	static uint16_t prev_buttons = 0;
	uint16_t pressed, released;
	__s32 axis_value;
	int i;

	/* Wiimote Button/Key Events */
	pressed = mesg->buttons & ~prev_buttons;
	released = ~mesg->buttons & prev_buttons;
	for (i=0; i < CONF_WM_BTN_COUNT; i++) {
		if (conf.wiimote_bmap[i].active) {
			if (pressed & conf.wiimote_bmap[i].mask) {
				send_event(&conf, EV_KEY, conf.wiimote_bmap[i].action, 1);
			}
			else if (released & conf.wiimote_bmap[i].mask) {
				send_event(&conf, EV_KEY, conf.wiimote_bmap[i].action, 0);
			}
		}
	}
	prev_buttons = mesg->buttons;

	/* Wiimote.Dpad.X */
	if (conf.amap[CONF_WM_AXIS_DPAD_X].active) {
		axis_value = 0;
		if (mesg->buttons & CWIID_BTN_LEFT) {
			axis_value = -1;
		}
		else if (mesg->buttons & CWIID_BTN_RIGHT) {
			axis_value = 1;
		}
		if (conf.amap[CONF_WM_AXIS_DPAD_X].flags & CONF_INVERT) {
			axis_value *= -1;
		}
		send_event(&conf, conf.amap[CONF_WM_AXIS_DPAD_X].axis_type,
		           conf.amap[CONF_WM_AXIS_DPAD_X].action, axis_value);
	}

	/* Wiimote.Dpad.Y */
	if (conf.amap[CONF_WM_AXIS_DPAD_Y].active) {
		axis_value = 0;
		if (mesg->buttons & CWIID_BTN_DOWN) {
			axis_value = -1;
		}
		else if (mesg->buttons & CWIID_BTN_UP) {
			axis_value = 1;
		}
		if (conf.amap[CONF_WM_AXIS_DPAD_Y].flags & CONF_INVERT) {
			axis_value *= -1;
		}
		send_event(&conf, conf.amap[CONF_WM_AXIS_DPAD_Y].axis_type,
		           conf.amap[CONF_WM_AXIS_DPAD_Y].action, axis_value);
	}
}

void process_nunchuk_mesg(struct cwiid_nunchuk_mesg *mesg)
{
	static uint8_t prev_buttons = 0;
	uint8_t pressed, released;
	__s32 axis_value;
	int i;

	/* Nunchuk Button/Key Events */
	pressed = mesg->buttons & ~prev_buttons;
	released = ~mesg->buttons & prev_buttons;
	for (i=0; i < CONF_NC_BTN_COUNT; i++) {
		if (conf.nunchuk_bmap[i].active) {
			if (pressed & conf.nunchuk_bmap[i].mask) {
				send_event(&conf, EV_KEY, conf.nunchuk_bmap[i].action, 1);
			}
			else if (released & conf.nunchuk_bmap[i].mask) {
				send_event(&conf, EV_KEY, conf.nunchuk_bmap[i].action, 0);
			}
		}
	}
	prev_buttons = mesg->buttons;

	/* Nunchuk.Stick.X */
	if (conf.amap[CONF_NC_AXIS_STICK_X].active) {
		axis_value = mesg->stick_x;
		if (conf.amap[CONF_NC_AXIS_STICK_X].flags & CONF_INVERT) {
			axis_value = 0xFF - axis_value;
		}
		send_event(&conf, conf.amap[CONF_NC_AXIS_STICK_X].axis_type,
		           conf.amap[CONF_NC_AXIS_STICK_X].action, axis_value);
	}

	/* Nunchuk.Stick.Y */
	if (conf.amap[CONF_NC_AXIS_STICK_Y].active) {
		axis_value = mesg->stick_y;
		if (conf.amap[CONF_NC_AXIS_STICK_Y].flags & CONF_INVERT) {
			axis_value = 0xFF - axis_value;
		}
		send_event(&conf, conf.amap[CONF_NC_AXIS_STICK_Y].axis_type,
		           conf.amap[CONF_NC_AXIS_STICK_Y].action, axis_value);
	}
}

void process_classic_mesg(struct cwiid_classic_mesg *mesg)
{
	static uint16_t prev_buttons = 0;
	uint16_t pressed, released;
	__s32 axis_value;
	int i;

	/* Classic Button/Key Events */
	pressed = mesg->buttons & ~prev_buttons;
	released = ~mesg->buttons & prev_buttons;
	for (i=0; i < CONF_CC_BTN_COUNT; i++) {
		if (conf.classic_bmap[i].active) {
			if (pressed & conf.classic_bmap[i].mask) {
				send_event(&conf, EV_KEY, conf.classic_bmap[i].action, 1);
			}
			else if (released & conf.classic_bmap[i].mask) {
				send_event(&conf, EV_KEY, conf.classic_bmap[i].action, 0);
			}
		}
	}
	prev_buttons = mesg->buttons;

	/* Classic.Dpad.X */
	if (conf.amap[CONF_CC_AXIS_DPAD_X].active) {
		axis_value = 0;
		if (mesg->buttons & CWIID_CLASSIC_BTN_LEFT) {
			axis_value = -1;
		}
		else if (mesg->buttons & CWIID_CLASSIC_BTN_RIGHT) {
			axis_value = 1;
		}
		if (conf.amap[CONF_CC_AXIS_DPAD_X].flags & CONF_INVERT) {
			axis_value *= -1;
		}
		send_event(&conf, conf.amap[CONF_CC_AXIS_DPAD_X].axis_type,
		           conf.amap[CONF_CC_AXIS_DPAD_X].action, axis_value);
	}

	/* Classic.Dpad.Y */
	if (conf.amap[CONF_CC_AXIS_DPAD_Y].active) {
		axis_value = 0;
		if (mesg->buttons & CWIID_CLASSIC_BTN_DOWN) {
			axis_value = -1;
		}
		else if (mesg->buttons & CWIID_CLASSIC_BTN_UP) {
			axis_value = 1;
		}
		if (conf.amap[CONF_CC_AXIS_DPAD_Y].flags & CONF_INVERT) {
			axis_value *= -1;
		}
		send_event(&conf, conf.amap[CONF_CC_AXIS_DPAD_Y].axis_type,
		           conf.amap[CONF_CC_AXIS_DPAD_Y].action, axis_value);
	}

	/* Classic.LStick.X */
	if (conf.amap[CONF_CC_AXIS_L_STICK_X].active) {
		axis_value = mesg->l_stick_x;
		if (conf.amap[CONF_CC_AXIS_L_STICK_X].flags & CONF_INVERT) {
			axis_value = CWIID_CLASSIC_L_STICK_MAX - axis_value;
		}
		send_event(&conf, conf.amap[CONF_CC_AXIS_L_STICK_X].axis_type,
		           conf.amap[CONF_CC_AXIS_L_STICK_X].action, axis_value);
	}

	/* Classic.LStick.Y */
	if (conf.amap[CONF_CC_AXIS_L_STICK_Y].active) {
		axis_value = mesg->l_stick_y;
		if (conf.amap[CONF_CC_AXIS_L_STICK_Y].flags & CONF_INVERT) {
			axis_value = CWIID_CLASSIC_L_STICK_MAX - axis_value;
		}
		send_event(&conf, conf.amap[CONF_CC_AXIS_L_STICK_Y].axis_type,
		           conf.amap[CONF_CC_AXIS_L_STICK_Y].action, axis_value);
	}

	/* Classic.RStick.X */
	if (conf.amap[CONF_CC_AXIS_R_STICK_X].active) {
		axis_value = mesg->r_stick_x;
		if (conf.amap[CONF_CC_AXIS_R_STICK_X].flags & CONF_INVERT) {
			axis_value = CWIID_CLASSIC_R_STICK_MAX - axis_value;
		}
		send_event(&conf, conf.amap[CONF_CC_AXIS_R_STICK_X].axis_type,
		           conf.amap[CONF_CC_AXIS_R_STICK_X].action, axis_value);
	}

	/* Classic.RStick.Y */
	if (conf.amap[CONF_CC_AXIS_R_STICK_Y].active) {
		axis_value = mesg->r_stick_y;
		if (conf.amap[CONF_CC_AXIS_R_STICK_Y].flags & CONF_INVERT) {
			axis_value = CWIID_CLASSIC_R_STICK_MAX - axis_value;
		}
		send_event(&conf, conf.amap[CONF_CC_AXIS_R_STICK_Y].axis_type,
		           conf.amap[CONF_CC_AXIS_R_STICK_Y].action, axis_value);
	}

	/* Classic.LAnalog */
	if (conf.amap[CONF_CC_AXIS_L].active) {
		axis_value = mesg->l;
		if (conf.amap[CONF_CC_AXIS_L].flags & CONF_INVERT) {
			axis_value = CWIID_CLASSIC_LR_MAX - axis_value;
		}
		send_event(&conf, conf.amap[CONF_CC_AXIS_L].axis_type,
		           conf.amap[CONF_CC_AXIS_L].action, axis_value);
	}

	/* Classic.RAnalog */
	if (conf.amap[CONF_CC_AXIS_R].active) {
		axis_value = mesg->r;
		if (conf.amap[CONF_CC_AXIS_R].flags & CONF_INVERT) {
			axis_value = CWIID_CLASSIC_LR_MAX - axis_value;
		}
		send_event(&conf, conf.amap[CONF_CC_AXIS_R].axis_type,
		           conf.amap[CONF_CC_AXIS_R].action, axis_value);
	}
}

void process_plugin(struct plugin *plugin, int mesg_count,
                    union cwiid_mesg *mesg[])
{
	union cwiid_mesg *plugin_mesg[CWIID_MAX_MESG_COUNT];
	int plugin_mesg_count = 0;
	int i;
	uint8_t flag;
	struct wmplugin_data *data;
	uint16_t pressed, released;
	__s32 axis_value;

	for (i=0; i < mesg_count; i++) {
		switch (mesg[i]->type) {
		case CWIID_MESG_STATUS:
			flag = CWIID_RPT_STATUS;
			break;
		case CWIID_MESG_BTN:
			flag = CWIID_RPT_BTN;
			break;
		case CWIID_MESG_ACC:
			flag = CWIID_RPT_ACC;
			break;
		case CWIID_MESG_IR:
			flag = CWIID_RPT_IR;
			break;
		case CWIID_MESG_NUNCHUK:
			flag = CWIID_RPT_NUNCHUK;
			break;
		case CWIID_MESG_CLASSIC:
			flag = CWIID_RPT_CLASSIC;
			break;
		default:
			break;
		}
		if (plugin->rpt_mode_flags & flag) {
			plugin_mesg[plugin_mesg_count++] = mesg[i];
		}
	}

	if (plugin_mesg_count > 0) {
		if (!(data = (*plugin->exec)(plugin_mesg_count, plugin_mesg))) {
			return;
		}

		/* Plugin Button/Key Events */
		pressed = data->buttons & ~plugin->prev_buttons;
		released = ~data->buttons & plugin->prev_buttons;
		for (i=0; i < plugin->info->button_count; i++) {
			if (plugin->bmap[i].active) {
				if (pressed & 1<<i) {
					send_event(&conf, EV_KEY, plugin->bmap[i].action, 1);
				}
				else if (released & 1<<i) {
					send_event(&conf, EV_KEY, plugin->bmap[i].action, 0);
				}
			}
		}
		plugin->prev_buttons = data->buttons;

		/* Plugin Axis Events */
		for (i=0; i < plugin->info->axis_count; i++) {
			if (plugin->amap[i].active && data->axes && data->axes[i].valid) {
				axis_value = data->axes[i].value;
				if (plugin->amap[i].flags & CONF_INVERT) {
					axis_value = plugin->info->axis_info[i].max +
					             plugin->info->axis_info[i].min - axis_value;
				}
				send_event(&conf, plugin->amap[i].axis_type,
				           plugin->amap[i].action, axis_value);
			}
		}
	}
}

