/* GnomeIconLoaders - a loader for icon-themes
 * gnome-icon-loader.c Copyright (C) 2002 Red Hat, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#include <string.h>
#include <stdlib.h>
#include <glib.h>

#include "gnome-icon-loader.h"
#include "gnome-theme-parser.h"

/* general TODO:
 * Handle icon specific properties
 * Rescan/stat handling.
 */

typedef enum
{
  ICON_THEME_DIR_FIXED,  
  ICON_THEME_DIR_SCALABLE,  
  ICON_THEME_DIR_THRESHOLD,  
} IconThemeDirType;

/* In reverse search order: */
typedef enum
{
  ICON_SUFFIX_NONE = 0,
  ICON_SUFFIX_XPM,
  ICON_SUFFIX_SVG,
  ICON_SUFFIX_PNG,  
} IconSuffix;

struct _GnomeIconLoaderPrivate
{
  char *current_theme;
  char **search_path;
  int search_path_len;

  gboolean themes_valid;
  /* A list of all the themes needed to look up icons.
   * In search order, without duplicates
   */
  GList *themes;
  GHashTable *unthemed_icons;
};

typedef struct
{
  char *name;
  char *display_name;
  char *comment;
  char *example;

  /* In search order */
  GList *dirs;
} IconTheme;

typedef struct
{
  IconThemeDirType type;
  GQuark context;

  int size;
  int min_size;
  int max_size;
  int threshold;

  char *dir;
  
  GHashTable *icons;
  GHashTable *icon_data;
} IconThemeDir;

static void   gnome_icon_loader_class_init (GnomeIconLoaderClass *klass);
static void   gnome_icon_loader_init       (GnomeIconLoader      *loader);
static void   gnome_icon_loader_finalize   (GObject              *object);
static void   theme_destroy                (IconTheme            *theme);
static void   theme_dir_destroy            (IconThemeDir         *dir);
static char * theme_lookup_icon            (IconTheme            *theme,
					    const char           *icon_name,
					    int                   size,
					    const GnomeIconData **icon_data);
static void   theme_list_icons             (IconTheme            *theme,
					    GHashTable           *icons,
					    GQuark                context);
static void   theme_subdir_load            (GnomeIconLoader      *loader,
					    IconTheme            *theme,
					    GnomeThemeFile       *theme_file,
					    char                 *subdir);
static void   icon_data_destroy            (GnomeIconData             *data);


GType
gnome_icon_loader_get_type (void)
{
  static GType type = 0;

  if (type == 0)
    {
      static const GTypeInfo info =
	{
	  sizeof (GnomeIconLoaderClass),
	  NULL,           /* base_init */
	  NULL,           /* base_finalize */
	  (GClassInitFunc) gnome_icon_loader_class_init,
	  NULL,           /* class_finalize */
	  NULL,           /* class_data */
	  sizeof (GnomeIconLoader),
	  0,              /* n_preallocs */
	  (GInstanceInitFunc) gnome_icon_loader_init,
	};

      type = g_type_register_static (G_TYPE_OBJECT, "GnomeIconLoader", &info, 0);
    }

  return type;
}

GnomeIconLoader *
gnome_icon_loader_new (void)
{
  return g_object_new (GNOME_TYPE_ICON_LOADER, NULL);
}

static void
gnome_icon_loader_class_init (GnomeIconLoaderClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  gobject_class->finalize = gnome_icon_loader_finalize;
}

static void
gnome_icon_loader_init (GnomeIconLoader *loader)
{
  GnomeIconLoaderPrivate *priv;

  priv = g_new (GnomeIconLoaderPrivate, 1);
  
  loader->priv = priv;

  priv->current_theme = g_strdup ("default");
  priv->search_path = g_new (char *, 3);
  

  priv->search_path[0] = g_build_filename (g_get_home_dir (),
					     ".icons",
					     NULL);
  priv->search_path[1] = g_strdup ("/usr/share/icons");
  priv->search_path[2] = g_strdup ("/usr/share/pixmaps");
  priv->search_path_len = 3;

  priv->themes_valid = FALSE;
  priv->themes = NULL;
  priv->unthemed_icons = NULL;
}

static void
blow_themes (GnomeIconLoaderPrivate *priv)
{
  if (priv->themes_valid)
    {
      g_list_foreach (priv->themes, (GFunc)theme_destroy, NULL);
      g_list_free (priv->themes);
      g_hash_table_destroy (priv->unthemed_icons);
    }
  priv->themes = NULL;
  priv->unthemed_icons = NULL;
  priv->themes_valid = FALSE;
}

static void
gnome_icon_loader_finalize (GObject *object)
{
  GnomeIconLoader *loader;
  GnomeIconLoaderPrivate *priv;
  int i;

  loader = GNOME_ICON_LOADER (object);
  priv = loader->priv;

  g_free (priv->current_theme);

  for (i=0; i < priv->search_path_len; i++)
    g_free (priv->search_path[i]);

  g_free (priv->search_path);

  blow_themes (priv);

  g_free (priv);
}

void
gnome_icon_loader_set_search_path (GnomeIconLoader *loader,
				   const char *path[],
				   int         n_elements)
{
  GnomeIconLoaderPrivate *priv;
  int i;

  priv = loader->priv;
  for (i=0; i < priv->search_path_len; i++)
    g_free (priv->search_path[i]);

  g_free (priv->search_path);

  priv->search_path = g_new (char *, n_elements);
  priv->search_path_len = n_elements;
  for (i=0; i < priv->search_path_len; i++)
    priv->search_path[i] = g_strdup (path[i]);

  blow_themes (priv);
}

void
gnome_icon_loader_set_current_theme (GnomeIconLoader *loader,
				     const char *theme_name)
{
  GnomeIconLoaderPrivate *priv;

  priv = loader->priv;
  g_free (priv->current_theme);
  priv->current_theme = g_strdup (theme_name);

  blow_themes (priv);
}

static void
insert_theme (GnomeIconLoader *loader, const char *theme_name)
{
  int i;
  GList *l;
  char **dirs;
  GnomeIconLoaderPrivate *priv;
  IconTheme *theme;
  char *path;
  char *contents;
  char *directories;
  GnomeThemeFile *theme_file;
  
  priv = loader->priv;
  
  for (l = priv->themes; l != NULL; l = l->next)
    {
      theme = l->data;
      if (strcmp (theme->name, theme_name) == 0)
	return;
    }

  theme_file = NULL;
  for (i = 0; i < priv->search_path_len; i++)
    {
      path = g_build_filename (priv->search_path[i],
			       theme_name,
			       "index.theme",
			       NULL);
      if (g_file_test (path, G_FILE_TEST_IS_REGULAR)) {
	if (g_file_get_contents (path, &contents, NULL, NULL)) {
	  theme_file = gnome_theme_file_new_from_string (contents, NULL);
	  g_free (contents);
	  g_free (path);
	  break;
	}
      }
      g_free (path);
    }

  if (theme_file == NULL)
    return;
  
  theme = g_new (IconTheme, 1);
  if (!gnome_theme_file_get_locale_string (theme_file,
					   "Icon Theme",
					   "Name",
					   &theme->display_name))
    {
      g_warning ("Theme file for %s has no name\n", theme_name);
      g_free (theme);
      gnome_theme_file_free (theme_file);
      return;
    }

  if (!gnome_theme_file_get_string (theme_file,
				    "Icon Theme",
				    "Directories",
				    &directories))
    {
      g_warning ("Theme file for %s has no directories\n", theme_name);
      g_free (theme->display_name);
      g_free (theme);
      gnome_theme_file_free (theme_file);
      return;
    }
  
  theme->name = g_strdup (theme_name);
  gnome_theme_file_get_locale_string (theme_file,
				      "Icon Theme",
				      "Comment",
				      &theme->comment);
  gnome_theme_file_get_string (theme_file,
			       "Icon Theme",
			       "Example",
			       &theme->example);
  
  dirs = g_strsplit (directories, ",", 0);

  theme->dirs = NULL;
  for (i = 0; dirs[i] != NULL; i++)
      theme_subdir_load (loader, theme, theme_file, dirs[i]);
  
  g_strfreev (dirs);
  
  theme->dirs = g_list_reverse (theme->dirs);

  g_free (directories);
  
  gnome_theme_file_free (theme_file);
  
  /* Prepend the finished theme */
  priv->themes = g_list_prepend (priv->themes, theme);
}

static gboolean
my_g_str_has_suffix (const gchar  *str,
		     const gchar  *suffix)
{
  int str_len;
  int suffix_len;
  
  g_return_val_if_fail (str != NULL, FALSE);
  g_return_val_if_fail (suffix != NULL, FALSE);

  str_len = strlen (str);
  suffix_len = strlen (suffix);

  if (str_len < suffix_len)
    return FALSE;

  return strcmp (str + str_len - suffix_len, suffix) == 0;
}

static void
load_themes (GnomeIconLoader *loader)
{
  GnomeIconLoaderPrivate *priv;
  GDir *gdir;
  int base;
  char *dir, *base_name, *dot;
  const char *file;
  char *abs_file;
  
  priv = loader->priv;

  insert_theme (loader, priv->current_theme);

  priv->unthemed_icons = g_hash_table_new_full (g_str_hash, g_str_equal,
						g_free, g_free);

  for (base = 0; base < loader->priv->search_path_len; base++)
    {
      dir = loader->priv->search_path[base];
      gdir = g_dir_open (dir, 0, NULL);

      if (gdir == NULL)
	continue;
      
      while ((file = g_dir_read_name (gdir)))
	{
	  if (my_g_str_has_suffix (file, ".png") ||
	      my_g_str_has_suffix (file, ".svg") ||
	      my_g_str_has_suffix (file, ".xpm"))
	    {
	      abs_file = g_build_filename (dir, file, NULL);
	      if (g_file_test (abs_file, G_FILE_TEST_IS_REGULAR))
		{
		  base_name = g_strdup (file);
		  
		  dot = strrchr (base_name, '.');
		  if (dot)
		    *dot = 0;
		  
		  g_hash_table_replace (priv->unthemed_icons,
					base_name,
					abs_file);
		}
	      else
		g_free (abs_file);
	    }
	}
      g_dir_close (gdir);
    }


  priv->themes_valid = TRUE;
}

char *
gnome_icon_loader_lookup_icon (GnomeIconLoader      *loader,
			       const char           *icon_name,
			       int                   size,
			       const GnomeIconData **icon_data)
{
  GnomeIconLoaderPrivate *priv;
  GList *l;
  char *icon;
  
  priv = loader->priv;
  
  if (!priv->themes_valid)
    load_themes (loader);

  if (icon_data)
    *icon_data = NULL;
  
  l = priv->themes;
  while (l != NULL)
    {
      icon = theme_lookup_icon (l->data, icon_name, size, icon_data);
      if (icon)
	return icon;
      
      l = l->next;
    }

  icon = g_hash_table_lookup (priv->unthemed_icons, icon_name);
  if (icon)
    return g_strdup (icon);

  return NULL;
}

static void
add_key_to_hash (gpointer  key,
		 gpointer  value,
		 gpointer  user_data)
{
  GHashTable *hash = user_data;

  g_hash_table_insert (hash, key, NULL);
}

static void
add_key_to_list (gpointer  key,
		 gpointer  value,
		 gpointer  user_data)
{
  GList **list = user_data;

  *list = g_list_prepend (*list, key);
}


/* Don't free the returned names */
GList *
gnome_icon_loader_list_icons (GnomeIconLoader *loader,
			      const char *context)
{
  GnomeIconLoaderPrivate *priv;
  GHashTable *icons;
  GList *list, *l;
  GQuark context_quark;
  
  priv = loader->priv;
  
  if (!priv->themes_valid)
    load_themes (loader);

  if (context)
    {
      context_quark = g_quark_try_string (context);

      if (!context_quark)
	return NULL;
    }
  else
    context_quark = 0;

  icons = g_hash_table_new (g_str_hash, g_str_equal);
  
  l = priv->themes;
  while (l != NULL)
    {
      theme_list_icons (l->data, icons, context_quark);
      l = l->next;
    }

  if (context_quark == 0)
    g_hash_table_foreach (priv->unthemed_icons,
			  add_key_to_hash,
			  icons);

  list = 0;
  
  g_hash_table_foreach (icons,
			add_key_to_list,
			&list);

  g_hash_table_destroy (icons);
  
  return list;
}

char *
gnome_icon_loader_get_example_icon_name (GnomeIconLoader *loader)
{
  GnomeIconLoaderPrivate *priv;
  GList *l;
  IconTheme *theme;
  
  priv = loader->priv;
  
  if (!priv->themes_valid)
    load_themes (loader);

  l = priv->themes;
  while (l != NULL)
    {
      theme = l->data;
      if (theme->example)
	return g_strdup (theme->example);
      
      l = l->next;
    }
  
  return NULL;
}

gboolean
gnome_icon_loader_rescan_if_needed (GnomeIconLoader *loader)
{
  /* TODO */
  return FALSE;
}

static void
theme_destroy (IconTheme *theme)
{
  g_free (theme->display_name);
  g_free (theme->comment);
  g_free (theme->name);
  g_free (theme->example);

  g_list_foreach (theme->dirs, (GFunc)theme_dir_destroy, NULL);
  g_list_free (theme->dirs);
  g_free (theme);
}

static void
theme_dir_destroy (IconThemeDir *dir)
{
  g_hash_table_destroy (dir->icons);
  if (dir->icon_data)
    g_hash_table_destroy (dir->icon_data);
  g_free (dir->dir);
  g_free (dir);
}

static int
theme_dir_size_difference (IconThemeDir *dir, int size)
{
  int min, max;
  switch (dir->type)
    {
    case ICON_THEME_DIR_FIXED:
      return abs (size - dir->size);
      break;
    case ICON_THEME_DIR_SCALABLE:
      if (size < dir->min_size)
	return dir->min_size - size;
      if (size > dir->max_size)
	return size - dir->max_size;
      return 0;
      break;
    case ICON_THEME_DIR_THRESHOLD:
      min = dir->size - dir->threshold;
      max = dir->size + dir->threshold;
      if (size < min)
	return min - size;
      if (size > max)
	return size - max;
      return 0;
      break;
    }
  g_assert_not_reached ();
  return 1000;
}

static const char *
string_from_suffix (IconSuffix suffix)
{
  switch (suffix)
    {
    case ICON_SUFFIX_XPM:
      return ".xpm";
    case ICON_SUFFIX_SVG:
      return ".svg";
    case ICON_SUFFIX_PNG:
      return ".png";
    default:
      g_assert_not_reached();
    }
  return NULL;
}

static char *
theme_lookup_icon (IconTheme *theme,
		   const char *icon_name,
		   int size,
		   const GnomeIconData **icon_data)
{
  GList *l;
  IconThemeDir *dir, *min_dir;
  char *file, *absolute_file;
  int min_difference, difference;
  IconSuffix suffix;

  l = theme->dirs;
  while (l != NULL)
    {
      dir = l->data;

      if (theme_dir_size_difference (dir, size) == 0)
	{
	  suffix = GPOINTER_TO_INT (g_hash_table_lookup (dir->icons, icon_name));
	  if (suffix != ICON_SUFFIX_NONE) {
	    file = g_strconcat (icon_name, string_from_suffix (suffix), NULL);
	    absolute_file = g_build_filename (dir->dir, file, NULL);
	    g_free (file);

	    if (icon_data && dir->icon_data != NULL)
	      *icon_data = g_hash_table_lookup (dir->icon_data, icon_name);
	    
	    return absolute_file;
	  }
	}
      
      l = l->next;
    }

  min_difference = G_MAXINT;
  min_dir = NULL;
  l = theme->dirs;
  while (l != NULL)
    {
      dir = l->data;

      difference = theme_dir_size_difference (dir, size);
      if (difference < min_difference &&
	  g_hash_table_lookup (dir->icons, icon_name) != ICON_SUFFIX_NONE)
	{
	  min_difference = difference;
	  min_dir = dir;
	}
      
      l = l->next;
    }

  if (min_dir)
    {
      suffix = GPOINTER_TO_INT (g_hash_table_lookup (min_dir->icons, icon_name));
      file = g_strconcat (icon_name, string_from_suffix (suffix), NULL);
      absolute_file = g_build_filename (min_dir->dir, file, NULL);
      g_free (file);

      if (icon_data && min_dir->icon_data != NULL)
	*icon_data = g_hash_table_lookup (min_dir->icon_data, icon_name);
	    
      return absolute_file;
    }
 
  return NULL;
}

static void
theme_list_icons (IconTheme *theme, GHashTable *icons,
		  GQuark context)
{
  GList *l = theme->dirs;
  IconThemeDir *dir;
  
  while (l != NULL)
    {
      dir = l->data;

      if (context == dir->context ||
	  context == 0)
	g_hash_table_foreach (dir->icons,
			      add_key_to_hash,
			      icons);

      l = l->next;
    }
}

static void
load_icon_data (IconThemeDir *dir, const char *path, const char *name)
{
  GnomeThemeFile *icon_file;
  char *base_name;
  char **split;
  char *contents;
  char *dot;
  char *str;
  char *split_point;
  int i;
  
  GnomeIconData *data;

  if (g_file_get_contents (path, &contents, NULL, NULL))
    {
      icon_file = gnome_theme_file_new_from_string (contents, NULL);
      
      if (icon_file)
	{
	  base_name = g_strdup (name);
	  dot = strrchr (base_name, '.');
	  *dot = 0;
	  
	  data = g_new0 (GnomeIconData, 1);
	  g_hash_table_replace (dir->icon_data, base_name, data);
	  
	  if (gnome_theme_file_get_string (icon_file, "Icon Data",
					   "EmbeddedTextRectangle",
					   &str))
	    {
	      split = g_strsplit (str, ",", 4);
	      
	      i = 0;
	      while (split[i] != NULL)
		i++;

	      if (i==4)
		{
		  data->has_embedded_rect = TRUE;
		  data->x0 = atoi (split[0]);
		  data->y0 = atoi (split[1]);
		  data->x1 = atoi (split[2]);
		  data->y1 = atoi (split[3]);
		}

	      g_strfreev (split);
	      g_free (str);
	    }


	  if (gnome_theme_file_get_string (icon_file, "Icon Data",
					   "AttachPoints",
					   &str))
	    {
	      split = g_strsplit (str, "|", -1);
	      
	      i = 0;
	      while (split[i] != NULL)
		i++;

	      
	      data->attach_points = g_new0 (GnomeIconDataPoint, i);
	      data->n_attach_points = i;

	      i = 0;
	      while (split[i] != NULL)
		{
		  split_point = strchr (split[i], ',');
		  if (split_point)
		    {
		      *split_point = 0;
		      split_point++;
		      data->attach_points[i].x = atoi (split[i]);
		      data->attach_points[i].y = atoi (split_point);
		    }
		  i++;
		}
	      
	      g_strfreev (split);
	      g_free (str);
	    }

	  gnome_theme_file_free (icon_file);
	}
      g_free (contents);
    }
  
}

static void
scan_directory (IconThemeDir *dir, char *full_dir)
{
  GDir *gdir;
  const char *name;
  char *base_name, *dot;
  char *path;
  IconSuffix suffix, hash_suffix;
  
  dir->icons = g_hash_table_new_full (g_str_hash, g_str_equal,
				      g_free, NULL);
  
  gdir = g_dir_open (full_dir, 0, NULL);

  if (gdir == NULL)
    return;

  while ((name = g_dir_read_name (gdir)))
    {
      if (my_g_str_has_suffix (name, ".icon"))
	{
	  if (dir->icon_data == NULL)
	    dir->icon_data = g_hash_table_new_full (g_str_hash, g_str_equal,
						    g_free, (GDestroyNotify)icon_data_destroy);
	  
	  path = g_build_filename (full_dir, name, NULL);
	  if (g_file_test (path, G_FILE_TEST_IS_REGULAR))
	    load_icon_data (dir, path, name);
	  
	  g_free (path);
	  
	  continue;
	}
      
      if (my_g_str_has_suffix (name, ".png")) 
	suffix = ICON_SUFFIX_PNG;
      else if (my_g_str_has_suffix (name, ".svg")) 
	suffix = ICON_SUFFIX_SVG;
      else if (my_g_str_has_suffix (name, ".xpm"))
	suffix = ICON_SUFFIX_XPM;
      else
	continue;

      base_name = g_strdup (name);
      dot = strrchr (base_name, '.');
      *dot = 0;
      
      hash_suffix = GPOINTER_TO_INT (g_hash_table_lookup (dir->icons, base_name));

      if (suffix > hash_suffix)
	g_hash_table_replace (dir->icons, base_name, GINT_TO_POINTER (suffix));
      else
	g_free (base_name);
    }
  
  g_dir_close (gdir);
}

static void
theme_subdir_load (GnomeIconLoader *loader,
		   IconTheme *theme,
		   GnomeThemeFile *theme_file,
		   char *subdir)
{
  int base;
  char *type_string;
  IconThemeDir *dir;
  IconThemeDirType type;
  char *context_string;
  GQuark context;
  int size;
  int min_size;
  int max_size;
  int threshold;
  char *full_dir;

  if (!gnome_theme_file_get_integer (theme_file,
				     subdir,
				     "Size",
				     &size))
    {
      g_warning ("Theme directory %s of theme %s has no size field\n", subdir, theme->name);
      return;
    }
  
  type = ICON_THEME_DIR_THRESHOLD;
  if (gnome_theme_file_get_string (theme_file, subdir, "Type", &type_string))
    {
      if (strcmp (type_string, "Fixed") == 0)
	type = ICON_THEME_DIR_FIXED;
      else if (strcmp (type_string, "Scalable") == 0)
	type = ICON_THEME_DIR_SCALABLE;
      else if (strcmp (type_string, "Threshold") == 0)
	type = ICON_THEME_DIR_THRESHOLD;
    }

  context = 0;
  if (gnome_theme_file_get_string (theme_file, subdir, "Context", &context_string))
    {
      context = g_quark_from_string (context_string);
      g_free (context_string);
    }

  if (!gnome_theme_file_get_integer (theme_file,
				     subdir,
				     "MaxSize",
				     &max_size))
    max_size = size;
  
  if (!gnome_theme_file_get_integer (theme_file,
				     subdir,
				     "MinSize",
				     &min_size))
    min_size = size;
  
  if (!gnome_theme_file_get_integer (theme_file,
				     subdir,
				     "Threshold",
				     &threshold))
    threshold = 2;

  for (base = 0; base < loader->priv->search_path_len; base++)
    {
      full_dir = g_build_filename (loader->priv->search_path[base],
				   theme->name,
				   subdir,
				   NULL);
      if (g_file_test (full_dir, G_FILE_TEST_IS_DIR))
	{
	  dir = g_new (IconThemeDir, 1);
	  dir->type = type;
	  dir->context = context;
	  dir->size = size;
	  dir->min_size = min_size;
	  dir->max_size = max_size;
	  dir->threshold = threshold;
	  dir->dir = full_dir;
	  dir->icon_data = NULL;
	  
	  scan_directory (dir, full_dir);

	  theme->dirs = g_list_append (theme->dirs, dir);
	}
      else
	g_free (full_dir);
    }
}

static void
icon_data_destroy (GnomeIconData *data)
{
  g_free (data->attach_points);
  g_free (data);
}
