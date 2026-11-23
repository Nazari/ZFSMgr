/* Written for ZFSMgr in place of the one libcbor's CMake generates: static library, and the
 * same on every compiler the project uses (GCC, Clang, MinGW GCC). See third_party/README.md. */
#ifndef CBOR_EXPORT_H
#define CBOR_EXPORT_H

#define CBOR_EXPORT
#define CBOR_NO_EXPORT
#define CBOR_DEPRECATED __attribute__((__deprecated__))
#define CBOR_DEPRECATED_EXPORT CBOR_EXPORT CBOR_DEPRECATED

#endif
