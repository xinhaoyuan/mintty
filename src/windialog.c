// windialog.c (part of mintty)
// Copyright 2008-11 Andy Koppe
// Based on code from PuTTY-0.60 by Simon Tatham and team.
// Licensed under the terms of the GNU General Public License v3 or later.

#include "winpriv.h"

#include "ctrls.h"
#include "winctrls.h"
#include "winids.h"
#include "res.h"
#include "appinfo.h"

#define Unicode_TreeView_does_not_work

#include "charset.h"  // nonascii, cs__utftowcs

#include <commctrl.h>


void setup_config_box(controlbox *);

/*
 * windlg.c - Dialogs, including the configuration dialog.
 */

/*
 * These are the various bits of data required to handle the
 * portable-dialog stuff in the config box.
 */
static controlbox *ctrlbox;
/*
 * ctrls_base holds the OK and Cancel buttons: the controls which
 * are present in all dialog panels. ctrls_panel holds the ones
 * which change from panel to panel.
 */
static winctrls ctrls_base, ctrls_panel;

windlg dlg;
wstring dragndrop;

static int dialog_height;  // dummy

enum {
  IDCX_TVSTATIC = 1001,
  IDCX_TREEVIEW,
  IDCX_STDBASE,
  IDCX_PANELBASE = IDCX_STDBASE + 32
};

typedef struct {
  HWND treeview;
  HTREEITEM lastat[4];
} treeview_faff;

static HTREEITEM
treeview_insert(treeview_faff * faff, int level, char *text, char *path)
{
// text will be the label of an Options dialog treeview item;
// it is passed in here as the basename of path

  HTREEITEM newitem;

  if (nonascii(path)) {
#ifdef Unicode_TreeView
    // Using this variant for Unicode TreeView entries should enable 
    // their proper display in the treeview, but it does not work,
    // the label is handled as ANSI string anyway
#endif
    wchar * utext = cs__utftowcs(text);
    TVINSERTSTRUCTW ins;
    ins.hParent = (level > 0 ? faff->lastat[level - 1] : TVI_ROOT);
    ins.hInsertAfter = faff->lastat[level];
    ins.item.mask = TVIF_TEXT | TVIF_PARAM;
    ins.item.pszText = utext;
    //ins.item.cchTextMax = wcslen(utext) + 1;  // ignored when setting
    ins.item.lParam = (LPARAM) path;
    newitem = (HTREEITEM)SendMessageW(faff->treeview, TVM_INSERTITEM, 0, (LPARAM)&ins);
    //TreeView_SetUnicodeFormat((HWND)newitem, TRUE);  // does not work
    free(utext);
  }
  else {
    TVINSERTSTRUCT ins;
    ins.hParent = (level > 0 ? faff->lastat[level - 1] : TVI_ROOT);
    ins.hInsertAfter = faff->lastat[level];
    ins.item.mask = TVIF_TEXT | TVIF_PARAM;
    ins.item.pszText = text;
    //ins.item.cchTextMax = strlen(text) + 1;  // ignored when setting
    ins.item.lParam = (LPARAM) path;
    newitem = TreeView_InsertItem(faff->treeview, &ins);
  }

  if (level > 0)
    TreeView_Expand(faff->treeview, faff->lastat[level - 1],
                    (level > 1 ? TVE_COLLAPSE : TVE_EXPAND));
  faff->lastat[level] = newitem;
  for (int i = level + 1; i < 4; i++)
    faff->lastat[i] = null;
  return newitem;
}

/*
 * Create the panelfuls of controls in the configuration box.
 */
static void
create_controls(HWND wnd, char *path)
{
  ctrlpos cp;
  int index;
  int base_id;
  winctrls *wc;

  if (!path[0]) {
   /*
    * Here we must create the basic standard controls.
    */
    ctrlposinit(&cp, wnd, 3, 3, DIALOG_HEIGHT - 17);
    wc = &ctrls_base;
    base_id = IDCX_STDBASE;
  }
  else {
   /*
    * Otherwise, we're creating the controls for a particular panel.
    */
    ctrlposinit(&cp, wnd, 69, 3, 3);
    wc = &ctrls_panel;
    base_id = IDCX_PANELBASE;
  }

#ifdef debug_layout
  printf("create_controls (%s)\n", path);
#endif
  for (index = -1; (index = ctrl_find_path(ctrlbox, path, index)) >= 0;) {
    controlset *s = ctrlbox->ctrlsets[index];
    winctrl_layout(wc, &cp, s, &base_id);
  }
}

static void
determine_geometry(HWND wnd)
{
  // determine height in dialog box coordinates
  // as was configured in res.rc (IDD_MAINBOX) and applied magically
  RECT r;
  GetClientRect(wnd, &r);

  RECT normr;
  normr.left = 0;
  normr.top = 0;
  normr.right = 100;
  normr.bottom = 100;
  MapDialogRect(config_wnd, &normr);

  dialog_height = 100 * (r.bottom - r.top) / normr.bottom;
}

#define dont_debug_messages

/*
 * This function is the configuration box.
 * (Being a dialog procedure, in general it returns 0 if the default
 * dialog processing should be performed, and 1 if it should not.)
 */
static INT_PTR CALLBACK
config_dialog_proc(HWND wnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
#ifdef debug_messages
static struct {
  uint wm_;
  char * wm_name;
} wm_names[] = {
#include "_wm.t"
};
  char * wm_name = "WM_?";
  for (uint i = 0; i < lengthof(wm_names); i++)
    if (msg == wm_names[i].wm_) {
      wm_name = wm_names[i].wm_name;
      break;
    }
  printf("[%d] dialog_proc %04X %s (%04X %08X)\n", (int)time(0), msg, wm_name, (unsigned)wParam, (unsigned)lParam);
#endif
  switch (msg) {
    when WM_INITDIALOG: {
      ctrlbox = ctrl_new_box();
      setup_config_box(ctrlbox);
      windlg_init();
      winctrl_init(&ctrls_base);
      winctrl_init(&ctrls_panel);
      windlg_add_tree(&ctrls_base);
      windlg_add_tree(&ctrls_panel);
      copy_config("dialog", &new_cfg, &file_cfg);

      RECT r;
      GetWindowRect(GetParent(wnd), &r);
      dlg.wnd = wnd;

     /*
      * Create the actual GUI widgets.
      */
      // here we need the correct DIALOG_HEIGHT already
      create_controls(wnd, "");        /* Open and Cancel buttons etc */

      SendMessage(wnd, WM_SETICON, (WPARAM) ICON_BIG,
                  (LPARAM) LoadIcon(inst, MAKEINTRESOURCE(IDI_MAINICON)));

     /*
      * Create the tree view.
      */
      r.left = 3;
      r.right = r.left + 64;
      r.top = 3;
      r.bottom = r.top + DIALOG_HEIGHT - 26;
      MapDialogRect(wnd, &r);
      HWND treeview =
        CreateWindowEx(WS_EX_CLIENTEDGE, WC_TREEVIEW, "",
#ifdef Unicode_TreeView
                       // this doesn't have any effect
                       SS_OWNERDRAW |
#endif
                       WS_CHILD | WS_VISIBLE | WS_TABSTOP | TVS_HASLINES |
                       TVS_DISABLEDRAGDROP | TVS_HASBUTTONS | TVS_LINESATROOT
                       | TVS_SHOWSELALWAYS, r.left, r.top, r.right - r.left,
                       r.bottom - r.top, wnd, (HMENU) IDCX_TREEVIEW, inst,
                       null);
#ifdef Unicode_TreeView
      // the impact of this property is hardly described;
      // it does not fix the treeview Unicode display failure
      // but it prevents the panels from being selected
      //TreeView_SetUnicodeFormat(treeview, TRUE);
#endif
      WPARAM font = SendMessage(wnd, WM_GETFONT, 0, 0);
      SendMessage(treeview, WM_SETFONT, font, MAKELPARAM(true, 0));
      treeview_faff tvfaff;
      tvfaff.treeview = treeview;
      memset(tvfaff.lastat, 0, sizeof (tvfaff.lastat));

     /*
      * Set up the tree view contents.
      */
      HTREEITEM hfirst = null;
      char *path = null;

      for (int i = 0; i < ctrlbox->nctrlsets; i++) {
        controlset *s = ctrlbox->ctrlsets[i];
        HTREEITEM item;
        int j;
        char *c;

        if (!s->pathname[0])
          continue;
        j = path ? ctrl_path_compare(s->pathname, path) : 0;
        if (j == INT_MAX)
          continue;   /* same path, nothing to add to tree */

       /*
        * We expect never to find an implicit path component. 
          For example, we expect never to see A/B/C followed by A/D/E, 
          because that would _implicitly_ create A/D. 
          All our path prefixes are expected to contain actual controls 
          and be selectable in the treeview; so we would expect 
          to see A/D _explicitly_ before encountering A/D/E.
        */

        c = strrchr(s->pathname, '/');
        if (!c)
          c = s->pathname;
        else
          c++;

        item = treeview_insert(&tvfaff, j, c, s->pathname);
        if (!hfirst)
          hfirst = item;

        path = s->pathname;

       /*
        * Put the treeview selection on to the Session panel.
        * This should also cause creation of the relevant controls.
        */
        TreeView_SelectItem(treeview, hfirst);
      }

#ifdef Unicode_TreeView
      // with TreeView_SetUnicodeFormat(treeview, TRUE):
      // the loop below is empty
      // WM_NOTIFY is not triggered
#endif

     /*
      * Set focus into the first available control.
      */
      for (winctrl *c = ctrls_panel.first; c; c = c->next) {
        if (c->ctrl) {
          dlg_set_focus(c->ctrl);
          break;
        }
      }
    }

    when WM_DESTROY:
      winctrl_cleanup(&ctrls_base);
      winctrl_cleanup(&ctrls_panel);
      ctrl_free_box(ctrlbox);
      config_wnd = 0;

    when WM_NOTIFY: {
      if (LOWORD(wParam) == IDCX_TREEVIEW &&
          ((LPNMHDR) lParam)->code == TVN_SELCHANGED) {
        HTREEITEM i = TreeView_GetSelection(((LPNMHDR) lParam)->hwndFrom);

        TVITEM item;
        char buffer[64];
        item.hItem = i;
        item.pszText = buffer;
        item.cchTextMax = sizeof (buffer);
        item.mask = TVIF_TEXT | TVIF_PARAM;
        TreeView_GetItem(((LPNMHDR) lParam)->hwndFrom, &item);

       /* Destroy all controls in the currently visible panel. */
        for (winctrl *c = ctrls_panel.first; c; c = c->next) {
          for (int k = 0; k < c->num_ids; k++) {
            HWND item = GetDlgItem(wnd, c->base_id + k);
            if (item)
              DestroyWindow(item);
          }
        }
        winctrl_cleanup(&ctrls_panel);

        // here we need the correct DIALOG_HEIGHT already
        create_controls(wnd, (char *) item.lParam);
        dlg_refresh(null); /* set up control values */
      }
    }

    when WM_CLOSE:
      DestroyWindow(wnd);

    when WM_COMMAND or WM_DRAWITEM: {
      int ret = winctrl_handle_command(msg, wParam, lParam);
      if (dlg.ended)
        DestroyWindow(wnd);
      return ret;
    }

    when WM_USER: {
      HWND target = (HWND)wParam;
      // could delegate this to winctrls.c, like winctrl_handle_command;
      // but then we'd have to fiddle with the location of dragndrop
     /*
      * Look up the window handle in our data; find the control.
        (Hmm, apparently it works without looking for the widget entry 
        that was particularly introduced for this purpose...)
      */
      control * ctrl = null;
      for (winctrl *c = ctrls_panel.first; c && !ctrl; c = c->next) {
        if (c->ctrl)
          for (int k = 0; k < c->num_ids; k++) {
#ifdef debug_dragndrop
            printf(" [->%8p] %8p\n", target, GetDlgItem(wnd, c->base_id + k));
#endif
            if (target == GetDlgItem(wnd, c->base_id + k)) {
              ctrl = c->ctrl;
              break;
            }
        }
      }
      if (ctrl) {
        //dlg_editbox_set_w(ctrl, L"Test");  // may hit unrelated items...
        // drop the drag-and-drop contents here
        dragndrop = (wstring)lParam;
        ctrl->handler(ctrl, EVENT_DROP);
      }
    }
  }
  return 0;
}

HWND config_wnd;

void
win_open_config(void)
{
  if (config_wnd)
    return;

  set_dpi_auto_scaling(true);

  static bool initialised = false;
  if (!initialised) {
    InitCommonControls();
    RegisterClass(&(WNDCLASS){
      .style = CS_DBLCLKS,
      .lpfnWndProc = DefDlgProc,
      .cbClsExtra = 0,
      .cbWndExtra = DLGWINDOWEXTRA + 2 * sizeof (LONG_PTR),
      .hInstance = inst,
      .hIcon = null,
      .hCursor = LoadCursor(null, IDC_ARROW),
      .hbrBackground = (HBRUSH)(COLOR_BACKGROUND + 1),
      .lpszMenuName = null,
      .lpszClassName = "ConfigBox"
    });
    initialised = true;
  }

  config_wnd = CreateDialog(inst, MAKEINTRESOURCE(IDD_MAINBOX),
                            wnd, config_dialog_proc);
  // At this point, we could actually calculate the size of the 
  // dialog box used for the Options menu; however, the resulting 
  // value(s) (here DIALOG_HEIGHT) is already needed before this point, 
  // as the callback config_dialog_proc sets up dialog box controls.
  // How insane is that resource concept! Shouldn't I know my own geometry?
  determine_geometry(config_wnd);  // dummy call

  // Set title of Options dialog explicitly to facilitate I18N
  SendMessageW(config_wnd, WM_SETTEXT, 0, (LPARAM)_W("Options"));

  ShowWindow(config_wnd, SW_SHOW);

  set_dpi_auto_scaling(false);
}

void
win_show_about(void)
{
#if CYGWIN_VERSION_API_MINOR < 74
  char * aboutfmt = newn(char, 
    strlen(VERSION_TEXT) + strlen(COPYRIGHT) + strlen(LICENSE_TEXT) + strlen(_(WARRANTY_TEXT)) + strlen(_(ABOUT_TEXT)) + 11);
  sprintf(aboutfmt, "%s\n%s\n%s\n%s\n\n%s", 
           VERSION_TEXT, COPYRIGHT, LICENSE_TEXT, _(WARRANTY_TEXT), _(ABOUT_TEXT));
  char * abouttext = newn(char, strlen(aboutfmt) + strlen(WEBSITE));
  sprintf(abouttext, aboutfmt, WEBSITE);
#else
  char * aboutfmt =
    asform("%s\n%s\n%s\n%s\n\n%s", 
           VERSION_TEXT, COPYRIGHT, LICENSE_TEXT, _(WARRANTY_TEXT), _(ABOUT_TEXT));
  char * abouttext = asform(aboutfmt, WEBSITE);
#endif
  free(aboutfmt);
  wchar * wmsg = cs__utftowcs(abouttext);
  free(abouttext);
  MessageBoxIndirectW(&(MSGBOXPARAMSW){
    .cbSize = sizeof(MSGBOXPARAMSW),
    .hwndOwner = config_wnd,
    .hInstance = inst,
    .lpszCaption = W(APPNAME),
    .dwStyle = MB_USERICON | MB_OK,
    .lpszIcon = MAKEINTRESOURCEW(IDI_MAINICON),
    .lpszText = wmsg
  });
  free(wmsg);
}

static void
win_show_msg(char * msg, UINT type)
{
  if (nonascii(msg)) {
    wchar * wmsg = cs__utftowcs(msg);
    MessageBoxW(0, wmsg, 0, type);
    free(wmsg);
  }
  else
    MessageBox(0, msg, 0, type);
}

void
win_show_error(char * msg)
{
  win_show_msg(msg, MB_ICONERROR);
}

void
win_show_warning(char * msg)
{
  win_show_msg(msg, MB_ICONWARNING);
}
