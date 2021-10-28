/*
 * Buddy List Filter Plugin
 *
 * Copyright (C) 2021, Alex Orlov <the.vindicar@gmail.com>,
 * 
 * Based on helloworld.c thoughtfully provided by pidgin dev crew.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02111-1301, USA.
 *
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

/* config.h may define PURPLE_PLUGINS; protect the definition here so that we
 * don't get complaints about redefinition when it's not necessary. */
#ifndef PURPLE_PLUGINS
# define PURPLE_PLUGINS
#endif

#include <glib.h>
//not my code - not my problem -_-
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#include <gtk/gtk.h>
#pragma GCC diagnostic pop

/* This will prevent compiler errors in some instances and is better explained in the
 * how-to documents on the wiki */
#ifndef G_GNUC_NULL_TERMINATED
# if __GNUC__ >= 4
#  define G_GNUC_NULL_TERMINATED __attribute__((__sentinel__))
# else
#  define G_GNUC_NULL_TERMINATED
# endif
#endif

#include <debug.h>
#include <notify.h>
#include <plugin.h>
#include <pluginpref.h>
#include <prefs.h>
#include <version.h>
#include <util.h>
#include <gtkblist.h>
#include "gtkutils.h"
#include "gtkplugin.h"

#define PLUGIN_ID "gtk-vindicar-buddy-list-filter"
#define PLUGIN_VERSION "0.1"
#define PLUGIN_PREF_ROOT "/plugins/gtk/" PLUGIN_ID
#define PLUGIN_PREF_ACTIVE_FILTER PLUGIN_PREF_ROOT "/selected_filter"
#define PLUGIN_PREF_MAXPATH 256
//Sadly, "\n" is not an option for this. Blame GtkCellRenderer. 
#define PLUGIN_PATTERN_SEPARATOR "|"


/* we're adding this here and assigning it in plugin_load because we need
 * a valid plugin handle for our call to purple_notify_message() in the
 * plugin_action_test_cb() callback function */
PurplePlugin* buddylistfilter_plugin = NULL;

//====================== Base structs and funcs ======================
typedef struct
{
	GPatternSpec* pattern;
	gboolean inverted;
} BListFilter;

typedef struct 
{
	const char* name;
	const char* icon_path;
	GList* group_patterns;
} BListFilterDescription;

static GList* blistfilter_filters;

static void free_pattern(gpointer data) 
{ 
	g_pattern_spec_free(((BListFilter*)data)->pattern);
	g_free((BListFilter*)data);
}

/*Output the general stats of the filter into the debug log
static void debug_filter(BListFilterDescription* filter)
{
	if (filter->name) purple_debug_misc(PLUGIN_ID, "\tName: '%s'\n", filter->name); else purple_debug_misc(PLUGIN_ID, "\tName: NULL\n");
	if (filter->icon_path) purple_debug_misc(PLUGIN_ID, "\tIcon: '%s'\n", filter->icon_path); else purple_debug_misc(PLUGIN_ID, "\tIcon: NULL\n");
	purple_debug_misc(PLUGIN_ID, "\tGroup patterns: %d\n", g_list_length(filter->group_patterns));
}
//*/
/*Outputs the visible name of a node into the debug log
static void debug_node(PurpleBlistNode* node)
{
	const char* vis;
	vis = PURPLE_BLIST_NODE_IS_VISIBLE(node) ? "V" : "H";
	switch (purple_blist_node_get_type(node))
	{
		case PURPLE_BLIST_GROUP_NODE:	purple_debug_misc(PLUGIN_ID, "[%s] [GROUP] %s\n", vis, purple_group_get_name(PURPLE_GROUP(node))); break;
		case PURPLE_BLIST_CONTACT_NODE:	purple_debug_misc(PLUGIN_ID, "[%s] [CNTCT] %s\n", vis, purple_contact_get_alias(PURPLE_CONTACT(node))); break;
		case PURPLE_BLIST_CHAT_NODE:	purple_debug_misc(PLUGIN_ID, "[%s] [CHAT ] %s\n", vis, purple_chat_get_name(PURPLE_CHAT(node))); break;
		case PURPLE_BLIST_BUDDY_NODE:	purple_debug_misc(PLUGIN_ID, "[%s] [BUDDY] %s\n", vis, purple_buddy_get_alias(PURPLE_BUDDY(node))); break;
		default: 			purple_debug_misc(PLUGIN_ID, "[%s] [OTHER] %s\n", vis, "???"); break;
	}
}
//*/
//Checks if a string is matching any of the specified list of glob-like patterns
//Empty list will match any string. Empty string will match any list.
static gboolean blistfilter_is_matching_list(const char* value, GList* patterns)
{
	gboolean has_positive_matches, has_negative_matches, has_any_positives;
	guint length;
	char* reversed;
	BListFilter* item;
	//empty value matches any list. empty list matches any value.
	if (!value || *value == '\0' || !patterns) return TRUE;
	length = strlen(value);
	reversed = g_utf8_strreverse(value, length);
	has_positive_matches = FALSE; //did string match any positive filter?
	has_negative_matches = FALSE; //did string match any negative filter?
	has_any_positives = FALSE; //was there even any positive filter?
	for (GList* i = patterns; i != NULL; i = i->next)
	{
		item = (BListFilter*)i->data;
		if (item->inverted)
			has_negative_matches = has_negative_matches || g_pattern_match(item->pattern, length, value, reversed);
		else
		{
			has_positive_matches = has_positive_matches || g_pattern_match(item->pattern, length, value, reversed);
			has_any_positives = TRUE;
		}
	}
	g_free(reversed);
	//we show only groups that match ANY positive filter (if such filters exist), but also don't match ANY of the negative filters
	return (!has_any_positives || has_positive_matches) && !has_negative_matches;
}
//Checks if a group node matches the specified filter. 
// - Group has at least one visible child item, either due to filter or because it's a non-standard item.
static gboolean blistfilter_match_group(PurpleBlistNode* node, BListFilterDescription* filter)
{
	const char* group_name;
	
	if (!PURPLE_BLIST_NODE_IS_GROUP(node))
	{
		purple_debug_error(PLUGIN_ID, "Somehow, blistfilter_match_group() was called not on a group node!\n");
		return TRUE;
	}
	if (!filter) 
		return TRUE;
	group_name = purple_group_get_name(PURPLE_GROUP(node));
	return blistfilter_is_matching_list(group_name, filter->group_patterns);
}

//Sets node visiblity
static void blistfilter_set_node_visibility(PurpleBlistNode* node, gboolean visible, gboolean recursive, PurpleBlistUiOps* ops)
{
	PurpleBlistNodeFlags flags;
	flags = purple_blist_node_get_flags(node);
	if (visible)
		flags = flags & ~PURPLE_BLIST_NODE_FLAG_INVISIBLE;
	else
		flags = flags | PURPLE_BLIST_NODE_FLAG_INVISIBLE;
	purple_blist_node_set_flags(node, flags);
	//debug_node(node);
	if (ops && ops->update) ops->update(purple_get_blist(), node);
	if (recursive)
		for (PurpleBlistNode* item = purple_blist_node_get_first_child(node); item != NULL; item = purple_blist_node_get_sibling_next(item))
			blistfilter_set_node_visibility(item, visible, recursive, ops);
}

//Decides if specified node is a match for the given filter, and adjusts its visibilty accordingly.
static void blistfilter_update_node(PurpleBlistNode* node, gpointer data)
{
	PurpleBlistUiOps* ops;
	BListFilterDescription* filter;
	
	if (!PURPLE_BLIST_NODE_IS_GROUP(node)) return;
	filter = (BListFilterDescription*)data;
	ops = purple_blist_get_ui_ops();
	if (filter != NULL)
		blistfilter_set_node_visibility(node, blistfilter_match_group(node, filter), TRUE, ops);
	else
		blistfilter_set_node_visibility(node, TRUE, TRUE, ops);	
}

//Updates all nodes in the buddy list
static void blistfilter_update_entire_blist(BListFilterDescription* filter)
{
	for (PurpleBlistNode* group = purple_blist_get_root(); 
		group != NULL; 
		group = purple_blist_node_get_sibling_next(group))
	{
		blistfilter_update_node(group, filter);
	}
}

//====================== Pref structs and funcs ======================
//Prepares a pref template for a filter with given id. Will not overwrite existing values!
static void blistfilter_make_filter_pref(int filter_id)
{
	char pref_name_buffer[PLUGIN_PREF_MAXPATH];
	snprintf(pref_name_buffer, PLUGIN_PREF_MAXPATH, PLUGIN_PREF_ROOT "/filters/filter%d", filter_id);
	purple_prefs_add_none(pref_name_buffer);
	snprintf(pref_name_buffer, PLUGIN_PREF_MAXPATH, PLUGIN_PREF_ROOT "/filters/filter%d/name", filter_id);
	purple_prefs_add_string(pref_name_buffer, "Everything");
	snprintf(pref_name_buffer, PLUGIN_PREF_MAXPATH, PLUGIN_PREF_ROOT "/filters/filter%d/icon_path", filter_id);
	purple_prefs_add_path(pref_name_buffer, NULL);
	snprintf(pref_name_buffer, PLUGIN_PREF_MAXPATH, PLUGIN_PREF_ROOT "/filters/filter%d/group_patterns", filter_id);
	purple_prefs_add_string_list(pref_name_buffer, NULL);
	purple_debug_misc(PLUGIN_ID, "Created prefs for filter #%d\n", filter_id);
}

//Clears a BListFilterDescription structure so it can be refilled or disposed of
static void blistfilter_free(gpointer data)
{
	BListFilterDescription* filter;
	
	filter = (BListFilterDescription*)data;
	filter->name = NULL;
	filter->icon_path = NULL;
	if (filter->group_patterns) 
		g_list_free_full(g_steal_pointer(&(filter->group_patterns)), free_pattern);
	g_free(filter);
}

//Loads a list of strings from the prefs and converts it into a list of compiled glob patterns
//Glob patterns are currently case-sensitive.
static GList* blistfilter_prefs_load_patterns(const char* path)
{
	GList* strings = NULL;
	GList* patterns = NULL;
	BListFilter* filter;
	strings = purple_prefs_get_string_list(path);
	for (GList* item = strings; item != NULL; item = item->next)
	{
		const gchar* line = (const gchar*)item->data;
		if (strlen(line) > 0)
		{
			filter = g_new0(BListFilter, 1);
			if (line[0] == '~')
			{
				filter->pattern = g_pattern_spec_new(&line[1]);
				filter->inverted = TRUE;
			}
			else
			{
				filter->pattern = g_pattern_spec_new(line);
				filter->inverted = FALSE;
			}
			patterns = g_list_append(patterns, filter);
		}
	}
	g_list_free_full(g_steal_pointer(&strings), g_free);
	return patterns;
}

//Loads a filter description from prefs and transforms it into a BListFilterDescription structure 
//that can be used for actual filtering.
static BListFilterDescription* blistfilter_load_filter_pref(int filter_id)
{
	BListFilterDescription* filter;
	char pref_name_buffer[PLUGIN_PREF_MAXPATH];
	
	snprintf(pref_name_buffer, PLUGIN_PREF_MAXPATH, PLUGIN_PREF_ROOT "/filters/filter%d/name", filter_id);
	if (!purple_prefs_exists(pref_name_buffer))
		return NULL;
	filter = g_new0(BListFilterDescription, 1);
	filter->name = purple_prefs_get_string(pref_name_buffer);
	if (!filter->name || *(filter->name) == '\0')
		filter->name = "<unnamed filter>";
	snprintf(pref_name_buffer, PLUGIN_PREF_MAXPATH, PLUGIN_PREF_ROOT "/filters/filter%d/icon_path", filter_id);
	filter->icon_path = purple_prefs_get_path(pref_name_buffer);
	snprintf(pref_name_buffer, PLUGIN_PREF_MAXPATH, PLUGIN_PREF_ROOT "/filters/filter%d/group_patterns", filter_id);
	filter->group_patterns = blistfilter_prefs_load_patterns(pref_name_buffer);
	//purple_debug_misc(PLUGIN_ID, "Loaded a filter:\n");
	//debug_filter(filter);
	return filter;
}

static void blistfilter_free_all_filters()
{
	if (blistfilter_filters)
		g_list_free_full(g_steal_pointer(&blistfilter_filters), blistfilter_free);
}

//Loads all filters from prefs.
static void blistfilter_load_all_filters()
{
	int filter_id;
	BListFilterDescription* filter;
	
	blistfilter_free_all_filters();
	filter_id = 0;
	while ((filter = blistfilter_load_filter_pref(filter_id)) != NULL)
	{
		blistfilter_filters = g_list_append(blistfilter_filters, filter);
		filter_id++;
	}
}
//====================== GUI structs and funcs ======================
//Selector types: vertical stack fo buttons, horizontal pane of buttons, drowdown list
typedef enum 
{ 
	FST_VERTICAL_TOP,
	FST_VERTICAL_BOTTOM, 
	FST_HORIZONTAL_TOP,
	FST_HORIZONTAL_BOTTOM, 
	FST_INVALID
} FilterSelectorStyle;

typedef struct
{
	GtkWidget* box;
	GList* buttons;
} FilterSelectorGui;

FilterSelectorGui* blistfilter_gui;

static void blistfilter_button_cb(GtkButton* btn, gpointer user_data)
{
	int new_filter_id;
	char pref_name_buffer[PLUGIN_PREF_MAXPATH];

	new_filter_id = GPOINTER_TO_INT(user_data);
	snprintf(pref_name_buffer, PLUGIN_PREF_MAXPATH, PLUGIN_PREF_ROOT "/filters/filter%d", new_filter_id);
	if (purple_prefs_exists(pref_name_buffer))
	{
		//purple_debug_misc(PLUGIN_ID, "Button pressed, selecting filter #%d\n", new_filter_id);
		purple_prefs_set_int(PLUGIN_PREF_ACTIVE_FILTER, new_filter_id);
	}
	else
	{
		purple_notify_error(buddylistfilter_plugin, "Filter not found", "Somehow, the filter you've selected, does not exist!", NULL);
		purple_prefs_set_int(PLUGIN_PREF_ACTIVE_FILTER, 0);
	}
}

static void blistfilter_destroy_filter_selector_gui()
{
	g_list_free_full(g_steal_pointer(&blistfilter_gui->buttons), g_object_unref);
	if (blistfilter_gui->box)
	{
		g_object_unref(blistfilter_gui->box);
		gtk_widget_destroy(g_steal_pointer(&blistfilter_gui->box));
	}
}

static void blistfilter_make_vertical_selector_gui()
{
	int spacing;
	int selected_index;
	int filter_id;
	BListFilterDescription* filter;
	GtkRadioButton* btn;
	GSList* group;
	
	selected_index = purple_prefs_get_int(PLUGIN_PREF_ACTIVE_FILTER);
	spacing = purple_prefs_get_int(PLUGIN_PREF_ROOT "/buttons_spacing");
	blistfilter_gui->box = g_object_ref(gtk_vbox_new(TRUE, spacing));
	gtk_box_set_homogeneous(GTK_BOX(blistfilter_gui->box), purple_prefs_get_bool(PLUGIN_PREF_ROOT "/homogenous_buttons"));
	gtk_widget_set_name(GTK_WIDGET(blistfilter_gui->box), "blistfilter_gui_box");
	
	filter_id = 0;
	group = NULL;
	for (GList* item = blistfilter_filters; item != NULL; item = item->next)
	{
		filter = (BListFilterDescription*) item->data;
		btn = GTK_RADIO_BUTTON(gtk_radio_button_new(group));
		group = gtk_radio_button_get_group(btn);
		if (filter->name && filter->name[0])
		{
			gtk_button_set_label(GTK_BUTTON(btn), filter->name);
			gtk_widget_set_tooltip_text(GTK_WIDGET(btn), filter->name);
		}
		else
		{
			char buf[32];
			snprintf(buf, 32, "%d", filter_id+1);
			gtk_button_set_label(GTK_BUTTON(btn), buf);
			gtk_widget_set_tooltip_text(GTK_WIDGET(btn), buf);
		}
		gtk_button_set_focus_on_click(GTK_BUTTON(btn), FALSE);
		if (filter->icon_path && filter->icon_path[0]) //neither NULL nor empty string
		{
			gtk_button_set_image(GTK_BUTTON(btn), gtk_image_new_from_file(filter->icon_path));
			gtk_button_set_image_position(GTK_BUTTON(btn), GTK_POS_LEFT);
		}
		gtk_toggle_button_set_mode(GTK_TOGGLE_BUTTON(btn), FALSE);
		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(btn), selected_index == filter_id);
		gtk_box_pack_start(GTK_BOX(blistfilter_gui->box), GTK_WIDGET(btn), TRUE, TRUE, 0);
		blistfilter_gui->buttons = g_list_append(blistfilter_gui->buttons, g_object_ref(btn));
		filter_id++;
	}
	filter_id = 0;
	for (GList* item = blistfilter_gui->buttons; item != NULL; item = item->next)
	{
		g_signal_connect(G_OBJECT(item->data), "clicked", G_CALLBACK(blistfilter_button_cb), GINT_TO_POINTER(filter_id));
		filter_id++;
	}
}

static void blistfilter_make_horizontal_selector_gui()
{
	int spacing;
	int selected_index;
	int filter_id;
	BListFilterDescription* filter;
	GtkRadioButton* btn;
	GSList* group;
	gboolean always_show_titles, has_icon, has_text;
	
	always_show_titles = purple_prefs_get_bool(PLUGIN_PREF_ROOT "/force_titles");
	selected_index = purple_prefs_get_int(PLUGIN_PREF_ACTIVE_FILTER);
	spacing = purple_prefs_get_int(PLUGIN_PREF_ROOT "/buttons_spacing");
	blistfilter_gui->box = g_object_ref(gtk_hbox_new(TRUE, spacing));
	gtk_box_set_homogeneous(GTK_BOX(blistfilter_gui->box), purple_prefs_get_bool(PLUGIN_PREF_ROOT "/homogenous_buttons"));
	gtk_widget_set_name(GTK_WIDGET(blistfilter_gui->box), "blistfilter_gui_box");
	
	filter_id = 0;
	group = NULL;
	for (GList* item = blistfilter_filters; item != NULL; item = item->next)
	{
		filter = (BListFilterDescription*) item->data;
		has_text = (filter->name && filter->name[0]);
		has_icon = (filter->icon_path && filter->icon_path[0]);  
		btn = GTK_RADIO_BUTTON(gtk_radio_button_new(group));
		group = gtk_radio_button_get_group(btn);
		if (has_text)
			gtk_widget_set_tooltip_text(GTK_WIDGET(btn), filter->name);
		gtk_button_set_focus_on_click(GTK_BUTTON(btn), FALSE);
		if (has_icon)  //neither NULL nor empty string
		{
			gtk_button_set_image(GTK_BUTTON(btn), gtk_image_new_from_file(filter->icon_path));
			gtk_button_set_image_position(GTK_BUTTON(btn), GTK_POS_TOP);
		}
		if (!has_icon || always_show_titles)
		{
			if (has_text)
				gtk_button_set_label(GTK_BUTTON(btn), filter->name);
			else
			{
				char buf[32];
				snprintf(buf, 32, "%d", filter_id+1);
				gtk_button_set_label(GTK_BUTTON(btn), buf);
			}
		}
		gtk_toggle_button_set_mode(GTK_TOGGLE_BUTTON(btn), FALSE);
		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(btn), selected_index == filter_id);
		gtk_box_pack_start(GTK_BOX(blistfilter_gui->box), GTK_WIDGET(btn), TRUE, TRUE, 0);
		blistfilter_gui->buttons = g_list_append(blistfilter_gui->buttons, g_object_ref(btn));
		filter_id++;
	}
	filter_id = 0;
	for (GList* item = blistfilter_gui->buttons; item != NULL; item = item->next)
	{
		g_signal_connect(G_OBJECT(item->data), "clicked", G_CALLBACK(blistfilter_button_cb), GINT_TO_POINTER(filter_id));
		filter_id++;
	}
}


static void blistfilter_make_filter_selector_gui()
{
	FilterSelectorStyle selector_style;
	PidginBuddyList* gtkblist;
	
	gtkblist = pidgin_blist_get_default_gtk_blist();
	//if there is no buddy list window, or if our container already exists, do nothing;
	if (!gtkblist || !gtkblist->window)
	{
		purple_debug_warning(PLUGIN_ID, "blistfilter_make_filter_selector_gui() was called, but buddy list is not there. This shouldn't happen.\n");
		return;
	}
	blistfilter_destroy_filter_selector_gui();
	selector_style = purple_prefs_get_int(PLUGIN_PREF_ROOT "/selector_style");
	if (selector_style < 0 || selector_style >= FST_INVALID)
	{
		purple_debug_warning(PLUGIN_ID, "Invalid selector style code (%d), changing to default.\n", selector_style);
		selector_style = FST_VERTICAL_TOP;
		purple_prefs_set_int(PLUGIN_PREF_ROOT "/selector_style", selector_style);
	}
	switch (selector_style)
	{
		case FST_VERTICAL_TOP:
		{ //create vertical list of buttons with icons and text
			blistfilter_make_vertical_selector_gui();
			gtk_box_pack_start(GTK_BOX(gtkblist->vbox), blistfilter_gui->box, FALSE, FALSE, 0);
			gtk_box_reorder_child(GTK_BOX(gtkblist->vbox), GTK_WIDGET(blistfilter_gui->box), 0);
		}; break;
		case FST_VERTICAL_BOTTOM:
		{ //create vertical list of buttons with icons and text
			blistfilter_make_vertical_selector_gui();
			gtk_box_pack_start(GTK_BOX(gtkblist->vbox), blistfilter_gui->box, FALSE, FALSE, 0);
		}; break;
		case FST_HORIZONTAL_TOP: 
		{ //create horizontal list of buttons with icons only
			blistfilter_make_horizontal_selector_gui();
			gtk_box_pack_start(GTK_BOX(gtkblist->vbox), blistfilter_gui->box, FALSE, FALSE, 0);
			gtk_box_reorder_child(GTK_BOX(gtkblist->vbox), GTK_WIDGET(blistfilter_gui->box), 0);
		}; break;
		case FST_HORIZONTAL_BOTTOM: 
		{ //create horizontal list of buttons with icons only
			blistfilter_make_horizontal_selector_gui();
			gtk_box_pack_start(GTK_BOX(gtkblist->vbox), blistfilter_gui->box, FALSE, FALSE, 0);
		}; break;
		default: {
			purple_debug_error(PLUGIN_ID, "Wait, how did THAT happen? Selector style code is %d despite having been normalized!\n", selector_style);
			blistfilter_gui->box = NULL;
			return;
		}; break;
	}
	gtk_widget_show_all(blistfilter_gui->box);
}

//====================== Events and callbacks ======================
//Triggers whenever a selected filter pref changes, loading a corresponding filter description.
static void blistfilter_active_filter_changed_cb(const char* name, PurplePrefType type, gconstpointer val, gpointer data)
{
	int selected_index;
	BListFilterDescription* filter;
	
	if (strcmp(name, PLUGIN_PREF_ACTIVE_FILTER)) return;
	if (type != PURPLE_PREF_INT)
	{
		purple_debug_error(PLUGIN_ID, PLUGIN_PREF_ACTIVE_FILTER " changed, but it is not an integer, ignoring.\n");
		return;
	}
	selected_index = GPOINTER_TO_INT(val);
	filter = (BListFilterDescription*)g_list_nth_data(blistfilter_filters, selected_index);
	purple_debug_info(PLUGIN_ID, "Filter #%d: %s is now selected.\n", selected_index, filter->name);
	blistfilter_update_entire_blist(filter);
}

//Triggers whenever a GUI setting changes. Updates GUI immediately.
static void blistfilter_gui_setting_changed_cb(const char* name, PurplePrefType type, gconstpointer val, gpointer data)
{
	blistfilter_make_filter_selector_gui();
}

//====================== Editor stuff ======================
static struct {
	GtkWidget* dialog;
	GtkTreeView* view;
	GtkListStore* model;
} blistfilter_editor_gui;

static GtkListStore* blistfilter_load_model_from_prefs()
{
	GtkListStore* model;
	GtkTreeIter iter;
	int filter_id;
	char pref_name[PLUGIN_PREF_MAXPATH];
	const char* filter_name;
	const char* filter_icon;
	char* filter_patterns;
	GList* filter_pattern_bits;
	int bits_length, bit_index;
	gchar** bits; 

	model = gtk_list_store_new(3, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
	filter_id = 0;
	do
	{
		snprintf(pref_name, PLUGIN_PREF_MAXPATH, PLUGIN_PREF_ROOT "/filters/filter%d/name", filter_id);
		if (!purple_prefs_exists(pref_name)) break;
		filter_name = purple_prefs_get_string(pref_name);
		snprintf(pref_name, PLUGIN_PREF_MAXPATH, PLUGIN_PREF_ROOT "/filters/filter%d/icon_path", filter_id);
		filter_icon = purple_prefs_get_path(pref_name);
		
		snprintf(pref_name, PLUGIN_PREF_MAXPATH, PLUGIN_PREF_ROOT "/filters/filter%d/group_patterns", filter_id);
		filter_pattern_bits = purple_prefs_get_string_list(pref_name);
		bits_length = g_list_length(filter_pattern_bits);
		bits = g_new0(gchar*, bits_length+1); //the array must be NULL-terminated 
		bit_index = 0;
		for (GList* i = filter_pattern_bits; i != NULL; i = i->next)
			bits[bit_index++] = (gchar*)i->data;
		filter_patterns = g_strjoinv(PLUGIN_PATTERN_SEPARATOR, bits);
		g_free(bits);
		g_list_free_full(g_steal_pointer(&filter_pattern_bits), g_free);
		gtk_list_store_insert_with_values(
			model, 
			&iter,
			-1, //end of the list
			0, filter_name,
			1, filter_icon,
			2, filter_patterns,
			-1);
		g_free(filter_patterns);
		filter_id++;
	} while (TRUE);
	return model;
}

static void blistfilter_save_model_to_prefs(GtkListStore* model)
{
	GtkTreeIter iter;
	char* name_value;
	char* icon_value;
	char* pattern_value;
	int filter_id, filter_count;
	int selected_id;
	char pref_name[PLUGIN_PREF_MAXPATH];
	GList* filter_pattern_bits;
	gchar** bits; 
	
	if (model == NULL)
	{
		purple_debug_error(PLUGIN_ID, "Somehow, list store for filters is NULL. How did *that* even happen?\n");
		return;
	}
 
	if (!gtk_tree_model_get_iter_first(GTK_TREE_MODEL(model), &iter))
	{
		purple_debug_error(PLUGIN_ID, "Somehow, list store for filters is empty. This should not have happened, as we prevent deleting the last filter.\n");
		return;
	}
	selected_id = purple_prefs_get_int(PLUGIN_PREF_ACTIVE_FILTER);
	filter_id = 0;
	do
	{
		gtk_tree_model_get(GTK_TREE_MODEL(model), &iter, 0, &name_value, 1, &icon_value, 2, &pattern_value, -1);
		snprintf(pref_name, PLUGIN_PREF_MAXPATH, PLUGIN_PREF_ROOT "/filters/filter%d", filter_id);
		if (!purple_prefs_exists(pref_name)) //create filter branch if necessary - just overwrite if not
			purple_prefs_add_none(pref_name);
		snprintf(pref_name, PLUGIN_PREF_MAXPATH, PLUGIN_PREF_ROOT "/filters/filter%d/name", filter_id);
		purple_prefs_set_string(pref_name, name_value);
		snprintf(pref_name, PLUGIN_PREF_MAXPATH, PLUGIN_PREF_ROOT "/filters/filter%d/icon_path", filter_id);
		purple_prefs_set_string(pref_name, icon_value);
		
		bits = g_strsplit(pattern_value, PLUGIN_PATTERN_SEPARATOR, 0);
		filter_pattern_bits = NULL;
		for (int i = 0; bits[i] != NULL; i++)
		{
			filter_pattern_bits = g_list_append(filter_pattern_bits, bits[i]);
		}
		snprintf(pref_name, PLUGIN_PREF_MAXPATH, PLUGIN_PREF_ROOT "/filters/filter%d/group_patterns", filter_id);
		purple_prefs_set_string_list(pref_name, filter_pattern_bits);
		g_list_free(filter_pattern_bits);
		g_strfreev(bits);
		
		g_free(name_value);
		g_free(icon_value);
		g_free(pattern_value);
		filter_id++;
	}
	while (gtk_tree_model_iter_next(GTK_TREE_MODEL(model), &iter));
	filter_count = filter_id;
	//removing any extraneous filters left
	do
	{
		snprintf(pref_name, PLUGIN_PREF_MAXPATH, PLUGIN_PREF_ROOT "/filters/filter%d", filter_id);
		if (purple_prefs_exists(pref_name))
			purple_prefs_remove(pref_name);
		else
			break;
		filter_id++;
	}
	while (TRUE);
	if (selected_id >= filter_count)
		purple_prefs_set_int(PLUGIN_PREF_ACTIVE_FILTER, filter_count-1);
}

static void blistfilter_destroy_editor()
{
	g_clear_object(&blistfilter_editor_gui.dialog);
	g_clear_object(&blistfilter_editor_gui.view);
	g_clear_object(&blistfilter_editor_gui.model);
}

static void blistfilter_save_filters_dlg(GtkWidget *w, GtkWidget *window)
{
	int selected_index;
	BListFilterDescription* filter;
	
	if (blistfilter_editor_gui.model)
	{
		blistfilter_destroy_filter_selector_gui();
		blistfilter_save_model_to_prefs(blistfilter_editor_gui.model);
		blistfilter_load_all_filters();
		selected_index = purple_prefs_get_int(PLUGIN_PREF_ACTIVE_FILTER);
		if (selected_index < 0 || selected_index > (int)g_list_length(blistfilter_filters))
		{
			selected_index = 0;
			purple_prefs_set_int(PLUGIN_PREF_ACTIVE_FILTER, selected_index);
		}
		filter = (BListFilterDescription*)g_list_nth_data(blistfilter_filters, selected_index);
		blistfilter_make_filter_selector_gui();
		blistfilter_update_entire_blist(filter);
	}
	blistfilter_destroy_editor();
	gtk_widget_destroy(window);
}

static void blistfilter_close_filters_dlg(GtkWidget *w, GtkWidget *window)
{
	blistfilter_destroy_editor();
	gtk_widget_destroy(window);
}

static void blistfilter_add_new_filter_cb(GtkWidget *w, GtkWidget *window)
{
	GtkTreeIter iter;
	GtkTreeSelection* selection;
	
	if (!blistfilter_editor_gui.model) return;
	selection = gtk_tree_view_get_selection(blistfilter_editor_gui.view);
	gtk_list_store_insert_with_values(
		blistfilter_editor_gui.model, 
		&iter,
		-1, //end of the list
		0, "New filter",
		1, "",
		2, "",
		-1);
	gtk_tree_selection_select_iter(selection, &iter);
}

static void blistfilter_delete_filter_cb(GtkWidget *w, GtkWidget *window)
{
	int length;
	GtkTreeSelection* selection;
	GtkTreeIter iter, new_selection_iter;
	gboolean has_new_selection;

	if (!blistfilter_editor_gui.model) return;
	length = gtk_tree_model_iter_n_children(GTK_TREE_MODEL(blistfilter_editor_gui.model), NULL);
	if (length < 2)
	{
		purple_notify_error(buddylistfilter_plugin, "Last filter", "Last filter", "Can't delete the last filter.");
		return;
	}
	selection = gtk_tree_view_get_selection(blistfilter_editor_gui.view);
	if (!gtk_tree_selection_get_selected(selection, (GtkTreeModel**)(&blistfilter_editor_gui.model), &iter))
	{
		purple_notify_error(buddylistfilter_plugin, "No selection", "No selection", "Nothing is selected.");
		return;
	}
	new_selection_iter = iter;
	if (gtk_tree_model_iter_next(GTK_TREE_MODEL(blistfilter_editor_gui.model), &new_selection_iter))
		has_new_selection = TRUE;
	else
		has_new_selection = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(blistfilter_editor_gui.model), &new_selection_iter);
	gtk_list_store_remove(blistfilter_editor_gui.model, &iter);
	if (has_new_selection)
		gtk_tree_selection_select_iter(selection, &new_selection_iter);
}

static void blistfilter_move_filter_up_cb(GtkWidget *w, GtkWidget *window)
{
	GtkTreeSelection* selection;
	GtkTreeIter iter_selected, iter_before;
	GtkTreePath* path;

	if (!blistfilter_editor_gui.model) return;
	selection = gtk_tree_view_get_selection(blistfilter_editor_gui.view);
	if (!gtk_tree_selection_get_selected(selection, (GtkTreeModel**)(&blistfilter_editor_gui.model), &iter_selected))
	{
		purple_notify_error(buddylistfilter_plugin, "No selection", "No selection", "Nothing is selected.");
		return;
	}
	//GTK 2.0 does not have gtk_tree_model_iter_previous() what the HELL 
	path = gtk_tree_model_get_path(GTK_TREE_MODEL(blistfilter_editor_gui.model), &iter_selected);
	if (gtk_tree_path_prev(path) && gtk_tree_model_get_iter(GTK_TREE_MODEL(blistfilter_editor_gui.model), &iter_before, path))
	{
		gtk_list_store_swap(blistfilter_editor_gui.model, &iter_before, &iter_selected);
		gtk_tree_selection_select_iter(selection, &iter_selected);
	}
	gtk_tree_path_free(path);
}

static void blistfilter_move_filter_down_cb(GtkWidget *w, GtkWidget *window)
{
	GtkTreeSelection* selection;
	GtkTreeIter iter_selected, iter_after;

	if (!blistfilter_editor_gui.model) return;
	selection = gtk_tree_view_get_selection(blistfilter_editor_gui.view);
	if (!gtk_tree_selection_get_selected(selection, (GtkTreeModel**)(&blistfilter_editor_gui.model), &iter_selected))
	{
		purple_notify_error(buddylistfilter_plugin, "No selection", "No selection", "Nothing is selected.");
		return;
	}
	iter_after = iter_selected;
	if (gtk_tree_model_iter_next(GTK_TREE_MODEL(blistfilter_editor_gui.model), &iter_after))
	{
		gtk_list_store_swap(blistfilter_editor_gui.model, &iter_after, &iter_selected);
		gtk_tree_selection_select_iter(selection, &iter_selected);
	}
}

//reacts to cell being edited.
static void blistfilter_filter_renderer_edited_cb(GtkCellRendererText* cell, gchar* path, char* new_text, gpointer column_idx_as_ptr)
{
	GtkTreeIter iter;
	
	gtk_tree_model_get_iter_from_string(GTK_TREE_MODEL(blistfilter_editor_gui.model), &iter, path);
	gtk_list_store_set(blistfilter_editor_gui.model, &iter, GPOINTER_TO_INT(column_idx_as_ptr), new_text, -1);
}

//shows filter editor dialog window
static void blistfilter_filter_editor_cb(PurplePluginAction *unused)
{
	GtkWidget* toptext;
	GtkWidget* dlgbox;
	GtkWidget* commandbox;
	GtkWidget* btn;
	GtkCellRenderer* renderer;
	GtkTreeViewColumn *column;
	GtkTreeSelection* selection;
	
	blistfilter_destroy_editor();
	blistfilter_editor_gui.model = g_object_ref(blistfilter_load_model_from_prefs());
	blistfilter_editor_gui.dialog = GTK_WIDGET(g_object_ref(pidgin_create_dialog("Pidgin Buddylist Filter Editor", 0, "blistfilter-editor", FALSE)));
	dlgbox = pidgin_dialog_get_vbox_with_properties(GTK_DIALOG(blistfilter_editor_gui.dialog), FALSE, PIDGIN_HIG_BOX_SPACE);
	toptext = gtk_label_new(
		"Pattern cheatsheet:\n"
		"    Work - matches a group named 'Work'\n" 
		"    ~Work - matches any group except one named 'Work'\n" 
		"    Work: * - matches groups like 'Work: HR' or 'Work: Accounting'\n" 
		"    Friends|Family - matches a group named 'Friends' and a group named 'Family'\n"
		"    Work: *|~Work: IT - matches any group named like 'Work: HR' or 'Work: Accounting', unless it's 'Work: IT'\n"
		"    ~Work: HR|~Work: IT - matches any group except 'Work: HR' and 'Work: IT'\n"
		"    Empty pattern (no spaces!) matches everything." 
	);
	gtk_box_pack_start(GTK_BOX(dlgbox), toptext, FALSE, FALSE, 0);

	blistfilter_editor_gui.view = GTK_TREE_VIEW(g_object_ref(gtk_tree_view_new_with_model(GTK_TREE_MODEL(blistfilter_editor_gui.model))));
	selection = gtk_tree_view_get_selection(blistfilter_editor_gui.view);
	gtk_tree_selection_set_mode(selection, GTK_SELECTION_BROWSE);
	//name column	
	renderer = GTK_CELL_RENDERER(gtk_cell_renderer_text_new());
	g_object_set(renderer, "editable", TRUE, NULL);
	g_object_set(renderer, "single-paragraph-mode", TRUE, NULL);
	g_signal_connect(renderer, "edited", (GCallback)blistfilter_filter_renderer_edited_cb, GINT_TO_POINTER(0)); 
	column = gtk_tree_view_column_new_with_attributes("Filter name", renderer, "text", 0, NULL);
	gtk_tree_view_column_set_resizable(GTK_TREE_VIEW_COLUMN(column), TRUE);
	gtk_tree_view_append_column(blistfilter_editor_gui.view, column);
	//icon column	
	renderer = GTK_CELL_RENDERER(gtk_cell_renderer_text_new());
	g_object_set(renderer, "editable", TRUE, NULL);
	g_object_set(renderer, "single-paragraph-mode", TRUE, NULL);
	g_signal_connect(renderer, "edited", (GCallback)blistfilter_filter_renderer_edited_cb, GINT_TO_POINTER(1)); 
	column = gtk_tree_view_column_new_with_attributes("Path to icon", renderer, "text", 1, NULL);
	gtk_tree_view_column_set_resizable(GTK_TREE_VIEW_COLUMN(column), TRUE);
	gtk_tree_view_column_set_fixed_width(GTK_TREE_VIEW_COLUMN(column), 400);
	gtk_tree_view_column_set_expand(GTK_TREE_VIEW_COLUMN(column), FALSE);
	gtk_tree_view_append_column(blistfilter_editor_gui.view, column);
	//patterns column	
	renderer = GTK_CELL_RENDERER(gtk_cell_renderer_text_new());
	g_object_set(renderer, "editable", TRUE, NULL);
	g_object_set(renderer, "single-paragraph-mode", TRUE, NULL);
	g_signal_connect(renderer, "edited", (GCallback)blistfilter_filter_renderer_edited_cb, GINT_TO_POINTER(2)); 
	column = gtk_tree_view_column_new_with_attributes("Group name patterns", renderer, "text", 2, NULL);
	gtk_tree_view_column_set_resizable(GTK_TREE_VIEW_COLUMN(column), TRUE);
	gtk_tree_view_append_column(blistfilter_editor_gui.view, column);

	gtk_box_pack_start(GTK_BOX(dlgbox), GTK_WIDGET(blistfilter_editor_gui.view), TRUE, TRUE, 0);
	//buttons	
	commandbox = gtk_hbox_new(TRUE, 2);
	gtk_box_set_homogeneous(GTK_BOX(commandbox), TRUE);
	//"Add"
	btn = gtk_button_new_with_label("Add");
	g_signal_connect(btn, "clicked", (GCallback)blistfilter_add_new_filter_cb, NULL);
	gtk_box_pack_start(GTK_BOX(commandbox), btn, FALSE, FALSE, 0);
	//"Remove"
	btn = gtk_button_new_with_label("Remove");
	g_signal_connect(btn, "clicked", (GCallback)blistfilter_delete_filter_cb, NULL);
	gtk_box_pack_start(GTK_BOX(commandbox), btn, FALSE, FALSE, 0);
	//"Move up"
	btn = gtk_button_new_with_label("Move up");
	g_signal_connect(btn, "clicked", (GCallback)blistfilter_move_filter_up_cb, NULL);
	gtk_box_pack_start(GTK_BOX(commandbox), btn, FALSE, FALSE, 0);
	//"Move down"
	btn = gtk_button_new_with_label("Move down");
	g_signal_connect(btn, "clicked", (GCallback)blistfilter_move_filter_down_cb, NULL);
	gtk_box_pack_start(GTK_BOX(commandbox), btn, FALSE, FALSE, 0);
	
	gtk_box_pack_start(GTK_BOX(dlgbox), GTK_WIDGET(commandbox), FALSE, FALSE, 0);
	
	gtk_dialog_set_has_separator(GTK_DIALOG(blistfilter_editor_gui.dialog), TRUE);
	pidgin_dialog_add_button(GTK_DIALOG(blistfilter_editor_gui.dialog), GTK_STOCK_SAVE, G_CALLBACK(blistfilter_save_filters_dlg), GTK_DIALOG(blistfilter_editor_gui.dialog));
	pidgin_dialog_add_button(GTK_DIALOG(blistfilter_editor_gui.dialog), GTK_STOCK_CLOSE, G_CALLBACK(blistfilter_close_filters_dlg), GTK_DIALOG(blistfilter_editor_gui.dialog));
	gtk_widget_show_all(blistfilter_editor_gui.dialog);
}


//====================== Plugins system structs and funcs ======================
/* we tell libpurple in the PurplePluginInfo struct to call this function to
 * get a list of plugin actions to use for the plugin.  This function gives
 * libpurple that list of actions. */

static GList* plugin_actions (PurplePlugin * plugin, gpointer context)
{
	/* some C89 (a.k.a. ANSI C) compilers will warn if any variable declaration
	 * includes an initilization that calls a function.  To avoid that, we
	 * generally initialize our variables first with constant values like NULL
	 * or 0 and assign to them with function calls later */
	GList *list = NULL;
	PurplePluginAction *action = NULL;

	action = purple_plugin_action_new ("Change filters...", blistfilter_filter_editor_cb);
	list = g_list_append (list, action);

	/* Once the list is complete, we send it to libpurple. */
	return list;
}

static gboolean plugin_load (PurplePlugin * plugin)
{	//this is called when plugin is loaded, either on Pidgin startup or when enabled in Plugins dialog.
	int selected_index;
	BListFilterDescription* filter;
	
	buddylistfilter_plugin = plugin;
	blistfilter_filters = NULL;
	blistfilter_gui = g_new0(FilterSelectorGui, 1);

	//loading the currently selected filter
	blistfilter_load_all_filters();
	selected_index = purple_prefs_get_int(PLUGIN_PREF_ACTIVE_FILTER);
	if (selected_index < 0 || selected_index > (int)g_list_length(blistfilter_filters))
	{
		purple_debug_warning(PLUGIN_ID, "Filter #%d was selected, but it does not exist in the prefs. Resetting to 0.\n", selected_index);
		selected_index = 0;
	}
	filter = (BListFilterDescription*)g_list_nth_data(blistfilter_filters, selected_index);
	blistfilter_update_entire_blist(filter);
	purple_debug_misc(PLUGIN_ID, "Selected filter #%d: %s.\n", selected_index, filter ? filter->name : "NULL");

	//connecting to signals
	purple_prefs_connect_callback(plugin, PLUGIN_PREF_ACTIVE_FILTER, blistfilter_active_filter_changed_cb, NULL);

	purple_prefs_connect_callback(plugin, PLUGIN_PREF_ROOT "/buttons_spacing", blistfilter_gui_setting_changed_cb, NULL);
	purple_prefs_connect_callback(plugin, PLUGIN_PREF_ROOT "/selector_style", blistfilter_gui_setting_changed_cb, NULL);
	purple_prefs_connect_callback(plugin, PLUGIN_PREF_ROOT "/force_titles", blistfilter_gui_setting_changed_cb, NULL);
	purple_prefs_connect_callback(plugin, PLUGIN_PREF_ROOT "/homogenous_buttons", blistfilter_gui_setting_changed_cb, NULL);
	
	purple_signal_connect(purple_blist_get_handle(),
		"blist-node-added",
		buddylistfilter_plugin,
		PURPLE_CALLBACK(blistfilter_update_node),
		NULL);
	purple_signal_connect(pidgin_blist_get_handle(), "gtkblist-created", plugin, PURPLE_CALLBACK(blistfilter_make_filter_selector_gui), NULL);
	//Done.
	purple_debug_info(PLUGIN_ID, "Plugin loaded.\n");
	return TRUE;
}

static gboolean plugin_unload(PurplePlugin* plugin)
{
	blistfilter_update_entire_blist(NULL);
	blistfilter_destroy_filter_selector_gui();
	blistfilter_free_all_filters();
	//Done.
	purple_debug_info(PLUGIN_ID, "Plugin unloaded.\n");
	return TRUE;
}

static PurplePluginPrefFrame* get_plugin_pref_frame(PurplePlugin* plugin)
{
	PurplePluginPrefFrame* frame;
	PurplePluginPref* pref;
	
	frame = purple_plugin_pref_frame_new();
	
	pref = purple_plugin_pref_new_with_name_and_label(PLUGIN_PREF_ROOT "/selector_style", "Filter selector style");
	purple_plugin_pref_set_type(pref, PURPLE_PLUGIN_PREF_CHOICE);
	purple_plugin_pref_add_choice(pref, "Vertical list (top)", GINT_TO_POINTER(FST_VERTICAL_TOP));
	purple_plugin_pref_add_choice(pref, "Vertical list (bottom)", GINT_TO_POINTER(FST_VERTICAL_BOTTOM));
	purple_plugin_pref_add_choice(pref, "Horizontal list (top)", GINT_TO_POINTER(FST_HORIZONTAL_TOP));
	purple_plugin_pref_add_choice(pref, "Horizontal list (bottom)", GINT_TO_POINTER(FST_HORIZONTAL_BOTTOM));
	purple_plugin_pref_frame_add(frame, pref);

	pref = purple_plugin_pref_new_with_name_and_label(PLUGIN_PREF_ROOT "/force_titles", "Always show filter names");
	purple_plugin_pref_frame_add(frame, pref);

	pref = purple_plugin_pref_new_with_name_and_label(PLUGIN_PREF_ROOT "/homogenous_buttons", "Keep all buttons same size");
	purple_plugin_pref_frame_add(frame, pref);

	pref = purple_plugin_pref_new_with_name_and_label(PLUGIN_PREF_ROOT "/buttons_spacing", "Selector button spacing (px)");
	purple_plugin_pref_set_bounds(pref, 0, 1280);
	purple_plugin_pref_frame_add(frame, pref);
	
	return frame;
}

/* For specific notes on the meanings of each of these members, consult the C Plugin Howto
 * on the website. */
static PurplePluginUiInfo ui_info = {
	get_plugin_pref_frame,
	0,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL
};

static PurplePluginInfo info = {
	PURPLE_PLUGIN_MAGIC,
	PURPLE_MAJOR_VERSION,
	PURPLE_MINOR_VERSION,
	PURPLE_PLUGIN_STANDARD,
	NULL,
	0,
	NULL,
	PURPLE_PRIORITY_DEFAULT,

	PLUGIN_ID, //Plugin ID. See https://developer.pidgin.im/wiki/CHowTo/ChoosingPluginIds
	"Buddy List Filter", //Visible plugin name
	PLUGIN_VERSION, //Visible plugin version
	"Allows user to set up custom filters (views) of their buddy list.", //Short plugin description
	//Long plugin description
	"This plugin allows user to set up a set of custom views of their buddy list, showing/hiding groups matching specific pattern.\n"
	"It should allow you to structure your buddy list into 'tabs' instead of having to work with one long flat list.",
	"Alex Orlov <the.vindicar@gmail.com>", //plugin author
	"https://github.com/the-vindicar/pidgin-blist-filter",//plugin homepage or github repo


	plugin_load, //to be called on plugin startup
	plugin_unload, //to be called on plugin shutdown
	NULL, //to be called on plugin emergency shutdown

	NULL, //pointer to a UI specific struct like PidginPluginUiInfo
	NULL, 
	&ui_info, //pointer to a UI specific struct like PidginPluginUiInfo - perhaps something to be used in "plugin configuration" dialog?
	// this tells libpurple the address of the function to call to get the list of plugin actions.
	plugin_actions,
	//these are reserved for future use and must be NULL
	NULL,
	NULL,
	NULL,
	NULL
};

static void
init_plugin (PurplePlugin * plugin)
{
	purple_debug_info(PLUGIN_ID, "Plugin probed.\n");
	purple_prefs_add_none(PLUGIN_PREF_ROOT);
	purple_prefs_add_none(PLUGIN_PREF_ROOT "/filters");
	if (!purple_prefs_exists(PLUGIN_PREF_ROOT "/filters/filter0"))
		blistfilter_make_filter_pref(0);
	purple_prefs_add_int(PLUGIN_PREF_ACTIVE_FILTER, 0);
	purple_prefs_add_int(PLUGIN_PREF_ROOT "/buttons_spacing", 0);
	purple_prefs_add_bool(PLUGIN_PREF_ROOT "/selector_style", FST_VERTICAL_TOP);
	purple_prefs_add_bool(PLUGIN_PREF_ROOT "/force_titles", FALSE);
	purple_prefs_add_bool(PLUGIN_PREF_ROOT "/homogenous_buttons", FALSE);
}

PURPLE_INIT_PLUGIN (vindicar_buddylistfilter, init_plugin, info)
