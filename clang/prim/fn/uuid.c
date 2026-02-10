#include <unistd.h>
#include <fcntl.h>

fn int uuid_fill_random(u8 out[16], int *err_out) {
  int fd = open("/dev/urandom", O_RDONLY);
  if (fd < 0) {
    if (err_out != NULL) {
      *err_out = errno;
    }
    return 0;
  }

  int got = 0;
  while (got < 16) {
    ssize_t n = read(fd, out + got, (size_t)(16 - got));
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      int err = errno;
      close(fd);
      if (err_out != NULL) {
        *err_out = err;
      }
      return 0;
    }
    if (n == 0) {
      close(fd);
      if (err_out != NULL) {
        *err_out = EIO;
      }
      return 0;
    }
    got += (int)n;
  }

  if (close(fd) != 0) {
    if (err_out != NULL) {
      *err_out = errno;
    }
    return 0;
  }

  if (err_out != NULL) {
    *err_out = 0;
  }
  return 1;
}

fn void uuid_v4_format(const u8 b[16], char out[37]) {
  static const char *HEX = "0123456789abcdef";
  int j = 0;

  for (int i = 0; i < 16; ++i) {
    if (i == 4 || i == 6 || i == 8 || i == 10) {
      out[j++] = '-';
    }
    out[j++] = HEX[(b[i] >> 4) & 0xF];
    out[j++] = HEX[b[i] & 0xF];
  }

  out[j] = 0;
}

// %uuid(dummy)
// ------------
// #OK{String} | #ERR{String}
fn Term prim_fn_uuid(Term *args) {
  (void)args[0];

  const char *RNG_ERR_FMT = "ERROR(uuid): failed to get secure random bytes: %s (errno=%d)";
  u8 bytes[16];
  int err = 0;
  if (!uuid_fill_random(bytes, &err)) {
    return term_new_ctr(NAM_ERR, 1, (Term[]){ term_string_printf(RNG_ERR_FMT, strerror(err), err) });
  }

  // RFC4122 variant + version 4 layout bits.
  bytes[6] = (u8)((bytes[6] & 0x0F) | 0x40);
  bytes[8] = (u8)((bytes[8] & 0x3F) | 0x80);

  char str[37];
  uuid_v4_format(bytes, str);
  Term out = term_string_from_utf8(str);
  return term_new_ctr(NAM_OK, 1, &out);
}

fn void prim_uuid_init(void) {
  prim_register("uuid", 4, 1, prim_fn_uuid);
}
