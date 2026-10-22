#include <assert.h>
#include <stdio.h>
#include "../src/common/texlive_provider.c"
int main(void)
{
  struct table t; init(&t);
  add(&t, "/texmf", "doc/latex/listings-ext", "hyperref.cfg");
  add(&t, "/texmf", "source/latex/package", "sample.sty");
  assert(find(&t, "hyperref.cfg") == NULL); assert(find(&t, "sample.sty") == NULL);
  add(&t, "/texmf", "tex/latex/hyperref", "hyperref.sty");
  add(&t, "/texmf", "tex/latex/site", "hyperref.cfg");
  assert(strcmp(find(&t, "hyperref.sty"), "/texmf/tex/latex/hyperref/hyperref.sty") == 0);
  assert(strcmp(find(&t, "hyperref.cfg"), "/texmf/tex/latex/site/hyperref.cfg") == 0);
  finalize(&t); puts("PASS: runtime TeX Live lookup excludes documentation/example and source files");
}
