/*
 * Reading and writing JSON messages (see json.h).
 */
#include "lsp/json.h"

JsonNode *json_at(JsonNode *node, const char *path)
{
  char **parts = g_strsplit(path, ".", -1);

  for (int i = 0; node && parts[i]; i++) {
    if (JSON_NODE_HOLDS_OBJECT(node)) {
      JsonObject *object = json_node_get_object(node);
      node = json_object_has_member(object, parts[i]) ? json_object_get_member(object, parts[i]) : NULL;
    } else if (JSON_NODE_HOLDS_ARRAY(node) && g_ascii_isdigit(parts[i][0])) {
      JsonArray *array = json_node_get_array(node);
      guint index = (guint)g_ascii_strtoull(parts[i], NULL, 10);
      node = index < json_array_get_length(array) ? json_array_get_element(array, index) : NULL;
    } else {
      node = NULL;
    }
  }
  g_strfreev(parts);
  return node && !JSON_NODE_HOLDS_NULL(node) ? node : NULL;
}

const char *json_string_at(JsonNode *node, const char *path)
{
  node = json_at(node, path);
  return node && json_node_get_value_type(node) == G_TYPE_STRING ? json_node_get_string(node) : NULL;
}

gint64 json_int_at(JsonNode *node, const char *path, gint64 fallback)
{
  node = json_at(node, path);
  return node && JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) != G_TYPE_STRING
             ? json_node_get_int(node)
             : fallback;
}

char *json_quote(const char *text)
{
  JsonNode *node = json_node_init_string(json_node_alloc(), text);
  char *quoted = json_to_string(node, FALSE);

  json_node_unref(node);
  return quoted;
}
