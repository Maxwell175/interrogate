/**
 * PANDA 3D SOFTWARE
 * Copyright (c) Carnegie Mellon University.  All rights reserved.
 *
 * All use of this software is subject to the terms of the revised BSD
 * license.  You should have received a copy of this license along
 * with this source code in a file named "LICENSE."
 *
 * @file streamBridge.h
 *
 * Native side of the System.IO.Stream / std::iostream bridge used by the C#
 * bindings that interrogate generates.  The managed side (Interrogate.Core's
 * StreamBridge.cs) creates C++ std::istream / std::ostream / std::iostream
 * instances backed by a streambuf that calls back into managed code for
 * every IO operation.
 */

#ifndef INTERROGATE_STREAM_BRIDGE_H
#define INTERROGATE_STREAM_BRIDGE_H

#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
  #define IG_SUPPORT_EXPORT __declspec(dllexport)
#else
  #define IG_SUPPORT_EXPORT __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Managed-side callbacks.  Return >=0 on success (bytes transferred for read
// /write, new absolute position for seek), or -1 on error.
typedef int64_t (*ig_stream_read_fn)(void *cookie, void *buf, int64_t len);
typedef int64_t (*ig_stream_write_fn)(void *cookie, const void *buf, int64_t len);
typedef int64_t (*ig_stream_seek_fn)(void *cookie, int64_t offset, int origin);

// Creates an std::istream / ostream / iostream backed by the given callbacks.
// Returns an opaque pointer that can be reinterpret_cast<> to the appropriate
// std::* pointer by the generated C wrapper.  Pass to igStreamBridge_Destroy
// to free.  Any NULL callback slot is treated as unsupported.
IG_SUPPORT_EXPORT void *
igStreamBridge_CreateIstream(ig_stream_read_fn read_cb,
                             ig_stream_seek_fn seek_cb,
                             void *cookie);

IG_SUPPORT_EXPORT void *
igStreamBridge_CreateOstream(ig_stream_write_fn write_cb,
                             ig_stream_seek_fn seek_cb,
                             void *cookie);

IG_SUPPORT_EXPORT void *
igStreamBridge_CreateIostream(ig_stream_read_fn read_cb,
                              ig_stream_write_fn write_cb,
                              ig_stream_seek_fn seek_cb,
                              void *cookie);

// ---------------------------------------------------------------------------
// The reverse direction: operating on a C++ stream that the *native* side owns.
//
// The bridge above only goes one way -- it wraps a managed Stream so C++ can read
// or write it.  A C++ function that hands back an std::istream * (VirtualFile::
// open_read_file, Multifile::open_read_subfile, ...) had nowhere to go: interrogate
// mapped the return to an opaque IntPtr, and the bound istream exposed only get(),
// one byte per P/Invoke.  So the managed side could open a file and then do nothing
// with it -- and worse, the close_read_file(istream *) overload took the *parameter*
// path, which bridges a brand-new native stream from a managed one, so it destroyed
// a stream C++ had never given out while the bridge destroyed it again.
//
// These let Interrogate.Core's NativeStream wrap such a pointer as a real
// System.IO.Stream.  They do not own the stream: freeing it stays with whoever
// produced it (the C++ close_* method), exactly as the C++ contract says.

// Reads up to len bytes.  Returns the number read (0 at end of stream), or -1.
IG_SUPPORT_EXPORT int64_t
igStream_Read(void *istream, void *buf, int64_t len);

// Writes len bytes.  Returns len on success, or -1.
IG_SUPPORT_EXPORT int64_t
igStream_Write(void *ostream, const void *buf, int64_t len);

// origin: 0 = begin, 1 = current, 2 = end.  is_input selects the get or put
// pointer.  Returns the new absolute position, or -1.
IG_SUPPORT_EXPORT int64_t
igStream_Seek(void *stream, int64_t offset, int origin, int is_input);

// Returns the current position, or -1.
IG_SUPPORT_EXPORT int64_t
igStream_Tell(void *stream, int is_input);

IG_SUPPORT_EXPORT void
igStream_Flush(void *ostream);

// Destroy a stream previously created by the matching factory.  The three
// variants exist because the C# side knows which direction it asked for; we
// need that information to downcast back to the concrete subclass whose
// virtual destructor owns the streambuf.
IG_SUPPORT_EXPORT void igStreamBridge_DestroyIstream(void *stream);
IG_SUPPORT_EXPORT void igStreamBridge_DestroyOstream(void *stream);
IG_SUPPORT_EXPORT void igStreamBridge_DestroyIostream(void *stream);

// Number of native bridge streams currently alive (created but not yet
// destroyed).  Used by memory-leak tests on the managed side to verify that
// every `using var` / finalizer path reaches Destroy.
IG_SUPPORT_EXPORT int64_t igStreamBridge_LiveCount();

#ifdef __cplusplus
}  // extern "C"
#endif

#endif
