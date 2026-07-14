#include "streams.h"

#include <ostream>
#include <istream>
#include <sstream>

StreamUser::StreamUser() : _id(0), _bytes_read(0) {}

void StreamUser::write_hello(std::ostream &out) const {
  out << "hello-" << _id;
  out.flush();
}

void StreamUser::read_all(std::istream &in) {
  _bytes_read = 0;
  char buf[256];
  while (in.read(buf, sizeof(buf)).gcount() > 0) {
    _bytes_read += (int)in.gcount();
  }
  // gcount() stores the last read's byte count even after eof; add any
  // partial-read trailing bytes.
  _bytes_read += (int)in.gcount();
}

void StreamUser::echo(std::iostream &io) {
  std::string line;
  std::getline(io, line);
  io.clear();
  io << "echo:" << line;
  io.flush();
}

int StreamUser::bytes_read() const {
  return _bytes_read;
}

void StreamUser::set_id(int id) {
  _id = id;
}

std::istream *StreamUser::open_stream() {
  // Owned by C++ until close_stream() takes it back.
  return new std::istringstream("native-stream-contents");
}

void StreamUser::close_stream(std::istream *stream) {
  delete stream;
}
