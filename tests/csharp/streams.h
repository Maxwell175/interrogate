//FLAGS: -promiscuous -string -refcount
#include <iosfwd>
#include <string>

// Exercises std::istream / std::ostream / std::iostream parameters and
// returns.  The C# backend bridges these to System.IO.Stream via the
// StreamBridge helper in Interrogate.Core.

class StreamUser {
public:
  StreamUser();

  // Writes the string "hello-<id>" to the given ostream.  The driver checks
  // that this exact byte sequence lands in a managed MemoryStream.
  void write_hello(std::ostream &out) const;

  // Reads all bytes from the given istream into an internal buffer.
  // The driver verifies bytes_read() matches what was written.
  void read_all(std::istream &in);

  // Reads from and writes to the same stream — exercises iostream.
  void echo(std::iostream &io);

  // Getter for verification of read_all.
  int bytes_read() const;

  // Setter for write_hello's id (so the test can verify marshalled content).
  void set_id(int id);

  // Hands back a stream that C++ owns, and takes it back to free.  This is the
  // open_read_file / close_read_file shape -- the one that was never tested, and
  // was therefore broken in two ways at once: the return came back as an opaque
  // IntPtr, and passing it to the close method bridged a *brand-new* native stream
  // which both sides then deleted.  A double free, reachable from the only close
  // method the bindings offered.
  std::istream *open_stream();
  void close_stream(std::istream *stream);

private:
  int _id;
  int _bytes_read;
};
