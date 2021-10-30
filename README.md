# pidgin-blist-filter
Pidgin 2.x.y plugin that lets you quickly show/hide groups in your buddy list to keep it small and readable.

If you are using Pidgin 2 to manage your communications, you can end up with a lot of groups in your buddy list - especially if you use protocols like Discord.
This plugin is here to keep things manageable, by creating a set of "views" of your buddy list. Each view has some groups visible and others hidden, depending on your settings, and you can change the view easily.

Additionally, when you have unread messages, the plugin shows the number next to each view, so you can quickly see where do those messages come from - and whether you should pay attention to them right now.

## Filter syntax
Plugin shows/hides groups in the buddy list depending on their names, matching those with a set of glob-like patterns. For example:
 - "Work" will match the group called "Work" (case sensitive!)
 - "~Work" will match any group except "Work"
 - "Work: *" will match groups like "Work: IT" or "Work: HR"
 - "Friends|Family" will match both groups "Friends" and "Family"
 - "Work: *|~Work: IT" will match any group starting with "Work: " unless it's "Work: IT"
 - Empty pattern matches any group - useful if you want to have access to the unfiltered list.
