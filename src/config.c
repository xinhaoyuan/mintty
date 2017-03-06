// config.c (part of mintty)
// Copyright 2008-13 Andy Koppe, 2015-2016 Thomas Wolff
// Based on code from PuTTY-0.60 by Simon Tatham and team.
// Licensed under the terms of the GNU General Public License v3 or later.

// Internationalization approach:
// instead of refactoring a lot of framework functions (here, *ctrls.c)
// to use Unicode strings, the API is simply redefined to use UTF-8;
// non-ASCII strings are converted before being passed to the platform 
// (using UTF-16 on Windows)

#include "term.h"
#include "ctrls.h"
#include "print.h"
#include "charset.h"
#include "win.h"

#include <windows.h>  // registry handling

#include <termios.h>
#include <sys/cygwin.h>


#define dont_support_blurred


static wstring rc_filename = 0;
static char linebuf[444];


// all entries need initialisation in options[] or crash...
const config default_cfg = {
  // Looks
  .fg_colour = 0xBFBFBF,
  .bold_colour = (colour)-1,
  .bg_colour = 0x000000,
  .cursor_colour = 0xBFBFBF,
  .underl_colour = (colour)-1,
  .underl_manual = false,
  .search_fg_colour = 0x000000,
  .search_bg_colour = 0x00DDDD,
  .search_current_colour = 0x0099DD,
  .theme_file = W(""),
  .colour_scheme = "",
  .transparency = 0,
  .blurred = false,
  .opaque_when_focused = false,
  .cursor_type = CUR_LINE,
  .cursor_blinks = true,
  .even_line_highlight_delta = 7,
  // Text
  .font = {.name = W("Lucida Console"), .size = 9, .weight = 400, .isbold = false},
  .font_sample = W(""),
  .show_hidden_fonts = false,
  .font_smoothing = FS_DEFAULT,
  .font_render = FR_UNISCRIBE,
  .bold_as_font = -1,  // -1 means "the opposite of bold_as_colour"
  .bold_as_colour = true,
  .allow_blinking = false,
  .locale = "",
  .charset = "",
  .old_fontmenu = false,
  // Keys
  .backspace_sends_bs = CERASE == '\b',
  .delete_sends_del = false,
  .ctrl_alt_is_altgr = false,
  .clip_shortcuts = true,
  .window_shortcuts = true,
  .switch_shortcuts = true,
  .zoom_shortcuts = true,
  .zoom_font_with_window = true,
  .alt_fn_shortcuts = true,
  .ctrl_shift_shortcuts = false,
  .ctrl_exchange_shift = false,
  .compose_key = 0,
  .key_prtscreen = "",	// VK_SNAPSHOT
  .key_pause = "",	// VK_PAUSE
  .key_break = "",	// VK_CANCEL
  .key_menu = "",	// VK_APPS
  .key_scrlock = "",	// VK_SCROLL
  // Mouse
  .copy_on_select = true,
  .copy_as_rtf = true,
  .clicks_place_cursor = false,
  .middle_click_action = MC_PASTE,
  .right_click_action = RC_MENU,
  .opening_clicks = 1,
  .zoom_mouse = true,
  .clicks_target_app = true,
  .click_target_mod = MDK_SHIFT,
  .hide_mouse = true,
  // Window
  .cols = 80,
  .rows = 24,
  .scrollbar = 1,
  .scrollback_lines = 10000,
  .scroll_mod = MDK_SHIFT,
  .pgupdn_scroll = false,
  .lang = W(""),
  .search_bar = "",
  // Terminal
  .term = "xterm",
  .answerback = W(""),
  .old_wrapmodes = false,
  .bell_sound = true,
  .bell_type = 1,
  .bell_file = W(""),
  .bell_freq = 0,
  .bell_len = 400,
  .bell_flash = false,  // xterm: visualBell
  .bell_taskbar = true, // xterm: bellIsUrgent
  .bell_popup = false,  // xterm: popOnBell
  .printer = W(""),
  .confirm_exit = true,
  .allow_set_selection = false,
  // Command line
  .class = W(""),
  .hold = HOLD_START,
  .exit_write = false,
  .exit_title = W(""),
  .icon = W(""),
  .log = W(""),
  .logging = true,
  .create_utmp = false,
  .title = W(""),
  .daemonize = true,
  .daemonize_always = false,
  // "Hidden"
  .app_id = W(""),
  .app_name = W(""),
  .app_launch_cmd = W(""),
  .drop_commands = W(""),
  .col_spacing = 0,
  .row_spacing = 0,
  .padding = 1,
  .handle_dpichanged = true,
  .word_chars = "",
  .word_chars_excl = "",
  .use_system_colours = false,
  .ime_cursor_colour = DEFAULT_COLOUR,
  .ansi_colours = {
    [BLACK_I]        = 0x000000,
    [RED_I]          = 0x0000BF,
    [GREEN_I]        = 0x00BF00,
    [YELLOW_I]       = 0x00BFBF,
    [BLUE_I]         = 0xBF0000,
    [MAGENTA_I]      = 0xBF00BF,
    [CYAN_I]         = 0xBFBF00,
    [WHITE_I]        = 0xBFBFBF,
    [BOLD_BLACK_I]   = 0x404040,
    [BOLD_RED_I]     = 0x4040FF,
    [BOLD_GREEN_I]   = 0x40FF40,
    [BOLD_YELLOW_I]  = 0x40FFFF,
    [BOLD_BLUE_I]    = 0xFF6060,
    [BOLD_MAGENTA_I] = 0xFF40FF,
    [BOLD_CYAN_I]    = 0xFFFF40,
    [BOLD_WHITE_I]   = 0xFFFFFF,
  },
  .sixel_clip_char = W(" ")
};

config cfg, new_cfg, file_cfg;

typedef enum {
  OPT_BOOL, OPT_MOD, OPT_TRANS, OPT_CURSOR, OPT_FONTSMOOTH, OPT_FONTRENDER,
  OPT_MIDDLECLICK, OPT_RIGHTCLICK, OPT_SCROLLBAR, OPT_WINDOW, OPT_HOLD,
  OPT_INT, OPT_COLOUR, OPT_STRING, OPT_WSTRING,
  OPT_LEGACY = 16
} opt_type;

#define offcfg(option) offsetof(config, option)

static const struct {
  string name;
  uchar type;
  ushort offset;
}
options[] = {
  // Colour base options;
  // check_legacy_options() assumes these are the first three here:
  {"ForegroundColour", OPT_COLOUR, offcfg(fg_colour)},
  {"BackgroundColour", OPT_COLOUR, offcfg(bg_colour)},
  {"UseSystemColours", OPT_BOOL | OPT_LEGACY, offcfg(use_system_colours)},

  // Looks
  {"BoldColour", OPT_COLOUR, offcfg(bold_colour)},
  {"CursorColour", OPT_COLOUR, offcfg(cursor_colour)},
  {"UnderlineColour", OPT_COLOUR, offcfg(underl_colour)},
  {"UnderlineManual", OPT_BOOL, offcfg(underl_manual)},
  {"SearchForegroundColour", OPT_COLOUR, offcfg(search_fg_colour)},
  {"SearchBackgroundColour", OPT_COLOUR, offcfg(search_bg_colour)},
  {"SearchCurrentColour", OPT_COLOUR, offcfg(search_current_colour)},
  {"ThemeFile", OPT_WSTRING, offcfg(theme_file)},
  {"ColourScheme", OPT_STRING, offcfg(colour_scheme)},
  {"Transparency", OPT_TRANS, offcfg(transparency)},
#ifdef support_blurred
  {"Blur", OPT_BOOL, offcfg(blurred)},
#endif
  {"OpaqueWhenFocused", OPT_BOOL, offcfg(opaque_when_focused)},
  {"CursorType", OPT_CURSOR, offcfg(cursor_type)},
  {"CursorBlinks", OPT_BOOL, offcfg(cursor_blinks)},
  {"EvenLineHighlightDelta", OPT_INT, offcfg(even_line_highlight_delta)},

  // Text
  {"Font", OPT_WSTRING, offcfg(font.name)},
  {"FontSample", OPT_WSTRING, offcfg(font_sample)},
  {"FontSize", OPT_INT | OPT_LEGACY, offcfg(font.size)},
  {"FontHeight", OPT_INT, offcfg(font.size)},
  {"FontWeight", OPT_INT, offcfg(font.weight)},
  {"FontIsBold", OPT_BOOL, offcfg(font.isbold)},
  {"ShowHiddenFonts", OPT_BOOL, offcfg(show_hidden_fonts)},
  {"FontSmoothing", OPT_FONTSMOOTH, offcfg(font_smoothing)},
  {"BoldAsFont", OPT_BOOL, offcfg(bold_as_font)},
  {"BoldAsColour", OPT_BOOL, offcfg(bold_as_colour)},
  {"AllowBlinking", OPT_BOOL, offcfg(allow_blinking)},
  {"Locale", OPT_STRING, offcfg(locale)},
  {"Charset", OPT_STRING, offcfg(charset)},
  {"FontRender", OPT_FONTRENDER, offcfg(font_render)},
  {"OldFontMenu", OPT_BOOL, offcfg(old_fontmenu)},

  // Keys
  {"BackspaceSendsBS", OPT_BOOL, offcfg(backspace_sends_bs)},
  {"DeleteSendsDEL", OPT_BOOL, offcfg(delete_sends_del)},
  {"CtrlAltIsAltGr", OPT_BOOL, offcfg(ctrl_alt_is_altgr)},
  {"ClipShortcuts", OPT_BOOL, offcfg(clip_shortcuts)},
  {"WindowShortcuts", OPT_BOOL, offcfg(window_shortcuts)},
  {"SwitchShortcuts", OPT_BOOL, offcfg(switch_shortcuts)},
  {"ZoomShortcuts", OPT_BOOL, offcfg(zoom_shortcuts)},
  {"ZoomFontWithWindow", OPT_BOOL, offcfg(zoom_font_with_window)},
  {"AltFnShortcuts", OPT_BOOL, offcfg(alt_fn_shortcuts)},
  {"CtrlShiftShortcuts", OPT_BOOL, offcfg(ctrl_shift_shortcuts)},
  {"CtrlExchangeShift", OPT_BOOL, offcfg(ctrl_exchange_shift)},
  {"ComposeKey", OPT_MOD, offcfg(compose_key)},
  {"Key_PrintScreen", OPT_STRING, offcfg(key_prtscreen)},
  {"Key_Pause", OPT_STRING, offcfg(key_pause)},
  {"Key_Break", OPT_STRING, offcfg(key_break)},
  {"Key_Menu", OPT_STRING, offcfg(key_menu)},
  {"Key_ScrollLock", OPT_STRING, offcfg(key_scrlock)},
  {"Break", OPT_STRING | OPT_LEGACY, offcfg(key_break)},
  {"Pause", OPT_STRING | OPT_LEGACY, offcfg(key_pause)},

  // Mouse
  {"CopyOnSelect", OPT_BOOL, offcfg(copy_on_select)},
  {"CopyAsRTF", OPT_BOOL, offcfg(copy_as_rtf)},
  {"ClicksPlaceCursor", OPT_BOOL, offcfg(clicks_place_cursor)},
  {"MiddleClickAction", OPT_MIDDLECLICK, offcfg(middle_click_action)},
  {"RightClickAction", OPT_RIGHTCLICK, offcfg(right_click_action)},
  {"OpeningClicks", OPT_INT, offcfg(opening_clicks)},
  {"ZoomMouse", OPT_BOOL, offcfg(zoom_mouse)},
  {"ClicksTargetApp", OPT_BOOL, offcfg(clicks_target_app)},
  {"ClickTargetMod", OPT_MOD, offcfg(click_target_mod)},
  {"HideMouse", OPT_BOOL, offcfg(hide_mouse)},

  // Window
  {"Columns", OPT_INT, offcfg(cols)},
  {"Rows", OPT_INT, offcfg(rows)},
  {"ScrollbackLines", OPT_INT, offcfg(scrollback_lines)},
  {"Scrollbar", OPT_SCROLLBAR, offcfg(scrollbar)},
  {"ScrollMod", OPT_MOD, offcfg(scroll_mod)},
  {"PgUpDnScroll", OPT_BOOL, offcfg(pgupdn_scroll)},
  {"Language", OPT_WSTRING, offcfg(lang)},
  {"SearchBar", OPT_STRING, offcfg(search_bar)},

  // Terminal
  {"Term", OPT_STRING, offcfg(term)},
  {"Answerback", OPT_WSTRING, offcfg(answerback)},
  {"OldWrapModes", OPT_BOOL, offcfg(old_wrapmodes)},
  {"BellSound", OPT_BOOL, offcfg(bell_sound)},
  {"BellType", OPT_INT, offcfg(bell_type)},
  {"BellFile", OPT_WSTRING, offcfg(bell_file)},
  {"BellFreq", OPT_INT, offcfg(bell_freq)},
  {"BellLen", OPT_INT, offcfg(bell_len)},
  {"BellFlash", OPT_BOOL, offcfg(bell_flash)},
  {"BellTaskbar", OPT_BOOL, offcfg(bell_taskbar)},
  {"BellPopup", OPT_BOOL, offcfg(bell_popup)},
  {"Printer", OPT_WSTRING, offcfg(printer)},
  {"ConfirmExit", OPT_BOOL, offcfg(confirm_exit)},
  {"AllowSetSelection", OPT_BOOL, offcfg(allow_set_selection)},

  // Command line
  {"Class", OPT_WSTRING, offcfg(class)},
  {"Hold", OPT_HOLD, offcfg(hold)},
  {"Daemonize", OPT_BOOL, offcfg(daemonize)},
  {"DaemonizeAlways", OPT_BOOL, offcfg(daemonize_always)},
  {"ExitWrite", OPT_BOOL, offcfg(exit_write)},
  {"ExitTitle", OPT_WSTRING, offcfg(exit_title)},
  {"Icon", OPT_WSTRING, offcfg(icon)},
  {"Log", OPT_WSTRING, offcfg(log)},
  {"Logging", OPT_BOOL, offcfg(logging)},
  {"Title", OPT_WSTRING, offcfg(title)},
  {"Utmp", OPT_BOOL, offcfg(create_utmp)},
  {"Window", OPT_WINDOW, offcfg(window)},
  {"X", OPT_INT, offcfg(x)},
  {"Y", OPT_INT, offcfg(y)},

  // "Hidden"
  {"AppID", OPT_WSTRING, offcfg(app_id)},
  {"AppName", OPT_WSTRING, offcfg(app_name)},
  {"AppLaunchCmd", OPT_WSTRING, offcfg(app_launch_cmd)},
  {"DropCommands", OPT_WSTRING, offcfg(drop_commands)},
  {"ColSpacing", OPT_INT, offcfg(col_spacing)},
  {"RowSpacing", OPT_INT, offcfg(row_spacing)},
  {"Padding", OPT_INT, offcfg(padding)},
  {"HandleDPI", OPT_BOOL, offcfg(handle_dpichanged)},
  {"WordChars", OPT_STRING, offcfg(word_chars)},
  {"WordCharsExcl", OPT_STRING, offcfg(word_chars_excl)},
  {"IMECursorColour", OPT_COLOUR, offcfg(ime_cursor_colour)},
  {"SixelClipChars", OPT_WSTRING, offcfg(sixel_clip_char)},

  // ANSI colours
  {"Black", OPT_COLOUR, offcfg(ansi_colours[BLACK_I])},
  {"Red", OPT_COLOUR, offcfg(ansi_colours[RED_I])},
  {"Green", OPT_COLOUR, offcfg(ansi_colours[GREEN_I])},
  {"Yellow", OPT_COLOUR, offcfg(ansi_colours[YELLOW_I])},
  {"Blue", OPT_COLOUR, offcfg(ansi_colours[BLUE_I])},
  {"Magenta", OPT_COLOUR, offcfg(ansi_colours[MAGENTA_I])},
  {"Cyan", OPT_COLOUR, offcfg(ansi_colours[CYAN_I])},
  {"White", OPT_COLOUR, offcfg(ansi_colours[WHITE_I])},
  {"BoldBlack", OPT_COLOUR, offcfg(ansi_colours[BOLD_BLACK_I])},
  {"BoldRed", OPT_COLOUR, offcfg(ansi_colours[BOLD_RED_I])},
  {"BoldGreen", OPT_COLOUR, offcfg(ansi_colours[BOLD_GREEN_I])},
  {"BoldYellow", OPT_COLOUR, offcfg(ansi_colours[BOLD_YELLOW_I])},
  {"BoldBlue", OPT_COLOUR, offcfg(ansi_colours[BOLD_BLUE_I])},
  {"BoldMagenta", OPT_COLOUR, offcfg(ansi_colours[BOLD_MAGENTA_I])},
  {"BoldCyan", OPT_COLOUR, offcfg(ansi_colours[BOLD_CYAN_I])},
  {"BoldWhite", OPT_COLOUR, offcfg(ansi_colours[BOLD_WHITE_I])},

  // Legacy
  {"BoldAsBright", OPT_BOOL | OPT_LEGACY, offcfg(bold_as_colour)},
  {"FontQuality", OPT_FONTSMOOTH | OPT_LEGACY, offcfg(font_smoothing)},
};

typedef const struct {
  string name;
  char val;
} opt_val;

static opt_val
*const opt_vals[] = {
  [OPT_BOOL] = (opt_val[]) {
    {"no", false},
    {"yes", true},
    {"false", false},
    {"true", true},
    {"off", false},
    {"on", true},
    {0, 0}
  },
  [OPT_MOD] = (opt_val[]) {
    {"off", 0},
    {"shift", MDK_SHIFT},
    {"alt", MDK_ALT},
    {"ctrl", MDK_CTRL},
    {0, 0}
  },
  [OPT_TRANS] = (opt_val[]) {
    {"off", TR_OFF},
    {"low", TR_LOW},
    {"medium", TR_MEDIUM},
    {"high", TR_HIGH},
    {"glass", TR_GLASS},
    {0, 0}
  },
  [OPT_CURSOR] = (opt_val[]) {
    {"line", CUR_LINE},
    {"block", CUR_BLOCK},
    {"underscore", CUR_UNDERSCORE},
    {0, 0}
  },
  [OPT_FONTSMOOTH] = (opt_val[]) {
    {"default", FS_DEFAULT},
    {"none", FS_NONE},
    {"partial", FS_PARTIAL},
    {"full", FS_FULL},
    {0, 0}
  },
  [OPT_FONTRENDER] = (opt_val[]) {
    {"textout", FR_TEXTOUT},
    {"uniscribe", FR_UNISCRIBE},
    {0, 0}
  },
  [OPT_MIDDLECLICK] = (opt_val[]) {
    {"enter", MC_ENTER},
    {"paste", MC_PASTE},
    {"extend", MC_EXTEND},
    {"void", MC_VOID},
    {0, 0}
  },
  [OPT_RIGHTCLICK] = (opt_val[]) {
    {"enter", RC_ENTER},
    {"paste", RC_PASTE},
    {"extend", RC_EXTEND},
    {"menu", RC_MENU},
    {0, 0}
  },
  [OPT_SCROLLBAR] = (opt_val[]) {
    {"left", -1},
    {"right", 1},
    {"none", 0},
    {0, 0}
  },
  [OPT_WINDOW] = (opt_val[]){
    {"hide", 0},   // SW_HIDE
    {"normal", 1}, // SW_SHOWNORMAL
    {"min", 2},    // SW_SHOWMINIMIZED
    {"max", 3},    // SW_SHOWMAXIMIZED
    {"full", -1},
    {0, 0}
  },
  [OPT_HOLD] = (opt_val[]) {
    {"never", HOLD_NEVER},
    {"start", HOLD_START},
    {"error", HOLD_ERROR},
    {"always", HOLD_ALWAYS},
    {0, 0}
  }
};


#ifdef debug_theme
#define trace_theme(params)	printf params
#else
#define trace_theme(params)
#endif


#define dont_debug_opterror

static void
opterror(string msg, bool utf8params, string p1, string p2)
{
  print_opterror(stderr, msg, utf8params, p1, p2);
}


static int
find_option(bool from_file, string name)
{
  for (uint i = 0; i < lengthof(options); i++) {
    if (!strcasecmp(name, options[i].name))
      return i;
  }
  //__ %s: unknown option name
  opterror(_("Ignoring unknown option '%s'"), from_file, name, 0);
  return -1;
}

#define MAX_COMMENTS (lengthof(options) * 3)
static struct {
  char * comment;
  uchar opti;
} file_opts[lengthof(options) + MAX_COMMENTS];
static uchar arg_opts[lengthof(options)];
static uint file_opts_num = 0;
static uint arg_opts_num;

static void
clear_opts(void)
{
  for (uint n = 0; n < file_opts_num; n++)
    if (file_opts[n].comment)
      free(file_opts[n].comment);
  file_opts_num = 0;
  arg_opts_num = 0;
}

static bool
seen_file_option(uint i)
{
//  return memchr(file_opts, i, file_opts_num);
  for (uint n = 0; n < file_opts_num; n++)
    if (!file_opts[n].comment && file_opts[n].opti == i)
      return true;
  return false;
}

static bool
seen_arg_option(uint i)
{
  return memchr(arg_opts, i, arg_opts_num);
}

static void
remember_file_option(char * tag, uint i)
{
  (void)tag;
  trace_theme(("[%s] remember_file_option (file %d arg %d) %d %s\n", tag, seen_file_option(i), seen_arg_option(i), i, options[i].name));
  if (file_opts_num >= lengthof(file_opts)) {
    opterror(_("Internal error: too many options"), true, 0, 0);
    exit(1);
  }

  if (!seen_file_option(i)) {
    file_opts[file_opts_num].comment = null;
    file_opts[file_opts_num].opti = i;
    file_opts_num++;
  }
}

static void
remember_file_comment(char * comment)
{
  trace_theme(("[] remember_file_comment <%s>\n", comment));
  if (file_opts_num >= lengthof(file_opts)) {
    opterror(_("Internal error: too many options/comments"), true, 0, 0);
    exit(1);
  }

  file_opts[file_opts_num++].comment = strdup(comment);
}

static void
remember_arg_option(char * tag, uint i)
{
  (void)tag;
  trace_theme(("[%s] remember_arg_option (file %d arg %d) %d %s\n", tag, seen_file_option(i), seen_arg_option(i), i, options[i].name));
  if (arg_opts_num >= lengthof(arg_opts)) {
    opterror(_("Internal error: too many options"), false, 0, 0);
    exit(1);
  }

  if (!seen_arg_option(i))
    arg_opts[arg_opts_num++] = i;
}

static void
check_legacy_options(void (*remember_option)(char * tag, uint))
{
  if (cfg.use_system_colours) {
    // Translate 'UseSystemColours' to colour settings.
    cfg.fg_colour = cfg.cursor_colour = win_get_sys_colour(true);
    cfg.bg_colour = win_get_sys_colour(false);
    cfg.use_system_colours = false;

    // Make sure they're written to the config file.
    // This assumes that the colour options are the first three in options[].
    remember_option("legacy", 0);
    remember_option("legacy", 1);
    remember_option("legacy", 2);
  }
}

static struct {
  uchar r, g, b;
  char * name;
} xcolours[] = {
#include "rgb.t"
};

bool
parse_colour(string s, colour *cp)
{
  uint r, g, b;
  if (sscanf(s, "%u,%u,%u%c", &r, &g, &b, &(char){0}) == 3)
    ;
  else if (sscanf(s, "#%2x%2x%2x%c", &r, &g, &b, &(char){0}) == 3)
    ;
  else if (sscanf(s, "rgb:%2x/%2x/%2x%c", &r, &g, &b, &(char){0}) == 3)
    ;
  else if (sscanf(s, "rgb:%4x/%4x/%4x%c", &r, &g, &b, &(char){0}) == 3)
    r >>=8, g >>= 8, b >>= 8;
  else {
    int coli = -1;
    for (uint i = 0; i < lengthof(xcolours); i++)
      if (!strcasecmp(s, xcolours[i].name)) {
        r = xcolours[i].r;
        g = xcolours[i].g;
        b = xcolours[i].b;
        coli = i;
        break;
      }
    if (coli < 0)
      return false;
  }

  *cp = make_colour(r, g, b);
  return true;
}

static int
set_option(string name, string val_str, bool from_file)
{
  int i = find_option(from_file, name);
  if (i < 0)
    return i;

  void *val_p = (void *)&cfg + options[i].offset;
  uint type = options[i].type & ~OPT_LEGACY;

  switch (type) {
    when OPT_STRING:
      strset(val_p, val_str);
      return i;
    when OPT_WSTRING: {
      wchar * ws;
      if (from_file)
        ws = cs__utforansitowcs(val_str);
      else
        ws = cs__mbstowcs(val_str);
      wstrset(val_p, ws);
      free(ws);
      return i;
    }
    when OPT_INT: {
      char *val_end;
      int val = strtol(val_str, &val_end, 0);
      if (val_end != val_str) {
        *(int *)val_p = val;
        return i;
      }
    }
    when OPT_COLOUR:
      if (parse_colour(val_str, val_p))
        return i;
    otherwise: {
      int len = strlen(val_str);
      if (!len)
        break;
      for (opt_val *o = opt_vals[type]; o->name; o++) {
        if (!strncasecmp(val_str, o->name, len)) {
          *(char *)val_p = o->val;
          return i;
        }
      }
      // Value not found: try interpreting it as a number.
      char *val_end;
      int val = strtol(val_str, &val_end, 0);
      if (val_end != val_str) {
        *(char *)val_p = val;
        return i;
      }
    }
  }
  //__ %2$s: option name, %1$s: invalid value
  opterror(_("Ignoring invalid value '%s' for option '%s'"), 
           from_file, val_str, name);
  return -1;
}

static int
parse_option(string option, bool from_file)
{
  const char *eq = strchr(option, '=');
  if (!eq) {
    //__ %s: option name
    opterror(_("Ignoring option '%s' with missing value"), 
             from_file, option, 0);
    return -1;
  }

  const char *name_end = eq;
  while (isspace((uchar)name_end[-1]))
    name_end--;

  uint name_len = name_end - option;
  char name[name_len + 1];
  memcpy(name, option, name_len);
  name[name_len] = 0;

  const char *val = eq + 1;
  while (isspace((uchar)*val))
    val++;

  return set_option(name, val, from_file);
}

static void
check_arg_option(int i)
{
  if (i >= 0) {
    remember_arg_option("chk_arg", i);
    check_legacy_options(remember_arg_option);
  }
}

void
set_arg_option(string name, string val)
{
  check_arg_option(set_option(name, val, false));
}

void
parse_arg_option(string option)
{
  check_arg_option(parse_option(option, false));
}


#include "winpriv.h"  // home

static char * * config_dirs = 0;
static int last_config_dir = -1;

static void
init_config_dirs(void)
{
  if (config_dirs)
    return;

  char * appdata = getenv("APPDATA");
  if (appdata) {
    config_dirs = newn(char *, 4);
    appdata = newn(char, strlen(appdata) + 8);
    sprintf(appdata, "%s/mintty", getenv("APPDATA"));
  }
  else
    config_dirs = newn(char *, 3);

  // /usr/share/mintty , $APPDATA/mintty , ~/.config/mintty , ~/.mintty
  config_dirs[++last_config_dir] = "/usr/share/mintty";
  if (appdata)
    config_dirs[++last_config_dir] = appdata;
  char * xdgconf = newn(char, strlen(home) + 16);
  sprintf(xdgconf, "%s/.config/mintty", home);
  config_dirs[++last_config_dir] = xdgconf;
  char * homeconf = newn(char, strlen(home) + 9);
  sprintf(homeconf, "%s/.mintty", home);
  config_dirs[++last_config_dir] = homeconf;
}

#include <fcntl.h>

char *
get_resource_file(wstring sub, wstring res, bool towrite)
{
  init_config_dirs();
  int fd;
  for (int i = last_config_dir; i >= 0; i--) {
    wchar * rf = path_posix_to_win_w(config_dirs[i]);
    int len = wcslen(rf);
    rf = renewn(rf, len + wcslen(sub) + wcslen(res) + 3);
    rf[len++] = L'/';
    wcscpy(&rf[len], sub);
    len += wcslen(sub);
    rf[len++] = L'/';
    wcscpy(&rf[len], res);

    char * resfn = path_win_w_to_posix(rf);
    free(rf);
    fd = open(resfn, towrite ? O_CREAT | O_EXCL | O_WRONLY | O_BINARY : O_RDONLY | O_BINARY, 0644);
    if (fd >= 0) {
      close(fd);
      return resfn;
    }
    free(resfn);
    if (errno == EACCES || errno == EEXIST)
      break;
  }
  return null;
}


#define dont_debug_messages 1

static struct {
  char * msg;
  char * locmsg;
  wchar * wmsg;
} * messages = 0;
int nmessages = 0;
int maxmessages = 0;

static void
clear_messages()
{
  for (int i = 0; i < nmessages; i++) {
    free(messages[i].msg);
    free(messages[i].locmsg);
    if (messages[i].wmsg)
      free(messages[i].wmsg);
  }
  nmessages = 0;
}

static void
add_message(char * msg, char * locmsg)
{
  if (nmessages >= maxmessages) {
    if (maxmessages)
      maxmessages += 20;
    else
      maxmessages = 180;
    messages = renewn(messages, maxmessages);
  }
#if defined(debug_messages) && debug_messages > 3
  printf("add %d <%s> <%s>\n", nmessages, msg, locmsg);
#endif
  messages[nmessages].msg = msg;
  messages[nmessages].locmsg = locmsg;
  messages[nmessages].wmsg = null;
  nmessages ++;
}

char * loctext(string msg)
{
  for (int i = 0; i < nmessages; i++) {
#if defined(debug_messages) && debug_messages > 5
    printf("?<%s> %d <%s> -> <%s>\n", msg, i, messages[i].msg, messages[i].locmsg);
#endif
    if (strcmp(msg, messages[i].msg) == 0) {
#if defined(debug_messages) && debug_messages > 4
      printf("!<%s> %d <%s> -> <%s>\n", msg, i, messages[i].msg, messages[i].locmsg);
#endif
      return messages[i].locmsg;
    }
  }
  return (char *) msg;
}

wchar * wloctext(string msg)
{
  for (int i = 0; i < nmessages; i++) {
#if defined(debug_messages) && debug_messages > 5
    printf("?<%s> %d <%s> -> <%s> <%ls>\n", msg, i, messages[i].msg, messages[i].locmsg, messages[i].wmsg);
#endif
    if (strcmp(msg, messages[i].msg) == 0) {
#if defined(debug_messages) && debug_messages > 4
      printf("!<%s> %d <%s> -> <%s> <%ls>\n", msg, i, messages[i].msg, messages[i].locmsg, messages[i].wmsg);
#endif
      if (messages[i].wmsg == null)
        messages[i].wmsg = cs__utftowcs(messages[i].locmsg);
      return messages[i].wmsg;
    }
  }
  add_message(strdup(msg), strdup(msg));
  return wloctext(msg);
}

static char *
readtext(char * buf, int len, FILE * file)
{
  char * unescape(char * s)
  {
    char * t = s;
    while (*s && *s != '"') {
      if (*s == '\\') {
        s++;
        switch (*s) {
          when 't': *t = '\t';
          when 'n': *t = '\n';
          otherwise: *t = *s;
        }
      }
      else
        *t = *s;
      t++;
      s++;
    }
    *t = '\0';
    return t;
  }

  char * p = buf;
  while (*p != ' ')
    p++;
  while (*p == ' ')
    p++;
  if (strncmp(p, "\"\"", 2) == 0) {
    // scan multi-line text
    char * str = newn(char, 1);
    *str = '\0';
    while (fgets(buf, len, file) && *buf == '"') {
      p = buf + 1;
      char * f = unescape(p);
      if (!*f) {
        str = renewn(str, strlen(str) + strlen(p) + 1);
        strcat(str, p);
      }
      else {
        free(str);
        return null;
      }
    }
    return str;
  }
  else {
    // scan single-line text
    p++;
    char * f = unescape(p);
    if (!*f) {
      char * str = strdup(p);
      fgets(buf, len, file);
      return str;
    }
  }
  return null;
}

static void
load_messages_file(char * textdbf)
{
  FILE * file = fopen(textdbf, "r");
  if (file) {
    clear_messages();

    while (fgets(linebuf, sizeof linebuf, file)) {
      linebuf[strcspn(linebuf, "\r\n")] = 0;  /* trim newline */
      if (strncmp(linebuf, "msgid ", 6) == 0) {
        char * msg = readtext(linebuf, sizeof linebuf, file);
        if (strncmp(linebuf, "msgstr ", 7) == 0) {
          char * locmsg = readtext(linebuf, sizeof linebuf, file);
          if (msg && *msg && locmsg && *locmsg)
            add_message(msg, locmsg);
        }
      }
      else {
      }
    }
    fclose(file);
  }
#ifdef debug_messages
  printf("read %d messages\n", nmessages);
#if debug_messages > 2
  printf("msg blö -> <%s> <%ls>\n", loctext("blö"), wloctext("blö"));
  printf("msg blö -> <%s> <%ls>\n", loctext("blö"), wloctext("blö"));
#endif
#endif
}

static bool
load_messages_lang_w(wstring lang, bool fallback)
{
  if (lang) {
    wchar * wl = newn(wchar, wcslen(lang) + 4);
    wcscpy(wl, lang);
    if (fallback) {
      wchar * _ = wcschr(wl, '_');
      if (_) {
        *_ = '\0';
        // continue below
      }
      else
        return false;
    }
    wcscat(wl, W(".po"));
    char * textdbf = get_resource_file(W("lang"), wl, false);
    free(wl);
#ifdef debug_messages
    printf("Trying to load messages from <%ls>: <%s>\n", lang, textdbf);
#endif
    if (textdbf) {
      load_messages_file(textdbf);
      free(textdbf);
      return true;
    }
  }
  return false;
}

static bool
load_messages_lang(string lang, bool fallback)
{
  wchar * wlang = cs__utftowcs(lang);
  bool res = load_messages_lang_w((wstring)wlang, fallback);
  free(wlang);
  return res;
}

static void
load_messages(config * cfg_p)
{
  if (cfg_p->lang) for (int fallback = false; fallback <= true; fallback++) {
#ifdef debug_messages
    printf("Loading localization <%ls> (fallback %d)\n", cfg_p->lang, fallback);
#endif
    clear_messages();
    if (wcscmp(cfg_p->lang, W("=")) == 0) {
      if (load_messages_lang(cfg_p->locale, fallback))
        return;
    }
    else if (wcscmp(cfg_p->lang, W("@")) == 0) {
      // locale_menu[1] is transformed from GetUserDefaultUILanguage()
      if (load_messages_lang(locale_menu[1], fallback))
        return;
    }
    else if (wcscmp(cfg_p->lang, W("*")) == 0) {
      // determine UI language from environment
      char * lang = getenv("LANGUAGE");
      if (lang) {
        lang = strdup(lang);
        while (lang && *lang) {
          char * sep = strchr(lang, ':');
          if (sep)
            *sep = '\0';
          if (load_messages_lang(lang, fallback))
            return;
          lang = sep;
          if (lang)
            lang++;
        }
        free(lang);
      }
      lang = (char *)getlocenvcat("LC_MESSAGES");
      if (lang && *lang) {
        lang = strdup(lang);
        char * dot = strchr(lang, '.');
        if (dot)
          *dot = '\0';
        if (load_messages_lang(lang, fallback))
          return;
        free(lang);
      }
    }
    else {
      if (load_messages_lang_w(cfg_p->lang, fallback))
        return;
    }
  }
}


void
load_theme(wstring theme)
{
  if (*theme) {
    if (wcschr(theme, L'/') || wcschr(theme, L'\\')) {
      char * thf = path_win_w_to_posix(theme);
      load_config(thf, false);
      free(thf);
    }
    else {
      char * thf = get_resource_file(W("themes"), theme, false);
      if (thf) {
        load_config(thf, false);
        free(thf);
      }
    }
  }
}

void
load_scheme(string cs)
{
  copy_config("scheme", &cfg, &file_cfg);

  // analyse scheme description
  char * scheme = strdup(cs);
  char * sch = scheme;
  char * param = scheme;
  char * value = null;
  while (*sch) {
    if (*sch == '=') {
      *sch++ = '\0';
      value = sch;
    }
    else if (*sch == ';') {
      *sch++ = '\0';
      if (value) {
        set_option(param, value, false);
      }
      param = sch;
      value = null;
    }
    else
      sch++;
  }
  free(scheme);
}

void
load_config(string filename, bool to_save)
{
  trace_theme(("load_config <%s> %d\n", filename, to_save));
  if (!to_save) {
    // restore base configuration, without theme mix-ins
    copy_config("load", &cfg, &file_cfg);
  }

  if (access(filename, R_OK) == 0 && access(filename, W_OK) < 0)
    to_save = false;

  FILE * file = fopen(filename, "r");

  if (to_save) {
    if (file || (!rc_filename && strstr(filename, "/.minttyrc"))) {
      clear_opts();

      delete(rc_filename);
      rc_filename = path_posix_to_win_w(filename);
    }
  }

  if (file) {
    while (fgets(linebuf, sizeof linebuf, file)) {
      char * lbuf = linebuf;
      while (!strchr(lbuf, '\n')) {
        if (lbuf == linebuf) {
          // make lbuf dynamic
          lbuf = strdup(lbuf);
        }
        // append to lbuf
        int len = strlen(lbuf);
        lbuf = renewn(lbuf, len + sizeof linebuf);
        if (!fgets(&lbuf[len], sizeof linebuf, file))
          break;
      }

      //lbuf[strcspn(lbuf, "\r\n")] = 0;  /* trim newline */
      // trim newline but allow embedded CR (esp. for DropCommands)
      lbuf[strcspn(lbuf, "\n")] = 0;
      // preserve comment lines and empty lines
      if (lbuf[0] == '#' || lbuf[0] == '\0') {
        if (to_save)
          remember_file_comment(lbuf);
      }
      else {
        int i = parse_option(lbuf, true);
        if (to_save) {
          if (i >= 0)
            remember_file_option("load", i);
          else
            // preserve unknown options as comment lines
            remember_file_comment(lbuf);
        }
      }
      if (lbuf != linebuf)
        free(lbuf);
    }
    fclose(file);
  }

  check_legacy_options(remember_file_option);

  if (to_save) {
    copy_config("after load", &file_cfg, &cfg);
  }
}

void
copy_config(char * tag, config * dst_p, const config * src_p)
{
#ifdef debug_theme
  char * cfg(config * p) {
    return p == new_cfg ? "new" : p == file_cfg ? "file" : p == cfg ? "cfg" : "?";
  }
  printf("[%s] copy_config %s <- %s\n", tag, cfg(dst_p), cfg(src_p));
#else
  (void)tag;
#endif
  for (uint i = 0; i < lengthof(options); i++) {
    opt_type type = options[i].type;
    if (!(type & OPT_LEGACY)) {
      uint offset = options[i].offset;
      void *dst_val_p = (void *)dst_p + offset;
      void *src_val_p = (void *)src_p + offset;
      switch (type) {
        when OPT_STRING:
          strset(dst_val_p, *(string *)src_val_p);
        when OPT_WSTRING:
          wstrset(dst_val_p, *(wstring *)src_val_p);
        when OPT_INT or OPT_COLOUR:
          *(int *)dst_val_p = *(int *)src_val_p;
        otherwise:
          *(char *)dst_val_p = *(char *)src_val_p;
      }
    }
  }
}

void
init_config(void)
{
  copy_config("init", &cfg, &default_cfg);
}

void
finish_config(void)
{
  if (*cfg.lang && (wcscmp(cfg.lang, W("=")) != 0 || *cfg.locale))
    load_messages(&cfg);
#if defined(debug_messages) && debug_messages > 1
  else
    (void)load_messages_lang("messages");
#endif
#ifdef debug_opterror
  opterror("Täst L %s %s", false, "b�h", "b�h�");
  opterror("Täst U %s %s", true, "böh", "büh€");
#endif

  // Avoid negative sizes.
  cfg.rows = max(1, cfg.rows);
  cfg.cols = max(1, cfg.cols);
  cfg.scrollback_lines = max(0, cfg.scrollback_lines);

  // Ignore charset setting if we haven't got a locale.
  if (!*cfg.locale)
    strset(&cfg.charset, "");

  // bold_as_font used to be implied by !bold_as_colour.
  if (cfg.bold_as_font == -1) {
    cfg.bold_as_font = !cfg.bold_as_colour;
    remember_file_option("finish", find_option(true, "BoldAsFont"));
  }

  if (0 < cfg.transparency && cfg.transparency <= 3)
    cfg.transparency *= 16;
}

static void
save_config(void)
{
  string filename;

  filename = path_win_w_to_posix(rc_filename);

  FILE *file = fopen(filename, "w");

  if (!file) {
    char *msg;
    char * up = cs__wcstoutf(rc_filename);
    //__ %1$s: config file name, %2$s: error message
    int len = asprintf(&msg, _("Could not save options to '%s':\n%s."),
                       up, strerror(errno));
    free(up);
    if (len > 0) {
      win_show_error(msg);
      delete(msg);
    }
  }
  else {
    for (uint j = 0; j < file_opts_num; j++) {
      if (file_opts[j].comment) {
        fprintf(file, "%s\n", file_opts[j].comment);
        continue;
      }
      uint i = file_opts[j].opti;
      opt_type type = options[i].type;
      if (!(type & OPT_LEGACY)) {
        fprintf(file, "%s=", options[i].name);
        //?void *cfg_p = seen_arg_option(i) ? &file_cfg : &cfg;
        void *cfg_p = &file_cfg;
        void *val_p = cfg_p + options[i].offset;
        switch (type) {
          when OPT_STRING:
            fprintf(file, "%s", *(string *)val_p);
          when OPT_WSTRING: {
            char * s = cs__wcstoutf(*(wstring *)val_p);
            fprintf(file, "%s", s);
            free(s);
          }
          when OPT_INT:
            fprintf(file, "%i", *(int *)val_p);
          when OPT_COLOUR: {
            colour c = *(colour *)val_p;
            fprintf(file, "%u,%u,%u", red(c), green(c), blue(c));
          }
          otherwise: {
            int val = *(char *)val_p;
            opt_val *o = opt_vals[type];
            for (; o->name; o++) {
              if (o->val == val)
                break;
            }
            if (o->name)
              fputs(o->name, file);
            else
              fprintf(file, "%i", val);
          }
        }
        fputc('\n', file);
      }
    }
    fclose(file);
  }

  delete(filename);
}


static control *cols_box, *rows_box, *locale_box, *charset_box;

void
apply_config(bool save)
{
  // Record what's changed
  for (uint i = 0; i < lengthof(options); i++) {
    opt_type type = options[i].type;
    uint offset = options[i].offset;
    //void *val_p = (void *)&cfg + offset;
    void *val_p = (void *)&file_cfg + offset;
    void *new_val_p = (void *)&new_cfg + offset;
    bool changed;
    switch (type) {
      when OPT_STRING:
        changed = strcmp(*(string *)val_p, *(string *)new_val_p);
      when OPT_WSTRING:
        changed = wcscmp(*(wstring *)val_p, *(wstring *)new_val_p);
      when OPT_INT or OPT_COLOUR:
        changed = (*(int *)val_p != *(int *)new_val_p);
      otherwise:
        changed = (*(char *)val_p != *(char *)new_val_p);
    }
    if (changed) {
      remember_file_option("apply", i);
    }
  }

  copy_config("apply", &file_cfg, &new_cfg);
  if (wcscmp(new_cfg.lang, cfg.lang) != 0
      || (wcscmp(new_cfg.lang, W("=")) == 0 && new_cfg.locale != cfg.locale)
     )
    load_messages(&new_cfg);
  win_reconfig();  // copy_config(&cfg, &new_cfg);
  if (save)
    save_config();
  bool had_theme = !!*cfg.theme_file;

  if (*cfg.colour_scheme) {
    load_scheme(cfg.colour_scheme);
    win_reset_colours();
  }
  else if (*cfg.theme_file) {
    load_theme(cfg.theme_file);
    win_reset_colours();
  }
  else if (had_theme)
    win_reset_colours();
}


// Registry handling (for retrieving localized sound labels)

static HKEY
regopen(HKEY key, char * subkey)
{
  HKEY hk = 0;
  RegOpenKeyA(key, subkey, &hk);
  return hk;
}

static HKEY
getmuicache()
{
  HKEY hk = regopen(HKEY_CURRENT_USER, "Software\\Classes\\Local Settings\\MuiCache");
  if (!hk)
    return 0;

  char sk[256];
  if (RegEnumKeyA(hk, 0, sk, 256) != ERROR_SUCCESS)
    return 0;

  HKEY hk1 = regopen(hk, sk);
  RegCloseKey(hk);
  if (!hk1)
    return 0;

  if (RegEnumKeyA(hk1, 0, sk, 256) != ERROR_SUCCESS)
    return 0;

  hk = regopen(hk1, sk);
  RegCloseKey(hk1);
  if (!hk)
    return 0;

  return hk;
}

static HKEY muicache = 0;
static HKEY evlabels = 0;

static void
retrievemuicache()
{
  muicache = getmuicache();
  if (muicache) {
    evlabels = regopen(HKEY_CURRENT_USER, "AppEvents\\EventLabels");
    if (!evlabels) {
      RegCloseKey(muicache);
      muicache = 0;
    }
  }
}

static void
closemuicache()
{
  if (muicache) {
    RegCloseKey(evlabels);
    RegCloseKey(muicache);
  }
}

static wchar *
getreg(HKEY key, wchar * subkey, wchar * attribute)
{
#if CYGWIN_VERSION_API_MINOR < 74
  (void)key;
  (void)subkey;
  (void)attribute;
  return 0;
#else
  DWORD blen;
  int res = RegGetValueW(key, subkey, attribute, RRF_RT_ANY, 0, 0, &blen);
  if (res)
    return 0;
  wchar * val = malloc(blen);
  res = RegGetValueW(key, subkey, attribute, RRF_RT_ANY, 0, val, &blen);
  if (res) {
    free(val);
    return 0;
  }
  return val;
#endif
}

static wchar *
muieventlabel(wchar * event)
{
  // HKEY_CURRENT_USER\AppEvents\EventLabels\SystemAsterisk
  // DispFileName -> "@mmres.dll,-5843"
  wchar * rsr = getreg(evlabels, event, W("DispFileName"));
  if (!rsr)
    return 0;
  // HKEY_CURRENT_USER\Software\Classes\Local Settings\MuiCache\N\M
  // "@mmres.dll,-5843" -> "Sternchen"
  wchar * lbl = getreg(muicache, 0, rsr);
  free(rsr);
  return lbl;
}


// Options dialog handlers

static void
ok_handler(control *unused(ctrl), int event)
{
  if (event == EVENT_ACTION) {
    apply_config(true);
    dlg_end();
  }
}

static void
cancel_handler(control *unused(ctrl), int event)
{
  if (event == EVENT_ACTION)
    dlg_end();
}

static void
apply_handler(control *unused(ctrl), int event)
{
  if (event == EVENT_ACTION)
    apply_config(false);
}

static void
about_handler(control *unused(ctrl), int event)
{
  if (event == EVENT_ACTION)
    win_show_about();
}


static void
add_file_resources(control *ctrl, wstring pattern)
{
  wstring suf = wcsrchr(pattern, L'.');
  int sufl = suf ? wcslen(suf) : 0;

  init_config_dirs();
  WIN32_FIND_DATAW ffd;
  HANDLE hFind = NULL;
  int ok = false;
  for (int i = last_config_dir; i >= 0; i--) {
    wchar * rcpat = path_posix_to_win_w(config_dirs[i]);
    int len = wcslen(rcpat);
    rcpat = renewn(rcpat, len + wcslen(pattern) + 2);
    rcpat[len++] = L'/';
    wcscpy(&rcpat[len], pattern);

    hFind = FindFirstFileW(rcpat, &ffd);
    ok = hFind != INVALID_HANDLE_VALUE;
    free(rcpat);
    if (ok)
      break;
    if (GetLastError() == ERROR_FILE_NOT_FOUND)  // empty valid dir
      break;
  }

  while (ok) {
    if (ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
      // skip
    }
    else {
      //LARGE_INTEGER filesize = {.LowPart = ffd.nFileSizeLow, .HighPart = ffd.nFileSizeHigh};
      //long s = filesize.QuadPart;

      // strip suffix
      int len = wcslen(ffd.cFileName);
      if (ffd.cFileName[0] != '.' && ffd.cFileName[len - 1] != '~') {
        ffd.cFileName[len - sufl] = 0;
        dlg_listbox_add_w(ctrl, ffd.cFileName);
      }
    }
    ok = FindNextFileW(hFind, &ffd);
  }
  FindClose(hFind);
}


static void
current_size_handler(control *unused(ctrl), int event)
{
  if (event == EVENT_ACTION) {
    new_cfg.cols = term.cols;
    new_cfg.rows = term.rows;
    dlg_refresh(cols_box);
    dlg_refresh(rows_box);
  }
}

static void
printer_handler(control *ctrl, int event)
{
  const wstring NONE = _W("◇ None (printing disabled) ◇");  // ♢◇
  const wstring CFG_NONE = W("");
  const wstring DEFAULT = _W("◆ Default printer ◆");  // ♦◆
  const wstring CFG_DEFAULT = W("*");
  wstring printer = new_cfg.printer;
  if (event == EVENT_REFRESH) {
    dlg_listbox_clear(ctrl);
    dlg_listbox_add_w(ctrl, NONE);
    dlg_listbox_add_w(ctrl, DEFAULT);
    uint num = printer_start_enum();
    for (uint i = 0; i < num; i++)
      dlg_listbox_add_w(ctrl, (wchar *)printer_get_name(i));
    printer_finish_enum();
    if (*printer == '*')
      dlg_editbox_set_w(ctrl, DEFAULT);
    else
      dlg_editbox_set_w(ctrl, *printer ? printer : NONE);
  }
  else if (event == EVENT_VALCHANGE || event == EVENT_SELCHANGE) {
    int n = dlg_listbox_getcur(ctrl);
    if (n == 0)
      wstrset(&printer, CFG_NONE);
    else if (n == 1)
      wstrset(&printer, CFG_DEFAULT);
    else
      dlg_editbox_get_w(ctrl, &printer);

    new_cfg.printer = printer;
  }
}

static void
set_charset(string charset)
{
  strset(&new_cfg.charset, charset);
  dlg_editbox_set(charset_box, charset);
}

static void
locale_handler(control *ctrl, int event)
{
  string locale = new_cfg.locale;
  switch (event) {
    when EVENT_REFRESH:
      dlg_listbox_clear(ctrl);
      string l;
      for (int i = 0; (l = locale_menu[i]); i++)
        dlg_listbox_add(ctrl, l);
      dlg_editbox_set(ctrl, locale);
    when EVENT_UNFOCUS:
      dlg_editbox_set(ctrl, locale);
      if (!*locale)
        set_charset("");
    when EVENT_VALCHANGE:
      dlg_editbox_get(ctrl, &new_cfg.locale);
    when EVENT_SELCHANGE:
      dlg_editbox_get(ctrl, &new_cfg.locale);
      if (*locale == '(')
        strset(&locale, "");
      if (!*locale)
        set_charset("");
#if HAS_LOCALES
      else if (!*new_cfg.charset)
        set_charset("UTF-8");
#endif
      new_cfg.locale = locale;
  }
}

static void
check_locale(void)
{
  if (!*new_cfg.locale) {
    strset(&new_cfg.locale, "C");
    dlg_editbox_set(locale_box, "C");
  }
}

static void
charset_handler(control *ctrl, int event)
{
  string charset = new_cfg.charset;
  switch (event) {
    when EVENT_REFRESH:
      dlg_listbox_clear(ctrl);
      string cs;
      for (int i = 0; (cs = charset_menu[i]); i++)
        dlg_listbox_add(ctrl, cs);
      dlg_editbox_set(ctrl, charset);
    when EVENT_UNFOCUS:
      dlg_editbox_set(ctrl, charset);
      if (*charset)
        check_locale();
    when EVENT_VALCHANGE:
      dlg_editbox_get(ctrl, &new_cfg.charset);
    when EVENT_SELCHANGE:
      dlg_editbox_get(ctrl, &charset);
      if (*charset == '(')
        strset(&charset, "");
      else {
        *strchr(charset, ' ') = 0;
        check_locale();
      }
      new_cfg.charset = charset;
  }
}

static void
lang_handler(control *ctrl, int event)
{
  //__ UI language
  const wstring NONE = _W("– None –");
  const wstring WINLOC = _W("@ Windows language @");
  const wstring LOCENV = _W("* Locale environm. *");
  const wstring LOCALE = _W("= cfg. Text Locale =");
  switch (event) {
    when EVENT_REFRESH:
      dlg_listbox_clear(ctrl);
      dlg_listbox_add_w(ctrl, NONE);
      dlg_listbox_add_w(ctrl, WINLOC);
      dlg_listbox_add_w(ctrl, LOCENV);
      dlg_listbox_add_w(ctrl, LOCALE);
      add_file_resources(ctrl, W("lang/*.po"));
      if (wcscmp(new_cfg.lang, W("")) == 0)
        dlg_editbox_set_w(ctrl, NONE);
      else if (wcscmp(new_cfg.lang, W("@")) == 0)
        dlg_editbox_set_w(ctrl, WINLOC);
      else if (wcscmp(new_cfg.lang, W("*")) == 0)
        dlg_editbox_set_w(ctrl, LOCENV);
      else if (wcscmp(new_cfg.lang, W("=")) == 0)
        dlg_editbox_set_w(ctrl, LOCALE);
      else
        dlg_editbox_set_w(ctrl, new_cfg.lang);
    when EVENT_VALCHANGE or EVENT_SELCHANGE: {
      int n = dlg_listbox_getcur(ctrl);
      if (n == 0)
        wstrset(&new_cfg.lang, W(""));
      else if (n == 1)
        wstrset(&new_cfg.lang, W("@"));
      else if (n == 2)
        wstrset(&new_cfg.lang, W("*"));
      else if (n == 3)
        wstrset(&new_cfg.lang, W("="));
      else
        dlg_editbox_get_w(ctrl, &new_cfg.lang);
    }
  }
}

static void
term_handler(control *ctrl, int event)
{
  switch (event) {
    when EVENT_REFRESH:
      dlg_listbox_clear(ctrl);
      dlg_listbox_add(ctrl, "xterm");
      dlg_listbox_add(ctrl, "xterm-256color");
      dlg_listbox_add(ctrl, "xterm-vt220");
      dlg_listbox_add(ctrl, "vt100");
      dlg_listbox_add(ctrl, "vt220");
      dlg_listbox_add(ctrl, "vt340");
      dlg_editbox_set(ctrl, new_cfg.term);
    when EVENT_VALCHANGE or EVENT_SELCHANGE:
      dlg_editbox_get(ctrl, &new_cfg.term);
  }
}

//  1 -> 0x00000000 MB_OK              Default Beep
//  2 -> 0x00000010 MB_ICONSTOP        Critical Stop
//  3 -> 0x00000020 MB_ICONQUESTION    Question
//  4 -> 0x00000030 MB_ICONEXCLAMATION Exclamation
//  5 -> 0x00000040 MB_ICONASTERISK    Asterisk
// -1 -> 0xFFFFFFFF                    Simple Beep
static struct {
  string name;
  wchar * event;
} beeps[] = {
  {__("simple beep"), null},
  {__("no beep"), null},
  {__("Default Beep"),	W(".Default")},
  {__("Critical Stop"),	W("SystemHand")},
  {__("Question"),	W("SystemQuestion")},
  {__("Exclamation"),	W("SystemExclamation")},
  {__("Asterisk"),	W("SystemAsterisk")},
};

static void
bell_handler(control *ctrl, int event)
{
  switch (event) {
    when EVENT_REFRESH:
      dlg_listbox_clear(ctrl);
      retrievemuicache();
      for (uint i = 0; i < lengthof(beeps); i++) {
        char * beepname = _(beeps[i].name);
        if (beepname == beeps[i].name) {
          // no localization entry, try to retrieve system localization
          if (muicache && beeps[i].event) {
            wchar * lbl = muieventlabel(beeps[i].event);
            if (lbl) {
              dlg_listbox_add_w(ctrl, lbl);
              if ((int)i == new_cfg.bell_type + 1)
                dlg_editbox_set_w(ctrl, lbl);
              beepname = null;
              free(lbl);
            }
          }
        }
        if (beepname) {
          dlg_listbox_add(ctrl, beepname);
          if ((int)i == new_cfg.bell_type + 1)
            dlg_editbox_set(ctrl, beepname);
        }
      }
      closemuicache();
    when EVENT_VALCHANGE or EVENT_SELCHANGE: {
      new_cfg.bell_type = dlg_listbox_getcur(ctrl) - 1;

      win_bell(&new_cfg);
    }
  }
}

static void
bellfile_handler(control *ctrl, int event)
{
  const wstring NONE = _W("◇ None (system sound) ◇");  // ♢◇
  const wstring CFG_NONE = W("");
  wstring bell_file = new_cfg.bell_file;
  if (event == EVENT_REFRESH) {
    dlg_listbox_clear(ctrl);
    dlg_listbox_add_w(ctrl, NONE);
    add_file_resources(ctrl, W("sounds/*.wav"));
    // strip std dir prefix...
    dlg_editbox_set_w(ctrl, *bell_file ? bell_file : NONE);
  }
  else if (event == EVENT_VALCHANGE || event == EVENT_SELCHANGE) {
    if (dlg_listbox_getcur(ctrl) == 0)
      wstrset(&bell_file, CFG_NONE);
    else
      dlg_editbox_get_w(ctrl, &bell_file);

    // add std dir prefix?
    new_cfg.bell_file = bell_file;
    win_bell(&new_cfg);
  }
  else if (event == EVENT_DROP) {
    dlg_editbox_set_w(ctrl, dragndrop);
    wstrset(&new_cfg.bell_file, dragndrop);
    win_bell(&new_cfg);
  }
}

static control * theme = null;
static control * store_button = null;

static void
enable_widget(control * ctrl, bool enable)
{
  if (!ctrl)
    return;

  HWND wid = ctrl->widget;
  EnableWindow(wid, enable);
}

static char *
download_scheme(char * url)
{
  if (strchr(url, '\''))
    return null;  // Insecure link

  static string cmdpat = "curl '%s' -o - 2> /dev/null";
  char * cmd = newn(char, strlen(cmdpat) -1 + strlen(url));
  sprintf(cmd, cmdpat, url);
  FILE * sf = popen(cmd, "r");
  if (!sf)
    return null;

  char * sch = null;
  while (fgets(linebuf, sizeof(linebuf) - 1, sf)) {
    char * eq = linebuf;
    while ((eq = strchr(++eq, '='))) {
      int dum;
      if (sscanf(eq, "= %d , %d , %d", &dum, &dum, &dum) == 3) {
        char *cp = eq;
        while (strchr("=0123456789, ", *cp))
          cp++;
        *cp++ = ';';
        *cp = '\0';
        cp = eq;
        if (cp != linebuf)
          cp--;
        while (strchr("BCFGMRWYacdeghiklnorstuwy ", *cp)) {
          eq = cp;
          if (cp == linebuf)
            break;
          else
            cp--;
        }
        while (*eq == ' ')
          eq++;
        if (*eq != '=') {
          // squeeze white space
          char * src = eq;
          char * dst = eq;
          while (*src) {
            if (*src != ' ' && *src != '\t')
              *dst++ = *src;
            src++;
          }
          *dst = '\0';

          int len = sch ? strlen(sch) : 0;
          sch = renewn(sch, len + strlen(eq) + 1);
          strcpy(&sch[len], eq);
        }
        break;
      }
    }
  }
  pclose(sf);

  return sch;
}

static void
theme_handler(control *ctrl, int event)
{
  //__ terminal theme / colour scheme
  const wstring NONE = _W("◇ None ◇");  // ♢◇
  const wstring CFG_NONE = W("");
  //__ indicator of unsaved downloaded colour scheme
  const wstring DOWNLOADED = _W("downloaded / give me a name!");
  // downloaded theme indicator must contain a slash
  // to steer enabled state of Store button properly
  const wstring CFG_DOWNLOADED = W("@/@");
  wstring theme_name = new_cfg.theme_file;
  if (event == EVENT_REFRESH) {
    dlg_listbox_clear(ctrl);
    dlg_listbox_add_w(ctrl, NONE);
    add_file_resources(ctrl, W("themes/*"));
#ifdef attempt_to_keep_scheme_hidden
    if (*new_cfg.colour_scheme)
      // don't do this, rather keep previously entered name to store scheme
      // scheme string will not be entered here anyway
      dlg_editbox_set_w(ctrl, W(""));
    else
#endif
    dlg_editbox_set_w(ctrl, !wcscmp(theme_name, CFG_DOWNLOADED) ? DOWNLOADED : *theme_name ? theme_name : NONE);
  }
  else if (event == EVENT_SELCHANGE) {  // pull-down selection
    if (dlg_listbox_getcur(ctrl) == 0)
      wstrset(&theme_name, CFG_NONE);
    else
      dlg_editbox_get_w(ctrl, &theme_name);

    new_cfg.theme_file = theme_name;
    // clear pending colour scheme
    strset(&new_cfg.colour_scheme, "");
    enable_widget(store_button, false);
  }
  else if (event == EVENT_VALCHANGE) {  // pasted or typed-in
    dlg_editbox_get_w(ctrl, &theme_name);
    new_cfg.theme_file = theme_name;
    enable_widget(store_button,
                  *new_cfg.colour_scheme && *theme_name
                  && !wcschr(theme_name, L'/') && !wcschr(theme_name, L'\\')
                 );
  }
  else if (event == EVENT_DROP) {
    if (wcsncmp(W("data:text/plain,"), dragndrop, 16) == 0) {
      // indicate availability of downloaded scheme to be stored
      dlg_editbox_set_w(ctrl, DOWNLOADED);
      wstrset(&new_cfg.theme_file, CFG_DOWNLOADED);
      // un-URL-escape scheme description
      char * scheme = cs__wcstoutf(&dragndrop[16]);
      char * url = scheme;
      char * sch = scheme;
      while (*url) {
        int c;
        if (sscanf(url, "%%%02X", &c) == 1) {
          url += 3;
        }
        else
          c = *url++;
        if (c == '\n')
          *sch++ = ';';
        else if (c != '\r')
          *sch++ = c;
      }
      *sch = '\0';
      strset(&new_cfg.colour_scheme, scheme);
      free(scheme);
      enable_widget(store_button, false);
    }
    else if (wcsncmp(W("http:"), dragndrop, 5) == 0
          || wcsncmp(W("https:"), dragndrop, 6) == 0
          || wcsncmp(W("ftp:"), dragndrop, 4) == 0
          || wcsncmp(W("ftps:"), dragndrop, 5) == 0
            ) {
      char * url = cs__wcstoutf(dragndrop);
      char * sch = download_scheme(url);
      if (sch) {
        wchar * urlpoi = wcschr(dragndrop, '?');
        if (urlpoi)
          *urlpoi = 0;
        urlpoi = wcsrchr(dragndrop, '/');
        if (urlpoi) {
          // set theme name proposal to url base name
          urlpoi++;
          dlg_editbox_set_w(ctrl, urlpoi);
          wstrset(&new_cfg.theme_file, urlpoi);
          // set scheme
          strset(&new_cfg.colour_scheme, sch);

          enable_widget(store_button, true);
        }
        free(sch);
      }
      else {
        win_bell(&new_cfg);  // Could not load web theme
        win_show_warning(_("Could not load web theme"));
      }
      free(url);
    }
    else {
      dlg_editbox_set_w(ctrl, dragndrop);
      wstrset(&new_cfg.theme_file, dragndrop);
      enable_widget(store_button, false);
    }
  }
}

#define dont_debug_dragndrop

static void
scheme_saver(control *ctrl, int event)
{
  wstring theme_name = new_cfg.theme_file;
  if (event == EVENT_REFRESH) {
    enable_widget(ctrl,
                  *new_cfg.colour_scheme && *theme_name
                  && !wcschr(theme_name, L'/') && !wcschr(theme_name, L'\\')
                 );
  }
  else if (event == EVENT_ACTION) {
#ifdef debug_dragndrop
    printf("%ls <- <%s>\n", new_cfg.theme_file, new_cfg.colour_scheme);
#endif
    if (*new_cfg.colour_scheme && *theme_name)
      if (!wcschr(theme_name, L'/') && !wcschr(theme_name, L'\\')) {
        char * sn = get_resource_file(W("themes"), theme_name, true);
        if (sn) {
          // save colour_scheme to theme_file
          FILE * thf = fopen(sn, "w");
          free(sn);
          if (thf) {
            char * sch = (char *)new_cfg.colour_scheme;
            for (int i = 0; sch[i]; i++) {
              if (sch[i] == ';')
                sch[i] = '\n';
            }
            fprintf(thf, "%s", sch);
            fclose(thf);

            strset(&new_cfg.colour_scheme, "");
            enable_widget(store_button, false);
          }
          else {
            win_bell(&new_cfg);  // Cannot write theme file
            win_show_warning(_("Cannot write theme file"));
          }
        }
        else {
          win_bell(&new_cfg);  // Cannot store theme file
          win_show_warning(_("Cannot store theme file"));
        }
      }
  }
}

static void
bell_tester(control *unused(ctrl), int event)
{
  if (event == EVENT_ACTION)
    win_bell(&new_cfg);
}

static void
url_opener(control *ctrl, int event)
{
  if (event == EVENT_ACTION) {
    wstring url = ctrl->context;
    win_open(wcsdup(url));
  }
  else if (event == EVENT_DROP) {
    theme_handler(theme, EVENT_DROP);
  }
}

void
setup_config_box(controlbox * b)
{
  controlset *s;
  control *c;

 /*
  * The standard panel that appears at the bottom of all panels:
  * Open, Cancel, Apply etc.
  */
  s = ctrl_new_set(b, "", "", "");
  ctrl_columns(s, 5, 20, 20, 20, 20, 20);
  //__ Dialog button - show About text
  c = ctrl_pushbutton(s, _("About..."), about_handler, 0);
  c->column = 0;
  //__ Dialog button - save changes
  c = ctrl_pushbutton(s, _("Save"), ok_handler, 0);
  c->button.isdefault = true;
  c->column = 2;
  //__ Dialog button - cancel
  c = ctrl_pushbutton(s, _("Cancel"), cancel_handler, 0);
  c->button.iscancel = true;
  c->column = 3;
  //__ Dialog button - apply changes
  c = ctrl_pushbutton(s, _("Apply"), apply_handler, 0);
  c->column = 4;
#ifdef __gettext
  //__ Dialog button - take notice
  __("I see")
  //__ Dialog button - confirm action
  __("OK")
#endif

 /*
  * The Looks panel.
  */
  //__ Options - Looks: treeview label
  s = ctrl_new_set(b, _("Looks"), 
  //__ Options - Looks: panel title
                      _("Looks in Terminal"), 
  //__ Options - Looks: section title
                      _("Colours"));
  ctrl_columns(s, 3, 33, 33, 33);
  ctrl_pushbutton(
    //__ Options - Looks:
    s, _("&Foreground..."), dlg_stdcolour_handler, &new_cfg.fg_colour
  )->column = 0;
  ctrl_pushbutton(
    //__ Options - Looks:
    s, _("&Background..."), dlg_stdcolour_handler, &new_cfg.bg_colour
  )->column = 1;
  ctrl_pushbutton(
    //__ Options - Looks:
    s, _("&Cursor..."), dlg_stdcolour_handler, &new_cfg.cursor_colour
  )->column = 2;
  theme = ctrl_combobox(
    //__ Options - Looks:
    s, _("&Theme"), 80, theme_handler, &new_cfg.theme_file
  );
  ctrl_columns(s, 1, 100);  // reset column stuff so we can rearrange them

  ctrl_columns(s, 3, 40, 40, 20);
  ctrl_editbox(
    s, _("&Line Highlight"), 20, dlg_stdintbox_handler, &new_cfg.even_line_highlight_delta
  )->column = 0;
  ctrl_pushbutton(s, _("Scheme Designer"), url_opener, W("http://ciembor.github.io/4bit/"))
    ->column = 1;
  (store_button = ctrl_pushbutton(s, _("Store"), scheme_saver, 0))
    ->column = 2;

  s = ctrl_new_set(b, _("Looks"), null, 
  //__ Options - Looks: section title
                      _("Transparency"));
  bool with_glass = win_is_glass_available();
  ctrl_radiobuttons(
    s, null, 4 + with_glass,
    dlg_stdradiobutton_handler, &new_cfg.transparency,
    //__ Options - Looks: transparency
    _("&Off"), TR_OFF,
    //__ Options - Looks: transparency
    _("&Low"), TR_LOW,
    //__ Options - Looks: transparency, short form of radio button label "Medium"
    with_glass ? _("&Med.")
    //__ Options - Looks: transparency
               : _("&Medium"), TR_MEDIUM,
    //__ Options - Looks: transparency
    _("&High"), TR_HIGH,
    //__ Options - Looks: transparency
    with_glass ? _("Gla&ss") : null, TR_GLASS,
    null
  );
#ifdef support_blurred
  ctrl_columns(s, 2, with_glass ? 80 : 75, with_glass ? 20 : 25);
  ctrl_checkbox(
    //__ Options - Looks: transparency
    s, _("Opa&que when focused"),
    dlg_stdcheckbox_handler, &new_cfg.opaque_when_focused
  )->column = 0;
  ctrl_checkbox(
    //__ Options - Looks: transparency
    s, _("Blu&r"),
    dlg_stdcheckbox_handler, &new_cfg.blurred
  )->column = 1;
#else
  ctrl_checkbox(
    //__ Options - Looks: transparency
    s, _("Opa&que when focused"),
    dlg_stdcheckbox_handler, &new_cfg.opaque_when_focused
  );
#endif

  s = ctrl_new_set(b, _("Looks"), null, 
  //__ Options - Looks: section title
                      _("Cursor"));
  ctrl_radiobuttons(
    s, null, 4 + with_glass,
    dlg_stdradiobutton_handler, &new_cfg.cursor_type,
    //__ Options - Looks: cursor type
    _("Li&ne"), CUR_LINE,
    //__ Options - Looks: cursor type
    _("Bloc&k"), CUR_BLOCK,
    //__ Options - Looks: cursor type
    _("&Underscore"), CUR_UNDERSCORE,
    null
  );
  ctrl_checkbox(
    //__ Options - Looks: cursor feature
    s, _("Blinkin&g"), dlg_stdcheckbox_handler, &new_cfg.cursor_blinks
  );

 /*
  * The Text panel.
  */
  //__ Options - Text: treeview label
  s = ctrl_new_set(b, _("Text"), 
  //__ Options - Text: panel title
                      _("Text and Font properties"), 
  //__ Options - Text: section title
                      _("Font"));
  ctrl_fontsel(
    s, null, dlg_stdfontsel_handler, &new_cfg.font
  );

  s = ctrl_new_set(b, _("Text"), null, null);
  ctrl_columns(s, 2, 50, 50);
  ctrl_radiobuttons(
    //__ Options - Text:
    s, _("Font smoothing"), 2,
    dlg_stdradiobutton_handler, &new_cfg.font_smoothing,
    //__ Options - Text:
    _("&Default"), FS_DEFAULT,
    //__ Options - Text:
    _("&None"), FS_NONE,
    //__ Options - Text:
    _("&Partial"), FS_PARTIAL,
    //__ Options - Text:
    _("&Full"), FS_FULL,
    null
  )->column = 1;

  ctrl_checkbox(
    //__ Options - Text:
    s, _("Sho&w bold as font"),
    dlg_stdcheckbox_handler, &new_cfg.bold_as_font
  )->column = 0;
  ctrl_checkbox(
    //__ Options - Text:
    s, _("Show &bold as colour"),
    dlg_stdcheckbox_handler, &new_cfg.bold_as_colour
  )->column = 0;
  ctrl_checkbox(
    //__ Options - Text:
    s, _("&Allow blinking"),
    dlg_stdcheckbox_handler, &new_cfg.allow_blinking
  )->column = 0;

  s = ctrl_new_set(b, _("Text"), null, null);
  ctrl_columns(s, 2, 29, 71);
  (locale_box = ctrl_combobox(
    s, _("&Locale"), 100, locale_handler, 0
  ))->column = 0;
  (charset_box = ctrl_combobox(
    s, _("&Character set"), 100, charset_handler, 0
  ))->column = 1;

 /*
  * The Keys panel.
  */
  //__ Options - Keys: treeview label
  s = ctrl_new_set(b, _("Keys"), 
  //__ Options - Keys: panel title
                      _("Keyboard features"), null);
  ctrl_columns(s, 2, 50, 50);
  ctrl_checkbox(
    //__ Options - Keys:
    s, _("&Backarrow sends ^H"),
    dlg_stdcheckbox_handler, &new_cfg.backspace_sends_bs
  )->column = 0;
  ctrl_checkbox(
    //__ Options - Keys:
    s, _("&Delete sends DEL"),
    dlg_stdcheckbox_handler, &new_cfg.delete_sends_del
  )->column = 1;
  ctrl_checkbox(
    //__ Options - Keys:
    s, _("Ctrl+LeftAlt is Alt&Gr"),
    dlg_stdcheckbox_handler, &new_cfg.ctrl_alt_is_altgr
  );

  s = ctrl_new_set(b, _("Keys"), null, 
  //__ Options - Keys: section title
                      _("Shortcuts"));
  ctrl_checkbox(
    //__ Options - Keys:
    s, _("Cop&y and Paste (Ctrl/Shift+Ins)"),
    dlg_stdcheckbox_handler, &new_cfg.clip_shortcuts
  );
  ctrl_checkbox(
    //__ Options - Keys:
    s, _("&Menu and Full Screen (Alt+Space/Enter)"),
    dlg_stdcheckbox_handler, &new_cfg.window_shortcuts
  );
  ctrl_checkbox(
    //__ Options - Keys:
    s, _("&Switch window (Ctrl+[Shift+]Tab)"),
    dlg_stdcheckbox_handler, &new_cfg.switch_shortcuts
  );
  ctrl_checkbox(
    //__ Options - Keys:
    s, _("&Zoom (Ctrl+plus/minus/zero)"),
    dlg_stdcheckbox_handler, &new_cfg.zoom_shortcuts
  );
  ctrl_checkbox(
    //__ Options - Keys:
    s, _("&Alt+Fn shortcuts"),
    dlg_stdcheckbox_handler, &new_cfg.alt_fn_shortcuts
  );
  ctrl_checkbox(
    //__ Options - Keys:
    s, _("&Ctrl+Shift+letter shortcuts"),
    dlg_stdcheckbox_handler, &new_cfg.ctrl_shift_shortcuts
  );

  s = ctrl_new_set(b, _("Keys"), null, 
  //__ Options - Keys: section title
                      _("Compose key"));
  ctrl_radiobuttons(
    s, null, 4,
    dlg_stdradiobutton_handler, &new_cfg.compose_key,
    //__ Options - Keys:
    _("&Shift"), MDK_SHIFT,
    //__ Options - Keys:
    _("&Ctrl"), MDK_CTRL,
    //__ Options - Keys:
    _("&Alt"), MDK_ALT,
    //__ Options - Keys:
    _("&Off"), 0,
    null
  );

 /*
  * The Mouse panel.
  */
  //__ Options - Mouse: treeview label
  s = ctrl_new_set(b, _("Mouse"), 
  //__ Options - Mouse: panel title
                      _("Mouse functions"), null);
  ctrl_columns(s, 2, 50, 50);
  ctrl_checkbox(
    //__ Options - Mouse:
    s, _("Cop&y on select"),
    dlg_stdcheckbox_handler, &new_cfg.copy_on_select
  )->column = 0;
  ctrl_checkbox(
    //__ Options - Mouse:
    s, _("Copy as &rich text"),
    dlg_stdcheckbox_handler, &new_cfg.copy_as_rtf
  )->column = 1;
  ctrl_checkbox(
    //__ Options - Mouse:
    s, _("Clic&ks place command line cursor"),
    dlg_stdcheckbox_handler, &new_cfg.clicks_place_cursor
  );

  s = ctrl_new_set(b, _("Mouse"), null, 
  //__ Options - Mouse: section title
                      _("Click actions"));
  ctrl_radiobuttons(
    //__ Options - Mouse:
    s, _("Right mouse button"), 4,
    dlg_stdradiobutton_handler, &new_cfg.right_click_action,
    //__ Options - Mouse:
    _("&Paste"), RC_PASTE,
    //__ Options - Mouse:
    _("E&xtend"), RC_EXTEND,
    //__ Options - Mouse:
    _("&Menu"), RC_MENU,
    //__ Options - Mouse:
    _("Ente&r"), RC_ENTER,
    null
  );
  ctrl_radiobuttons(
    //__ Options - Mouse:
    s, _("Middle mouse button"), 4,
    dlg_stdradiobutton_handler, &new_cfg.middle_click_action,
    //__ Options - Mouse:
    _("&Paste"), MC_PASTE,
    //__ Options - Mouse:
    _("E&xtend"), MC_EXTEND,
    //__ Options - Mouse:
    _("&Nothing"), MC_VOID,
    //__ Options - Mouse:
    _("Ente&r"), MC_ENTER,
    null
  );

  s = ctrl_new_set(b, _("Mouse"), null, 
  //__ Options - Mouse: section title
                      _("Application mouse mode"));
  ctrl_radiobuttons(
    //__ Options - Mouse:
    s, _("Default click target"), 4,
    dlg_stdradiobutton_handler, &new_cfg.clicks_target_app,
    //__ Options - Mouse: application mouse mode click target
    _("&Window"), false,
    //__ Options - Mouse: application mouse mode click target
    _("&Application"), true,
    null
  );
  ctrl_radiobuttons(
    //__ Options - Mouse:
    s, _("Modifier for overriding default"), 4,
    dlg_stdradiobutton_handler, &new_cfg.click_target_mod,
    //__ Options - Mouse:
    _("&Shift"), MDK_SHIFT,
    //__ Options - Mouse:
    _("&Ctrl"), MDK_CTRL,
    //__ Options - Mouse:
    _("&Alt"), MDK_ALT,
    //__ Options - Mouse:
    _("&Off"), 0,
    null
  );

 /*
  * The Window panel.
  */
  //__ Options - Window: treeview label
  s = ctrl_new_set(b, _("Window"), 
  //__ Options - Window: panel title
                      _("Window properties"), 
  //__ Options - Window: section title
                      _("Default size"));
  ctrl_columns(s, 5, 35, 3, 28, 4, 30);
  (cols_box = ctrl_editbox(
    //__ Options - Window:
    s, _("Colu&mns"), 44, dlg_stdintbox_handler, &new_cfg.cols
  ))->column = 0;
  (rows_box = ctrl_editbox(
    //__ Options - Window:
    s, _("Ro&ws"), 55, dlg_stdintbox_handler, &new_cfg.rows
  ))->column = 2;
  ctrl_pushbutton(
    //__ Options - Window:
    s, _("C&urrent size"), current_size_handler, 0
  )->column = 4;

  s = ctrl_new_set(b, _("Window"), null, null);
  ctrl_columns(s, 2, 66, 34);
  ctrl_editbox(
    //__ Options - Window:
    s, _("Scroll&back lines"), 50,
    dlg_stdintbox_handler, &new_cfg.scrollback_lines
  )->column = 0;
  ctrl_radiobuttons(
    //__ Options - Window:
    s, _("Scrollbar"), 4,
    dlg_stdradiobutton_handler, &new_cfg.scrollbar,
    //__ Options - Window: scrollbar
    _("&Left"), -1,
    //__ Options - Window: scrollbar
    _("&None"), 0,
    //__ Options - Window: scrollbar
    _("&Right"), 1,
    null
  );
  ctrl_radiobuttons(
    //__ Options - Window:
    s, _("Modifier for scrolling"), 4,
    dlg_stdradiobutton_handler, &new_cfg.scroll_mod,
    //__ Options - Window:
    _("&Shift"), MDK_SHIFT,
    //__ Options - Window:
    _("&Ctrl"), MDK_CTRL,
    //__ Options - Window:
    _("&Alt"), MDK_ALT,
    //__ Options - Window:
    _("&Off"), 0,
    null
  );
  ctrl_checkbox(
    //__ Options - Window:
    s, _("&PgUp and PgDn scroll without modifier"),
    dlg_stdcheckbox_handler, &new_cfg.pgupdn_scroll
  );

  s = ctrl_new_set(b, _("Window"), null, 
  //__ Options - Window: section title
                      _("UI language"));
  ctrl_columns(s, 2, 60, 40);
  ctrl_combobox(
    s, null, 100, lang_handler, 0
  )->column = 0;

 /*
  * The Terminal panel.
  */
  //__ Options - Terminal: treeview label
  s = ctrl_new_set(b, _("Terminal"), 
  //__ Options - Terminal: panel title
                      _("Terminal features"), null);
  ctrl_columns(s, 2, 50, 50);
  ctrl_combobox(
    //__ Options - Terminal:
    s, _("&Type"), 100, term_handler, 0
  )->column = 0;
  ctrl_editbox(
    //__ Options - Terminal:
    s, _("&Answerback"), 100, dlg_stdstringbox_handler, &new_cfg.answerback
  )->column = 1;

  s = ctrl_new_set(b, _("Terminal"), null, 
  //__ Options - Terminal: section title
                      _("Bell"));
  ctrl_columns(s, 2, 73, 27);
  ctrl_combobox(
    s, null, 100, bell_handler, 0
  )->column = 0;
  ctrl_pushbutton(
    //__ Options - Terminal: bell
    s, _("► &Play"), bell_tester, 0
  )->column = 1;
  ctrl_columns(s, 1, 100);  // reset column stuff so we can rearrange them
  ctrl_columns(s, 2, 100, 0);
  ctrl_combobox(
    //__ Options - Terminal: bell
    s, _("&Wave"), 83, bellfile_handler, &new_cfg.bell_file
  )->column = 0;
  ctrl_columns(s, 1, 100);  // reset column stuff so we can rearrange them
  // balance column widths of the following 3 fields 
  // to accomodate different length of localized labels
  int strwidth(string s0) {
    int len = 0;
    unsigned char * sp = (unsigned char *)s0;
    while (*sp) {
      if ((*sp >= 0xE3 && *sp <= 0xED) || 
          (*sp == 0xF0 && *(sp + 1) >= 0xA0 && *(sp + 1) <= 0xBF))
        // approx. CJK range
        len += 4;
      else if (strchr(" il.,'()!:;[]|", *sp))
        len ++;
      else if (*sp != '&' && (*sp & 0xC0) != 0x80)
        len += 2;
      sp++;
    }
    return len;
  }
  //__ Options - Terminal: bell
  string lbl_flash = _("&Flash");
  //__ Options - Terminal: bell
  string lbl_highl = _("&Highlight in taskbar");
  //__ Options - Terminal: bell
  string lbl_popup = _("&Popup");
  int len = strwidth(lbl_flash) + strwidth(lbl_highl) + strwidth(lbl_popup);
# define cbw 14
  int l00_flash = (100 - 3 * cbw) * strwidth(lbl_flash) / len + cbw;
  int l00_highl = (100 - 3 * cbw) * strwidth(lbl_highl) / len + cbw;
  int l00_popup = (100 - 3 * cbw) * strwidth(lbl_popup) / len + cbw;
  ctrl_columns(s, 3, l00_flash, l00_highl, l00_popup);
  ctrl_checkbox(
    //__ Options - Terminal: bell
    s, _("&Flash"), dlg_stdcheckbox_handler, &new_cfg.bell_flash
  )->column = 0;
  ctrl_checkbox(
    //__ Options - Terminal: bell
    s, _("&Highlight in taskbar"), dlg_stdcheckbox_handler, &new_cfg.bell_taskbar
  )->column = 1;
  ctrl_checkbox(
    //__ Options - Terminal: bell
    s, _("&Popup"), dlg_stdcheckbox_handler, &new_cfg.bell_popup
  )->column = 2;

  s = ctrl_new_set(b, _("Terminal"), null, 
  //__ Options - Terminal: section title
                      _("Printer"));
#ifdef use_multi_listbox_for_printers
#warning left in here just to demonstrate the usage of ctrl_listbox
  ctrl_listbox(
    s, null, 4, 100, printer_handler, 0
  );
#else
  ctrl_combobox(
    s, null, 100, printer_handler, 0
  );
#endif

  s = ctrl_new_set(b, _("Terminal"), null, null);
  ctrl_checkbox(
    //__ Options - Terminal:
    s, _("Prompt about running processes on &close"),
    dlg_stdcheckbox_handler, &new_cfg.confirm_exit
  );
}
