/*
 * cthd_cdev_rapl.cpp: thermal cooling class implementation
 *	using RAPL
 * Copyright (C) 2012 Intel Corporation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License version
 * 2 or later as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 *
 *
 * Author Name <Srinivas.Pandruvada@linux.intel.com>
 *
 */
#include "thd_cdev_rapl.h"
#include "thd_engine.h"

/* This uses Intel RAPL driver to cool the system. RAPL driver show
 * mas thermal spec power in max_state. Each state can compensate
 * rapl_power_dec_percent, from the max state.
 *
 */
void cthd_sysfs_cdev_rapl::set_curr_state(int state, int control) {

	std::stringstream tc_state_dev;

	std::stringstream state_str;
	int new_state = state, ret;

	if (bios_locked) {
		if (state <= inc_dec_val)
			curr_state = min_state;
		else
			curr_state = max_state;

		return;
	}

	if (state < max_state)
		new_state = max_state;

	if (!control && state <= max_state)
		new_state = min_state;

	if (new_state >= min_state) {
		std::stringstream time_window_attr;

		if (power_on_constraint_0_pwr)
			new_state = power_on_constraint_0_pwr;
		else
			new_state = min_state;

		curr_state = min_state;
		cdev_sysfs.write("enabled", "0");

		time_window_attr << "constraint_" << constraint_index
				<< "_time_window_us";
		cdev_sysfs.write(time_window_attr.str(),
				power_on_constraint_0_time_window);
		constrained = false;
	} else if (control) {
		if (!constrained) {
			std::stringstream time_window_attr;

			time_window_attr << "constraint_" << constraint_index
					<< "_time_window_us";
			cdev_sysfs.write(time_window_attr.str(), def_rapl_time_window);
			cdev_sysfs.write("enabled", "1");
			constrained = true;
		}
	}
	thd_log_info("set cdev state index %d state %d wr:%d\n", index, state,
			new_state);

	tc_state_dev << "constraint_" << constraint_index << "_power_limit_uw";
	ret = cdev_sysfs.write(tc_state_dev.str(), new_state);
	if (ret < 0) {
		curr_state = (state == 0) ? 0 : max_state;
		if (ret == -ENODATA) {
			thd_log_info("powercap RAPL is BIOS locked, cannot update\n");
			bios_locked = true;
		}
	}
	curr_state = new_state;
}

void cthd_sysfs_cdev_rapl::set_curr_state_raw(int state, int arg) {
	set_curr_state(state, arg);
}

int cthd_sysfs_cdev_rapl::get_curr_state() {
	if (dynamic_phy_max_enable) {
		if (constrained)
			return thd_engine->rapl_power_meter.rapl_action_get_power(
					PACKAGE);
		else
			return min_state;
	}
	return curr_state;
}

int cthd_sysfs_cdev_rapl::get_curr_state(bool read_again) {
	thd_engine->rapl_power_meter.rapl_start_measure_power();
	return thd_engine->rapl_power_meter.rapl_action_get_power(PACKAGE);
}

int cthd_sysfs_cdev_rapl::get_max_state() {

	return max_state;
}

int cthd_sysfs_cdev_rapl::update() {
	int i;
	std::stringstream temp_str;
	int _index = -1;
	int constraint_phy_max;
	bool ppcc = false;
	std::string domain_name;

	for (i = 0; i < rapl_no_time_windows; ++i) {
		temp_str << "constraint_" << i << "_name";
		if (cdev_sysfs.exists(temp_str.str())) {
			std::string type_str;
			cdev_sysfs.read(temp_str.str(), type_str);
			if (type_str == "long_term") {
				_index = i;
				break;
			}
		}
	}
	if (_index < 0) {
		thd_log_info("powercap RAPL no long term time window\n");
		return THD_ERROR;
	}

	cdev_sysfs.read("name", domain_name);
	if (domain_name == "package-0")
		ppcc = read_ppcc_power_limits();

	if (ppcc) {
		int current_pl1;

		phy_max = pl0_max_pwr;
		set_inc_value(-pl0_step_pwr * 2);
		set_dec_value(-pl0_step_pwr);
		min_state = pl0_max_pwr;
		max_state = pl0_min_pwr;

		std::stringstream temp_power_str;
		temp_power_str.str(std::string());

		temp_power_str << "constraint_0" << "_power_limit_uw";
		if (!cdev_sysfs.exists(temp_power_str.str())) {
			thd_log_debug("powercap RAPL no  power limit uw %s \n",
					temp_str.str().c_str());
			return THD_ERROR;
		}

		if (cdev_sysfs.read(temp_power_str.str(), &current_pl1) > 0) {
			if (pl0_max_pwr > current_pl1) {
				thd_log_info(
						"pkg_power: powercap ppcc RAPL max power limit is more than the current PL1, current:%d max:%d \n",
						current_pl1, pl0_max_pwr);

				if (cdev_sysfs.write(temp_power_str.str(), pl0_max_pwr) <= 0)
					thd_log_info(
							"pkg_power: powercap ppcc RAPL max power limit failed to write %d \n",
							pl0_max_pwr);
			}
		}

		thd_engine->rapl_power_meter.rapl_start_measure_power();
		dynamic_phy_max_enable = true;
		//set_debounce_interval(1);
	} else {

		temp_str.str(std::string());
		temp_str << "constraint_" << _index << "_max_power_uw";
		if (!cdev_sysfs.exists(temp_str.str())) {
			thd_log_info("powercap RAPL no max power limit range %s \n",
					temp_str.str().c_str());
			return THD_ERROR;
		}
		if (cdev_sysfs.read(temp_str.str(), &phy_max) < 0 || phy_max < 0
				|| phy_max > rapl_max_sane_phy_max) {
			thd_log_info("%s:powercap RAPL invalid max power limit range \n",
					domain_name.c_str());
			thd_log_info("Calculate dynamically phy_max \n");

			std::stringstream temp_power_str;
			temp_power_str.str(std::string());
			temp_power_str << "constraint_" << _index << "_power_limit_uw";
			if (!cdev_sysfs.exists(temp_power_str.str())) {
				thd_log_info("powercap RAPL no  power limit uw %s \n",
						temp_str.str().c_str());
				return THD_ERROR;
			}

			if (cdev_sysfs.read(temp_power_str.str(),
					&power_on_constraint_0_pwr) <= 0) {
				thd_log_info("powercap RAPL invalid max power limit range \n");
			}

			thd_log_debug("power_on_constraint_0_pwr %d\n",
					power_on_constraint_0_pwr);

			phy_max = max_state = 0;
			curr_state = min_state = rapl_max_sane_phy_max;
			thd_engine->rapl_power_meter.rapl_start_measure_power();
			set_inc_dec_value(-rapl_min_default_step);
			dynamic_phy_max_enable = true;

			std::stringstream time_window;
			temp_str.str(std::string());
			temp_str << "constraint_" << _index << "_time_window_us";
			if (!cdev_sysfs.exists(temp_str.str())) {
				thd_log_info("powercap RAPL no time_window_us %s \n",
						temp_str.str().c_str());
				return THD_ERROR;
			}

			if (cdev_sysfs.read(temp_str.str(), &power_on_constraint_0_time_window)
					<= 0) {
				thd_log_info("powercap RAPL invalid time window \n");
				return THD_ERROR;
			}

			return THD_SUCCESS;
		}

		std::stringstream temp_power_str;
		temp_power_str.str(std::string());
		temp_power_str << "constraint_" << _index << "_power_limit_uw";
		if (!cdev_sysfs.exists(temp_power_str.str())) {
			thd_log_info("powercap RAPL no  power limit uw %s \n",
					temp_str.str().c_str());
			return THD_ERROR;
		}
		if (cdev_sysfs.read(temp_power_str.str(), &constraint_phy_max) <= 0) {
			thd_log_info("powercap RAPL invalid max power limit range \n");
			constraint_phy_max = 0;
		}
		if (constraint_phy_max > phy_max) {
			thd_log_info(
					"Default constraint power limit is more than max power %d:%d\n",
					constraint_phy_max, phy_max);
			phy_max = constraint_phy_max;
		}
		thd_log_info("powercap RAPL max power limit range %d \n", phy_max);

		set_inc_dec_value(-phy_max * (float) rapl_power_dec_percent / 100);
		min_state = phy_max;
		max_state = min_state
				- (float) min_state * rapl_low_limit_percent / 100;
	}
	std::stringstream time_window;
	temp_str.str(std::string());
	temp_str << "constraint_" << _index << "_time_window_us";
	if (!cdev_sysfs.exists(temp_str.str())) {
		thd_log_info("powercap RAPL no time_window_us %s \n",
				temp_str.str().c_str());
		return THD_ERROR;
	}

	if (cdev_sysfs.read(temp_str.str(), &power_on_constraint_0_time_window)
			<= 0) {
		thd_log_info("powercap RAPL invalid time window \n");
		return THD_ERROR;
	}

	std::stringstream enable;
	temp_str.str(std::string());
	temp_str << "enabled";
	if (!cdev_sysfs.exists(temp_str.str())) {
		thd_log_info("powercap RAPL not enabled %s \n", temp_str.str().c_str());
		return THD_ERROR;
	}
	cdev_sysfs.write(temp_str.str(), "0");

	thd_log_debug("RAPL max limit %d increment: %d\n", max_state, inc_dec_val);
	constraint_index = _index;
	set_pid_param(-1000, 100, 10);
	curr_state = min_state;

	return THD_SUCCESS;
}

bool cthd_sysfs_cdev_rapl::read_ppcc_power_limits() {
	csys_fs sys_fs;
	ppcc_t *ppcc;

	ppcc = thd_engine->parser.get_ppcc_param();
	if (ppcc) {
		thd_log_info("Reading PPCC from the thermal-conf.xml\n");
		pl0_max_pwr = ppcc->power_limit_max * 1000;
		pl0_min_pwr = ppcc->power_limit_min * 1000;
		pl0_min_window = ppcc->time_wind_min * 1000;
		pl0_step_pwr = ppcc->step_size * 1000;
		return true;
	}

	if (sys_fs.exists("/sys/bus/pci/devices/0000:00:04.0/power_limits/"))
		sys_fs.update_path("/sys/bus/pci/devices/0000:00:04.0/power_limits/");
	else if (sys_fs.exists("/sys/bus/pci/devices/0000:00:0b.0/power_limits/"))
		sys_fs.update_path("/sys/bus/pci/devices/0000:00:0b.0/power_limits/");
	else if (sys_fs.exists(
			"/sys/bus/platform/devices/INT3401:00/power_limits/"))
		sys_fs.update_path(
				"/sys/bus/platform/devices/INT3401:00/power_limits/");
	else
		return false;

	if (sys_fs.exists("power_limit_0_max_uw")) {
		if (sys_fs.read("power_limit_0_max_uw", &pl0_max_pwr) <= 0)
			return false;
	}

	if (sys_fs.exists("power_limit_0_min_uw")) {
		if (sys_fs.read("power_limit_0_min_uw", &pl0_min_pwr) <= 0)
			return false;
	}

	if (sys_fs.exists("power_limit_0_tmin_us")) {
		if (sys_fs.read("power_limit_0_tmin_us", &pl0_min_window) <= 0)
			return false;
	}

	if (sys_fs.exists("power_limit_0_step_uw")) {
		if (sys_fs.read("power_limit_0_step_uw", &pl0_step_pwr) <= 0)
			return false;
	}

	if (pl0_max_pwr && pl0_min_pwr && pl0_min_window && pl0_step_pwr) {
		thd_log_debug("ppcc limits max:%u min:%u  min_win:%u step:%u\n",
				pl0_max_pwr, pl0_min_pwr, pl0_min_window, pl0_step_pwr);
		return true;
	}

	return false;
}

void cthd_sysfs_cdev_rapl::thd_cdev_set_min_state_param(int arg) {
	min_state = curr_state = arg;
}
