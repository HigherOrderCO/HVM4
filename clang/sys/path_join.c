#include <stdio.h>
#include <string.h>

#if HVM_WINDOWS
static int sys_path_is_abs(const char *path) {
  if (path == NULL || path[0] == '\0') {
    return 0;
  }

  // UNC path or root-relative path: \\server\share or \foo
  if (path[0] == '\\' || path[0] == '/') {
    return 1;
  }

  // Drive-letter absolute path: C:\foo or C:/foo
  if (((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')) &&
      path[1] == ':' &&
      (path[2] == '\\' || path[2] == '/')) {
    return 1;
  }

  return 0;
}
#else
static int sys_path_is_abs(const char *path) {
  return path != NULL && path[0] == '/';
}
#endif

fn void sys_path_join(char *out, int size, const char *base, const char *rel) {
  if (size <= 0) {
    return;
  }

  if (rel == NULL || rel[0] == '\0') {
    snprintf(out, size, "%s", base ? base : "");
    return;
  }

  if (sys_path_is_abs(rel)) {
    snprintf(out, size, "%s", rel);
    return;
  }

  if (base == NULL || base[0] == '\0') {
    snprintf(out, size, "%s", rel);
    return;
  }

  const char *slash = strrchr(base, '/');
  const char *backslash = strrchr(base, '\\');
  const char *sep = slash;

  if (backslash != NULL && (sep == NULL || backslash > sep)) {
    sep = backslash;
  }

#if HVM_WINDOWS
  const char join_sep = '\\';
#else
  const char join_sep = '/';
#endif

  if (sep != NULL) {
    snprintf(out, size, "%.*s%c%s", (int)(sep - base), base, join_sep, rel);
  } else {
    snprintf(out, size, "%s", rel);
  }
}
