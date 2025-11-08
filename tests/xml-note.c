/* plain-note.c
 *
 * Copyright 2025 Mohammed Sadiq <sadiq@sadiqpk.org>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#undef NDEBUG
#undef G_DISABLE_ASSERT
#undef G_DISABLE_CHECKS
#undef G_DISABLE_CAST_CHECKS
#undef G_LOG_DOMAIN

#include <libedataserver/libedataserver.h>

#include "items/bjb-xml-note.h"
#include "bjb-log.h"

static void
test_xml_note_empty (void)
{
  g_autoptr(BjbItem) note = NULL;
  g_autoptr(BjbTagStore) tag_store = NULL;
  g_autofree char *content = NULL;

  tag_store = bjb_tag_store_new ();
  note = bjb_xml_note_new_from_data (NULL, tag_store);
  g_assert (BJB_IS_XML_NOTE (note));

  g_assert_cmpstr (bjb_item_get_title (note), ==, "");
  content = bjb_note_get_text_content (BJB_NOTE (note));
  g_assert_cmpstr (content, ==, NULL);

  bjb_note_set_text_content (BJB_NOTE (note), "text content");
  content = bjb_note_get_text_content (BJB_NOTE (note));
  g_assert_cmpstr (content, ==, "text content");
}

static void
compare_xml (const char *expected_file,
             const char *xml_str)
{
  g_autofree char *expected_xml_str = NULL;
  g_autoptr(GError) error = NULL;
  xmlChar *new_xml, *new_expected_xml;
  xmlDoc *xml, *expected_xml;

  g_file_get_contents (expected_file, &expected_xml_str, NULL, &error);
  g_assert_no_error (error);

  xml = e_xml_parse_data (xml_str, strlen (xml_str));
  expected_xml = e_xml_parse_data (expected_xml_str, strlen (expected_xml_str));

  xmlDocDumpMemory (xml, &new_xml, NULL);
  xmlDocDumpMemory (expected_xml, &new_expected_xml, NULL);
  g_assert_cmpstr ((char *)new_xml, ==, (char *)new_expected_xml);

  xmlFreeDoc (xml);
  xmlFreeDoc (expected_xml);
  xmlFree (new_xml);
  xmlFree (new_expected_xml);
}

static void
test_tomboy_note_parse (gconstpointer user_data)
{
  g_autoptr(BjbTagStore) store = NULL;
  g_autofree char *generated_html = NULL;
  g_autofree char *expected_html = NULL;
  g_autofree char *expected_file = NULL;
  const char *tomboy_file = user_data;
  const char *in_title, *expected_title;
  BjbItem *tomboy_note, *expected_note;
  char *xml_content;

  store = bjb_tag_store_new ();
  tomboy_note = bjb_xml_note_new_from_data (tomboy_file, store);
  xml_content = bjb_note_get_xml (BJB_NOTE (tomboy_note));
  g_assert_finalize_object (tomboy_note);
  tomboy_note = bjb_xml_note_new_from_xml (xml_content, store);

  expected_file = g_strdup_printf ("%s.expected.note", tomboy_file);
  expected_note = bjb_xml_note_new_from_data (expected_file, store);
  xml_content = bjb_note_get_xml (BJB_NOTE (expected_note));
  /* Check if the populated XML is same as the content in the file */
  compare_xml (expected_file, xml_content);
  g_assert_finalize_object (expected_note);
  expected_note = bjb_xml_note_new_from_xml (xml_content, store);

  generated_html = bjb_note_get_html (BJB_NOTE (tomboy_note));
  expected_html = bjb_note_get_html (BJB_NOTE (expected_note));
  g_assert_cmpstr (generated_html, ==, expected_html);

  in_title = bjb_item_get_title (tomboy_note);
  expected_title = bjb_item_get_title (expected_note);
  g_assert_cmpstr (in_title, ==, expected_title);

  /* todo: test metadata */
  /* to be implemeneted */

  g_assert_finalize_object (tomboy_note);
  g_assert_finalize_object (expected_note);
}

static void
create_test (const char *path,
             const char *test_prefix)
{
  g_autoptr(GPtrArray) files = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(GDir) dir = NULL;
  const char *file_name;

  files = g_ptr_array_new ();
  dir = g_dir_open (path, 0, &error);
  g_assert_no_error (error);

  while ((file_name = g_dir_read_name (dir)) != NULL)
    {
      if (!strstr (file_name, "expected"))
        g_ptr_array_add (files, (gpointer)file_name);
    }

  g_ptr_array_sort_values (files, (GCompareFunc)g_strcmp0);

  for (guint i = 0; i < files->len; i++)
    {
      g_autofree char *test_path = NULL;

      file_name = files->pdata[i];
      test_path = g_strdup_printf ("/%s-import/%s", test_prefix, file_name);
      g_test_add_data_func_full (test_path,
                                 g_build_filename (path, file_name, NULL),
                                 test_tomboy_note_parse,
                                 g_free);
    }
}

int
main (int   argc,
      char *argv[])
{
  char *path;

  e_xml_initialize_in_main ();
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/xml/empty", test_xml_note_empty);

  path = g_test_build_filename (G_TEST_DIST, "notes", "tomboy", NULL);
  create_test (path, "tomboy");
  g_free (path);

 return g_test_run ();
}
