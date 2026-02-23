#if HVM_WINDOWS
  #include <windows.h>
#else
  #include <dirent.h>
  #include <sys/stat.h>
#endif

fn void ffi_load(const char *path);

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
  char search_path[MAX_PATH];
  snprintf(search_path, sizeof(search_path), "%s\\*", dir);

  WIN32_FIND_DATAA find_data;
  HANDLE find_handle = FindFirstFileA(search_path, &find_data);
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

    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s\\%s", dir, name);
    ffi_load(path);
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
#endif
}
