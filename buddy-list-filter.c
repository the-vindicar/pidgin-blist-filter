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
#include <gtkconv.h>
#include "gtkutils.h"
#include "gtkplugin.h"

#define PLUGIN_ID "gtk-vindicar-buddy-list-filter"
#define PLUGIN_VERSION "0.1"
#define PLUGIN_PREF_ROOT "/plugins/gtk/" PLUGIN_ID
#define PLUGIN_PREF_ACTIVE_FILTER PLUGIN_PREF_ROOT "/selected_filter"
#define PLUGIN_PREF_BTN_SPACING PLUGIN_PREF_ROOT "/buttons_spacing"
#define PLUGIN_PREF_SELECTOR_STYLE PLUGIN_PREF_ROOT "/selector_style"
#define PLUGIN_PREF_FORCE_TITLES PLUGIN_PREF_ROOT "/force_titles"
#define PLUGIN_PREF_HOMOGENOUS_BTNS PLUGIN_PREF_ROOT "/homogenous_buttons"
#define PLUGIN_PREF_TRACK_UNREAD PLUGIN_PREF_ROOT "/track_unread"
#define PLUGIN_PREF_NTH_NAME PLUGIN_PREF_ROOT "/filters/filter%d/name"
#define PLUGIN_PREF_NTH_ICON PLUGIN_PREF_ROOT "/filters/filter%d/icon_path"
#define PLUGIN_PREF_NTH_GROUP PLUGIN_PREF_ROOT "/filters/filter%d/group_patterns"
#define PLUGIN_PREF_MAXPATH 256
//Sadly, "\n" is not an option for this. Blame GtkCellRenderer. 
#define PLUGIN_PATTERN_SEPARATOR "|"


#if !PURPLE_VERSION_CHECK(2, 14, 0)
#define PURPLE_BLIST_NODE_FLAG_INVISIBLE 1 << 1
#endif


#if !GLIB_CHECK_VERSION(2, 44, 0)
static inline gpointer
g_steal_pointer (gpointer pp)
{
	gpointer *ptr = (gpointer *) pp;
	gpointer ref;
	ref = *ptr;
	*ptr = NULL;
	return ref;
}
#endif /* 2.44.0 */


/* we're adding this here and assigning it in plugin_load because we need
 * a valid plugin handle for our call to purple_notify_message() in the
 * plugin_action_test_cb() callback function */
PurplePlugin* buddylistfilter_plugin = NULL;

//====================== Base structs and funcs ======================
//This describes pattern type - match all, regular, inverted, miscellaneous
typedef enum { BLFT_REGULAR, BLFT_INVERTED, BLFT_ALL, BLFT_MISC } BListFilterType;
//This describes a single pattern against which a string can be matched either positively or negatively
typedef struct
{
	GPatternSpec* pattern;
	BListFilterType type;
} BListFilter;
//This describes a single compiled filter as a named set of patterns.
//We also track unread messages in contacts that are visible under this filter,
//so unread count is also stored here. 
typedef struct 
{
	const char* name;
	const char* icon_path;
	GList* group_patterns;
	int matching_unreads;
} BListFilterDescription;
//This global list stores currently active compiled filters
static GList* blistfilter_filters;
//clears and frees individual BListFilter struct 
static void free_pattern(gpointer data) 
{ 
	g_pattern_spec_free(((BListFilter*)data)->pattern);
	g_free((BListFilter*)data);
}

//Checks if a string is matching any of the specified list of glob-like patterns
//It MUST match ANY of the positive patterns (if any is present), and it MUST NOT match ANY of the negative patterns.  
//Empty list will match any string. Empty string will match any list.
static gboolean blistfilter_is_matching_list(const char* value, GList* patterns, gboolean regular_only)
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
		switch (item->type)
		{
			case BLFT_REGULAR:
			{
				has_positive_matches = has_positive_matches || g_pattern_match(item->pattern, length, value, reversed);
				has_any_positives = TRUE;
			}; break;
			case BLFT_INVERTED:
			{
				has_negative_matches = has_negative_matches || g_pattern_match(item->pattern, length, value, reversed);
			}; break;
			case BLFT_ALL:
			{
				if (!regular_only)
				{
					has_positive_matches = TRUE;
					has_any_positives = TRUE;
				}
			}; break;
			case BLFT_MISC:
			{
				if (!regular_only) //recursion stop condition
				{	//if any other non-all filter matches this group, we don't want to see it
					for (GList* filter = blistfilter_filters; !has_negative_matches && (filter != NULL); filter = filter->next)
					{
						BListFilterDescription* filterdesc = (BListFilterDescription*)(filter->data);
						gboolean has_regular_filters = FALSE;
						for (GList* flt = filterdesc->group_patterns; !has_regular_filters && (flt != NULL); flt = flt->next)
						{
							BListFilter* subitem = (BListFilter*)flt->data;
							has_regular_filters = has_regular_filters || (subitem->type == BLFT_REGULAR) || (subitem->type == BLFT_INVERTED);
						}
						if (has_regular_filters)
							has_negative_matches = has_negative_matches || blistfilter_is_matching_list(value, filterdesc->group_patterns, TRUE);
					}
				}
			}
		}
	}
	g_free(reversed);
	//we show only groups that match ANY positive filter (if such filters exist), but also don't match ANY of the negative filters
	return (!has_any_positives || has_positive_matches) && !has_negative_matches;
}
//Checks if a group node matches the specified filter. 
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
	return blistfilter_is_matching_list(group_name, filter->group_patterns, FALSE);
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

//Loads unread convo count
static void blistfilter_update_all_unread_counters()
{
	GList* convos;
	PurpleConversation* conv;
	BListFilterDescription* filter;
	PurpleBlistNode* node;

	if (!blistfilter_filters) return;
	//reset all counters to zero
	for (GList* item = blistfilter_filters; item != NULL; item = item->next)
	{
		filter = (BListFilterDescription*)item->data;
		if (filter) filter->matching_unreads = 0;
	}
	//get list of conversations with unread messages
	convos = pidgin_conversations_find_unseen_list(PURPLE_CONV_TYPE_ANY, PIDGIN_UNSEEN_TEXT, TRUE, 0);
	for (GList* item = convos; item != NULL; item = item->next)
		if ((conv = (PurpleConversation*)item->data) != NULL)
		{
			switch (conv->type) //looking for buddy list node corresponsing to this conversation
			{
				case PURPLE_CONV_TYPE_IM:
				{	//if it's an IM, we look for a buddy
					PurpleBuddy* buddy;
					buddy = purple_find_buddy(conv->account, purple_conversation_get_name(conv));
					node = buddy ? &(buddy->node) : NULL;
				}; break;
				case PURPLE_CONV_TYPE_CHAT:
				{	//if it's a chat, we look for a chat room
					PurpleChat* chat;
					chat = purple_blist_find_chat(conv->account, purple_conversation_get_name(conv));
					node = chat ? &(chat->node) : NULL;
				}; break;
				default: node = NULL; break; //we ignore everything else
			}
			while (node && !PURPLE_BLIST_NODE_IS_GROUP(node)) //looking for the group containing this node
				node = node->parent;
			if (node) //did we find the group?
			{
				//now bump counters on every filter that matches this group
				for (GList* item = blistfilter_filters; item != NULL; item = item->next)
				{
					filter = (BListFilterDescription*)item->data;
					if (filter && blistfilter_match_group(node, filter))
						filter->matching_unreads++;
				} 				
			}
		}
	g_list_free(convos);
}

//====================== Pref structs and funcs ======================
//Prepares a pref template for a filter with given id. Will not overwrite existing values!
static void blistfilter_make_filter_pref(int filter_id)
{
	char pref_name_buffer[PLUGIN_PREF_MAXPATH];
	snprintf(pref_name_buffer, PLUGIN_PREF_MAXPATH, PLUGIN_PREF_ROOT "/filters/filter%d", filter_id);
	purple_prefs_add_none(pref_name_buffer);
	snprintf(pref_name_buffer, PLUGIN_PREF_MAXPATH, PLUGIN_PREF_NTH_NAME, filter_id);
	purple_prefs_add_string(pref_name_buffer, "Everything");
	snprintf(pref_name_buffer, PLUGIN_PREF_MAXPATH, PLUGIN_PREF_NTH_ICON, filter_id);
	purple_prefs_add_path(pref_name_buffer, NULL);
	snprintf(pref_name_buffer, PLUGIN_PREF_MAXPATH, PLUGIN_PREF_NTH_GROUP, filter_id);
	purple_prefs_add_string_list(pref_name_buffer, NULL);
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
	int linelen;
	
	strings = purple_prefs_get_string_list(path);
	for (GList* item = strings; item != NULL; item = item->next)
	{
		const gchar* line = (const gchar*)item->data;
		linelen = strlen(line);
		if (linelen > 0)//we ignore empty filters
		{
			filter = g_new0(BListFilter, 1);
			if (line[0] == '~' && linelen > 1) //filters starting with ~ are negative
			{
				filter->pattern = g_pattern_spec_new(&line[1]);
				filter->type = BLFT_INVERTED;
			}
			else if (line[0] == '~' && linelen == 1) //filter consisting of ~ is a catch-all "everything else" filter
			{
				filter->pattern = NULL;
				filter->type = BLFT_MISC;
			}
			else if (line[0] == '*' && linelen == 1)
			{
				filter->pattern = NULL;
				filter->type = BLFT_ALL;
			}
			else
			{
				filter->pattern = g_pattern_spec_new(line);
				filter->type = BLFT_REGULAR;
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
	
	snprintf(pref_name_buffer, PLUGIN_PREF_MAXPATH, PLUGIN_PREF_NTH_NAME, filter_id);
	if (!purple_prefs_exists(pref_name_buffer))
		return NULL;
	filter = g_new0(BListFilterDescription, 1);
	filter->name = purple_prefs_get_string(pref_name_buffer);
	snprintf(pref_name_buffer, PLUGIN_PREF_MAXPATH, PLUGIN_PREF_NTH_ICON, filter_id);
	filter->icon_path = purple_prefs_get_path(pref_name_buffer);
	snprintf(pref_name_buffer, PLUGIN_PREF_MAXPATH, PLUGIN_PREF_NTH_GROUP, filter_id);
	filter->group_patterns = blistfilter_prefs_load_patterns(pref_name_buffer);
	return filter;
}
//Frees filter list
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
//Selector types: vertical stack of buttons or horizontal pane of buttons
typedef enum 
{ 
	FST_VERTICAL_TOP,
	FST_VERTICAL_BOTTOM, 
	FST_HORIZONTAL_TOP,
	FST_HORIZONTAL_BOTTOM, 
	FST_INVALID
} FilterSelectorStyle;
//stores handles of important filter selector GUI elements
struct
{
	GtkWidget* box;
	GList* buttons;
} blistfilter_gui;

//Callback triggered when user selects a filter using GUI 
static void blistfilter_button_cb(GtkButton* btn, gpointer user_data)
{
	int new_filter_id;
	char pref_name_buffer[PLUGIN_PREF_MAXPATH];

	new_filter_id = GPOINTER_TO_INT(user_data);
	snprintf(pref_name_buffer, PLUGIN_PREF_MAXPATH, PLUGIN_PREF_ROOT "/filters/filter%d", new_filter_id);
	if (purple_prefs_exists(pref_name_buffer))
	{
		purple_prefs_set_int(PLUGIN_PREF_ACTIVE_FILTER, new_filter_id);
	}
	else
	{
		purple_notify_error(buddylistfilter_plugin, "Filter not found", "Somehow, the filter you've selected, does not exist!", NULL);
		purple_prefs_set_int(PLUGIN_PREF_ACTIVE_FILTER, 0);
	}
}
//Destroys created filter selector panel. Useful when it's being remade, or when plugin is unloaded.
static void blistfilter_destroy_filter_selector_gui()
{
	g_list_free_full(g_steal_pointer(&blistfilter_gui.buttons), g_object_unref);
	if (blistfilter_gui.box)
	{
		g_object_unref(blistfilter_gui.box);
		gtk_widget_destroy(g_steal_pointer(&blistfilter_gui.box));
	}
}
//configures label and icon of a filter button, depending on filter settings and unread messages count
static void blistfilter_configure_selector_button(GtkWidget* btn, const BListFilterDescription* filter, int index, gboolean prefer_icon)
{
	char* namebuffer;
	char* hintbuffer;
	size_t len;
	int variant;
	
	len = filter->name ? strlen(filter->name)+32 : 32;
	namebuffer = g_new0(char, len);
	hintbuffer = g_new0(char, len);
	variant = 0;
	if (filter->name && filter->name[0]) variant |= 0x01;
	if (filter->icon_path && filter->icon_path[0]) variant |= 0x02;
	if (filter->matching_unreads > 0) variant |= 0x04;
	switch (variant)
	{
		case 0x00: //no name, no icon, no unreads
		{
			snprintf(namebuffer, len, "Filter #%d", index);
			snprintf(hintbuffer, len, "Filter #%d", index);
		}; break;
		case 0x01: //name, no icon, no unreads
		{
			snprintf(namebuffer, len, "%s", filter->name);
			snprintf(hintbuffer, len, "%s", filter->name);
		}; break;
		case 0x02: //no name, icon, no unreads
		{
			if (prefer_icon)
				namebuffer[0] = 0;
			else
				snprintf(namebuffer, len, "Filter #%d", index);
			snprintf(hintbuffer, len, "Filter #%d", index);
			gtk_button_set_image(GTK_BUTTON(btn), gtk_image_new_from_file(filter->icon_path));
			gtk_button_set_image_position(GTK_BUTTON(btn), GTK_POS_LEFT);
		}; break;
		case 0x03: //name, icon, no unreads
		{
			if (prefer_icon)
				namebuffer[0] = 0;
			else
				snprintf(namebuffer, len, "%s", filter->name);
			snprintf(hintbuffer, len, "%s", filter->name);
			gtk_button_set_image(GTK_BUTTON(btn), gtk_image_new_from_file(filter->icon_path));
			gtk_button_set_image_position(GTK_BUTTON(btn), GTK_POS_LEFT);
		}; break;
		case 0x04: //no name, no icon, has unreads
		{
			snprintf(namebuffer, len, "[%d] Filter #%d", filter->matching_unreads, index);
			snprintf(hintbuffer, len, "[%d] Filter #%d", filter->matching_unreads, index);
		}; break;
		case 0x05: //name, no icon, has unreads
		{
			snprintf(namebuffer, len, "[%d] %s", filter->matching_unreads, filter->name);
			snprintf(hintbuffer, len, "[%d] %s", filter->matching_unreads, filter->name);
		}; break;
		case 0x06: //no name, icon, has unreads
		{
			if (prefer_icon)
				snprintf(namebuffer, len, "[%d]", filter->matching_unreads);
			else
				snprintf(namebuffer, len, "[%d] Filter #%d", filter->matching_unreads, index);
			snprintf(hintbuffer, len, "[%d] Filter #%d", filter->matching_unreads, index);
			gtk_button_set_image(GTK_BUTTON(btn), gtk_image_new_from_file(filter->icon_path));
			gtk_button_set_image_position(GTK_BUTTON(btn), GTK_POS_LEFT);
		}; break;
		case 0x07: //name, icon, has unreads
		{
			if (prefer_icon)
				snprintf(namebuffer, len, "[%d]", filter->matching_unreads);
			else
				snprintf(namebuffer, len, "[%d] %s", filter->matching_unreads, filter->name);
			snprintf(hintbuffer, len, "[%d] %s", filter->matching_unreads, filter->name);
			gtk_button_set_image(GTK_BUTTON(btn), gtk_image_new_from_file(filter->icon_path));
			gtk_button_set_image_position(GTK_BUTTON(btn), GTK_POS_LEFT);
		}; break;
	}
	gtk_button_set_label(GTK_BUTTON(btn), namebuffer);
	gtk_widget_set_tooltip_text(GTK_WIDGET(btn), hintbuffer);
	g_free(namebuffer);
	g_free(hintbuffer);
} 
//Updates all buttons' icons/labels according to filter settigns and unread counts
static void blistfilter_update_all_buttons()
{
	gboolean prefer_icon, always_show_titles;
	int selector_style;
	int filter_id = 0;	
	GList* btn = blistfilter_gui.buttons;
	
	selector_style = purple_prefs_get_int(PLUGIN_PREF_SELECTOR_STYLE);
	always_show_titles = purple_prefs_get_bool(PLUGIN_PREF_FORCE_TITLES);
	prefer_icon = ((selector_style == FST_HORIZONTAL_TOP) || (selector_style == FST_HORIZONTAL_BOTTOM)) && !always_show_titles; 
	for (GList* item = blistfilter_filters; item != NULL; item = item->next)
	{
		blistfilter_configure_selector_button(GTK_WIDGET(btn->data), (BListFilterDescription*)item->data, filter_id, prefer_icon);
		btn = btn->next;
		filter_id++;
	}
}

//Creates filter selector panel in vertical orientation
static void blistfilter_make_vertical_selector_gui()
{
	int spacing;
	int selected_index;
	int filter_id;
	GtkRadioButton* btn;
	GSList* group;
	
	selected_index = purple_prefs_get_int(PLUGIN_PREF_ACTIVE_FILTER);
	spacing = purple_prefs_get_int(PLUGIN_PREF_BTN_SPACING);
	blistfilter_gui.box = g_object_ref(gtk_vbox_new(TRUE, spacing));
	gtk_box_set_homogeneous(GTK_BOX(blistfilter_gui.box), purple_prefs_get_bool(PLUGIN_PREF_HOMOGENOUS_BTNS));
	gtk_widget_set_name(GTK_WIDGET(blistfilter_gui.box), "blistfilter_gui_box");
	
	filter_id = 0;
	group = NULL;
	for (GList* item = blistfilter_filters; item != NULL; item = item->next)
	{
		btn = GTK_RADIO_BUTTON(gtk_radio_button_new(group));
		group = gtk_radio_button_get_group(btn);
		gtk_button_set_focus_on_click(GTK_BUTTON(btn), FALSE);
		gtk_toggle_button_set_mode(GTK_TOGGLE_BUTTON(btn), FALSE);
		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(btn), selected_index == filter_id);
		gtk_box_pack_start(GTK_BOX(blistfilter_gui.box), GTK_WIDGET(btn), TRUE, TRUE, 0);
		blistfilter_gui.buttons = g_list_append(blistfilter_gui.buttons, g_object_ref(btn));
		filter_id++;
	}
	blistfilter_update_all_buttons();
	filter_id = 0;
	for (GList* item = blistfilter_gui.buttons; item != NULL; item = item->next)
	{
		g_signal_connect(G_OBJECT(item->data), "clicked", G_CALLBACK(blistfilter_button_cb), GINT_TO_POINTER(filter_id));
		filter_id++;
	}
}
//Creates filter selector panel in horizontal orientation
static void blistfilter_make_horizontal_selector_gui()
{
	int spacing;
	int selected_index;
	int filter_id;
	GtkRadioButton* btn;
	GSList* group;
	
	selected_index = purple_prefs_get_int(PLUGIN_PREF_ACTIVE_FILTER);
	spacing = purple_prefs_get_int(PLUGIN_PREF_BTN_SPACING);
	blistfilter_gui.box = g_object_ref(gtk_hbox_new(TRUE, spacing));
	gtk_box_set_homogeneous(GTK_BOX(blistfilter_gui.box), purple_prefs_get_bool(PLUGIN_PREF_HOMOGENOUS_BTNS));
	gtk_widget_set_name(GTK_WIDGET(blistfilter_gui.box), "blistfilter_gui_box");
	
	filter_id = 0;
	group = NULL;
	for (GList* item = blistfilter_filters; item != NULL; item = item->next)
	{
		btn = GTK_RADIO_BUTTON(gtk_radio_button_new(group));
		group = gtk_radio_button_get_group(btn);
		gtk_button_set_focus_on_click(GTK_BUTTON(btn), FALSE);
		gtk_toggle_button_set_mode(GTK_TOGGLE_BUTTON(btn), FALSE);
		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(btn), selected_index == filter_id);
		gtk_box_pack_start(GTK_BOX(blistfilter_gui.box), GTK_WIDGET(btn), TRUE, TRUE, 0);
		blistfilter_gui.buttons = g_list_append(blistfilter_gui.buttons, g_object_ref(btn));
		filter_id++;
	}
	blistfilter_update_all_buttons();
	filter_id = 0;
	for (GList* item = blistfilter_gui.buttons; item != NULL; item = item->next)
	{
		g_signal_connect(G_OBJECT(item->data), "clicked", G_CALLBACK(blistfilter_button_cb), GINT_TO_POINTER(filter_id));
		filter_id++;
	}
}
//(Re-)creates filter selector panel and attaches it accordingly
static void blistfilter_make_filter_selector_gui()
{
	FilterSelectorStyle selector_style;
	PidginBuddyList* gtkblist;
	
	gtkblist = pidgin_blist_get_default_gtk_blist();
	//if there is no buddy list window, or if our container already exists, do nothing;
	if (!gtkblist || !gtkblist->window)
	{
		//nothing to do if blist is not there
		return;
	}
	blistfilter_destroy_filter_selector_gui();
	selector_style = purple_prefs_get_int(PLUGIN_PREF_SELECTOR_STYLE);
	if (selector_style >= FST_INVALID)
	{
		purple_debug_warning(PLUGIN_ID, "Invalid selector style code (%d), changing to default.\n", selector_style);
		selector_style = FST_VERTICAL_TOP;
		purple_prefs_set_int(PLUGIN_PREF_SELECTOR_STYLE, selector_style);
	}
	blistfilter_update_all_unread_counters();
	switch (selector_style)
	{
		case FST_VERTICAL_TOP:
		{ //create vertical list of buttons with icons and text
			blistfilter_make_vertical_selector_gui();
			gtk_box_pack_start(GTK_BOX(gtkblist->vbox), blistfilter_gui.box, FALSE, FALSE, 0);
			gtk_box_reorder_child(GTK_BOX(gtkblist->vbox), GTK_WIDGET(blistfilter_gui.box), 0);
		}; break;
		case FST_VERTICAL_BOTTOM:
		{ //create vertical list of buttons with icons and text
			blistfilter_make_vertical_selector_gui();
			gtk_box_pack_start(GTK_BOX(gtkblist->vbox), blistfilter_gui.box, FALSE, FALSE, 0);
		}; break;
		case FST_HORIZONTAL_TOP: 
		{ //create horizontal list of buttons with icons only
			blistfilter_make_horizontal_selector_gui();
			gtk_box_pack_start(GTK_BOX(gtkblist->vbox), blistfilter_gui.box, FALSE, FALSE, 0);
			gtk_box_reorder_child(GTK_BOX(gtkblist->vbox), GTK_WIDGET(blistfilter_gui.box), 0);
		}; break;
		case FST_HORIZONTAL_BOTTOM: 
		{ //create horizontal list of buttons with icons only
			blistfilter_make_horizontal_selector_gui();
			gtk_box_pack_start(GTK_BOX(gtkblist->vbox), blistfilter_gui.box, FALSE, FALSE, 0);
		}; break;
		default: {
			purple_debug_error(PLUGIN_ID, "Wait, how did THAT happen? Selector style code is %d despite having been normalized!\n", selector_style);
		}; break;
	}
	gtk_widget_show_all(blistfilter_gui.box);
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
	if (filter != NULL)
	{
		blistfilter_update_entire_blist(filter);
	}
	else if (selected_index != 0)
	{
		purple_debug_error(PLUGIN_ID, "Filter #%d has been selected, but it is not present. Resetting it to 0.\n", selected_index);
		purple_prefs_set_int(PLUGIN_PREF_ACTIVE_FILTER, 0);
	}
	else
	{
		purple_debug_error(PLUGIN_ID, "Filter #0 has been selected, but it is not present. This REALLY shouldn't have happened, so we just show everything.\n");
		blistfilter_update_entire_blist(NULL);
	}
}

//Triggers whenever a GUI setting changes. Updates GUI immediately.
static void blistfilter_gui_setting_changed_cb(const char* name, PurplePrefType type, gconstpointer val, gpointer data)
{
	blistfilter_make_filter_selector_gui();
}

//Triggers when a conversation is updated.
static void blistfilter_convo_has_updated(PurpleConversation* conv, PurpleConvUpdateType type)
{
	if (type == PURPLE_CONV_UPDATE_UNSEEN)
	{
		blistfilter_update_all_unread_counters();
		blistfilter_update_all_buttons();
	}
}

//====================== Editor stuff ======================
//global storage for editor GUI objects used by the callbacks.
//if C had classes, it'd be a class instance.
static struct {
	GtkWidget* dialog;
	GtkTreeView* view;
	GtkListStore* model;
} blistfilter_editor_gui;
//Creates a GtkListStore based on filter data stored in prefs. 
//This store will be used in filter editor dialog, and will be exported back to prefs if user saves the changes.  
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
		snprintf(pref_name, PLUGIN_PREF_MAXPATH, PLUGIN_PREF_NTH_NAME, filter_id);
		if (!purple_prefs_exists(pref_name)) break; //No name node? we must've loaded everything
		filter_name = purple_prefs_get_string(pref_name);
		snprintf(pref_name, PLUGIN_PREF_MAXPATH, PLUGIN_PREF_NTH_ICON, filter_id);
		filter_icon = purple_prefs_get_path(pref_name);
		//We have to combine a string list into a single string for ease of editing.
		//A specified separator character is used for that purpose
		snprintf(pref_name, PLUGIN_PREF_MAXPATH, PLUGIN_PREF_NTH_GROUP, filter_id);
		filter_pattern_bits = purple_prefs_get_string_list(pref_name);
		bits_length = g_list_length(filter_pattern_bits);
		bits = g_new0(gchar*, bits_length+1); //the array must be NULL-terminated, so we add an extra element and leave it as NULL 
		//GList of gchar* --> gchar**		
		bit_index = 0;
		for (GList* i = filter_pattern_bits; i != NULL; i = i->next)
			bits[bit_index++] = (gchar*)i->data;
		filter_patterns = g_strjoinv(PLUGIN_PATTERN_SEPARATOR, bits); //got our single string
		g_free(bits); //clear the array, but not the items - those are owned by the GList
		g_list_free_full(g_steal_pointer(&filter_pattern_bits), g_free); //clear the GList
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
//Export a GtkListStore to filter prefs
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
	//after the save, selected filter might end up out of bounds for the new array
	selected_id = purple_prefs_get_int(PLUGIN_PREF_ACTIVE_FILTER);
	filter_id = 0;
	//we overwrite first N filters in the prefs with new values 
	do
	{
		gtk_tree_model_get(GTK_TREE_MODEL(model), &iter, 0, &name_value, 1, &icon_value, 2, &pattern_value, -1);
		snprintf(pref_name, PLUGIN_PREF_MAXPATH, PLUGIN_PREF_ROOT "/filters/filter%d", filter_id);
		if (!purple_prefs_exists(pref_name)) //create filter branch if necessary - just overwrite if not
			purple_prefs_add_none(pref_name);
		snprintf(pref_name, PLUGIN_PREF_MAXPATH, PLUGIN_PREF_NTH_NAME, filter_id);
		purple_prefs_set_string(pref_name, name_value);
		snprintf(pref_name, PLUGIN_PREF_MAXPATH, PLUGIN_PREF_NTH_ICON, filter_id);
		purple_prefs_set_string(pref_name, icon_value);
		
		bits = g_strsplit(pattern_value, PLUGIN_PATTERN_SEPARATOR, 0);
		filter_pattern_bits = NULL;
		for (int i = 0; bits[i] != NULL; i++)
		{
			filter_pattern_bits = g_list_append(filter_pattern_bits, bits[i]);
		}
		snprintf(pref_name, PLUGIN_PREF_MAXPATH, PLUGIN_PREF_NTH_GROUP, filter_id);
		purple_prefs_set_string_list(pref_name, filter_pattern_bits);
		g_list_free(filter_pattern_bits);
		g_strfreev(bits);
		
		g_free(name_value);
		g_free(icon_value);
		g_free(pattern_value);
		filter_id++;
	}
	while (gtk_tree_model_iter_next(GTK_TREE_MODEL(model), &iter));
	filter_count = filter_id; //now we remember how many filters we have 
	//then we remove any extraneous filters left in the prefs
	do
	{
		snprintf(pref_name, PLUGIN_PREF_MAXPATH, PLUGIN_PREF_ROOT "/filters/filter%d", filter_id);
		if (purple_prefs_exists(pref_name))
			purple_prefs_remove(pref_name);
		else
			break; //no extra filters left, we can stop
		filter_id++;
	}
	while (TRUE);
	//ensure selected filter index is not out of bounds
	if (selected_id >= filter_count) 
		purple_prefs_set_int(PLUGIN_PREF_ACTIVE_FILTER, filter_count-1);
}
//destroys stored references to editor GUI objects
static void blistfilter_destroy_editor()
{
	g_clear_object(&blistfilter_editor_gui.dialog);
	g_clear_object(&blistfilter_editor_gui.view);
	g_clear_object(&blistfilter_editor_gui.model);
}
//Callback triggered when user chooses to save the changes in filter editor
static void blistfilter_save_filters_dlg(GtkWidget *w, GtkWidget *window)
{
	int selected_index;
	BListFilterDescription* filter;
	
	if (blistfilter_editor_gui.model) //should always be true. just in case we *somehow* don't have the model.
	{
		blistfilter_destroy_filter_selector_gui(); //get rid of current set of filter selector buttons 
		blistfilter_save_model_to_prefs(blistfilter_editor_gui.model); //store model of the filter set to preferences
		blistfilter_load_all_filters(); //load that filter set from preferences and compile it
		selected_index = purple_prefs_get_int(PLUGIN_PREF_ACTIVE_FILTER); //double-check selected filter index is not out of bounds
		if (selected_index < 0 || selected_index > (int)g_list_length(blistfilter_filters))
		{
			selected_index = 0;
			purple_prefs_set_int(PLUGIN_PREF_ACTIVE_FILTER, selected_index);
		}
		blistfilter_make_filter_selector_gui(); //remake filter selector buttons with new filter set and new selected index 
		filter = (BListFilterDescription*)g_list_nth_data(blistfilter_filters, selected_index);
		blistfilter_update_entire_blist(filter); //ensure currently selected filter is applied to the buddy list
	}
	blistfilter_destroy_editor(); //get rid of our stored references to filter editor GUI objects
	gtk_widget_destroy(window); //the window itself can go, too
}
//Callback triggered when user chooses to discard changes in filter editor 
static void blistfilter_close_filters_dlg(GtkWidget *w, GtkWidget *window)
{
	blistfilter_destroy_editor(); //get rid of our stored references to filter editor GUI objects
	gtk_widget_destroy(window); //the window itself can go, too
}
//Callback triggered when user wants to add a new filter
static void blistfilter_add_new_filter_cb(GtkWidget *w, GtkWidget *window)
{
	GtkTreeIter iter;
	GtkTreeSelection* selection;
	
	if (!blistfilter_editor_gui.model) return; //should never fire, but...
	selection = gtk_tree_view_get_selection(blistfilter_editor_gui.view);
	gtk_list_store_insert_with_values( //add new filter to the end of the model
		blistfilter_editor_gui.model, 
		&iter,
		-1, //end of the list
		0, "New filter",
		1, "",
		2, "",
		-1);
	gtk_tree_selection_select_iter(selection, &iter); //make sure it's selected
}
//Callback triggered when user wants to delete a filter
static void blistfilter_delete_filter_cb(GtkWidget *w, GtkWidget *window)
{
	int length;
	GtkTreeSelection* selection;
	GtkTreeIter iter, new_selection_iter;
	gboolean has_new_selection;

	if (!blistfilter_editor_gui.model) return;//should never fire, but...
	//lets make sure we aren't deleting the last filter
	length = gtk_tree_model_iter_n_children(GTK_TREE_MODEL(blistfilter_editor_gui.model), NULL);
	if (length < 2)
	{
		purple_notify_error(buddylistfilter_plugin, "Last filter", "Last filter", "Can't delete the last filter.");
		return;
	}
	//find out which filter is selected
	selection = gtk_tree_view_get_selection(blistfilter_editor_gui.view);
	if (!gtk_tree_selection_get_selected(selection, (GtkTreeModel**)(&blistfilter_editor_gui.model), &iter))
	{
		purple_notify_error(buddylistfilter_plugin, "No selection", "No selection", "Nothing is selected.");
		return;
	}
	//it's preferrable if we have selection after deletion as well, so let's determine which one it will be
	new_selection_iter = iter;
	if (gtk_tree_model_iter_next(GTK_TREE_MODEL(blistfilter_editor_gui.model), &new_selection_iter))
		has_new_selection = TRUE; //okay, so we have an element after the one we deleted.
	else	//we don't, so we try and pick the first one
		has_new_selection = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(blistfilter_editor_gui.model), &new_selection_iter);
	gtk_list_store_remove(blistfilter_editor_gui.model, &iter); //anyway, drop the element
	if (has_new_selection) //if we somehow couldn't find the selection, it will be up to user. Othwerwise, select.
		gtk_tree_selection_select_iter(selection, &new_selection_iter);
}
//Callback triggered when user wants to move a filter up in the list
static void blistfilter_move_filter_up_cb(GtkWidget *w, GtkWidget *window)
{
	GtkTreeSelection* selection;
	GtkTreeIter iter_selected, iter_before;
	GtkTreePath* path;

	if (!blistfilter_editor_gui.model) return;
	//figuring out which element is selected
	selection = gtk_tree_view_get_selection(blistfilter_editor_gui.view);
	if (!gtk_tree_selection_get_selected(selection, (GtkTreeModel**)(&blistfilter_editor_gui.model), &iter_selected))
	{
		purple_notify_error(buddylistfilter_plugin, "No selection", "No selection", "Nothing is selected.");
		return;
	}
	//GTK 2.0 does not have gtk_tree_model_iter_previous() what the HELL >_<
	//stole the implementation from GTK 3+ 
	path = gtk_tree_model_get_path(GTK_TREE_MODEL(blistfilter_editor_gui.model), &iter_selected);
	if (gtk_tree_path_prev(path) && gtk_tree_model_get_iter(GTK_TREE_MODEL(blistfilter_editor_gui.model), &iter_before, path))
	{
		gtk_list_store_swap(blistfilter_editor_gui.model, &iter_before, &iter_selected);
		gtk_tree_selection_select_iter(selection, &iter_selected);
	}
	gtk_tree_path_free(path);
}
//Callback triggered when user wants to move a filter down in the list
static void blistfilter_move_filter_down_cb(GtkWidget *w, GtkWidget *window)
{
	GtkTreeSelection* selection;
	GtkTreeIter iter_selected, iter_after;

	if (!blistfilter_editor_gui.model) return;
	//figuring out which element is selected
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

//Callback reacts to a filter view cell being edited. We have to store the new value in the model ourselves.
static void blistfilter_filter_renderer_edited_cb(GtkCellRendererText* cell, gchar* path, char* new_text, gpointer column_idx_as_ptr)
{
	GtkTreeIter iter;
	//figure out which filter has been edited
	gtk_tree_model_get_iter_from_string(GTK_TREE_MODEL(blistfilter_editor_gui.model), &iter, path);
	//store new value in the model
	gtk_list_store_set(blistfilter_editor_gui.model, &iter, GPOINTER_TO_INT(column_idx_as_ptr), new_text, -1);
}

//Callback is triggered when user wants to edit the filters. Creates the dialog window and shows it.
static void blistfilter_filter_editor_cb(PurplePluginAction *unused)
{
	GtkWidget* toptext; //filter syntax hint
	GtkWidget* dlgbox; //dialog box that shows everything
	GtkWidget* scroll; //scroll pane for the filter treeview
	GtkWidget* commandbox; //command button container
	GtkWidget* btn; 
	GtkCellRenderer* renderer;
	GtkTreeViewColumn *column;
	GtkTreeSelection* selection;
	
	blistfilter_destroy_editor(); //clear out existing dialog window if it was present.
	//set up dialog window and its container
	blistfilter_editor_gui.model = g_object_ref(blistfilter_load_model_from_prefs());
	blistfilter_editor_gui.dialog = GTK_WIDGET(g_object_ref(pidgin_create_dialog("Pidgin Buddylist Filter Editor", 0, "blistfilter-editor", FALSE)));
	gtk_window_set_resizable(GTK_WINDOW(blistfilter_editor_gui.dialog), TRUE); //let user resize the dialog as they see fit
	dlgbox = pidgin_dialog_get_vbox_with_properties(GTK_DIALOG(blistfilter_editor_gui.dialog), FALSE, PIDGIN_HIG_BOX_SPACE);
	//a cheatsheet for the user
	toptext = gtk_label_new(
		"Pattern cheatsheet:\n"
		"    Work - matches a group named 'Work'\n" 
		"    ~Work - matches any group except one named 'Work'\n" 
		"    Work: * - matches groups like 'Work: HR' or 'Work: Accounting'\n" 
		"    Friends|Family - matches a group named 'Friends' and a group named 'Family'\n"
		"    Work: *|~Work: IT - matches any group named like 'Work: HR' or 'Work: Accounting', unless it's 'Work: IT'\n"
		"    ~Work: HR|~Work: IT - matches any group except 'Work: HR' and 'Work: IT'\n"
		"    ~ - matches everything that can't be seen through the other filters\n"
		"    Empty pattern (no spaces!) matches everything." 
	);
	gtk_box_pack_start(GTK_BOX(dlgbox), toptext, FALSE, FALSE, 0);
	//setting up scrollbox for the treeview
	scroll = gtk_scrolled_window_new(NULL, NULL);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
	//filter list (actually a treeview) 
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
	
	gtk_container_add(GTK_CONTAINER(scroll), GTK_WIDGET(blistfilter_editor_gui.view));
	gtk_box_pack_start(GTK_BOX(dlgbox), GTK_WIDGET(scroll), TRUE, TRUE, 0);
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
	//add save/close buttons
	gtk_dialog_set_has_separator(GTK_DIALOG(blistfilter_editor_gui.dialog), TRUE);
	pidgin_dialog_add_button(GTK_DIALOG(blistfilter_editor_gui.dialog), GTK_STOCK_SAVE, G_CALLBACK(blistfilter_save_filters_dlg), GTK_DIALOG(blistfilter_editor_gui.dialog));
	pidgin_dialog_add_button(GTK_DIALOG(blistfilter_editor_gui.dialog), GTK_STOCK_CLOSE, G_CALLBACK(blistfilter_close_filters_dlg), GTK_DIALOG(blistfilter_editor_gui.dialog));
	//show dialog window
	gtk_widget_show_all(blistfilter_editor_gui.dialog);
}

//====================== Plugins system structs and funcs ======================
/* we tell libpurple in the PurplePluginInfo struct to call this function to
 * get a list of plugin actions to use for the plugin.  This function gives
 * libpurple that list of actions. */

static GList* plugin_actions (PurplePlugin * plugin, gpointer context)
{
	GList *list = NULL;
	PurplePluginAction *action;
	//show filter editor dialog
	action = purple_plugin_action_new ("Change filters...", blistfilter_filter_editor_cb);
	list = g_list_append (list, action);
	
	return list;
}

//this is called when plugin is loaded, either on Pidgin startup or when enabled in Plugins dialog.
static gboolean plugin_load (PurplePlugin * plugin)
{	
	int selected_index;
	BListFilterDescription* filter;
	//store reference to our plugin for future use
	buddylistfilter_plugin = plugin;
	
	//zero out the global structs
	blistfilter_filters = NULL;
	blistfilter_gui.box = NULL;
	blistfilter_gui.buttons = NULL;
	blistfilter_editor_gui.dialog = NULL;
	blistfilter_editor_gui.view = NULL;
	blistfilter_editor_gui.model = NULL;

	//ensure we have a set of default settings
	purple_prefs_add_none(PLUGIN_PREF_ROOT);
	purple_prefs_add_none(PLUGIN_PREF_ROOT "/filters");
	if (!purple_prefs_exists(PLUGIN_PREF_ROOT "/filters/filter0"))
		blistfilter_make_filter_pref(0);
	purple_prefs_add_int(PLUGIN_PREF_ACTIVE_FILTER, 0);
	purple_prefs_add_int(PLUGIN_PREF_BTN_SPACING, 0);
	purple_prefs_add_int(PLUGIN_PREF_SELECTOR_STYLE, FST_VERTICAL_TOP);
	purple_prefs_add_bool(PLUGIN_PREF_FORCE_TITLES, FALSE);
	purple_prefs_add_bool(PLUGIN_PREF_HOMOGENOUS_BTNS, FALSE);
	
	//loading the currently selected filter
	blistfilter_load_all_filters();
	selected_index = purple_prefs_get_int(PLUGIN_PREF_ACTIVE_FILTER);
	if (selected_index < 0 || selected_index > (int)g_list_length(blistfilter_filters))
	{
		purple_debug_warning(PLUGIN_ID, "Filter #%d was selected, but it does not exist in the prefs. Resetting to 0.\n", selected_index);
		selected_index = 0;
		purple_prefs_set_int(PLUGIN_PREF_ACTIVE_FILTER, selected_index);
	}
	filter = (BListFilterDescription*)g_list_nth_data(blistfilter_filters, selected_index);
	//applying the filter to the buddy list
	blistfilter_update_entire_blist(filter);
	
	//connecting to signals
	//active filter changed - main entry point for filter control
	purple_prefs_connect_callback(plugin, PLUGIN_PREF_ACTIVE_FILTER, blistfilter_active_filter_changed_cb, NULL);
	//gui settings changed - used in conjunction with plugin preference dialog
	purple_prefs_connect_callback(plugin, PLUGIN_PREF_BTN_SPACING, blistfilter_gui_setting_changed_cb, NULL);
	purple_prefs_connect_callback(plugin, PLUGIN_PREF_SELECTOR_STYLE, blistfilter_gui_setting_changed_cb, NULL);
	purple_prefs_connect_callback(plugin, PLUGIN_PREF_FORCE_TITLES, blistfilter_gui_setting_changed_cb, NULL);
	purple_prefs_connect_callback(plugin, PLUGIN_PREF_HOMOGENOUS_BTNS, blistfilter_gui_setting_changed_cb, NULL);
	//blist node addition - in case a new node appears, we want it processed. 
	purple_signal_connect(purple_blist_get_handle(),
		"blist-node-added",
		buddylistfilter_plugin,
		PURPLE_CALLBACK(blistfilter_update_node),
		NULL);
	//when buddy list is created, we will add our GUI. But if we are enabled late, and it's been created already...
	blistfilter_make_filter_selector_gui(); //try and create it immediately. Worst case, the call will silently fail.
	purple_signal_connect(pidgin_blist_get_handle(), "gtkblist-created", plugin, PURPLE_CALLBACK(blistfilter_make_filter_selector_gui), NULL);
	//wehn a conversation gains/loses "has unread messages" status, we need to update our GUI
	purple_signal_connect(purple_conversations_get_handle(), "conversation-updated", plugin, PURPLE_CALLBACK(blistfilter_convo_has_updated), NULL); 
	//Done.
	return TRUE;
}
//this is called when plugin is unloaded, either on Pidgin shutdown or when disabled in Plugins dialog.
static gboolean plugin_unload(PurplePlugin* plugin)
{
	//show the entire buddy list again
	blistfilter_update_entire_blist(NULL);
	//remove filter selector GUI
	blistfilter_destroy_filter_selector_gui();
	//clear out compiled filters
	blistfilter_free_all_filters();
	//Done.
	return TRUE;
}
//This creates plugin settings dialog window
static PurplePluginPrefFrame* get_plugin_pref_frame(PurplePlugin* plugin)
{
	PurplePluginPrefFrame* frame;
	PurplePluginPref* pref;
	
	frame = purple_plugin_pref_frame_new();
	
	pref = purple_plugin_pref_new_with_name_and_label(PLUGIN_PREF_SELECTOR_STYLE, "Filter selector style");
	purple_plugin_pref_set_type(pref, PURPLE_PLUGIN_PREF_CHOICE);
	purple_plugin_pref_add_choice(pref, "Vertical list (top)", GINT_TO_POINTER(FST_VERTICAL_TOP));
	purple_plugin_pref_add_choice(pref, "Vertical list (bottom)", GINT_TO_POINTER(FST_VERTICAL_BOTTOM));
	purple_plugin_pref_add_choice(pref, "Horizontal list (top)", GINT_TO_POINTER(FST_HORIZONTAL_TOP));
	purple_plugin_pref_add_choice(pref, "Horizontal list (bottom)", GINT_TO_POINTER(FST_HORIZONTAL_BOTTOM));
	purple_plugin_pref_frame_add(frame, pref);

	pref = purple_plugin_pref_new_with_name_and_label(PLUGIN_PREF_FORCE_TITLES, "Always show filter names");
	purple_plugin_pref_frame_add(frame, pref);

	pref = purple_plugin_pref_new_with_name_and_label(PLUGIN_PREF_HOMOGENOUS_BTNS, "Keep all buttons same size");
	purple_plugin_pref_frame_add(frame, pref);

	pref = purple_plugin_pref_new_with_name_and_label(PLUGIN_PREF_BTN_SPACING, "Selector button spacing (px)");
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
	"Alex 'Vindicar' Orlov <the.vindicar@gmail.com>", //plugin author
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

//Triggers when the plugin is probed by Pidgin
static void init_plugin (PurplePlugin * plugin)
{
	
}

PURPLE_INIT_PLUGIN (vindicar_buddylistfilter, init_plugin, info)
