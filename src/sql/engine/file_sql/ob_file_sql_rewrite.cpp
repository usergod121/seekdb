/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define USING_LOG_PREFIX SQL_ENG

#include "sql/engine/file_sql/ob_file_sql_rewrite.h"

#include <dirent.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "lib/container/ob_se_array.h"
#include "lib/oblog/ob_log_module.h"
#include "lib/utility/ob_macro_utils.h"
#include "share/ob_errno.h"
#include "share/system_variable/ob_sys_var_class_type.h"
#include "sql/engine/file_sql/ob_file_column_infer.h"
#include "sql/engine/file_sql/ob_file_sql_def.h"
#include "sql/engine/file_sql/ob_file_value_classify.h"
#include "sql/session/ob_sql_session_info.h"

namespace oceanbase
{
namespace sql
{
namespace file_sql
{

// ---------------------------------------------------------------------------
// small helpers
// ---------------------------------------------------------------------------

static const int64_t FSQ_MAX_FILE_SIZE = 128LL * 1024 * 1024;

static bool fsq_is_blank(const char c) { return ' ' == c || '\t' == c || '\n' == c || '\r' == c; }

static bool fsq_is_ident_start(const char c)
{
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || '_' == c || '$' == c;
}

static bool fsq_is_ident_char(const char c)
{
  return fsq_is_ident_start(c) || (c >= '0' && c <= '9');
}

static char fsq_lower(const char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

static bool fsq_is_null(const common::ObString &s) { return NULL == s.ptr(); }

static bool fsq_iequal(const char *a, const int64_t alen, const char *b, const int64_t blen)
{
  bool equal = (alen == blen);
  for (int64_t i = 0; equal && i < alen; ++i) {
    equal = (fsq_lower(a[i]) == fsq_lower(b[i]));
  }
  return equal;
}

// case insensitive compare of the word at [pos, pos + wlen) with `word`
static bool fsq_word_at(const char *s, const int64_t len, const int64_t pos,
                        const char *word, const int64_t wlen)
{
  return pos >= 0 && pos + wlen <= len && fsq_iequal(s + pos, wlen, word, wlen)
         && (pos + wlen == len || !fsq_is_ident_char(s[pos + wlen]));
}

static int fsq_skip_blanks(const char *s, const int64_t len, int64_t &pos)
{
  int ret = OB_SUCCESS;
  while (pos < len && fsq_is_blank(s[pos])) {
    ++pos;
  }
  return ret;
}

// skip whitespace and comments
static int fsq_skip_spaces(const char *s, const int64_t len, int64_t &pos)
{
  int ret = OB_SUCCESS;
  bool go_on = true;
  while (OB_SUCC(ret) && go_on && pos < len) {
    if (fsq_is_blank(s[pos])) {
      ++pos;
    } else if (pos + 1 < len && '-' == s[pos] && '-' == s[pos + 1]) {
      while (pos < len && '\n' != s[pos]) { ++pos; }
    } else if (pos + 1 < len && '/' == s[pos] && '*' == s[pos + 1]) {
      pos += 2;
      while (pos + 1 < len && !('/' == s[pos] && '*' == s[pos + 1])) { ++pos; }
      if (pos + 1 < len) { pos += 2; }
    } else {
      go_on = false;
    }
  }
  return ret;
}

// skip a single quote delimited literal or a backtick delimited identifier.
static int fsq_skip_quoted(const char *s, const int64_t len, int64_t &pos)
{
  int ret = OB_SUCCESS;
  const char quote = s[pos];
  bool done = false;
  ++pos;
  while (!done) {
    if (pos >= len) {
      done = true;
    } else if ('\\' == s[pos] && pos + 1 < len && quote != '`') {
      pos += 2;
    } else if (quote == s[pos]) {
      if (pos + 1 < len && quote == s[pos + 1]) {
        pos += 2;
      } else {
        ++pos;
        done = true;
      }
    } else {
      ++pos;
    }
  }
  return ret;
}

// ---------------------------------------------------------------------------
// growable text buffer
// ---------------------------------------------------------------------------

class TextBuf
{
public:
  explicit TextBuf(common::ObIAllocator &alloc)
      : alloc_(alloc), buf_(NULL), len_(0), cap_(0)
  {}
  // The buffer is carved out of the statement allocator and intentionally not
  // released here: the rewritten statement text has to stay valid until the
  // statement is parsed.
  ~TextBuf() {}

  int reserve(const int64_t need)
  {
    int ret = OB_SUCCESS;
    if (need > cap_) {
      int64_t new_cap = cap_ <= 0 ? 1024 : cap_;
      while (new_cap < need) { new_cap *= 2; }
      char *nb = static_cast<char *>(alloc_.alloc(new_cap));
      if (NULL == nb) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else {
        if (NULL != buf_) {
          MEMCPY(nb, buf_, len_);
          alloc_.free(buf_);
        }
        buf_ = nb;
        cap_ = new_cap;
      }
    }
    return ret;
  }

  int append(const char *p, const int64_t len)
  {
    int ret = OB_SUCCESS;
    if (len > 0) {
      if (OB_FAIL(reserve(len_ + len))) {
      } else {
        MEMCPY(buf_ + len_, p, len);
        len_ += len;
      }
    }
    return ret;
  }

  int append(const char *p)
  {
    return append(p, static_cast<int64_t>(strlen(p)));
  }

  int append_char(const char c)
  {
    return append(&c, 1);
  }

  int append_int(const int64_t v)
  {
    char tmp[32];
    int n = snprintf(tmp, sizeof(tmp), "%ld", static_cast<long>(v));
    return n > 0 ? append(tmp, static_cast<int64_t>(n)) : OB_ERR_UNEXPECTED;
  }

  int64_t length() const { return len_; }

  common::ObString get_string() const
  {
    return common::ObString(static_cast<int32_t>(len_), buf_);
  }

private:
  common::ObIAllocator &alloc_;
  char *buf_;
  int64_t len_;
  int64_t cap_;
};

// ---------------------------------------------------------------------------
// one loaded file table
// ---------------------------------------------------------------------------

struct ColMeta
{
  ColMeta() : name_(), type_(0), type_name_(NULL), type_name_len_(0), nullable_(true) {}
  common::ObString name_;
  int64_t type_;               // ObObjType
  const char *type_name_;      // what DESCRIBE prints
  int64_t type_name_len_;
  bool nullable_;
};

class Table
{
public:
  Table() : col_count_(0) {}
  ~Table() {}
  common::ObSEArray<ColMeta, 16> cols_;
  common::ObSEArray<common::ObString, 256> cells_;   // row major, NULL ptr == SQL NULL
  int64_t col_count_;
};

static int fsq_append_row(Table &t, common::ObSEArray<common::ObString, 32> &rec)
{
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < t.col_count_; ++i) {
    common::ObString cell;   // default == NULL
    if (i < rec.count()) {
      cell = rec.at(i);
    }
    if (OB_FAIL(t.cells_.push_back(cell))) {
    }
  }
  return ret;
}

// deduplicate a column name the way DuckDB does: id, id__2, id__3, ...
static int fsq_unique_name(common::ObIAllocator &alloc,
                           const common::ObIArray<ColMeta> &cols,
                           common::ObString &name)
{
  int ret = OB_SUCCESS;
  bool dup = false;
  for (int64_t i = 0; !dup && i < cols.count(); ++i) {
    dup = (0 == name.compare(cols.at(i).name_));
  }
  if (dup) {
    const common::ObString base = name;
    int64_t seq = 2;
    while (OB_SUCC(ret)) {
      char suffix[32];
      int n = snprintf(suffix, sizeof(suffix), "__%ld", static_cast<long>(seq));
      if (n <= 0) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        char *buf = static_cast<char *>(alloc.alloc(base.length() + n));
        if (NULL == buf) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
        } else {
          MEMCPY(buf, base.ptr(), base.length());
          MEMCPY(buf + base.length(), suffix, n);
          common::ObString candidate(static_cast<int32_t>(base.length() + n), buf);
          bool clash = false;
          for (int64_t i = 0; !clash && i < cols.count(); ++i) {
            clash = (0 == candidate.compare(cols.at(i).name_));
          }
          if (clash) {
            alloc.free(buf);
            ++seq;
          } else {
            name = candidate;
            break;
          }
        }
      }
    }
  }
  return ret;
}

// ---------------------------------------------------------------------------
// reading a file
// ---------------------------------------------------------------------------

static int fsq_read_file(common::ObIAllocator &alloc, const char *path, char *&data, int64_t &size)
{
  int ret = OB_SUCCESS;
  data = NULL;
  size = 0;
  int fd = ::open(path, O_RDONLY);
  if (fd < 0) {
    ret = OB_FILE_NOT_EXIST;
    LOG_WARN("open file failed", K(ret));
  } else {
    struct stat st;
    if (0 != ::fstat(fd, &st)) {
      ret = OB_IO_ERROR;
    } else if (st.st_size > FSQ_MAX_FILE_SIZE) {
      ret = OB_SIZE_OVERFLOW;
      LOG_WARN("file too large to be read as a table", K(ret), K(st.st_size));
    } else {
      size = static_cast<int64_t>(st.st_size);
      char *buf = static_cast<char *>(alloc.alloc(size + 1));
      if (NULL == buf) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else {
        int64_t got = 0;
        while (OB_SUCC(ret) && got < size) {
          ssize_t n = ::read(fd, buf + got, static_cast<size_t>(size - got));
          if (n <= 0) {
            ret = OB_IO_ERROR;
          } else {
            got += n;
          }
        }
        if (OB_SUCC(ret)) {
          buf[size] = '\0';
          data = buf;
        }
      }
    }
    ::close(fd);
  }
  return ret;
}

// ---------------------------------------------------------------------------
// CSV
// ---------------------------------------------------------------------------

struct CsvOptions
{
  CsvOptions() : delim_(','), quote_('"'), escape_('\\'), has_header_(true) {}
  char delim_;
  char quote_;
  char escape_;
  bool has_header_;
};

static int fsq_next_record(common::ObIAllocator &alloc,
                           const char *data, const int64_t size, int64_t &pos,
                           const CsvOptions &opt,
                           common::ObSEArray<common::ObString, 32> &fields)
{
  int ret = OB_SUCCESS;
  fields.reuse();
  // skip blank lines between records
  while (pos < size && ('\n' == data[pos] || '\r' == data[pos])) {
    ++pos;
  }
  bool end_of_record = (pos >= size);
  while (!end_of_record && pos < size) {
    common::ObString field;
    if (opt.quote_ == data[pos]) {
      ++pos;
      const int64_t begin = pos;
      bool closed = false;
      bool doubled = false;
      while (!closed) {
        if (pos >= size) {
          closed = true;
        } else if (opt.quote_ == data[pos]) {
          if (pos + 1 < size && opt.quote_ == data[pos + 1]) {
            doubled = true;
            pos += 2;
          } else {
            closed = true;
            ++pos;
          }
        } else {
          ++pos;
        }
      }
      int64_t end = closed ? pos - 1 : pos;
      if (end < begin) { end = begin; }
      // A quoted field may carry line breaks.  Normalise CRLF to LF there so a
      // file checked out with windows line endings reads the same as one with
      // unix line endings, and collapse the doubled quotes.
      bool need_copy = doubled;
      for (int64_t i = begin; !need_copy && i + 1 < end; ++i) {
        need_copy = ('\r' == data[i] && '\n' == data[i + 1]);
      }
      if (!need_copy) {
        field.assign_ptr(const_cast<char *>(data + begin), static_cast<int32_t>(end - begin));
      } else {
        char *buf = static_cast<char *>(alloc.alloc(end - begin + 1));
        if (NULL == buf) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
        } else {
          int64_t out = 0;
          for (int64_t i = begin; i < end; ++i) {
            if (opt.quote_ == data[i] && i + 1 < end && opt.quote_ == data[i + 1]) {
              buf[out++] = opt.quote_;
              ++i;
            } else if ('\r' == data[i] && i + 1 < end && '\n' == data[i + 1]) {
              // drop the carriage return of a CRLF line break
            } else {
              buf[out++] = data[i];
            }
          }
          field.assign_ptr(buf, static_cast<int32_t>(out));
        }
      }
      // tolerate trailing blanks before the next delimiter
      while (OB_SUCC(ret) && pos < size && opt.delim_ != data[pos] && '\n' != data[pos]) {
        ++pos;
      }
    } else {
      const int64_t begin = pos;
      while (pos < size && opt.delim_ != data[pos] && '\n' != data[pos]) {
        ++pos;
      }
      int64_t end = pos;
      if (end > begin && '\r' == data[end - 1]) {
        --end;
      }
      if (end > begin) {
        field.assign_ptr(const_cast<char *>(data + begin), static_cast<int32_t>(end - begin));
      }
    }
    if (OB_SUCC(ret) && OB_FAIL(fields.push_back(field))) {
    }
    if (OB_SUCC(ret)) {
      if (pos < size && opt.delim_ == data[pos]) {
        ++pos;
        if (pos >= size) {
          end_of_record = true;
        }
      } else {
        if (pos < size && '\n' == data[pos]) {
          ++pos;
        }
        end_of_record = true;
      }
    }
  }
  return ret;
}

static int fsq_parse_csv(common::ObIAllocator &alloc,
                         const char *data, const int64_t size,
                         const CsvOptions &opt,
                         Table &t)
{
  int ret = OB_SUCCESS;
  int64_t pos = 0;
  bool saw_header = false;
  common::ObSEArray<common::ObString, 32> rec;
  while (OB_SUCC(ret)) {
    if (OB_FAIL(fsq_next_record(alloc, data, size, pos, opt, rec))) {
    } else if (rec.count() <= 0) {
      if (pos >= size) { break; }
    } else if (!saw_header && opt.has_header_) {
      saw_header = true;
      for (int64_t i = 0; OB_SUCC(ret) && i < rec.count(); ++i) {
        ColMeta col;
        common::ObString name = rec.at(i);
        if (fsq_is_null(name) || 0 == name.length()) {
          char tmp[32];
          int n = snprintf(tmp, sizeof(tmp), "column%ld", static_cast<long>(i));
          char *buf = static_cast<char *>(alloc.alloc(n > 0 ? n : 1));
          if (NULL == buf) {
            ret = OB_ALLOCATE_MEMORY_FAILED;
          } else {
            MEMCPY(buf, tmp, n);
            name.assign_ptr(buf, n);
          }
        }
        if (OB_SUCC(ret)) {
          if (OB_FAIL(fsq_unique_name(alloc, t.cols_, name))) {
          } else {
            col.name_ = name;
            if (OB_FAIL(t.cols_.push_back(col))) {
            }
          }
        }
      }
      if (OB_SUCC(ret)) {
        t.col_count_ = t.cols_.count();
      }
      if (pos >= size) { break; }
    } else {
      if (!saw_header) {
        // no header line: the first record defines the width
        saw_header = true;
        for (int64_t i = 0; OB_SUCC(ret) && i < rec.count(); ++i) {
          char tmp[32];
          int n = snprintf(tmp, sizeof(tmp), "column%ld", static_cast<long>(i));
          char *buf = static_cast<char *>(alloc.alloc(n > 0 ? n : 1));
          if (NULL == buf) {
            ret = OB_ALLOCATE_MEMORY_FAILED;
          } else {
            MEMCPY(buf, tmp, n);
            ColMeta col;
            col.name_.assign_ptr(buf, n);
            if (OB_FAIL(t.cols_.push_back(col))) {
            }
          }
        }
        if (OB_SUCC(ret)) {
          t.col_count_ = t.cols_.count();
        }
      }
      if (OB_SUCC(ret) && OB_FAIL(fsq_append_row(t, rec))) {
      }
      if (pos >= size) { break; }
    }
  }
  return ret;
}

// ---------------------------------------------------------------------------
// JSON lines
// ---------------------------------------------------------------------------

static int fsq_json_skip_blanks(const char *s, const int64_t len, int64_t &pos)
{
  while (pos < len && fsq_is_blank(s[pos])) { ++pos; }
  return OB_SUCCESS;
}

// unescape a JSON string body [begin, end)
static int fsq_json_unescape(common::ObIAllocator &alloc, const char *s,
                             const int64_t begin, const int64_t end,
                             common::ObString &out)
{
  int ret = OB_SUCCESS;
  bool need = false;
  for (int64_t i = begin; !need && i < end; ++i) {
    need = ('\\' == s[i]);
  }
  if (!need) {
    out.assign_ptr(const_cast<char *>(s + begin), static_cast<int32_t>(end - begin));
  } else {
    char *buf = static_cast<char *>(alloc.alloc(end - begin + 1));
    if (NULL == buf) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      int64_t o = 0;
      for (int64_t i = begin; OB_SUCC(ret) && i < end; ++i) {
        if ('\\' != s[i] || i + 1 >= end) {
          buf[o++] = s[i];
        } else {
          ++i;
          switch (s[i]) {
            case 'n': buf[o++] = '\n'; break;
            case 't': buf[o++] = '\t'; break;
            case 'r': buf[o++] = '\r'; break;
            case 'b': buf[o++] = '\b'; break;
            case 'f': buf[o++] = '\f'; break;
            case 'u': {
              // keep the escape: only the BMP ASCII subset is expected here
              if (i + 4 < end) {
                int code = 0;
                bool good = true;
                for (int k = 1; k <= 4 && good; ++k) {
                  const char c = s[i + k];
                  good = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
                  if (good) {
                    code = code * 16 + (c <= '9' ? c - '0' : (fsq_lower(c) - 'a' + 10));
                  }
                }
                if (good) {
                  i += 4;
                  if (code < 0x80) {
                    buf[o++] = static_cast<char>(code);
                  } else if (code < 0x800) {
                    buf[o++] = static_cast<char>(0xC0 | (code >> 6));
                    buf[o++] = static_cast<char>(0x80 | (code & 0x3F));
                  } else {
                    buf[o++] = static_cast<char>(0xE0 | (code >> 12));
                    buf[o++] = static_cast<char>(0x80 | ((code >> 6) & 0x3F));
                    buf[o++] = static_cast<char>(0x80 | (code & 0x3F));
                  }
                } else {
                  buf[o++] = 'u';
                }
              } else {
                buf[o++] = 'u';
              }
              break;
            }
            default: buf[o++] = s[i]; break;
          }
        }
      }
      // ObString needs a non null pointer to mean "not NULL", a zero length
      // value is a legitimate empty string.
      out.assign_ptr(buf, static_cast<int32_t>(o));
    }
  }
  return ret;
}

// parse one JSON value at `pos`; returns a text form plus an is_null flag
static int fsq_json_value(common::ObIAllocator &alloc, const char *s, const int64_t len,
                          int64_t &pos, common::ObString &out, bool &is_null)
{
  int ret = OB_SUCCESS;
  is_null = false;
  out.reset();
  fsq_json_skip_blanks(s, len, pos);
  if (pos >= len) {
    ret = OB_ERR_PARSE_SQL;
  } else if ('"' == s[pos]) {
    const int64_t begin = pos + 1;
    int64_t i = begin;
    bool closed = false;
    while (!closed && i < len) {
      if ('\\' == s[i]) {
        i += 2;
      } else if ('"' == s[i]) {
        closed = true;
      } else {
        ++i;
      }
    }
    if (!closed) {
      ret = OB_ERR_PARSE_SQL;
    } else if (OB_FAIL(fsq_json_unescape(alloc, s, begin, i, out))) {
    } else {
      pos = i + 1;
    }
  } else if ('{' == s[pos] || '[' == s[pos]) {
    // nested value: keep the raw text, it becomes a VARCHAR column
    const int64_t begin = pos;
    int depth = 0;
    bool closed = false;
    bool in_str = false;
    while (!closed && pos < len) {
      const char c = s[pos];
      if (in_str) {
        if ('\\' == c) {
          pos += 2;
          continue;
        } else if ('"' == c) {
          in_str = false;
        }
      } else if ('"' == c) {
        in_str = true;
      } else if ('{' == c || '[' == c) {
        ++depth;
      } else if ('}' == c || ']' == c) {
        --depth;
        if (depth <= 0) { closed = true; }
      }
      ++pos;
    }
    out.assign_ptr(const_cast<char *>(s + begin), static_cast<int32_t>(pos - begin));
  } else {
    const int64_t begin = pos;
    while (pos < len && ',' != s[pos] && '}' != s[pos] && ']' != s[pos] && !fsq_is_blank(s[pos])) {
      ++pos;
    }
    if (pos == begin) {
      ret = OB_ERR_PARSE_SQL;
    } else {
      out.assign_ptr(const_cast<char *>(s + begin), static_cast<int32_t>(pos - begin));
      if (fsq_iequal(out.ptr(), out.length(), "null", 4)) {
        is_null = true;
        out.reset();
      }
    }
  }
  return ret;
}

struct JsonLine
{
  JsonLine() : keys_(), vals_(), nulls_() {}
  common::ObSEArray<common::ObString, 16> keys_;
  common::ObSEArray<common::ObString, 16> vals_;
  common::ObSEArray<int8_t, 16> nulls_;
};

static int fsq_parse_ndjson(common::ObIAllocator &alloc,
                            const char *data, const int64_t size,
                            Table &t)
{
  int ret = OB_SUCCESS;
  common::ObSEArray<JsonLine *, 64> lines;
  int64_t pos = 0;
  while (OB_SUCC(ret) && pos < size) {
    // one object per line
    int64_t eol = pos;
    while (eol < size && '\n' != data[eol]) { ++eol; }
    int64_t p = pos;
    pos = eol + 1;
    fsq_json_skip_blanks(data, eol, p);
    if (p >= eol) {
      continue;
    }
    JsonLine *line = static_cast<JsonLine *>(alloc.alloc(sizeof(JsonLine)));
    if (NULL == line) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      new (line) JsonLine();
      if ('{' != data[p]) {
        ret = OB_ERR_PARSE_SQL;
      } else {
        ++p;
        bool closed = false;
        while (OB_SUCC(ret) && !closed) {
          fsq_json_skip_blanks(data, eol, p);
          if (p >= eol) {
            ret = OB_ERR_PARSE_SQL;
          } else if ('}' == data[p]) {
            ++p;
            closed = true;
          } else {
            common::ObString key;
            bool key_null = false;
            if (OB_FAIL(fsq_json_value(alloc, data, eol, p, key, key_null))) {
            } else if (key_null) {
              ret = OB_ERR_PARSE_SQL;
            } else {
              fsq_json_skip_blanks(data, eol, p);
              if (p >= eol || ':' != data[p]) {
                ret = OB_ERR_PARSE_SQL;
              } else {
                ++p;
                common::ObString val;
                bool val_null = false;
                if (OB_FAIL(fsq_json_value(alloc, data, eol, p, val, val_null))) {
                } else if (OB_FAIL(line->keys_.push_back(key))) {
                } else if (OB_FAIL(line->vals_.push_back(val))) {
                } else if (OB_FAIL(line->nulls_.push_back(val_null ? 1 : 0))) {
                }
              }
            }
            if (OB_SUCC(ret)) {
              fsq_json_skip_blanks(data, eol, p);
              if (p < eol && ',' == data[p]) {
                ++p;
              }
            }
          }
        }
      }
      if (OB_SUCC(ret) && OB_FAIL(lines.push_back(line))) {
      }
    }
  }
  // union of the fields in first appearance order
  for (int64_t i = 0; OB_SUCC(ret) && i < lines.count(); ++i) {
    const JsonLine *line = lines.at(i);
    for (int64_t k = 0; OB_SUCC(ret) && k < line->keys_.count(); ++k) {
      const common::ObString &key = line->keys_.at(k);
      bool found = false;
      for (int64_t c = 0; !found && c < t.cols_.count(); ++c) {
        found = (0 == key.compare(t.cols_.at(c).name_));
      }
      if (!found) {
        ColMeta col;
        col.name_ = key;
        if (OB_FAIL(t.cols_.push_back(col))) {
        }
      }
    }
  }
  if (OB_SUCC(ret)) {
    t.col_count_ = t.cols_.count();
    for (int64_t i = 0; OB_SUCC(ret) && i < lines.count(); ++i) {
      const JsonLine *line = lines.at(i);
      for (int64_t c = 0; OB_SUCC(ret) && c < t.col_count_; ++c) {
        common::ObString cell;
        for (int64_t k = 0; k < line->keys_.count(); ++k) {
          if (0 == line->keys_.at(k).compare(t.cols_.at(c).name_)) {
            if (0 == line->nulls_.at(k)) {
              cell = line->vals_.at(k);
            }
            break;
          }
        }
        if (OB_FAIL(t.cells_.push_back(cell))) {
        }
      }
    }
  }
  return ret;
}

// ---------------------------------------------------------------------------
// type inference over the loaded table
// ---------------------------------------------------------------------------

static int fsq_infer(Table &t)
{
  int ret = OB_SUCCESS;
  for (int64_t c = 0; OB_SUCC(ret) && c < t.col_count_; ++c) {
    ObFileColumnInfer infer;
    for (int64_t r = 0; r < t.cells_.count() / (t.col_count_ > 0 ? t.col_count_ : 1); ++r) {
      const common::ObString &cell = t.cells_.at(r * t.col_count_ + c);
      infer.observe(fsq_is_null(cell) ? common::ObString() : cell, true);
    }
    ObObjType type = ObNullType;
    const char *name = NULL;
    int64_t name_len = 0;
    bool nullable = true;
    infer.resolve(type, name, name_len, nullable);
    t.cols_.at(c).type_ = static_cast<int64_t>(type);
    t.cols_.at(c).type_name_ = name;
    t.cols_.at(c).type_name_len_ = name_len;
    t.cols_.at(c).nullable_ = nullable;
  }
  return ret;
}

// ---------------------------------------------------------------------------
// emitting SQL text out of the loaded table
// ---------------------------------------------------------------------------

static int fsq_emit_backticked(TextBuf &out, const common::ObString &name)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(out.append_char('`'))) {
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < name.length(); ++i) {
    if ('`' == name.ptr()[i] && OB_FAIL(out.append_char('`'))) {
    } else if (OB_FAIL(out.append_char(name.ptr()[i]))) {
    }
  }
  if (OB_SUCC(ret) && OB_FAIL(out.append_char('`'))) {
  }
  return ret;
}

// quoted text literal, `t_to_space` turns the ISO 'T' of a timestamp into a blank
static int fsq_emit_quoted(TextBuf &out, const char *p, const int64_t len, const bool t_to_space)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(out.append_char('\''))) {
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < len; ++i) {
    const char c = p[i];
    if ('\\' == c || '\'' == c) {
      if (OB_FAIL(out.append_char('\\'))) {
      } else if (OB_FAIL(out.append_char(c))) {
      }
    } else if (t_to_space && 'T' == c) {
      if (OB_FAIL(out.append_char(' '))) {
      }
    } else if (OB_FAIL(out.append_char(c))) {
    }
  }
  if (OB_SUCC(ret) && OB_FAIL(out.append_char('\''))) {
  }
  return ret;
}

static bool fsq_has_exponent(const char *p, const int64_t len)
{
  bool has = false;
  for (int64_t i = 0; !has && i < len; ++i) {
    has = ('e' == p[i] || 'E' == p[i]);
  }
  return has;
}

static int fsq_emit_cell(TextBuf &out, const common::ObString &cell, const int64_t type)
{
  int ret = OB_SUCCESS;
  const bool is_null = fsq_is_null(cell);
  switch (static_cast<ObObjType>(type)) {
    case ObIntType:
    case ObTinyIntType:
    case ObInt32Type: {
      if (is_null) {
        if (OB_FAIL(out.append("cast(null as signed)"))) {}
      } else if (OB_FAIL(out.append(cell.ptr(), cell.length()))) {}
      break;
    }
    case ObDoubleType:
    case ObFloatType: {
      if (is_null) {
        if (OB_FAIL(out.append("cast(null as double)"))) {}
      } else if (fsq_has_exponent(cell.ptr(), cell.length())) {
        if (OB_FAIL(out.append(cell.ptr(), cell.length()))) {}
      } else if (OB_FAIL(out.append(cell.ptr(), cell.length()))
                 || OB_FAIL(out.append("e0"))) {
      }
      break;
    }
    case ObDateType: {
      if (is_null) {
        if (OB_FAIL(out.append("cast(null as date)"))) {}
      } else if (OB_FAIL(out.append("cast("))
                 || OB_FAIL(fsq_emit_quoted(out, cell.ptr(), cell.length(), false))
                 || OB_FAIL(out.append(" as date)"))) {
      }
      break;
    }
    case ObDateTimeType: {
      if (is_null) {
        if (OB_FAIL(out.append("cast(null as datetime)"))) {}
      } else if (OB_FAIL(out.append("cast("))
                 || OB_FAIL(fsq_emit_quoted(out, cell.ptr(), cell.length(), true))
                 || OB_FAIL(out.append(" as datetime)"))) {
      }
      break;
    }
    default: {
      if (is_null) {
        if (OB_FAIL(out.append("cast(null as char)"))) {}
      } else if (OB_FAIL(fsq_emit_quoted(out, cell.ptr(), cell.length(), false))) {
      }
      break;
    }
  }
  return ret;
}

// (select .. union all select ..) [alias]
static int fsq_emit_derived(TextBuf &out, const Table &t, const bool need_alias,
                            const int64_t idx)
{
  int ret = OB_SUCCESS;
  const int64_t rows = t.col_count_ > 0 ? t.cells_.count() / t.col_count_ : 0;
  if (OB_FAIL(out.append_char('('))) {
  } else if (0 == rows || 0 == t.col_count_) {
    if (OB_FAIL(out.append("select "))) {
    }
    for (int64_t c = 0; OB_SUCC(ret) && c < t.col_count_; ++c) {
      if (c > 0 && OB_FAIL(out.append(", "))) {}
      if (OB_SUCC(ret) && OB_FAIL(fsq_emit_cell(out, common::ObString(), t.cols_.at(c).type_))) {}
      if (OB_SUCC(ret) && OB_FAIL(out.append(" as "))) {}
      if (OB_SUCC(ret) && OB_FAIL(fsq_emit_backticked(out, t.cols_.at(c).name_))) {}
    }
    if (OB_SUCC(ret) && OB_FAIL(out.append(" from (select 1 as __fsq_one) __fsq_e where 1 = 0"))) {}
  } else {
    for (int64_t r = 0; OB_SUCC(ret) && r < rows; ++r) {
      if (0 == r) {
        if (OB_FAIL(out.append("select "))) {}
      } else {
        if (OB_FAIL(out.append(" union all select "))) {}
      }
      for (int64_t c = 0; OB_SUCC(ret) && c < t.col_count_; ++c) {
        if (c > 0 && OB_FAIL(out.append(", "))) {}
        if (OB_SUCC(ret)
            && OB_FAIL(fsq_emit_cell(out, t.cells_.at(r * t.col_count_ + c),
                                     t.cols_.at(c).type_))) {}
        if (OB_SUCC(ret) && 0 == r && OB_FAIL(out.append(" as "))) {}
        if (OB_SUCC(ret) && 0 == r
            && OB_FAIL(fsq_emit_backticked(out, t.cols_.at(c).name_))) {}
      }
    }
  }
  if (OB_SUCC(ret) && OB_FAIL(out.append_char(')'))) {
  }
  if (OB_SUCC(ret) && need_alias) {
    if (OB_FAIL(out.append(" __file_sql_"))) {
    } else if (OB_FAIL(out.append_int(idx))) {
    }
  }
  return ret;
}

// one row of values of the DESCRIBE output
static int fsq_emit_describe_values(TextBuf &out, const ColMeta &col)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(fsq_emit_quoted(out, col.name_.ptr(), col.name_.length(), false))) {
  } else if (OB_FAIL(out.append(" as column_name, "))) {
  } else if (OB_FAIL(fsq_emit_quoted(out, col.type_name_, col.type_name_len_, false))) {
  } else if (OB_FAIL(out.append(" as column_type, "))) {
  } else if (OB_FAIL(out.append(col.nullable_ ? "'YES'" : "'NO'"))) {
  } else if (OB_FAIL(out.append(" as `null`, null as `key`, null as `default`, null as `extra`"))) {
  }
  return ret;
}

// ---------------------------------------------------------------------------
// parsing one file table function call
// ---------------------------------------------------------------------------

struct CallSpec
{
  CallSpec()
      : fmt_(ObFileFormat::FILE_FORMAT_INVALID),
        path_(),
        path_buf_(NULL),
        csv_(),
        sample_size_(-1),
        is_glob_(false)
  {}
  ObFileFormat fmt_;
  common::ObString path_;
  char *path_buf_;
  CsvOptions csv_;
  int64_t sample_size_;
  bool is_glob_;
};

static int fsq_parse_string_arg(common::ObIAllocator &alloc, const char *s, const int64_t len,
                                int64_t &pos, common::ObString &raw, char *&nul_terminated)
{
  int ret = OB_SUCCESS;
  raw.reset();
  nul_terminated = NULL;
  if (pos >= len || ('\'' != s[pos] && '"' != s[pos])) {
    ret = OB_ERR_PARSE_SQL;
  } else {
    const char quote = s[pos];
    const int64_t begin = pos + 1;
    int64_t i = begin;
    bool closed = false;
    while (!closed && i < len) {
      if ('\\' == s[i] && i + 1 < len) {
        i += 2;
      } else if (quote == s[i]) {
        if (i + 1 < len && quote == s[i + 1]) {
          i += 2;
        } else {
          closed = true;
        }
      } else {
        ++i;
      }
    }
    if (!closed) {
      ret = OB_ERR_PARSE_SQL;
    } else {
      char *buf = static_cast<char *>(alloc.alloc(i - begin + 1));
      if (NULL == buf) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else {
        int64_t o = 0;
        for (int64_t k = begin; k < i; ++k) {
          if ('\\' == s[k] && k + 1 < i) {
            ++k;
            buf[o++] = s[k];
          } else if (quote == s[k] && k + 1 < i && quote == s[k + 1]) {
            buf[o++] = quote;
            ++k;
          } else {
            buf[o++] = s[k];
          }
        }
        buf[o] = '\0';
        raw.assign_ptr(buf, static_cast<int32_t>(o));
        nul_terminated = buf;
        pos = i + 1;
      }
    }
  }
  return ret;
}

// ---------------------------------------------------------------------------
// directory listing for glob()
// ---------------------------------------------------------------------------

static bool fsq_match(const char *name, const int64_t nlen, const char *pat, const int64_t plen)
{
  int64_t ni = 0;
  int64_t pi = 0;
  int64_t star_p = -1;
  int64_t star_n = 0;
  while (ni < nlen) {
    if (pi < plen && ('?' == pat[pi] || pat[pi] == name[ni])) {
      ++ni;
      ++pi;
    } else if (pi < plen && '*' == pat[pi]) {
      star_p = pi;
      star_n = ni;
      ++pi;
    } else if (star_p >= 0) {
      pi = star_p + 1;
      ++star_n;
      ni = star_n;
    } else {
      return false;
    }
  }
  while (pi < plen && '*' == pat[pi]) { ++pi; }
  return pi == plen;
}

static int fsq_glob(common::ObIAllocator &alloc, const common::ObString &pattern, Table &t)
{
  int ret = OB_SUCCESS;
  // split the pattern into directory and file pattern
  int64_t slash = -1;
  for (int64_t i = 0; i < pattern.length(); ++i) {
    if ('/' == pattern.ptr()[i]) { slash = i; }
  }
  common::ObString dir;
  common::ObString fpat;
  char *dir_buf = NULL;
  if (slash >= 0) {
    dir_buf = static_cast<char *>(alloc.alloc(slash + 2));
    if (NULL == dir_buf) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      MEMCPY(dir_buf, pattern.ptr(), slash + 1);
      dir_buf[slash + 1] = '\0';
      dir.assign_ptr(dir_buf, static_cast<int32_t>(slash + 1));
      fpat.assign_ptr(pattern.ptr() + slash + 1,
                      static_cast<int32_t>(pattern.length() - slash - 1));
    }
  } else {
    dir_buf = static_cast<char *>(alloc.alloc(2));
    if (NULL == dir_buf) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      dir_buf[0] = '.';
      dir_buf[1] = '\0';
      dir.assign_ptr(dir_buf, 1);
      fpat = pattern;
    }
  }
  ColMeta col;
  char *name_buf = NULL;
  if (OB_FAIL(ret)) {
  } else if (NULL == (name_buf = static_cast<char *>(alloc.alloc(5)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    MEMCPY(name_buf, "file", 4);
    col.name_.assign_ptr(name_buf, 4);
    col.type_ = static_cast<int64_t>(ObVarcharType);
    col.type_name_ = "VARCHAR";
    col.type_name_len_ = 7;
    col.nullable_ = false;
    if (OB_FAIL(t.cols_.push_back(col))) {
    } else {
      t.col_count_ = 1;
    }
  }
  // collect the matching entries, sorted by name
  common::ObSEArray<common::ObString, 32> names;
  if (OB_SUCC(ret)) {
    DIR *d = ::opendir(dir_buf);
    if (NULL == d) {
      ret = OB_FILE_NOT_EXIST;
      LOG_WARN("open dir failed", K(ret), K(dir));
    } else {
      struct dirent *ent = NULL;
      while (OB_SUCC(ret) && NULL != (ent = ::readdir(d))) {
        const int64_t nlen = static_cast<int64_t>(strlen(ent->d_name));
        if ((1 == nlen && '.' == ent->d_name[0])
            || (2 == nlen && '.' == ent->d_name[0] && '.' == ent->d_name[1])) {
          continue;
        }
        if (fsq_match(ent->d_name, nlen, fpat.ptr(), fpat.length())) {
          char *full = static_cast<char *>(alloc.alloc(dir.length() + nlen + 1));
          if (NULL == full) {
            ret = OB_ALLOCATE_MEMORY_FAILED;
          } else {
            MEMCPY(full, dir.ptr(), dir.length());
            MEMCPY(full + dir.length(), ent->d_name, nlen);
            common::ObString entry(static_cast<int32_t>(dir.length() + nlen), full);
            bool is_dir = false;
            struct stat st;
            if (0 == ::stat(full, &st)) {
              is_dir = S_ISDIR(st.st_mode);
            }
            if (!is_dir && OB_FAIL(names.push_back(entry))) {
            }
          }
        }
      }
      ::closedir(d);
    }
  }
  // insertion sort by name (the lists are small)
  for (int64_t i = 1; OB_SUCC(ret) && i < names.count(); ++i) {
    common::ObString key = names.at(i);
    int64_t j = i - 1;
    while (j >= 0 && names.at(j).compare(key) > 0) {
      names.at(j + 1) = names.at(j);
      --j;
    }
    names.at(j + 1) = key;
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < names.count(); ++i) {
    if (OB_FAIL(t.cells_.push_back(names.at(i)))) {
    }
  }
  return ret;
}

// ---------------------------------------------------------------------------
// scanning the statement text
// ---------------------------------------------------------------------------

static const char *const FSQ_FUNCS[] = {
  "read_csv", "read_json", "read_ndjson", "read_parquet", "glob"
};
static const int64_t FSQ_FUNC_COUNT = 5;

struct ArgSeg
{
  ArgSeg() : begin_(0), end_(0) {}
  int64_t begin_;
  int64_t end_;
};

// find the next file table function call at or after `pos`
static int fsq_find_call(const char *s, const int64_t len, int64_t pos,
                         int64_t &name_pos, int64_t &name_len, bool &found)
{
  found = false;
  while (!found && pos < len) {
    const char c = s[pos];
    if ('\'' == c || '"' == c || '`' == c) {
      fsq_skip_quoted(s, len, pos);
    } else if (fsq_is_ident_start(c)) {
      const int64_t begin = pos;
      while (pos < len && fsq_is_ident_char(s[pos])) { ++pos; }
      const int64_t wl = pos - begin;
      for (int64_t i = 0; !found && i < FSQ_FUNC_COUNT; ++i) {
        if (fsq_iequal(s + begin, wl, FSQ_FUNCS[i],
                       static_cast<int64_t>(strlen(FSQ_FUNCS[i])))) {
          int64_t q = pos;
          fsq_skip_blanks(s, len, q);
          if (q < len && '(' == s[q]) {
            found = true;
            name_pos = begin;
            name_len = wl;
          }
        }
      }
    } else {
      ++pos;
    }
  }
  return OB_SUCCESS;
}

static int fsq_find_close(const char *s, const int64_t len, const int64_t open,
                          int64_t &close_pos)
{
  int ret = OB_SUCCESS;
  int64_t pos = open;
  int depth = 0;
  bool done = false;
  while (!done) {
    if (pos >= len) {
      ret = OB_ERR_PARSE_SQL;
      done = true;
    } else if ('\'' == s[pos] || '"' == s[pos] || '`' == s[pos]) {
      fsq_skip_quoted(s, len, pos);
    } else {
      if ('(' == s[pos]) {
        ++depth;
      } else if (')' == s[pos]) {
        --depth;
        if (0 == depth) {
          close_pos = pos;
          done = true;
        }
      }
      ++pos;
    }
  }
  return ret;
}

static int fsq_split_args(const char *s, const int64_t begin, const int64_t end,
                          common::ObSEArray<ArgSeg, 16> &segs)
{
  int ret = OB_SUCCESS;
  int64_t pos = begin;
  int64_t seg_begin = begin;
  int depth = 0;
  while (OB_SUCC(ret) && pos < end) {
    const char c = s[pos];
    if ('\'' == c || '"' == c || '`' == c) {
      fsq_skip_quoted(s, end, pos);
    } else {
      if ('(' == c) {
        ++depth;
      } else if (')' == c) {
        --depth;
      } else if (',' == c && 0 == depth) {
        ArgSeg seg;
        seg.begin_ = seg_begin;
        seg.end_ = pos;
        ret = segs.push_back(seg);
        seg_begin = pos + 1;
      }
      ++pos;
    }
  }
  if (OB_SUCC(ret)) {
    ArgSeg seg;
    seg.begin_ = seg_begin;
    seg.end_ = end;
    ret = segs.push_back(seg);
  }
  return ret;
}

static void fsq_trim(const char *s, ArgSeg &seg)
{
  while (seg.begin_ < seg.end_ && fsq_is_blank(s[seg.begin_])) { ++seg.begin_; }
  while (seg.end_ > seg.begin_ && fsq_is_blank(s[seg.end_ - 1])) { --seg.end_; }
}

static int64_t fsq_top_eq(const char *s, const ArgSeg &seg)
{
  int64_t pos = seg.begin_;
  int64_t found = -1;
  int depth = 0;
  while (pos < seg.end_ && found < 0) {
    const char c = s[pos];
    if ('\'' == c || '"' == c || '`' == c) {
      fsq_skip_quoted(s, seg.end_, pos);
    } else {
      if ('(' == c) {
        ++depth;
      } else if (')' == c) {
        --depth;
      } else if ('=' == c && 0 == depth) {
        found = pos;
      }
      ++pos;
    }
  }
  return found;
}

// is there a table alias right after the call?
static bool fsq_alias_follows(const char *s, const int64_t len, int64_t pos)
{
  bool alias = false;
  static const char *const RESERVED[] = {
    "where", "group", "order", "limit", "having", "join", "inner", "left",
    "right", "full", "cross", "natural", "on", "using", "union", "window",
    "for", "lock", "into", "as", "partition", "offset", "fetch", "except",
    "intersect", "minus", "straight_join", "procedure", "set", "values"
  };
  if (pos < len) {
    const char c = s[pos];
    if ('\'' == c || '"' == c || '`' == c) {
      alias = true;
    } else if (fsq_is_ident_start(c)) {
      const int64_t begin = pos;
      while (pos < len && fsq_is_ident_char(s[pos])) { ++pos; }
      alias = true;
      for (int64_t i = 0; alias && i < static_cast<int64_t>(sizeof(RESERVED) / sizeof(RESERVED[0])); ++i) {
        if (fsq_iequal(s + begin, pos - begin, RESERVED[i],
                       static_cast<int64_t>(strlen(RESERVED[i])))) {
          alias = false;
        }
      }
    }
  }
  return alias;
}

// ---------------------------------------------------------------------------
// option validation
// ---------------------------------------------------------------------------

static int fsq_unsupported(const char *what)
{
  LOG_USER_ERROR(OB_NOT_SUPPORTED, what);
  return OB_NOT_SUPPORTED;
}

// the on/off value of enable_file_sql, whatever object type it is stored as
static int fsq_bool_value(const common::ObObj &obj, bool &value)
{
  int ret = OB_SUCCESS;
  value = false;
  if (obj.is_tinyint()) {
    value = (0 != obj.get_tinyint());
  } else if (obj.is_int()) {
    value = (0 != obj.get_int());
  } else if (obj.is_int32()) {
    value = (0 != obj.get_int32());
  } else if (obj.is_uint64()) {
    value = (0 != obj.get_uint64());
  } else if (obj.is_varchar() || obj.is_char()) {
    const common::ObString str = obj.get_string();
    value = (str.length() > 0
             && ('1' == str.ptr()[0] || 'o' == fsq_lower(str.ptr()[0])
                 || 't' == fsq_lower(str.ptr()[0])));
  } else if (obj.is_null()) {
    value = false;
  } else {
    ret = OB_OBJ_TYPE_ERROR;
  }
  return ret;
}

static int fsq_syntax_error(const char *s, const int64_t begin, const int64_t end)
{
  const int32_t str_len = static_cast<int32_t>(end - begin);
  LOG_USER_ERROR(OB_ERR_PARSE_SQL, ob_errpkt_strerror(OB_ERR_PARSER_SYNTAX),
                 str_len, s + begin, 1);
  return OB_ERR_PARSE_SQL;
}

static int fsq_check_format(CallSpec &spec, const char *s, int64_t begin, int64_t end)
{
  int ret = OB_SUCCESS;
  if (end > begin && ('\'' == s[begin] || '"' == s[begin])) {
    ++begin;
    if (end > begin && (s[end - 1] == '\'' || s[end - 1] == '"')) {
      --end;
    }
  }
  const char *p = s + begin;
  const int64_t l = end - begin;
  if (ObFileFormat::FILE_FORMAT_CSV == spec.fmt_) {
    if (fsq_iequal(p, l, "csv", 3) || fsq_iequal(p, l, "auto", 4)) {
      // defaults
    } else if (fsq_iequal(p, l, "csv_with_names", 15)
               || fsq_iequal(p, l, "csv_with_names_and_types", 25)) {
      spec.csv_.has_header_ = true;
    } else {
      ret = fsq_syntax_error(s, begin, end);
      LOG_WARN("unsupported csv format argument", K(ret));
    }
  } else if (ObFileFormat::FILE_FORMAT_JSON == spec.fmt_
             || ObFileFormat::FILE_FORMAT_NDJSON == spec.fmt_) {
    if (fsq_iequal(p, l, "newline_delimited", 17) || fsq_iequal(p, l, "ndjson", 6)
        || fsq_iequal(p, l, "auto", 4) || fsq_iequal(p, l, "json", 4)) {
      spec.fmt_ = ObFileFormat::FILE_FORMAT_NDJSON;
    } else if (fsq_iequal(p, l, "array", 5)) {
      ret = fsq_unsupported("the array json format");
    } else {
      ret = fsq_syntax_error(s, begin, end);
      LOG_WARN("unsupported json format argument", K(ret));
    }
  } else {
    ret = fsq_syntax_error(s, begin, end);
  }
  return ret;
}

// single quoted character option value, e.g. delimiter = ';'
static int fsq_char_value(const char *s, int64_t begin, int64_t end, char &value)
{
  int ret = OB_SUCCESS;
  if (end > begin && ('\'' == s[begin] || '"' == s[begin])) {
    ++begin;
    if (end > begin && (s[end - 1] == '\'' || s[end - 1] == '"')) {
      --end;
    }
  }
  if (end - begin != 1) {
    ret = OB_ERR_PARSE_SQL;
  } else {
    value = s[begin];
  }
  return ret;
}

static bool fsq_parse_int(const char *p, const int64_t len, int64_t &value)
{
  bool ok = false;
  int64_t i = 0;
  int64_t sign = 1;
  if (len > 0 && ('-' == p[0] || '+' == p[0])) {
    sign = ('-' == p[0]) ? -1 : 1;
    i = 1;
  }
  if (i < len) {
    int64_t v = 0;
    ok = true;
    for (; ok && i < len; ++i) {
      ok = (p[i] >= '0' && p[i] <= '9');
      if (ok) {
        v = v * 10 + (p[i] - '0');
        if (v > 1000000000000LL) { ok = false; }
      }
    }
    if (ok) { value = sign * v; }
  }
  return ok;
}

static int fsq_parse_bool(const char *p, const int64_t len, bool &value)
{
  int ret = OB_SUCCESS;
  if (fsq_iequal(p, len, "true", 4) || fsq_iequal(p, len, "on", 2)
      || fsq_iequal(p, len, "1", 1)) {
    value = true;
  } else if (fsq_iequal(p, len, "false", 5) || fsq_iequal(p, len, "off", 3)
             || fsq_iequal(p, len, "0", 1)) {
    value = false;
  } else {
    ret = OB_ERR_PARSE_SQL;
  }
  return ret;
}

// ---------------------------------------------------------------------------
// one call: name, arguments, validation
// ---------------------------------------------------------------------------

static int fsq_parse_call(common::ObIAllocator &alloc, const char *s, const int64_t len,
                          const int64_t name_pos, const int64_t name_len,
                          CallSpec &spec, int64_t &end_pos)
{
  int ret = OB_SUCCESS;
  int64_t open = name_pos + name_len;
  int64_t close = -1;
  if (fsq_iequal(s + name_pos, name_len, "read_csv", 8)) {
    spec.fmt_ = ObFileFormat::FILE_FORMAT_CSV;
  } else if (fsq_iequal(s + name_pos, name_len, "read_json", 9)
             || fsq_iequal(s + name_pos, name_len, "read_ndjson", 11)) {
    spec.fmt_ = ObFileFormat::FILE_FORMAT_NDJSON;
  } else if (fsq_iequal(s + name_pos, name_len, "read_parquet", 12)) {
    spec.fmt_ = ObFileFormat::FILE_FORMAT_PARQUET;
  } else if (fsq_iequal(s + name_pos, name_len, "glob", 4)) {
    spec.is_glob_ = true;
  } else {
    ret = OB_ERR_PARSE_SQL;
  }
  if (OB_SUCC(ret)) {
    fsq_skip_blanks(s, len, open);
    if (open >= len || '(' != s[open]) {
      ret = OB_ERR_PARSE_SQL;
    } else if (OB_FAIL(fsq_find_close(s, len, open, close))) {
    } else {
      end_pos = close + 1;
    }
  }
  common::ObSEArray<ArgSeg, 16> segs;
  if (OB_SUCC(ret) && OB_FAIL(fsq_split_args(s, open + 1, close, segs))) {
  }
  bool saw_positional_path = false;
  for (int64_t i = 0; OB_SUCC(ret) && i < segs.count(); ++i) {
    ArgSeg seg = segs.at(i);
    fsq_trim(s, seg);
    if (seg.begin_ >= seg.end_) {
      continue;
    }
    const int64_t eq = fsq_top_eq(s, seg);
    ArgSeg name;
    ArgSeg val;
    if (eq > seg.begin_) {
      name.begin_ = seg.begin_;
      name.end_ = eq;
      val.begin_ = eq + 1;
      val.end_ = seg.end_;
      fsq_trim(s, name);
      fsq_trim(s, val);
    }
    if (eq > seg.begin_) {
      const bool is_format = fsq_iequal(s + name.begin_, name.end_ - name.begin_, "format", 6);
      const bool is_header = fsq_iequal(s + name.begin_, name.end_ - name.begin_, "header", 6);
      const bool is_sample = fsq_iequal(s + name.begin_, name.end_ - name.begin_, "sample_size", 11);
      const bool is_delim = fsq_iequal(s + name.begin_, name.end_ - name.begin_, "delimiter", 9)
                            || fsq_iequal(s + name.begin_, name.end_ - name.begin_, "sep", 3);
      const bool is_quote = fsq_iequal(s + name.begin_, name.end_ - name.begin_, "quote", 5);
      const bool is_escape = fsq_iequal(s + name.begin_, name.end_ - name.begin_, "escape", 6);
      if (is_format) {
        ret = fsq_check_format(spec, s, val.begin_, val.end_);
      } else if (is_header) {
        bool header = true;
        if (OB_FAIL(fsq_parse_bool(s + val.begin_, val.end_ - val.begin_, header))) {
          ret = fsq_syntax_error(s, val.begin_, val.end_);
        } else {
          spec.csv_.has_header_ = header;
        }
      } else if (is_sample) {
        int64_t value = 0;
        if (!fsq_parse_int(s + val.begin_, val.end_ - val.begin_, value)) {
          ret = fsq_syntax_error(s, val.begin_, val.end_);
        } else if (-1 != value) {
          ret = fsq_unsupported("the sample_size argument, only -1 is accepted");
        } else {
          spec.sample_size_ = value;
        }
      } else if (is_delim || is_quote || is_escape) {
        char c = 0;
        if (OB_FAIL(fsq_char_value(s, val.begin_, val.end_, c))) {
          ret = fsq_syntax_error(s, val.begin_, val.end_);
        } else if (is_delim) {
          spec.csv_.delim_ = c;
        } else if (is_quote) {
          spec.csv_.quote_ = c;
        } else {
          spec.csv_.escape_ = c;
        }
      } else {
        // unknown named argument: ignored on purpose, the readers only use
        // what they understand
      }
    } else {
      // positional argument: the first one is the path, a second string is the
      // format, the way DuckDB spells it
      if (seg.begin_ >= seg.end_) {
        ret = OB_ERR_PARSE_SQL;
      } else if ('\'' == s[seg.begin_] || '"' == s[seg.begin_]) {
        if (!saw_positional_path) {
          common::ObString raw;
          char *nul = NULL;
          int64_t pos = seg.begin_;
          if (OB_FAIL(fsq_parse_string_arg(alloc, s, len, pos, raw, nul))) {
          } else {
            saw_positional_path = true;
            spec.path_ = raw;
            spec.path_buf_ = nul;
          }
        } else if (OB_FAIL(fsq_check_format(spec, s, seg.begin_, seg.end_))) {
        }
      } else {
        ret = fsq_syntax_error(s, seg.begin_, seg.end_);
      }
    }
  }
  if (OB_SUCC(ret) && (NULL == spec.path_buf_ || 0 == spec.path_.length())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("file table function without a path", K(ret));
  }
  return ret;
}

// ---------------------------------------------------------------------------
// loading the file behind one call
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Parquet: thrift compact footer, page headers, snappy, plain/dictionary data.
// Only what a flat (non repeated) schema needs is implemented; the decoder is
// deliberately self contained because no parquet library is available here.
// ---------------------------------------------------------------------------

struct FsqPqCursor
{
  FsqPqCursor(const char *b, const int64_t n) : p_(b), e_(b + n), bad_(false) {}
  bool eof() const { return p_ >= e_; }
  uint8_t u8()
  {
    uint8_t v = 0;
    if (p_ >= e_) {
      bad_ = true;
    } else {
      v = static_cast<uint8_t>(*p_++);
    }
    return v;
  }
  uint64_t uvarint()
  {
    uint64_t v = 0;
    int shift = 0;
    while (!bad_ && p_ < e_ && shift < 64) {
      const uint8_t b = static_cast<uint8_t>(*p_++);
      v |= static_cast<uint64_t>(b & 0x7f) << shift;
      if (0 == (b & 0x80)) { break; }
      shift += 7;
    }
    return v;
  }
  int64_t zigzag()
  {
    const uint64_t u = uvarint();
    return static_cast<int64_t>(u >> 1) ^ -static_cast<int64_t>(u & 1);
  }
  int binary(const char *&ptr, int64_t &len)
  {
    int ret = OB_SUCCESS;
    const uint64_t n = uvarint();
    if (bad_ || n > static_cast<uint64_t>(e_ - p_)) {
      bad_ = true;
      ret = OB_ERR_UNEXPECTED;
    } else {
      ptr = p_;
      len = static_cast<int64_t>(n);
      p_ += n;
    }
    return ret;
  }
  // returns the element type of a list header
  int list(uint64_t &size)
  {
    const uint8_t h = u8();
    size = (h >> 4) & 0x0f;
    if (15 == size) { size = uvarint(); }
    return bad_ ? OB_ERR_UNEXPECTED : OB_SUCCESS;
  }
  int skip(int type)
  {
    int ret = OB_SUCCESS;
    switch (type) {
      case 1: case 2: break;
      case 3: u8(); break;
      case 4: case 5: case 6: uvarint(); break;
      case 7: if (e_ - p_ >= 8) { p_ += 8; } else { bad_ = true; } break;
      case 8: { const char *ptr = NULL; int64_t len = 0; ret = binary(ptr, len); break; }
      case 9: case 10: {
        uint64_t size = 0;
        if (OB_FAIL(list(size))) {
        } else {
          const uint8_t h = 0;  // element type is inside the header already
          UNUSED(h);
        }
        break;
      }
      default: bad_ = true; break;
    }
    return ret;
  }
  const char *p_;
  const char *e_;
  bool bad_;
};

// list skip needs the element type, keep it separate from skip()
static int fsq_pq_skip(FsqPqCursor &c, const int type);

static int fsq_pq_skip_list(FsqPqCursor &c)
{
  int ret = OB_SUCCESS;
  const uint8_t h = c.u8();
  uint64_t size = (h >> 4) & 0x0f;
  const int etype = h & 0x0f;
  if (15 == size) { size = c.uvarint(); }
  for (uint64_t i = 0; OB_SUCC(ret) && i < size; ++i) {
    ret = fsq_pq_skip(c, etype);
    if (OB_FAIL(ret) || c.bad_) { break; }
  }
  return ret;
}

static int fsq_pq_skip(FsqPqCursor &c, const int type)
{
  int ret = OB_SUCCESS;
  switch (type) {
    case 1: case 2: break;
    case 3: c.u8(); break;
    case 4: case 5: case 6: c.uvarint(); break;
    case 7: if (c.e_ - c.p_ >= 8) { c.p_ += 8; } else { c.bad_ = true; } break;
    case 8: { const char *ptr = NULL; int64_t len = 0; ret = c.binary(ptr, len); break; }
    case 9: case 10: ret = fsq_pq_skip_list(c); break;
    case 11: {
      uint64_t n = c.uvarint();
      if (n > 0) {
        const uint8_t kv = c.u8();
        const int kt = (kv >> 4) & 0x0f;
        const int vt = kv & 0x0f;
        for (uint64_t i = 0; OB_SUCC(ret) && i < n; ++i) {
          if (OB_FAIL(fsq_pq_skip(c, kt))) {
          } else {
            ret = fsq_pq_skip(c, vt);
          }
        }
      }
      break;
    }
    case 12: {
      int last = 0;
      bool done = false;
      while (!done && !c.bad_) {
        const uint8_t b = c.u8();
        if (0 == b) {
          done = true;
        } else {
          const int ft = b & 0x0f;
          const int delta = (b >> 4) & 0x0f;
          last = delta ? last + delta : static_cast<int>(c.zigzag());
          ret = fsq_pq_skip(c, ft);
          if (OB_FAIL(ret)) { break; }
        }
      }
      break;
    }
    default: c.bad_ = true; ret = OB_ERR_UNEXPECTED; break;
  }
  return ret;
}

struct FsqPqCol
{
  FsqPqCol() : type_(-1), repetition_(0), converted_(0), name_(NULL), name_len_(0) {}
  int32_t type_;
  int32_t repetition_;
  int32_t converted_;
  const char *name_;
  int64_t name_len_;
};

struct FsqPqChunk
{
  FsqPqChunk()
      : type_(0), codec_(0), num_values_(0), data_page_offset_(-1), dict_page_offset_(-1)
  {}
  int32_t type_;
  int32_t codec_;
  int64_t num_values_;
  int64_t data_page_offset_;
  int64_t dict_page_offset_;
};

struct FsqPqGroup
{
  FsqPqGroup() : first_chunk_(0), chunk_count_(0), num_rows_(0) {}
  int64_t first_chunk_;
  int64_t chunk_count_;
  int64_t num_rows_;
};

struct FsqPqPage
{
  FsqPqPage()
      : type_(0), uncompressed_size_(0), compressed_size_(0), num_values_(0),
        encoding_(0), def_len_(0), rep_len_(0), is_compressed_(1), dict_num_values_(0)
  {}
  int32_t type_;
  int32_t uncompressed_size_;
  int32_t compressed_size_;
  int32_t num_values_;
  int32_t encoding_;
  int32_t def_len_;
  int32_t rep_len_;
  int32_t is_compressed_;
  int32_t dict_num_values_;
};

static int fsq_pq_schema_element(FsqPqCursor &c, FsqPqCol &out)
{
  int ret = OB_SUCCESS;
  int last = 0;
  bool done = false;
  while (OB_SUCC(ret) && !done && !c.bad_) {
    const uint8_t b = c.u8();
    if (0 == b) {
      done = true;
    } else {
      const int ft = b & 0x0f;
      const int delta = (b >> 4) & 0x0f;
      const int id = delta ? last + delta : static_cast<int>(c.zigzag());
      last = id;
      if (1 == id) {
        out.type_ = static_cast<int32_t>(c.zigzag());
      } else if (3 == id) {
        out.repetition_ = static_cast<int32_t>(c.zigzag());
      } else if (4 == id) {
        ret = c.binary(out.name_, out.name_len_);
      } else if (6 == id) {
        out.converted_ = static_cast<int32_t>(c.zigzag());
      } else {
        ret = fsq_pq_skip(c, ft);
      }
    }
  }
  return ret;
}

static int fsq_pq_column_meta(FsqPqCursor &c, FsqPqChunk &out)
{
  int ret = OB_SUCCESS;
  int last = 0;
  bool done = false;
  while (OB_SUCC(ret) && !done && !c.bad_) {
    const uint8_t b = c.u8();
    if (0 == b) {
      done = true;
    } else {
      const int ft = b & 0x0f;
      const int delta = (b >> 4) & 0x0f;
      const int id = delta ? last + delta : static_cast<int>(c.zigzag());
      last = id;
      if (1 == id) {
        out.type_ = static_cast<int32_t>(c.zigzag());
      } else if (3 == id) {         // path_in_schema
        uint64_t n = 0;
        if (OB_FAIL(c.list(n))) {
        }
        for (uint64_t i = 0; OB_SUCC(ret) && i < n; ++i) {
          const char *ptr = NULL;
          int64_t len = 0;
          ret = c.binary(ptr, len);
        }
      } else if (4 == id) {
        out.codec_ = static_cast<int32_t>(c.zigzag());
      } else if (5 == id) {
        out.num_values_ = c.zigzag();
      } else if (9 == id) {
        out.data_page_offset_ = c.zigzag();
      } else if (11 == id) {
        out.dict_page_offset_ = c.zigzag();
      } else {
        ret = fsq_pq_skip(c, ft);
      }
    }
  }
  return ret;
}

static int fsq_pq_column_chunk(FsqPqCursor &c, FsqPqChunk &out)
{
  int ret = OB_SUCCESS;
  int last = 0;
  bool done = false;
  while (OB_SUCC(ret) && !done && !c.bad_) {
    const uint8_t b = c.u8();
    if (0 == b) {
      done = true;
    } else {
      const int ft = b & 0x0f;
      const int delta = (b >> 4) & 0x0f;
      const int id = delta ? last + delta : static_cast<int>(c.zigzag());
      last = id;
      if (3 == id) {
        ret = fsq_pq_column_meta(c, out);
      } else {
        ret = fsq_pq_skip(c, ft);
      }
    }
  }
  return ret;
}

static int fsq_pq_row_group(FsqPqCursor &c, common::ObIArray<FsqPqChunk> &chunks,
                            FsqPqGroup &out)
{
  int ret = OB_SUCCESS;
  int last = 0;
  bool done = false;
  while (OB_SUCC(ret) && !done && !c.bad_) {
    const uint8_t b = c.u8();
    if (0 == b) {
      done = true;
    } else {
      const int ft = b & 0x0f;
      const int delta = (b >> 4) & 0x0f;
      const int id = delta ? last + delta : static_cast<int>(c.zigzag());
      last = id;
      if (1 == id) {
        uint64_t n = 0;
        out.first_chunk_ = chunks.count();
        if (OB_FAIL(c.list(n))) {
        }
        for (uint64_t i = 0; OB_SUCC(ret) && i < n; ++i) {
          FsqPqChunk chunk;
          if (OB_FAIL(fsq_pq_column_chunk(c, chunk))) {
          } else {
            ret = chunks.push_back(chunk);
          }
        }
        out.chunk_count_ = chunks.count() - out.first_chunk_;
      } else if (3 == id) {
        out.num_rows_ = c.zigzag();
      } else {
        ret = fsq_pq_skip(c, ft);
      }
    }
  }
  return ret;
}

static int fsq_pq_page_header(FsqPqCursor &c, FsqPqPage &out)
{
  int ret = OB_SUCCESS;
  int last = 0;
  bool done = false;
  while (OB_SUCC(ret) && !done && !c.bad_) {
    const uint8_t b = c.u8();
    if (0 == b) {
      done = true;
    } else {
      const int ft = b & 0x0f;
      const int delta = (b >> 4) & 0x0f;
      const int id = delta ? last + delta : static_cast<int>(c.zigzag());
      last = id;
      if (1 == id) {
        out.type_ = static_cast<int32_t>(c.zigzag());
      } else if (2 == id) {
        out.uncompressed_size_ = static_cast<int32_t>(c.zigzag());
      } else if (3 == id) {
        out.compressed_size_ = static_cast<int32_t>(c.zigzag());
      } else if (5 == id || 7 == id || 8 == id) {
        // DataPageHeader / DictionaryPageHeader / DataPageHeaderV2
        int sub_last = 0;
        bool sub_done = false;
        while (OB_SUCC(ret) && !sub_done && !c.bad_) {
          const uint8_t sb = c.u8();
          if (0 == sb) {
            sub_done = true;
          } else {
            const int sft = sb & 0x0f;
            const int sdelta = (sb >> 4) & 0x0f;
            const int sid = sdelta ? sub_last + sdelta : static_cast<int>(c.zigzag());
            sub_last = sid;
            if (5 == id && 1 == sid) {
              out.num_values_ = static_cast<int32_t>(c.zigzag());
            } else if (5 == id && 2 == sid) {
              out.encoding_ = static_cast<int32_t>(c.zigzag());
            } else if (7 == id && 1 == sid) {
              out.dict_num_values_ = static_cast<int32_t>(c.zigzag());
            } else if (7 == id && 2 == sid) {
              out.encoding_ = static_cast<int32_t>(c.zigzag());
            } else if (8 == id && 1 == sid) {
              out.num_values_ = static_cast<int32_t>(c.zigzag());
            } else if (8 == id && 4 == sid) {
              out.encoding_ = static_cast<int32_t>(c.zigzag());
            } else if (8 == id && 5 == sid) {
              out.def_len_ = static_cast<int32_t>(c.zigzag());
            } else if (8 == id && 6 == sid) {
              out.rep_len_ = static_cast<int32_t>(c.zigzag());
            } else if (8 == id && 7 == sid) {
              out.is_compressed_ = (1 == sft) ? 1 : 0;
            } else {
              ret = fsq_pq_skip(c, sft);
            }
          }
        }
      } else {
        ret = fsq_pq_skip(c, ft);
      }
    }
  }
  return ret;
}

// raw snappy block decompression
static int fsq_snappy(const char *in, const int64_t in_len, char *out, const int64_t out_cap,
                      int64_t &out_len)
{
  int ret = OB_SUCCESS;
  out_len = 0;
  int64_t ip = 0;
  uint64_t expect = 0;
  int shift = 0;
  bool done_len = false;
  while (!done_len && ip < in_len) {
    const uint8_t b = static_cast<uint8_t>(in[ip++]);
    expect |= static_cast<uint64_t>(b & 0x7f) << shift;
    if (0 == (b & 0x80)) {
      done_len = true;
    } else if (shift > 32) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      shift += 7;
    }
  }
  if (OB_FAIL(ret)) {
  } else if (static_cast<int64_t>(expect) > out_cap) {
    ret = OB_SIZE_OVERFLOW;
  } else {
    int64_t op = 0;
    while (OB_SUCC(ret) && ip < in_len) {
      const uint8_t tag = static_cast<uint8_t>(in[ip++]);
      const int type = tag & 0x03;
      if (0 == type) {
        int64_t len = (tag >> 2) + 1;
        if (len > 60) {
          const int extra = static_cast<int>(len) - 60;
          int64_t v = 0;
          for (int i = 0; i < extra; ++i) {
            if (ip >= in_len) { ret = OB_ERR_UNEXPECTED; break; }
            v |= static_cast<int64_t>(static_cast<uint8_t>(in[ip++])) << (8 * i);
          }
          len = v + 1;
        }
        if (OB_FAIL(ret)) {
        } else if (ip + len > in_len || op + len > out_cap) {
          ret = OB_ERR_UNEXPECTED;
        } else {
          MEMCPY(out + op, in + ip, len);
          ip += len;
          op += len;
        }
      } else {
        int64_t len = 0;
        int64_t offset = 0;
        if (1 == type) {
          if (ip >= in_len) {
            ret = OB_ERR_UNEXPECTED;
          } else {
            len = 4 + ((tag >> 2) & 0x07);
            offset = (static_cast<int64_t>(tag >> 5) << 8)
                     | static_cast<uint8_t>(in[ip++]);
          }
        } else if (2 == type) {
          if (ip + 2 > in_len) {
            ret = OB_ERR_UNEXPECTED;
          } else {
            len = 1 + (tag >> 2);
            offset = static_cast<int64_t>(static_cast<uint8_t>(in[ip]))
                     | (static_cast<int64_t>(static_cast<uint8_t>(in[ip + 1])) << 8);
            ip += 2;
          }
        } else {
          if (ip + 4 > in_len) {
            ret = OB_ERR_UNEXPECTED;
          } else {
            len = 1 + (tag >> 2);
            offset = static_cast<int64_t>(static_cast<uint8_t>(in[ip]))
                     | (static_cast<int64_t>(static_cast<uint8_t>(in[ip + 1])) << 8)
                     | (static_cast<int64_t>(static_cast<uint8_t>(in[ip + 2])) << 16)
                     | (static_cast<int64_t>(static_cast<uint8_t>(in[ip + 3])) << 24);
            ip += 4;
          }
        }
        if (OB_FAIL(ret)) {
        } else if (offset <= 0 || offset > op || op + len > out_cap) {
          ret = OB_ERR_UNEXPECTED;
        } else {
          for (int64_t i = 0; OB_SUCC(ret) && i < len; ++i) {
            out[op] = out[op - offset];
            ++op;
          }
        }
      }
    }
    if (OB_SUCC(ret)) {
      out_len = op;
    }
  }
  return ret;
}

// RLE / bit-packed hybrid
static int fsq_pq_hybrid(const char *data, const int64_t size, int width,
                         const int64_t count, common::ObIArray<int64_t> &out)
{
  int ret = OB_SUCCESS;
  int64_t pos = 0;
  while (OB_SUCC(ret) && out.count() < count && pos < size) {
    uint64_t header = 0;
    int shift = 0;
    bool done = false;
    while (!done && pos < size) {
      const uint8_t b = static_cast<uint8_t>(data[pos++]);
      header |= static_cast<uint64_t>(b & 0x7f) << shift;
      if (0 == (b & 0x80)) {
        done = true;
      } else if (shift > 56) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        shift += 7;
      }
    }
    if (OB_FAIL(ret)) {
    } else if (0 == (header & 1)) {
      const int64_t run = static_cast<int64_t>(header >> 1);
      const int nbytes = (width + 7) / 8;
      int64_t value = 0;
      for (int i = 0; i < nbytes; ++i) {
        if (pos < size) {
          value |= static_cast<int64_t>(static_cast<uint8_t>(data[pos++])) << (8 * i);
        }
      }
      for (int64_t i = 0; OB_SUCC(ret) && i < run && out.count() < count; ++i) {
        ret = out.push_back(value);
      }
    } else {
      const int64_t groups = static_cast<int64_t>(header >> 1);
      const int64_t nvals = groups * 8;
      const int64_t nbytes = groups * width;
      for (int64_t v = 0; OB_SUCC(ret) && v < nvals && out.count() < count; ++v) {
        const int64_t bit = v * width;
        int64_t value = 0;
        for (int b = 0; OB_SUCC(ret) && b < width; ++b) {
          const int64_t byte_off = pos + (bit + b) / 8;
          if (byte_off >= size) {
            ret = OB_ERR_UNEXPECTED;
          } else {
            const int bit_in_byte = static_cast<int>((bit + b) % 8);
            if (static_cast<uint8_t>(data[byte_off]) & (1 << bit_in_byte)) {
              value |= static_cast<int64_t>(1) << b;
            }
          }
        }
        if (OB_SUCC(ret)) {
          ret = out.push_back(value);
        }
      }
      pos += nbytes;
      if (pos > size) { pos = size; }
    }
  }
  return ret;
}

static int32_t fsq_pq_i32(const char *p)
{
  int32_t v = 0;
  MEMCPY(&v, p, 4);
  return v;
}

static int64_t fsq_pq_i64(const char *p)
{
  int64_t v = 0;
  MEMCPY(&v, p, 8);
  return v;
}

// one PLAIN value formatted as text into the statement allocator
static int fsq_pq_plain_value(common::ObIAllocator &alloc, const int ptype,
                              const int converted, const char *p, const int64_t psize,
                              int64_t &pos, common::ObString &out)
{
  int ret = OB_SUCCESS;
  out.reset();
  char tmp[64];
  int n = 0;
  if (1 == ptype) {                    // INT32
    if (pos + 4 > psize) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      const int32_t v = fsq_pq_i32(p + pos);
      pos += 4;
      if (6 == converted) {            // DATE: days since the epoch
        int64_t days = v;
        int64_t z = days + 719468;
        int64_t era = (z >= 0 ? z : z - 146096) / 146097;
        int64_t doe = z - era * 146097;
        int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
        int64_t y = yoe + era * 400;
        int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
        int64_t mp = (5 * doy + 2) / 153;
        int64_t d = doy - (153 * mp + 2) / 5 + 1;
        int64_t m = mp < 10 ? mp + 3 : mp - 9;
        if (m <= 2) { ++y; }
        n = snprintf(tmp, sizeof(tmp), "%04ld-%02ld-%02ld",
                     static_cast<long>(y), static_cast<long>(m), static_cast<long>(d));
      } else {
        n = snprintf(tmp, sizeof(tmp), "%d", v);
      }
    }
  } else if (2 == ptype) {             // INT64
    if (pos + 8 > psize) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      n = snprintf(tmp, sizeof(tmp), "%ld", static_cast<long>(fsq_pq_i64(p + pos)));
      pos += 8;
    }
  } else if (5 == ptype) {             // DOUBLE
    if (pos + 8 > psize) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      double v = 0;
      MEMCPY(&v, p + pos, 8);
      n = snprintf(tmp, sizeof(tmp), "%.17g", v);
      pos += 8;
    }
  } else if (6 == ptype) {             // BYTE_ARRAY
    if (pos + 4 > psize) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      const int32_t len = fsq_pq_i32(p + pos);
      pos += 4;
      if (len < 0 || pos + len > psize) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        char *buf = static_cast<char *>(alloc.alloc(len + 1));
        if (NULL == buf) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
        } else {
          MEMCPY(buf, p + pos, len);
          buf[len] = '\0';
          out.assign_ptr(buf, len);
          pos += len;
        }
      }
    }
  } else {
    ret = fsq_unsupported("this parquet physical type");
  }
  if (OB_SUCC(ret) && 6 != ptype) {
    if (n <= 0) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      char *buf = static_cast<char *>(alloc.alloc(n + 1));
      if (NULL == buf) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else {
        MEMCPY(buf, tmp, n);
        buf[n] = '\0';
        out.assign_ptr(buf, n);
      }
    }
  }
  return ret;
}

static int fsq_pq_load_column(common::ObIAllocator &alloc,
                              const char *data, const int64_t size,
                              const FsqPqChunk &chunk, const FsqPqCol &schema,
                              common::ObIArray<common::ObString> &values)
{
  int ret = OB_SUCCESS;
  const bool optional = (1 == schema.repetition_);
  common::ObSEArray<common::ObString, 32> dict;
  int64_t pos = (chunk.dict_page_offset_ >= 0
                 && (chunk.data_page_offset_ < 0
                     || chunk.dict_page_offset_ < chunk.data_page_offset_))
                ? chunk.dict_page_offset_ : chunk.data_page_offset_;
  int64_t produced = 0;
  while (OB_SUCC(ret) && produced < chunk.num_values_ && pos >= 0 && pos < size - 8) {
    FsqPqCursor cur(data + pos, size - pos);
    FsqPqPage page;
    if (OB_FAIL(fsq_pq_page_header(cur, page))) {
    } else if (cur.bad_) {
      ret = OB_ERR_UNEXPECTED;
    } else if (page.compressed_size_ <= 0) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      const int64_t body = cur.p_ - data;
      if (body + page.compressed_size_ > size) {
        ret = OB_ERR_UNEXPECTED;
      } else if (2 == page.type_ || 1 == page.type_) {
        // dictionary page
        char *ubuf = NULL;
        const char *pd = data + body;
        int64_t psize = page.compressed_size_;
        if (1 == chunk.codec_) {
          const int64_t cap = page.uncompressed_size_ > 0 ? page.uncompressed_size_ : 0;
          ubuf = static_cast<char *>(alloc.alloc(cap > 0 ? cap : 1));
          if (NULL == ubuf) {
            ret = OB_ALLOCATE_MEMORY_FAILED;
          } else {
            int64_t got = 0;
            ret = fsq_snappy(pd, psize, ubuf, cap, got);
            pd = ubuf;
            psize = got;
          }
        } else if (0 != chunk.codec_) {
          ret = fsq_unsupported("this parquet compression codec");
        }
        int64_t p = 0;
        dict.reuse();
        for (int32_t i = 0; OB_SUCC(ret) && i < page.dict_num_values_; ++i) {
          common::ObString v;
          if (OB_FAIL(fsq_pq_plain_value(alloc, chunk.type_, schema.converted_,
                                         pd, psize, p, v))) {
          } else {
            ret = dict.push_back(v);
          }
        }
      } else if (0 == page.type_ || 3 == page.type_) {
        const int64_t nvalues = page.num_values_;
        common::ObSEArray<int64_t, 32> defs;
        char *ubuf = NULL;
        const char *vp = NULL;
        int64_t vsize = 0;
        if (3 == page.type_) {
          // v2: levels are not compressed, only the values are
          const int64_t level_bytes = page.rep_len_ + page.def_len_;
          if (page.compressed_size_ < level_bytes || page.uncompressed_size_ < level_bytes) {
            ret = OB_ERR_UNEXPECTED;
          } else if (optional && page.def_len_ > 0
                     && OB_FAIL(fsq_pq_hybrid(data + body + page.rep_len_, page.def_len_, 1,
                                              nvalues, defs))) {
          } else {
            const char *vc = data + body + level_bytes;
            const int64_t vcsize = page.compressed_size_ - level_bytes;
            const int64_t vusize = page.uncompressed_size_ - level_bytes;
            if (1 == page.is_compressed_ && 1 == chunk.codec_) {
              ubuf = static_cast<char *>(alloc.alloc(vusize > 0 ? vusize : 1));
              if (NULL == ubuf) {
                ret = OB_ALLOCATE_MEMORY_FAILED;
              } else {
                int64_t got = 0;
                ret = fsq_snappy(vc, vcsize, ubuf, vusize, got);
                vp = ubuf;
                vsize = got;
              }
            } else if (1 == page.is_compressed_ && 0 != chunk.codec_) {
              ret = fsq_unsupported("this parquet compression codec");
            } else {
              vp = vc;
              vsize = vcsize;
            }
          }
        } else {
          const char *pd = data + body;
          int64_t psize = page.compressed_size_;
          if (1 == chunk.codec_) {
            const int64_t cap = page.uncompressed_size_ > 0 ? page.uncompressed_size_ : 0;
            ubuf = static_cast<char *>(alloc.alloc(cap > 0 ? cap : 1));
            if (NULL == ubuf) {
              ret = OB_ALLOCATE_MEMORY_FAILED;
            } else {
              int64_t got = 0;
              ret = fsq_snappy(pd, psize, ubuf, cap, got);
              pd = ubuf;
              psize = got;
            }
          } else if (0 != chunk.codec_) {
            ret = fsq_unsupported("this parquet compression codec");
          }
          int64_t p = 0;
          if (OB_FAIL(ret)) {
          } else if (2 == schema.repetition_ && p + 4 <= psize) {
            const int32_t l = fsq_pq_i32(pd + p);
            p += 4 + l;
          }
          if (OB_FAIL(ret)) {
          } else if (optional) {
            if (p + 4 > psize) {
              ret = OB_ERR_UNEXPECTED;
            } else {
              const int32_t len = fsq_pq_i32(pd + p);
              p += 4;
              if (len < 0 || p + len > psize) {
                ret = OB_ERR_UNEXPECTED;
              } else {
                ret = fsq_pq_hybrid(pd + p, len, 1, nvalues, defs);
                p += len;
              }
            }
          }
          if (OB_SUCC(ret)) {
            vp = pd + p;
            vsize = psize - p;
          }
        }
        int64_t present = nvalues;
        if (OB_SUCC(ret) && optional) {
          present = 0;
          for (int64_t i = 0; i < defs.count(); ++i) {
            if (1 == defs.at(i)) { ++present; }
          }
        }
        common::ObSEArray<common::ObString, 32> vals;
        int64_t voff = 0;
        if (OB_FAIL(ret)) {
        } else if (8 == page.encoding_ || 2 == page.encoding_) {
          if (voff >= vsize) {
            ret = OB_ERR_UNEXPECTED;
          } else {
            const int width = static_cast<uint8_t>(vp[voff++]);
            common::ObSEArray<int64_t, 32> idx;
            if (OB_FAIL(fsq_pq_hybrid(vp + voff, vsize - voff, width, present, idx))) {
            }
            for (int64_t i = 0; OB_SUCC(ret) && i < idx.count(); ++i) {
              const int64_t id = idx.at(i);
              ret = vals.push_back((id >= 0 && id < dict.count())
                                   ? dict.at(id) : common::ObString());
            }
          }
        } else if (0 == page.encoding_) {
          for (int64_t i = 0; OB_SUCC(ret) && i < present; ++i) {
            common::ObString v;
            if (OB_FAIL(fsq_pq_plain_value(alloc, chunk.type_, schema.converted_,
                                           vp, vsize, voff, v))) {
            } else {
              ret = vals.push_back(v);
            }
          }
        } else {
          ret = fsq_unsupported("this parquet value encoding");
        }
        if (OB_SUCC(ret)) {
          int64_t vi = 0;
          for (int64_t i = 0; OB_SUCC(ret) && i < nvalues; ++i) {
            if (!optional || (i < defs.count() && 1 == defs.at(i))) {
              ret = values.push_back(vi < vals.count() ? vals.at(vi) : common::ObString());
              ++vi;
            } else {
              ret = values.push_back(common::ObString());
            }
          }
          produced += nvalues;
        }
      } else {
        // index page: nothing to read
      }
      if (OB_SUCC(ret)) {
        pos = body + page.compressed_size_;
      }
    }
  }
  return ret;
}

static int fsq_parse_parquet(common::ObIAllocator &alloc,
                             const char *data, const int64_t size, Table &t)
{
  int ret = OB_SUCCESS;
  common::ObSEArray<FsqPqCol, 16> cols;
  common::ObSEArray<FsqPqChunk, 16> chunks;
  common::ObSEArray<FsqPqGroup, 8> groups;
  int64_t num_rows = 0;
  if (size < 12 || 0 != MEMCMP(data + size - 4, "PAR1", 4)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("not a parquet file", K(ret));
  } else {
    uint32_t meta_len = 0;
    MEMCPY(&meta_len, data + size - 8, 4);
    if (static_cast<int64_t>(meta_len) + 8 > size) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("bad parquet metadata length", K(ret), K(meta_len));
    } else {
      FsqPqCursor cur(data + size - 8 - meta_len, static_cast<int64_t>(meta_len));
      int last = 0;
      bool done = false;
      while (OB_SUCC(ret) && !done && !cur.bad_) {
        const uint8_t b = cur.u8();
        if (0 == b) {
          done = true;
        } else {
          const int ft = b & 0x0f;
          const int delta = (b >> 4) & 0x0f;
          const int id = delta ? last + delta : static_cast<int>(cur.zigzag());
          last = id;
          if (2 == id) {
            uint64_t n = 0;
            if (OB_FAIL(cur.list(n))) {
            }
            for (uint64_t i = 0; OB_SUCC(ret) && i < n; ++i) {
              FsqPqCol col;
              if (OB_FAIL(fsq_pq_schema_element(cur, col))) {
              } else {
                ret = cols.push_back(col);
              }
            }
          } else if (3 == id) {
            num_rows = cur.zigzag();
          } else if (4 == id) {
            uint64_t n = 0;
            if (OB_FAIL(cur.list(n))) {
            }
            for (uint64_t i = 0; OB_SUCC(ret) && i < n; ++i) {
              FsqPqGroup group;
              if (OB_FAIL(fsq_pq_row_group(cur, chunks, group))) {
              } else {
                ret = groups.push_back(group);
              }
            }
          } else {
            ret = fsq_pq_skip(cur, ft);
          }
        }
      }
      if (OB_SUCC(ret) && cur.bad_) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("parquet footer parse failed", K(ret));
      }
    }
  }
  if (OB_SUCC(ret)) {
    // leaf columns are schema[1..]
    const int64_t leaf = cols.count() - 1;
    if (leaf <= 0) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("parquet file without columns", K(ret));
    }
    for (int64_t c = 0; OB_SUCC(ret) && c < leaf; ++c) {
      const FsqPqCol &sc = cols.at(c + 1);
      char *name = static_cast<char *>(alloc.alloc(sc.name_len_ + 1));
      if (NULL == name) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else {
        MEMCPY(name, sc.name_, sc.name_len_);
        name[sc.name_len_] = '\0';
        ColMeta meta;
        meta.name_.assign_ptr(name, static_cast<int32_t>(sc.name_len_));
        if (OB_FAIL(t.cols_.push_back(meta))) {
        }
      }
    }
    if (OB_SUCC(ret)) {
      t.col_count_ = t.cols_.count();
    }
    // decode row group by row group, column by column
    common::ObSEArray<common::ObSEArray<common::ObString, 32> *, 16> col_values;
    for (int64_t c = 0; OB_SUCC(ret) && c < t.col_count_; ++c) {
      void *buf = alloc.alloc(sizeof(common::ObSEArray<common::ObString, 32>));
      if (NULL == buf) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else {
        common::ObSEArray<common::ObString, 32> *vec
            = new (buf) common::ObSEArray<common::ObString, 32>();
        ret = col_values.push_back(vec);
      }
    }
    for (int64_t g = 0; OB_SUCC(ret) && g < groups.count(); ++g) {
      const FsqPqGroup &group = groups.at(g);
      for (int64_t c = 0; OB_SUCC(ret) && c < t.col_count_; ++c) {
        if (c < group.chunk_count_) {
          const FsqPqChunk &chunk = chunks.at(group.first_chunk_ + c);
          if (OB_FAIL(fsq_pq_load_column(alloc, data, size, chunk, cols.at(c + 1),
                                         *col_values.at(c)))) {
          }
        }
      }
    }
    // transpose into the row major table
    for (int64_t r = 0; OB_SUCC(ret) && r < num_rows; ++r) {
      for (int64_t c = 0; OB_SUCC(ret) && c < t.col_count_; ++c) {
        common::ObSEArray<common::ObString, 32> *vec = col_values.at(c);
        ret = t.cells_.push_back(r < vec->count() ? vec->at(r) : common::ObString());
      }
    }
  }
  return ret;
}

static int fsq_load_table(common::ObIAllocator &alloc, const CallSpec &spec, Table &t)
{
  int ret = OB_SUCCESS;
  if (spec.is_glob_) {
    if (OB_FAIL(fsq_glob(alloc, spec.path_, t))) {
    }
  } else {
    char *data = NULL;
    int64_t size = 0;
    if (OB_FAIL(fsq_read_file(alloc, spec.path_buf_, data, size))) {
    } else if (ObFileFormat::FILE_FORMAT_CSV == spec.fmt_) {
      if (OB_FAIL(fsq_parse_csv(alloc, data, size, spec.csv_, t))) {
      }
    } else if (ObFileFormat::FILE_FORMAT_NDJSON == spec.fmt_
               || ObFileFormat::FILE_FORMAT_JSON == spec.fmt_) {
      if (OB_FAIL(fsq_parse_ndjson(alloc, data, size, t))) {
      }
    } else if (ObFileFormat::FILE_FORMAT_PARQUET == spec.fmt_) {
      if (OB_FAIL(fsq_parse_parquet(alloc, data, size, t))) {
      }
    } else {
      ret = OB_ERR_PARSE_SQL;
    }
    if (OB_SUCC(ret) && OB_FAIL(fsq_infer(t))) {
    }
  }
  return ret;
}

// ---------------------------------------------------------------------------
// describe select .. from read_csv(..)
// ---------------------------------------------------------------------------

static int fsq_rewrite_describe(common::ObIAllocator &alloc, const bool enabled,
                                const char *s, const int64_t len, common::ObString &out)
{
  int ret = OB_SUCCESS;
  const common::ObString original(static_cast<int32_t>(len), const_cast<char *>(s));
  int64_t pos = 0;
  fsq_skip_spaces(s, len, pos);
  if (fsq_word_at(s, len, pos, "describe", 8)) {
    pos += 8;
  } else {
    pos += 4;
  }
  fsq_skip_spaces(s, len, pos);
  if (!fsq_word_at(s, len, pos, "select", 6)) {
    out = original;
  } else {
    const int64_t sel_begin = pos + 6;
    int64_t from_pos = -1;
    int64_t p = sel_begin;
    int depth = 0;
    while (OB_SUCC(ret) && p < len && from_pos < 0) {
      const char c = s[p];
      if ('\'' == c || '"' == c || '`' == c) {
        fsq_skip_quoted(s, len, p);
      } else {
        if ('(' == c) {
          ++depth;
        } else if (')' == c) {
          --depth;
        } else if (0 == depth && fsq_word_at(s, len, p, "from", 4)) {
          from_pos = p;
        } else {
          ++p;
        }
      }
    }
    int64_t name_pos = -1;
    int64_t name_len = 0;
    bool found = false;
    if (OB_FAIL(ret)) {
    } else if (from_pos < 0) {
      out = original;
    } else {
      int64_t q = from_pos + 4;
      fsq_skip_spaces(s, len, q);
      fsq_find_call(s, len, q, name_pos, name_len, found);
      if (!found || name_pos != q) {
        out = original;
      } else if (!enabled) {
        ret = fsq_unsupported("file sql");
      } else {
        CallSpec spec;
        int64_t end_pos = name_pos + name_len;
        Table t;
        if (OB_FAIL(fsq_parse_call(alloc, s, len, name_pos, name_len, spec, end_pos))) {
        } else if (OB_FAIL(fsq_load_table(alloc, spec, t))) {
        } else {
          // walk the select list
          common::ObSEArray<ArgSeg, 16> items;
          if (OB_FAIL(fsq_split_args(s, sel_begin, from_pos, items))) {
          }
          common::ObSEArray<int64_t, 16> picked;
          for (int64_t i = 0; OB_SUCC(ret) && i < items.count(); ++i) {
            ArgSeg item = items.at(i);
            fsq_trim(s, item);
            if (item.begin_ >= item.end_) {
              continue;
            }
            if (1 == item.end_ - item.begin_ && '*' == s[item.begin_]) {
              for (int64_t c = 0; c < t.cols_.count(); ++c) {
                ret = picked.push_back(c);
              }
            } else if (2 == item.end_ - item.begin_ && '*' == s[item.begin_ + 1]) {
              for (int64_t c = 0; OB_SUCC(ret) && c < t.cols_.count(); ++c) {
                ret = picked.push_back(c);
              }
            } else {
              int64_t nb = item.begin_;
              int64_t ne = item.end_;
              if ('`' == s[nb] && ne > nb + 1 && '`' == s[ne - 1]) {
                ++nb;
                --ne;
              }
              for (int64_t dot = nb; dot < ne; ++dot) {
                if ('.' == s[dot]) {
                  nb = dot + 1;
                  if ('`' == s[nb] && ne > nb + 1 && '`' == s[ne - 1]) {
                    ++nb;
                    --ne;
                  }
                  break;
                }
              }
              bool matched = false;
              for (int64_t c = 0; !matched && c < t.cols_.count(); ++c) {
                if (fsq_iequal(s + nb, ne - nb, t.cols_.at(c).name_.ptr(),
                               t.cols_.at(c).name_.length())) {
                  matched = true;
                  if (OB_FAIL(picked.push_back(c))) {
                  }
                }
              }
              if (OB_SUCC(ret) && !matched) {
                ret = fsq_unsupported("describing a column that is not in the file");
              }
            }
          }
          if (OB_SUCC(ret)) {
            TextBuf buf(alloc);
            for (int64_t i = 0; OB_SUCC(ret) && i < picked.count(); ++i) {
              if (0 == i) {
                if (OB_FAIL(buf.append("select "))) {}
              } else if (OB_FAIL(buf.append(" union all select "))) {}
              if (OB_SUCC(ret)
                  && OB_FAIL(fsq_emit_describe_values(buf, t.cols_.at(picked.at(i))))) {
              }
            }
            if (OB_SUCC(ret)) {
              out = buf.get_string();
            }
          }
        }
      }
    }
  }
  return ret;
}

// ---------------------------------------------------------------------------
// copy (query) to 'file' (format csv, header, nullstr 'NULL')
// ---------------------------------------------------------------------------

static int fsq_rewrite_general(common::ObIAllocator &alloc, const bool enabled,
                               const char *s, const int64_t len, common::ObString &out);

static int fsq_emit_copy_col(TextBuf &out, const int64_t idx)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(out.append("`__fsq_c"))) {
  } else if (OB_FAIL(out.append_int(idx))) {
  } else if (OB_FAIL(out.append_char('`'))) {
  }
  return ret;
}

static void fsq_strip_backticks(const char *q, int64_t &begin, int64_t &end)
{
  while (begin < end && fsq_is_blank(q[begin])) { ++begin; }
  while (end > begin && fsq_is_blank(q[end - 1])) { --end; }
  if (end > begin + 1 && '`' == q[begin] && '`' == q[end - 1]) {
    ++begin;
    --end;
  }
}

// the output names of `select .. from ..`, used as the CSV header
static int fsq_select_list_names(common::ObIAllocator &alloc, const char *q, const int64_t qlen,
                                 common::ObSEArray<common::ObString, 16> &names)
{
  int ret = OB_SUCCESS;
  UNUSED(alloc);
  int64_t pos = 0;
  fsq_skip_spaces(q, qlen, pos);
  if (!fsq_word_at(q, qlen, pos, "select", 6)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("copy query is not a select", K(ret));
  } else {
    const int64_t sel_begin = pos + 6;
    int64_t from_pos = -1;
    int64_t p = sel_begin;
    int depth = 0;
    while (p < qlen && from_pos < 0) {
      const char c = q[p];
      if ('\'' == c || '"' == c || '`' == c) {
        fsq_skip_quoted(q, qlen, p);
      } else {
        if ('(' == c) {
          ++depth;
        } else if (')' == c) {
          --depth;
        } else if (0 == depth && fsq_word_at(q, qlen, p, "from", 4)) {
          from_pos = p;
        } else {
          ++p;
        }
      }
    }
    const int64_t sel_end = from_pos < 0 ? qlen : from_pos;
    common::ObSEArray<ArgSeg, 16> items;
    if (OB_FAIL(fsq_split_args(q, sel_begin, sel_end, items))) {
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < items.count(); ++i) {
      ArgSeg item = items.at(i);
      fsq_trim(q, item);
      if (item.begin_ >= item.end_) {
        continue;
      }
      int64_t nb = item.begin_;
      int64_t ne = item.end_;
      // a top level "as"
      int64_t as_pos = -1;
      {
        int64_t k = item.begin_;
        int d = 0;
        while (k < item.end_ && as_pos < 0) {
          const char c = q[k];
          if ('\'' == c || '"' == c || '`' == c) {
            fsq_skip_quoted(q, item.end_, k);
          } else {
            if ('(' == c) {
              ++d;
            } else if (')' == c) {
              --d;
            } else if (0 == d && fsq_word_at(q, item.end_, k, "as", 2)) {
              as_pos = k;
            } else {
              ++k;
            }
          }
        }
      }
      if (as_pos >= 0) {
        nb = as_pos + 2;
      } else {
        // the last identifier of the expression, so f.id becomes id
        int64_t k = ne;
        while (k > nb && fsq_is_ident_char(q[k - 1])) { --k; }
        if (k > nb && '.' == q[k - 1]) {
          nb = k;
        } else if (k != ne && k == nb) {
          // the whole item is one identifier
        }
      }
      fsq_strip_backticks(q, nb, ne);
      common::ObString name(static_cast<int32_t>(ne - nb), const_cast<char *>(q + nb));
      if (OB_FAIL(names.push_back(name))) {
      }
    }
  }
  return ret;
}

// if(<col> is null, '<nullstr>', cast(<col> as char)) as `__fsq_cN`
static int fsq_emit_copy_body_col(TextBuf &out, const int64_t idx,
                                  const common::ObString &name,
                                  const common::ObString &null_str)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(out.append("if(__fsq_g."))) {
  } else if (OB_FAIL(fsq_emit_backticked(out, name))) {
  } else if (OB_FAIL(out.append(" is null,"))) {
  } else if (OB_FAIL(fsq_emit_quoted(out, null_str.ptr(), null_str.length(), false))) {
  } else if (OB_FAIL(out.append(",cast(__fsq_g."))) {
  } else if (OB_FAIL(fsq_emit_backticked(out, name))) {
  } else if (OB_FAIL(out.append(" as char)) as "))) {
  } else if (OB_FAIL(fsq_emit_copy_col(out, idx))) {
  }
  return ret;
}

static int fsq_parse_copy_options(common::ObIAllocator &alloc, const char *s, const int64_t len,
                                  int64_t &pos, bool &has_header, common::ObString &null_str,
                                  char *&null_buf)
{
  int ret = OB_SUCCESS;
  has_header = false;
  null_str.reset();
  null_buf = NULL;
  fsq_skip_spaces(s, len, pos);
  if (pos < len && '(' == s[pos]) {
    int64_t close = -1;
    if (OB_FAIL(fsq_find_close(s, len, pos, close))) {
      LOG_WARN("unbalanced copy option list", K(ret));
    } else {
      common::ObSEArray<ArgSeg, 16> segs;
      if (OB_FAIL(fsq_split_args(s, pos + 1, close, segs))) {
      }
      for (int64_t i = 0; OB_SUCC(ret) && i < segs.count(); ++i) {
        ArgSeg seg = segs.at(i);
        fsq_trim(s, seg);
        if (seg.begin_ >= seg.end_) {
          continue;
        }
        int64_t wb = seg.begin_;
        while (wb < seg.end_ && (fsq_is_ident_char(s[wb]))) { ++wb; }
        const int64_t wlen = wb - seg.begin_;
        int64_t vb = wb;
        fsq_skip_blanks(s, seg.end_, vb);
        if (fsq_iequal(s + seg.begin_, wlen, "format", 6)) {
          CallSpec probe;
          probe.fmt_ = ObFileFormat::FILE_FORMAT_CSV;
          if (vb >= seg.end_) {
            ret = fsq_syntax_error(s, seg.begin_, seg.end_);
          } else {
            ret = fsq_check_format(probe, s, vb, seg.end_);
          }
        } else if (fsq_iequal(s + seg.begin_, wlen, "header", 6)) {
          has_header = true;
          if (vb < seg.end_) {
            bool value = true;
            if (OB_FAIL(fsq_parse_bool(s + vb, seg.end_ - vb, value))) {
              ret = fsq_syntax_error(s, vb, seg.end_);
            } else {
              has_header = value;
            }
          }
        } else if (fsq_iequal(s + seg.begin_, wlen, "nullstr", 7)) {
          if (vb >= seg.end_ || ('\'' != s[vb] && '"' != s[vb])) {
            ret = fsq_syntax_error(s, seg.begin_, seg.end_);
          } else {
            int64_t p = vb;
            ret = fsq_parse_string_arg(alloc, s, len, p, null_str, null_buf);
          }
        } else {
          // not an option this implementation understands
        }
      }
      pos = close + 1;
    }
  }
  if (OB_SUCC(ret) && NULL == null_buf) {
    char *buf = static_cast<char *>(alloc.alloc(5));
    if (NULL == buf) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      MEMCPY(buf, "NULL", 4);
      null_str.assign_ptr(buf, 4);
      null_buf = buf;
    }
  }
  return ret;
}

static int fsq_rewrite_copy(common::ObIAllocator &alloc, const bool enabled,
                            const char *s, const int64_t len, common::ObString &out)
{
  int ret = OB_SUCCESS;
  const common::ObString original(static_cast<int32_t>(len), const_cast<char *>(s));
  int64_t pos = 0;
  fsq_skip_spaces(s, len, pos);
  if (!fsq_word_at(s, len, pos, "copy", 4)) {
    out = original;
  } else {
    pos += 4;
    fsq_skip_spaces(s, len, pos);
    int64_t qclose = -1;
    if (pos >= len || '(' != s[pos]) {
      out = original;
    } else if (OB_FAIL(fsq_find_close(s, len, pos, qclose))) {
      LOG_WARN("unbalanced copy statement", K(ret));
    } else {
      const int64_t q_begin = pos + 1;
      const int64_t q_end = qclose;
      int64_t p = qclose + 1;
      fsq_skip_spaces(s, len, p);
      if (!fsq_word_at(s, len, p, "to", 2)) {
        out = original;
      } else {
        p += 2;
        fsq_skip_spaces(s, len, p);
        common::ObString path;
        char *path_nul = NULL;
        if (OB_FAIL(fsq_parse_string_arg(alloc, s, len, p, path, path_nul))) {
          LOG_WARN("copy without a valid target path", K(ret));
        } else {
          bool has_header = false;
          common::ObString null_str;
          char *null_buf = NULL;
          if (OB_FAIL(fsq_parse_copy_options(alloc, s, len, p, has_header, null_str, null_buf))) {
          }
          common::ObString inner;
          common::ObSEArray<common::ObString, 16> names;
          if (OB_FAIL(ret)) {
          } else if (OB_FAIL(fsq_rewrite_general(alloc, enabled, s + q_begin, q_end - q_begin,
                                                 inner))) {
          } else if (OB_FAIL(fsq_select_list_names(alloc, inner.ptr(), inner.length(), names))) {
          } else if (0 == names.count()) {
            ret = OB_ERR_UNEXPECTED;
            LOG_WARN("copy without any output column", K(ret));
          } else {
            TextBuf buf(alloc);
            int64_t ncols = names.count();
            if (OB_FAIL(buf.append("select "))) {
            }
            for (int64_t i = 0; OB_SUCC(ret) && i < ncols; ++i) {
              if (i > 0 && OB_FAIL(buf.append(", "))) {}
              if (OB_SUCC(ret) && OB_FAIL(fsq_emit_copy_col(buf, i))) {}
            }
            if (OB_SUCC(ret) && OB_FAIL(buf.append(" from ("))) {}
            if (OB_SUCC(ret) && has_header) {
              if (OB_FAIL(buf.append("select "))) {}
              for (int64_t i = 0; OB_SUCC(ret) && i < ncols; ++i) {
                if (i > 0 && OB_FAIL(buf.append(", "))) {}
                if (OB_SUCC(ret)
                    && OB_FAIL(fsq_emit_quoted(buf, names.at(i).ptr(), names.at(i).length(),
                                               false))) {}
                if (OB_SUCC(ret) && OB_FAIL(buf.append(" as "))) {}
                if (OB_SUCC(ret) && OB_FAIL(fsq_emit_copy_col(buf, i))) {}
              }
              if (OB_SUCC(ret) && OB_FAIL(buf.append(" union all "))) {}
            }
            if (OB_SUCC(ret) && OB_FAIL(buf.append("select "))) {}
            for (int64_t i = 0; OB_SUCC(ret) && i < ncols; ++i) {
              if (i > 0 && OB_FAIL(buf.append(", "))) {}
              if (OB_SUCC(ret) && OB_FAIL(fsq_emit_copy_body_col(buf, i, names.at(i),
                                                                 null_str))) {}
            }
            if (OB_SUCC(ret) && OB_FAIL(buf.append(" from ("))) {}
            if (OB_SUCC(ret) && OB_FAIL(buf.append(inner.ptr(), inner.length()))) {}
            if (OB_SUCC(ret) && OB_FAIL(buf.append(") __fsq_g) __fsq_z into outfile "))) {}
            if (OB_SUCC(ret) && OB_FAIL(buf.append("\'"))) {}
            if (OB_SUCC(ret) && OB_FAIL(buf.append(path.ptr(), path.length()))) {}
            if (OB_SUCC(ret) && OB_FAIL(buf.append("\' format=(type=\'csv\', field_delimiter=\',\')"))) {}
            if (OB_SUCC(ret)) {
              out = buf.get_string();
            }
          }
        }
      }
    }
  }
  return ret;
}

// ---------------------------------------------------------------------------
// the general rewrite: replace every file table function with a derived table
// ---------------------------------------------------------------------------

static int fsq_rewrite_general(common::ObIAllocator &alloc, const bool enabled,
                               const char *s, const int64_t len, common::ObString &out)
{
  int ret = OB_SUCCESS;
  const common::ObString original(static_cast<int32_t>(len), const_cast<char *>(s));
  TextBuf buf(alloc);
  int64_t pos = 0;
  int64_t copied = 0;
  int64_t idx = 0;
  bool found_any = false;
  while (OB_SUCC(ret)) {
    int64_t name_pos = -1;
    int64_t name_len = 0;
    bool found = false;
    if (OB_FAIL(fsq_find_call(s, len, pos, name_pos, name_len, found))) {
    } else if (!found) {
      break;
    } else {
      found_any = true;
      if (!enabled) {
        ret = fsq_unsupported("file sql");
      } else {
        CallSpec spec;
        int64_t end_pos = name_pos + name_len;
        Table t;
        if (OB_FAIL(fsq_parse_call(alloc, s, len, name_pos, name_len, spec, end_pos))) {
        } else if (OB_FAIL(fsq_load_table(alloc, spec, t))) {
        } else if (OB_FAIL(buf.append(s + copied, name_pos - copied))) {
        } else {
          int64_t q = end_pos;
          fsq_skip_blanks(s, len, q);
          const bool need_alias = !fsq_alias_follows(s, len, q);
          if (OB_FAIL(fsq_emit_derived(buf, t, need_alias, idx))) {
          } else {
            copied = end_pos;
            pos = end_pos;
            ++idx;
          }
        }
      }
    }
  }
  if (OB_SUCC(ret)) {
    if (!found_any) {
      out = original;
    } else if (OB_FAIL(buf.append(s + copied, len - copied))) {
    } else {
      out = buf.get_string();
    }
  }
  return ret;
}

} // namespace file_sql

bool ObFileSqlRewriter::need_rewrite(const common::ObString &sql)
{
  bool found = false;
  const char *s = sql.ptr();
  const int64_t len = sql.length();
  for (int64_t pos = 0; !found && NULL != s && pos < len;) {
    if (file_sql::fsq_is_ident_start(s[pos])) {
      const int64_t begin = pos;
      while (pos < len && file_sql::fsq_is_ident_char(s[pos])) { ++pos; }
      for (int64_t i = 0; !found && i < file_sql::FSQ_FUNC_COUNT; ++i) {
        found = file_sql::fsq_iequal(s + begin, pos - begin, file_sql::FSQ_FUNCS[i],
                                     static_cast<int64_t>(strlen(file_sql::FSQ_FUNCS[i])));
      }
    } else {
      ++pos;
    }
  }
  return found;
}

int ObFileSqlRewriter::rewrite(ObSQLSessionInfo &session,
                               common::ObIAllocator &alloc,
                               const common::ObString &sql,
                               common::ObString &rewritten)
{
  int ret = OB_SUCCESS;
  const char *s = sql.ptr();
  const int64_t len = sql.length();
  if (NULL == s || len <= 0) {
    rewritten = sql;
  } else {
    bool enabled = false;
    common::ObObj flag;
    if (OB_FAIL(session.get_sys_variable(share::SYS_VAR_ENABLE_FILE_SQL, flag))) {
      LOG_WARN("get enable_file_sql failed", K(ret));
    } else if (OB_FAIL(file_sql::fsq_bool_value(flag, enabled))) {
      LOG_WARN("read enable_file_sql failed", K(ret));
    } else {
      int64_t pos = 0;
      file_sql::fsq_skip_spaces(s, len, pos);
      if (file_sql::fsq_word_at(s, len, pos, "describe", 8)
          || file_sql::fsq_word_at(s, len, pos, "desc", 4)) {
        ret = file_sql::fsq_rewrite_describe(alloc, enabled, s, len, rewritten);
      } else if (file_sql::fsq_word_at(s, len, pos, "copy", 4)) {
        ret = file_sql::fsq_rewrite_copy(alloc, enabled, s, len, rewritten);
      } else {
        ret = file_sql::fsq_rewrite_general(alloc, enabled, s, len, rewritten);
      }
      if (OB_FAIL(ret)) {
        LOG_WARN("rewrite file table statement failed", K(ret));
      }
    }
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase

