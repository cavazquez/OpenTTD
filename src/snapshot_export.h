/*
 * Snapshot export for openttdrs parity (#110).
 * Applied onto OpenTTD @ docs/parity/openttd-reference.json (tag 15.3).
 */

#ifndef OPENTTDRS_SNAPSHOT_EXPORT_H
#define OPENTTDRS_SNAPSHOT_EXPORT_H

#include <string>

/**
 * If OPENTTDRS_SNAPSHOT_OUT is set, write a JSON snapshot compatible with
 * openttdrs `snapshot_dumper` hard fields (see docs/parity/SNAPSHOT_SCHEMA.md).
 * @param source_path Path of the loaded save (for metadata only).
 * @return true on success or when the env var is unset; false on I/O error.
 */
bool OpenttdrsMaybeExportSnapshot(const std::string &source_path);

/**
 * If OPENTTDRS_WORLD_RAW_OUT is set, writes a `world-raw` v2 JSONL stream
 * with every live map byte after AfterLoadGame. This is the tile-level oracle
 * for SAV rendering parity (#305).
 * @param source_path Path of the loaded save (for metadata only).
 * @return true on success or when the env var is unset; false on I/O error.
 */
bool OpenttdrsMaybeExportWorldRaw(const std::string &source_path);

/**
 * Arms an optional PBS JSONL exporter once the requested save completed loading.
 *
 * The exporter is enabled only when OPENTTDRS_PBS_TRACE_OUT is set. Its rows
 * are emitted by OpenttdrsMaybeExportPbsTraceTick() after each game tick.
 */
void OpenttdrsMaybeStartPbsTrace(const std::string &source_path);

/** Emits one post-tick PBS row and exits after the configured number of rows. */
void OpenttdrsMaybeExportPbsTraceTick();

/**
 * Arms an optional airport FTA JSONL exporter when OPENTTDRS_AIRPORT_FTA_TRACE_OUT is set.
 * Rows are emitted by OpenttdrsMaybeExportAirportFtaTraceTick() after each game tick.
 */
void OpenttdrsMaybeStartAirportFtaTrace(const std::string &source_path);

/** Emits one post-tick airport FTA row and exits after the configured number of rows. */
void OpenttdrsMaybeExportAirportFtaTraceTick();

#endif /* OPENTTDRS_SNAPSHOT_EXPORT_H */
