#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "../src/frontend/json_parser.h"
#include "../src/frontend/sexp_parser.h"

static void check(fz_context *ctx, const char *input, bool json, bool fragmented)
{
  const double expected[] = {1073741824,1073741825,1073741826,2147483646,2147483647,-2147483648,1.25};
  vstack *stack = vstack_new(ctx);
  json_parser jp = initial_json_parser;
  sexp_parser sp = initial_sexp_parser;
  const char *end = input + strlen(input), *done = NULL;
  if (fragmented)
    for (const char *p = input; p < end; ++p)
    {
      done = json ? json_parse(ctx,&jp,stack,p,p+1) : sexp_parse(ctx,&sp,stack,p,p+1);
      assert(!done || done == end);
    }
  else
    done = json ? json_parse(ctx,&jp,stack,input,end) : sexp_parse(ctx,&sp,stack,input,end);
  assert(done == end);
  val root;
  vstack_get_arguments(ctx,stack,&root,1);
  assert(val_array_length(ctx,stack,root) == sizeof(expected)/sizeof(expected[0]));
  for (int i=0; i<sizeof(expected)/sizeof(expected[0]); ++i)
    assert(val_number(ctx,val_array_get(ctx,stack,root,i)) == expected[i]);
  vstack_free(ctx,stack);
}

int main(void)
{
  // The compact decoded representation must not double DVI metadata size.
  assert(sizeof(val) == 8);
  fz_context *ctx = fz_new_context(NULL,NULL,FZ_STORE_UNLIMITED);
  assert(ctx);
  const char *json = "[1073741824,1073741825,1073741826,2147483646,2147483647,-2147483648,1.25]";
  const char *sexp = "(1073741824 1073741825 1073741826 2147483646 2147483647 -2147483648 1.25)";
  check(ctx,json,true,false); check(ctx,json,true,true);
  check(ctx,sexp,false,false); check(ctx,sexp,false,true);
  fz_drop_context(ctx);
  puts("protocol integer precision: passed");
}
