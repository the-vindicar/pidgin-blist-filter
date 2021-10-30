
PIDGIN_TREE_TOP ?= ../pidgin-2.10.11
PIDGIN3_TREE_TOP ?= ../pidgin-main
WIN32_DEV_TOP ?= $(PIDGIN_TREE_TOP)/../win32-dev
WIN32_GTK_TOP ?= $(WIN32_DEV_TOP)/gtk_2_0-2.14

WIN32_CC ?= $(WIN32_DEV_TOP)/mingw-4.7.2/bin/gcc
MAKENSIS ?= makensis

PKG_CONFIG ?= pkg-config

CFLAGS	?= -O2 -g -pipe
LDFLAGS ?= 

# Do some nasty OS and purple version detection
ifeq ($(OS),Windows_NT)
  #only defined on 64-bit windows
  PROGFILES32 = ${ProgramFiles(x86)}
  ifndef PROGFILES32
    PROGFILES32 = $(PROGRAMFILES)
  endif
  PLUGIN_TARGET = buddy-list-filter.dll
  PLUGIN_DEST = "$(PROGFILES32)/Pidgin/plugins"
  PLUGIN_ICONS_DEST = "$(PROGFILES32)/Pidgin/pixmaps/pidgin/protocols"
  MAKENSIS = "$(PROGFILES32)/NSIS/makensis.exe"
else

  UNAME_S := $(shell uname -s)

  #.. There are special flags we need for OSX
  ifeq ($(UNAME_S), Darwin)
    #
    #.. /opt/local/include and subdirs are included here to ensure this compiles
    #   for folks using Macports.  I believe Homebrew uses /usr/local/include
    #   so things should "just work".  You *must* make sure your packages are
    #   all up to date or you will most likely get compilation errors.
    #
    INCLUDES = -I/opt/local/include -lz $(OS)

    CC = gcc
  else
    INCLUDES = 
    CC ?= gcc
  endif

  ifeq ($(shell $(PKG_CONFIG) --exists pidgin-3 2>/dev/null && echo "true"),)
    ifeq ($(shell $(PKG_CONFIG) --exists pidgin 2>/dev/null && echo "true"),)
      PLUGIN_TARGET = FAILNOPURPLE
      PLUGIN_DEST =
	  PLUGIN_ICONS_DEST =
    else
      PLUGIN_TARGET = buddy-list-filter.so
      PLUGIN_DEST = $(DESTDIR)`$(PKG_CONFIG) --variable=plugindir pidgin`
	  PLUGIN_ICONS_DEST = $(DESTDIR)`$(PKG_CONFIG) --variable=datadir pidgin`/pixmaps/pidgin/protocols
    endif
  else
    PLUGIN_TARGET = buddy-list-filter.so
    PLUGIN_DEST = $(DESTDIR)`$(PKG_CONFIG) --variable=plugindir pidgin-3`
	PLUGIN_ICONS_DEST = $(DESTDIR)`$(PKG_CONFIG) --variable=datadir pidgin-3`/pixmaps/pidgin/protocols
  endif
endif

WIN32_CFLAGS = -I$(WIN32_GTK_TOP)/include/gtk-2.0 -I$(WIN32_GTK_TOP)/include/pango-1.0 -I$(WIN32_GTK_TOP)/include/atk-1.0 -I$(WIN32_GTK_TOP)/include/cairo -I$(WIN32_GTK_TOP)/lib/gtk-2.0/include -I$(WIN32_DEV_TOP)/glib-2.28.8/include -I$(WIN32_DEV_TOP)/glib-2.28.8/include/glib-2.0 -I$(WIN32_DEV_TOP)/glib-2.28.8/lib/glib-2.0/include -I$(PIDGIN_TREE_TOP)/pidgin -I$(PIDGIN_TREE_TOP)/pidgin/win32 -DENABLE_NLS -DPACKAGE_VERSION='"$(PLUGIN_VERSION)"' -Wall -Wextra -Werror -Wno-deprecated-declarations -Wno-unused-parameter -fno-strict-aliasing -Wformat
WIN32_LDFLAGS = -L$(WIN32_DEV_TOP)/glib-2.28.8/lib -L$(WIN32_GTK_TOP)/lib -L$(WIN32_GTK_TOP)/bin -L$(PIDGIN_TREE_TOP)/pidgin -lpidgin -lpurple -lgtk-win32-2.0-0 -lintl -lglib-2.0 -lgobject-2.0 -g -ggdb -static-libgcc -lz
WIN32_PIDGIN2_CFLAGS = -I$(PIDGIN_TREE_TOP)/libpurple -I$(PIDGIN_TREE_TOP) $(WIN32_CFLAGS)
WIN32_PIDGIN3_CFLAGS = -I$(PIDGIN3_TREE_TOP)/libpurple -I$(PIDGIN3_TREE_TOP) -I$(WIN32_DEV_TOP)/gplugin-dev/gplugin $(WIN32_CFLAGS)
WIN32_PIDGIN2_LDFLAGS = -L$(PIDGIN_TREE_TOP)/libpurple $(WIN32_LDFLAGS)
WIN32_PIDGIN3_LDFLAGS = -L$(PIDGIN3_TREE_TOP)/libpurple -L$(WIN32_DEV_TOP)/gplugin-dev/gplugin $(WIN32_LDFLAGS) -lgplugin

C_FILES := \
	buddy-list-filter.c
	
PURPLE_COMPAT_FILES := 
PURPLE_C_FILES := $(C_FILES)



.PHONY:	all install FAILNOPURPLE clean

all: $(PLUGIN_TARGET)

buddy-list-filter.so: $(PURPLE_C_FILES) $(PURPLE_COMPAT_FILES)
	$(CC) -std=gnu99 -fPIC $(CFLAGS) -shared -o $@ $^ $(LDFLAGS) `$(PKG_CONFIG) pidgin gtk-2.0 --libs --cflags` $(INCLUDES) -g -ggdb

buddy-list-filter.dll: $(PURPLE_C_FILES) $(PURPLE_COMPAT_FILES)
	$(WIN32_CC) -std=gnu99 -shared -o $@ $^ $(WIN32_PIDGIN2_CFLAGS) $(WIN32_PIDGIN2_LDFLAGS) -Ipurple2compat

install: $(PLUGIN_TARGET)
	mkdir -p $(PLUGIN_DEST)
	install -p $(PLUGIN_TARGET) $(PLUGIN_DEST)

FAILNOPURPLE:
	echo "You need libpurple development headers installed to be able to compile this plugin"

clean:
	rm -f $(PLUGIN_TARGET)


