// minimal duktape runner for testing app plugins: dukrun file1.js [file2.js ...]
// - files are compiled and run in order in one heap, as PluginManager::reload() does
// - provides print(), console.log(), readFile(path), getenv(name); app API stubs are defined in JS (app-stubs.js)
#include <stdio.h>
#include <stdlib.h>
#include "duktape.h"

static char* slurp(const char* path, size_t* len)
{
  FILE* f = fopen(path, "rb");
  if(!f) return NULL;
  fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
  char* buf = malloc(n + 1);
  *len = fread(buf, 1, n, f); buf[*len] = '\0';
  fclose(f);
  return buf;
}

static duk_ret_t print(duk_context* ctx)
{
  duk_push_string(ctx, " ");
  duk_insert(ctx, 0);
  duk_join(ctx, duk_get_top(ctx) - 1);
  printf("%s\n", duk_safe_to_string(ctx, -1));
  return 0;
}

static duk_ret_t readFile(duk_context* ctx)
{
  size_t len;
  char* buf = slurp(duk_require_string(ctx, 0), &len);
  if(!buf) return duk_error(ctx, DUK_ERR_ERROR, "cannot read %s", duk_get_string(ctx, 0));
  duk_push_lstring(ctx, buf, len);
  free(buf);
  return 1;
}

static duk_ret_t getEnv(duk_context* ctx)
{
  const char* val = getenv(duk_require_string(ctx, 0));
  if(val) duk_push_string(ctx, val); else duk_push_undefined(ctx);
  return 1;
}

int main(int argc, char* argv[])
{
  duk_context* ctx = duk_create_heap_default();
  duk_push_c_function(ctx, print, DUK_VARARGS);
  duk_put_global_string(ctx, "print");
  duk_push_object(ctx);
  duk_push_c_function(ctx, print, DUK_VARARGS);
  duk_put_prop_string(ctx, -2, "log");
  duk_put_global_string(ctx, "console");
  duk_push_c_function(ctx, readFile, 1);
  duk_put_global_string(ctx, "readFile");
  duk_push_c_function(ctx, getEnv, 1);
  duk_put_global_string(ctx, "getenv");

  int err = 0;
  for(int ii = 1; ii < argc && !err; ++ii) {
    size_t len;
    char* js = slurp(argv[ii], &len);
    if(!js) { fprintf(stderr, "cannot read %s\n", argv[ii]); err = 1; break; }
    duk_push_string(ctx, argv[ii]);
    if(duk_pcompile_lstring_filename(ctx, 0, js, len) != 0 || duk_pcall(ctx, 0) != 0) {
      duk_get_prop_string(ctx, -1, "stack");  // stack trace includes message
      fprintf(stderr, "%s: %s\n", argv[ii], duk_safe_to_string(ctx, -1));
      duk_pop(ctx);
      err = 1;
    }
    duk_pop(ctx);
    free(js);
  }
  duk_destroy_heap(ctx);
  return err;
}
