/*
 * Opt-in viewport screenshot oracle for openttdrs renderer parity.
 *
 * This lives beside the headless draw-call exporter, but deliberately uses
 * OpenTTD's normal screenshot pipeline. It is only activated by an output
 * path supplied through OPENTTDRS_WORLD_SCREENSHOT_OUT.
 */

#ifndef OPENTTDRS_WORLD_SCREENSHOT_EXPORT_H
#define OPENTTDRS_WORLD_SCREENSHOT_EXPORT_H

/**
 * Schedules one normal-zoom map screenshot after loading a game, copies it to
 * OPENTTDRS_WORLD_SCREENSHOT_OUT, and then exits OpenTTD. Optional variables:
 *
 * - OPENTTDRS_WORLD_SCREENSHOT_CENTER=x,y
 * - OPENTTDRS_WORLD_SCREENSHOT_RES=widthxheight
 */
bool OpenttdrsMaybeCaptureWorldScreenshot();

#endif /* OPENTTDRS_WORLD_SCREENSHOT_EXPORT_H */
