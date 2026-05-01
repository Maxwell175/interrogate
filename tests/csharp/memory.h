//FLAGS: -promiscuous -string -refcount
#include <iosfwd>
#include <string>
#include <vector>

typedef std::vector<std::string> vector_string;

// Exercises the memory management story: NativeObject ownership / disposal,
// GC finalizer coverage, stream-bridge lifecycle, and non-blittable
// (std::string) returns under heavy iteration.  The driver snapshots
// counters from native code to assert there are no leaks.

// Test-only probes exported at module scope.  The binding emits them as
// static methods on the globals class.
int memory_live_trackers();
int memory_total_trackers_created();
void memory_reset_counts();

// Every Tracker constructor bumps a global live counter; every destructor
// drops it back.  The driver uses this to verify that disposing / GC'ing
// NativeObject wrappers actually runs the C++ destructor on the other side.
class Tracker {
public:
  Tracker(const std::string &label);
  ~Tracker();

  std::string get_label() const;

  // Exercises string-return + string-param round-trip under iteration.
  std::string echo_label() const;

  // Stream methods to test bridge disposal under exceptions / forgot-using.
  void write_label(std::ostream &out) const;
  int read_into_label(std::istream &in);

  // Takes a Tracker* param so we can verify ref-counting / ownership on
  // arguments (the other Tracker must stay alive through the call).
  std::string combined_label(Tracker *other) const;

  // Returns a fresh vector every call — used to test that repeated
  // collection returns don't leak managed OR native memory.
  vector_string build_labels(int count) const;

private:
  std::string _label;
};
