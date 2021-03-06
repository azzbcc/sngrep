/**************************************************************************
 **
 ** sngrep - SIP Messages flow viewer
 **
 ** Copyright (C) 2013-2019 Ivan Alonso (Kaian)
 ** Copyright (C) 2013-2019 Irontec SL. All rights reserved.
 **
 ** This program is free software: you can redistribute it and/or modify
 ** it under the terms of the GNU General Public License as published by
 ** the Free Software Foundation, either version 3 of the License, or
 ** (at your option) any later version.
 **
 ** This program is distributed in the hope that it will be useful,
 ** but WITHOUT ANY WARRANTY; without even the implied warranty of
 ** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 ** GNU General Public License for more details.
 **
 ** You should have received a copy of the GNU General Public License
 ** along with this program.  If not, see <http://www.gnu.org/licenses/>.
 **
 ****************************************************************************/
/**
 * @file save_win.h
 * @author Ivan Alonso [aka Kaian] <kaian@irontec.com>
 *
 * @brief Functions to manage ui window for saving captured packets
 *
 * This file contains the functions and structures to manage the save
 * dialog, that can be used to copy the temporal sngrep file to another location
 *
 */

#ifndef __SNGREP_SAVE_WIN_H__
#define __SNGREP_SAVE_WIN_H__

#include "config.h"
#include <form.h>
#include "storage/group.h"
#include "tui/tui.h"


G_BEGIN_DECLS

#define WINDOW_TYPE_SAVE save_get_type()
G_DECLARE_FINAL_TYPE(SaveWindow, save, TUI, SAVE, Window)


/**
 * @brief Enum of available dialog fields
 *
 * Dialog form has a field array. Following enum represents the
 * order this fields are stored in panel info structure.
 */
typedef enum
{
    FLD_SAVE_PATH = 0,
    FLD_SAVE_FILE,
    FLD_SAVE_ALL,
    FLD_SAVE_SELECTED,
    FLD_SAVE_DISPLAYED,
    FLD_SAVE_MESSAGE,
    FLD_SAVE_STREAM,
    FLD_SAVE_PCAP,
    FLD_SAVE_PCAP_RTP,
    FLD_SAVE_TXT,
    FLD_SAVE_WAV,
    FLD_SAVE_SAVE,
    FLD_SAVE_CANCEL,
    FLD_SAVE_COUNT
} SaveWindowField;

/**
 * @brief Dialogs to be saved
 */
typedef enum
{
    SAVE_ALL = 0,
    SAVE_SELECTED,
    SAVE_DISPLAYED,
    SAVE_MESSAGE,
    SAVE_STREAM
} SaveWindowMode;

/**
 * @brief Save file formats
 */
typedef enum
{
    SAVE_PCAP = 0,
    SAVE_PCAP_RTP,
    SAVE_TXT,
    SAVE_WAV
} SaveWindowFormat;

/**
 * @brief Save panel private information
 *
 * This structure contains the durable data of save panel.
 */
struct _SaveWindow
{
    //! Parent object attributes
    Window parent;
    //! Form that contains the save fields
    FORM *form;
    //! An array of fields
    FIELD *fields[FLD_SAVE_COUNT + 1];
    //! Save mode @see save_modes
    SaveWindowMode savemode;
    //! Save format @see save_formats
    SaveWindowFormat saveformat;
    //! Call group to be saved
    CallGroup *group;
    //! Message to be saved
    Message *msg;
    //! Stream to be saved
    Stream *stream;
};

/**
 * @brief Creates a new save panel
 *
 * This function allocates all required memory for
 * displaying the save panel. It also draws all the
 * static information of the panel that will never be
 * redrawn.
 *
 * @param window UI structure pointer
 */
Window *
save_win_new();

/**
 * @brief Destroy save panel
 *
 * This function do the final cleanups for this panel
 *
 * @param window UI structure pointer
 */
void
save_win_free(Window *window);

/**
 * @brief Set the group call of the panel
 *
 * This function will access the panel information and will set the
 * group call pointer to the selected calls
 *
 * @param window UI structure pointer
 * @param group Call group pointer to be set in the internal info struct
 */
void
save_set_group(Window *window, CallGroup *group);

/**
 * @brief Set the SIP message to be saved
 *
 * This function will access the panel information and will set the
 * pointer to the selected SIP message
 *
 * @param window UI structure pointer
 * @param msg SIP message pointer to be set in the internal info struct
 */
void
save_set_msg(Window *window, Message *msg);

/**
 * @brief Set the SIP message to be saved
 *
 * This function will access the panel information and will set the
 * pointer to the selected SIP message
 *
 * @param window UI structure pointer
 * @param stream Stream packets to be saved
 */
void
save_set_stream(Window *window, Stream *stream);

#endif  /* __SNGREP_SAVE_WIN_H__ */
