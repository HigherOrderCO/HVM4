#include <dirent.h>
#include <sys/stat.h>

fn void ffi_load(const char *path);

fn int ffi_is_shared_lib(const char *name) {
  size_t len = strlen(name);
  if (len >= 6 && strcmp(name + len - 6, ".dylib") == 0) {
    return 1;
  }
  if (len >= 3 && strcmp(name + len - 3, ".so") == 0) {
    return 1;
  }
  return 0;
}

fn void ffi_load_dir(const char *dir) {
  DIR *dp = opendir(dir);
  if (dp == NULL) {
    fprintf(stderr, "ERROR: could not open FFI dir '%s'\n", dir);
    exit(1);
  }

  struct dirent *ent;
  while ((ent = readdir(dp)) != NULL) {
    const char *name = ent->d_name;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
      continue;
    }
    if (!ffi_is_shared_lib(name)) {
      continue;
    }

    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", dir, name);

    struct stat st;
    if (stat(path, &st) != 0) {
      fprintf(stderr, "ERROR: could not stat FFI library '%s'\n", path);
      exit(1);
    }
    if (!S_ISREG(st.st_mode)) {
      continue;
    }

    ffi_load(path);
  }

  closedir(dp);
}
