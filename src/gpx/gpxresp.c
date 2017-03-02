//
//  gpxresp.c
//  
//  gpxresp adds the responses (x3g->reprap) to the gcode translator to allow
//  a bidirectional emulation of a reprap compatible printer in online mode
//
//  Copyright (c) 2013 MarkWal, All rights reserved.
//
//  This program is free software; you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation; either version 2 of the License, or
//  (at your option) any later version.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program; if not, write to the Free Software Foundation,
//  Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

#include <ctype.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <errno.h>

#include "gpx.h"

// make a new string table
// cs_chunk -- count of strings -- grow the string array in chunks of this many strings
Sttb *sttb_init(Sttb *psttb, long cs_chunk)
{
    size_t cb = (size_t)cs_chunk * sizeof(char *);

    psttb->cs = 0;
    psttb->rgs = malloc(cb);
    if (psttb->rgs == NULL)
        return NULL;

    psttb->cb_expand = psttb->cb = cb;
    return psttb;
}

void sttb_cleanup(Sttb *psttb)
{
    char **ps = NULL;

    if (psttb->rgs == NULL)
        return;

    for (ps = psttb->rgs; ps < psttb->rgs + psttb->cs; ps++)
        free(*ps);

    free(psttb->rgs);
    psttb->rgs = NULL;
    psttb->cb = psttb->cb_expand = 0;
    psttb->cs = 0;
}

char *sttb_add(Sttb *psttb, char *s)
{
    if (psttb->rgs == NULL)
        return NULL;
    size_t cb_needed = sizeof(char *) * ((size_t)psttb->cs + 1);
    if (cb_needed > psttb->cb) {
        if (psttb->cb + psttb->cb_expand > cb_needed)
            cb_needed = psttb->cb + psttb->cb_expand;
        char **rgs_new = realloc(psttb->rgs, cb_needed);
        if (rgs_new == NULL)
            return NULL;
        psttb->rgs = rgs_new;
        psttb->cb = cb_needed;
    }
    if ((s = strdup(s)) == NULL)
        return NULL;
    return psttb->rgs[psttb->cs++] = s;
}

void sttb_remove(Sttb *psttb, long i)
{
    if (i < 0 || i >= psttb->cs) // a little bounds checking
        return;
    free(psttb->rgs[i]);
    memcpy(psttb->rgs + i, psttb->rgs + i + 1, (psttb->cs - i - 1) * sizeof(char *));
    psttb->cs--;
}

long sttb_find_nocase(Sttb *psttb, char *s)
{
    long i;
    for (i = 0; i < psttb->cs; i++) {
        if (strcasecmp(s, psttb->rgs[i]) == 0)
            return i;
    }
    return -1;
}

static Tio tio;

int tio_vprintf(Tio *tio, const char *fmt, va_list ap)
{
    size_t result;

    if (tio->cur >= sizeof(tio->translation))
        return 0;
    result = vsnprintf(tio->translation + tio->cur,
            sizeof(tio->translation) - tio->cur, fmt, ap);
    if (result > 0)
        tio->cur += result;
    return result;
}

int tio_printf(Tio *tio, char const* fmt, ...)
{
    va_list args;
    int result;

    va_start(args, fmt);
    result = tio_vprintf(tio, fmt, args);
    va_end(args);

    return result;
}

int tio_log_printf(Tio *tio, char const* fmt, ...)
{
    va_list args;
    int result;

    va_start(args, fmt);
    vfprintf(tio->gpx->log, fmt, args);
    result = tio_vprintf(tio, fmt, args);
    va_end(args);

    return result;
}

Tio *tio_initialize(Gpx *gpx)
{
    tio.cur = 0;
    tio.translation[0] = 0;
    tio.sio.port = -1;
    tio.flags = 0;
    tio.waiting = 0;
    tio.sec = 0;
    tio.gpx = gpx;
    sttb_init(&tio.sttb, 10);
    gpx->axis.positionKnown = 0;
    gpx->flag.M106AlwaysValve = 1;
    return &tio;
}

void tio_cleanup(Tio *tio)
{
    if (tio->gpx->log != NULL && tio->gpx->log != stderr) {
        fflush(tio->gpx->log);
        fclose(tio->gpx->log);
        tio->gpx->log = stderr;
    }
    if (tio->sio.port > -1) {
        close(tio->sio.port);
        tio->sio.port = -1;
    }
    if (tio->sttb.cs > 0) {
        sttb_cleanup(&tio->sttb);
        sttb_init(&tio->sttb, 10);
    }
    tio->sec = 0;
    tio->waiting = 0;
    tio->flags = 0;
    gpx_set_machine(tio->gpx, "r2", 1);
}


void tio_clear_state_for_cancel(Tio *tio)
{
    tio->gpx->flag.programState = READY_STATE;
    tio->gpx->axis.positionKnown = 0;
    tio->gpx->excess.a = 0;
    tio->gpx->excess.b = 0;
    if (tio->waiting) {
        tio->flag.waitClearedByCancel = 1;
        if(tio->gpx->flag.verboseMode)
            fprintf(tio->gpx->log, "setting waitClearedByCancel");
    }
    tio->waiting = 0;
    tio->waitflag.waitForEmptyQueue = 1;
    tio->flag.getPosWhenReady = 0;
}

// wrap port_handler and translate to the expect gcode response
#define COMMAND_OFFSET 2
#define EXTRUDER_ID_OFFSET 3
#define QUERY_COMMAND_OFFSET 4
#define EEPROM_LENGTH_OFFSET 8

static void translate_extruder_query_response(Gpx *gpx, Tio *tio, unsigned query_command, char *buffer)
{
    unsigned extruder_id = buffer[EXTRUDER_ID_OFFSET];

    switch (query_command) {
            // Query 00 - Query firmware version information
        case 0:
            // sio->response.firmware.version = read_16(gpx);
            break;

            // Query 02 - Get extruder temperature
        case 2:
            // like T0:170
            tio_printf(tio, " T");
            if (gpx->machine.extruder_count > 1)
                tio_printf(tio, "%u", extruder_id);
            tio_printf(tio, ":%u", tio->sio.response.temperature);
            break;

            // Query 22 - Is extruder ready
        case 22:
            if (extruder_id)
                tio->waitflag.waitForExtruderB = !tio->sio.response.isReady;
            else
                tio->waitflag.waitForExtruderA = !tio->sio.response.isReady;
            break;

            // Query 30 - Get build platform temperature
        case 30:
            tio_printf(tio, " B:%u", tio->sio.response.temperature);
            break;

            // Query 32 - Get extruder target temperature
        case 32:
            if (tio->waiting && !tio->waitflag.waitForEmptyQueue && tio->sio.response.temperature == 0) {
                if (extruder_id)
                    tio->waitflag.waitForExtruderB = 0;
                else
                    tio->waitflag.waitForExtruderA = 0;
            }
            tio_printf(tio, " /%u", tio->sio.response.temperature);
            break;

            // Query 33 - Get build platform target temperature
        case 33:
            if (tio->waiting && !tio->waitflag.waitForEmptyQueue && tio->sio.response.temperature == 0)
                tio->waitflag.waitForPlatform = 0;
            tio_printf(tio, " /%u", tio->sio.response.temperature);
            break;

            // Query 35 - Is build platform ready?
        case 35:
            tio->waitflag.waitForPlatform = !tio->sio.response.isReady;
            break;

            // Query 36 - Get extruder status
        case 36: /* NYI
            if(gpx->flag.verboseMode && gpx->flag.logMessages) {
                fprintf(gpx->log, "Extruder T%u status" EOL, extruder_id);
                if(sio->response.extruder.flag.ready) fputs("Target temperature reached" EOL, gpx->log);
                if(sio->response.extruder.flag.notPluggedIn) fputs("The extruder or build plate is not plugged in" EOL, gpx->log);
                if(sio->response.extruder.flag.softwareCutoff) fputs("Above maximum allowable temperature recorded: heater shutdown for safety" EOL, gpx->log);
                if(sio->response.extruder.flag.temperatureDropping) fputs("Heater temperature dropped below target temperature" EOL, gpx->log);
                if(sio->response.extruder.flag.buildPlateError) fputs("An error was detected with the build plate heater or sensor" EOL, gpx->log);
                if(sio->response.extruder.flag.extruderError) fputs("An error was detected with the extruder heater or sensor" EOL, gpx->log);
            } */
            break;

            // Query 37 - Get PID state
        case 37: /* NYI
            sio->response.pid.extruderError = read_16(gpx);
            sio->response.pid.extruderDelta = read_16(gpx);
            sio->response.pid.extruderOutput = read_16(gpx);
            sio->response.pid.buildPlateError = read_16(gpx);
            sio->response.pid.buildPlateDelta = read_16(gpx);
            sio->response.pid.buildPlateOutput = read_16(gpx); */
            break;
    }
}

// translate_handler
// Callback function for gpx_convert_and_send.  It's where we translate the
// s3g/x3g response into a text response that mimics a reprap printer.
static int translate_handler(Gpx *gpx, Tio *tio, char *buffer, size_t length)
{
    unsigned command;
    unsigned extruder;

    if (tio->flag.okPending) {
        tio->flag.okPending = 0;
        tio_printf(tio, "ok");
        // ok means: I'm ready for another command, not necessarily that everything worked
    }

    if (length == 0) {
        // we translated a command that has no translation to x3g, but there
        // still may be something to do to emulate gcode behavior
        if (gpx->command.flag & M_IS_SET) {
            switch (gpx->command.m) {
                case 23: { // M23 - select SD file
                    // Some host software expects case insensitivity for M23
                    long i = sttb_find_nocase(&tio->sttb, gpx->selectedFilename);
                    if (i >= 0) {
                        char *s = strdup(tio->sttb.rgs[i]);
                        if (s != NULL) {
                            free(gpx->selectedFilename);
                            gpx->selectedFilename = s;
                        }
                    }
                    // answer to M23, at least on Marlin, Repetier and Sprinter: "File opened:%s Size:%d"
                    // followed by "File selected:%s Size:%d".  Caller is going to 
                    // be really surprised when the failure happens on start print
                    // but other than enumerating all the files again, we don't
                    // have a way to tell the printer to go check if it can be
                    // opened
                    tio_printf(tio, "\nFile opened:%s Size:%d\nFile selected:%s", gpx->selectedFilename, 0, gpx->selectedFilename);
                    // currently no way to ask Sailfish for the file size, that I can tell :-(
                    break;
                }
            }
        }
        return SUCCESS;
    }

    command = buffer[COMMAND_OFFSET];
    extruder = buffer[EXTRUDER_ID_OFFSET];

    // throw any queuable command in the bit bucket while we're waiting for the cancel
    if (tio->flag.cancelPending && (command & 0x80))
        return SUCCESS;

    int rval = port_handler(gpx, &tio->sio, buffer, length);
    if (rval != SUCCESS) {
        VERBOSE(fprintf(gpx->log, "port_handler returned: rval = %d\n", rval);)
        return rval;
    }

    // we got a SUCCESS on a queable command, so we're not waiting anymore
    if (command & 0x80)
        tio->waitflag.waitForBuffer = 0;

    switch (command) {
            // 03 - Clear buffer
        case 3:
            // 07 - Abort immediately
        case 7:
            // 17 - reset
        case 17:
            tio->waiting = 0;
            tio->waitflag.waitForBotCancel = 1;
            break;

            // 10 - Extruder (tool) query response
        case 10: {
            unsigned query_command = buffer[QUERY_COMMAND_OFFSET];
            translate_extruder_query_response(gpx, tio, query_command, buffer);
            break;
        }

            // 11 - Is ready?
        case 11:
            VERBOSE( fprintf(gpx->log, "is_ready: %d\n", tio->sio.response.isReady) );
            if (tio->sio.response.isReady) {
                tio->waitflag.waitForEmptyQueue = tio->waitflag.waitForButton = 0;
                if (tio->flag.getPosWhenReady) {
                    get_extended_position(gpx);
                    tio->flag.getPosWhenReady = 0;
                }
            }
            break;

            // 14 - Begin capture to file
        case 14:
            if (gpx->command.flag & ARG_IS_SET)
                tio_printf(tio, "\nWriting to file: %s", gpx->command.arg);
            break;

            // 15 - End capture
        case 15:
            tio_printf(tio, "\nDone saving file");
            break;

            // 16 - Playback capture (print from SD)
        case 16:
            // give the bot a chance to clear the BUILD_CANCELLED from the previous build
            if (tio->sio.response.sd.status == 7)
                tio_printf(tio, "\nError:  Not SD printing file not found");
            else {
                tio->cur = 0;
                tio->translation[0] = 0;
                tio->sec = time(NULL) + 3;
                tio->waitflag.waitForStart = 1;
            }
            break;

            // 18 - Get next filename
        case 18:
            if (!tio->flag.listingFiles && (gpx->command.flag & M_IS_SET) && gpx->command.m == 21) {
                // we used "get_next_filename(1)" to emulate M21
                if (tio->sio.response.sd.status == 0)
                    tio_printf(tio, "\nSD card ok");
                else
                    tio_printf(tio, "\nSD init fail");
            }
            else {
                // otherwise generate the M20 response
                if (!tio->flag.listingFiles) {
                    tio_printf(tio, "\nBegin file list\n");
                    tio->flag.listingFiles = 1;
                    if (tio->sttb.cs > 0)
                        sttb_cleanup(&tio->sttb);
                    sttb_init(&tio->sttb, 10);
                }
                if (!tio->sio.response.sd.filename[0]) {
                    tio_printf(tio, "End file list");
                    tio->flag.listingFiles = 0;
                }
                else {
                    tio_printf(tio, "%s", tio->sio.response.sd.filename);
                    sttb_add(&tio->sttb, tio->sio.response.sd.filename);
                }
            }
            break;

            // 21 - Get extended position
        case 21: {
            double epos = (double)tio->sio.response.position.a / gpx->machine.a.steps_per_mm;
            if (gpx->current.extruder == 1)
                epos = (double)tio->sio.response.position.b / gpx->machine.b.steps_per_mm;
            tio_printf(tio, " X:%0.2f Y:%0.2f Z:%0.2f E:%0.2f",
                (double)tio->sio.response.position.x / gpx->machine.x.steps_per_mm,
                (double)tio->sio.response.position.y / gpx->machine.y.steps_per_mm,
                (double)tio->sio.response.position.z / gpx->machine.z.steps_per_mm,
                epos);

            // squirrel away any positions we don't think we know, in case the
            // incoming stream tries to do a G92 without those axes
            if (tio->flag.getPosWhenReady) {
                if (!(gpx->axis.positionKnown & X_IS_SET)) gpx->current.position.x = (double)tio->sio.response.position.x / gpx->machine.x.steps_per_mm;
                if (!(gpx->axis.positionKnown & Y_IS_SET)) gpx->current.position.y = (double)tio->sio.response.position.y / gpx->machine.y.steps_per_mm;
                if (!(gpx->axis.positionKnown & Z_IS_SET)) gpx->current.position.z = (double)tio->sio.response.position.z / gpx->machine.z.steps_per_mm;
                if (!(gpx->axis.positionKnown & A_IS_SET)) gpx->current.position.a = (double)tio->sio.response.position.a / gpx->machine.a.steps_per_mm;
                if (!(gpx->axis.positionKnown & B_IS_SET)) gpx->current.position.b = (double)tio->sio.response.position.b / gpx->machine.b.steps_per_mm;
            }
            break;
        }

            // 23 - Get motherboard status
        case 23:
            if (!tio->sio.response.motherboard.bitfield)
                tio->waitflag.waitForButton = 0;
            else {
                if (tio->sio.response.motherboard.flag.buildCancelling)
                    return 0x89;
                if (tio->sio.response.motherboard.flag.heatShutdown) {
                    tio->cur = 0;
                    tio_printf(tio, "Error:  Heaters were shutdown after 30 minutes of inactivity");
                    return 0x89;
                }
                if (tio->sio.response.motherboard.flag.powerError) {
                    tio->cur = 0;
                    tio_printf(tio, "Error:  Error detected in system power");
                    return 0x89;
                }
            }
            break;

            // Query 24 - Get build statistics
        case 24:
            if (tio->waitflag.waitForBotCancel) {
                switch (tio->sio.response.build.status) {
                    case BUILD_RUNNING:
                    case BUILD_PAUSED:
                    case BUILD_CANCELLING:
                        break;
                    default:
                        tio->waitflag.waitForBotCancel = 0;
                }
            }
            if (tio->waitflag.waitForStart || ((gpx->command.flag & M_IS_SET) && (gpx->command.m == 27))) {
                // M27 response
                time_t t;
                if (tio->sec && tio->sio.response.build.status != 1 && time(&t) < tio->sec) {
                    if ((tio->sec - t) > 4) {
                        // in case we have a clock discontinuity we don't want to ignore status forever
                        tio->sec = 0;
                        tio->waitflag.waitForStart = 0;
                    }
                    break; // ignore it if we haven't started yet
                }
                switch (tio->sio.response.build.status) {
                    case BUILD_NONE:
                        // this is a matter of Not SD printing *yet* when we just
                        // kicked off the print, let's give it a moment
                        tio_printf(tio, "\nNot SD printing\n");
                        break;
                    case BUILD_RUNNING:
                        tio->sec = 0;
                        tio->waitflag.waitForStart = 0;
                        tio_printf(tio, "\nSD printing byte on line %u/0", tio->sio.response.build.lineNumber);
                        break;
                    case BUILD_CANCELED:
                        tio_printf(tio, "\nSD printing cancelled.\n");
                        tio->waiting = 0;
                        tio->flag.getPosWhenReady = 0;
                        // fall through
                    case BUILD_FINISHED_NORMALLY:
                        tio_printf(tio, "\nDone printing file\n");
                        break;
                    case BUILD_PAUSED:
                        tio_printf(tio, "\nSD printing paused at line %u\n", tio->sio.response.build.lineNumber);
                        break;
                    case BUILD_CANCELLING:
                        tio_printf(tio, "\nSD printing sleeping at line %u\n", tio->sio.response.build.lineNumber);
                        break;
                }
            }
            else {
                // not an M27 response, we're checking routinely or to clear a wait state
                switch (tio->sio.response.build.status) {
                    case BUILD_NONE:
                    case BUILD_RUNNING:
                        if (tio->waitflag.waitForUnpause)
                            tio->waitflag.waitForEmptyQueue = 1;
                        // fallthrough
                    default:
                        tio->waitflag.waitForUnpause = 0;
                        break;
                    case BUILD_PAUSED:
                        tio->waitflag.waitForUnpause = 1;
                        tio_printf(tio, "\n// echo: Waiting for unpause button on the LCD panel\n");
                        break;
                }
            }
            break;

            // 27 - Get advanced version number
        case 27: {
            char *variant = "Unknown";
            char *variant_url = variant;
            switch(tio->sio.response.firmware.variant) {
                case 0x01:
                    variant = "Makerbot";
                    variant_url = "https://support.makerbot.com/learn/earlier-products/replicator-original/updating-firmware-for-the-makerbot-replicator-via-replicatorg_13302";
                    break;
                case 0x80:
                    variant = "Sailfish";
                    variant_url = "http://www.sailfishfirmware.com";
                    break;
            }
            if ((gpx->command.flag & M_IS_SET) && gpx->command.m == 115) {
                // protocol version means the version of the RepRap protocol we're emulating
                // not the version of the x3g protocol we're talking
                tio_printf(tio, " PROTOCOL_VERSION:0.1 FIRMWARE_NAME:%s FIRMWARE_VERSION:%u.%u FIRMWARE_URL:%s MACHINE_TYPE:%s EXTRUDER_COUNT:%u\n",
                        variant, tio->sio.response.firmware.version / 100, tio->sio.response.firmware.version %100,
                        variant_url, gpx->machine.type, gpx->machine.extruder_count);
            }
            else {
                tio_printf(tio, " %s v%u.%u", variant, tio->sio.response.firmware.version / 100, tio->sio.response.firmware.version % 100);
            }
            break;
            }

            // 135 - wait for extruder
        case 135:
            tio->cur = 0;
            tio->translation[0] = 0;
            VERBOSE( fprintf(gpx->log, "waiting for extruder %d\n", extruder) );
            if (extruder == 0)
                tio->waitflag.waitForEmptyQueue = tio->waitflag.waitForExtruderA = 1;
            else
                tio->waitflag.waitForEmptyQueue = tio->waitflag.waitForExtruderB = 1;
            break;

            // 141 - wait for build platform
        case 141:
            tio->cur = 0;
            tio->translation[0] = 0;
            VERBOSE( fprintf(gpx->log, "waiting for platform\n") );
            tio->waitflag.waitForEmptyQueue = tio->waitflag.waitForPlatform = 1;
            break;

            // 144 - recall home position
        case 144:
            // fallthrough

            // 131, 132 - home axes
        case 131:
        case 132:
            VERBOSE( fprintf(gpx->log, "homing or recall home positions, wait for queue then ask bot for pos\n") );
            tio->cur = 0;
            tio->translation[0] = 0;
            tio->waitflag.waitForEmptyQueue = 1;
            tio->flag.getPosWhenReady = 1;
            break;

        case 133:
            VERBOSE( fprintf(gpx->log, "wait for (133) delay\n") );
            tio->cur = 0;
            tio->translation[0] = 0;
            tio->waitflag.waitForEmptyQueue = 1;
            break;

            // 148, 149 - message to the LCD, may be waiting for a button
        case 148:
        case 149:
            tio->cur = 0;
            tio->translation[0] = 0;
            VERBOSE( fprintf(gpx->log, "waiting for button\n") );
            tio->waitflag.waitForButton = 1;
            break;
    }

    return rval;
}

static int translate_result(Gpx *gpx, Tio *tio, const char *fmt, va_list ap)
{
    int len = 0;
    if (!strcmp(fmt, "@clear_cancel")) {
        if (!tio->flag.cancelPending && gpx->flag.programState == RUNNING_STATE) {
            // cancel gcode came through before cancel event
            VERBOSE( fprintf(gpx->log, "got @clear_cancel, waiting for abort call\n") );
            tio->waitflag.waitForCancelSync = 1;
        }
        else {
            tio->flag.cancelPending = 0;
            tio->waitflag.waitForEmptyQueue = 1;
        }
        return 0;
    }
    if (tio->flag.okPending) {
        tio->flag.okPending = 0;
        tio_printf(tio, "ok");
        // ok means: I'm ready for another command, not necessarily that everything worked
    }
    if (tio->cur > 0 && tio->translation[tio->cur - 1] != '\n') 
       len = tio_printf(tio, "\n"); 
    return len + tio_printf(tio, "// echo: ") + tio_vprintf(tio, fmt, ap);
}

int gpx_return_translation(Gpx *gpx, int rval)
{
    int waiting = tio.waiting;

    // ENDED -> READY
    if (gpx->flag.programState > RUNNING_STATE)
        gpx->flag.programState = READY_STATE;
    gpx->flag.macrosEnabled = 1;

    // if we're waiting for something and we haven't produced any output
    // give back current temps
    if (rval == SUCCESS && tio.waiting && tio.cur == 0) {
        if(gpx->flag.verboseMode)
            fprintf(gpx->log, "implicit M105\n");
        strncpy(gpx->buffer.in, "M105", sizeof(gpx->buffer.in));
        rval = gpx_convert_line(gpx, gpx->buffer.in);
        if(gpx->flag.verboseMode)
            fprintf(gpx->log, "implicit M105 rval = %d\n", rval);
    }

    if(gpx->flag.verboseMode)
        fprintf(gpx->log, "gpx_return_translation rval = %d\n", rval);
    fflush(gpx->log);
    switch (rval) {
        case SUCCESS:
        case END_OF_FILE:
            break;

        case EOSERROR:
            tio.cur = 0;
            tio_printf(&tio, "Error: OS error trying to access X3G port");
            break;
        case ERROR:
            tio.cur = 0;
            tio_printf(&tio, "Error: GPX error");
            break;
        case ESIOWRITE:
        case ESIOREAD:
        case ESIOFRAME:
        case ESIOCRC:
            tio.cur = 0;
            tio_printf(&tio, "Error: Serial communication error on X3G port. code = %d", rval);
            break;
        case ESIOTIMEOUT:
            tio.cur = 0;
            tio_printf(&tio, "Error: Timeout on X3G port");
            break;
        case 0x80:
            tio.cur = 0;
            tio_printf(&tio, "Error: X3G generic packet error");
            break;
        case 0x82: // Action buffer overflow
            tio.waitflag.waitForBuffer = 1;
            tio_printf(&tio, "Status: Buffer full");
            break;
        case 0x83:
            // TODO resend?
            tio.cur = 0;
            tio_printf(&tio, "Error: X3G checksum mismatch");
            break;
        case 0x84:
            tio.cur = 0;
            tio_printf(&tio, "Error: X3G query packet too big");
            break;
        case 0x85:
            tio.cur = 0;
            tio_printf(&tio, "Error: X3G command not supported or recognized");
            break;
        case 0x87:
            tio.cur = 0;
            tio_printf(&tio, "Error: X3G timeout downstream");
            break;
        case 0x88:
            tio.cur = 0;
            tio_printf(&tio, "Error: X3G timeout for tool lock");
            break;
        case 0x89:
            if (tio.waitflag.waitForBotCancel) {
                // ah, we told the bot to abort, and this 0x89 means that it did
                tio.waitflag.waitForBotCancel = 0;
                if(gpx->flag.verboseMode)
                    fprintf(gpx->log, "cleared waitForBotCancel\n");
                rval = SUCCESS;
                break;
            }

            // bot is initiating a cancel
            if (gpx->flag.verboseMode)
                fprintf(gpx->log, "bot cancelled, now waiting for @clear_cancel\n");
            // we'll only get a @clear_cancel from the host loop, an M112
            // won't come through because the event layer will eat the next
            // event (because it's anticipating this event)
            tio.flag.cancelPending = 1;
            tio_clear_state_for_cancel(&tio);
            tio_printf(&tio, "Build cancelled");
            break;
        case 0x8A:
            tio.cur = 0;
            tio_printf(&tio, "SD printing");
            break;
        case 0x8B:
            tio.cur = 0;
            tio_printf(&tio, "Error: RC_BOT_OVERHEAT Printer reports overheat condition");
            break;
        case 0x8C:
            tio.cur = 0;
            tio_printf(&tio, "Error: timeout");
            break;

        default:
            if (gpx->flag.verboseMode)
                fprintf(gpx->log, "Error: Unknown error code: %d", rval);
            tio.cur = 0;
            tio_printf(&tio, "Error: Unknown error code: %d", rval);
            break;
    }

    // if the rval cleared the wait state, we need an ok
    if(waiting && !tio.waiting) {
        if(gpx->flag.verboseMode)
            fprintf(gpx->log, "add ok for wait cleared\n");
        if (tio.cur > 0 && tio.translation[tio.cur - 1] != '\n')
            tio_printf(&tio, "\n");
        tio_printf(&tio, "ok");
    }
    else if (tio.cur > 0 && tio.translation[tio.cur - 1] == '\n')
        tio.translation[--tio.cur] = 0;

    fflush(gpx->log);
    return rval;
}

int gpx_write_string_core(Gpx *gpx, const char *s)
{
    unsigned waiting = tio.waiting;
    if (waiting && gpx->flag.verboseMode)
        fprintf(gpx->log, "waiting in gpx_write_string\n");

    strncpy(gpx->buffer.in, s, sizeof(gpx->buffer.in));
    int rval = gpx_convert_line(gpx, gpx->buffer.in);

    if (gpx->flag.verboseMode)
        fprintf(gpx->log, "gpx_write_string_core rval = %d\n", rval);

    if (tio.flag.okPending) {
        tio_printf(&tio, "ok");
        // ok means: I'm ready for another command, not necessarily that everything worked
    }
    // if we were waiting, but now we're not, throw an ok on there
    else if (!tio.waiting && waiting)
        tio_printf(&tio, "\nok");
    tio.flag.okPending = 0;
    if (waiting && gpx->flag.verboseMode)
        fprintf(gpx->log, "leaving gpx_write_string_core %d\n", tio.waiting);
    fflush(gpx->log);

    return rval;
}

int gpx_write_string(Gpx *gpx, const char *s)
{
    return gpx_return_translation(gpx, gpx_write_string_core(gpx, s));
}

// convert from a long int value to a speed_t constant
// returns B0 on failure
speed_t speed_from_long(long *baudrate)
{
    speed_t speed = B0;

    // TODO Baudrate warning for Replicator 2/2X with 57600?  Throw maybe?
    switch(*baudrate) {
        case 4800:
            speed=B4800;
            break;
        case 9600:
            speed=B9600;
            break;
#ifdef B14400
        case 14400:
            speed=B14400;
            break;
#endif
        case 19200:
            speed=B19200;
            break;
#ifdef B28800
        case 28800:
            speed=B28800;
            break;
#endif
        case 38400:
            speed=B38400;
            break;
        case 57600:
            speed=B57600;
            break;
            // TODO auto detect speed when 0?
        case 0: // 0 means default of 115200
            *baudrate=115200;
        case 115200:
            speed=B115200;
            break;
        default:
            tio_log_printf(&tio, "Error: Unsupported baud rate '%ld'\n", *baudrate);
            break;
    }
    return speed;
}

int gpx_connect(Gpx *gpx, const char *printer_port, speed_t speed)
{
    // open the port
    if (speed == B0)
        return ESIOBADBAUD;
    if (!gpx_sio_open(gpx, printer_port, speed, &tio.sio.port))
        return EOSERROR;

    // initialize tio
    tio.gpx = gpx;
    tio.sio.in = NULL;
    tio.sio.bytes_out = tio.sio.bytes_in = 0;
    tio.sio.flag.retryBufferOverflow = 1;
    tio.sio.flag.shortRetryBufferOverflowOnly = 1;

    // set up gpx
    gpx_start_convert(gpx, "", 0);
    gpx->flag.framingEnabled = 1;
    gpx->flag.sioConnected = 1;
    gpx->sio = &tio.sio;
    gpx_register_callback(gpx, (int (*)(Gpx*, void*, char*, size_t))translate_handler, &tio);
    gpx->resultHandler = (int (*)(Gpx*, void*, const char*, va_list))translate_result;

    fprintf(gpx->log, "gpx connected to %s\n", printer_port);

    tio.cur = 0;
    tio_printf(&tio, "start\n");
    return SUCCESS;
}

static int gpx_create_daemon_port(Gpx *gpx, const char *daemon_port)
{
    int rval = SUCCESS;
    char *upstream_port = NULL;
    FILE *socat_stdin = NULL;

    size_t l = strlen(daemon_port) + 3;
    if ((upstream_port = malloc(l)) == NULL) {
        fprintf(gpx->log, "Error: Out of memory. Unable to generate name for the other end of the virtual port.\n");
        rval = ERROR;
        goto Exit;
    }
    snprintf(upstream_port, l, "%s_u", daemon_port);

    snprintf(gpx->buffer.in, sizeof(gpx->buffer.in),
            "socat -d -d pty,mode=777,raw,echo=0,link=%s pty,mode=777,raw,echo=0,link=%s",
            upstream_port, daemon_port);
    VERBOSE( fprintf(gpx->log, "Spawning socat: %s\n", gpx->buffer.in); )

    if ((socat_stdin = popen(gpx->buffer.in, "r")) == NULL) {
        fprintf(gpx->log, "Error: Unable to create virtual port (launching socat failed). errno = %d\n", errno);
        rval = EOSERROR;
        goto Exit;
    }
    sleep(2); // TODO markwal: wait until pty appears

    if ((tio.upstream_w = open(upstream_port, O_WRONLY)) == -1) {
        fprintf(gpx->log, "Error: Unable to open upstream write port (%s) errno = %d\n", upstream_port, errno);
        rval = EOSERROR;
        goto Exit;
    }

    if ((tio.upstream_r = open(upstream_port, O_RDONLY)) == -1) {
        fprintf(gpx->log, "Error: Unable to open upstream read port (%s) errno = %d\n", upstream_port, errno);
        rval = EOSERROR;
        goto Exit;
    }

Exit:
    if (upstream_port != NULL)
        free(upstream_port);
    return rval;
}

int gpx_daemon(Gpx *gpx, const char *daemon_port, const char *printer_port, speed_t speed)
{
    tio_initialize(gpx);

    int rval = gpx_create_daemon_port(gpx, daemon_port);
    int overflow = 0;

    if (rval != SUCCESS)
        return rval;

    if ((rval = gpx_connect(gpx, printer_port, speed)) != SUCCESS) {
        return rval;
    }

    /*
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(fileno(tio.upstream), &rfds);

    for(;;) {
        VERBOSE( fprintf(gpx->log, "waiting on tio.upstream\n"); )
        rval = select(1, &rfds, NULL, NULL, NULL); // block until there are bytes to read
        VERBOSE( fprintf(gpx->log, "select returned\n"); )
        if(rval != SUCCESS) {
            VERBOSE( fprintf(gpx->log, "select failed. rval = %d\n", rval); )
            break;
        }
    size_t size = fread(gpx->buffer.in, 1, 1, tio.upstream);
    VERBOSE(fprintf(gpx->log, "fread returned %d.\n", size);)
    */

    const char *start = "start\nok\n";
    write(tio.upstream_w, start, strlen(start));
    for (;;) {
        char *p = gpx->buffer.in;
        int remaining = BUFFER_MAX;
        for(; remaining; remaining--, p++) {
            if(read(tio.upstream_r, p, 1) != 1) {
                VERBOSE( fprintf(gpx->log, "read upstream failed. errno = %d\n", errno); )
                return EOSERROR;
            }
            if(*p == '\n')
                break;
        }
        *p = '\0';
        VERBOSE( fprintf(gpx->log, "read a line: %s\n", gpx->buffer.in); )
        // detect input buffer overflow and ignore overflow input
        if(overflow) {
            if(strlen(gpx->buffer.in) != BUFFER_MAX - 1) {
                overflow = 0;
            }
            continue;
        }
        if(strlen(gpx->buffer.in) == BUFFER_MAX - 1) {
            overflow = 1;
            // ignore run-on comments, this is actually a little too permissive
            // since technically we should ignore ';' contained within a
            // parenthetical comment
            if(!strchr(gpx->buffer.in, ';'))
                tio_printf(&tio, "(line %u) Buffer overflow: input exceeds %u character limit, remaining characters in line will be ignored" EOL, gpx->lineNumber, BUFFER_MAX);
        }

        tio.cur = 0;
        tio.translation[0] = 0;
        tio.waitflag.waitForBuffer = 0; // maybe clear this every time?
        tio.flag.okPending = !tio.waiting;
        rval = gpx_write_string(gpx, gpx->buffer.in);
        tio.flag.okPending = 0;
        tio_printf(&tio, "\n");
        VERBOSE( fprintf(gpx->log, "write: %s\n", tio.translation); )
        int len = strlen(tio.translation);
        if(len != write(tio.upstream_w, tio.translation, strlen(tio.translation))) {
            VERBOSE( fprintf(gpx->log, "write on upstream failed to write all bytes.  errno = %d.\n", errno) );
        }
    }

    VERBOSE( fprintf(gpx->log, "fgets on upstream failed.  errno = %d\n", errno); )
    return EOSERROR;
}