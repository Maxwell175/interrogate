#include "keepalive.h"

// Live-probe accounting, so the driver can confirm the wrappers really own and
// free native objects (i.e. the finalizer path is real, which is what makes the
// keep-alive necessary in the first place).
static int g_live_probes = 0;

int kp_live_probes() {
  return g_live_probes;
}

void kp_reset_counts() {
  g_live_probes = 0;
}

// A destroyed Probe scribbles poison over its id before the memory is released,
// so a read through a dangling pointer returns a recognizably-wrong value rather
// than (only sometimes) the stale-but-correct one.
static const long kPoison = -0x7EADBEEFL;

Probe::Probe(long id) : _id(id) {
  ++g_live_probes;
}

Probe::~Probe() {
  _id = kPoison;
  --g_live_probes;
}

long Probe::id() const {
  return _id;
}

long read_probe(Probe *p) {
  return p->id();
}

Holder::Holder() : _probe(nullptr) {}

Holder::Holder(Probe *p) : _probe(p) {}

Holder::~Holder() {}

void Holder::set_probe(Probe *p) {
  _probe = p;
}

long Holder::holder_probe_id() const {
  return _probe != nullptr ? _probe->id() : -1;
}
