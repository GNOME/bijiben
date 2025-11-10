/* biji-webkit-editor.c
 * Copyright (C) Pierre-Yves LUYTEN 2012 <py@luyten.fr>
 *
 * bijiben is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * bijiben is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <libxml/xmlwriter.h>

#include "config.h"

#include "bjb-utils.h"
#include "biji-webkit-editor.h"
#include "biji-editor-selection.h"
#include <jsc/jsc.h>

#define ZOOM_LARGE  1.5f;
#define ZOOM_MEDIUM 1.0f;
#define ZOOM_SMALL  0.8f;

/* Signals */
enum {
  EDITOR_CLOSED,
  CONTENT_CHANGED,
  EDITOR_SIGNALS
};

/* Block Format */
typedef enum {
  BLOCK_FORMAT_NONE,
  BLOCK_FORMAT_UNORDERED_LIST,
  BLOCK_FORMAT_ORDERED_LIST,
  BLOCK_FORMAT_INDENT,
  BLOCK_FORMAT_OUTDENT
} BlockFormat;

static guint biji_editor_signals [EDITOR_SIGNALS] = { 0 };

struct _BijiWebkitEditor
{
  WebKitWebView     parent_instance;

  BlockFormat       block_format;
  gboolean          first_load;
  gboolean          load_finished;
  GdkRGBA           note_color;
  gboolean          has_color;

  EEditorSelection *sel;
};

G_DEFINE_TYPE (BijiWebkitEditor, biji_webkit_editor, WEBKIT_TYPE_WEB_VIEW)

static WebKitWebContext *
biji_webkit_editor_get_web_context (void)
{
  static WebKitWebContext *web_context = NULL;

  if (!web_context)
  {
    web_context = webkit_web_context_get_default ();
    webkit_web_context_set_cache_model (web_context, WEBKIT_CACHE_MODEL_DOCUMENT_VIEWER);
    webkit_web_context_set_spell_checking_enabled (web_context, TRUE);
  }

  return web_context;
}

static WebKitSettings *
biji_webkit_editor_get_web_settings (void)
{
  static WebKitSettings *settings = NULL;

  if (!settings)
  {
    settings = webkit_settings_new_with_settings (
      "enable-page-cache", FALSE,
      "enable-tabs-to-links", FALSE,
      "allow-file-access-from-file-urls", TRUE,
      NULL);
  }

  return settings;
}

typedef gboolean GetFormatFunc (EEditorSelection*);
typedef void     SetFormatFunc (EEditorSelection*, gboolean);

static void
biji_toggle_format (EEditorSelection *sel,
                    GetFormatFunc get_format,
                    SetFormatFunc set_format)
{
  set_format (sel, !get_format (sel));
}

static void
biji_toggle_block_format (BijiWebkitEditor *self,
                          BlockFormat block_format)
{
  /* insert commands toggle the formatting */
  switch (block_format)
  {
    case BLOCK_FORMAT_NONE:
      break;
    case BLOCK_FORMAT_UNORDERED_LIST:
      webkit_web_view_execute_editing_command (WEBKIT_WEB_VIEW (self), "insertUnorderedList");
      break;
    case BLOCK_FORMAT_ORDERED_LIST:
      webkit_web_view_execute_editing_command (WEBKIT_WEB_VIEW (self), "insertOrderedList");
      break;
    case BLOCK_FORMAT_INDENT:
      webkit_web_view_execute_editing_command (WEBKIT_WEB_VIEW (self), "indent");
      break;
    case BLOCK_FORMAT_OUTDENT:
      webkit_web_view_execute_editing_command (WEBKIT_WEB_VIEW (self), "outdent");
      break;
    default:
      g_assert_not_reached ();
  }
}

void
biji_webkit_editor_apply_format (BijiWebkitEditor *self, gint format)
{
  gboolean has_list = self->block_format == BLOCK_FORMAT_UNORDERED_LIST
                      || self-> block_format == BLOCK_FORMAT_ORDERED_LIST;

  switch (format)
  {
    case BIJI_BOLD:
      biji_toggle_format (self->sel, e_editor_selection_get_bold,
                                      e_editor_selection_set_bold);
      break;

    case BIJI_ITALIC:
      biji_toggle_format (self->sel, e_editor_selection_get_italic,
                                      e_editor_selection_set_italic);
      break;

    case BIJI_STRIKE:
      biji_toggle_format (self->sel, e_editor_selection_get_strike_through,
                                      e_editor_selection_set_strike_through);
      break;

    case BIJI_BULLET_LIST:
      biji_toggle_block_format (self, BLOCK_FORMAT_UNORDERED_LIST);
      break;

    case BIJI_ORDER_LIST:
      biji_toggle_block_format (self, BLOCK_FORMAT_ORDERED_LIST);
      break;

    case BIJI_INDENT:
      if (has_list)
        biji_toggle_block_format (self, BLOCK_FORMAT_INDENT);
      break;

    case BIJI_OUTDENT:
      if (has_list)
        biji_toggle_block_format (self, BLOCK_FORMAT_OUTDENT);
      break;

    default:
      g_warning ("biji_webkit_editor_apply_format : Invalid format");
  }
}

static gboolean
biji_webkit_editor_undo_cb (BijiWebkitEditor *self)
{
  webkit_web_view_execute_editing_command (WEBKIT_WEB_VIEW (self), WEBKIT_EDITING_COMMAND_UNDO);

  return GDK_EVENT_STOP;
}

static gboolean
biji_webkit_editor_redo_cb (BijiWebkitEditor *self)
{
  webkit_web_view_execute_editing_command (WEBKIT_WEB_VIEW (self), WEBKIT_EDITING_COMMAND_REDO);

  return GDK_EVENT_STOP;
}

void
biji_webkit_editor_set_font (BijiWebkitEditor *self, gchar *font)
{
  PangoFontDescription *font_desc;
  const gchar *family;

  /* parse : but we only parse font properties we'll be able
   * to transfer to webkit editor
   * Maybe is there a better way than webkitSettings,
   * eg applying format to the whole body */
  font_desc = pango_font_description_from_string (font);
  family = pango_font_description_get_family (font_desc);

  /* Set */
  g_object_set (biji_webkit_editor_get_web_settings (),
                "default-font-family", family,
                NULL);

  pango_font_description_free (font_desc);
}

void
biji_webkit_editor_set_text_size (BijiWebkitEditor *self,
                                  BjbTextSizeType   text_size)
{
  double zoom_level = ZOOM_MEDIUM;

  if (text_size == BJB_TEXT_SIZE_LARGE)
    {
      zoom_level = ZOOM_LARGE;
    }
  else if (text_size == BJB_TEXT_SIZE_SMALL)
    {
      zoom_level = ZOOM_SMALL;
    }

  webkit_web_view_set_zoom_level (WEBKIT_WEB_VIEW (self), zoom_level);
}

static void
biji_webkit_editor_init (BijiWebkitEditor *self)
{
}

static gboolean
on_navigation_request (WebKitWebView           *web_view,
                       WebKitPolicyDecision    *decision,
                       WebKitPolicyDecisionType decision_type,
                       gpointer                 user_data)
{
  WebKitNavigationPolicyDecision *navigation_decision;
  g_autoptr(GtkUriLauncher) launcher = NULL;
  WebKitNavigationAction *action;
  const char *requested_uri;
  GtkWidget *toplevel;

  if (decision_type != WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION)
    return FALSE;

  navigation_decision = WEBKIT_NAVIGATION_POLICY_DECISION (decision);
  action = webkit_navigation_policy_decision_get_navigation_action (navigation_decision);
  requested_uri = webkit_uri_request_get_uri (webkit_navigation_action_get_request (action));
  if (g_strcmp0 (webkit_web_view_get_uri (web_view), requested_uri) == 0)
    return FALSE;

  toplevel = gtk_widget_get_ancestor (GTK_WIDGET (web_view), GTK_TYPE_WINDOW);
  g_return_val_if_fail (GTK_IS_WINDOW (toplevel), FALSE);

  launcher = gtk_uri_launcher_new (requested_uri);
  gtk_uri_launcher_launch (launcher, GTK_WINDOW (toplevel), NULL, NULL, NULL);

  webkit_policy_decision_ignore (decision);
  return TRUE;
}

static void
on_load_change (WebKitWebView  *web_view,
                WebKitLoadEvent event)
{
  BijiWebkitEditor *self = BIJI_WEBKIT_EDITOR (web_view);

  if (event != WEBKIT_LOAD_FINISHED)
    return;

  self->load_finished = TRUE;
  if (self->has_color)
    biji_webkit_editor_set_color (self, &self->note_color);
}

static gboolean
on_context_menu (WebKitWebView       *web_view,
                 WebKitContextMenu   *context_menu,
                 GdkEvent            *event,
                 WebKitHitTestResult *hit_test_result,
                 gpointer             user_data)
{
  return TRUE;
}

static void
biji_webkit_editor_handle_contents_update (BijiWebkitEditor *self,
                                           JSCValue         *js_value)
{
  g_autoptr (JSCValue) js_outer_html = NULL;
  g_autoptr (JSCValue) js_inner_text = NULL;
  g_autofree gchar *html = NULL;
  g_autofree gchar *text = NULL;

  js_outer_html = jsc_value_object_get_property (js_value, "outerHTML");
  html = jsc_value_to_string (js_outer_html);
  if (!html)
    return;

  js_inner_text = jsc_value_object_get_property (js_value, "innerText");
  text = jsc_value_to_string (js_inner_text);
  if (!text)
    return;

  g_signal_emit (self, biji_editor_signals[CONTENT_CHANGED], 0, html, text);
}

static void
biji_webkit_editor_handle_selection_change (BijiWebkitEditor *self,
                                            JSCValue         *js_value)
{
  g_autoptr (JSCValue) js_block_format = NULL;
  g_autofree char *block_format_str = NULL;

  js_block_format = jsc_value_object_get_property (js_value, "blockFormat");
  block_format_str = jsc_value_to_string (js_block_format);
  if (g_strcmp0 (block_format_str, "UL") == 0)
    self->block_format = BLOCK_FORMAT_UNORDERED_LIST;
  else if (g_strcmp0 (block_format_str, "OL") == 0)
    self->block_format = BLOCK_FORMAT_ORDERED_LIST;
  else
    self->block_format = BLOCK_FORMAT_NONE;
}

static void
on_script_message (WebKitUserContentManager *user_content,
                   JSCValue *js_value,
                   BijiWebkitEditor *self)
{
  g_autoptr (JSCValue)  js_message_name = NULL;
  g_autofree char *message_name = NULL;

  g_assert (jsc_value_is_object (js_value));

  js_message_name = jsc_value_object_get_property (js_value, "messageName");
  message_name = jsc_value_to_string (js_message_name);
  if (g_strcmp0 (message_name, "ContentsUpdate") == 0)
    {
      if (self->first_load)
        self->first_load = FALSE;
      else
        biji_webkit_editor_handle_contents_update (self, js_value);
    }
  else if (g_strcmp0 (message_name, "SelectionChange") == 0)
    biji_webkit_editor_handle_selection_change (self, js_value);
}

static char *
get_empty_html (void)
{
  return g_strconcat ("<html xmlns=\"http://www.w3.org/1999/xhtml\">",
                      "<head>",
                      "<link rel=\"stylesheet\" href=\"Default.css\" type=\"text/css\"/>",
                      "<script language=\"javascript\" src=\"bijiben.js\"></script>"
                      "</head>",
                      "<body id=\"editable\">",
                      "</body></html>",
                      NULL);
}

static void
biji_webkit_editor_constructed (GObject *obj)
{
  BijiWebkitEditor *self;
  WebKitWebView *view;
  WebKitUserContentManager *user_content;

  self = BIJI_WEBKIT_EDITOR (obj);
  view = WEBKIT_WEB_VIEW (self);
  self->first_load = TRUE;

  G_OBJECT_CLASS (biji_webkit_editor_parent_class)->constructed (obj);

  user_content = webkit_web_view_get_user_content_manager (view);
  webkit_user_content_manager_register_script_message_handler (user_content, "bijiben", NULL);
  g_signal_connect (user_content, "script-message-received::bijiben",
                    G_CALLBACK (on_script_message), self);

  self->sel = e_editor_selection_new (view);

  /* Do not be a browser */
  g_signal_connect (view, "decide-policy",
                    G_CALLBACK (on_navigation_request), NULL);
  g_signal_connect (view, "load-changed",
                    G_CALLBACK (on_load_change), NULL);
  g_signal_connect (view, "context-menu",
                    G_CALLBACK (on_context_menu), NULL);
}

static void
biji_webkit_editor_class_init (BijiWebkitEditorClass *klass)
{
  GObjectClass* object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->constructed = biji_webkit_editor_constructed;

  biji_editor_signals[EDITOR_CLOSED] = g_signal_new ("closed",
                                       G_OBJECT_CLASS_TYPE (klass),
                                       G_SIGNAL_RUN_FIRST,
                                       0,
                                       NULL,
                                       NULL,
                                       g_cclosure_marshal_VOID__VOID,
                                       G_TYPE_NONE,
                                       0);
  biji_editor_signals[CONTENT_CHANGED] = g_signal_new ("content-changed",
                                         G_OBJECT_CLASS_TYPE (klass),
                                         G_SIGNAL_RUN_LAST,
                                         0,
                                         NULL,
                                         NULL,
                                         NULL,
                                         G_TYPE_NONE,
                                         2, G_TYPE_STRING, G_TYPE_STRING);

  gtk_widget_class_add_binding (widget_class, GDK_KEY_Z, GDK_CONTROL_MASK,
                                (GtkShortcutFunc) biji_webkit_editor_undo_cb, NULL);
  gtk_widget_class_add_binding (widget_class, GDK_KEY_Z, GDK_CONTROL_MASK | GDK_SHIFT_MASK,
                                (GtkShortcutFunc) biji_webkit_editor_redo_cb, NULL);
}

BijiWebkitEditor *
biji_webkit_editor_new (void)
{
  WebKitUserContentManager *manager = webkit_user_content_manager_new ();

  return g_object_new (BIJI_TYPE_WEBKIT_EDITOR,
                       "web-context", biji_webkit_editor_get_web_context (),
                       "settings", biji_webkit_editor_get_web_settings (),
                       "user-content-manager", manager,
                       NULL);

  g_object_unref (manager);
}

void
biji_webkit_editor_set_html (BijiWebkitEditor *self,
                             char             *html)
{
  g_autoptr(GBytes) html_data = NULL;

  g_return_if_fail (BIJI_IS_WEBKIT_EDITOR (self));

  self->load_finished = FALSE;
  self->has_color = FALSE;

  if (!html || !*html)
    html = get_empty_html ();

  html_data = g_bytes_new_take (html, strlen (html));
  webkit_web_view_load_bytes (WEBKIT_WEB_VIEW (self), html_data,
                              "application/xhtml+xml", NULL,
                              "file://" DATADIR G_DIR_SEPARATOR_S "bijiben" G_DIR_SEPARATOR_S);
}

void
biji_webkit_editor_set_color (BijiWebkitEditor *self,
                              GdkRGBA          *rgba)
{
  g_autofree char *script = NULL;

  g_return_if_fail (BIJI_IS_WEBKIT_EDITOR (self));

  if (!self->load_finished)
    {
      if (!gdk_rgba_equal (rgba, &self->note_color))
        self->note_color = *rgba;
      self->has_color = TRUE;
      return;
    }

  webkit_web_view_set_background_color (WEBKIT_WEB_VIEW (self), rgba);
  script = g_strdup_printf ("document.getElementById('editable').style.color = '%s';",
                            BJB_UTILS_COLOR_INTENSITY (rgba) < 0.5 ? "white" : "black");
  webkit_web_view_evaluate_javascript (WEBKIT_WEB_VIEW (self), script, -1,
                                       NULL, NULL, NULL, NULL, NULL);
  self->note_color = (GdkRGBA){};
  self->has_color = FALSE;
}
