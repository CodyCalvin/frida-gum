/*
 * Copyright (C) 2016 Ole André Vadla Ravnås <oleavr@nowsecure.com>
 *
 * Licence: wxWindows Library Licence, Version 3.1
 */

#include "gumobjcapiresolver.h"

#include <dlfcn.h>
#include <gio/gio.h>
#include <objc/runtime.h>
#include <stdlib.h>

typedef struct _GumObjcClassMetadata GumObjcClassMetadata;

struct _GumObjcApiResolver
{
  GObject parent;

  GRegex * query_pattern;

  gboolean available;
  GHashTable * class_by_handle;

  gint (* objc_getClassList) (Class * buffer, gint class_count);
  Class (* class_getSuperclass) (Class klass);
  const gchar * (* class_getName) (Class klass);
  Method * (* class_copyMethodList) (Class klass, guint * method_count);
  Class (* object_getClass) (gpointer object);
  SEL (* method_getName) (Method method);
  IMP (* method_getImplementation) (Method method);
  const gchar * (* sel_getName) (SEL selector);
};

struct _GumObjcClassMetadata
{
  Class handle;
  const gchar * name;

  Method * class_methods;
  guint class_method_count;

  Method * instance_methods;
  guint instance_method_count;

  GSList * subclasses;

  GumObjcApiResolver * resolver;
};

static void gum_objc_api_resolver_iface_init (gpointer g_iface,
    gpointer iface_data);
static void gum_objc_api_resolver_finalize (GObject * object);
static void gum_objc_api_resolver_enumerate_matches (GumApiResolver * resolver,
    const gchar * query, GumFoundApiFunc func, gpointer user_data,
    GError ** error);
static gboolean gum_objc_api_resolver_enumerate_matches_for_class (
    GumObjcApiResolver * self, GumObjcClassMetadata * klass, gchar method_type,
    GPatternSpec * method_spec, GHashTable * visited_classes,
    GumFoundApiFunc func, gpointer user_data);

static gchar gum_method_type_from_match_info (GMatchInfo * match_info,
    gint match_num);
static GPatternSpec * gum_pattern_spec_from_match_info (GMatchInfo * match_info,
    gint match_num);

static GHashTable * gum_objc_api_resolver_create_snapshot (
    GumObjcApiResolver * resolver);

static void gum_objc_class_metadata_free (GumObjcClassMetadata * klass);
static const Method * gum_objc_class_metadata_get_methods (
    GumObjcClassMetadata * self, gchar type, guint * count);

G_DEFINE_TYPE_EXTENDED (GumObjcApiResolver,
                        gum_objc_api_resolver,
                        G_TYPE_OBJECT,
                        0,
                        G_IMPLEMENT_INTERFACE (GUM_TYPE_API_RESOLVER,
                            gum_objc_api_resolver_iface_init))

static void
gum_objc_api_resolver_class_init (GumObjcApiResolverClass * klass)
{
  GObjectClass * object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = gum_objc_api_resolver_finalize;
}

static void
gum_objc_api_resolver_iface_init (gpointer g_iface,
                                  gpointer iface_data)
{
  GumApiResolverIface * iface = (GumApiResolverIface *) g_iface;

  (void) iface_data;

  iface->enumerate_matches = gum_objc_api_resolver_enumerate_matches;
}

static void
gum_objc_api_resolver_init (GumObjcApiResolver * self)
{
  gpointer objc;

  self->query_pattern = g_regex_new ("([+*-])\\[(\\S+)\\s+(\\S+)\\]", 0, 0,
      NULL);

  objc = dlopen ("/usr/lib/libobjc.A.dylib", RTLD_LAZY | RTLD_GLOBAL | RTLD_NOLOAD);
  if (objc == NULL)
    goto beach;

#define GUM_TRY_ASSIGN_OBJC_FUNC(N) \
  self->N = dlsym (objc, G_STRINGIFY (N)); \
  if (self->N == NULL) \
    goto beach

  GUM_TRY_ASSIGN_OBJC_FUNC (objc_getClassList);
  GUM_TRY_ASSIGN_OBJC_FUNC (class_getSuperclass);
  GUM_TRY_ASSIGN_OBJC_FUNC (class_getName);
  GUM_TRY_ASSIGN_OBJC_FUNC (class_copyMethodList);
  GUM_TRY_ASSIGN_OBJC_FUNC (object_getClass);
  GUM_TRY_ASSIGN_OBJC_FUNC (method_getName);
  GUM_TRY_ASSIGN_OBJC_FUNC (method_getImplementation);
  GUM_TRY_ASSIGN_OBJC_FUNC (sel_getName);

  self->available = TRUE;
  self->class_by_handle = gum_objc_api_resolver_create_snapshot (self);

beach:
  if (objc != NULL)
    dlclose (objc);
}

static void
gum_objc_api_resolver_finalize (GObject * object)
{
  GumObjcApiResolver * self = GUM_OBJC_API_RESOLVER (object);

  g_clear_pointer (&self->class_by_handle, g_hash_table_unref);

  g_regex_unref (self->query_pattern);

  G_OBJECT_CLASS (gum_objc_api_resolver_parent_class)->finalize (object);
}

GumApiResolver *
gum_objc_api_resolver_new (void)
{
  GumObjcApiResolver * resolver;

  resolver = g_object_new (GUM_TYPE_OBJC_API_RESOLVER, NULL);
  if (!resolver->available)
  {
    g_object_unref (resolver);
    return NULL;
  }

  return GUM_API_RESOLVER (resolver);
}

static void
gum_objc_api_resolver_enumerate_matches (GumApiResolver * resolver,
                                         const gchar * query,
                                         GumFoundApiFunc func,
                                         gpointer user_data,
                                         GError ** error)
{
  GumObjcApiResolver * self = GUM_OBJC_API_RESOLVER (resolver);
  GMatchInfo * query_info;
  gchar method_type;
  GPatternSpec * class_spec, * method_spec;
  GHashTableIter iter;
  gboolean carry_on;
  GHashTable * visited_classes;
  GumObjcClassMetadata * klass;

  g_regex_match (self->query_pattern, query, 0, &query_info);
  if (!g_match_info_matches (query_info))
    goto invalid_query;

  method_type = gum_method_type_from_match_info (query_info, 1);
  class_spec = gum_pattern_spec_from_match_info (query_info, 2);
  method_spec = gum_pattern_spec_from_match_info (query_info, 3);

  g_match_info_free (query_info);

  g_hash_table_iter_init (&iter, self->class_by_handle);
  carry_on = TRUE;
  visited_classes = g_hash_table_new (NULL, NULL);
  while (carry_on && g_hash_table_iter_next (&iter, NULL, (gpointer *) &klass))
  {
    if (g_pattern_match_string (class_spec, klass->name))
    {
      carry_on = gum_objc_api_resolver_enumerate_matches_for_class (self, klass,
          method_type, method_spec, visited_classes, func, user_data);
    }
  }
  g_hash_table_unref (visited_classes);

  g_pattern_spec_free (method_spec);
  g_pattern_spec_free (class_spec);

  return;

invalid_query:
  {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
        "invalid query; format is: "
        "-[NS*Number foo:bar:], +[Foo foo*] or *[Bar baz]");
  }
}

static gboolean
gum_objc_api_resolver_enumerate_matches_for_class (GumObjcApiResolver * self,
                                                   GumObjcClassMetadata * klass,
                                                   gchar method_type,
                                                   GPatternSpec * method_spec,
                                                   GHashTable * visited_classes,
                                                   GumFoundApiFunc func,
                                                   gpointer user_data)
{
  const gchar all_method_types[3] = { '+', '-', '\0' };
  const gchar one_method_type[2] = { method_type, '\0' };
  const gchar * method_types, * t;
  gboolean carry_on;
  GSList * cur;

  if (g_hash_table_lookup (visited_classes, klass) != NULL)
    return TRUE;
  g_hash_table_add (visited_classes, klass);

  method_types = (method_type == '*') ? all_method_types : one_method_type;

  for (t = method_types; *t != '\0'; t++)
  {
    const Method * method_handles;
    guint method_count, method_index;
    const gchar prefix[3] = { *t, '[', '\0' };
    const gchar suffix[2] = { ']', '\0' };

    method_handles =
        gum_objc_class_metadata_get_methods (klass, *t, &method_count);
    for (method_index = 0; method_index != method_count; method_index++)
    {
      Method method_handle = method_handles[method_index];
      const gchar * method_name;

      method_name = self->sel_getName (self->method_getName (method_handle));
      if (g_pattern_match_string (method_spec, method_name))
      {
        GumApiDetails details;

        details.name =
            g_strconcat (prefix, klass->name, " ", method_name, suffix, NULL);
        details.address =
            GUM_ADDRESS (self->method_getImplementation (method_handle));

        carry_on = func (&details, user_data);

        g_free ((gpointer) details.name);

        if (!carry_on)
          return FALSE;
      }
    }
  }

  for (cur = klass->subclasses; cur != NULL; cur = cur->next)
  {
    Class subclass_handle = cur->data;
    GumObjcClassMetadata * subclass;

    subclass = g_hash_table_lookup (self->class_by_handle, subclass_handle);
    g_assert (subclass != NULL);

    carry_on = gum_objc_api_resolver_enumerate_matches_for_class (self,
        subclass, method_type, method_spec, visited_classes, func, user_data);
    if (!carry_on)
      return FALSE;
  }

  return TRUE;
}

static gchar
gum_method_type_from_match_info (GMatchInfo * match_info,
                                 gint match_num)
{
  gchar * type_str, type;

  type_str = g_match_info_fetch (match_info, match_num);
  type = type_str[0];
  g_free (type_str);

  return type;
}

static GPatternSpec *
gum_pattern_spec_from_match_info (GMatchInfo * match_info,
                                  gint match_num)
{
  gchar * pattern;
  GPatternSpec * spec;

  pattern = g_match_info_fetch (match_info, match_num);
  spec = g_pattern_spec_new (pattern);
  g_free (pattern);

  return spec;
}

static GHashTable *
gum_objc_api_resolver_create_snapshot (GumObjcApiResolver * self)
{
  GHashTable * class_by_handle;
  gint class_count, class_index;
  Class * classes;

  class_by_handle = g_hash_table_new_full (NULL, NULL, NULL,
      (GDestroyNotify) gum_objc_class_metadata_free);

  class_count = self->objc_getClassList (NULL, 0);
  classes = g_malloc (class_count * sizeof (Class));
  self->objc_getClassList (classes, class_count);

  for (class_index = 0; class_index != class_count; class_index++)
  {
    Class handle = classes[class_index];
    GumObjcClassMetadata * klass;

    klass = g_slice_new (GumObjcClassMetadata);
    klass->handle = handle;
    klass->name = self->class_getName (handle);
    klass->class_methods = NULL;
    klass->instance_methods = NULL;
    klass->subclasses = NULL;

    klass->resolver = self;

    g_hash_table_insert (class_by_handle, handle, klass);
  }

  for (class_index = 0; class_index != class_count; class_index++)
  {
    Class handle = classes[class_index];
    Class super_handle;

    super_handle = self->class_getSuperclass (handle);
    if (super_handle != NULL)
    {
      GumObjcClassMetadata * klass;

      klass = g_hash_table_lookup (class_by_handle, super_handle);
      klass->subclasses = g_slist_prepend (klass->subclasses, handle);
    }
  }

  g_free (classes);

  return class_by_handle;
}

static void
gum_objc_class_metadata_free (GumObjcClassMetadata * klass)
{
  g_slist_free (klass->subclasses);

  if (klass->instance_methods != NULL)
    free (klass->instance_methods);

  if (klass->class_methods != NULL)
    free (klass->class_methods);

  g_slice_free (GumObjcClassMetadata, klass);
}

static const Method *
gum_objc_class_metadata_get_methods (GumObjcClassMetadata * self,
                                     gchar type,
                                     guint * count)
{
  Method ** cached_methods;
  guint * cached_method_count;

  if (type == '+')
  {
    cached_methods = &self->class_methods;
    cached_method_count = &self->class_method_count;
  }
  else
  {
    cached_methods = &self->instance_methods;
    cached_method_count = &self->instance_method_count;
  }

  if (*cached_methods == NULL)
  {
    GumObjcApiResolver * resolver = self->resolver;

    *cached_methods = resolver->class_copyMethodList (
        (type == '+') ? resolver->object_getClass (self->handle) : self->handle,
        cached_method_count);
  }

  *count = *cached_method_count;

  return *cached_methods;
}
