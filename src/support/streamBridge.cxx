/**
 * PANDA 3D SOFTWARE
 * Copyright (c) Carnegie Mellon University.  All rights reserved.
 *
 * All use of this software is subject to the terms of the revised BSD
 * license.  You should have received a copy of this license along
 * with this source code in a file named "LICENSE."
 *
 * @file streamBridge.cxx
 */

#include "streamBridge.h"

#include <atomic>
#include <iostream>
#include <streambuf>

namespace {

// Incremented on every successful Create*, decremented on every Destroy*.
// Exposed via igStreamBridge_LiveCount for managed leak tests.  Atomic so
// stream creation from multiple threads doesn't corrupt the counter.
std::atomic<int64_t> g_live_bridges{0};

/**
 * A std::streambuf implementation whose read/write/seek operations are
 * dispatched to caller-supplied function pointers.  One instance is owned
 * by each iostream returned to C# via Interrogate.StreamBridge; when the
 * managed wrapper is disposed, igStreamBridge_Destroy() deletes the stream
 * object which deletes this buffer.
 *
 * There is no own-buffer because we implement underflow() by directly
 * reading a chunk into _get_area each time the get-pointer runs dry, and
 * overflow()/sync() by flushing _put_area through the write callback.
 */
class BridgeBuf : public std::streambuf {
public:
  BridgeBuf(ig_stream_read_fn read_cb,
            ig_stream_write_fn write_cb,
            ig_stream_seek_fn seek_cb,
            void *cookie)
      : _read(read_cb), _write(write_cb), _seek(seek_cb), _cookie(cookie) {
    if (_write != nullptr) {
      setp(_put_area, _put_area + sizeof(_put_area));
    }
    // get_area starts empty; underflow() populates it on demand.
  }

  ~BridgeBuf() override {
    // Flush any pending writes.  Swallow errors — destructors must not throw.
    sync();
  }

protected:
  int_type underflow() override {
    if (_read == nullptr) {
      return traits_type::eof();
    }
    int64_t n = _read(_cookie, _get_area, (int64_t)sizeof(_get_area));
    if (n <= 0) {
      return traits_type::eof();
    }
    setg(_get_area, _get_area, _get_area + n);
    return traits_type::to_int_type(_get_area[0]);
  }

  int_type overflow(int_type c) override {
    if (_write == nullptr) {
      return traits_type::eof();
    }
    if (sync() != 0) {
      return traits_type::eof();
    }
    if (!traits_type::eq_int_type(c, traits_type::eof())) {
      char ch = traits_type::to_char_type(c);
      if (_write(_cookie, &ch, 1) != 1) {
        return traits_type::eof();
      }
    }
    return traits_type::not_eof(c);
  }

  int sync() override {
    if (_write == nullptr) {
      return 0;
    }
    char *begin = pbase();
    char *end = pptr();
    int64_t pending = end - begin;
    while (pending > 0) {
      int64_t n = _write(_cookie, begin, pending);
      if (n <= 0) {
        return -1;
      }
      begin += n;
      pending -= n;
    }
    setp(_put_area, _put_area + sizeof(_put_area));
    return 0;
  }

  pos_type seekoff(off_type offset, std::ios_base::seekdir dir,
                   std::ios_base::openmode which) override {
    if (_seek == nullptr) {
      return pos_type(off_type(-1));
    }
    // Drop any buffered input/output so the seek actually moves the
    // underlying stream position that C++ will read from / write to next.
    if ((which & std::ios_base::out) != 0) {
      if (sync() != 0) {
        return pos_type(off_type(-1));
      }
    }
    if ((which & std::ios_base::in) != 0) {
      setg(_get_area, _get_area, _get_area);
    }

    int origin = (dir == std::ios_base::beg) ? 0
               : (dir == std::ios_base::cur) ? 1 : 2;

    // Account for any bytes already read ahead of where C++ thinks it is.
    int64_t adjusted_offset = (int64_t)offset;
    if (dir == std::ios_base::cur) {
      adjusted_offset -= (egptr() - gptr());
    }

    int64_t abs = _seek(_cookie, adjusted_offset, origin);
    if (abs < 0) {
      return pos_type(off_type(-1));
    }
    return pos_type(off_type(abs));
  }

  pos_type seekpos(pos_type pos, std::ios_base::openmode which) override {
    return seekoff(off_type(pos), std::ios_base::beg, which);
  }

private:
  ig_stream_read_fn _read;
  ig_stream_write_fn _write;
  ig_stream_seek_fn _seek;
  void *_cookie;

  // Might want to adjust these buffer sizes later...
  char _get_area[4096];
  char _put_area[4096];
};

/**
 * Pairs a BridgeBuf with an std::iostream that owns it.  std::iostream's
 * destructor does not delete the streambuf — we do it here in the owner
 * subclass, so a single `delete` on the returned pointer cleans up both.
 */
class BridgeIstream : public std::istream {
public:
  explicit BridgeIstream(BridgeBuf *buf) : std::istream(buf), _buf(buf) {}
  ~BridgeIstream() override { delete _buf; }
private:
  BridgeBuf *_buf;
};

class BridgeOstream : public std::ostream {
public:
  explicit BridgeOstream(BridgeBuf *buf) : std::ostream(buf), _buf(buf) {}
  ~BridgeOstream() override { delete _buf; }
private:
  BridgeBuf *_buf;
};

class BridgeIostream : public std::iostream {
public:
  explicit BridgeIostream(BridgeBuf *buf) : std::iostream(buf), _buf(buf) {}
  ~BridgeIostream() override { delete _buf; }
private:
  BridgeBuf *_buf;
};

}  // namespace

extern "C" {

void *
igStreamBridge_CreateIstream(ig_stream_read_fn read_cb,
                             ig_stream_seek_fn seek_cb,
                             void *cookie) {
  auto *buf = new BridgeBuf(read_cb, nullptr, seek_cb, cookie);
  g_live_bridges.fetch_add(1, std::memory_order_relaxed);
  return static_cast<std::istream *>(new BridgeIstream(buf));
}

void *
igStreamBridge_CreateOstream(ig_stream_write_fn write_cb,
                             ig_stream_seek_fn seek_cb,
                             void *cookie) {
  auto *buf = new BridgeBuf(nullptr, write_cb, seek_cb, cookie);
  g_live_bridges.fetch_add(1, std::memory_order_relaxed);
  return static_cast<std::ostream *>(new BridgeOstream(buf));
}

void *
igStreamBridge_CreateIostream(ig_stream_read_fn read_cb,
                              ig_stream_write_fn write_cb,
                              ig_stream_seek_fn seek_cb,
                              void *cookie) {
  auto *buf = new BridgeBuf(read_cb, write_cb, seek_cb, cookie);
  g_live_bridges.fetch_add(1, std::memory_order_relaxed);
  return static_cast<std::iostream *>(new BridgeIostream(buf));
}

void
igStreamBridge_DestroyIstream(void *stream) {
  if (stream == nullptr) return;
  delete static_cast<std::istream *>(stream);
  g_live_bridges.fetch_sub(1, std::memory_order_relaxed);
}

void
igStreamBridge_DestroyOstream(void *stream) {
  if (stream == nullptr) return;
  delete static_cast<std::ostream *>(stream);
  g_live_bridges.fetch_sub(1, std::memory_order_relaxed);
}

void
igStreamBridge_DestroyIostream(void *stream) {
  if (stream == nullptr) return;
  delete static_cast<std::iostream *>(stream);
  g_live_bridges.fetch_sub(1, std::memory_order_relaxed);
}

int64_t
igStreamBridge_LiveCount() {
  return g_live_bridges.load(std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// The reverse direction: a C++ stream the native side owns, driven from managed
// code.  None of these free the stream -- that stays with whoever produced it.

int64_t
igStream_Read(void *istream, void *buf, int64_t len) {
  if (istream == nullptr || buf == nullptr || len < 0) {
    return -1;
  }
  std::istream *in = static_cast<std::istream *>(istream);
  in->read(static_cast<char *>(buf), (std::streamsize)len);
  std::streamsize count = in->gcount();

  // eof with a partial (or empty) read is a normal end, not a failure: clear the
  // flags so the stream stays usable for tell/seek afterwards.
  if (in->fail() && in->eof()) {
    in->clear(in->rdstate() & ~(std::ios::failbit | std::ios::eofbit));
    return (int64_t)count;
  }
  if (in->bad()) {
    return -1;
  }
  return (int64_t)count;
}

int64_t
igStream_Write(void *ostream, const void *buf, int64_t len) {
  if (ostream == nullptr || buf == nullptr || len < 0) {
    return -1;
  }
  std::ostream *out = static_cast<std::ostream *>(ostream);
  out->write(static_cast<const char *>(buf), (std::streamsize)len);
  return out->fail() ? -1 : len;
}

int64_t
igStream_Seek(void *stream, int64_t offset, int origin, int is_input) {
  if (stream == nullptr) {
    return -1;
  }
  std::ios_base::seekdir dir = (origin == 1) ? std::ios_base::cur
                             : (origin == 2) ? std::ios_base::end
                                             : std::ios_base::beg;
  // A failed seek must leave the stream usable: a decompressing stream (a compressed
  // multifile subfile) refuses to seek, and if that left failbit set every later read
  // would silently do nothing.  Clear it and report the failure through the return
  // value instead.
  if (is_input) {
    std::istream *in = static_cast<std::istream *>(stream);
    in->clear();
    in->seekg((std::streamoff)offset, dir);
    if (in->fail()) {
      in->clear();
      return -1;
    }
    return (int64_t)in->tellg();
  }
  std::ostream *out = static_cast<std::ostream *>(stream);
  out->clear();
  out->seekp((std::streamoff)offset, dir);
  if (out->fail()) {
    out->clear();
    return -1;
  }
  return (int64_t)out->tellp();
}

int64_t
igStream_Tell(void *stream, int is_input) {
  if (stream == nullptr) {
    return -1;
  }
  if (is_input) {
    return (int64_t)static_cast<std::istream *>(stream)->tellg();
  }
  return (int64_t)static_cast<std::ostream *>(stream)->tellp();
}

void
igStream_Flush(void *ostream) {
  if (ostream != nullptr) {
    static_cast<std::ostream *>(ostream)->flush();
  }
}

}  // extern "C"
