#if HVM_WINDOWS
  #include <windows.h>
#else
  #include <dirent.h>
  #include <sys/stat.h>
#endif

fn void ffi_load(const char *path);

fn int ffi_is_path_sep(char ch) {
  return ch == '/' || ch == '\\';
}

fn char *ffi_path_join(const char *dir, const char *name) {
  if (dir == NULL) {
    dir = "";
  }
  if (name == NULL) {
    name = "";
  }

  size_t dir_len = strlen(dir);
  size_t name_len = strlen(name);
  int need_sep = dir_len > 0 && !ffi_is_path_sep(dir[dir_len - 1]);

  size_t out_len = dir_len + (size_t)need_sep + name_len + 1;
  char *out = malloc(out_len);
  if (out == NULL) {
    return NULL;
  }

  memcpy(out, dir, dir_len);
  size_t at = dir_len;
  if (need_sep) {
    out[at++] = PATH_SEP;
  }
  memcpy(out + at, name, name_len);
  out[at + name_len] = '\0';
  return out;
}

fn int ffi_is_shared_lib(const char *name) {
  size_t len = strlen(name);

#if HVM_WINDOWS
  if (len >= 4 && _stricmp(name + len - 4, ".dll") == 0) {
    return 1;
  }
#else
  if (len >= 6 && strcmp(name + len - 6, ".dylib") == 0) {
    return 1;
  }
  if (len >= 3 && strcmp(name + len - 3, ".so") == 0) {
    return 1;
  }
#endif

  return 0;
}

fn void ffi_load_dir(const char *dir) {
#if HVM_WINDOWS
  char *search_path = ffi_path_join(dir, "*");
  if (search_path == NULL) {
    fprintf(stderr, "ERROR: could not allocate FFI search path for '%s'\n", dir);
    exit(1);
  }

  WIN32_FIND_DATAA find_data;
  HANDLE find_handle = FindFirstFileA(search_path, &find_data);
  free(search_path);
  if (find_handle == INVALID_HANDLE_VALUE) {
    fprintf(stderr, "ERROR: could not open FFI dir '%s'\n", dir);
    exit(1);
  }

  do {
    const char *name = find_data.cFileName;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
      continue;
    }
    if (!ffi_is_shared_lib(name)) {
      continue;
    }
    if (find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
      continue;
    }

    char *path = ffi_path_join(dir, name);
    if (path == NULL) {
      FindClose(find_handle);
      fprintf(stderr, "ERROR: could not allocate FFI path for '%s\\%s'\n", dir, name);
      exit(1);
    }
    ffi_load(path);
    free(path);
  } while (FindNextFileA(find_handle, &find_data));

  FindClose(find_handle);
#else
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

    char *path = ffi_path_join(dir, name);
    if (path == NULL) {
      closedir(dp);
      fprintf(stderr, "ERROR: could not allocate FFI path for '%s/%s'\n", dir, name);
      exit(1);
    }

    struct stat st;
    if (stat(path, &st) != 0) {
      fprintf(stderr, "ERROR: could not stat FFI library '%s'\n", path);
      free(path);
      closedir(dp);
      exit(1);
    }
    if (!S_ISREG(st.st_mode)) {
      free(path);
      continue;
    }

    ffi_load(path);
    free(path);
  }

  closedir(dp);
#endif
}
