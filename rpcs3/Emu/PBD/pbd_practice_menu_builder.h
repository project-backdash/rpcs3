#pragma once

#include "../Cell/pbd_rollback_session_boundary.h"

struct pbd::rollback_session::practice_steps::native_menu : practice_steps
{
	bool decision, remembered_ready, situation_ready;
	u32 timer_frames;
	bool r3_remembers;
	pbd::game_overlay::snapshot panel;
	std::vector<pbd::game_overlay::row> rows;
	const std::string busy = "Stop the dummy's recording or playback first.";
	const std::string input_delay_help = "How many frames late your fighter acts on your buttons, as in an online match with input delay.";
	u32 page = data.practice.menu.page;
	bool reacts = false, list_page = false, asking = false;
	std::string reaction_name;

	void row(std::string label, std::string value, bool enabled, std::string help, bool is_default = false);
	void choice_row(u32 index_in_page, std::string label, bool enabled, std::string help);
	void restore_row(bool enabled, std::string help);
	void begin();
	void decision_rows();
	void training_rows();
	void offense_options();
	void offense_reaction();
	void offense_header();
	void defense_rows();
	void restart_rows();
	void display_rows();
	void selection_rows();
	void confirm_dialog();
	void focused_rows();
	void move_list();
	void build();
};
