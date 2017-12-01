//------------------------------------------------------------------------------
//  Copyright 2017 H2O.ai
//
//  Licensed under the Apache License, Version 2.0 (the "License");
//  you may not use this file except in compliance with the License.
//  You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
//  Unless required by applicable law or agreed to in writing, software
//  distributed under the License is distributed on an "AS IS" BASIS,
//  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//  See the License for the specific language governing permissions and
//  limitations under the License.
//------------------------------------------------------------------------------
#include "csv/py_fread.h"
#include "csv/fread.h"
#include <string.h>    // memcpy
#include <sys/mman.h>  // mmap
#include <exception>
#include "memorybuf.h"
#include "datatable.h"
#include "column.h"
#include "py_datatable.h"
#include "py_encodings.h"
#include "py_utils.h"
#include "utils/assert.h"
#include "utils/file.h"
#include "utils/pyobj.h"
#include "utils.h"


static const SType colType_to_stype[NUMTYPE] = {
    ST_VOID,
    ST_BOOLEAN_I1,
    ST_INTEGER_I4,
    ST_INTEGER_I4,
    ST_INTEGER_I8,
    ST_REAL_F4,
    ST_REAL_F8,
    ST_REAL_F8,
    ST_REAL_F8,
    ST_STRING_I4_VCHAR,
};


// Forward declarations
static void cleanup_fread_session(freadMainArgs *frargs);



// Python FReader object, which holds specifications for the current reader
// logic. This reference is non-NULL when fread() is running; and serves as a
// lock preventing from running multiple fread() instances.
static PyObject *freader = NULL;
static PyObject *flogger = NULL;

// DataTable being constructed.
static DataTable *dt = NULL;

static MemoryBuffer* mbuf = nullptr;

// Array of StrBufs to coincide with the number of columns being constructed in the datatable
static StrBuf** strbufs = nullptr;

// These variables are handed down to `freadMain`, and are stored globally only
// because we want to free these memory buffers in the end.
static char *targetdir = NULL;
static char **na_strings = NULL;

// For temporary printing file names.
static char fname[1000];

// ncols -- number of fields in the CSV file. This field first becomes available
//     in the `userOverride()` callback, and doesn't change after that.
// nstrcols -- number of string columns in the output DataTable. This will be
//     computed within `allocateDT()` callback, and used for allocation of
//     string buffers. If the file is re-read (due to type bumps), this variable
//     will only count those string columns that need to be re-read.
// ndigits = len(str(ncols))
// verbose -- if True, then emit verbose messages during parsing.
//
static int ncols = 0;
static int nstrcols = 0;
static int ndigits = 0;
static int verbose = 0;

// types -- array of types for each field in the input file. Length = `ncols`.
// sizes -- array of byte sizes for each field. Length = `ncols`.
// Both of these arrays are borrowed references and are valid only for the
// duration of the parse. Must not be freed.
static int8_t *types = NULL;
static int8_t *sizes = NULL;



//------------------------------------------------------------------------------

/**
 * Python wrapper around `freadMain()`. This function extracts the arguments
 * from the provided :class:`FReader` python object, converts them into the
 * `freadMainArgs` structure, and then passes that structure to the `freadMain`
 * function.
 */
PyObject* pyfread(PyObject*, PyObject *args)
{
  PyObject *pydt = NULL;
  int retval = 0;
  freadMainArgs *frargs = NULL;
  if (freader != NULL || dt != NULL) {
      PyErr_SetString(PyExc_RuntimeError,
          "Cannot run multiple instances of fread() in-parallel.");
      return NULL;
  }
  if (!PyArg_ParseTuple(args, "O:fread", &freader))
      return NULL;

  try {
    Py_INCREF(freader);
    dtmalloc_g(frargs, freadMainArgs, 1);

    PyObj pyfreader(freader);
    PyObj filename_arg = pyfreader.attr("file");
    PyObj input_arg = pyfreader.attr("text");
    PyObj skipstring_arg = pyfreader.attr("skip_to_string");

    // filename and input are borrowed references; they remain valid as long as
    // filename_arg and input_arg are alive.
    const char* filename = filename_arg.as_cstring();
    const char* input = input_arg.as_cstring();
    const char* skipstring = skipstring_arg.as_cstring();
    na_strings = pyfreader.attr("na_strings").as_cstringlist();
    verbose = pyfreader.attr("verbose").as_bool();
    flogger = pyfreader.attr("logger").as_pyobject();
    int64_t fileno64 = pyfreader.attr("_fileno").as_int64();
    int fileno = fileno64 < 0? -1 : static_cast<int>(fileno64);

    frargs->sep = pyfreader.attr("sep").as_char();
    frargs->dec = pyfreader.attr("dec").as_char();
    frargs->quote = pyfreader.attr("quotechar").as_char();
    frargs->nrowLimit = pyfreader.attr("max_nrows").as_int64();
    frargs->skipNrow = pyfreader.attr("skip_lines").as_int64();
    frargs->skipString = skipstring;
    frargs->header = pyfreader.attr("header").as_bool();
    frargs->verbose = verbose;
    frargs->NAstrings = (const char* const*) na_strings;
    frargs->stripWhite = pyfreader.attr("strip_white").as_bool();
    frargs->skipEmptyLines = pyfreader.attr("skip_blank_lines").as_bool();
    frargs->fill = pyfreader.attr("fill").as_bool();
    frargs->showProgress = pyfreader.attr("show_progress").as_bool();
    frargs->nth = static_cast<int32_t>(pyfreader.attr("nthreads").as_int64());
    frargs->warningsAreErrors = 0;
    if (frargs->nrowLimit < 0)
        frargs->nrowLimit = LONG_MAX;
    if (frargs->skipNrow < 0)
        frargs->skipNrow = 0;

    frargs->freader = freader;
    Py_INCREF(freader);

    if (input) {
      mbuf = new ExternalMemBuf(input);
    } else if (filename) {
      if (verbose) DTPRINT("  Opening file %s [fd=%d]", filename, fileno);
      mbuf = new OvermapMemBuf(filename, 1, fileno);
      if (verbose) {
        size_t sz = mbuf->size();
        DTPRINT("  File opened, size: %s", sz? filesize_to_str(sz - 1) : "0");
      }
    } else {
      throw ValueError() << "Neither filename nor input were provided";
    }
    frargs->buf = mbuf->get();
    frargs->bufsize = mbuf->size();

    retval = freadMain(*frargs);
    if (!retval) goto fail;

    pydt = pydt_from_dt(dt);
    if (pydt == NULL) goto fail;
    cleanup_fread_session(frargs);
    return pydt;
  } catch (const std::exception& e) {
    exception_to_python(e);
    // fall-through into the "fail" clause
  }

  fail:
    delete dt;
    cleanup_fread_session(frargs);
    dtfree(frargs);
    return NULL;
}


Column* alloc_column(SType stype, size_t nrows, int j)
{
    // TODO(pasha): figure out how to use `WritableBuffer`s here
    Column *col = NULL;
    if (targetdir) {
        snprintf(fname, 1000, "%s/col%0*d", targetdir, ndigits, j);
        col = Column::new_mmap_column(stype, static_cast<int64_t>(nrows), fname);
    } else{
        col = Column::new_data_column(stype, static_cast<int64_t>(nrows));
    }
    if (col == NULL) return NULL;

    if (stype_info[stype].ltype == LT_STRING) {
        dtrealloc(strbufs[j], StrBuf, 1);
        StrBuf* sb = strbufs[j];
        // Pre-allocate enough memory to hold 5-char strings in the buffer. If
        // this is not enough, we will always be able to re-allocate during the
        // run time.
        size_t alloc_size = nrows * 5;
        sb->mbuf = static_cast<StringColumn<int32_t>*>(col)->strbuf;
        sb->mbuf->resize(alloc_size);
        sb->ptr = 0;
        sb->idx8 = -1;  // not needed for this structure
        sb->idxdt = j;
        sb->numuses = 0;
    }
    return col;
}


Column* realloc_column(Column *col, SType stype, size_t nrows, int j)
{
    if (col != NULL && stype != col->stype()) {
        delete col;
        return alloc_column(stype, nrows, j);
    }
    if (col == NULL) {
        return alloc_column(stype, nrows, j);
    }

    size_t new_alloc_size = stype_info[stype].elemsize * nrows;
    col->mbuf->resize(new_alloc_size);
    col->nrows = (int64_t) nrows;
    return col;
}



static void cleanup_fread_session(freadMainArgs *frargs) {
    strbufs = nullptr;
    ncols = 0;
    nstrcols = 0;
    types = NULL;
    sizes = NULL;
    dtfree(targetdir);
    if (mbuf) {
      mbuf->release();
      mbuf = NULL;
    }
    if (frargs) {
        if (na_strings) {
            char **ptr = na_strings;
            while (*ptr++) delete[] *ptr;
            delete[] na_strings;
        }
        pyfree(frargs->freader);
    }
    pyfree(freader);
    pyfree(flogger);
    dt = NULL;
}



bool userOverride(int8_t *types_, lenOff *colNames, const char *anchor,
                   int ncols_)
{
  types = types_;
  PyObject *colNamesList = PyList_New(ncols_);
  PyObject *colTypesList = PyList_New(ncols_);
  for (int i = 0; i < ncols_; i++) {
    lenOff ocol = colNames[i];
    PyObject* pycol = NULL;
    if (ocol.len > 0) {
      const char* src = anchor + ocol.off;
      const uint8_t* usrc = reinterpret_cast<const uint8_t*>(src);
      size_t zlen = static_cast<size_t>(ocol.len);
      if (is_valid_utf8(usrc, zlen)) {
        pycol = PyUnicode_FromStringAndSize(src, ocol.len);
      } else {
        char* newsrc = new char[zlen * 4];
        uint8_t* unewsrc = reinterpret_cast<uint8_t*>(newsrc);
        int newlen = decode_win1252(usrc, ocol.len, unewsrc);
        assert(newlen > 0);
        pycol = PyUnicode_FromStringAndSize(newsrc, newlen);
        delete[] newsrc;
      }
    } else {
      pycol = PyUnicode_FromFormat("V%d", i);
    }
    PyObject *pytype = PyLong_FromLong(types[i]);
    PyList_SET_ITEM(colNamesList, i, pycol);
    PyList_SET_ITEM(colTypesList, i, pytype);
  }
  PyObject *ret = PyObject_CallMethod(freader, "_override_columns",
                                      "OO", colNamesList, colTypesList);
  if (!ret) {
    pyfree(colTypesList);
    pyfree(colNamesList);
    return 0;
  }

  for (int i = 0; i < ncols_; i++) {
    PyObject *t = PyList_GET_ITEM(colTypesList, i);
    types[i] = (int8_t) PyLong_AsUnsignedLongMask(t);
  }

  pyfree(colTypesList);
  pyfree(colNamesList);
  pyfree(ret);
  return 1;  // continue reading the file
}


/**
 * Allocate memory for the DataTable that is being constructed.
 */
size_t allocateDT(int8_t *types_, int8_t *sizes_, int ncols_, int ndrop_,
                  size_t nrows)
{
    Column **columns = NULL;
    types = types_;
    sizes = sizes_;
    nstrcols = 0;

    // First we need to estimate the size of the dataset that needs to be
    // created. However this needs to be done on first run only.
    // Also in this block we compute: `nstrcols` (will be used later in
    // `prepareThreadContext` and `postprocessBuffer`), as well as allocating
    // the `Column**` array.
    if (ncols == 0) {
        // DTPRINT("Writing the DataTable into %s", targetdir);
        assert(dt == NULL);
        ncols = ncols_;

        size_t alloc_size = 0;
        int i, j;
        for (i = j = 0; i < ncols; i++) {
            if (types[i] == CT_DROP) continue;
            nstrcols += (types[i] == CT_STRING);
            SType stype = colType_to_stype[types[i]];
            alloc_size += stype_info[stype].elemsize * nrows;
            if (types[i] == CT_STRING) alloc_size += 5 * nrows;
            j++;
        }
        assert(j == ncols_ - ndrop_);
        dtcalloc_g(columns, Column*, j + 1);
        dtcalloc_g(strbufs, StrBuf*, j);
        columns[j] = NULL;

        // Call the Python upstream to determine the strategy where the
        // DataTable should be created.
        PyObject *r = PyObject_CallMethod(freader, "_get_destination", "n", alloc_size);
        targetdir = PyObj(r).as_ccstring();
    } else {
        assert(dt != NULL && ncols == ncols_);
        columns = dt->columns;
        for (int i = 0; i < ncols; i++)
            nstrcols += (types[i] == CT_STRING);
    }

    // Compute number of digits in `ncols` (needed for creating file names).
    if (targetdir) {
        ndigits = 0;
        for (int nc = ncols; nc; nc /= 10) ndigits++;
    }

    // Create individual columns
    for (int i = 0, j = 0; i < ncols_; i++) {
        int8_t type = types[i];
        if (type == CT_DROP) continue;
        if (type > 0) {
            SType stype = colType_to_stype[type];
            columns[j] = realloc_column(columns[j], stype, nrows, j);
            if (columns[j] == NULL) goto fail;
        }
        j++;
    }

    if (dt == NULL) {
        dt = new DataTable(columns);
        if (dt == NULL) goto fail;
    }
    return 1;

  fail:
    if (columns) {
        Column **col = columns;
        while (*col++) delete (*col);
        dtfree(columns);
    }
    delete strbufs;
    return 0;
}



void setFinalNrow(size_t nrows) {
  int i, j;
  for (i = j = 0; i < ncols; i++) {
    int type = types[i];
    if (type == CT_DROP) continue;
    Column *col = dt->columns[j];
    if (type == CT_STRING) {
      StrBuf* sb = strbufs[j];
      assert(sb->numuses == 0);
      sb->mbuf->resize(sb->ptr);
      sb->mbuf = nullptr; // MemoryBuffer is also pointed to by the column
      col->mbuf->resize(sizeof(int32_t) * (nrows + 1));
      col->nrows = static_cast<int64_t>(nrows);
    } else if (type > 0) {
      Column *c = realloc_column(col, colType_to_stype[type], nrows, j);
      if (c == nullptr) throw Error() << "Could not reallocate column";
    }
    j++;
  }
  dt->nrows = (int64_t) nrows;
}


void prepareThreadContext(ThreadLocalFreadParsingContext *ctx)
{
  try {
    ctx->strbufs = new StrBuf[nstrcols]();
    for (int i = 0, j = 0, k = 0, off8 = 0; i < (int)ncols; i++) {
        if (types[i] == CT_DROP) continue;
        if (types[i] == CT_STRING) {
            assert(k < nstrcols);
            ctx->strbufs[k].mbuf = new MemoryMemBuf(4096);
            ctx->strbufs[k].ptr = 0;
            ctx->strbufs[k].idx8 = off8;
            ctx->strbufs[k].idxdt = j;
            k++;
        }
        off8 += (sizes[i] == 8);
        j++;
    }
    return;

  } catch (std::exception&) {
    printf("prepareThreadContext() failed\n");
    for (int k = 0; k < nstrcols; k++) {
      if (ctx->strbufs[k].mbuf)
        ctx->strbufs[k].mbuf->release();
    }
    dtfree(ctx->strbufs);
    *(ctx->stopTeam) = 1;
  }
}


void postprocessBuffer(ThreadLocalFreadParsingContext *ctx)
{
  try {
    StrBuf* ctx_strbufs = ctx->strbufs;
    const unsigned char *anchor = (const unsigned char*) ctx->anchor;
    size_t nrows = ctx->nRows;
    lenOff* __restrict__ const lenoffs = (lenOff *__restrict__) ctx->buff8;
    int rowCount8 = (int) ctx->rowSize8 / 8;

    for (int k = 0; k < nstrcols; k++) {
      assert(ctx_strbufs != NULL);

      lenOff *__restrict__ lo = lenoffs + ctx_strbufs[k].idx8;
      MemoryBuffer* strdest = ctx_strbufs[k].mbuf;
      int32_t off = 1;
      size_t bufsize = ctx_strbufs[k].mbuf->size();
      for (size_t n = 0; n < nrows; n++) {
        int32_t len = lo->len;
        if (len > 0) {
          size_t zlen = (size_t) len;
          if (bufsize < zlen * 3 + (size_t) off) {
            bufsize = bufsize * 2 + zlen * 3;
            strdest->resize(bufsize);
          }
          const unsigned char *src = anchor + lo->off;
          unsigned char *dest =
              static_cast<unsigned char*>(strdest->at(off - 1));
          if (is_valid_utf8(src, zlen)) {
            memcpy(dest, src, zlen);
            off += zlen;
            lo->off = off;
          } else {
            int newlen = decode_win1252(src, len, dest);
            assert(newlen > 0);
            off += (size_t) newlen;
            lo->off = off;
          }
        } else if (len == 0) {
          lo->off = off;
        } else {
          assert(len == NA_LENOFF);
          lo->off = -off;
        }
        lo += rowCount8;
      }
      ctx_strbufs[k].ptr = (size_t) (off - 1);
    }
    return;
  } catch (std::exception&) {
    printf("postprocessBuffer() failed\n");
    *(ctx->stopTeam) = 1;
  }
}


void orderBuffer(ThreadLocalFreadParsingContext *ctx)
{
  try {
    StrBuf* ctx_strbufs = ctx->strbufs;
    for (int k = 0; k < nstrcols; ++k) {
      int j = ctx_strbufs[k].idxdt;
      StrBuf* sb = strbufs[j];
      size_t sz = ctx_strbufs[k].ptr;
      size_t ptr = sb->ptr;
      MemoryBuffer* sb_mbuf = sb->mbuf;
      // If we need to write more than the size of the available buffer, the
      // buffer has to grow. Check documentation for `StrBuf.numuses` in
      // `py_fread.h`.
      while (ptr + sz > sb_mbuf->size()) {
        size_t newsize = (ptr + sz) * 2;
        int old = 0;
        // (1) wait until no other process is writing into the buffer
        while (sb->numuses > 0)
          /* wait until .numuses == 0 (all threads finished writing) */;
        // (2) make `numuses` negative, indicating that no other thread may
        // initiate a memcopy operation for now.
        #pragma omp atomic capture
        {
          old = sb->numuses;
          sb->numuses -= 1000000;
        }
        // (3) The only case when `old != 0` is if another thread started
        // memcopy operation in-between statements (1) and (2) above. In
        // that case we restore the previous value of `numuses` and repeat
        // the loop.
        // Otherwise (and it is the most common case) we reallocate the
        // buffer and only then restore the `numuses` variable.
        if (old == 0) {
          sb_mbuf->resize(newsize);
        }
        #pragma omp atomic update
        sb->numuses += 1000000;
      }
      ctx_strbufs[k].ptr = ptr;
      sb->ptr = ptr + sz;
    }
    return;
  } catch (std::exception&) {
    printf("orderBuffer() failed");
    *(ctx->stopTeam) = 1;
  }
}


void pushBuffer(ThreadLocalFreadParsingContext *ctx)
{
    StrBuf *__restrict__ ctx_strbufs = ctx->strbufs;
    const void *__restrict__ buff8 = ctx->buff8;
    const void *__restrict__ buff4 = ctx->buff4;
    const void *__restrict__ buff1 = ctx->buff1;
    int nrows = (int) ctx->nRows;
    size_t row0 = ctx->DTi;

    int i = 0;  // index within the `types` and `sizes`
    int j = 0;  // index within `dt->columns`, `buff` and `strbufs`
    int off8 = 0, off4 = 0, off1 = 0;  // offsets within the buffers
    int rowCount8 = (int) ctx->rowSize8 / 8;
    int rowCount4 = (int) ctx->rowSize4 / 4;
    int rowCount1 = (int) ctx->rowSize1;

    int k = 0;
    for (; i < ncols; i++) {
        if (types[i] == CT_DROP) continue;
        Column *col = dt->columns[j];

        if (types[i] == CT_STRING) {
            StrBuf *sb = strbufs[j];
            int idx8 = ctx_strbufs[k].idx8;
            size_t ptr = ctx_strbufs[k].ptr;
            const lenOff *__restrict__ lo =
                (const lenOff*) add_constptr(buff8, idx8 * 8);
            size_t sz = (size_t) abs(lo[(nrows - 1)*rowCount8].off) - 1;

            int done = 0;
            while (!done) {
                int old;
                #pragma omp atomic capture
                old = sb->numuses++;
                if (old >= 0) {
                    memcpy(sb->mbuf->at(ptr), ctx_strbufs[k].mbuf->get(), sz);
                    done = 1;
                }
                #pragma omp atomic update
                sb->numuses--;
            }

            int32_t* dest = ((int32_t*) col->data()) + row0 + 1;
            int32_t iptr = (int32_t) ptr;
            for (int n = 0; n < nrows; n++) {
                int32_t off = lo->off;
                *dest++ = (off < 0)? off - iptr : off + iptr;
                lo += rowCount8;
            }
            k++;

        } else if (types[i] > 0) {
            int8_t elemsize = sizes[i];
            if (elemsize == 8) {
                const uint64_t *src = ((const uint64_t*) buff8) + off8;
                uint64_t *dest = ((uint64_t*) col->data()) + row0;
                for (int r = 0; r < nrows; r++) {
                    *dest = *src;
                    src += rowCount8;
                    dest++;
                }
            } else
            if (elemsize == 4) {
                const uint32_t *src = ((const uint32_t*) buff4) + off4;
                uint32_t *dest = ((uint32_t*) col->data()) + row0;
                for (int r = 0; r < nrows; r++) {
                    *dest = *src;
                    src += rowCount4;
                    dest++;
                }
            } else
            if (elemsize == 1) {
                const uint8_t *src = ((const uint8_t*) buff1) + off1;
                uint8_t *dest = ((uint8_t*) col->data()) + row0;
                for (int r = 0; r < nrows; r++) {
                    *dest = *src;
                    src += rowCount1;
                    dest++;
                }
            }
        }
        off8 += (sizes[i] == 8);
        off4 += (sizes[i] == 4);
        off1 += (sizes[i] == 1);
        j++;
    }
}


void progress(double percent/*[0,100]*/) {
    PyObject_CallMethod(freader, "_progress", "d", percent);
}


void freeThreadContext(ThreadLocalFreadParsingContext *ctx)
{
  if (ctx->strbufs) {
    for (int k = 0; k < nstrcols; k++) {
      if (ctx->strbufs[k].mbuf)
        ctx->strbufs[k].mbuf->release();
    }
    dtfree(ctx->strbufs);
  }
}


__attribute__((format(printf, 1, 2)))
void DTPRINT(const char *format, ...) {
    va_list args;
    va_start(args, format);
    char *msg;
    if (strcmp(format, "%s") == 0) {
        msg = va_arg(args, char*);
    } else {
        msg = (char*) alloca(2001);
        vsnprintf(msg, 2000, format, args);
    }
    va_end(args);
    PyObject_CallMethod(flogger, "debug", "O", PyUnicode_FromString(msg));
}