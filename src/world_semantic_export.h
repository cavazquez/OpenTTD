/*
 * Per-tile semantic oracle for openttdrs SAV rendering parity (#306).
 */

#ifndef OPENTTDRS_WORLD_SEMANTIC_EXPORT_H
#define OPENTTDRS_WORLD_SEMANTIC_EXPORT_H

#include <string>

/**
 * If OPENTTDRS_WORLD_SEMANTIC_OUT is set, writes a `world-semantic` v2
 * JSONL stream after AfterLoadGame.
 * @return true on success or when the env var is unset; false on I/O error.
 */
bool OpenttdrsMaybeExportWorldSemantic(const std::string &source_path);

#endif /* OPENTTDRS_WORLD_SEMANTIC_EXPORT_H */
