#include "harness.h"

#include "util.h"

static int count_item(const char *item, size_t len, void *udata) {
  (void)item, (void)len;
  ++*(int *)udata;
  return 0;
}

void test_util(void) {
  char buf[64];
  size_t n;

  n = fss_pct_decode(buf, sizeof(buf), "a%20b+c", 7);
  CHECK_STR(buf, n, "a b c");
  CHECK(fss_pct_decode(buf, sizeof(buf), "bad%2", 5) == (size_t)-1);
  CHECK(fss_pct_decode(buf, sizeof(buf), "bad%zz", 6) == (size_t)-1);
  CHECK(fss_pct_decode(buf, 3, "abcd", 4) == (size_t)-1);
  n = fss_pct_decode(buf, sizeof(buf), "%E5%9C%B0", 9); /* UTF-8 passthrough */
  CHECK_EQ_INT(n, 3);

  const char *q = "bbox=1,2,3,4&must=whiteboard&q=%E5%9C%B0+x&empty=&flag";
  long r = fss_query_get(q, strlen(q), "must", buf, sizeof(buf));
  CHECK_STR(buf, (size_t)r, "whiteboard");
  r = fss_query_get(q, strlen(q), "bbox", buf, sizeof(buf));
  CHECK_STR(buf, (size_t)r, "1,2,3,4");
  r = fss_query_get(q, strlen(q), "empty", buf, sizeof(buf));
  CHECK_EQ_INT(r, 0);
  r = fss_query_get(q, strlen(q), "flag", buf, sizeof(buf));
  CHECK_EQ_INT(r, 0);
  CHECK_EQ_INT(fss_query_get(q, strlen(q), "missing", buf, sizeof(buf)), -1);
  CHECK_EQ_INT(fss_query_get(q, strlen(q), "bb", buf, sizeof(buf)), -1);
  CHECK_EQ_INT(fss_query_get(q, strlen(q), "bbox", buf, 4), -2);

  int64_t i;
  CHECK(!fss_parse_i64("42", 2, &i) && i == 42);
  CHECK(!fss_parse_i64("-7", 2, &i) && i == -7);
  CHECK(fss_parse_i64("4x", 2, &i));
  CHECK(fss_parse_i64("", 0, &i));
  double d;
  CHECK(!fss_parse_double("40.0012", 7, &d));
  CHECK_NEAR(d, 40.0012, 1e-12);
  CHECK(fss_parse_double("nan", 3, &d));
  CHECK(fss_parse_double("1.5e", 4, &d));

  int items = 0;
  CHECK_EQ_INT(fss_split_csv(" a, b ,,c ", 10, count_item, &items), 3);
  CHECK_EQ_INT(items, 3);
  items = 0;
  CHECK_EQ_INT(fss_split_csv("", 0, count_item, &items), 0);
}
