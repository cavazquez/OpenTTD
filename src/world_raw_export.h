/*
 * Lightweight world-raw export for OpenTTD trees that are not pinned to 15.3.
 * This intentionally has no PBS/FTA fixture helpers.
 */

#ifndef OPENTTDRS_WORLD_RAW_EXPORT_H
#define OPENTTDRS_WORLD_RAW_EXPORT_H

#include <string>

/**
 * If OPENTTDRS_WORLD_RAW_OUT is set, writes a `world-raw` v2 JSONL stream
 * with every live map byte after AfterLoadGame.
 * @return true on success or when the env var is unset; false on I/O error.
 */
bool OpenttdrsMaybeExportWorldRaw(const std::string &source_path);

#endif /* OPENTTDRS_WORLD_RAW_EXPORT_H */
