/*
 * Copyright 2009 Intel Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU Lesser General Public License,
 * version 2.1, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for
 * more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St - Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Author: Thomas Wood <thos@gnome.org>
 *
 */
#include "mx-css.h"
#include <clutter/clutter.h>
#include <string.h>

#include <unistd.h>
#include <fcntl.h>

struct _MxStyleSheet
{
  GList *selectors;
  GList *styles;
  GList *filenames;
};

typedef struct _MxSelector MxSelector;
struct _MxSelector
{
  gchar *type;
  gchar *id;
  gchar *class;
  gchar *pseudo_class;
  MxSelector *parent;
  MxSelector *ancestor;
  GHashTable *style;
  const gchar *filename; /* origin of this selector */
  gint priority;
};


/* MxStyleSheetValue */

static MxStyleSheetValue *
mx_style_sheet_value_new ()
{
  return g_slice_new0 (MxStyleSheetValue);
}

static void
mx_style_sheet_value_free (MxStyleSheetValue *value)
{
  g_slice_free (MxStyleSheetValue, value);
}


static gchar*
append (gchar *str1, const gchar *str2)
{
  gchar *tmp;

  if (!str1)
      return g_strdup (str2);

  if (!str2)
      return str1;

  tmp = g_strconcat (str1, str2, NULL);
  g_free (str1);
  return tmp;
}

static gchar*
appendc (gchar *str, gchar c)
{
  gchar *tmp;
  gint len;

  if (str == NULL)
    {
      tmp = g_malloc (2);
      len = 0;
    }
  else
    {
      len = strlen (str);
      tmp = g_realloc (str, len + 2);
    }

  tmp[len] = c;
  tmp[len + 1] = '\0';

  return tmp;
}

static GTokenType
css_parse_key_value (GScanner *scanner, gchar **key, gchar **value)
{
  GTokenType token;
  gboolean start_with_dash = FALSE;
  gchar *id_first = scanner->config->cset_identifier_first;
  gchar *id_nth = scanner->config->cset_identifier_nth;
  guint scan_identifier_1char = scanner->config->scan_identifier_1char;

  /* parse property name */
  token = g_scanner_get_next_token (scanner);

  /* allow property names to start with '-' */
  if (token == '-')
    {
      /* FIXME: this will now ignore whitepsace and comments, but this should
       * not be allowed in the middle of a property name!
       */
      token = g_scanner_get_next_token (scanner);
      start_with_dash = TRUE;
    }

  if (token != G_TOKEN_IDENTIFIER)
    return G_TOKEN_IDENTIFIER;

  if (start_with_dash)
    *key = g_strconcat ("-", scanner->value.v_identifier, NULL);
  else
    *key = g_strdup (scanner->value.v_identifier);

  token = g_scanner_get_next_token (scanner);
  if (token != ':')
    return ':';

  /* value parsing options */
  scanner->config->cset_identifier_first = G_CSET_a_2_z "#_-0123456789"
    G_CSET_A_2_Z G_CSET_LATINS G_CSET_LATINC;
  scanner->config->cset_identifier_nth = scanner->config->cset_identifier_first;
  scanner->config->scan_identifier_1char = 1;
  scanner->config->char_2_token = FALSE;
  scanner->config->cset_skip_characters = "\n";

  /* parse value */
  while (scanner->next_value.v_char != ';')
    {
      token = g_scanner_get_next_token (scanner);
      switch (token)
        {
        case G_TOKEN_IDENTIFIER:
          *value = append (*value, scanner->value.v_identifier);
          break;
        case G_TOKEN_CHAR:
          *value = appendc (*value, scanner->value.v_char);
          break;

        default:
          return ';';
        }

      token = g_scanner_peek_next_token (scanner);
    }

  /* semi colon */
  token = g_scanner_get_next_token (scanner);
  if (scanner->value.v_char != ';')
    return ';';

  /* we've come to the end of the value, so reset the options */
  scanner->config->cset_identifier_nth = id_nth;
  scanner->config->cset_identifier_first = id_first;
  scanner->config->scan_identifier_1char = scan_identifier_1char;
  scanner->config->char_2_token = TRUE;
  scanner->config->cset_skip_characters = " \t\n";

  /* strip the leading and trailing whitespace */
  g_strstrip (*value);

  return G_TOKEN_NONE;
}

static GTokenType
css_parse_style (GScanner *scanner, GHashTable *table)
{
  GTokenType token;

  /* { */
  token = g_scanner_get_next_token (scanner);
  if (token != G_TOKEN_LEFT_CURLY)
    return G_TOKEN_LEFT_CURLY;

  /* keep going until we find '}' */
  token = g_scanner_peek_next_token (scanner);
  while (token != G_TOKEN_RIGHT_CURLY)
    {
      gchar *key = NULL, *value = NULL;

      token = css_parse_key_value (scanner, &key, &value);
      if (token != G_TOKEN_NONE)
        return token;

      g_hash_table_insert (table, key, value);

      token = g_scanner_peek_next_token (scanner);
    }

  /* } */
  token = g_scanner_get_next_token (scanner);
  if (token != G_TOKEN_RIGHT_CURLY)
    return G_TOKEN_RIGHT_CURLY;

  return G_TOKEN_NONE;
}


static GTokenType
css_parse_simple_selector (GScanner      *scanner,
                           MxSelector    *selector)
{
  guint token;
  gchar *tmp;

  /* parse optional type (either '*' or an identifier) */
  token = g_scanner_peek_next_token (scanner);
  switch (token)
    {
    case '*':
      token = g_scanner_get_next_token (scanner);
      selector->type = g_strdup ("*");
      break;
    case G_TOKEN_IDENTIFIER:
      token = g_scanner_get_next_token (scanner);
      selector->type = g_strdup (scanner->value.v_identifier);
      break;
    default:
      break;
    }

  /* Here we look for '#', '.' or ':' and return if we find anything else */
  token = g_scanner_peek_next_token (scanner);
  while (token != G_TOKEN_NONE)
    {
      switch (token)
        {
          /* id */
        case '#':
          token = g_scanner_get_next_token (scanner);
          token = g_scanner_get_next_token (scanner);
          if (token != G_TOKEN_IDENTIFIER)
            return G_TOKEN_IDENTIFIER;
          selector->id = g_strdup (scanner->value.v_identifier);
          break;
          /* class */
        case '.':
          token = g_scanner_get_next_token (scanner);
          token = g_scanner_get_next_token (scanner);
          if (token != G_TOKEN_IDENTIFIER)
            return G_TOKEN_IDENTIFIER;
          selector->class = g_strdup (scanner->value.v_identifier);
          break;
          /* pseudo-class */
        case ':':
          token = g_scanner_get_next_token (scanner);
          token = g_scanner_get_next_token (scanner);
          if (token != G_TOKEN_IDENTIFIER)
            return G_TOKEN_IDENTIFIER;

          tmp = selector->pseudo_class;

          if (selector->pseudo_class)
            selector->pseudo_class = g_strconcat (selector->pseudo_class, ":",
                                                  scanner->value.v_identifier,
                                                  NULL);
          else
            selector->pseudo_class = g_strdup (scanner->value.v_identifier);

          g_free (tmp);

          break;

          /* unhandled */
        default:
          return G_TOKEN_NONE;
          break;
        }
      token = g_scanner_peek_next_token (scanner);
    }
  return G_TOKEN_NONE;
}

#ifdef MX_DEBUG_CSS
static void
print_selector (MxSelector *selector, gint indent)
{
  int i;

  for (i = 0; i < indent; i++)
      printf ("  ");

  printf ("%s.%s#%s:%s\n", selector->type, selector->class, selector->id,
          selector->pseudo_class);

  if (selector->parent)
    {
      for (i = 0; i < indent; i++)
        printf ("  ");
      printf ("p");
      print_selector (selector->parent, indent + 1);
    }

  if (selector->ancestor)
    {
      for (i = 0; i < indent; i++)
        printf ("  ");
      printf ("a");
      print_selector (selector->ancestor, indent + 1);
    }
}
#endif


static MxSelector *
mx_selector_new (const gchar *filename, gint priority)
{
  MxSelector *s;

  s = g_slice_new0 (MxSelector);
  s->filename = filename;
  s->priority = priority;

  return s;
}

static void
mx_selector_free (MxSelector *selector)
{
  if (!selector)
    return;

  g_free (selector->type);
  g_free (selector->id);
  g_free (selector->class);
  g_free (selector->pseudo_class);

  mx_selector_free (selector->parent);

  g_slice_free (MxSelector, selector);
}

static GTokenType
css_parse_ruleset (GScanner *scanner, GList **selectors)
{
  guint token;
  MxSelector *selector, *parent;

  /* parse the first selector, then keep going until we find left curly */
  token = g_scanner_peek_next_token (scanner);

  parent = NULL;
  selector = NULL;
  while (token != G_TOKEN_LEFT_CURLY)
    {
      switch (token)
        {
        case G_TOKEN_IDENTIFIER:
        case '*':
        case '#':
        case '.':
        case ':':

          if (selector)
            parent = selector;
          else
            parent = NULL;

          /* check if there was a previous selector and if so, the new one
           * should use the previous selector to match an ancestor */

          selector = mx_selector_new (scanner->input_name,
                                      GPOINTER_TO_INT (scanner->user_data));
          *selectors = g_list_prepend (*selectors, selector);

          if (parent)
            {
              *selectors = g_list_remove (*selectors, parent);
              selector->ancestor = parent;
            }

          token = css_parse_simple_selector (scanner, selector);
          if (token != G_TOKEN_NONE)
            return token;

          break;

        case '>':
          g_scanner_get_next_token (scanner);
          if (!selector)
            {
              g_warning ("NULL parent when parsing '>'");
            }

          parent = selector;

          selector = mx_selector_new (scanner->input_name,
                                      GPOINTER_TO_INT (scanner->user_data));
          *selectors = g_list_prepend (*selectors, selector);

          /* remove parent from list of selectors and link it to the new
           * selector */
          selector->parent = parent;
          *selectors = g_list_remove (*selectors, parent);

          token = css_parse_simple_selector (scanner, selector);
          if (token != G_TOKEN_NONE)
            return token;

          break;

        case ',':
          g_scanner_get_next_token (scanner);

          selector = mx_selector_new (scanner->input_name,
                                      GPOINTER_TO_INT (scanner->user_data));
          *selectors = g_list_prepend (*selectors, selector);

          token = css_parse_simple_selector (scanner, selector);

          if (token != G_TOKEN_NONE)
            return token;

          break;

        default:
          g_scanner_get_next_token (scanner);
          g_scanner_unexp_token (scanner, G_TOKEN_ERROR, NULL, NULL, NULL,
                                 "Unhandled selector", 1);
          return '{';
        }
      token = g_scanner_peek_next_token (scanner);
    }

  return G_TOKEN_NONE;
}

static GTokenType
css_parse_block (GScanner *scanner, GList **selectors, GList **styles)
{
  GTokenType token;
  GHashTable *table;
  GList *l, *list = NULL;


  token = css_parse_ruleset (scanner, &list);
  if (token != G_TOKEN_NONE)
    return token;


  /* create a hash table for the properties */
  table = g_hash_table_new_full (g_str_hash, g_direct_equal, g_free,
                                 (GDestroyNotify) mx_style_sheet_value_free);

  token = css_parse_style (scanner, table);

  /* assign all the selectors to this style */
  for (l = list; l; l = l->next)
    {
      MxSelector* sl;

      sl = (MxSelector*) l->data;

      sl->style = table;
    }

  *styles = g_list_append (*styles, table);

  *selectors = g_list_concat (*selectors, list);

  return token;
}


static gboolean
css_parse_file (MxStyleSheet *sheet,
                gchar        *filename,
                gint          priority)
{
  GScanner *scanner;
  int fd;
  GTokenType token;

  fd = open (filename, O_RDONLY);
  if (fd == -1)
    return FALSE;

  scanner = g_scanner_new (NULL);
  scanner->input_name = filename;
  scanner->user_data = GINT_TO_POINTER (priority);

  /* turn off single line comments, we need to parse '#' */
  scanner->config->cpair_comment_single = "\1\n";
  scanner->config->cset_identifier_nth = G_CSET_a_2_z "-_0123456789"
    G_CSET_A_2_Z G_CSET_LATINS G_CSET_LATINC;
  scanner->config->scan_float = FALSE; /* allows scanning '.' */
  scanner->config->scan_hex = FALSE;
  scanner->config->scan_string_sq = FALSE;
  scanner->config->scan_string_dq = FALSE;

  g_scanner_input_file (scanner, fd);


  token = g_scanner_peek_next_token (scanner);
  while (token != G_TOKEN_EOF)
    {
      token = css_parse_block (scanner, &sheet->selectors,
                               &sheet->styles);
      if (token != G_TOKEN_NONE)
        break;

      token = g_scanner_peek_next_token (scanner);
    }

  if (token != G_TOKEN_EOF)
    g_scanner_unexp_token (scanner, token, NULL, NULL, NULL, "Error",
                           TRUE);

  close (fd);
  g_scanner_destroy (scanner);

  if (token == G_TOKEN_EOF)
    return TRUE;
  else
    return FALSE;
}

static gint
css_node_matches_selector (MxSelector *selector,
                           MxStylable *stylable)
{
  gint score;
  gint a, b, c;

  const gchar *type = "*";
  const gchar *class, *pseudo_class;
  const gchar *id;
  ClutterActor *actor;
  MxStylable *parent;

  score = 0;
  a = 0;
  b = 0;
  c = 0;

  /* get properties for this stylable */
  id = clutter_actor_get_name (CLUTTER_ACTOR (stylable));
  class = mx_stylable_get_style_class (stylable);
  pseudo_class = mx_stylable_get_style_pseudo_class (stylable);

  /* check type */
  if (selector->type == NULL || selector->type[0] == '*')
    {
      /* NULL or universal selector match, but are ignored for score */
    }
  else
    {
      GType type_id;
      gint matched;
      gint depth;

      type_id = G_OBJECT_CLASS_TYPE (G_OBJECT_GET_CLASS (stylable));
      type = g_type_name (type_id);
      matched = FALSE;

      depth = 10;
      while (type)
        {
          if (!strcmp (selector->type, type))
            {
              matched = depth;
              break;
            }
          else
            {
              type_id = g_type_parent (type_id);
              type = g_type_name (type_id);
              if (depth > 1)
                depth--;
            }
        }

      if (!matched)
        return -1;
      else
        c += depth;
    }

  /* check id */
  if (selector->id)
    {
      if (!id || strcmp (selector->id, id))
        return -1;
      else
        a += 10;
    }

  /* check pseudo_class */
  if (selector->pseudo_class)
    {
      gchar **pseudo_classes = NULL;
      gint i, len;

      /* if no pseudo class is supplied on the node, return instantly */
      if (!pseudo_class)
        return -1;

      /* an exact match is a common case */
      if (!strstr (selector->pseudo_class, ":")
          && !strcmp (selector->pseudo_class, pseudo_class))
        b += 10;
      else
        {
          gint n_matches = 0;

          /* check each pseudo class from the selector occurs on the node */
          pseudo_classes = g_strsplit (selector->pseudo_class, ":", -1);

          len = g_strv_length (pseudo_classes);
          for (i = 0; i < len; i++)
            {
              /* Increase the 'b' score if this pseudo class is in the
               * node. */
              if (strstr (pseudo_class, pseudo_classes[i]))
                n_matches++;
            }

          if (n_matches != len)
            return -1;
          else
            b = b + (10 * n_matches);
        }

      g_strfreev (pseudo_classes);
    }

  /* check class */
  if (selector->class)
    {
      if (!class || strcmp (selector->class, class))
        return -1;
      else
        b += 10;
    }

  /* check parent */
  actor = clutter_actor_get_parent (CLUTTER_ACTOR (stylable));
  if (MX_IS_STYLABLE (actor))
    parent = MX_STYLABLE (actor);
  else
    parent = NULL;

  if (selector->parent)
    {
      gint parent_matches;

      if (!parent)
        return -1;

      parent_matches = css_node_matches_selector (selector->parent, parent);
      if (parent_matches < 0)
        return -1;

      /* increase the 'c' score, since the parent matched */
      c += parent_matches;
    }

  /* check ancestor */
  if (selector->ancestor)
    {
      gint ancestor_matches;
      MxStylable *pparent, *ancestor;
      ClutterActor *parent_actor;

      if (!parent)
        return -1;

      ancestor = parent;
      while (ancestor)
        {
          parent_actor = clutter_actor_get_parent (CLUTTER_ACTOR (ancestor));
          if (MX_IS_STYLABLE (parent_actor))
            pparent = MX_STYLABLE (parent_actor);
          else
            pparent = NULL;


          ancestor_matches = css_node_matches_selector (selector->ancestor,
                                                        ancestor);

          /* if one of the ancestors match, stop search and increase 'c' score
           */
          if (ancestor_matches >= 0)
            {
              c += ancestor_matches;
              break;
            }

          ancestor = pparent;
          if (!ancestor || !MX_IS_STYLABLE (ancestor))
            return -1;
        }
    }


  a = a * 10000;
  b = b * 100;

  score = a + b + c;

  return score;
}

typedef struct _SelectorMatch
{
  MxSelector *selector;
  gint score;
} SelectorMatch;

static gint
compare_selector_matches (SelectorMatch *a,
                          SelectorMatch *b)
{
  gint priority = a->selector->priority - b->selector->priority;
  return priority ? priority : (a->score - b->score);
}

struct _css_table_copy_data
{
  GHashTable *table;
  const gchar *filename;
};

static void
css_table_copy (gpointer                    *key,
                gpointer                    *value,
                struct _css_table_copy_data *data)
{
  MxStyleSheetValue *css_value;

  css_value = mx_style_sheet_value_new ();
  css_value->source = data->filename;
  css_value->string = (gchar*) value;

  g_hash_table_insert (data->table, key, css_value);
}

static void
free_selector_match (SelectorMatch *data)
{
  g_slice_free (SelectorMatch, data);
}

GHashTable *
mx_style_sheet_get_properties (MxStyleSheet *sheet,
                               MxStylable   *node)
{
  GList *l, *matching_selectors = NULL;
  SelectorMatch *selector_match = NULL;
  GHashTable *result;

  /* find matching selectors */
#ifdef MX_DEBUG_CSS
  GTimer *timer = g_timer_new ();
#endif
  for (l = sheet->selectors; l; l = l->next)
    {
      gint score;

      score = css_node_matches_selector (l->data, node);

      if (score >= 0)
        {
          selector_match = g_slice_new (SelectorMatch);
          selector_match->selector = l->data;
          selector_match->score = score;
          matching_selectors = g_list_prepend (matching_selectors,
                                               selector_match);
        }
    }

  /* score the selectors by their score */
  matching_selectors = g_list_sort (matching_selectors,
                                    (GCompareFunc) compare_selector_matches);

  /* get properties from selector's styles */
  result = g_hash_table_new_full (g_str_hash,
                                  g_str_equal,
                                  NULL,
                                  (GDestroyNotify)mx_style_sheet_value_free);
  for (l = matching_selectors; l; l = l->next)
    {
      SelectorMatch *match = l->data;
      struct _css_table_copy_data copy_data;

      copy_data.filename = match->selector->filename;
      copy_data.table = result;

      g_hash_table_foreach (match->selector->style, (GHFunc) css_table_copy,
                            &copy_data);

#ifdef MX_DEBUG_CSS
          printf ("(%6d) ", match->score);
          print_selector (match->selector, 3);
#endif
    }

  g_list_foreach (matching_selectors, (GFunc) free_selector_match, NULL);
  g_list_free (matching_selectors);

#ifdef MX_DEBUG_CSS

  g_timer_stop (timer);
  static int count = 1;
  static double avg = 0, new;
  new = g_timer_elapsed (timer, NULL);
  avg = ((avg * count + new) / (count + 1));
  count++;
  printf ("%f - finished (total: %d, average: %f) %s \n",
          new, count, avg, G_OBJECT_CLASS_NAME (G_OBJECT_GET_CLASS (node)));
  printf ("----\n");
#endif
  return result;
}

MxStyleSheet *
mx_style_sheet_new ()
{
  return g_new0 (MxStyleSheet, 1);
}

void
mx_style_sheet_destroy (MxStyleSheet *sheet)
{
  g_list_foreach (sheet->selectors, (GFunc) mx_selector_free, NULL);
  g_list_free (sheet->selectors);

  g_list_foreach (sheet->styles, (GFunc) g_hash_table_destroy, NULL);
  g_list_free (sheet->styles);

  g_list_foreach (sheet->filenames, (GFunc) g_free, NULL);
  g_list_free (sheet->filenames);

  g_free (sheet);
}

gboolean
mx_style_sheet_add_from_file (MxStyleSheet *sheet,
                              const gchar  *filename,
                              GError       **error)
{
  gboolean result;
  gchar *input_name;

  g_return_val_if_fail (sheet != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error != NULL, FALSE);
  g_return_val_if_fail (filename != NULL, FALSE);

  input_name = g_strdup (filename);
  result = css_parse_file (sheet, input_name, g_list_length (sheet->filenames));
  sheet->filenames = g_list_prepend (sheet->filenames, input_name);

#ifdef MX_DEBUG_CSS
  g_list_foreach (sheet->selectors, (GFunc)print_selector, GINT_TO_POINTER (0));
  printf ("-------\n");
#endif

  return result;
}
